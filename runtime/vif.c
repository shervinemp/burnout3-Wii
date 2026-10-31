/*
 * DMA source chains and the VIF0/VIF1 command processors.
 *
 * DMA runs to completion when started: the chain is walked and each qword is
 * handed to the channel's consumer (VIF, GIF). VIF commands write VU memory
 * (UNPACK/MPG), forward data to the GIF (DIRECT, PATH2), and start VU
 * microprograms (MSCAL), which run to completion and XGKICK to the GIF (PATH1).
 */
#include "hw.h"
#include "gs.h"
#include "vu.h"
#include "nw_car.h"

#include <string.h>

/* ---- DMA chain walking ------------------------------------------------------------ */

typedef void (*QwordSink)(void *ctx, const uint32_t *qw, uint32_t n);

/* guest qword pointer: RAM or scratchpad (bit 31 of a tag address) */
static void read_qwords(uint32_t addr, int spr, uint32_t *dst, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n * 4; k++)
        dst[k] = spr ? LW((addr + 4 * k) & 0x3FFCu) : LW(addr + 4 * k);
}

#define CHUNK 256
extern int g_opt3;                      /* hle_render.c (autopilot "opt3 0": the old ways, 2026-10-16) */
extern int g_wstat;
/* "wstat 9" (vif_cmd_report): tags whose two VIF words were NOPs taken at once (vif_tag_sink), transfers of no
 * qwords not handed to the sink (about 550 and 1900 a race frame of 3200 tags) */
static uint32_t g_dma_tag_nops, g_dma_empty;

static void transfer(uint32_t addr, int spr, uint32_t qwc, QwordSink sink, void *ctx)
{
    uint32_t buf[CHUNK * 4];
    if (!qwc && g_opt3) {
        /* (the VIF's sinks do nothing with no words: a tag that carries VIF codes alone, CNT 0) */
        if (PS2_UNLIKELY(g_wstat == 9))
            g_dma_empty++;
        return;
    }
#if !defined(PS2_DYNAMIC_IO)
    /* guest RAM holds host-order words: hand the sink the memory itself */
    if (!spr && (addr >> 28) != 1 && (addr & PS2_RAM_MASK) + qwc * 16 <= PS2_RAM_MASK + 1) {
        sink(ctx, (const uint32_t *)GPTR(addr), qwc);
        return;
    }
#endif
    while (qwc) {
        uint32_t n = qwc > CHUNK ? CHUNK : qwc;
        read_qwords(addr, spr, buf, n);
        sink(ctx, buf, n);
        addr += n * 16;
        qwc -= n;
    }
}

/* debugging: target of the last DMA call tag; VU microcode is uploaded from
 * chains the game calls, so this names the VU1 code that is running */
static uint32_t g_dma_call;
uint32_t g_dma_where;           /* status line: the DMA tag being transferred (id | call depth << 4) */
/* debugging: the last VIF1 commands, with where the DMA chain was */
static uint32_t g_vif1_ring[16][2];
static unsigned g_vif1_ring_n;
uint32_t g_vu1_code;

static int vif_defer_call(void *ctx, uint32_t addr, int tte);

/* Walk a channel transfer. tte: send the upper 64 bits of each tag to the sink (VIF). */
static void dma_run(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, int tte_capable,
                    QwordSink sink, QwordSink tag_sink, void *ctx)
{
    uint32_t mode = (chcr >> 2) & 3;
    uint32_t asr[2];
    int sp = 0, guard = 0;
    if (mode == 0 || mode == 2) {         /* normal / interleave */
        transfer(madr, (madr >> 31) & 1, qwc, sink, ctx);
        return;
    }
    if (qwc)                               /* a pending normal part before the chain resumes */
        transfer(madr, (madr >> 31) & 1, qwc, sink, ctx);
    for (;;) {
        uint32_t tag[4], id, n, addr;
        int tag_spr = (tadr >> 31) & 1;
        read_qwords(tadr & 0x7FFFFFF0u, tag_spr, tag, 1);
        n = tag[0] & 0xFFFF;
        id = (tag[0] >> 28) & 7;
        addr = tag[1] & 0x7FFFFFF0u;
        if (tte_capable && (chcr & 0x40))
            tag_sink(ctx, tag, 1);
        g_dma_where = id | (uint32_t)sp << 4 | (tte_capable ? 0x100 : 0);
        if (++guard > 1000000)
            FATAL("DMA chain does not terminate (tadr %08X)", tadr);
        switch (id) {
        case 0: /* refe */
            transfer(addr, (tag[1] >> 31) & 1, n, sink, ctx);
            return;
        case 1: /* cnt */
            transfer(tadr + 16, tag_spr, n, sink, ctx);
            tadr = tadr + 16 + n * 16;
            break;
        case 2: /* next */
            transfer(tadr + 16, tag_spr, n, sink, ctx);
            tadr = tag[1];
            break;
        case 3: case 4: /* ref, refs */
            transfer(addr, (tag[1] >> 31) & 1, n, sink, ctx);
            tadr += 16;
            break;
        case 5: /* call */
            g_dma_call = tag[1] & 0x7FFFFFF0u;
            transfer(tadr + 16, tag_spr, n, sink, ctx);
            if (tte_capable && vif_defer_call(ctx, g_dma_call, (chcr & 0x40) != 0)) {
                tadr = tadr + 16 + n * 16;      /* render data drawn natively: not called */
                break;
            }
            if (sp >= 2)
                FATAL("DMA call stack overflow");
            asr[sp++] = tadr + 16 + n * 16;
            tadr = tag[1];
            break;
        case 6: /* ret */
            transfer(tadr + 16, tag_spr, n, sink, ctx);
            if (sp == 0)
                return;
            tadr = asr[--sp];
            break;
        case 7: /* end */
            transfer(tadr + 16, tag_spr, n, sink, ctx);
            return;
        }
        if ((tag[0] >> 31) && (chcr & 0x80))   /* IRQ with TIE: stop */
            return;
    }
}

/* ---- VIF --------------------------------------------------------------------------- */

typedef struct Vif {
    int unit;               /* 0 or 1 */
    uint32_t *mem;          /* VU data memory, host-order words */
    uint32_t mem_size;      /* bytes */
    /* registers */
    uint32_t cl, wl, mode, mask, row[4], col[4];
    uint32_t base, ofst, tops, top, itops, itop, dbf;
    uint32_t mark;
    int mskpath3;
    /* command decoding */
    uint32_t code;          /* current VIFcode */
    uint32_t remaining;     /* words left for the current command */
    uint32_t stage;         /* command-specific progress */
    uint32_t addr;          /* write address (bytes) for UNPACK/MPG */
    uint32_t num;
    uint32_t cycle;         /* UNPACK write-cycle counter */
    uint32_t ufill;         /* UNPACK payload words gathered so far (split transfers) */
    uint32_t ubuf[1024];    /* the largest payload: 256 V4-32 vectors */
    uint32_t direct_qw[4];
    int direct_fill;
    uint32_t native[16];    /* payload of a native draw command (VIF_NATIVE_CODE) */
    uint32_t deferred;      /* VIFcodes of called render data not sent yet (native draws), up to: */
    uint32_t deferred_end;
    int dkind;              /* NW_CAR: dbatch, else a world pipeline: dwbatch */
    NcBlock dbatch;         /* the batch they hold */
    NwBlock dwbatch;
} Vif;

static Vif g_vif[2];
int g_gif_m3p;
int g_gif_p3_pending;
static void p3_resume(int stop_at_eop);

static void vif_reset(Vif *v)
{
    int u = v->unit;
    memset(v, 0, sizeof *v);
    v->unit = u;
    v->mem = g_vu_mem + (u ? 0xC000 : 0x4000) / 4;
    v->mem_size = u ? 0x4000 : 0x1000;
    v->cl = v->wl = 1;
}

void vif_init(void)
{
    g_vif[0].unit = 0;
    g_vif[1].unit = 1;
    vif_reset(&g_vif[0]);
    vif_reset(&g_vif[1]);
}

uint32_t vif_top(int unit) { return g_vif[unit].top; }
uint32_t vif_itop(int unit) { return g_vif[unit].itop; }

static void vif_feed_words(Vif *v, const uint32_t *w, uint32_t n);

/* VIFcodes and data from guest memory, as a DMA transfer would send them */
static void vif_feed_range(Vif *v, uint32_t a, uint32_t end)
{
    uint32_t w[256], n, k, code = v->code;
    while (a < end) {
        n = (end - a) / 4 < 256 ? (end - a) / 4 : 256;
        for (k = 0; k < n; k++)
            w[k] = LW(a + 4 * k);
        vif_feed_words(v, w, n);
        a += 4 * n;
    }
    v->code = code;
}

/* VIF registers as they were where a batch of render data starts */
static void vif_batch_regs(Vif *v, uint32_t cl, uint32_t wl, uint32_t mode, const uint32_t *row)
{
    v->cl = cl;
    v->wl = wl;
    v->mode = mode;
    memcpy(v->row, row, sizeof v->row);
}

/* the microprogram start a batch ends with, without running it */
static void vif_batch_done(Vif *v, uint32_t n)
{
    v->itops = n;
    v->top = v->tops;
    v->itop = v->itops;
    v->dbf ^= 1;
    v->tops = v->base + (v->dbf ? v->ofst : 0);
}

/* Send the deferred batch to the VIF after all, as the call would have. */
static void vif_flush_deferred(Vif *v)
{
    uint32_t a = v->deferred;
    v->deferred = 0;
    if (v->dkind == NW_CAR)
        vif_batch_regs(v, v->dbatch.cl, v->dbatch.wl, v->dbatch.mode, v->dbatch.srow);
    else
        vif_batch_regs(v, v->dwbatch.cl, v->dwbatch.wl, v->dwbatch.mode, v->dwbatch.srow);
    vif_feed_range(v, a, v->deferred_end);
}

int g_vif_defer_test;           /* debugging ("defertest"): defer render data without native drawing */
uint32_t g_vif_deferred[4];     /* status line: car batches drawn natively, sent after all; world the same */

/* one batch of a native pipeline, as parsed */
typedef struct VifBatch {
    uint32_t n, start, cl, wl, mode;
    const uint32_t *row;
    int mscnt;
} VifBatch;

static VifBatch vif_batch(int kind, const NcBlock *cb, const NwBlock *wb, int k)
{
    VifBatch r;
    if (kind == NW_CAR) {
        const NcBlock *b = &cb[k];
        r.n = b->n; r.start = b->start; r.cl = b->cl; r.wl = b->wl; r.mode = b->mode; r.row = b->srow;
        r.mscnt = b->mscnt;
    } else {
        const NwBlock *b = &wb[k];
        r.n = b->n; r.start = b->start; r.cl = b->cl; r.wl = b->wl; r.mode = b->mode; r.row = b->srow;
        r.mscnt = b->mscnt;
    }
    return r;
}

/* debugging logs of the native pipelines: from the race on (vblank 13000),
 * or from when the autopilot switches them on ("<t> nlog 1") */
int g_native_log;

static inline int native_log_on(void)
{
    return g_native_log || kernel_vblank_count() > 13000;
}

/* debugging: the VIFcodes of the first world render data packets the parser
 * turns down in a race */
static void __attribute__((noinline)) log_world_reject(int kind, uint32_t addr, uint32_t a, uint32_t end, const Vif *v)
{
    static int logged;
    char line[256];
    size_t m = 0;
    int k;
    if (logged >= 12 || !native_log_on())
        return;
    for (k = 0; k < 4 && a + 4 * k < end; k++)
        if (((LW(a + 4 * k) >> 24) & 0x7F) == 0x4A)
            return;                     /* a microprogram upload */
    logged++;
    for (k = 0; k < 24 && a < end && m < sizeof line - 16; k++) {
        uint32_t code = LW(a), cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF;
        m += (size_t)snprintf(line + m, sizeof line - m, " %08X", code);
        a += 4;
        if (cmd == 0x30 || cmd == 0x31)
            a += 16;
        else if ((cmd & 0x60) == 0x60) {
            uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, bits = (32u >> vl) * (vn + 1);
            if (!num)
                num = 256;
            a += ((bits * (v->wl <= v->cl ? num : num) + 31) / 32) * 4;
        } else if (cmd == 0x4A)
            a += (num ? num : 256) * 8;
    }
    LOG("world render data (kind %d) at %08X not native, cl %u wl %u:%s", kind, addr, v->cl, v->wl, line);
}

/* "wstat 1": the cycles of the world batches' path, in ticks of the time base: [0] a call's parse (the
 * cache) and everything after it, [1] the parse alone, [2] the draws (gs_backend_world), [3] calls of the
 * kind, [4] batches drawn */
int g_wstat;
int g_wskip, g_world_rep;               /* gs.h: autopilot "wskip" (debug) */
extern int g_opt_new;                  /* gs.c (autopilot "newopt 0") */
uint64_t g_wst[6];
static int g_wst_world;
uint64_t plat_ticks(void);

void vif_wstat_report(uint32_t frames)
{
    const uint32_t f = frames ? frames : 1;
    if (g_wst[3])
        LOG("world path per frame (%u calls, %u batches): %u cycles in the VIF's call (parse %u, draws %u, the rest %u); "
            "the game's batch function %u cycles in %u calls", (uint32_t)(g_wst[3] / f), (uint32_t)(g_wst[4] / f),
            (uint32_t)(g_wst[0] * 12 / f), (uint32_t)(g_wst[1] * 12 / f), (uint32_t)(g_wst[2] * 12 / f),
            (uint32_t)((g_wst[0] - g_wst[1] - g_wst[2]) * 12 / f), (uint32_t)(g_wst[5] * 12 / f),
            (uint32_t)(g_wst[3] / f));
    memset(g_wst, 0, sizeof g_wst);
}

/* A DMA call of VU1 render data for a native pipeline (cars, world): batches
 * that end in MSCNT inside the packet are drawn now (their constants are all
 * in VU1 memory); a last batch without waits for the next microprogram start
 * (vif_start_micro). What can not be drawn goes to the VIF after all.
 * Returns 1 when the call is done. */
static int vif_defer_call_in(void *ctx, uint32_t addr, int tte);

static int vif_defer_call(void *ctx, uint32_t addr, int tte)
{
    uint64_t t0;
    int r;
    if (g_wstat != 1)
        return vif_defer_call_in(ctx, addr, tte);
    t0 = plat_ticks();
    g_wst_world = 0;
    r = vif_defer_call_in(ctx, addr, tte);
    if (g_wst_world)
        g_wst[0] += plat_ticks() - t0;
    return r;
}

/* Touches the lines a world batch's display list lookup will read (platform/wii/gs_gx.c; stage 0: its hash bucket and
 * the words dl_check samples, stage 1: the bucket's first list). The packet's batches are walked in order, so a batch
 * or two ahead there is time for the fills (a hint; the console's cache hints only: g_dcbz_on). A look-ahead along the
 * DMA chain, to touch the parse cache's lines of the calls to come, was tried and left out: the model's stalls moved
 * to the scanner's own tag reads (one line fill at a time) and it ended 1.4% slower than without. */
__attribute__((weak)) void gs_world_prefetch(const NwBlock *b, int stage)
{
    (void)b;
    (void)stage;
}
__attribute__((weak)) int g_dcbz_on;        /* runtime/memfast.c has it (the console's); 0 where that is not linked */

static int vif_defer_call_in(void *ctx, uint32_t addr, int tte)
{
    static NcPacket p;
    static NwPacket wp;
    const NcBlock *cb = NULL;           /* the batches, where they are (nc_packet_ref, nw_packet_ref) */
    const NwBlock *wb = NULL;
    Vif *v = ctx;
    uint32_t tag, qwc, end, start;
    int k, nb, kind, st, ok, okd;
    if (v->unit != 1 || !(g_native_render || g_vif_defer_test))
        return 0;
    if (v->deferred)
        vif_flush_deferred(v);
    kind = g_vu1_code == NC_CODE ? NW_CAR : nw_kind(g_vu1_code);
    if (!kind || !((g_native_render | (g_vif_defer_test ? 3 : 0)) & (kind == NW_CAR ? NATIVE_CARS : NATIVE_WORLD)))
        return 0;
    tag = LW(addr);
    qwc = tag & 0xFFFF;
    if (((tag >> 28) & 7) != 6 || !qwc || qwc > 16384) {    /* car select: lit packets of 5300 qwords */
        static int logged;
        if (logged < 12 && native_log_on()) {
            logged++;
            LOG("render data call (kind %d) at %08X declined: tag %08X %08X, next %08X %08X", kind, addr, tag,
                LW(addr + 4), LW(addr + 16 + qwc * 16), LW(addr + 20 + qwc * 16));
        }
        return 0;
    }
    end = addr + 16 + qwc * 16;
    start = tte ? addr + 8 : addr + 16;
    if (kind == NW_CAR) {
        p.cl = v->cl; p.wl = v->wl; p.mode = v->mode;
        memcpy(p.row, v->row, sizeof p.row);
        ok = nc_packet_ref(start, end, &p, &cb);
        nb = p.nb;
        if (!ok && ((LW(start + 4) >> 24) & 0x7F) != 0x4A) {
            static int logged;
            if (logged < 20 && native_log_on()) {
                logged++;
                LOG("car render data at %08X not native (qwc %u, cl %u wl %u mode %u)", addr, qwc, v->cl, v->wl, v->mode);
            }
        }
    } else {
        uint64_t t0 = g_wstat == 1 ? plat_ticks() : 0;
        wp.cl = v->cl; wp.wl = v->wl; wp.mode = v->mode;
        memcpy(wp.row, v->row, sizeof wp.row);
        ok = nw_packet_ref(start, end, kind, &wp, &wb);
        nb = wp.nb;
        if (!ok)
            log_world_reject(kind, addr, start, end, v);
        if (g_wstat == 1) {
            g_wst[1] += plat_ticks() - t0;
            g_wst[3]++;
            g_wst_world = 1;
        }
    }
    if (!ok)
        return 0;
    st = kind == NW_CAR ? 0 : 2;
    if (kind != NW_CAR && g_dcbz_on && nb > 0) {
        gs_world_prefetch(&wb[0], 0);       /* (the first batch has no lead for the second stage) */
        if (nb > 1)
            gs_world_prefetch(&wb[1], 0);
    }
    for (k = 0; k < nb; k++) {
        VifBatch b = vif_batch(kind, cb, wb, k);
        if (kind != NW_CAR && g_dcbz_on) {
            if (k + 2 < nb)
                gs_world_prefetch(&wb[k + 2], 0);
            if (k >= 1 && k + 1 < nb)
                gs_world_prefetch(&wb[k + 1], 1);
        }
        if (!b.mscnt) {
            v->deferred = b.start;
            v->deferred_end = end;
            v->dkind = kind;
            if (kind == NW_CAR)
                v->dbatch = cb[k];
            else
                v->dwbatch = wb[k];
            v->itops = b.n;
            break;
        }
        g_world_rep = kind != NW_CAR && k > 0;
        if (g_wstat == 1 && kind != NW_CAR) {
            const uint64_t d0 = plat_ticks();
            okd = gs_backend_world(&wb[k], kind);
            g_wst[2] += plat_ticks() - d0;
            g_wst[4]++;
        } else {
            okd = kind == NW_CAR ? gs_backend_car(&cb[k]) : gs_backend_world(&wb[k], kind);
        }
        if (!okd) {
            static int logged;
            if (logged < 12 && native_log_on()) {
                logged++;
                LOG("native draw of kind %d batch %d/%d (%u vertices) at %08X failed", kind, k, nb, b.n, addr);
            }
            g_vif_deferred[st + 1] += nb - k;
            vif_batch_regs(v, b.cl, b.wl, b.mode, b.row);
            vif_feed_range(v, b.start, end);
            return 1;
        }
        vif_batch_done(v, b.n);
        g_vif_deferred[st]++;
    }
    if (kind == NW_CAR)
        vif_batch_regs(v, p.cl, p.wl, p.mode, p.row);
    else
        vif_batch_regs(v, wp.cl, wp.wl, wp.mode, wp.row);
    return 1;
}

/* debugging: VU1 runs of programs that have a native pipeline */
static void __attribute__((noinline)) log_native_run(const Vif *v, int use_imm, uint32_t addr)
{
    static int logged;
    if ((g_vu1_code == NC_CODE || nw_kind(g_vu1_code)) && (!use_imm || addr) && logged < 60 &&
        native_log_on()) {
        logged++;
        LOG("native-kind VU1 run: code %06X %s %04X, dma %03X, itops %u, tops %u", g_vu1_code,
            use_imm ? "MSCAL" : "MSCNT", addr, g_dma_where, v->itops, v->tops);
        if (g_vu1_code == 0x00471FA0u && logged < 8 && g_native_log) {
            char line[256];
            size_t m = 0;
            unsigned k;
            for (k = 16; k > 0 && m < sizeof line - 24; k--) {
                unsigned j = (g_vif1_ring_n - k) & 15;
                m += (size_t)snprintf(line + m, sizeof line - m, " %08X/%03X", g_vif1_ring[j][0], g_vif1_ring[j][1]);
            }
            LOG("  last VIF1 commands:%s", line);
        }
    }
}

uint32_t g_vu1_particle_runs;   /* debugging: runs of the particle microprogram (the backend's state squares) */

static void vif_start_micro(Vif *v, int use_imm, uint32_t addr)
{
    if (v->unit == 1 && g_vu1_code == 0x0046D090u)
        g_vu1_particle_runs++;
    if (v->unit == 1 && v->deferred) {
        int st = v->dkind == NW_CAR ? 0 : 2;
        int same = v->dkind == NW_CAR ? g_vu1_code == NC_CODE : nw_kind(g_vu1_code) == v->dkind;
        if ((!use_imm || addr) && same &&
            (v->dkind == NW_CAR ? gs_backend_car(&v->dbatch) : gs_backend_world(&v->dwbatch, v->dkind))) {
            v->deferred = 0;
            vif_batch_done(v, v->dkind == NW_CAR ? v->dbatch.n : v->dwbatch.n);
            g_vif_deferred[st]++;
            return;
        }
        g_vif_deferred[st + 1]++;
        vif_flush_deferred(v);
    }
    if (v->unit == 1 && g_native_render)
        log_native_run(v, use_imm, addr);
    if (v->unit == 1) {
        v->top = v->tops;
        v->itop = v->itops;
        v->dbf ^= 1;
        v->tops = v->base + (v->dbf ? v->ofst : 0);
    } else {
        v->itop = v->itops;
    }
    if (v->unit == 1 && g_vu1_code == 0x0046A590u && use_imm && !addr && gs_backend_sky(v->mem))
        return;                         /* the sky: its one entry; nothing of a run is read afterwards */
    if (v->unit == 1 && g_vu1_code == 0x0046D090u && (!use_imm || !addr)) {
        /* the particles: its two entries are 0 (an emitter's first batch) and MSCNT */
        if (gs_backend_sparks(v->mem, v->top, v->itop, use_imm))
            return;
        vu_run(1, use_imm ? addr : 0xFFFFFFFFu);
        gs_backend_sparks_done(v->mem, vu_vi(1, 2));
        return;
    }
    if (v->unit == 1 && g_vu1_code == 0x0047C6F0u && (!use_imm || !addr)) {
        /* the embers: its entries are 0 (an emitter's first batch) and MSCNT */
        if (gs_backend_embers(v->mem, v->top, v->itop, use_imm))
            return;
        vu_run(1, use_imm ? addr : 0xFFFFFFFFu);
        gs_backend_embers_done();
        return;
    }
    if (v->unit == 1 && g_vu1_code == 0x0046D750u) {
        /* the particle systems: entry 0 once a frame, then each pool's
         * batches by MSCNT; the microprogram sets a pool up when the run
         * before was a pool's last (256 in its ITOP) or entry 0 */
        static int first = 1;
        if (!use_imm) {
            const int head = first;
            first = (v->itop & 0x3FF) >= 256;
            if (gs_backend_puffs(v->mem, v->top, v->itop, head))
                return;
            vu_run(1, 0xFFFFFFFFu);
            gs_backend_puffs_done(first);
            return;
        }
        first = 1;
    }
    vu_run(v->unit, use_imm ? addr : 0xFFFFFFFFu);
}

/* UNPACK, a whole command at a time (PCSX2 Vif_Unpack semantics). src holds
 * the payload words; elements are packed little-endian within them. */
static uint32_t unpack_elem(const uint32_t *src, uint32_t nwords, uint32_t ebits, uint32_t j, int usn)
{
    uint32_t raw;
    if (ebits == 32)
        return j < nwords ? src[j] : 0;
    if (ebits == 16) {
        if ((j >> 1) >= nwords)
            return 0;
        raw = (src[j >> 1] >> ((j & 1) * 16)) & 0xFFFF;
        return usn ? raw : (uint32_t)(int32_t)(int16_t)raw;
    }
    if ((j >> 2) >= nwords)
        return 0;
    raw = (src[j >> 2] >> ((j & 3) * 8)) & 0xFF;
    return usn ? raw : (uint32_t)(int32_t)(int8_t)raw;
}

/* The common UNPACK: no write mask, no filling write (WL <= CL), mode 0 or 1
 * (offset by ROW), and the payload is all there. One loop per format.
 * Returns 0 when the command needs the general path. */
static int unpack_fast(Vif *v, const uint32_t *src, uint32_t nwords)
{
    uint32_t cmd = (v->code >> 24) & 0x7F, fmt = cmd & 0xF;
    uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, ebits = 32u >> vl;
    uint32_t n = v->num, i, cl = v->cl ? v->cl : 1, wl = v->wl ? v->wl : 1;
    uint32_t a = v->addr >> 4, qmask = (v->mem_size >> 4) - 1, cyc, skip;
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, *mem = v->mem;
    int usn = (v->code >> 14) & 1;
    if (((cmd >> 4) & 1) && v->mask)
        return 0;
    if (wl > cl || v->mode > 1 || !n)
        return 0;
    if (fmt == 0xF) {
        if (nwords * 2 < n)
            return 0;
    } else if ((uint64_t)nwords * 32 < (uint64_t)n * (vn + 1) * ebits) {
        return 0;
    }
    if (v->mode == 1 && fmt != 0xF) {
        r0 = v->row[0]; r1 = v->row[1]; r2 = v->row[2]; r3 = v->row[3];
    }
    skip = cl - wl;
    cyc = v->cycle % wl;
#define PUT(x, y, z, w) do {                                                    \
        uint32_t *d_ = mem + ((a & qmask) << 2);                                \
        d_[0] = (x) + r0; d_[1] = (y) + r1; d_[2] = (z) + r2; d_[3] = (w) + r3; \
        a++;                                                                    \
        if (++cyc == wl) { cyc = 0; a += skip; }                                \
    } while (0)
#define E16(j) (usn ? (uint32_t)(uint16_t)(src[(j) >> 1] >> (((j) & 1) * 16)) \
                    : (uint32_t)(int32_t)(int16_t)(src[(j) >> 1] >> (((j) & 1) * 16)))
#define E8(j) (usn ? (uint32_t)(uint8_t)(src[(j) >> 2] >> (((j) & 3) * 8)) \
                   : (uint32_t)(int32_t)(int8_t)(src[(j) >> 2] >> (((j) & 3) * 8)))
    switch (fmt) {
    case 0x0: for (i = 0; i < n; i++) { uint32_t x = src[i]; PUT(x, x, x, x); } break;
    case 0x1: for (i = 0; i < n; i++) { uint32_t x = E16(i); PUT(x, x, x, x); } break;
    case 0x2: for (i = 0; i < n; i++) { uint32_t x = E8(i); PUT(x, x, x, x); } break;
    case 0x4: for (i = 0; i < n; i++) PUT(src[2 * i], src[2 * i + 1], src[2 * i], src[2 * i + 1]); break;
    case 0x5:
        for (i = 0; i < n; i++) { uint32_t x = E16(2 * i), y = E16(2 * i + 1); PUT(x, y, x, y); }
        break;
    case 0x6:
        for (i = 0; i < n; i++) { uint32_t x = E8(2 * i), y = E8(2 * i + 1); PUT(x, y, x, y); }
        break;
    /* V3 reads a fourth element like V4 (the next vector's x, 0 past the payload) */
    case 0x8:
        for (i = 0; i + 1 < n; i++) PUT(src[3 * i], src[3 * i + 1], src[3 * i + 2], src[3 * i + 3]);
        PUT(src[3 * i], src[3 * i + 1], src[3 * i + 2], 3 * i + 3 < nwords ? src[3 * i + 3] : 0);
        break;
    case 0x9:
        for (i = 0; i + 1 < n; i++) PUT(E16(3 * i), E16(3 * i + 1), E16(3 * i + 2), E16(3 * i + 3));
        PUT(E16(3 * i), E16(3 * i + 1), E16(3 * i + 2), (3 * i + 3) / 2 < nwords ? E16(3 * i + 3) : 0);
        break;
    case 0xA:
        for (i = 0; i + 1 < n; i++) PUT(E8(3 * i), E8(3 * i + 1), E8(3 * i + 2), E8(3 * i + 3));
        PUT(E8(3 * i), E8(3 * i + 1), E8(3 * i + 2), (3 * i + 3) / 4 < nwords ? E8(3 * i + 3) : 0);
        break;
    case 0xC:
        /* whole qwords as they are (a particle system's run is 400 words of these): a copy when nothing is
         * added, skipped or wrapped */
        if (!(r0 | r1 | r2 | r3) && !skip && (a & qmask) + n <= qmask + 1) {
            memcpy(mem + ((a & qmask) << 2), src, (size_t)n * 16);
            a += n;
            break;
        }
        for (i = 0; i < n; i++) PUT(src[4 * i], src[4 * i + 1], src[4 * i + 2], src[4 * i + 3]);
        break;
    case 0xD:
        for (i = 0; i < n; i++) PUT(E16(4 * i), E16(4 * i + 1), E16(4 * i + 2), E16(4 * i + 3));
        break;
    case 0xE:
        for (i = 0; i < n; i++) PUT(E8(4 * i), E8(4 * i + 1), E8(4 * i + 2), E8(4 * i + 3));
        break;
    case 0xF:
        for (i = 0; i < n; i++) {
            uint32_t c = (uint16_t)(src[i >> 1] >> ((i & 1) * 16));
            PUT((c & 0x1F) << 3, (c & 0x3E0) >> 2, (c & 0x7C00) >> 7, (c & 0x8000) >> 8);
        }
        break;
    default:
        return 0;
    }
#undef PUT
#undef E16
#undef E8
    v->cycle += n;
    v->addr = (a << 4) & (v->mem_size - 1);
    v->num = 0;
    v->remaining = 0;
    return 1;
}

uint32_t g_vif_unpack_fmt[16][2];       /* status line: UNPACKs and their words by format (vn, vl) */

static void unpack_bulk(Vif *v, const uint32_t *src, uint32_t nwords)
{
    uint32_t cmd = (v->code >> 24) & 0x7F;
    uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, nelem = vn + 1, ebits = 32u >> vl;
    int usn = (v->code >> 14) & 1, masked = (cmd >> 4) & 1, v45 = vn == 3 && vl == 3;
    uint32_t cl = v->cl ? v->cl : 1, wl = v->wl ? v->wl : 1;
    uint32_t sv = 0, i;
    uint32_t *mem = v->mem, wmask = (v->mem_size >> 2) - 1;
    g_vif_unpack_fmt[cmd & 15][0]++;
    g_vif_unpack_fmt[cmd & 15][1] += nwords;
#if defined(VIF_UNPACK_CHECK)
    {
        /* debug: the fast path must write what the general one does */
        static uint32_t m0[0x1000], m1[0x1000];
        static Vif v0, v1;
        static unsigned checks, bad;
        static int inside;
        if (!inside) {
            int k;
            memcpy(m0, v->mem, v->mem_size);
            v0 = *v;
            if (unpack_fast(v, src, nwords)) {
                memcpy(m1, v->mem, v->mem_size);
                v1 = *v;
                memcpy(v->mem, m0, v->mem_size);
                *v = v0;
                inside = 1;
                unpack_bulk(v, src, nwords);
                inside = 0;
                checks++;
                for (k = 0; k < (int)v->mem_size / 4; k++)
                    if (m1[k] != v->mem[k])
                        break;
                if (k < (int)v->mem_size / 4 || (v1.addr & (v->mem_size - 1)) != (v->addr & (v->mem_size - 1))) {
                    if (++bad <= 50)
                        LOG("VIF unpack check: code %08X cl %u wl %u mode %u: word %X fast %08X general %08X, addr %X vs %X",
                            v0.code, v0.cl, v0.wl, v0.mode, k, k < (int)v->mem_size / 4 ? m1[k] : 0,
                            k < (int)v->mem_size / 4 ? v->mem[k] : 0, v1.addr, v->addr);
                }
                if ((checks & 0xFFFF) == 0)
                    LOG("VIF unpack check: %u fast unpacks, %u differ", checks, bad);
                return;
            }
        }
        if (!inside && unpack_fast(v, src, nwords))
            return;
    }
#else
    if (unpack_fast(v, src, nwords))
        return;
#endif
    for (i = 0; i < v->num; i++) {
        uint32_t cyc = v->cycle % wl, e[4], k;
        int fill = wl > cl && cyc >= cl;    /* filling write: no data of its own */
        if (v45) {
            uint32_t c = unpack_elem(src, nwords, 16, sv, 1);
            e[0] = (c & 0x1F) << 3;
            e[1] = (c & 0x3E0) >> 2;
            e[2] = (c & 0x7C00) >> 7;
            e[3] = (c & 0x8000) >> 8;
        } else {
            uint32_t j = sv * nelem;
            e[0] = unpack_elem(src, nwords, ebits, j, usn);
            switch (nelem) {
            case 1: e[1] = e[2] = e[3] = e[0]; break;
            case 2: e[1] = unpack_elem(src, nwords, ebits, j + 1, usn); e[2] = e[0]; e[3] = e[1]; break;
            default:     /* V3 reads a fourth element like V4 (the next vector's x) */
                e[1] = unpack_elem(src, nwords, ebits, j + 1, usn);
                e[2] = unpack_elem(src, nwords, ebits, j + 2, usn);
                e[3] = unpack_elem(src, nwords, ebits, j + 3, usn);
                break;
            }
        }
        if (!fill)
            sv++;
        for (k = 0; k < 4; k++) {
            uint32_t val = e[k];
            uint32_t m = masked ? (v->mask >> (2 * (k + 4 * (cyc > 3 ? 3 : cyc)))) & 3 : 0;
            switch (m) {
            case 0:
                switch (v45 ? 0 : v->mode) {
                case 1: val += v->row[k]; break;
                case 2: v->row[k] = val = val + v->row[k]; break;
                case 3: v->row[k] = val; break;
                }
                break;
            case 1: val = v->row[k]; break;
            case 2: val = v->col[cyc > 3 ? 3 : cyc]; break;
            case 3: continue;                       /* write protected */
            }
            mem[((v->addr >> 2) + k) & wmask] = val;
        }
        v->cycle++;
        v->addr += 16;
        if (wl < cl && v->cycle % wl == 0)
            v->addr += 16 * (cl - wl);              /* skipping write */
    }
    v->num = 0;
    v->remaining = 0;
}

static uint32_t unpack_words(uint32_t cmd, uint32_t num)
{
    uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
    uint32_t bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
    return (bits * num + 31) / 32;
}

/* "wstat 9": VIF1 commands per frame by code (vif_cmd_report) */
static uint32_t g_vif_cmds[128];

void vif_cmd_report(uint32_t frames)
{
    char line[400];
    size_t m = 0;
    uint32_t k, j, all = 0, f = frames ? frames : 1;
    for (k = 0; k < 128; k++)
        all += g_vif_cmds[k];
    for (j = 0; j < 12; j++) {
        uint32_t best = 0;
        for (k = 1; k < 128; k++)
            if (g_vif_cmds[k] > g_vif_cmds[best])
                best = k;
        if (!g_vif_cmds[best])
            break;
        m += (size_t)snprintf(line + m, sizeof line - m, " %02X:%u", best, g_vif_cmds[best] / f);
        g_vif_cmds[best] = 0;
    }
    line[m] = 0;
    LOG("vif: VIF1 commands per frame %u, by code:%s", all / f, line);
    LOG("vif: DMA tags per frame whose two VIF words were NOPs taken at once %u; transfers of no qwords %u",
        g_dma_tag_nops / f, g_dma_empty / f);
    memset(g_vif_cmds, 0, sizeof g_vif_cmds);
    g_dma_tag_nops = g_dma_empty = 0;
}

static void vif_command(Vif *v, uint32_t code)
{
    uint32_t cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    if (PS2_UNLIKELY(g_wstat == 9) && v->unit)
        g_vif_cmds[cmd]++;
    if (PS2_UNLIKELY(g_native_log) && v->unit) {
        /* (the ring of the last commands, for log_native_run: with "nlog 1" only, not at every command of a frame) */
        g_vif1_ring[g_vif1_ring_n & 15][0] = code;
        g_vif1_ring[g_vif1_ring_n++ & 15][1] = g_dma_where;
    }
    v->code = code;
    v->remaining = 0;
    v->stage = 0;
    switch (cmd) {
    case 0x00: break;                                       /* NOP */
    case 0x01: v->cl = imm & 0xFF; v->wl = imm >> 8; break;  /* STCYCL */
    case 0x02: if (v->unit) { v->ofst = imm & 0x3FF; v->dbf = 0; v->tops = v->base; } break;
    case 0x03: if (v->unit) v->base = imm & 0x3FF; break;
    case 0x04: v->itops = imm & 0x3FF; break;
    case 0x05: v->mode = imm & 3; break;
    case 0x06:
        if (v->unit) {
            v->mskpath3 = (imm >> 15) & 1;
            g_gif_m3p = v->mskpath3;
            p3_resume(1);
        }
        break;
    case 0x07: v->mark = imm; break;
    case 0x10: case 0x11: break;                            /* FLUSHE/FLUSH: VU runs synchronously */
    case 0x13: if (v->unit) p3_resume(0); break;            /* FLUSHA: also waits for PATH3 */
    case 0x14: vif_start_micro(v, 1, imm * 8); break;       /* MSCAL */
    case 0x15: vif_start_micro(v, 1, imm * 8); break;       /* MSCALF */
    case 0x17: vif_start_micro(v, 0, 0); break;             /* MSCNT */
    case 0x20: v->remaining = 1; break;                     /* STMASK */
    case 0x0B:                                              /* native draw command (hle_render.c) */
        if ((code & 0xFFFF0000u) == VIF_NATIVE_CODE)
            v->remaining = imm < 16 ? imm : 16;
        break;
    case 0x30: case 0x31: v->remaining = 4; break;          /* STROW/STCOL */
    case 0x4A:                                              /* MPG */
        if (v->deferred)
            vif_flush_deferred(v);
        if (v->unit)
            g_vu1_code = g_dma_call;
        v->num = num ? num : 256;
        v->remaining = v->num * 2;
        v->addr = imm * 8;
        break;
    case 0x50: case 0x51:                                   /* DIRECT/DIRECTHL */
        v->remaining = (imm ? imm : 65536) * 4;
        v->direct_fill = 0;
        break;
    default:
        if ((cmd & 0x60) == 0x60) {                         /* UNPACK */
            uint32_t addr = imm & 0x3FF;
            if (v->unit && (imm & 0x8000))
                addr += v->tops;
            v->addr = addr * 16;
            v->num = num ? num : 256;
            v->cycle = 0;
            v->ufill = 0;
            v->remaining = unpack_words(cmd, v->num);
            if (!(v->wl <= v->cl) && v->wl) {
                /* filling write: WL > CL; data only for the first CL of each WL */
                uint32_t full = v->num / v->wl, rest = v->num % v->wl;
                uint32_t with_data = full * v->cl + (rest < v->cl ? rest : v->cl);
                v->remaining = unpack_words(cmd, with_data);
            }
            break;
        }
        LOG("VIF%d: unknown command %02X (code %08X)", v->unit, cmd, code);
        break;
    }
}

static void vif_data(Vif *v, uint32_t w)
{
    uint32_t cmd = (v->code >> 24) & 0x7F;
    switch (cmd) {
    case 0x20:
        v->mask = w;
        v->remaining = 0;
        return;
    case 0x30:
        v->row[v->stage++] = w;
        v->remaining--;
        return;
    case 0x0B:
        v->native[v->stage++] = w;
        if (--v->remaining == 0)
            gs_backend_native(v->native, (int)v->stage);
        return;
    case 0x31:
        v->col[v->stage++] = w;
        v->remaining--;
        return;
    case 0x4A: {
        uint32_t *micro = g_vu_mem + (v->unit ? 0x8000 : 0x0000) / 4;
        uint32_t size = v->unit ? 0x4000 : 0x1000, *m = &micro[(v->addr & (size - 1)) >> 2];
        if (v->unit)
            vif_mpg_flush();
        if (*m != w) {
            *m = w;
            vu_micro_written_at(v->unit, v->addr & (size - 1), 4);
        }
        v->addr += 4;
        v->remaining--;
        return;
    }
    case 0x50: case 0x51:
        v->direct_qw[v->direct_fill++] = w;
        if (v->direct_fill == 4) {
            int prev = pt_enter(PT_GIF);
            gif_path_qword(2, v->direct_qw);
            pt_leave(prev);
            v->direct_fill = 0;
        }
        v->remaining--;
        return;
    default:
        v->remaining = 0;
        return;
    }
}

uint32_t g_vif_words[4];        /* status line: UNPACK, DIRECT, MPG, other words */

/* Microprograms for VU1 stay where the game has them. It loads one before
 * nearly every group of batches, 70 times a frame and 150 KB in all, and
 * almost none of it is ever read: a recompiled program is known by 12 words
 * of each of its ranges (rec_find), a native pipeline by where the upload
 * came from. So an MPG's words are noted (where in micro memory, where in
 * guest memory), not copied; vif_micro_words hands out what micro memory
 * would hold, and vif_mpg_flush writes the notes out when something wants
 * the memory itself (the interpreter, a search through every program, a
 * ninth note) and when the chain is done, since the game's memory is its own
 * again after that. g_mpg_lazy 0 (autopilot "mpglazy 0"): copied as before. */
int g_mpg_lazy = 1;
uint32_t g_mpg_noted, g_mpg_flushed;    /* status line: words noted, words written out after all */
#define MPG_SEGS 32
static struct MpgSeg {
    uint32_t off, n;                    /* in words of micro memory */
    const uint32_t *src;
} g_mpg[MPG_SEGS];                      /* no two overlap */
static int g_nmpg;

void vif_mpg_flush(void)
{
    uint32_t *micro = g_vu_mem + 0x8000 / 4;
    int k;
    for (k = 0; k < g_nmpg; k++) {
        memcpy(&micro[g_mpg[k].off], g_mpg[k].src, g_mpg[k].n * 4);
        g_mpg_flushed += g_mpg[k].n;
    }
    g_nmpg = 0;
}

/* n words from src for micro memory from word off: what they write over of older notes is dropped from those
 * (a program is loaded over the one before it: its note is all that is left of that one, unless it was longer) */
static void mpg_note(uint32_t off, uint32_t n, const uint32_t *src)
{
    const uint32_t ne = off + n;
    int k, j = 0;
    for (k = 0; k < g_nmpg; k++)
        if (g_mpg[k].off < off && g_mpg[k].off + g_mpg[k].n > ne) {
            vif_mpg_flush();            /* it would be cut in two */
            break;
        }
    for (k = 0; k < g_nmpg; k++) {
        struct MpgSeg t = g_mpg[k];
        const uint32_t e = t.off + t.n;
        if (t.off >= off && e <= ne)
            continue;                   /* written over whole */
        if (t.off >= off && t.off < ne) {
            t.src += ne - t.off;        /* its head */
            t.n = e - ne;
            t.off = ne;
        } else if (e > off && e <= ne) {
            t.n = off - t.off;          /* its tail */
        }
        g_mpg[j++] = t;
    }
    g_nmpg = j;
    if (g_nmpg == MPG_SEGS)
        vif_mpg_flush();
    g_mpg[g_nmpg].off = off;
    g_mpg[g_nmpg].n = n;
    g_mpg[g_nmpg++].src = src;
    g_mpg_noted += n;
}

/* n words of VU1 micro memory from word off as they are or will be: NULL when they are in more than one place
 * (write the notes out, then they are in micro memory) */
const uint32_t *vif_micro_words(uint32_t off, uint32_t n)
{
    int k;
    for (k = g_nmpg - 1; k >= 0; k--) {
        const struct MpgSeg *s = &g_mpg[k];
        if (off >= s->off && off + n <= s->off + s->n)
            return s->src + (off - s->off);
        if (off < s->off + s->n && off + n > s->off)
            return NULL;
    }
    return g_vu_mem + 0x8000 / 4 + off;
}

static void vif_feed_words(Vif *v, const uint32_t *w, uint32_t n)
{
    uint32_t k = 0;
    while (k < n) {
        if (!v->remaining) {
            if (!w[k] && g_opt_new) {
                /* NOP (half of a race frame's 9000 commands are this padding): what vif_command leaves */
                do
                    k++;
                while (k < n && !w[k]);
                v->code = 0;
                v->stage = 0;
                continue;
            }
            vif_command(v, w[k++]);
            if (v->remaining || ((v->code >> 29) & 3) != 3)
                continue;
            unpack_bulk(v, w, 0);               /* UNPACK without data words */
            continue;
        }
        if ((((v->code >> 24) & 0x7F) | 1) == 0x51 && !v->direct_fill && v->remaining >= 4 && n - k >= 4) {
            /* DIRECT: image data goes to the GIF in bulk */
            uint32_t nq = (v->remaining < n - k ? v->remaining : n - k) / 4, t;
            int prev = pt_enter(PT_GIF);
            g_vif_words[1] += nq * 4;
            while (nq) {
                t = gif_path_bulk(2, w + k, nq);
                k += t * 4;
                v->remaining -= t * 4;
                nq -= t;
            }
            pt_leave(prev);
            continue;
        }
        if (((v->code >> 24) & 0x7F) == 0x4A) {  /* MPG: microcode in bulk, written only when it differs */
            uint32_t *micro = g_vu_mem + (v->unit ? 0x8000 : 0x0000) / 4;
            uint32_t size = v->unit ? 0x4000 : 0x1000, t = v->remaining < n - k ? v->remaining : n - k;
            uint32_t off = (v->addr & (size - 1)) >> 2, run = t;
            g_vif_words[2] += t;
            if (off + run > size / 4)
                run = size / 4 - off;           /* wraps: the rest word by word */
            if (v->unit && g_mpg_lazy && (const uint8_t *)(w + k) >= ps2_ram &&
                (const uint8_t *)(w + k + run) <= ps2_ram + PS2_RAM_SIZE) {
                mpg_note(off, run, w + k);
                vu_micro_written(1);
            } else if ((v->unit ? (vif_mpg_flush(), 0) : 0) || memcmp(&micro[off], w + k, run * 4)) {
                memcpy(&micro[off], w + k, run * 4);
                vu_micro_written_at(v->unit, off * 4, run * 4);
            }
            v->addr += run * 4;
            v->remaining -= run;
            k += run;
            continue;
        }
        if (((v->code >> 29) & 3) == 3) {        /* UNPACK: decode the payload in one go */
            uint32_t need = v->remaining, have = n - k;
            g_vif_words[0] += need < have ? need : have;
            if (!v->ufill && have >= need) {
                unpack_bulk(v, w + k, need);
                k += need;
            } else {
                uint32_t t = have < need ? have : need;
                memcpy(v->ubuf + v->ufill, w + k, t * 4);
                v->ufill += t;
                v->remaining -= t;
                k += t;
                if (!v->remaining) {
                    unpack_bulk(v, v->ubuf, v->ufill);
                    v->ufill = 0;
                }
            }
            continue;
        }
        {
            uint32_t cmd = (v->code >> 24) & 0x7F;
            g_vif_words[cmd == 0x50 || cmd == 0x51 ? 1 : cmd == 0x4A ? 2 : 3]++;
        }
        vif_data(v, w[k++]);
    }
}

static void vif_sink(void *ctx, const uint32_t *qw, uint32_t n)
{
    vif_feed_words(ctx, qw, n * 4);
}

static void vif_tag_sink(void *ctx, const uint32_t *qw, uint32_t n)
{
    Vif *v = ctx;
    (void)n;
    if (!(qw[2] | qw[3]) && !v->remaining && g_opt_new && g_opt3) {
        /* two NOPs, no command waiting for data: what vif_feed_words leaves for them */
        if (PS2_UNLIKELY(g_wstat == 9))
            g_dma_tag_nops++;
        v->code = 0;
        v->stage = 0;
        return;
    }
    vif_feed_words(ctx, qw + 2, 2);
}

void dma_vif1(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
{
    if (chcr & 1) {
        int prev = pt_enter(PT_VIF);
        dma_run(chcr, madr, qwc, tadr, 1, vif_sink, vif_tag_sink, &g_vif[1]);
        if (g_vif[1].deferred)
            vif_flush_deferred(&g_vif[1]);
        p3_resume(0);                   /* an unmasked PATH3 carries on after VIF1 */
        pt_leave(prev);
    } else
        LOG("VIF1 DMA from VIF (download) not emulated");
}

void dma_vif0(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
{
    int prev = pt_enter(PT_VIF);
    dma_run(chcr, madr, qwc, tadr, 1, vif_sink, vif_tag_sink, &g_vif[0]);
    pt_leave(prev);
}

void vif1_fifo_write(u128 v) { vif_feed_words(&g_vif[1], v.w, 4); }
void vif0_fifo_write(u128 v) { vif_feed_words(&g_vif[0], v.w, 4); }

/* ---- GIF path 3 -------------------------------------------------------------------- */

/* The GIF channel is a resumable transfer. While VIF1 masks PATH3 (MSKPATH3)
 * the GIF stops it at the end of a packet: Burnout 3 sends a frame's textures
 * in batches separated by EOP and releases one batch at a time from the VIF1
 * stream (MSKPATH3 0, MSKPATH3 1, FLUSHA), so texture memory is reused within
 * the frame. Running the whole chain at once would overwrite textures before
 * the geometry that uses them is drawn. */
typedef struct Path3 {
    int active;             /* channel started, not finished */
    uint32_t chcr, tadr;
    uint32_t addr, left;    /* data block being sent */
    int spr, last;          /* last: no tag follows the block */
    uint32_t asr[2];
    int sp;
} Path3;

static Path3 g_p3;

static void p3_next_tag(void)
{
    Path3 *p = &g_p3;
    uint32_t tag[4], n, id;
    int tspr = (p->tadr >> 31) & 1;
    read_qwords(p->tadr & 0x7FFFFFF0u, tspr, tag, 1);
    n = tag[0] & 0xFFFF;
    id = (tag[0] >> 28) & 7;
    p->left = n;
    p->addr = p->tadr + 16;
    p->spr = tspr;
    switch (id) {
    case 0: /* refe */
        p->addr = tag[1] & 0x7FFFFFF0u;
        p->spr = (tag[1] >> 31) & 1;
        p->last = 1;
        break;
    case 1: /* cnt */
        p->tadr += 16 + n * 16;
        break;
    case 2: /* next */
        p->tadr = tag[1];
        break;
    case 3: case 4: /* ref, refs */
        p->addr = tag[1] & 0x7FFFFFF0u;
        p->spr = (tag[1] >> 31) & 1;
        p->tadr += 16;
        break;
    case 5: /* call */
        if (p->sp >= 2)
            FATAL("GIF DMA call stack overflow");
        p->asr[p->sp++] = p->tadr + 16 + n * 16;
        p->tadr = tag[1];
        break;
    case 6: /* ret */
        if (p->sp == 0)
            p->last = 1;
        else
            p->tadr = p->asr[--p->sp];
        break;
    default: /* end */
        p->last = 1;
        break;
    }
    if ((tag[0] >> 31) && (p->chcr & 0x80))   /* IRQ with TIE: stop after this tag */
        p->last = 1;
}

/* Send PATH3 data until the channel finishes (returns 1) or, with
 * stop_at_eop, until a packet with EOP has been sent (returns 0). */
static int p3_run(int stop_at_eop)
{
    Path3 *p = &g_p3;
    long guard = 0;
    int prev = pt_enter(PT_GIF);
    while (p->active) {
        uint32_t q[4];
        if (!p->left) {
            if (p->last) {
                p->active = 0;
                break;
            }
            if (++guard > 1000000)
                FATAL("GIF DMA chain does not terminate (tadr %08X)", p->tadr);
            p3_next_tag();
            continue;
        }
        if (!p->spr && (p->addr & PS2_RAM_MASK) + p->left * 16 <= PS2_RAM_SIZE) {
            uint32_t n = gif_path_bulk(3, (const uint32_t *)GPTR(p->addr), p->left);
            p->addr += 16 * n;
            p->left -= n;
            if (stop_at_eop && gif_path_at_eop(3) && (p->left || !p->last))
                break;
            continue;
        }
        read_qwords(p->addr, p->spr, q, 1);
        p->addr += 16;
        p->left--;
        gif_path_qword(3, q);
        if (stop_at_eop && gif_path_at_eop(3) && (p->left || !p->last))
            break;
    }
    g_gif_p3_pending = p->active;
    pt_leave(prev);
    return !p->active;
}

/* VIF1 sync points: unmask lets one packet through; FLUSHA and the end of the
 * VIF1 transfer wait for an unmasked PATH3 to finish. */
static void p3_resume(int stop_at_eop)
{
    if (g_p3.active && !g_gif_m3p && p3_run(stop_at_eop))
        hw_dma_complete(DMAC_GIF);
}

int dma_gif(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr)
{
    Path3 *p = &g_p3;
    uint32_t mode = (chcr >> 2) & 3;
    if (p->active)
        LOG("GIF DMA restarted while %u qwords of the last one were pending", p->left);
    memset(p, 0, sizeof *p);
    p->active = 1;
    p->chcr = chcr;
    p->tadr = tadr;
    p->addr = madr & 0x7FFFFFF0u;
    p->spr = (madr >> 31) & 1;
    p->left = qwc;
    p->last = mode == 0 || mode == 2;       /* normal: just the block */
    if (g_gif_m3p) {
        g_gif_p3_pending = 1;
        return 0;
    }
    return p3_run(0);
}

void gif_fifo_write(u128 v) { gif_path_qword(3, v.w); }

/* ---- scratchpad DMA --------------------------------------------------------------- */

void dma_spr(int ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t sadr)
{
    uint32_t n = qwc * 16, k;
    if ((chcr >> 2) & 1) {
        LOG("SPR DMA ch%d chain mode not emulated (chcr %08X tadr %08X)", ch, chcr, tadr);
        return;
    }
#if !defined(PS2_DYNAMIC_IO)
    /* the scratchpad is guest RAM 0..0x3FFF, words in host order like RAM:
     * copy runs that wrap neither the scratchpad nor RAM */
    if ((madr >> 28) != 1) {
        madr &= PS2_RAM_MASK & ~15u;
        sadr &= 0x3FF0;
        while (n) {
            uint32_t run = n, s_left = 0x4000 - sadr, m_left = PS2_RAM_SIZE - madr;
            if (run > s_left) run = s_left;
            if (run > m_left) run = m_left;
            if (ch == DMAC_TO_SPR)
                memcpy(GPTR(sadr), GPTR(madr), run);
            else
                memcpy(GPTR(madr), GPTR(sadr), run);
            n -= run;
            sadr = (sadr + run) & 0x3FF0;
            madr = (madr + run) & PS2_RAM_MASK;
        }
        return;
    }
#endif
    if (ch == DMAC_TO_SPR)
        for (k = 0; k < n; k += 4)
            SW((sadr + k) & 0x3FFC, LW(madr + k));
    else
        for (k = 0; k < n; k += 4)
            SW(madr + k, LW((sadr + k) & 0x3FFC));
#if defined(PS2_DYNAMIC_IO)
    {   /* debug: the address watchpoint also sees scratchpad DMA into RAM */
        extern uint32_t g_watch_addr;
        if (ch == DMAC_FROM_SPR && g_watch_addr - (madr & 0x1FFFFFF0u) < n) {
            R5900 *c = kernel_current_ctx();
            LOG("watch: SPR DMA %08X <- spr %04X (%u bytes) writes %08X", madr, sadr, n, LW(g_watch_addr));
            if (c)
                rt_dump_stack(c, "watch");
        }
    }
#endif
}
