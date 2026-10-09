#include <tamtypes.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <netman.h>
#include <ps2ip.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <errno.h>


#define PAD_PORT 0
#define PAD_SLOT 0
#define PAD_BUFFER_SIZE 256
#define LAYOUT_COUNT 62

static unsigned char pad_buffer[PAD_BUFFER_SIZE] __attribute__((aligned(64)));
static struct padButtonStatus buttons;
static unsigned short previous_buttons = 0xffff;
static int pad_ready = 0;
static int selected = 0;
static int page = 0;
static int layout_index = 0;
static int running = 1;
static int dev9_module_result = -999;
static int xdev9_module_result = -999;
static int xdev9serv_module_result = -999;
static int dev9_probe_attempted = 0;
static int net_init_attempted = 0;
static char bios_romver[17] = "UNAVAILABLE";
static const char *family_estimate = "UNKNOWN";
static char rom_region = '-';
static char rom_machine = '-';
static int rom_eeconf = -1, rom_atad = -1, rom_iopbtconf = -1;
static int rom_smap = -1, rom_speed = -1;
/* LAN discovery protocol v1: UDP broadcast, automatic peer timeout. */
extern unsigned char DEV9_irx[], NETMAN_irx[], SMAP_irx[];
extern unsigned int size_DEV9_irx, size_NETMAN_irx, size_SMAP_irx;
#define DISCOVERY_PORT 39512
#define HELLO_PREFIX "PS2MSNET/2 "
#define HELLO_PREFIX_LEN 11
static int lan_socket = -1, lan_state = 0, lan_error = 0;
static unsigned int lan_tx = 0, lan_rx = 0, lan_frames = 0;
static unsigned int peer_last_frame = 0;
static unsigned int lan_nonce = 0, peer_nonce = 0, lan_self_rx = 0, lan_invalid_rx = 0;
static char peer_address[24] = "NONE";
static char local_address[24] = "DHCP PENDING";
static int lan_link = -1, lan_dhcp_status = -1, lan_config_error = 0;
/* Select before starting LAN: 0 DHCP, 1 static PS2 #1, 2 static PS2 #2. */
static int lan_profile = 0, lan_send_error = 0;
static int lan_started_profile = -1, lan_start_count = 0, lan_init_stage = 0, lan_getconfig_rc = 0;
static int lan_poll_getconfig_rc = -999, lan_poll_count = 0, lan_ready = 0;
static int lan_cfg_dhcp_enabled = -1, lan_cfg_ip_zero = -1;
static int mod_dev9_rc=-999, mod_netman_rc=-999, mod_smap_rc=-999, netman_init_rc=-999, ps2ip_init_rc=-999;
static unsigned int lan_bcast_tx = 0, lan_ucast_tx = 0, lan_bcast_rx = 0, lan_ucast_rx = 0;
static unsigned int lan_bcast_fail = 0, lan_ucast_fail = 0;

#define TCP_TEST_PORT 39513
/* Peer-to-peer TCP destination follows the selected static LAN profile. */
static int tcp_fd=-1, tcp_listen_fd=-1, tcp_state=0, tcp_errno=0;
static unsigned int tcp_accept_count=0, tcp_listen_fail=0;
static unsigned int tcp_tx=0,tcp_rx=0,tcp_tries=0,tcp_attempt_frame=0;
/* Socket diagnostics retained across retries for on-screen inspection. */
static int tcp_connect_rc=0,tcp_connect_errno=0,tcp_select_rc=0;
static int tcp_so_error=0,tcp_so_result=0,tcp_last_recv=0,tcp_last_send=0;
static unsigned int tcp_connected_count=0;
static unsigned int tcp_target_ip=0;
static unsigned int tcp_gateway_ip=0,tcp_netmask_ip=0;
static int tcp_initialized=0;
static int relay_handshake_sent=0;
static unsigned int relay_peer_messages=0, sync_sequence=0, sync_peer_sequence=0;
static unsigned int sync_packets_sent=0, sync_packets_received=0, sync_duplicates=0;
static unsigned int sync_remote_buttons=0, sync_remote_x=0, sync_remote_y=0;
static unsigned int sync_local_x=160, sync_local_y=120;
static unsigned int sync_last_frame=0;
static char relay_last_message[33]="NONE";
static char tcp_input[192];
static int tcp_input_len=0;
static char tcp_output[128];
static int tcp_output_len=0,tcp_output_pos=0;
static unsigned int sync_local_buttons=0;
static int sync_is_newer(unsigned int a,unsigned int b) { return (int)(a-b)>0; }
static void sync_receive_line(const char *line) {
    unsigned int seq,x,y,buttons_value;
    if(sscanf(line,"S1 %u %u %u %x",&seq,&x,&y,&buttons_value)==4) {
        if(sync_packets_received && !sync_is_newer(seq,sync_peer_sequence)) {
            sync_duplicates++;
            return;
        }
        sync_peer_sequence=seq;
        sync_remote_x=x;
        sync_remote_y=y;
        sync_remote_buttons=buttons_value;
        sync_packets_received++;
        sync_last_frame=lan_frames;
        strcpy(relay_last_message,"S1 STATE RECEIVED");
    }
}
static void tcp_disconnect(void) {
    if(tcp_fd>=0)close(tcp_fd);
    tcp_fd=-1;tcp_state=(lan_profile==1?5:2);tcp_attempt_frame=lan_frames;
    relay_handshake_sent=0;tcp_input_len=0;tcp_output_len=0;tcp_output_pos=0;
}
static void tcp_setup(void) {
    if (lan_profile == 1 || lan_profile == 2) {
        tcp_target_ip=(192U<<24)|(168U<<16)|(50U<<8)|(lan_profile==1?102U:101U);
        tcp_state=(lan_profile==1?5:2);
    } else {
        /* DHCP has no known peer until discovery completes. */
        tcp_target_ip=0;
        tcp_state=0;
    }
}
/* PS2 #1 listens; PS2 #2 connects. One session per pair avoids
   competing outbound connections and makes both ends symmetric. */
static void tcp_listen_tick(void) {
    struct sockaddr_in addr,remote;
    socklen_t remote_len;
    int fd,flags,one=1;
    if(lan_profile!=1 || tcp_state==4)return;
    if(tcp_listen_fd<0) {
        if(tcp_attempt_frame && lan_frames-tcp_attempt_frame<180)return;
        tcp_attempt_frame=lan_frames;
        fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(fd<0){tcp_errno=errno;tcp_listen_fail++;return;}
        setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        memset(&addr,0,sizeof(addr));
        addr.sin_family=AF_INET;addr.sin_port=htons(TCP_TEST_PORT);
        addr.sin_addr.s_addr=htonl(INADDR_ANY);
        if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0 || listen(fd,1)<0) {
            tcp_errno=errno;tcp_listen_fail++;close(fd);return;
        }
        flags=fcntl(fd,F_GETFL,0);
        if(flags>=0)fcntl(fd,F_SETFL,flags|O_NONBLOCK);
        tcp_listen_fd=fd;tcp_state=5;
    }
    remote_len=sizeof(remote);
    fd=accept(tcp_listen_fd,(struct sockaddr*)&remote,&remote_len);
    if(fd>=0) {
        flags=fcntl(fd,F_GETFL,0);
        if(flags>=0)fcntl(fd,F_SETFL,flags|O_NONBLOCK);
        tcp_fd=fd;tcp_state=4;tcp_connected_count++;tcp_accept_count++;
        relay_handshake_sent=0;tcp_input_len=0;tcp_output_len=0;tcp_output_pos=0;
    } else if(errno!=EAGAIN && errno!=EWOULDBLOCK)tcp_errno=errno;
}
static void tcp_tick(void) {
    struct sockaddr_in a;
    struct timeval tv;
    fd_set w;
    char buf[128];
    int n,flags,e=0,i;
    socklen_t len;
    if(lan_profile==1 && tcp_state!=4) {tcp_listen_tick();return;}
    if(tcp_state==2 && (tcp_attempt_frame==0 || lan_frames-tcp_attempt_frame>180)) {
        tcp_attempt_frame=lan_frames;tcp_tries++;
        tcp_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(tcp_fd<0){tcp_errno=errno;return;}
        flags=fcntl(tcp_fd,F_GETFL,0);fcntl(tcp_fd,F_SETFL,flags|O_NONBLOCK);
        memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(TCP_TEST_PORT);
        a.sin_addr.s_addr=htonl(tcp_target_ip);
        n=connect(tcp_fd,(struct sockaddr*)&a,sizeof(a));
        tcp_connect_rc=n;tcp_connect_errno=n<0?errno:0;
        if(n==0){tcp_state=4;tcp_connected_count++;relay_handshake_sent=0;}
        else if(errno==EINPROGRESS || errno==EWOULDBLOCK)tcp_state=3;
        else {tcp_errno=errno;tcp_disconnect();}
    } else if(tcp_state==3) {
        FD_ZERO(&w);FD_SET(tcp_fd,&w);tv.tv_sec=0;tv.tv_usec=0;
        n=select(tcp_fd+1,NULL,&w,NULL,&tv);tcp_select_rc=n;
        if(n>0) {
            len=sizeof(e);tcp_so_result=getsockopt(tcp_fd,SOL_SOCKET,SO_ERROR,&e,&len);
            tcp_so_error=e;
            if(tcp_so_result==0 && !e){tcp_state=4;tcp_connected_count++;relay_handshake_sent=0;}
            else{tcp_errno=e?e:errno;tcp_disconnect();}
        } else if(n<0 || lan_frames-tcp_attempt_frame>120) {
            tcp_errno=n<0?errno:ETIMEDOUT;tcp_disconnect();
        }
    } else if(tcp_state==4) {
        n=recv(tcp_fd,buf,sizeof(buf),MSG_DONTWAIT);tcp_last_recv=n;
        if(n>0) {
            tcp_rx+=(unsigned int)n;
            for(i=0;i<n;i++) {
                char c=buf[i];
                if(c=='\n') {
                    tcp_input[tcp_input_len]=0;
                    sync_receive_line(tcp_input);
                    tcp_input_len=0;relay_peer_messages++;
                } else if(tcp_input_len<(int)sizeof(tcp_input)-1) {
                    tcp_input[tcp_input_len++]=c;
                } else tcp_input_len=0;
            }
        } else if(n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK)) {
            if(n<0)tcp_errno=errno;
            tcp_disconnect();return;
        }
        if(tcp_output_len==0) {
            if(!relay_handshake_sent) {
                const char *role=lan_profile==1?"PS2HOST/1\\n":lan_profile==2?"PS2CLIENT/1\\n":"PS2AUTO/1\\n";
                tcp_output_len=(int)strlen(role);
                memcpy(tcp_output,role,tcp_output_len);
            } else if(lan_frames%6==1) {
                sync_sequence++;
                tcp_output_len=sprintf(tcp_output,"S1 %u %u %u %04X\\n",
                    sync_sequence,sync_local_x,sync_local_y,sync_local_buttons);
            }
            tcp_output_pos=0;
        }
        if(tcp_output_len>tcp_output_pos) {
            n=send(tcp_fd,tcp_output+tcp_output_pos,tcp_output_len-tcp_output_pos,MSG_DONTWAIT);
            tcp_last_send=n;
            if(n>0) {
                tcp_tx+=(unsigned int)n;tcp_output_pos+=n;
                if(tcp_output_pos==tcp_output_len) {
                    if(!relay_handshake_sent)relay_handshake_sent=1;
                    else sync_packets_sent++;
                    tcp_output_len=0;tcp_output_pos=0;
                }
            } else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) {
                tcp_errno=errno;tcp_disconnect();
            }
        }
    }
}
static void lan_start(void) {
    struct ip4_addr ip, nm, gw;
    struct sockaddr_in addr;
    int one = 1, rc;
    if (lan_state) return;
    net_init_attempted = 1;
    lan_started_profile = lan_profile;
    lan_start_count++;
    lan_init_stage = 1;
    lan_ready = 0; lan_poll_count = 0; lan_poll_getconfig_rc = -999;
    lan_cfg_dhcp_enabled = -1; lan_cfg_ip_zero = -1;
    lan_state = 1;
    /* Session token distinguishes instances even behind virtual NAT. */
    lan_nonce = (unsigned int)clock() ^ (unsigned int)(unsigned long)&lan_nonce;
    if (!lan_nonce) lan_nonce = 1;
    SifLoadFileInit();
    SifInitIopHeap();
    sbv_patch_enable_lmb();
    rc = SifExecModuleBuffer(DEV9_irx, size_DEV9_irx, 0, NULL, NULL); mod_dev9_rc = rc;
    if (rc < 0) { lan_error = rc; lan_state = -1; return; }
    lan_init_stage = 2;
    rc = SifExecModuleBuffer(NETMAN_irx, size_NETMAN_irx, 0, NULL, NULL); mod_netman_rc = rc;
    if (rc < 0) { lan_error = rc; lan_state = -2; return; }
    lan_init_stage = 3;
    rc = SifExecModuleBuffer(SMAP_irx, size_SMAP_irx, 0, NULL, NULL); mod_smap_rc = rc;
    if (rc < 0) { lan_error = rc; lan_state = -3; return; }
    lan_init_stage = 4;
    rc = NetManInit(); netman_init_rc = rc;
    if (rc < 0) { lan_error = rc; lan_state = -4; return; }
    IP4_ADDR(&ip, 0,0,0,0);
    IP4_ADDR(&nm, 0,0,0,0);
    IP4_ADDR(&gw, 0,0,0,0);
    lan_init_stage = 5;
    rc = ps2ipInit(&ip, &nm, &gw); ps2ip_init_rc = rc;
    if (rc < 0) { lan_error = rc; lan_state = -7; return; }
    lan_init_stage = 6;
    /* SMAP registration can lag behind ps2ipInit. Defer configuration
       until sm0 is visible; never treat socket creation as IP readiness. */
    lan_getconfig_rc = -1;
    lan_config_error = -1;
    lan_init_stage = 7;
    lan_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (lan_socket < 0) { lan_error = lan_socket; lan_state = -5; return; }
    setsockopt(lan_socket, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(lan_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lan_state = -6; lan_error = -6; return;
    }
    lan_init_stage = 8;
    lan_state = 2;
    /* Wait until IP configuration is available before starting TCP. */
}
static void lan_tick(void) {
    struct sockaddr_in dest, from;
    socklen_t fromlen;
    char packet[64];
    unsigned int received_nonce;
    int n;
    if (lan_state != 2) return;
    lan_link = NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0);
    {
        t_ip_info cfg;
        lan_poll_count++;
        lan_poll_getconfig_rc = ps2ip_getconfig("sm0", &cfg);
        /* Retry initial configuration every 30 ticks until it succeeds.
           A failed getconfig must not permanently skip static IP setup. */
        if (lan_config_error != 1 && lan_poll_getconfig_rc >= 0 &&
            (lan_poll_count == 1 || lan_poll_count % 30 == 0)) {
            lan_getconfig_rc = lan_poll_getconfig_rc;
            cfg.dhcp_enabled = (lan_started_profile == 0);
            if (cfg.dhcp_enabled) {
                cfg.ipaddr.s_addr = 0;
                cfg.netmask.s_addr = 0;
                cfg.gw.s_addr = 0;
            } else {
                cfg.ipaddr.s_addr = htonl((192U<<24) | (168U<<16) | (50U<<8) |
                                          (100U + (unsigned int)lan_started_profile));
                cfg.netmask.s_addr = htonl(0xFFFFFF00U);
                cfg.gw.s_addr = 0;
            }
            lan_config_error = ps2ip_setconfig(&cfg);
            if (lan_config_error >= 0) lan_config_error = 1;
            /* Read back the effective config on the next tick. */
            lan_ready = 0;
            return;
        }
        if (lan_config_error != 1) {
            lan_ready = 0;
            strcpy(local_address, "STATIC IP PENDING");
            return;
        }
        if (lan_poll_getconfig_rc < 0) {
            lan_ready = 0;
            lan_dhcp_status = -1;
            strcpy(local_address, "CONFIG UNAVAILABLE");
            return;
        }
        lan_dhcp_status = cfg.dhcp_status;
        lan_cfg_dhcp_enabled = cfg.dhcp_enabled;
        lan_cfg_ip_zero = (cfg.ipaddr.s_addr == 0);
        tcp_gateway_ip=ntohl(cfg.gw.s_addr);
        tcp_netmask_ip=ntohl(cfg.netmask.s_addr);
        if (cfg.ipaddr.s_addr == 0 ||
            (cfg.dhcp_enabled && cfg.dhcp_status != DHCP_STATE_BOUND)) {
            lan_ready = 0;
            strcpy(local_address, cfg.dhcp_enabled ? "DHCP PENDING" : "STATIC IP PENDING");
            return;
        }
        {
            unsigned int a = ntohl(cfg.ipaddr.s_addr);
            sprintf(local_address,"%u.%u.%u.%u",(a>>24)&255,(a>>16)&255,(a>>8)&255,a&255);
        }
    }
    lan_ready = 1;
    ++lan_frames;
    sync_local_buttons=(unsigned int)(~previous_buttons)&0xffff;
    if((~previous_buttons)&PAD_LEFT && sync_local_x>2)sync_local_x-=2;
    if((~previous_buttons)&PAD_RIGHT && sync_local_x<638)sync_local_x+=2;
    if((~previous_buttons)&PAD_UP && sync_local_y>2)sync_local_y-=2;
    if((~previous_buttons)&PAD_DOWN && sync_local_y<478)sync_local_y+=2;
    if(!tcp_initialized){tcp_setup();tcp_initialized=1;}
    tcp_tick();
    if ((lan_frames % 120) == 1) {
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(DISCOVERY_PORT);
        dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sprintf(packet, HELLO_PREFIX "%08X", lan_nonce);
        n = sendto(lan_socket, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
        if (n > 0) { lan_tx += (unsigned int)n; lan_bcast_tx++; }
        else { lan_send_error = n; lan_bcast_fail++; }
        /* Static profiles also send to the other console directly. */
        if (lan_profile != 0) {
            dest.sin_addr.s_addr = htonl((192U<<24)|(168U<<16)|(50U<<8)|(lan_profile==1?102U:101U));
            n = sendto(lan_socket, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
            if (n > 0) { lan_tx += (unsigned int)n; lan_ucast_tx++; }
            else { lan_send_error = n; lan_ucast_fail++; }
        }
    }
    /* Use a nonblocking receive flag so controller and graphics never stall. */
    fromlen = sizeof(from);
    n = recvfrom(lan_socket, packet, sizeof(packet)-1, MSG_DONTWAIT,
                 (struct sockaddr *)&from, &fromlen);
    if (n > 0) {
        packet[n] = 0;
        if (n == 19 && !memcmp(packet, HELLO_PREFIX, HELLO_PREFIX_LEN) &&
            sscanf(packet + HELLO_PREFIX_LEN, "%8x", &received_nonce) == 1) {
            if (received_nonce == lan_nonce) {
                ++lan_self_rx;
            } else {
                unsigned int a = ntohl(from.sin_addr.s_addr);
                sprintf(peer_address, "%u.%u.%u.%u", (a>>24)&255,(a>>16)&255,(a>>8)&255,a&255);
                peer_nonce = received_nonce;
                peer_last_frame = lan_frames;
                lan_rx += (unsigned int)n;
                if (ntohl(from.sin_addr.s_addr) == ((192U<<24)|(168U<<16)|(50U<<8)|(lan_profile==1?102U:101U)) && lan_profile != 0) lan_ucast_rx++;
                else lan_bcast_rx++;
            }
        } else ++lan_invalid_rx;
    }
    if (peer_last_frame && lan_frames - peer_last_frame > 600) {
        strcpy(peer_address, "NONE");
        peer_last_frame = 0;
        peer_nonce = 0;
    }
}
static int rom_file_present(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

typedef struct { int columns, rows; } Layout;
static Layout layouts[LAYOUT_COUNT];

static void build_layouts(void) {
    int n = 0, rows, cols;
    for (cols = 1; cols <= 32; ++cols) { layouts[n].columns = cols; layouts[n++].rows = 1; }
    for (rows = 2; rows <= 5; ++rows)
        for (cols = rows; cols <= 32 / rows; ++cols) {
            layouts[n].columns = cols;
            layouts[n++].rows = rows;
        }
}

static void pad_setup(void) {
    int state;
    SifInitRpc(0);
    if (SifLoadModule("rom0:SIO2MAN", 0, NULL) < 0) return;
    if (SifLoadModule("rom0:PADMAN", 0, NULL) < 0) return;
    if (padInit(0) == 0) return;
    if (padPortOpen(PAD_PORT, PAD_SLOT, pad_buffer) == 0) return;
    state = padGetState(PAD_PORT, PAD_SLOT);
    pad_ready = state >= 0;
}

static void probe_romver(void) {
    char v[17] = {0};
    int fd = open("rom0:ROMVER", O_RDONLY);
    int n, i, version = 0;
    rom_eeconf = rom_file_present("rom0:EECONF");
    rom_atad = rom_file_present("rom0:ATAD");
    rom_iopbtconf = rom_file_present("rom0:IOPBTCONF");
    rom_smap = rom_file_present("rom0:SMAP");
    rom_speed = rom_file_present("rom0:SPEED");
    if (fd < 0) return;
    n = read(fd, v, 16);
    close(fd);
    if (n < 4) return;
    for (i = 0; i < 4; ++i) {
        if (v[i] < '0' || v[i] > '9') return;
        version = version * 10 + v[i] - '0';
    }
    memcpy(bios_romver, v, 16);
    bios_romver[16] = 0;
    /* ROMVER: four version digits, region, machine type, date. */
    if (n >= 6) {
        rom_region = v[4];
        rom_machine = v[5];
    }
    family_estimate = version <= 190 ? "FAT (ESTIMATE)" : "SLIM (ESTIMATE)";
}

/* Probe ROM driver availability only. A successful module load is NOT
 * proof of physical Ethernet hardware or an active link. */
static void probe_network_modules(void) {
    if (dev9_probe_attempted) return;
    dev9_probe_attempted = 1;
    probe_romver();
    dev9_module_result = SifLoadModule("rom0:DEV9", 0, NULL);
    xdev9_module_result = SifLoadModule("rom0:XDEV9", 0, NULL);
    xdev9serv_module_result = SifLoadModule("rom0:XDEV9SERV", 0, NULL);
}

static volatile unsigned int dbg_poll=0, dbg_readok=0, dbg_edges=0;
static volatile int dbg_state=-999, dbg_read=-999;
static volatile unsigned short dbg_raw=0xffff, dbg_pressed=0;
static unsigned short read_pressed(void) {
    int state = padGetState(PAD_PORT, PAD_SLOT);
    dbg_poll++; dbg_state=state;
    unsigned short current = 0xffff;
    unsigned short pressed;
    if (!pad_ready || (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)) return 0;
    dbg_read=padRead(PAD_PORT, PAD_SLOT, &buttons);
    if (dbg_read == 0) return 0;
    dbg_readok++;
    current = buttons.btns; dbg_raw=current;
    pressed = (unsigned short)(previous_buttons & (unsigned short)~current);
    previous_buttons = current; dbg_pressed=pressed; if(pressed) dbg_edges++;
    return pressed;
}

static const char glyph_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.-/()># |+";
static const unsigned char glyphs[][7] = {
{0xe,0x11,0x13,0x15,0x19,0x11,0xe},
{0x4,0xc,0x4,0x4,0x4,0x4,0xe},
{0xe,0x11,0x1,0x2,0x4,0x8,0x1f},
{0x1e,0x1,0x1,0xe,0x1,0x1,0x1e},
{0x2,0x6,0xa,0x12,0x1f,0x2,0x2},
{0x1f,0x10,0x10,0x1e,0x1,0x1,0x1e},
{0xf,0x10,0x10,0x1e,0x11,0x11,0xe},
{0x1f,0x1,0x2,0x4,0x8,0x8,0x8},
{0xe,0x11,0x11,0xe,0x11,0x11,0xe},
{0xe,0x11,0x11,0xf,0x1,0x1,0x1e},
{0xe,0x11,0x11,0x1f,0x11,0x11,0x11},
{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
{0xf,0x10,0x10,0x10,0x10,0x10,0xf},
{0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},
{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
{0xf,0x10,0x10,0x17,0x11,0x11,0xf},
{0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
{0x1f,0x4,0x4,0x4,0x4,0x4,0x1f},
{0x7,0x2,0x2,0x2,0x12,0x12,0xc},
{0x11,0x12,0x14,0x18,0x14,0x12,0x11},
{0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
{0x11,0x1b,0x15,0x15,0x11,0x11,0x11},
{0x11,0x19,0x15,0x13,0x11,0x11,0x11},
{0xe,0x11,0x11,0x11,0x11,0x11,0xe},
{0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
{0xe,0x11,0x11,0x11,0x15,0x12,0xd},
{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
{0xf,0x10,0x10,0xe,0x1,0x1,0x1e},
{0x1f,0x4,0x4,0x4,0x4,0x4,0x4},
{0x11,0x11,0x11,0x11,0x11,0x11,0xe},
{0x11,0x11,0x11,0x11,0x11,0xa,0x4},
{0x11,0x11,0x11,0x15,0x15,0x15,0xa},
{0x11,0x11,0xa,0x4,0xa,0x11,0x11},
{0x11,0x11,0xa,0x4,0x4,0x4,0x4},
{0x1f,0x1,0x2,0x4,0x8,0x10,0x1f},
{0x0,0x4,0x4,0x0,0x4,0x4,0x0},
{0x0,0x0,0x0,0x0,0x0,0xc,0xc},
{0x0,0x0,0x0,0x1f,0x0,0x0,0x0},
{0x1,0x1,0x2,0x4,0x8,0x10,0x10},
{0x2,0x4,0x8,0x8,0x8,0x4,0x2},
{0x8,0x4,0x2,0x2,0x2,0x4,0x8},
{0x10,0x8,0x4,0x2,0x4,0x8,0x10},
{0xa,0x1f,0xa,0xa,0x1f,0xa,0x0},
{0x0,0x0,0x0,0x0,0x0,0x0,0x0},
{0x4,0x4,0x4,0x4,0x4,0x4,0x4},
{0x0,0x4,0x4,0x1f,0x4,0x4,0x0}
};
static GSGLOBAL *gs;
#define COLOR(r,g,b) GS_SETREG_RGBAQ((r),(g),(b),0x80,0)
static void rect(float x,float y,float w,float h,u64 color) {
    gsKit_prim_sprite(gs,x,y,x+w,y+h,1,color);
}
static void label(float x,float y,float scale,u64 color,const char *str) {
    int i,j,k;
    float origin=x;
    for (;*str;++str) {
        const char *found;
        if (*str=='\n') {x=origin;y+=9*scale;continue;}
        found=strchr(glyph_chars,(*str>='a'&&*str<='z')?*str-32:*str);
        if(found) {
            i=(int)(found-glyph_chars);
            for(j=0;j<7;j++) for(k=0;k<5;k++)
                if(glyphs[i][j]&(1u<<(4-k)))
                    rect(x+k*scale,y+j*scale,scale,scale,color);
        }
        x+=6*scale;
    }
}
/* Number screens from the centre outward, keeping screen 1 at the
 * bottom-centre position. This is a visual proposal, not a peer assignment. */
static int screen_number(int columns,int rows,int col,int row) {
    int anchor_col=(columns-1)/2,anchor_row=rows-1;
    int distance=0,number=0,r,c;
    for(distance=0;distance<columns+rows;distance++) {
        for(r=rows-1;r>=0;r--) {
            for(c=0;c<columns;c++) {
                int dx=c-anchor_col,dy=anchor_row-r;
                int d=(dx<0?-dx:dx)+dy;
                if(d==distance) {
                    ++number;
                    if(c==col&&r==row)return number;
                }
            }
        }
    }
    return 0;
}
static void draw_layout(float x,float y,float w,float h) {
    Layout l=layouts[layout_index];
    float gap=3.0f;
    float cellw=(w-(l.columns-1)*gap)/l.columns;
    float cellh=(h-(l.rows-1)*gap)/l.rows;
    float actualw,actualh,px0,py0;
    int r,c;
    if(cellw>62.0f)cellw=62.0f;
    if(cellh>54.0f)cellh=54.0f;
    actualw=l.columns*cellw+(l.columns-1)*gap;
    actualh=l.rows*cellh+(l.rows-1)*gap;
    px0=x+(w-actualw)/2.0f;
    py0=y+(h-actualh)/2.0f;
    for(r=0;r<l.rows;r++)for(c=0;c<l.columns;c++) {
        int n=screen_number(l.columns,l.rows,c,r);
        float px=px0+c*(cellw+gap),py=py0+r*(cellh+gap);
        u64 col=n==1?COLOR(22,177,197):COLOR(46,83,121);
        char number[16];
        rect(px,py,cellw,cellh,col);
        if(cellw>=14.0f&&cellh>=12.0f) {
            float scale=cellw>=32.0f?1.7f:1.0f;
            snprintf(number,sizeof(number),"%d",n);
            label(px+(cellw-(float)strlen(number)*6.0f*scale)/2.0f,
                  py+(cellh-7.0f*scale)/2.0f,scale,
                  COLOR(255,255,255),number);
        }
    }
}
static void draw(void) {
    u64 white=COLOR(225,237,255),muted=COLOR(145,166,190),accent=COLOR(40,170,220);
    char line[100];
    Layout l=layouts[layout_index];
    float W=(float)gs->Width,H=(float)gs->Height;
    float sx=W/640.0f,sy=H/448.0f;
    int i;
    const char *items[]={"NETWORK ADAPTER","SCREEN LAYOUT","DIAGNOSTIC COUNTERS","ABOUT / STATUS"};
    gsKit_clear(gs,COLOR(8,17,32));
    rect(20*sx,20*sy,600*sx,55*sy,COLOR(20,40,65));
    label(34*sx,34*sy,2.5f*sx,white,"PS2 MULTI-SCREEN NETWORK TEST");
    rect(20*sx,85*sy,600*sx,2*sy,accent);
    if(page==0) {
        for(i=0;i<4;i++) {
            float y=(118+i*51)*sy;
            if(selected==i)rect(24*sx,y-8*sy,390*sx,43*sy,COLOR(24,65,96));
            label(42*sx,y,2.2f*sx,selected==i?white:muted,items[i]);
        }
        label(34*sx,380*sy,1.8f*sx,muted,"D-PAD MOVE  X OPEN");
    } else if(page==1) {
        label(34*sx,112*sy,2.2f*sx,white,"NETWORK ADAPTER");
        label(34*sx,150*sy,1.55f*sx,muted,"BIOS ROM MODULE LOAD RESULTS");
        if (!dev9_probe_attempted) {
            label(34*sx,185*sy,1.45f*sx,muted,"PROBE NOT STARTED");
        } else {
            sprintf(line,"ROM0:DEV9       %d",dev9_module_result);
            label(34*sx,182*sy,1.5f*sx,muted,line);
            sprintf(line,"ROM0:XDEV9      %d",xdev9_module_result);
            label(34*sx,214*sy,1.5f*sx,muted,line);
            sprintf(line,"ROM0:XDEV9SERV  %d",xdev9serv_module_result);
            label(34*sx,246*sy,1.5f*sx,muted,line);
        }
        sprintf(line,"ROMVER: %s",bios_romver);
        label(34*sx,268*sy,1.13f*sx,muted,line);
        sprintf(line,"REGION: %c  MACHINE: %c  %s",rom_region,rom_machine,family_estimate);
        label(34*sx,290*sy,1.10f*sx,muted,line);
        sprintf(line,"ROM EECONF:%s ATAD:%s IOPBTCONF:%s",
            rom_eeconf==1?"YES":"NO",rom_atad==1?"YES":"NO",rom_iopbtconf==1?"YES":"NO");
        label(34*sx,312*sy,1.05f*sx,muted,line);
        sprintf(line,"ROM SMAP:%s SPEED:%s",rom_smap==1?"YES":"NO",rom_speed==1?"YES":"NO");
        label(34*sx,334*sy,1.10f*sx,muted,line);
        label(34*sx,357*sy,1.00f*sx,muted,"ROM PROBES DO NOT TEST BUNDLED DEV9/NETMAN/SMAP");
        label(34*sx,378*sy,1.00f*sx,muted,"OPEN DIAGNOSTIC COUNTERS; PRESS X TO INIT LAN");
    } else if(page==2) {
        sprintf(line,"LAYOUT %d / %d",layout_index+1,LAYOUT_COUNT);
        label(34*sx,105*sy,2.0f*sx,white,line);
        sprintf(line,"%d X %d  -  %d SCREENS",l.columns,l.rows,l.columns*l.rows);
        label(34*sx,145*sy,1.8f*sx,muted,line);
        rect(30*sx,184*sy,580*sx,175*sy,COLOR(14,31,52));
        draw_layout(36*sx,190*sy,568*sx,162*sy);
        label(34*sx,365*sy,1.35f*sx,COLOR(50,210,220),"CYAN: SCREEN 1  -  BOTTOM CENTRE");
        label(34*sx,385*sy,1.35f*sx,muted,"LEFT / RIGHT CHANGE  -  PREVIEW ONLY");
    } else if(page==3) {
        label(34*sx,112*sy,2.2f*sx,white,"DIAGNOSTICS 1 / 2");
        sprintf(line,"LAN: %s  ERROR: %d",lan_state==2?"DISCOVERING":lan_state==0?"NOT STARTED":"INITIALIZING / FAILED",lan_error);
        label(34*sx,145*sy,1.28f*sx,muted,line);
        sprintf(line,"LOCAL IP: %s",local_address);
        label(34*sx,169*sy,1.25f*sx,muted,line);
        sprintf(line,"TX / RX BYTES: %u / %u",lan_tx,lan_rx);
        label(34*sx,193*sy,1.25f*sx,muted,line);
        sprintf(line,"PEER: %s",peer_address);
        label(34*sx,217*sy,1.25f*sx,muted,line);
        sprintf(line,"LINK: %d DHCP: %d CONFIG: %d",lan_link,lan_dhcp_status,lan_config_error);
        label(34*sx,241*sy,1.25f*sx,muted,line);
        sprintf(line,"SESSION: %08X PEER ID: %08X",lan_nonce,peer_nonce);
        label(34*sx,265*sy,1.05f*sx,muted,line);
        sprintf(line,"SELF RX: %u  INVALID RX: %u",lan_self_rx,lan_invalid_rx);
        label(34*sx,289*sy,1.05f*sx,muted,line);
        sprintf(line,"PROFILE: %s",lan_profile==0?"DHCP":lan_profile==1?"PS2 #1 192.168.50.101":"PS2 #2 192.168.50.102");
        label(34*sx,313*sy,1.10f*sx,muted,line);
        sprintf(line,"B TX/RX:%u/%u  U TX/RX:%u/%u",lan_bcast_tx,lan_bcast_rx,lan_ucast_tx,lan_ucast_rx);
        label(34*sx,337*sy,1.02f*sx,muted,line);
        sprintf(line,"TCP:%d ERR:%d TX/RX:%u/%u TRY:%u",tcp_state,tcp_errno,tcp_tx,tcp_rx,tcp_tries);
        label(34*sx,361*sy,0.95f*sx,muted,line);
        sprintf(line,"INIT:%d STARTS:%d PROFILE:%d SETUPCFG:%d",lan_init_stage,lan_start_count,lan_started_profile,lan_getconfig_rc);
        label(34*sx,385*sy,0.95f*sx,muted,line);
        sprintf(line,"READY:%d POLL:%d CFGRC:%d DHCPEN:%d IPZERO:%d",lan_ready,lan_poll_count,lan_poll_getconfig_rc,lan_cfg_dhcp_enabled,lan_cfg_ip_zero);
        label(34*sx,409*sy,0.85f*sx,muted,line);
    } else if(page==5) {
        label(34*sx,105*sy,2.0f*sx,white,"DIAGNOSTICS 2 / 2");
        sprintf(line,"DEV9 MODULE RC: %d",mod_dev9_rc);
        label(34*sx,143*sy,1.30f*sx,muted,line);
        sprintf(line,"NETMAN MODULE RC: %d",mod_netman_rc);
        label(34*sx,174*sy,1.30f*sx,muted,line);
        sprintf(line,"SMAP MODULE RC: %d",mod_smap_rc);
        label(34*sx,205*sy,1.30f*sx,muted,line);
        sprintf(line,"NETMAN INIT RC: %d",netman_init_rc);
        label(34*sx,236*sy,1.30f*sx,muted,line);
        sprintf(line,"PS2IP INIT RC: %d",ps2ip_init_rc);
        label(34*sx,267*sy,1.30f*sx,muted,line);
        sprintf(line,"CONFIG RC: %d  POLL RC: %d",lan_getconfig_rc,lan_poll_getconfig_rc);
        label(34*sx,298*sy,1.30f*sx,muted,line);
        sprintf(line,"READY: %d  LINK: %d  STARTS: %d",lan_ready,lan_link,lan_start_count);
        label(34*sx,329*sy,1.30f*sx,muted,line);
        sprintf(line,"PROFILE: %d  IP: %s",lan_started_profile,local_address);
        label(34*sx,360*sy,1.18f*sx,muted,line);
        label(34*sx,390*sy,1.00f*sx,muted,"LEFT: COUNTERS   TRIANGLE: BACK");
    } else {
        label(34*sx,112*sy,2.2f*sx,white,"ABOUT / STATUS");
        label(34*sx,166*sy,1.6f*sx,muted,"CONTROLLER MENU: ACTIVE");
        label(34*sx,200*sy,1.6f*sx,muted,"LAN DISCOVERY: EXPERIMENTAL");
        sprintf(line,"TCP TARGET: %u.%u.%u.%u:%d",(tcp_target_ip>>24)&255,(tcp_target_ip>>16)&255,(tcp_target_ip>>8)&255,tcp_target_ip&255,TCP_TEST_PORT);
        label(34*sx,230*sy,1.2f*sx,muted,line);
        sprintf(line,"GATEWAY: %u.%u.%u.%u MASK: %u.%u.%u.%u",(tcp_gateway_ip>>24)&255,(tcp_gateway_ip>>16)&255,(tcp_gateway_ip>>8)&255,tcp_gateway_ip&255,(tcp_netmask_ip>>24)&255,(tcp_netmask_ip>>16)&255,(tcp_netmask_ip>>8)&255,tcp_netmask_ip&255);
        label(34*sx,253*sy,1.0f*sx,muted,line);
        sprintf(line,"CONNECT RC:%d ERR:%d SELECT:%d",tcp_connect_rc,tcp_connect_errno,tcp_select_rc);
        label(34*sx,275*sy,1.08f*sx,muted,line);
        sprintf(line,"SO RESULT:%d ERROR:%d CONNECTED:%u",tcp_so_result,tcp_so_error,tcp_connected_count);
        label(34*sx,297*sy,1.03f*sx,muted,line);
        sprintf(line,"TCP RECV:%d SEND:%d STATE:%d ERR:%d TRY:%u",tcp_last_recv,tcp_last_send,tcp_state,tcp_errno,tcp_tries);
        label(34*sx,319*sy,1.0f*sx,muted,line);
        sprintf(line,"UDP SEND RC:%d  BCAST FAIL:%u  UCAST FAIL:%u",lan_send_error,lan_bcast_fail,lan_ucast_fail);
        label(34*sx,343*sy,1.05f*sx,muted,line);
        sprintf(line,"UDP BCAST TX/RX:%u/%u  UCAST TX/RX:%u/%u",lan_bcast_tx,lan_bcast_rx,lan_ucast_tx,lan_ucast_rx);
        label(34*sx,365*sy,1.0f*sx,muted,line);
        sprintf(line,"DEV9:%d  NETMAN:%d  SMAP:%d",mod_dev9_rc,mod_netman_rc,mod_smap_rc);
        label(34*sx,366*sy,0.93f*sx,muted,line);
        sprintf(line,"NET INIT:%d  IP INIT:%d  CFGRC:%d",netman_init_rc,ps2ip_init_rc,lan_poll_getconfig_rc);
        label(34*sx,387*sy,0.90f*sx,muted,line);
    }
    { char dbg[128];
      rect(0,0,640*sx,68*sy,COLOR(5,15,35));
      sprintf(dbg,"PAD READY:%d STATE:%d READ:%d PAGE:%d",pad_ready,dbg_state,dbg_read,page);
      label(8*sx,4*sy,1.0f*sx,white,dbg);
      sprintf(dbg,"RAW:%04X EDGE:%04X POLLS:%u OK:%u EDGES:%u",dbg_raw,dbg_pressed,dbg_poll,dbg_readok,dbg_edges);
      label(8*sx,26*sy,0.95f*sx,white,dbg);
      label(8*sx,48*sy,0.95f*sx,white,"CROSS=4000 SQUARE=8000 (ACTIVE LOW RAW)");
    }
    rect(20*sx,405*sy,600*sx,1*sy,COLOR(65,90,115));
    label(34*sx,418*sy,1.12f*sx,muted,page==3?"PAGE 1/2   RIGHT: MODULES   UP/DOWN: PROFILE":page==5?"PAGE 2/2   LEFT: COUNTERS":"CROSS: OPEN   TRIANGLE: BACK");
}

int main(int argc, char **argv) {
    unsigned short pressed;
    unsigned int redraw = 0;
    (void)argc; (void)argv;
    build_layouts();
    pad_setup();
    gs=gsKit_init_global();
    gs->PSM=GS_PSM_CT16;
    gs->PSMZ=GS_PSMZ_16;
    gs->ZBuffering=GS_SETTING_OFF;
    gs->DoubleBuffering=GS_SETTING_ON;
    dmaKit_init(D_CTRL_RELE_OFF,D_CTRL_MFD_OFF,D_CTRL_STS_UNSPEC,D_CTRL_STD_OFF,D_CTRL_RCYC_8,1<<DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs,GS_ONESHOT);
    while (running) {
        lan_tick();
        pressed = read_pressed();
        if (pressed & PAD_TRIANGLE) { page = 0; redraw = 1; }
        if ((pressed & PAD_RIGHT) && page == 3) { page = 5; redraw = 1; }
        else if ((pressed & PAD_LEFT) && page == 5) { page = 3; redraw = 1; }
        if (page == 0) {
            if (pressed & PAD_DOWN) { selected = (selected + 1) % 4; redraw = 1; }
            if (pressed & PAD_UP) { selected = (selected + 3) % 4; redraw = 1; }
            if (pressed & PAD_CROSS) { page = selected + 1; if(page == 1) probe_network_modules();  redraw = 1; }
        } else if (page == 3 && lan_state == 0) {
            if (pressed & PAD_DOWN) { lan_profile = (lan_profile + 1) % 3; redraw = 1; }
            if (pressed & PAD_UP) { lan_profile = (lan_profile + 2) % 3; redraw = 1; }
            if (pressed & PAD_CROSS) lan_start();
        } else if (page == 2) {
            if (pressed & PAD_RIGHT) { layout_index = (layout_index + 1) % LAYOUT_COUNT; redraw = 1; }
            if (pressed & PAD_LEFT) { layout_index = (layout_index + LAYOUT_COUNT - 1) % LAYOUT_COUNT; redraw = 1; }
        }
        (void)redraw;
        draw();
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    return 0;
}
