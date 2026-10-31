/*
 * Native IOP RPC servers: the BIOS services the game binds to (LOADFILE) and
 * stand-ins for modules that are not recompiled.
 */
#include "iop.h"

#include <string.h>

static int g_next_module = 1;
static int g_starting;               /* recompiled modules whose start function is running */
static IopServer *g_loadfile;

int iop_modules_starting(void)
{
    return g_starting;
}

typedef struct LoadReq {
    uint32_t data;
    int id;
} LoadReq;

static void module_done(void *user, int32_t result)
{
    LoadReq *r = user;
    g_starting--;
    iop_w32(r->data, (uint32_t)r->id);
    iop_w32(r->data + 4, (uint32_t)result);
    free(r);
    sif_rpc_complete(g_loadfile, 0);
}

/* LOADFILE (0x80000006). fn 0/1 load a module from a file: request {arg_len, 0,
 * path[252], args[252]}, reply {module id, start result}. Modules that are
 * recompiled run their start function on an IOP thread and the reply waits
 * for it; the others succeed at once (their services are emulated natively). */
static int loadfile_server(uint32_t fn, uint32_t d, int size, void *user)
{
    (void)user;
    if (fn == 0 || fn == 1) {
        char path[256], args[256];
        uint32_t arg_len = iop_r32(d);
        const IopModuleImage *m;
        iop_read_str(path, d + 8, 252);
        iop_read_bytes(args, d + 8 + 252, 252);
        args[252] = 0;
        if (arg_len > 252)
            arg_len = 252;
        m = iop_find_module(path);
        LOG("IOP load module %s (args %u bytes)%s", path, arg_len, m ? ": recompiled" : "");
        if (m && iop_module_loaded(m)) {
            /* a second copy would find its libraries registered and not stay resident */
            LOG("IOP module %s is already resident", path);
            iop_w32(d, (uint32_t)g_next_module++);
            iop_w32(d + 4, 1);          /* NO_RESIDENT_END */
            return 0;
        }
        if (m) {
            LoadReq *r = calloc(1, sizeof *r);
            r->data = d;
            r->id = g_next_module++;
            g_starting++;
            iop_load_module(m, args, arg_len, module_done, r);
            return IOP_RPC_DEFER;
        }
        iop_w32(d, (uint32_t)g_next_module++);
        iop_w32(d + 4, 0);
        return 0;
    }
    if (fn == 255) {
        /* version query: the EE library requires the IOPRP280 LOADFILE */
        iop_write_bytes(d, "2800", 4);
        return 0;
    }
    LOG("LOADFILE fn %u size %d", fn, size);
    iop_w32(d, 0);
    iop_w32(d + 4, 0);
    return 0;
}

/* Logitech device library (wheels): report the IRX version the EE library
 * wants and no devices. */
static int lgdev_server(uint32_t fn, uint32_t d, int size, void *user)
{
    (void)user;
    iop_memset(d, 0, (uint32_t)size);
    if (fn == 12) {
        iop_w32(d + 4, 0x010B1B00u);     /* 1.11.027 */
        return 0;
    }
    if (g_trace)
        LOG("lgdev fn %u size %d", fn, size);
    iop_w32(d, 0x80000001u);             /* error: no such device */
    return 0;
}

/* IOP heap (0x80000003): the EE's sceSifAllocIopHeap & co. Request words are
 * the arguments, the reply is one word. */
static int iopheap_server(uint32_t fn, uint32_t d, int size, void *user)
{
    uint32_t r = 0;
    (void)user;
    (void)size;
    switch (fn) {
    case 1:     /* AllocIopHeap(size) */
        r = iop_heap_alloc(iop_r32(d));
        break;
    case 2:     /* FreeIopHeap(addr) */
    case 5:     /* FreeSysMemory(addr) */
        iop_heap_free(iop_r32(d));
        break;
    case 4:     /* AllocSysMemory(size, mode, addr) */
        r = iop_heap_alloc_mode((int)iop_r32(d + 4), iop_r32(d), iop_r32(d + 8));
        break;
    default:
        LOG("iopheap fn %u not implemented", fn);
        break;
    }
    if (g_trace)
        LOG("iopheap fn %u (%08X %08X %08X) = %08X", fn, iop_r32(d), iop_r32(d + 4), iop_r32(d + 8), r);
    if ((fn == 1 || fn == 4) && !r)
        LOG("iopheap: out of IOP memory for %X bytes", iop_r32(d));
    iop_w32(d, r);
    return 0;
}

void iop_services_init(void)
{
    iop_register_server(0x80000003u, "iopheap", iopheap_server, NULL, 0x100);
    g_loadfile = iop_register_server(0x80000006u, "loadfile", loadfile_server, NULL, 0x400);
    iop_register_server(0x046D046Du, "lgdev", lgdev_server, NULL, 0x1000);
}
