/*
 * SIF: the EE <-> IOP link, emulated from the IOP side.
 *
 * The game's own libkernl SIF command/RPC code runs unchanged. It sends
 * packets to the IOP with the sceSifSetDma syscall; we receive them in IOP
 * RAM and answer the way the IOP's SIFCMD module would: DMA the reply data
 * into EE memory, write the reply packet into the EE's command buffer and
 * raise the SIF0 DMAC interrupt, whose handler (game code) completes the call.
 *
 * RPC servers are either native (iop_register_server) or registered by a
 * recompiled IOP module (iop_sif.c), whose server thread runs the call.
 *
 * Packet layouts follow the SIFCMD/SIFRPC protocol (see ps2sdk sifcmd.h,
 * sifrpc.h). No ps2sdk source is included. See CREDITS.md.
 */
#include "iop.h"

#include <string.h>

#define SIF_REG_MAINADDR 1
#define SIF_REG_SUBADDR 2
#define SIF_REG_MSFLAG 3
#define SIF_REG_SMFLAG 4
#define SIF_STAT_SIFINIT 0x10000u
#define SIF_STAT_CMDINIT 0x20000u
#define SIF_STAT_BOOTEND 0x40000u

#define CMD_CHANGE_SADDR 0x80000000u
#define CMD_SET_SREG 0x80000001u
#define CMD_INIT_CMD 0x80000002u
#define CMD_RESET 0x80000003u
#define CMD_RPC_END 0x80000008u
#define CMD_RPC_BIND 0x80000009u
#define CMD_RPC_CALL 0x8000000Au
#define CMD_RPC_RDATA 0x8000000Cu

#define IOP_CMD_BUF 0x0001F000u      /* where the EE sends packets */
#define MAX_SERVERS 64
#define MAX_QUEUE 128

uint8_t *iop_ram;

static uint32_t g_regs[8];
static uint32_t g_sregs[32];          /* software registers 0x80000000+ */
#define SIF_SYSREG_RPCINIT 0x80000002u
static uint32_t g_ee_pktbuf;           /* EE address packets are delivered to */
static IopServer g_servers[MAX_SERVERS];
static int g_nservers;
static int g_dma_id;

typedef struct Pending {
    uint32_t pkt[32];                  /* packet words */
    int pkt_words;
    uint32_t data_src;                 /* IOP address of data to DMA first */
    uint32_t data_dst;                 /* EE destination */
    uint32_t data_size;
} Pending;
static Pending g_queue[MAX_QUEUE];
static int g_qhead, g_qtail;

/* ---- IOP memory owned by native servers ------------------------------------------------ */

uint32_t iop_alloc(uint32_t size)
{
    uint32_t a = iop_heap_alloc(size);
    if (!a)
        FATAL("IOP heap exhausted");
    return a;
}

static IopServer *new_server(uint32_t sid, const char *name)
{
    IopServer *s;
    if (g_nservers >= MAX_SERVERS)
        FATAL("too many IOP servers");
    s = &g_servers[g_nservers++];
    memset(s, 0, sizeof *s);
    s->sid = sid;
    s->name = name;
    return s;
}

IopServer *iop_register_server(uint32_t sid, const char *name, IopRpcFn fn, void *user, uint32_t buff_size)
{
    IopServer *s = new_server(sid, name);
    s->fn = fn;
    s->user = user;
    s->buff_size = buff_size ? buff_size : 0x10000u;
    s->buff = iop_alloc(s->buff_size);
    s->addr = iop_alloc(64);
    return s;
}

IopServer *sif_register_guest_server(uint32_t sid, uint32_t sd, uint32_t func, uint32_t buff, uint32_t cfunc,
                                     uint32_t cbuff, uint32_t qd)
{
    IopServer *s = NULL;
    int i;
    for (i = 0; i < g_nservers; i++)
        if (g_servers[i].sid == sid)
            s = &g_servers[i];             /* a module replacing a fallback server */
    if (!s)
        s = new_server(sid, "module");
    s->fn = NULL;
    s->guest = 1;
    s->addr = sd;
    s->buff = buff;
    s->func = func;
    s->cfunc = cfunc;
    s->cbuff = cbuff;
    s->qd = qd;
    LOG("IOP module registered RPC server %08X (func %s, buffer %08X)", sid, iop_func_name(func), buff);
    return s;
}

static IopServer *find_server(uint32_t sid)
{
    int i;
    for (i = 0; i < g_nservers; i++)
        if (g_servers[i].sid == sid)
            return &g_servers[i];
    return NULL;
}

static IopServer *server_by_addr(uint32_t addr)
{
    int i;
    for (i = 0; i < g_nservers; i++)
        if (g_servers[i].addr == addr)
            return &g_servers[i];
    return NULL;
}

/* ---- registers ---------------------------------------------------------------------- */

uint32_t sif_get_reg(uint32_t reg)
{
    if (reg == SIF_REG_SMFLAG)
        return SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND;
    if (reg == SIF_REG_SUBADDR)
        return IOP_CMD_BUF;
    if (reg < 8)
        return g_regs[reg];
    if (reg == SIF_SYSREG_RPCINIT)
        return 1;                     /* the IOP's RPC layer is always up */
    if (reg >= 0x80000000u && reg < 0x80000020u)
        return g_sregs[reg & 31];
    LOG("sceSifGetReg(%08X)", reg);
    return 0;
}

void sif_set_reg(uint32_t reg, uint32_t val)
{
    if (g_trace)
        LOG("sceSifSetReg(%08X, %08X)", reg, val);
    if (reg < 8)
        g_regs[reg] = val;
    else if (reg >= 0x80000000u && reg < 0x80000020u)
        g_sregs[reg & 31] = val;
}

/* ---- EE-bound packets ----------------------------------------------------------------- */

void sif_queue_packet(const uint32_t *words, int n, uint32_t data_src, uint32_t data_dst, uint32_t data_size)
{
    Pending *p = &g_queue[g_qtail];
    int next = (g_qtail + 1) % MAX_QUEUE;
    if (next == g_qhead)
        FATAL("SIF queue overflow");
    if (n > 32)
        FATAL("SIF packet too large (%d words)", n);
    memcpy(p->pkt, words, (size_t)n * 4);
    p->pkt_words = n;
    p->data_src = data_src;
    p->data_dst = data_dst;
    p->data_size = data_size;
    g_qtail = next;
}

/* a packet for the EE is waiting and its interrupt can be raised: the idle
 * loops must not sleep to the next timer event first (kernel.c). They did:
 * the reply to every RPC call (the game makes three a frame, more when
 * sounds start) reached the EE at the next vblank or timer tick, up to 16 ms
 * after the IOP had served the call in microseconds, with the game's main
 * thread asleep on the call's semaphore meanwhile. */
int g_sif_wait = 0;             /* debugging (autopilot "sifwait 1"): as it was */
uint32_t g_sif_last_call[2];    /* debugging: the last RPC call taken, server and function */

int sif_pending(void)
{
    return !g_sif_wait && g_qhead != g_qtail && g_ee_pktbuf && !kernel_dmac_is_pending(DMAC_SIF0);
}

void sif_poll(void)
{
    Pending *p;
    int k;
    if (g_qhead == g_qtail || !g_ee_pktbuf)
        return;
    /* one packet at a time: the previous one is consumed when its interrupt is delivered */
    if (kernel_dmac_is_pending(DMAC_SIF0))
        return;
    p = &g_queue[g_qhead];
    g_qhead = (g_qhead + 1) % MAX_QUEUE;
    if (p->data_size && p->data_dst)
        iop_to_ee(p->data_dst, p->data_src, p->data_size);
    for (k = 1; k < p->pkt_words; k++)
        g_w32(g_ee_pktbuf + 4 * k, p->pkt[k]);
    g_w32(g_ee_pktbuf, p->pkt[0]);
    if (g_trace > 1)
        LOG("SIF -> EE: cid %08X (%d words, %u data bytes)", p->pkt[2], p->pkt_words, p->data_size);
    kernel_raise_dmac(DMAC_SIF0);
}

void sif_rpc_end(const uint32_t *req, uint32_t cid, uint32_t server, uint32_t buff, uint32_t cbuff,
                 uint32_t data_src, uint32_t data_dst, uint32_t data_size)
{
    uint32_t w[12];
    w[0] = 12 * 4;                 /* psize, dsize 0 */
    w[1] = 0;
    w[2] = CMD_RPC_END;
    w[3] = 0;
    w[4] = req[4];                 /* rec_id */
    w[5] = req[5];                 /* pkt_addr */
    w[6] = req[6];                 /* rpc_id */
    w[7] = req[7];                 /* client */
    w[8] = cid;
    w[9] = server;
    w[10] = buff;
    w[11] = cbuff;
    sif_queue_packet(w, 12, data_src, data_dst, data_size);
}

/* Finish a call whose native server returned IOP_RPC_DEFER. */
void sif_rpc_complete(IopServer *s, int out)
{
    const uint32_t *req = s->req;
    uint32_t receive = req[10], recv_size = req[11];
    if (!s->busy)
        FATAL("RPC completion for idle server %08X", s->sid);
    s->busy = 0;
    if (out < 0)
        out = 0;
    sif_rpc_end(req, CMD_RPC_CALL, s->addr, s->buff, s->buff, s->buff + (uint32_t)out, receive,
                receive ? recv_size : 0);
}

/* ---- IOP-side command processing -------------------------------------------------- */

static int default_server(uint32_t rpc_number, uint32_t data, int size, void *user)
{
    IopServer *s = user;
    LOG("RPC to unimplemented server %08X fn %u size %d", s->sid, rpc_number, size);
    iop_memset(data, 0, 64);
    return 0;
}

static void handle_packet(uint32_t addr)
{
    uint32_t req[16];
    uint32_t cid;
    int k;
    for (k = 0; k < 16; k++)
        req[k] = iop_r32(addr + 4 * k);
    cid = req[2];
    switch (cid) {
    case CMD_INIT_CMD:
        if (req[3] == 0) {
            g_ee_pktbuf = req[4];
            LOG("SIF init: EE packet buffer %08X", g_ee_pktbuf);
        } else {
            LOG("SIF init cmd opt %u (%08X)", req[3], req[4]);
        }
        break;
    case CMD_RESET:
        LOG("IOP reset requested (%u bytes of argument)", req[4]);
        break;
    case CMD_SET_SREG:
        if (req[4] < 32)
            g_sregs[req[4]] = req[5];
        break;
    case CMD_CHANGE_SADDR:
        break;
    case CMD_RPC_BIND: {
        uint32_t sid = req[8];
        IopServer *s = find_server(sid);
        if (!s && iop_modules_starting()) {
            /* a module that may provide it is still starting: the EE library retries */
            if (g_trace)
                LOG("bind %08X: not registered yet", sid);
            sif_rpc_end(req, CMD_RPC_BIND, 0, 0, 0, 0, 0, 0);
            break;
        }
        if (!s) {
            char name[32];
            snprintf(name, sizeof name, "unknown-%08X", sid);
            iop_register_server(sid, strdup(name), default_server, NULL, 0x1000);
            s = find_server(sid);
            s->user = s;
            LOG("bind to unknown IOP server %08X", sid);
            rt_dump_stack(kernel_current_ctx(), "binder");
        } else if (g_trace) {
            LOG("bind %08X (%s)", sid, s->name);
        }
        sif_rpc_end(req, CMD_RPC_BIND, s->addr, s->buff, s->guest ? s->cbuff : s->buff, 0, 0, 0);
        break;
    }
    case CMD_RPC_CALL: {
        uint32_t rpc_number = req[8], send_size = req[9], receive = req[10], recv_size = req[11];
        uint32_t server = req[13];
        IopServer *s = server_by_addr(server);
        int out;
        if (!s) {
            LOG("RPC call to unknown server address %08X", server);
            sif_rpc_end(req, CMD_RPC_CALL, 0, 0, 0, 0, 0, 0);
            break;
        }
        if (g_trace > 1)
            LOG("rpc %08X (%s) fn %u send %u recv %u -> %08X", s->sid, s->name, rpc_number, send_size,
                recv_size, receive);
        if (s->busy)
            LOG("RPC %08X: call while the previous one is still running", s->sid);
        memcpy(s->req, req, sizeof req);
        s->busy = 1;
        g_sif_last_call[0] = s->sid;
        g_sif_last_call[1] = rpc_number;
        if (s->guest) {
            iop_sif_queue_call(s);
            break;
        }
        out = s->fn(rpc_number, s->buff, (int)send_size, s->user);
        if (out != IOP_RPC_DEFER)
            sif_rpc_complete(s, out);
        break;
    }
    case CMD_RPC_RDATA: {
        uint32_t src = req[8], dst = req[9], size = req[10];
        sif_rpc_end(req, CMD_RPC_RDATA, 0, 0, 0, src, dst, size);
        break;
    }
    default:
        if (!iop_sif_user_cmd(cid, addr))
            LOG("unhandled SIF command %08X", cid);
        break;
    }
}

/* sceSifSetDma(transfers, count): each transfer is {src, dest, size, attr} */
int sif_set_dma(uint32_t transfers, uint32_t count)
{
    uint32_t k;
    for (k = 0; k < count; k++) {
        uint32_t t = transfers + 16 * k;
        uint32_t src = g_r32(t), dst = g_r32(t + 4) & IOP_MASK, size = g_r32(t + 8);
        if (dst + size > IOP_RAM_SIZE)
            FATAL("SIF DMA past IOP RAM: %08X+%X", dst, size);
        ee_to_iop(dst, src, size);
        if (dst == IOP_CMD_BUF)
            handle_packet(dst);
    }
    return ++g_dma_id & 0x7FFFFFFF;
}

int sif_dma_stat(int id)
{
    (void)id;
    return -1;      /* complete */
}

void sif_init(void)
{
    iop_ram = plat_alloc(IOP_RAM_SIZE);
    memset(iop_ram, 0, IOP_RAM_SIZE);
    iop_kernel_init();
    iop_sif_init();
    iop_cdvd_init();
    iop_sd_init();
    iop_sio2_init();
    iop_services_init();
}
