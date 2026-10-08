#include <tamtypes.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static unsigned short read_pressed(void) {
    int state = padGetState(PAD_PORT, PAD_SLOT);
    unsigned short current = 0xffff;
    unsigned short pressed;
    if (!pad_ready || (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1)) return 0;
    if (padRead(PAD_PORT, PAD_SLOT, &buttons) == 0) return 0;
    current = buttons.btns;
    pressed = (unsigned short)(previous_buttons & (unsigned short)~current);
    previous_buttons = current;
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
        if (*str=='\\n') {x=origin;y+=9*scale;continue;}
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
static void draw_layout(float x,float y,float w,float h) {
    Layout l=layouts[layout_index];
    float gap=2.0f, cellw=(w-(l.columns-1)*gap)/l.columns;
    float cellh=(h-(l.rows-1)*gap)/l.rows;
    int r,c,n=0;
    if(cellw>38)cellw=38;
    if(cellh>55)cellh=55;
    for(r=0;r<l.rows;r++)for(c=0;c<l.columns;c++){
        float px=x+c*(cellw+gap),py=y+r*(cellh+gap);
        u64 col=(++n==1)?COLOR(30,175,210):COLOR(50,85,125);
        rect(px,py,cellw,cellh,col);
        if(cellw>=18&&cellh>=15){
            char number[8];sprintf(number,"%d",n);
            label(px+3,py+3,1.4f,COLOR(255,255,255),number);
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
        label(34*sx,166*sy,1.8f*sx,muted,"LAN: NOT PROBED");
        label(34*sx,200*sy,1.8f*sx,muted,"I.LINK: NOT PROBED");
        label(34*sx,244*sy,1.6f*sx,muted,"NO TRANSPORT INITIALIZED");
    } else if(page==2) {
        sprintf(line,"LAYOUT %d / %d",layout_index+1,LAYOUT_COUNT);
        label(34*sx,105*sy,2.0f*sx,white,line);
        sprintf(line,"%d X %d  -  %d SCREENS",l.columns,l.rows,l.columns*l.rows);
        label(34*sx,145*sy,1.8f*sx,muted,line);
        draw_layout(36*sx,190*sy,560*sx,150*sy);
        label(34*sx,375*sy,1.6f*sx,muted,"LEFT / RIGHT CHANGE LAYOUT");
    } else if(page==3) {
        label(34*sx,112*sy,2.2f*sx,white,"DIAGNOSTIC COUNTERS");
        label(34*sx,166*sy,1.7f*sx,muted,"TX / RX: INACTIVE");
        label(34*sx,200*sy,1.7f*sx,muted,"PEERS: NOT DISCOVERED");
        label(34*sx,234*sy,1.7f*sx,muted,"SYNC: NOT STARTED");
    } else {
        label(34*sx,112*sy,2.2f*sx,white,"ABOUT / STATUS");
        label(34*sx,166*sy,1.6f*sx,muted,"CONTROLLER MENU: ACTIVE");
        label(34*sx,200*sy,1.6f*sx,muted,"NETWORK I/O: NOT IMPLEMENTED");
    }
    rect(20*sx,405*sy,600*sx,1*sy,COLOR(65,90,115));
    label(34*sx,418*sy,1.35f*sx,muted,"NOT A VALIDATED NETWORK TEST");
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
        pressed = read_pressed();
        if (pressed & PAD_TRIANGLE) { page = 0; redraw = 1; }
        if (page == 0) {
            if (pressed & PAD_DOWN) { selected = (selected + 1) % 4; redraw = 1; }
            if (pressed & PAD_UP) { selected = (selected + 3) % 4; redraw = 1; }
            if (pressed & PAD_CROSS) { page = selected + 1; redraw = 1; }
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
