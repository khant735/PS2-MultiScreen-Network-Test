#include <tamtypes.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <delaythread.h>
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
static int input_diag_tab = 0; /* 0 host, 1 clients 01-16, 2 clients 17-31 */
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
extern unsigned char DEV9_irx[], NETMAN_irx[], SMAP_irx[], MACPROBE_irx[];
extern unsigned int size_MACPROBE_irx;
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
static unsigned char lan_mac[6] = {0};
static int lan_mac_rc = -999, lan_mac_valid = 0;
static unsigned char eeprom_mac[6] = {0};
static int eeprom_mac_rc = -999, eeprom_mac_valid = 0;
static int eeprom_probe_module_id = -999;
#define MACPROBE_RPC_ID 0x4D534D41
static SifRpcClientData_t eeprom_rpc_client __attribute__((aligned(64)));
static unsigned char eeprom_rpc_send[64] __attribute__((aligned(64)));
static unsigned char eeprom_rpc_recv[64] __attribute__((aligned(64)));
static void read_dev9_eeprom_mac(void)
{
    int tries, rc, result;
    memset(&eeprom_rpc_client, 0, sizeof(eeprom_rpc_client));
    memset(eeprom_mac, 0, sizeof(eeprom_mac));
    eeprom_mac_valid = 0;
    eeprom_mac_rc = -100;
    for (tries = 0; tries < 30; tries++) {
        rc = SifBindRpc(&eeprom_rpc_client, MACPROBE_RPC_ID, 0);
        if (rc < 0) { eeprom_mac_rc = rc; return; }
        if (eeprom_rpc_client.server != NULL) break;
        DelayThread(10000);
    }
    if (eeprom_rpc_client.server == NULL) { eeprom_mac_rc = -101; return; }
    memset(eeprom_rpc_send, 0, sizeof(eeprom_rpc_send));
    memset(eeprom_rpc_recv, 0, sizeof(eeprom_rpc_recv));
    rc = SifCallRpc(&eeprom_rpc_client, 1, 0, eeprom_rpc_send,
                    sizeof(eeprom_rpc_send), eeprom_rpc_recv,
                    sizeof(eeprom_rpc_recv), NULL, NULL);
    if (rc < 0) { eeprom_mac_rc = rc; return; }
    memcpy(&result, eeprom_rpc_recv, sizeof(result));
    eeprom_mac_rc = result;
    if (result == 0) {
        memcpy(eeprom_mac, eeprom_rpc_recv + 4, 6);
        eeprom_mac_valid = 1;
    }
}

/* Select before starting LAN: 0 DHCP, 1 static PS2 #1, 2 static PS2 #2. */
static int lan_profile = 0, lan_send_error = 0;
static int lan_started_profile = -1, lan_start_count = 0, lan_init_stage = 0, lan_getconfig_rc = 0;
static int lan_poll_getconfig_rc = -999, lan_poll_count = 0, lan_ready = 0;
static int lan_cfg_dhcp_enabled = -1, lan_cfg_ip_zero = -1;
static int mod_dev9_rc=-999, mod_netman_rc=-999, mod_smap_rc=-999, netman_init_rc=-999, ps2ip_init_rc=-999;
static unsigned int lan_bcast_tx = 0, lan_ucast_tx = 0, lan_bcast_rx = 0, lan_ucast_rx = 0;
static unsigned int b84_recv_empty=0, b84_recv_error=0, b84_recv_data=0;
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
static char tcp_output[256];
static int tcp_output_len=0,tcp_output_pos=0;
static unsigned int sync_local_buttons=0;
/* I2 carries the native DualShock 2 axes and 12 pressure bytes.
   Player 1 is the host, Client 01 is Player 2; Client 31 is Player 32.
   Only Client 01 has a TCP transport at present. */
#define INPUT_CLIENT_CAPACITY 31
typedef struct { unsigned int mask, last_frame; unsigned char analog[4], pressure[12]; int valid; } InputSnapshot;
static InputSnapshot input_host, input_clients[INPUT_CLIENT_CAPACITY];
static void capture_host_input(void) {
    static const unsigned char zero_pressure[12]={0};
    input_host.mask=(unsigned int)(~previous_buttons)&0xffff;
    if(pad_ready) {
        input_host.analog[0]=buttons.ljoy_h;input_host.analog[1]=buttons.ljoy_v;
        input_host.analog[2]=buttons.rjoy_h;input_host.analog[3]=buttons.rjoy_v;
        input_host.pressure[0]=buttons.up_p;input_host.pressure[1]=buttons.right_p;
        input_host.pressure[2]=buttons.down_p;input_host.pressure[3]=buttons.left_p;
        input_host.pressure[4]=buttons.triangle_p;input_host.pressure[5]=buttons.circle_p;
        input_host.pressure[6]=buttons.cross_p;input_host.pressure[7]=buttons.square_p;
        input_host.pressure[8]=buttons.l1_p;input_host.pressure[9]=buttons.l2_p;
        input_host.pressure[10]=buttons.r1_p;input_host.pressure[11]=buttons.r2_p;
        input_host.valid=1;
    } else {
        memset(input_host.pressure,0,sizeof(input_host.pressure));
        memset(input_host.analog,128,sizeof(input_host.analog));
        input_host.valid=0;
    }
    (void)zero_pressure;
}
static int pressure_percent(unsigned char value){return ((int)value*100+127)/255;}
/* Distinct DualShock face-button colours; non-face inputs retain cyan accent. */
static u64 input_button_color(int index, int active, u64 inactive, u64 default_active) {
    if (!active) return inactive;
    switch(index) {
        case 12: return GS_SETREG_RGBAQ(58,220,85,0x80,0);   /* Triangle green */
        case 13: return GS_SETREG_RGBAQ(245,67,75,0x80,0);   /* Circle red */
        case 14: return GS_SETREG_RGBAQ(75,130,255,0x80,0);  /* Cross blue */
        case 15: return GS_SETREG_RGBAQ(184,105,240,0x80,0); /* Square purple */
        default: return default_active;
    }
}

static int sync_is_newer(unsigned int a,unsigned int b) { return (int)(a-b)>0; }
static void input_receive_line(const char *line) {
    unsigned int v[20], seq; int count,i;
    if(strncmp(line,"I2 ",3)!=0)return;
    count=sscanf(line,"I2 %u %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x",
        &seq,&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10],&v[11],&v[12],&v[13],&v[14],&v[15],&v[16]);
    /* seq + mask + 4 axes + 12 pressures = 18 conversions */
    if(count!=18)return;
    input_clients[0].mask=v[0]&0xffff;
    for(i=0;i<4;i++)input_clients[0].analog[i]=(unsigned char)(v[i+1]&255);
    for(i=0;i<12;i++)input_clients[0].pressure[i]=(unsigned char)(v[i+5]&255);
    input_clients[0].last_frame=lan_frames;
    input_clients[0].valid=1;
}
static void sync_receive_line(const char *line) {
    unsigned int seq,x,y,buttons_value;
    if(strncmp(line,"I2 ",3)==0){input_receive_line(line);return;}
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
        printf("SYNC S1 RX seq=%u x=%u y=%u buttons=%04X frame=%u\n",seq,x,y,buttons_value,lan_frames);fflush(stdout);
        sync_last_frame=lan_frames;
        strcpy(relay_last_message,"S1 STATE RECEIVED");
    }
}
static void tcp_disconnect(void) {
    if(tcp_fd>=0)close(tcp_fd);
    tcp_fd=-1;tcp_state=(lan_profile==1?5:2);tcp_attempt_frame=lan_frames;
    relay_handshake_sent=0;tcp_input_len=0;tcp_output_len=0;tcp_output_pos=0;
    input_clients[0].valid=0;
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
    if(lan_frames<=10){printf("B82 LISTEN ENTER frame=%u state=%d listen_fd=%d\n",lan_frames,tcp_state,tcp_listen_fd);fflush(stdout);}
    struct sockaddr_in addr,remote;
    socklen_t remote_len;
    int fd,flags,one=1;
    if(lan_profile!=1 || tcp_state==4)return;
    if(tcp_listen_fd<0) {
        if(tcp_attempt_frame && lan_frames-tcp_attempt_frame<180)return;
        tcp_attempt_frame=lan_frames;
        if(lan_frames<=10){printf("B82 LISTEN SOCKET BEFORE\n");fflush(stdout);}
        fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(lan_frames<=10){printf("B82 LISTEN SOCKET AFTER fd=%d\n",fd);fflush(stdout);}
        if(fd<0){tcp_errno=errno;tcp_listen_fail++;return;}
        setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
        memset(&addr,0,sizeof(addr));
        addr.sin_family=AF_INET;addr.sin_port=htons(TCP_TEST_PORT);
        addr.sin_addr.s_addr=htonl(INADDR_ANY);
        if(lan_frames<=10){printf("B82 LISTEN BIND BEFORE\n");fflush(stdout);}
        if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0 || listen(fd,1)<0) {
            tcp_errno=errno;tcp_listen_fail++;close(fd);return;
        }
        if(lan_frames<=10){printf("B82 LISTEN BIND AFTER\n");fflush(stdout);}
        flags=fcntl(fd,F_GETFL,0);
        if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){tcp_errno=errno;tcp_listen_fail++;close(fd);return;}
        tcp_listen_fd=fd;tcp_state=5;
    }
    /* Never enter accept unless select says the listener is readable. */
    { fd_set rd; struct timeval zero={0,0}; int ready;
      FD_ZERO(&rd); FD_SET(tcp_listen_fd,&rd);
      if(lan_frames<=10){printf("B82 LISTEN SELECT BEFORE fd=%d\n",tcp_listen_fd);fflush(stdout);}
      ready=select(tcp_listen_fd+1,&rd,NULL,NULL,&zero);
      if(lan_frames<=10){printf("B82 LISTEN SELECT AFTER rc=%d errno=%d\n",ready,ready<0?errno:0);fflush(stdout);}
      if(ready<=0){if(ready<0)tcp_errno=errno;return;}
    }
    remote_len=sizeof(remote);
    if(lan_frames<=10){printf("B82 ACCEPT BEFORE\n");fflush(stdout);}
    fd=accept(tcp_listen_fd,(struct sockaddr*)&remote,&remote_len);
    if(lan_frames<=10){printf("B82 ACCEPT AFTER fd=%d errno=%d\n",fd,fd<0?errno:0);fflush(stdout);}
    if(fd>=0) {
        flags=fcntl(fd,F_GETFL,0);
        if(flags>=0)fcntl(fd,F_SETFL,flags|O_NONBLOCK);
        tcp_fd=fd;tcp_state=4;tcp_connected_count++;tcp_accept_count++;
        printf("SYNC TCP ACCEPT fd=%d frame=%u\n",fd,lan_frames);fflush(stdout);
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
    if(lan_frames<=10){printf("B82 TCP ENTER frame=%u profile=%d state=%d\n",lan_frames,lan_profile,tcp_state);fflush(stdout);}
    if(lan_profile==1 && tcp_state!=4) {tcp_listen_tick();return;}
    if(tcp_state==2 && (tcp_attempt_frame==0 || lan_frames-tcp_attempt_frame>180)) {
        tcp_attempt_frame=lan_frames;tcp_tries++;
        tcp_fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(tcp_fd<0){tcp_errno=errno;return;}
        flags=fcntl(tcp_fd,F_GETFL,0);
        if(flags<0 || fcntl(tcp_fd,F_SETFL,flags|O_NONBLOCK)<0){tcp_errno=errno;tcp_disconnect();return;}
        memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons(TCP_TEST_PORT);
        a.sin_addr.s_addr=htonl(tcp_target_ip);
        if(lan_frames<=10){printf("B82 CONNECT BEFORE fd=%d\n",tcp_fd);fflush(stdout);}
        n=connect(tcp_fd,(struct sockaddr*)&a,sizeof(a));
        if(lan_frames<=10){printf("B82 CONNECT AFTER rc=%d errno=%d\n",n,n<0?errno:0);fflush(stdout);}
        tcp_connect_rc=n;tcp_connect_errno=n<0?errno:0;
        if(n==0){tcp_state=4;tcp_connected_count++;relay_handshake_sent=0;printf("SYNC TCP CONNECT immediate frame=%u\n",lan_frames);fflush(stdout);}
        else if(errno==EINPROGRESS || errno==EWOULDBLOCK)tcp_state=3;
        else {tcp_errno=errno;tcp_disconnect();}
    } else if(tcp_state==3) {
        FD_ZERO(&w);FD_SET(tcp_fd,&w);tv.tv_sec=0;tv.tv_usec=0;
        n=select(tcp_fd+1,NULL,&w,NULL,&tv);tcp_select_rc=n;
        if(n>0) {
            len=sizeof(e);tcp_so_result=getsockopt(tcp_fd,SOL_SOCKET,SO_ERROR,&e,&len);
            tcp_so_error=e;
            if(tcp_so_result==0 && !e){tcp_state=4;tcp_connected_count++;relay_handshake_sent=0;printf("SYNC TCP CONNECT complete frame=%u\n",lan_frames);fflush(stdout);}
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
            printf("SYNC TCP RECV DISCONNECT n=%d errno=%d frame=%u\n",n,n<0?errno:0,lan_frames);fflush(stdout);tcp_disconnect();return;
        }
        if(tcp_output_len==0) {
            if(!relay_handshake_sent) {
                const char *role=lan_profile==1?"PS2HOST/1\n":lan_profile==2?"PS2CLIENT/1\n":"PS2AUTO/1\n";
                tcp_output_len=(int)strlen(role);
                memcpy(tcp_output,role,tcp_output_len);
            } else if(lan_frames%6==1) {
                sync_sequence++;
                tcp_output_len=sprintf(tcp_output,"S1 %u %u %u %04X\n",
                    sync_sequence,sync_local_x,sync_local_y,sync_local_buttons);
                /* I2 is sent on the next tick to preserve S1 compatibility. */
            } else if(lan_frames%6==2) {
                int k;
                tcp_output_len=sprintf(tcp_output,"I2 %u %04X",sync_sequence,input_host.mask);
                for(k=0;k<4;k++)tcp_output_len+=sprintf(tcp_output+tcp_output_len," %02X",input_host.analog[k]);
                for(k=0;k<12;k++)tcp_output_len+=sprintf(tcp_output+tcp_output_len," %02X",input_host.pressure[k]);
                tcp_output[tcp_output_len++]='\n';
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
static const char *format_net_rc(int value) {
    static char buffer[24];
    if (value == -999) return "NOT ATTEMPTED";
    snprintf(buffer, sizeof(buffer), "%d", value);
    return buffer;
}
static volatile int lan_operation_entered = 0;
static volatile unsigned int netman_wait_frames = 0;
static volatile int netman_call_pending = 0;
static volatile int lan_operation_completed = 0;
/* B72: timestamped TTY markers; preserve all existing diagnostics. */
#define B79_TRACE(stage, rc) do { printf("B79 [%lu ticks] %s rc=%d\n", (unsigned long)clock(), stage, (int)(rc)); fflush(stdout); } while (0)
static volatile int lan_main_handoff = 0;
static volatile int lan_boot_modules_ready = 0;
static volatile int lan_boot_modules_error = 0;
static volatile int lan_boot_netman_ready = 0;
static volatile int lan_boot_ps2ip_ready = 0;
static volatile int lan_boot_ps2ip_rc = -999;
static void lan_boot_load_modules(void) {
    int rc, mod_start_result = -999;
    B79_TRACE("BOOT MODULES BEFORE PAD ENTER", 0);
    B79_TRACE("SIF LOADFILE ENTER", 0); SifLoadFileInit(); lan_operation_completed = 1; B79_TRACE("SIF LOADFILE RETURN", 0);
    lan_operation_entered = 2;
    B79_TRACE("IOP HEAP ENTER", 0); SifInitIopHeap(); lan_operation_completed = 2; B79_TRACE("IOP HEAP RETURN", 0);
    lan_operation_entered = 3;
    B79_TRACE("LMB PATCH ENTER", 0); rc = sbv_patch_enable_lmb(); lan_operation_completed = 3; B79_TRACE("LMB PATCH RETURN", rc);
    /* Artemis/debug-logger lesson: record the actual LOADFILE patch result,
       rather than printing success unconditionally. Zero means success. */
    if (rc != 0) { lan_boot_modules_error = rc ? rc : -1; B79_TRACE("LMB PATCH FAILED", rc); return; }
    lan_operation_entered = 4;
    B79_TRACE("DEV9 ENTER", 0); rc = SifExecModuleBuffer(DEV9_irx, size_DEV9_irx, 0, NULL, &mod_start_result); B79_TRACE("DEV9 RETURN", rc); B79_TRACE("DEV9 START RESULT", mod_start_result); mod_dev9_rc = rc; lan_operation_completed = 4;
    if (rc < 0) { lan_boot_modules_error = rc; return; }
    /* Preserve module-specific start results for diagnosis; some IRX modules
       legitimately return nonzero status, so do not reject them generically. */
    mod_start_result = -999;

    lan_init_stage = 2;
    lan_operation_entered = 5;
    B79_TRACE("NETMAN MODULE ENTER", 0); rc = SifExecModuleBuffer(NETMAN_irx, size_NETMAN_irx, 0, NULL, &mod_start_result); B79_TRACE("NETMAN MODULE RETURN", rc); B79_TRACE("NETMAN START RESULT", mod_start_result); mod_netman_rc = rc; lan_operation_completed = 5;
    if (rc < 0) { lan_boot_modules_error = rc; return; }
    mod_start_result = -999;

    lan_init_stage = 3;
    lan_operation_entered = 6;
    B79_TRACE("SMAP ENTER", 0); rc = SifExecModuleBuffer(SMAP_irx, size_SMAP_irx, 0, NULL, &mod_start_result); B79_TRACE("SMAP RETURN", rc); B79_TRACE("SMAP START RESULT", mod_start_result); mod_smap_rc = rc; lan_operation_completed = 6;
    if (rc < 0) { lan_boot_modules_error = rc; return; }

    lan_init_stage = 4;
    /* B75 isolation: invoke the same EE-side NETMAN RPC as Build 70,
       immediately after module loading, before PAD/GS/heartbeat threads. */
    B79_TRACE("EARLY NetManInit ENTER", 0);
    netman_call_pending = 1;
    lan_operation_entered = 7;
    rc = NetManInit();
    B79_TRACE("EARLY NetManInit RETURN", rc);
    netman_call_pending = 0;
    netman_init_rc = rc;
    lan_operation_completed = 7;
    if (rc < 0) { lan_boot_modules_error = rc; return; }
    lan_boot_netman_ready = 1;
    /* Independent, read-only DEV9 EEPROM probe; failure is non-fatal. */
    {
        int probe_start_rc = -999;
        eeprom_probe_module_id = SifExecModuleBuffer(MACPROBE_irx,
            size_MACPROBE_irx, 0, NULL, &probe_start_rc);
        if (eeprom_probe_module_id >= 0) read_dev9_eeprom_mac();
        else eeprom_mac_rc = eeprom_probe_module_id;
        printf("B86 EEPROM MAC module=%d start=%d rc=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
               eeprom_probe_module_id, probe_start_rc, eeprom_mac_rc,
               eeprom_mac[0],eeprom_mac[1],eeprom_mac[2],
               eeprom_mac[3],eeprom_mac[4],eeprom_mac[5]);
        fflush(stdout);
    }
    /* Query the MAC exposed by the active DEV9/SMAP interface. */
    memset(lan_mac, 0, sizeof(lan_mac));
    lan_mac_rc = NetManIoctl(NETMAN_NETIF_IOCTL_ETH_GET_MAC, NULL, 0,
                             lan_mac, sizeof(lan_mac));
    lan_mac_valid = (lan_mac_rc >= 0);
    printf("B85 NETMAN MAC rc=%d mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
           lan_mac_rc, lan_mac[0], lan_mac[1], lan_mac[2],
           lan_mac[3], lan_mac[4], lan_mac[5]);
    fflush(stdout);
    /* Build 76: initialize TCP/IP on main EE thread before PAD/GS. */
    {
        struct ip4_addr ip, nm, gw;
        IP4_ADDR(&ip, 0,0,0,0);
        IP4_ADDR(&nm, 0,0,0,0);
        IP4_ADDR(&gw, 0,0,0,0);
        lan_init_stage = 5;
        lan_operation_entered = 8;
        B79_TRACE("EARLY ps2ipInit ENTER", 0);
        rc = ps2ipInit(&ip, &nm, &gw);
        B79_TRACE("EARLY ps2ipInit RETURN", rc);
        ps2ip_init_rc = rc;
        lan_boot_ps2ip_rc = rc;
        lan_operation_completed = 8;
        if (rc < 0) { lan_boot_modules_error = rc; return; }
        lan_boot_ps2ip_ready = 1;
    }
    lan_boot_modules_ready = 1;
    B79_TRACE("BOOT MODULES BEFORE PAD RETURN", 0);
}

static int lan_worker_cancelled(void);
static void lan_finish_main(void);
static void lan_start_worker(void) {
    B79_TRACE("START_WORKER ENTRY STATE", lan_state);
    if (lan_state != 1) { B79_TRACE("START_WORKER BAD STATE", lan_state); return; }
    B79_TRACE("START_WORKER BEGIN", 0);
    net_init_attempted = 1;
    B79_TRACE("START_WORKER INIT ATTEMPT", net_init_attempted);
    lan_started_profile = lan_profile;
    lan_start_count++;
    lan_init_stage = 1;
    lan_ready = 0; lan_poll_count = 0; lan_poll_getconfig_rc = -999;
    lan_cfg_dhcp_enabled = -1; lan_cfg_ip_zero = -1;
    lan_state = 1;
    /* Session token distinguishes instances even behind virtual NAT. */
    /* Distinguish static-IP profiles and successive sessions.  The profile
       occupies the upper byte so the two test profiles cannot collide,
       even if emulated clock() values match exactly.  Not cryptographic. */
    lan_nonce = (((unsigned int)(lan_started_profile & 255)) << 24) |
                (((unsigned int)clock() ^ ((unsigned int)lan_start_count * 0x9E3779B9U)) & 0x00FFFFFFU);
    if (!lan_nonce) lan_nonce = 1;
    lan_operation_entered = 1;
    /* B74: modules loaded once at boot, before PADMAN and GS. */
    B79_TRACE("START_WORKER MODULES READY", lan_boot_modules_ready);
    if (!lan_boot_modules_ready) {
        lan_error = lan_boot_modules_error ? lan_boot_modules_error : -74;
        lan_state = -3;
        return;
    }
    lan_operation_completed = 6;
    lan_init_stage = 4;
    B79_TRACE("WORKER MODULES PRELOADED", 0);
    B79_TRACE("START_WORKER BEFORE FINISH", lan_state);
    lan_finish_main();
    B79_TRACE("START_WORKER AFTER FINISH", lan_state);

}
/* Run potentially blocking IOP module initialization off the UI thread.
   Keep lan_state == 1 until startup completes, so lan_tick() cannot
   touch the network stack while the worker is initializing it. */
static unsigned char lan_worker_stack[32768] __attribute__((aligned(16)));
static int lan_worker_thread = -1;
static volatile int lan_worker_entered = 0;
static volatile int lan_worker_exited = 1;
static volatile int lan_thread_create_rc = -999;
static volatile int lan_thread_start_rc = -999;
static volatile int lan_worker_entry_marker = 0;
static volatile int lan_thread_status_rc = -999;
static volatile int lan_thread_status = -1;
/* Independent EE scheduler probe: no IOP calls. */
static unsigned char heartbeat_stack[4096] __attribute__((aligned(16)));
static volatile unsigned int heartbeat_count = 0;
static volatile unsigned int ui_frame_count = 0;
static volatile unsigned int ui_input_count = 0;
static volatile int heartbeat_create_rc = -999;
static volatile int heartbeat_start_rc = -999;
static volatile int main_thread_priority = -1;
static void heartbeat_entry(void *arg) {
    (void)arg;
    for (;;) { heartbeat_count++; DelayThread(1000); }
}
static void heartbeat_start(void) {
    ee_thread_t t;
    int id;
    memset(&t, 0, sizeof(t));
    t.func = (void *)heartbeat_entry;
    t.stack = heartbeat_stack;
    t.stack_size = sizeof(heartbeat_stack);
    t.initial_priority = 48;
    id = CreateThread(&t);
    heartbeat_create_rc = id;
    if (id >= 0) heartbeat_start_rc = StartThread(id, NULL);
}
/* Cooperative cancellation: never terminate a thread during an IOP RPC. */
static volatile int lan_cancel_requested = 0;
static volatile int lan_worker_active = 0;
static clock_t lan_cancel_started = 0;
static int lan_cancel_stage = 0;
static void lan_close_sockets(void) {
    if (tcp_fd >= 0) { close(tcp_fd); tcp_fd = -1; }
    if (tcp_listen_fd >= 0) { close(tcp_listen_fd); tcp_listen_fd = -1; }
    if (lan_socket >= 0) { close(lan_socket); lan_socket = -1; }
    tcp_state = 0; tcp_initialized = 0;
    lan_ready = 0;
}
static void lan_request_cancel(void) {
    /* A second Circle must never reset state while the worker is active. */
    if (lan_state == 3 || lan_state == 4) return;
    if (lan_state == 1 && lan_worker_exited) { lan_state = -10; lan_error = lan_thread_start_rc; return; }
    if (lan_worker_active || lan_state == 1) {
        lan_cancel_requested = 1;
        lan_cancel_started = clock();
        lan_cancel_stage = lan_init_stage;
        lan_state = 3;
        return;
    }
    if (lan_state != 0) {
        lan_close_sockets();
        lan_state = 4; /* Cancelled/disconnected; user can change profile. */
        lan_error = 0;
        lan_started_profile = -1;
    }
}
static int lan_worker_cancelled(void) {
    /* Only the worker acknowledges cancellation, after returning from RPC. */
    if (!lan_cancel_requested) return 0;
    lan_close_sockets();
    lan_error = 0;
    lan_started_profile = -1;
    return 1;
}
static void lan_finish_main(void) {
    struct ip4_addr ip, nm, gw;
    struct sockaddr_in addr;
    int one = 1, rc;
    if (lan_cancel_requested) { lan_state = 4; return; }
    B79_TRACE("WORKER NETMAN START", 0);
    lan_operation_entered = 7;
    netman_call_pending = 1;
    /* B75: do not call NetManInit twice. Boot already initialized it. */
    rc = lan_boot_netman_ready ? netman_init_rc : -75;
    B79_TRACE("WORKER REUSE EARLY NETMAN", rc);
    lan_operation_completed = 7;
    netman_call_pending = 0;
    if (rc < 0) { lan_error = rc; lan_state = -4; return; }
    IP4_ADDR(&ip, 0,0,0,0);
    IP4_ADDR(&nm, 0,0,0,0);
    IP4_ADDR(&gw, 0,0,0,0);
    if (lan_worker_cancelled()) return;
    lan_init_stage = 5;
    lan_operation_entered = 8;
    /* Build 76: reuse early ps2ipInit result; never initialize twice. */
    rc = lan_boot_ps2ip_ready ? lan_boot_ps2ip_rc : -76;
    B79_TRACE("WORKER REUSE EARLY PS2IP", rc); ps2ip_init_rc = rc; lan_operation_completed = 8;
    if (rc < 0) { lan_error = rc; lan_state = -7; return; }
    if (lan_worker_cancelled()) return;
    lan_init_stage = 6;
    /* SMAP registration can lag behind ps2ipInit. Defer configuration
       until sm0 is visible; never treat socket creation as IP readiness. */
    lan_getconfig_rc = -1;
    lan_config_error = -1;
    if (lan_worker_cancelled()) return;
    lan_init_stage = 7;
    lan_operation_entered = 9;
    B79_TRACE("UDP SOCKET ENTER", 0);
    lan_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); lan_operation_completed = 9; B79_TRACE("UDP SOCKET RETURN", lan_socket);
    if (lan_socket < 0) { lan_error = lan_socket; lan_state = -5; return; }
    lan_operation_entered = 10; B79_TRACE("UDP SETSOCKOPT ENTER", 0);
    rc = setsockopt(lan_socket, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    lan_operation_completed = 10; B79_TRACE("UDP SETSOCKOPT RETURN", rc);
    if (rc < 0) { lan_error = errno; lan_state = -5; lan_close_sockets(); return; }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    lan_operation_entered = 11; B79_TRACE("UDP BIND ENTER", 0);
    rc = bind(lan_socket, (struct sockaddr *)&addr, sizeof(addr));
    lan_operation_completed = 11; B79_TRACE("UDP BIND RETURN", rc);
    if (rc < 0) {
        lan_state = -6; lan_error = -6; return;
    }
    if (lan_worker_cancelled()) return;
    lan_init_stage = 8;
    if (lan_worker_cancelled()) return;
    lan_state = 2; B79_TRACE("WORKER LAN READY FOR POLL", 0);
    /* Wait until IP configuration is available before starting TCP. */
}
static void lan_worker_entry(void *arg) {
    B79_TRACE("WORKER ENTER", 0);
    (void)arg;
    lan_worker_entry_marker = 1;
    B79_TRACE("WORKER ENTRY MARKER", lan_worker_entry_marker);
    lan_worker_active = 1;
    B79_TRACE("WORKER ACTIVE SET", lan_worker_active);
    lan_worker_entered = 1;
    B79_TRACE("WORKER CANCEL FLAG", lan_cancel_requested);
    if (!lan_worker_cancelled()) {
        B79_TRACE("WORKER BEFORE START", lan_state);
        lan_start_worker();
        B79_TRACE("WORKER AFTER START", lan_state);
    } else {
        B79_TRACE("WORKER CANCELLED BEFORE START", lan_state);
    }
    if (lan_cancel_requested) {
        lan_close_sockets();
        lan_state = 4;
    }
    lan_worker_active = 0;
    lan_worker_exited = 1;
    ExitThread();
}
static void lan_start(void) {
    ee_thread_t worker;
    int id, rc;
    if ((lan_state != 0 && lan_state != 4) || !lan_worker_exited) return;
    if (lan_worker_thread >= 0) {
        DeleteThread(lan_worker_thread);
        lan_worker_thread = -1;
    }
    lan_cancel_requested = 0;
    lan_cancel_started = 0;
    lan_main_handoff = 0;
    lan_worker_entered = 0;
    lan_worker_exited = 0;
    netman_call_pending = 0; netman_wait_frames = 0;
    lan_operation_entered = 0;
    lan_operation_completed = 0;
    lan_thread_create_rc = -999;
    lan_thread_start_rc = -999;
    lan_worker_entry_marker = 0;
    lan_thread_status_rc = -999;
    lan_thread_status = -1;
    lan_close_sockets();
    lan_link = -1; lan_dhcp_status = -1;
    memset(&worker, 0, sizeof(worker));
    worker.func = (void *)lan_worker_entry;
    worker.stack = lan_worker_stack;
    worker.stack_size = sizeof(lan_worker_stack);
    worker.initial_priority = 45; /* Lower priority than main UI (40), prevent worker starvation of PAD/render. */
    lan_state = 1;
    lan_init_stage = 0;
    id = CreateThread(&worker);
    lan_thread_create_rc = id;
    if (id < 0) {
        lan_error = id;
        lan_state = -8;
        lan_worker_exited = 1;
        return;
    }
    lan_worker_thread = id;
    rc = StartThread(id, NULL);
    lan_thread_start_rc = rc;
    if (rc < 0) {
        lan_error = rc;
        lan_state = -9;
        DeleteThread(id);
        lan_worker_thread = -1;
        lan_worker_exited = 1;
    }
}
static void lan_tick(void) {
    struct sockaddr_in dest, from;
    socklen_t fromlen;
    char packet[64];
    unsigned int received_nonce;
    int n;
    if (lan_state != 2) return;
    if (lan_poll_count < 10) { printf("B81 TICK enter count=%d frames=%u\n",lan_poll_count,lan_frames); fflush(stdout); }
    lan_link = NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0);
    {
        t_ip_info cfg;
        lan_poll_count++;
        if (lan_poll_count == 1 || lan_poll_count % 300 == 0)
            printf("B80 POLL profile=%d started=%d state=%d link=%d polls=%d cfg_rc=%d cfg_state=%d frames=%u tx_b=%u tx_u=%u rx_b=%u rx_u=%u invalid=%u self=%u\n",
                   lan_profile, lan_started_profile, lan_state, lan_link, lan_poll_count,
                   lan_poll_getconfig_rc, lan_config_error, lan_frames,
                   lan_bcast_tx, lan_ucast_tx, lan_bcast_rx, lan_ucast_rx, lan_invalid_rx, lan_self_rx);
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
            printf("B80 SETCONFIG profile=%d rc=%d ip=%08X dhcp=%d\n", lan_started_profile,
                   lan_config_error, (unsigned int)ntohl(cfg.ipaddr.s_addr), cfg.dhcp_enabled);
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
        if (lan_poll_count <= 5 || lan_poll_count % 300 == 0)
            printf("B80 CONFIG profile=%d rc=%d ip=%08X dhcp=%d dhcp_status=%d\n",
                   lan_started_profile, lan_poll_getconfig_rc,
                   (unsigned int)ntohl(cfg.ipaddr.s_addr), cfg.dhcp_enabled, cfg.dhcp_status);
        if (lan_poll_count <= 10) { printf("B81 CONFIG GATE PASS poll=%d\n",lan_poll_count); fflush(stdout); }
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
    if (lan_frames <= 10 || lan_frames % 60 == 0) { printf("B81 FRAME %u poll=%d\n",lan_frames,lan_poll_count); fflush(stdout); }
    /* Diagnostic input: changing coordinates without requiring a physical pad.
       Compile with -DTEST_DYNAMIC_INPUT_A or -DTEST_DYNAMIC_INPUT_B. */
#if defined(TEST_DYNAMIC_INPUT_A)
    if (lan_frames >= 60 && lan_frames < 120) {
        previous_buttons = (unsigned short)~PAD_RIGHT;
        if (lan_frames == 60) printf("DYNAMIC_INPUT A RIGHT start frame=%u\\n", lan_frames);
    } else previous_buttons = 0xffff;
#elif defined(TEST_DYNAMIC_INPUT_B)
    if (lan_frames >= 90 && lan_frames < 150) {
        previous_buttons = (unsigned short)~PAD_LEFT;
        if (lan_frames == 90) printf("DYNAMIC_INPUT B LEFT start frame=%u\\n", lan_frames);
    } else previous_buttons = 0xffff;
#endif
    sync_local_buttons=(unsigned int)(~previous_buttons)&0xffff;
    /* Host input is sampled in read_pressed(), regardless of LAN state. */
    if((~previous_buttons)&PAD_LEFT && sync_local_x>2)sync_local_x-=2;
    if((~previous_buttons)&PAD_RIGHT && sync_local_x<638)sync_local_x+=2;
    if((~previous_buttons)&PAD_UP && sync_local_y>2)sync_local_y-=2;
    if((~previous_buttons)&PAD_DOWN && sync_local_y<478)sync_local_y+=2;
    if (lan_frames <= 10) { printf("B81 TCP BEFORE frame=%u\n",lan_frames); fflush(stdout); }
    if(!tcp_initialized){tcp_setup();tcp_initialized=1;}
    if (lan_frames <= 10) { printf("B81 TCP SETUP DONE frame=%u\n",lan_frames); fflush(stdout); }
    /* B83: Do not let the client-side PS2IP connect() block UDP discovery.
       The client TCP path is gated until a peer HELLO has been received.
       This is a diagnostic workaround, not a permanent TCP connect fix. */
    if (lan_profile != 2 || peer_last_frame != 0) {
        tcp_tick();
        if (lan_frames <= 10) { printf("B83 TCP TICK DONE frame=%u\n",lan_frames); fflush(stdout); }
    } else if (lan_frames <= 10 || lan_frames % 120 == 1) {
        printf("B83 TCP CLIENT DEFERRED frame=%u until UDP peer discovered\n",lan_frames); fflush(stdout);
    }
    if ((lan_frames % 120) == 1) {
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(DISCOVERY_PORT);
        dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sprintf(packet, HELLO_PREFIX "%08X", lan_nonce);
        printf("B81 BEFORE SEND broadcast frame=%u fd=%d\n",lan_frames,lan_socket); fflush(stdout);
        n = sendto(lan_socket, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
        printf("B81 AFTER SEND broadcast result=%d\n",n); fflush(stdout);
        if (n > 0) { lan_tx += (unsigned int)n; lan_bcast_tx++; }
        else { lan_send_error = n; lan_bcast_fail++; }
        if (lan_frames <= 121 || lan_frames % 1200 == 1)
            printf("B80 SEND broadcast frame=%u result=%d success=%u fail=%u\n", lan_frames, n, lan_bcast_tx, lan_bcast_fail);
        /* Static profiles also send to the other console directly. */
        if (lan_profile != 0) {
            dest.sin_addr.s_addr = htonl((192U<<24)|(168U<<16)|(50U<<8)|(lan_profile==1?102U:101U));
            printf("B81 BEFORE SEND unicast frame=%u\n",lan_frames); fflush(stdout);
            n = sendto(lan_socket, packet, strlen(packet), 0, (struct sockaddr *)&dest, sizeof(dest));
            printf("B81 AFTER SEND unicast result=%d\n",n); fflush(stdout);
            if (n > 0) { lan_tx += (unsigned int)n; lan_ucast_tx++; }
            else { lan_send_error = n; lan_ucast_fail++; }
            if (lan_frames <= 121 || lan_frames % 1200 == 1)
                printf("B80 SEND unicast frame=%u result=%d success=%u fail=%u\n", lan_frames, n, lan_ucast_tx, lan_ucast_fail);
        }
    }
    /* Use a nonblocking receive flag so controller and graphics never stall. */
    fromlen = sizeof(from);
    n = recvfrom(lan_socket, packet, sizeof(packet)-1, MSG_DONTWAIT,
                 (struct sockaddr *)&from, &fromlen);
    if (n <= 0) {
        if (n == 0) ++b84_recv_empty;
        else { ++b84_recv_error; if (b84_recv_error <= 8 || lan_frames % 600 == 1) printf("B84 RECV NEG frame=%u rc=%d errno=%d fd=%d errors=%u\n",lan_frames,n,errno,lan_socket,b84_recv_error); }
    } else {
        ++b84_recv_data;
        if (b84_recv_data <= 16) printf("B84 RECV RAW frame=%u n=%d from=%08X port=%u data=%.*s\n",lan_frames,n,(unsigned)ntohl(from.sin_addr.s_addr),(unsigned)ntohs(from.sin_port),n,packet);
    }
    if (lan_frames <= 10 || lan_frames % 120 == 1) { printf("B84 RECV STATS frame=%u fd=%d data=%u zero=%u neg=%u invalid=%u self=%u peer=%u\n",lan_frames,lan_socket,b84_recv_data,b84_recv_empty,b84_recv_error,lan_invalid_rx,lan_self_rx,lan_bcast_rx+lan_ucast_rx); fflush(stdout); }
    if (n > 0) {
        packet[n] = 0;
        if (n == 19 && !memcmp(packet, HELLO_PREFIX, HELLO_PREFIX_LEN) &&
            sscanf(packet + HELLO_PREFIX_LEN, "%8x", &received_nonce) == 1) {
            if (received_nonce == lan_nonce) {
                ++lan_self_rx;
            } else {
                unsigned int a = ntohl(from.sin_addr.s_addr);
                sprintf(peer_address, "%u.%u.%u.%u", (a>>24)&255,(a>>16)&255,(a>>8)&255,a&255);
                if (lan_bcast_rx + lan_ucast_rx < 5)
                    printf("B80 PEER from=%s nonce=%08X frame=%u\n", peer_address, received_nonce, lan_frames);
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

static volatile int dbg_pad_sio2_rc=-999, dbg_pad_module_rc=-999;
static volatile int dbg_pad_init_rc=-999, dbg_pad_open_rc=-999;
static volatile unsigned int dbg_pad_retries=0, dbg_state_changes=0;
static volatile int dbg_last_logged_state=-999;
static void pad_setup(void) {
    int state;
    SifInitRpc(0);
    dbg_pad_sio2_rc=SifLoadModule("rom0:SIO2MAN", 0, NULL);
    B79_TRACE("PAD SIO2MAN LOAD", dbg_pad_sio2_rc);
    if (dbg_pad_sio2_rc < 0) return;
    dbg_pad_module_rc=SifLoadModule("rom0:PADMAN", 0, NULL);
    B79_TRACE("PAD PADMAN LOAD", dbg_pad_module_rc);
    if (dbg_pad_module_rc < 0) return;
    dbg_pad_init_rc=padInit(0);
    B79_TRACE("PAD INIT", dbg_pad_init_rc);
    if (dbg_pad_init_rc == 0) return;
    dbg_pad_open_rc=padPortOpen(PAD_PORT, PAD_SLOT, pad_buffer);
    B79_TRACE("PAD PORT OPEN", dbg_pad_open_rc);
    if (dbg_pad_open_rc == 0) return;
    state=padGetState(PAD_PORT,PAD_SLOT);
    dbg_last_logged_state=state;
    B79_TRACE("PAD INITIAL STATE",state);
    pad_ready=1; /* Port opened; read_pressed still checks actual state. */
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
static int pressure_activation_attempted=0;
static int pressure_setup_phase=0, pressure_setup_wait=0, pressure_mode_active=0;
static int pressure_setup_retries=0;
static int pressure_mode_supported=0, pressure_mode_result=0;
static int analog_mode_result=0;
static int controller_mode_id=-1; /* PAD_TYPE_DIGITAL=4, PAD_TYPE_DUALSHOCK=7 */
static unsigned int controller_mode_changes=0;
static int previous_controller_mode_id=-1;
static unsigned short read_pressed(void) {
    int state=padGetState(PAD_PORT,PAD_SLOT);
    unsigned short current=0xffff, pressed=0;
    dbg_poll++; dbg_state=state;
    if (state!=dbg_last_logged_state) {
        dbg_last_logged_state=state; dbg_state_changes++;
        B79_TRACE("PAD STATE CHANGE",state);
    }
    if (!pad_ready || (state!=PAD_STATE_STABLE && state!=PAD_STATE_FINDCTP1)) {
        previous_buttons=0xffff;
        dbg_pressed=0;
        capture_host_input();
        if (state==PAD_STATE_DISCONN) {
            pressure_setup_phase=0;
            pressure_setup_wait=0;
            pressure_setup_retries=0;
            pressure_mode_active=0;
        }
        return 0;
    }
    /* libpad mode transitions are asynchronous. Never request analog and
       pressure mode back-to-back in the same poll: the pad can be BUSY.
       Retry after a stable handshake, and renegotiate after disconnect. */
    if (pressure_setup_phase==0) {
        pressure_activation_attempted=1;
        analog_mode_result=padSetMainMode(PAD_PORT,PAD_SLOT,PAD_MMODE_DUALSHOCK,PAD_MMODE_UNLOCK);
        pressure_setup_phase=1;
        pressure_setup_wait=0;
    } else if (pressure_setup_phase==1) {
        if (++pressure_setup_wait>=3) {
            pressure_mode_supported=padInfoPressMode(PAD_PORT,PAD_SLOT);
            if (pressure_mode_supported>0) {
                pressure_mode_result=padEnterPressMode(PAD_PORT,PAD_SLOT);
                pressure_setup_phase=2;
                pressure_setup_wait=0;
            } else if (++pressure_setup_retries>=120) {
                pressure_setup_phase=3; /* Unsupported: report it, do not invent values. */
            }
        }
    } else if (pressure_setup_phase==2) {
        /* Allow the IOP to finish the pressure-mode transition. */
        if (++pressure_setup_wait>=4) {
            pressure_mode_active=(pressure_mode_result>0);
            pressure_setup_phase=3;
        }
    }
    controller_mode_id=padInfoMode(PAD_PORT,PAD_SLOT,PAD_MODECURID,0);
    if(controller_mode_id!=previous_controller_mode_id){
        if(previous_controller_mode_id>=0)controller_mode_changes++;
        previous_controller_mode_id=controller_mode_id;
    }
    dbg_read=padRead(PAD_PORT,PAD_SLOT,&buttons);
    if (dbg_read==0) {
        previous_buttons=0xffff;
        capture_host_input();
        dbg_pad_retries++;
        return 0;
    }
    dbg_readok++;
    current=buttons.btns; dbg_raw=current;
    pressed=(unsigned short)(previous_buttons & (unsigned short)~current);
    previous_buttons=current; dbg_pressed=pressed;
    /* Keep host diagnostics live on the main menu and without Ethernet. */
    capture_host_input();
    if (pressed) {
        dbg_edges++;
        B79_TRACE("PAD BUTTON EDGE",pressed);
    }
    return pressed;
}

static const char glyph_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.-/()># |+%";
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
{0x0,0x4,0x4,0x1f,0x4,0x4,0x0},
{0x19,0x19,0x2,0x4,0x8,0x13,0x13}
};
static GSGLOBAL *gs;
#define COLOR(r,g,b) GS_SETREG_RGBAQ((r),(g),(b),0x80,0)
static void rect(float x,float y,float w,float h,u64 color) {
    gsKit_prim_sprite(gs,x,y,x+w,y+h,1,color);
}
static void label(float x,float y,float scale,u64 color,const char *str) {
    int i,j,k;
    float origin=x;
    if (scale < 1.0f) scale = 1.0f; /* pixel glyphs must not shrink below one GS pixel */
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
    char line[192];
    Layout l=layouts[layout_index];
    float W=(float)gs->Width,H=(float)gs->Height;
    float sx=W/640.0f,sy=H/448.0f;
    int i;
    const char *items[]={"NETWORK ADAPTER","SCREEN LAYOUT","DIAGNOSTIC COUNTERS","ABOUT / STATUS","INPUT DIAGNOSTICS"};
    gsKit_clear(gs,COLOR(8,17,32));
    rect(20*sx,20*sy,600*sx,55*sy,COLOR(20,40,65));
    label(34*sx,34*sy,2.5f*sx,white,"PS2 MULTI-SCREEN NETWORK TEST");
    rect(20*sx,85*sy,600*sx,2*sy,accent);
    if(page==0) {
        for(i=0;i<5;i++) {
            float y=(113+i*49)*sy;
            if(selected==i)rect(24*sx,y-8*sy,390*sx,43*sy,COLOR(24,65,96));
            label(42*sx,y,2.2f*sx,selected==i?white:muted,items[i]);
        }
        label(34*sx,380*sy,1.8f*sx,muted,"D-PAD MOVE  X OPEN");
    } else if(page==6) {
        const char *diag_titles[]={"INPUT DIAGNOSTICS - HOST","INPUT DIAGNOSTICS - CLIENTS 01-16","INPUT DIAGNOSTICS - CLIENTS 17-31"};
        static const char *press_names[]={"UP","RIGHT","DOWN","LEFT","TRI","CIR","CROSS","SQR","L1","L2","R1","R2"};
        static const char *digital_names[]={"SELECT","L3","R3","START","UP","RIGHT","DOWN","LEFT","L2","R2","L1","R1","TRI","CIR","CROSS","SQR"};
        label(34*sx,94*sy,1.50f*sx,white,diag_titles[input_diag_tab]);
        if(input_diag_tab==0) {
            unsigned int mask=input_host.mask;
            snprintf(line,sizeof(line),"REVERSE JOKER (HEX): 0x%04X",mask);
            label(34*sx,125*sy,1.25f*sx,white,line);
            /* Separate labels let the colour of each button follow its current bit. */
            for(i=0;i<16;i++) {
                int column=i%8,row=i/8;
                float x=(35+column*73)*sx,y=(156+row*30)*sy;
                snprintf(line,sizeof(line),"%s:%d",digital_names[i],!!(mask&(1u<<i)));
                label(x,y,0.68f*sx,input_button_color(i,!!(mask&(1u<<i)),muted,accent),line);
            }
            snprintf(line,sizeof(line),"LX:%02X LY:%02X RX:%02X RY:%02X",input_host.analog[0],input_host.analog[1],input_host.analog[2],input_host.analog[3]);
            label(34*sx,226*sy,1.1f*sx,white,line);
            snprintf(line,sizeof(line),"MODE:%s (%d) CHANGES:%u PRESS:%d SUP:%d STEP:%d",
                controller_mode_id==PAD_TYPE_DIGITAL?"DIGITAL":
                controller_mode_id==PAD_TYPE_DUALSHOCK?"ANALOG":
                controller_mode_id==PAD_TYPE_ANALOG?"ANALOG":"UNKNOWN",
                controller_mode_id,controller_mode_changes,pressure_mode_result,pressure_mode_supported,pressure_setup_phase);
            label(34*sx,249*sy,0.76f*sx,muted,line);
            for(i=0;i<12;i++) {
                static const unsigned char pressure_bits[]={4,5,6,7,12,13,14,15,10,8,11,9};
                unsigned int active=mask&(1u<<pressure_bits[i]);
                /* Use native pressure bytes, not an estimate based on the mask. */
                snprintf(line,sizeof(line),"%s:%02X %3d%%",press_names[i],input_host.pressure[i],pressure_percent(input_host.pressure[i]));
                label((34+(i%4)*143)*sx,(276+(i/4)*28)*sy,0.79f*sx,input_button_color(i==4?12:i==5?13:i==6?14:i==7?15:-1,!!(input_host.pressure[i]||active),muted,accent),line);
            }
        } else {
            int first=input_diag_tab==1?0:16;
            int end=input_diag_tab==1?16:31;
            label(34*sx,120*sy,0.61f*sx,muted,"CL/PLAYER ST MASK   U   R   D   L   T   O   X   S  L1  L2  R1  R2  STSELR  LXLYRXRY");
            for(i=first;i<end;i++) {
                InputSnapshot *c=&input_clients[i];
                float y=(137+(i-first)*14.2f)*sy;
                int connected=c->valid && lan_frames-c->last_frame<300 && tcp_state==4;
                if(connected) {
                    unsigned int m=c->mask;
                    snprintf(line,sizeof(line),"%02d/P%02d ON %04X %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %02X/%03d %d%d%d%d %02X%02X%02X%02X",
                        i+1,i+2,m,c->pressure[0],pressure_percent(c->pressure[0]),c->pressure[1],pressure_percent(c->pressure[1]),
                        c->pressure[2],pressure_percent(c->pressure[2]),c->pressure[3],pressure_percent(c->pressure[3]),
                        c->pressure[4],pressure_percent(c->pressure[4]),c->pressure[5],pressure_percent(c->pressure[5]),
                        c->pressure[6],pressure_percent(c->pressure[6]),c->pressure[7],pressure_percent(c->pressure[7]),
                        c->pressure[8],pressure_percent(c->pressure[8]),c->pressure[9],pressure_percent(c->pressure[9]),
                        c->pressure[10],pressure_percent(c->pressure[10]),c->pressure[11],pressure_percent(c->pressure[11]),
                        !!(m&8),!!(m&1),!!(m&2),!!(m&4),
                        c->analog[0],c->analog[1],c->analog[2],c->analog[3]);
                } else snprintf(line,sizeof(line),"%02d/P%02d OFF --",i+1,i+2);
                label(34*sx,y,0.30f*sx,connected&&c->mask?accent:muted,line);
            }
            label(34*sx,372*sy,0.67f*sx,muted,"PRESSURES: HEX/PERCENT  |  STICKS: HEX 00-FF  |  OFF = NO REMOTE DATA");
        }
        label(34*sx,397*sy,0.85f*sx,muted,"LEFT/RIGHT: HOST / CLIENTS 1 / CLIENTS 2     TRIANGLE: MENU");
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
        if (lan_mac_valid)
            snprintf(line,sizeof(line),"ADAPTER MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     lan_mac[0],lan_mac[1],lan_mac[2],lan_mac[3],lan_mac[4],lan_mac[5]);
        else
            snprintf(line,sizeof(line),"ADAPTER MAC: UNAVAILABLE (RC:%d)",lan_mac_rc);
        label(34*sx,350*sy,0.91f*sx,muted,line);
        if (eeprom_mac_valid)
            snprintf(line,sizeof(line),"DEV9 EEPROM: %02X:%02X:%02X:%02X:%02X:%02X",
                     eeprom_mac[0],eeprom_mac[1],eeprom_mac[2],eeprom_mac[3],eeprom_mac[4],eeprom_mac[5]);
        else snprintf(line,sizeof(line),"DEV9 EEPROM: UNAVAILABLE (RC:%d)",eeprom_mac_rc);
        label(34*sx,368*sy,0.91f*sx,muted,line);
        snprintf(line,sizeof(line),"MAC MATCH: %s",
                 !lan_mac_valid || !eeprom_mac_valid ? "UNAVAILABLE" :
                 memcmp(lan_mac,eeprom_mac,6)==0 ? "YES" : "NO");
        label(34*sx,386*sy,0.91f*sx,muted,line);
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
        sprintf(line,"LAN: %s  ERROR: %d",lan_state==2?"DISCOVERING":lan_state==0?"NOT STARTED":lan_state==3?"CANCEL REQUESTED":lan_state==4?"CANCELLED":lan_state==1?"INITIALIZING":"FAILED",lan_error);
        label(34*sx,145*sy,1.28f*sx,muted,line);
        if (lan_state == 3) {
            unsigned long seconds = (unsigned long)((clock() - lan_cancel_started) / CLOCKS_PER_SEC);
            snprintf(line, sizeof(line), "CANCEL %lus %s T:%d/%d OP:%d/%d", seconds, seconds >= 5 ? "STALLED" : "WAIT", lan_worker_entered, lan_worker_exited, lan_operation_entered, lan_operation_completed);
        } else if (lan_state == 4) {
            snprintf(line, sizeof(line), "CANCELLED - SELECT PROFILE / CROSS RETRY");
        } else {
            snprintf(line, sizeof(line), "LOCAL IP: %s",local_address);
        }
        label(34*sx,169*sy,1.25f*sx,muted,line);
        if (lan_state == 1 || lan_state == 3 || lan_state == -8 || lan_state == -9 || lan_state == -10) {
            snprintf(line,sizeof(line),"THREAD:%d CR:%d ST:%d HBCR:%d HBST:%d",lan_worker_thread,lan_thread_create_rc,lan_thread_start_rc,heartbeat_create_rc,heartbeat_start_rc);
            label(34*sx,356*sy,0.95f*sx,muted,line);
            snprintf(line,sizeof(line),"ENTRY:%d STATUS:%d HB:%u",lan_worker_entry_marker,lan_thread_status,heartbeat_count);
            label(34*sx,375*sy,0.95f*sx,muted,line);
        }
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
    } else if(page==5) {
        /* Diagnostics page 2 owns all low-level TCP and initialization
           counters. Keep every baseline within the content viewport. */
        label(34*sx,105*sy,2.0f*sx,white,"DIAGNOSTICS 2 / 2");
        sprintf(line,"DEV9 MODULE RC: %s",format_net_rc(mod_dev9_rc));
        label(34*sx,139*sy,1.0f*sx,muted,line);
        sprintf(line,"NETMAN MODULE RC: %s",format_net_rc(mod_netman_rc));
        label(34*sx,162*sy,1.0f*sx,muted,line);
        sprintf(line,"SMAP MODULE RC: %s",format_net_rc(mod_smap_rc));
        label(34*sx,185*sy,1.0f*sx,muted,line);
        if (netman_call_pending) snprintf(line,sizeof(line),"NETMAN INIT: IN CALL %us",netman_wait_frames/60);
        else sprintf(line,"NETMAN INIT RC: %s",format_net_rc(netman_init_rc));
        label(34*sx,208*sy,1.0f*sx,muted,line);
        sprintf(line,"PS2IP INIT RC: %s",format_net_rc(ps2ip_init_rc));
        label(34*sx,231*sy,1.0f*sx,muted,line);
        sprintf(line,"CONFIG RC:%d  POLL RC:%d",lan_getconfig_rc,lan_poll_getconfig_rc);
        label(34*sx,254*sy,1.0f*sx,muted,line);
        sprintf(line,"READY:%d LINK:%d STARTS:%d",lan_ready,lan_link,lan_start_count);
        label(34*sx,277*sy,1.0f*sx,muted,line);
        sprintf(line,"PROFILE:%d IP:%s",lan_started_profile,local_address);
        label(34*sx,300*sy,1.0f*sx,muted,line);
        sprintf(line,"TCP:%d ERR:%d TX/RX:%u/%u TRY:%u",tcp_state,tcp_errno,tcp_tx,tcp_rx,tcp_tries);
        label(34*sx,323*sy,1.0f*sx,muted,line);
        sprintf(line,"INIT:%d STARTS:%d PROFILE:%d CFG:%d",lan_init_stage,lan_start_count,lan_started_profile,lan_getconfig_rc);
        label(34*sx,346*sy,1.0f*sx,muted,line);
        sprintf(line,"READY:%d POLL:%d CFGRC:%d",lan_ready,lan_poll_count,lan_poll_getconfig_rc);
        label(34*sx,369*sy,1.0f*sx,muted,line);
    } else {
        /* Reserve y=0..68 for controller debug and y>=405 for footer.
           Every row uses a unique baseline; long values use separate rows. */
        label(34*sx,94*sy,1.85f*sx,white,"ABOUT / STATUS");
        label(34*sx,132*sy,1.20f*sx,muted,"CONTROLLER: ACTIVE   LAN: EXPERIMENTAL");
        sprintf(line,"TCP TARGET: %u.%u.%u.%u:%d",(tcp_target_ip>>24)&255,(tcp_target_ip>>16)&255,(tcp_target_ip>>8)&255,tcp_target_ip&255,TCP_TEST_PORT);
        label(34*sx,156*sy,0.99f*sx,muted,line);
        sprintf(line,"GATEWAY: %u.%u.%u.%u",(tcp_gateway_ip>>24)&255,(tcp_gateway_ip>>16)&255,(tcp_gateway_ip>>8)&255,tcp_gateway_ip&255);
        label(34*sx,179*sy,0.99f*sx,muted,line);
        sprintf(line,"MASK: %u.%u.%u.%u",(tcp_netmask_ip>>24)&255,(tcp_netmask_ip>>16)&255,(tcp_netmask_ip>>8)&255,tcp_netmask_ip&255);
        label(34*sx,201*sy,0.99f*sx,muted,line);
        sprintf(line,"CONNECT RC:%d ERR:%d SELECT:%d",tcp_connect_rc,tcp_connect_errno,tcp_select_rc);
        label(34*sx,223*sy,0.94f*sx,muted,line);
        sprintf(line,"SO RESULT:%d ERROR:%d CONNECTED:%u",tcp_so_result,tcp_so_error,tcp_connected_count);
        label(34*sx,245*sy,0.94f*sx,muted,line);
        sprintf(line,"TCP RECV:%d SEND:%d STATE:%d ERR:%d TRY:%u",tcp_last_recv,tcp_last_send,tcp_state,tcp_errno,tcp_tries);
        label(34*sx,267*sy,0.86f*sx,muted,line);
        sprintf(line,"UDP SEND RC:%d BCAST FAIL:%u UCAST FAIL:%u",lan_send_error,lan_bcast_fail,lan_ucast_fail);
        label(34*sx,289*sy,0.88f*sx,muted,line);
        sprintf(line,"BCAST TX/RX:%u/%u UCAST TX/RX:%u/%u",lan_bcast_tx,lan_bcast_rx,lan_ucast_tx,lan_ucast_rx);
        label(34*sx,311*sy,0.90f*sx,muted,line);
        sprintf(line,"DEV9:%s NETMAN:%s SMAP:%s",mod_dev9_rc==-999?"WAIT":"DONE",mod_netman_rc==-999?"WAIT":"DONE",mod_smap_rc==-999?"WAIT":"DONE");
        label(34*sx,337*sy,1.0f*sx,muted,line);
        sprintf(line,"NET:%s IP:%s CFG:%s",netman_init_rc==-999?"WAIT":"DONE",ps2ip_init_rc==-999?"WAIT":"DONE",lan_poll_getconfig_rc==-999?"WAIT":"DONE");
        label(34*sx,362*sy,1.0f*sx,muted,line);
        /* Footer already provides navigation instructions. */
    }
    { char dbg[128];
      rect(0,0,640*sx,68*sy,COLOR(5,15,35));
      sprintf(dbg,"PAD READY:%d STATE:%d READ:%d PAGE:%d",pad_ready,dbg_state,dbg_read,page);
      label(8*sx,4*sy,1.0f*sx,white,dbg);
      sprintf(dbg,"RAW:%04X EDGE:%04X POLLS:%u OK:%u EDGES:%u",dbg_raw,dbg_pressed,dbg_poll,dbg_readok,dbg_edges);
      label(8*sx,26*sy,0.95f*sx,white,dbg);
      sprintf(dbg,"UI:%u INPUT:%u OP:%d/%d HB:%u",ui_frame_count,ui_input_count,lan_operation_entered,lan_operation_completed,heartbeat_count);
      label(8*sx,48*sy,0.85f*sx,white,dbg);
    }
    rect(20*sx,405*sy,600*sx,1*sy,COLOR(65,90,115));
    label(34*sx,418*sy,1.12f*sx,muted,page==3?"PAGE 1/2   RIGHT: MODULES   UP/DOWN: PROFILE":page==5?"PAGE 2/2   LEFT: COUNTERS":"CROSS: OPEN   TRIANGLE: BACK");
}

int main(int argc, char **argv) {
    unsigned short pressed;
    unsigned int redraw = 0;
    (void)argc; (void)argv;
    B79_TRACE("MAIN ENTER", 0);
    /* Match PS2SDK network examples: reset/synchronize IOP before loading
       PADMAN/SIO2MAN or starting any network worker. Never reset mid-session. */
    SifInitRpc(0);
    B79_TRACE("IOP RESET ENTER", 0);
    while (!SifIopReset("", 0)) { DelayThread(1000); }
    while (!SifIopSync()) { DelayThread(1000); }
    SifInitRpc(0);
    B79_TRACE("IOP RESET RETURN", 0);
    build_layouts();
    ChangeThreadPriority(GetThreadId(), 40);
    lan_boot_load_modules();
    heartbeat_start();
    B79_TRACE("PAD SETUP ENTER", 0);
    pad_setup();
    B79_TRACE("PAD SETUP RETURN", 0);
    gs=gsKit_init_global();
    gs->PSM=GS_PSM_CT16;
    gs->PSMZ=GS_PSMZ_16;
    gs->ZBuffering=GS_SETTING_OFF;
    gs->DoubleBuffering=GS_SETTING_ON;
    dmaKit_init(D_CTRL_RELE_OFF,D_CTRL_MFD_OFF,D_CTRL_STS_UNSPEC,D_CTRL_STD_OFF,D_CTRL_RCYC_8,1<<DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs,GS_ONESHOT);
    /* Normal startup: main menu; LAN is started explicitly by the user. */
    page = 0;
    selected = 0;
    lan_profile = 1; /* Player 1 / Host is the default role, but not auto-started. */
    while (running) {
        if (netman_call_pending) ++netman_wait_frames;
        if (lan_worker_thread >= 0 && !lan_worker_exited) {
            ee_thread_status_t ts;
            lan_thread_status_rc = ReferThreadStatus(lan_worker_thread, &ts);
            lan_thread_status = lan_thread_status_rc >= 0 ? ts.status : -1;
        }
        if (ui_frame_count < 10 || ui_frame_count % 60 == 0) { printf("B81 UI LOOP frame=%u state=%d\n",ui_frame_count,lan_state); fflush(stdout); }
        lan_tick();
        pressed = read_pressed();
        if (pressed) { ui_input_count++; B79_TRACE("UI BUTTON EDGE", (int)pressed); }
        if (pressed & PAD_TRIANGLE) { page = 0; redraw = 1; }
        if ((pressed & PAD_RIGHT) && page == 3) { page = 5; redraw = 1; }
        else if ((pressed & PAD_LEFT) && page == 5) { page = 3; redraw = 1; }
        if (page == 0) {
            if (pressed & PAD_DOWN) { selected = (selected + 1) % 5; redraw = 1; }
            if (pressed & PAD_UP) { selected = (selected + 4) % 5; redraw = 1; }
            if (pressed & PAD_CROSS) { page = selected == 4 ? 6 : selected + 1; if(page == 1) probe_network_modules();  redraw = 1; }
        } else if (page == 3) {
            if (pressed & PAD_CIRCLE) { lan_request_cancel(); redraw = 1; }
            if ((lan_state == 0 || lan_state == 4) && !lan_worker_active && (pressed & PAD_DOWN)) { lan_profile = (lan_profile + 1) % 3; redraw = 1; }
            if ((lan_state == 0 || lan_state == 4) && !lan_worker_active && (pressed & PAD_UP)) { lan_profile = (lan_profile + 2) % 3; redraw = 1; }
            if ((lan_state == 0 || lan_state == 4) && !lan_worker_active && (pressed & PAD_CROSS)) lan_start();
        } else if (page == 6) {
            if (pressed & PAD_RIGHT) { input_diag_tab=(input_diag_tab+1)%3; redraw=1; }
            if (pressed & PAD_LEFT) { input_diag_tab=(input_diag_tab+2)%3; redraw=1; }
        } else if (page == 2) {
            if (pressed & PAD_RIGHT) { layout_index = (layout_index + 1) % LAYOUT_COUNT; redraw = 1; }
            if (pressed & PAD_LEFT) { layout_index = (layout_index + LAYOUT_COUNT - 1) % LAYOUT_COUNT; redraw = 1; }
        }
        (void)redraw;
        draw();
        gsKit_queue_exec(gs);
        if (ui_frame_count < 10 || ui_frame_count % 60 == 0) { printf("B81 FLIP BEFORE frame=%u\n",ui_frame_count); fflush(stdout); }
        gsKit_sync_flip(gs);
        if (ui_frame_count < 10 || ui_frame_count % 60 == 0) { printf("B81 FLIP AFTER frame=%u\n",ui_frame_count); fflush(stdout); }
        ui_frame_count++;
        /* Yield the UI thread regardless of its actual kernel priority. */
        RotateThreadReadyQueue(40);
        /* Build 79: explicit sleep lets priority-45 LAN worker run.
           Rotating priority 40 alone cannot schedule priority 45. */
        DelayThread(1000);
    }
    return 0;
}
