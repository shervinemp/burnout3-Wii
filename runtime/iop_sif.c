/*
 * SIFCMD / SIFMAN exports for recompiled IOP modules: command handlers that
 * receive EE packets, IOP -> EE commands and DMA, and RPC servers whose
 * request loop runs on the module's own thread.
 */
#include "iop.h"
#include "kernel_int.h"

#include <string.h>

#define MAX_CMD 32
#define MAX_CALLS 64

typedef struct CmdHandler { uint32_t func, data; } CmdHandler;

static CmdHandler g_usr_cmd[MAX_CMD], g_sys_cmd[MAX_CMD];
static IopServer *g_calls[MAX_CALLS];    /* guest RPC calls waiting for their server thread */
static int g_ncalls;
static int g_dma_id;

void iop_sif_init(void)
{
    memset(g_usr_cmd, 0, sizeof g_usr_cmd);
    memset(g_sys_cmd, 0, sizeof g_sys_cmd);
    g_ncalls = 0;
}

static CmdHandler *cmd_slot(uint32_t cid)
{
    if (cid & 0x80000000u)
        return (cid & 0x7FFFFFFFu) < MAX_CMD ? &g_sys_cmd[cid & 0x7FFFFFFFu] : NULL;
    return cid < MAX_CMD ? &g_usr_cmd[cid] : NULL;
}

int iop_sif_user_cmd(uint32_t cid, uint32_t pkt_addr)
{
    CmdHandler *h = cmd_slot(cid);
    if (!h || !h->func)
        return 0;
    if (g_trace > 1) {
        uint32_t k;
        char line[200];
        int n = snprintf(line, sizeof line, "EE -> IOP command %08X:", cid);
        for (k = 16; k < (iop_r32(pkt_addr) & 0xFF) && k < 64; k += 4)
            n += snprintf(line + n, sizeof line - (size_t)n, " %08X", iop_r32(pkt_addr + k));
        LOG("%s", line);
    }
    iop_call_irq(h->func, pkt_addr, h->data, 0);
    return 1;
}

void iop_hle_sceSifInitCmd(R5900 *c)
{
    iop_ret(c, 0);
}

void iop_hle_sceSifAddCmdHandler(R5900 *c)
{
    uint32_t cid = iop_arg(c, 0);
    CmdHandler *h = cmd_slot(cid);
    if (h) {
        h->func = iop_arg(c, 1);
        h->data = iop_arg(c, 2);
        LOG("IOP SIF command %08X -> %s", cid, iop_func_name(h->func));
    }
    iop_ret(c, 0);
}

void iop_hle_sceSifRemoveCmdHandler(R5900 *c)
{
    CmdHandler *h = cmd_slot(iop_arg(c, 0));
    if (h)
        h->func = 0;
    iop_ret(c, 0);
}

/* sceSifSendCmd(cid, packet, packet_size, src_extra, dest_extra, size_extra) */
void iop_hle_sceSifSendCmd(R5900 *c)
{
    uint32_t cid = iop_arg(c, 0), pkt = iop_arg(c, 1), psize = iop_arg(c, 2);
    uint32_t src = iop_arg(c, 3), dst = iop_arg(c, 4), dsize = iop_arg(c, 5);
    uint32_t w[32];
    int n, k;
    if (psize < 16 || psize > sizeof w) {
        LOG("IOP sceSifSendCmd: bad packet size %u", psize);
        iop_ret(c, 0);
        return;
    }
    iop_w32(pkt, (psize & 0xFF) | (dsize << 8));
    iop_w32(pkt + 4, dst);
    iop_w32(pkt + 8, cid);
    n = (int)(psize / 4);
    for (k = 0; k < n; k++)
        w[k] = iop_r32(pkt + 4 * (uint32_t)k);
    sif_queue_packet(w, n, src, dst, dsize);
    if (g_trace > 1) {
        char line[200];
        int m = snprintf(line, sizeof line, "IOP -> EE command %08X (+%u data %08X->%08X):", cid, dsize, src, dst);
        for (k = 4; k < n && k < 12; k++)
            m += snprintf(line + m, sizeof line - (size_t)m, " %08X", w[k]);
        LOG("%s", line);
    }
    iop_ret(c, ++g_dma_id & 0x7FFFFFFF);
}

/* sceSifRegisterRpc(sd, sid, func, buff, cfunc, cbuff, qd) */
void iop_hle_sceSifRegisterRpc(R5900 *c)
{
    uint32_t sd = iop_arg(c, 0), sid = iop_arg(c, 1);
    sif_register_guest_server(sid, sd, iop_arg(c, 2), iop_arg(c, 3), iop_arg(c, 4), iop_arg(c, 5), iop_arg(c, 6));
    iop_w32(sd + 0, sid);
    iop_ret(c, 0);
}

/* sceSifSetRpcQueue(qd, thread_id) */
void iop_hle_sceSifSetRpcQueue(R5900 *c)
{
    uint32_t qd = iop_arg(c, 0);
    iop_w32(qd + 0, iop_arg(c, 1));
    iop_ret(c, 0);
}

void iop_sif_queue_call(IopServer *s)
{
    Thread *t;
    if (g_ncalls == MAX_CALLS)
        FATAL("too many queued IOP RPC calls");
    g_calls[g_ncalls++] = s;
    t = kern_first_waiter(WAIT_IOP_RPC, (int)s->qd);
    if (t)
        kern_wake(t);
}

static IopServer *take_call(uint32_t qd)
{
    int i;
    for (i = 0; i < g_ncalls; i++) {
        if (g_calls[i]->qd == qd) {
            IopServer *s = g_calls[i];
            memmove(&g_calls[i], &g_calls[i + 1], (size_t)(g_ncalls - i - 1) * sizeof g_calls[0]);
            g_ncalls--;
            return s;
        }
    }
    return NULL;
}

/* sceSifRpcLoop(qd): serve calls on this thread forever */
void iop_hle_sceSifRpcLoop(R5900 *c)
{
    uint32_t qd = iop_arg(c, 0);
    for (;;) {
        IopServer *s = take_call(qd);
        uint32_t ret, fno, size, receive, recv_size;
        uint32_t req[16];
        if (!s) {
            kern_wait_current(WAIT_IOP_RPC, (int)qd);
            continue;
        }
        memcpy(req, s->req, sizeof req);
        fno = req[8];
        size = req[9];
        receive = req[10];
        recv_size = req[11];
        if (g_trace > 1) {
            char text[65];
            uint32_t k;
            for (k = 0; k < 64 && k < size; k++) {
                uint8_t ch = iop_r8(s->buff + k);
                text[k] = ch >= 32 && ch < 127 ? (char)ch : '.';
            }
            text[k] = 0;
            LOG("IOP rpc %08X fn %u request: %08X %08X %08X %08X \"%s\"", s->sid, fno, iop_r32(s->buff),
                iop_r32(s->buff + 4), iop_r32(s->buff + 8), iop_r32(s->buff + 12), text);
        }
        ret = iop_call_guest(c, s->func, fno, s->buff, size, 0);
        if (g_trace > 1)
            LOG("IOP rpc %08X fn %u -> %08X (%u bytes back: %08X %08X %08X %08X)", s->sid, fno, ret, recv_size,
                ret ? iop_r32(ret) : 0, ret ? iop_r32(ret + 4) : 0, ret ? iop_r32(ret + 8) : 0,
                ret ? iop_r32(ret + 12) : 0);
        s->busy = 0;
        sif_rpc_end(req, 0x8000000Au, s->addr, s->buff, s->cbuff, ret, receive, receive && ret ? recv_size : 0);
    }
}

/* sceSifSetDma(transfers, count), IOP -> EE */
static void do_dma(uint32_t t, uint32_t count)
{
    uint32_t k;
    for (k = 0; k < count; k++, t += 16) {
        uint32_t src = iop_r32(t), dst = iop_r32(t + 4), size = iop_r32(t + 8);
        iop_to_ee(dst, src, (size + 3) & ~3u);
    }
}

void iop_hle_sceSifSetDma(R5900 *c)
{
    do_dma(iop_arg(c, 0), iop_arg(c, 1));
    iop_ret(c, ++g_dma_id & 0x7FFFFFFF);
}

void iop_hle_sceSifDmaStat(R5900 *c)
{
    iop_ret(c, -1);            /* transfers complete immediately */
}

/* sceSifSetDmaIntr(transfers, count, callback, data): callback(data) runs at completion */
void iop_hle_sceSifSetDmaIntr(R5900 *c)
{
    uint32_t func = iop_arg(c, 2), data = iop_arg(c, 3);
    do_dma(iop_arg(c, 0), iop_arg(c, 1));
    iop_ret(c, ++g_dma_id & 0x7FFFFFFF);
    if (func) {
        uint32_t v0 = c->r[2][0];
        iop_call_irq(func, data, 0, 0);
        c->r[2][0] = v0;
    }
}

void iop_hle_sceSifCheckInit(R5900 *c)
{
    iop_ret(c, 1);
}
