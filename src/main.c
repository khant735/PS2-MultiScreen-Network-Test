#include <tamtypes.h>
#include <debug.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <libpad.h>
#include <stdio.h>
#include <string.h>

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
static unsigned int frames = 0;

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

static void draw(void) {
    const Layout *l = &layouts[layout_index];
    init_scr();
    scr_printf("PS2 MULTI-SCREEN NETWORK TEST - DEVELOPMENT\n");
    scr_printf("==========================================\n");
    scr_printf("Controller: %s  |  Uptime: %lu frames\n", pad_ready ? "initialized" : "unavailable", (unsigned long)frames);
    scr_printf("D-PAD: select/change  X: open  TRIANGLE: back\n\n");
    if (page == 0) {
        scr_printf("%c Network adapter status\n", selected == 0 ? '>' : ' ');
        scr_printf("%c Screen layout selector\n", selected == 1 ? '>' : ' ');
        scr_printf("%c Diagnostic counters\n", selected == 2 ? '>' : ' ');
        scr_printf("%c About / test status\n", selected == 3 ? '>' : ' ');
    } else if (page == 1) {
        scr_printf("NETWORK ADAPTER\n");
        scr_printf("LAN: not probed; link state unknown\n");
        scr_printf("i.LINK: not probed; link state unknown\n");
        scr_printf("No network modules or transport initialized.\n");
    } else if (page == 2) {
        scr_printf("SCREEN LAYOUT %d / %d\n", layout_index + 1, LAYOUT_COUNT);
        scr_printf("Grid: %d columns x %d rows (%d screens)\n", l->columns, l->rows, l->columns * l->rows);
        scr_printf("LEFT/RIGHT: change layout\n");
        scr_printf("Screen assignment: not active\n");
    } else if (page == 3) {
        scr_printf("DIAGNOSTIC COUNTERS\n");
        scr_printf("TX bytes: 0 (transport inactive)\n");
        scr_printf("RX bytes: 0 (transport inactive)\n");
        scr_printf("Peers: 0 (discovery inactive)\n");
        scr_printf("Synchronization: not started\n");
    } else {
        scr_printf("ABOUT / STATUS\n");
        scr_printf("Functional controller/menu diagnostic build.\n");
        scr_printf("Network discovery, packet I/O and sync pending.\n");
        scr_printf("No GT game code included.\n");
    }
    scr_printf("\nThis is not a validated network test.\n");
}

int main(int argc, char **argv) {
    unsigned short pressed;
    unsigned int redraw = 0;
    (void)argc; (void)argv;
    build_layouts();
    pad_setup();
    draw();
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
        ++frames;
        if (redraw || (frames % 1800000u) == 0) { draw(); redraw = 0; }
        DelayThread(16000);
    }
    return 0;
}
