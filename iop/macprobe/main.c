/* Read-only DEV9 EEPROM MAC probe. Adapted from the access method used by
 * F0bes/biosdrain and ps2homebrew/PS2Ident; no EEPROM/NVRAM writes.
 * RPC output: signed result in bytes 0..3, six MAC bytes in bytes 4..9.
 */
#include <types.h>
#include <irx.h>
#include <loadcore.h>
#include <sifcmd.h>
#include <thbase.h>
#include <sysclib.h>
#include <dev9.h>

IRX_ID("ps2ms_macprobe", 1, 0);

#define MACPROBE_RPC_ID 0x4D534D41
static SifRpcDataQueue_t rpc_queue;
static SifRpcServerData_t rpc_server;
static unsigned char rpc_buffer[64] __attribute__((aligned(16)));

static iop_library_t *find_dev9(void)
{
    lc_internals_t *internal = GetLoadcoreInternalData();
    iop_library_t *p;
    if (!internal) return NULL;
    for (p = internal->let_next; p; p = p->prev)
        if (memcmp(p->name, "dev9", 4) == 0) return p;
    return NULL;
}

static void *rpc_dispatch(unsigned int fno, void *data, int size)
{
    int result = -1;
    unsigned short words[4] = {0, 0, 0, 0};
    iop_library_t *lib;
    int (*read_eeprom)(unsigned short *);
    (void)size;
    if (fno == 1 && (lib = find_dev9()) != NULL) {
        /* Export 9 is SpdGetEthernetID / dev9GetEEPROM in PS2SDK. */
        read_eeprom = (int (*)(unsigned short *))lib->fptrs[9];
        if (read_eeprom) result = read_eeprom(words);
    }
    memcpy(data, &result, sizeof(result));
    if (result == 0) memcpy((unsigned char *)data + 4, words, 6);
    else {
        unsigned char zeros[6] = {0,0,0,0,0,0};
        memcpy((unsigned char *)data + 4, zeros, 6);
    }
    return data;
}

static void rpc_thread(void *arg)
{
    (void)arg;
    sceSifSetRpcQueue(&rpc_queue, GetThreadId());
    sceSifRegisterRpc(&rpc_server, MACPROBE_RPC_ID, rpc_dispatch,
                      rpc_buffer, NULL, NULL, &rpc_queue);
    sceSifRpcLoop(&rpc_queue);
}

int _start(int argc, char **argv)
{
    iop_thread_t th;
    int id;
    (void)argc; (void)argv;
    th.attr = TH_C;
    th.thread = rpc_thread;
    th.priority = 40;
    th.stacksize = 0x1000;
    th.option = 0;
    id = CreateThread(&th);
    if (id <= 0) return 1;
    return StartThread(id, NULL) < 0 ? 1 : 0;
}
