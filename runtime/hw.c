/*
 * EE hardware registers (0x10000000-0x1000FFFF), VU memory (0x11000000) and
 * GS privileged registers (0x12000000).
 *
 * DMA transfers complete at the moment they are started; the per-channel
 * handlers (gif, vif, spr) consume the data immediately.
 */
#include "rt.h"
#include "hw.h"
#include "vu.h"
#include "wtime.h"

#include <string.h>

static uint32_t g_ee_regs[0x10000 / 4];
static uint64_t g_gs_priv[0x2000 / 8];
uint32_t g_vu_mem[0x4000];              /* VU0 micro/data, VU1 micro/data as at 0x11000000, host-order words */
static uint8_t g_unknown_logged[0x10000 / 4];

#define REG(a) g_ee_regs[((a) & 0xFFFF) >> 2]

/* DMA channel base addresses */
static const uint32_t g_dma_base[10] = {
    0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
    0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400,
};

static int dma_channel(uint32_t a)
{
    int k;
    for (k = 0; k < 10; k++)
        if ((a & ~0xFFu) == g_dma_base[k])
            return k;
    return -1;
}

uint32_t hw_reg(uint32_t a) { return REG(a); }

void hw_dump_state(void)
{
    extern int g_gif_m3p, g_gif_p3_pending;
    LOG("hw: D1_CHCR %08X D2_CHCR %08X D_STAT %08X VIF1_STAT %08X GIF_STAT %08X m3p %d p3_pending %d",
        REG(0x10009000), REG(0x1000A000), REG(D_STAT), REG(VIF1_STAT), REG(0x10003020), g_gif_m3p,
        g_gif_p3_pending);
}
void hw_set_reg(uint32_t a, uint32_t v) { REG(a) = v; }

void hw_dma_complete(int ch);

static void dma_done(int ch)
{
    uint32_t chcr = REG(g_dma_base[ch]);
    REG(g_dma_base[ch]) = chcr & ~0x100u;     /* STR off */
    REG(D_STAT) |= 1u << ch;
    kernel_raise_dmac(ch);              /* the kernel's mask decides delivery */
}

#ifdef WTIME
static void dma_kick_inner(int ch);
static void dma_kick(int ch)
{
    /* the scratchpad's copies are the game's own code's work; the chain and the GIF are the renderer's */
    const int foreign = ch != DMAC_TO_SPR && ch != DMAC_FROM_SPR;
    if (foreign)
        WT_ENTER();
    dma_kick_inner(ch);
    if (foreign)
        WT_LEAVE();
}
static void dma_kick_inner(int ch)
#else
static void dma_kick(int ch)
#endif
{
    uint32_t base = g_dma_base[ch];
    uint32_t chcr = REG(base), madr = REG(base + 0x10), qwc = REG(base + 0x20), tadr = REG(base + 0x30);
    if (g_trace)
        LOG("DMA ch%d start chcr %08X madr %08X qwc %X tadr %08X", ch, chcr, madr, qwc, tadr);
    switch (ch) {
    case DMAC_VIF1:
        /* Burnout 3 sends each frame's display list as one VIF1 chain */
        if ((chcr & 0xC) == 4) {
            void tl_add(int what, uint32_t arg);
            g_vif1_chains++;
            tl_add('C', g_vif1_chains);
            dma_vif1(chcr, madr, qwc, tadr);
            vif_mpg_flush();            /* the microprograms noted in the chain: the game's memory is its own again */
            tl_add('c', g_vif1_chains);
            break;
        }
        dma_vif1(chcr, madr, qwc, tadr);
        vif_mpg_flush();
        break;
    case DMAC_GIF:
        if (!dma_gif(chcr, madr, qwc, tadr))
            return;                      /* held back by MSKPATH3; completes later */
        break;
    case DMAC_VIF0:
        dma_vif0(chcr, madr, qwc, tadr);
        break;
    case DMAC_TO_SPR:
    case DMAC_FROM_SPR:
        dma_spr(ch, chcr, madr, qwc, tadr, REG(base + 0x80));
        break;
    default:
        LOG("DMA on channel %d not emulated (chcr %08X madr %08X qwc %X)", ch, chcr, madr, qwc);
        break;
    }
    REG(base + 0x20) = 0;
    dma_done(ch);
}

void hw_dma_complete(int ch)
{
    REG(g_dma_base[ch] + 0x20) = 0;
    dma_done(ch);
}

/* ---- EE registers ------------------------------------------------------------------- */

static void note_unknown(uint32_t a, const char *what, uint64_t v)
{
    uint32_t k = (a & 0xFFFF) >> 2;
    if (g_unknown_logged[k] < 3) {
        g_unknown_logged[k]++;
        LOG("io %s %08X = %llX", what, a, (unsigned long long)v);
    }
}

/* ---- timers ----------------------------------------------------------------------- */

/* T0..T3: 16-bit counters on the bus clock (147.456 MHz, /16, /256) or the
 * horizontal blank, counting from host time while enabled (MODE.CUE). They
 * raise their interrupt when they overflow (OVFE) or reach COMP (CMPE; with
 * ZRET they start over from 0 there). Burnout 3's millisecond clock is T0 at
 * /256 plus a count of its overflows (its handler, every 113.8 ms): without
 * the interrupt that clock started over nine times a second. The kernel
 * polls the timers with everything else that is due (hw_timers_poll). */
#define TM_COMP 0x20u               /* register offsets from COUNT */
#define TM_CUE 0x80u
#define TM_ZRET 0x40u
#define TM_CMPE 0x100u
#define TM_OVFE 0x200u
#define TM_EQUF 0x400u
#define TM_OVFF 0x800u

static struct {
    uint64_t base_us;               /* host time at which the counter was base */
    uint32_t base;
    uint64_t next_us;               /* host time of its next overflow or compare, when there is one (on) */
    int on, next_cmp;               /* next_cmp: that event is the compare */
} g_tm[4];

static uint64_t tm_ticks(uint32_t mode, uint64_t us)
{
    if (us < 200000u) {
        /* the same fractions in 32 bits (a division of 64 is a library call on the console, and
         * the game reads its clock all the time) */
        const uint32_t u = (uint32_t)us;
        switch (mode & 3) {
        case 0: return u * 18432u / 125u;
        case 1: return u * 1152u / 125u;
        case 2: return u * 72u / 125u;
        default: break;
        }
    }
    switch (mode & 3) {
    case 0: return us * 147456u / 1000u;            /* bus clock */
    case 1: return us * 147456u / 16000u;           /* /16 */
    case 2: return us * 147456u / 256000u;          /* /256 */
    default: return us * 1000u / HSYNC_NS_DIV;      /* hblank */
    }
}

/* host time for a number of ticks, rounded up */
static uint64_t tm_us(uint32_t mode, uint64_t ticks)
{
    switch (mode & 3) {
    case 0: return (ticks * 1000u + 147455u) / 147456u;
    case 1: return (ticks * 16000u + 147455u) / 147456u;
    case 2: return (ticks * 256000u + 147455u) / 147456u;
    default: return (ticks * HSYNC_NS_DIV + 999u) / 1000u;
    }
}

static uint32_t tm_count_at(int n, uint64_t now)
{
    uint32_t mode = REG(T0_MODE + n * 0x800);
    if (!(mode & TM_CUE) || now < g_tm[n].base_us)
        return g_tm[n].base & 0xFFFF;
    return (uint32_t)(g_tm[n].base + tm_ticks(mode, now - g_tm[n].base_us)) & 0xFFFF;
}

/* the counter is `count` now: when is its next event */
static void tm_schedule(int n, uint32_t count, uint64_t now)
{
    uint32_t mode = REG(T0_MODE + n * 0x800), comp = REG(T0_COUNT + n * 0x800 + TM_COMP) & 0xFFFF;
    uint32_t to_ovf = 0x10000u - (count & 0xFFFF), to_cmp = ((comp - count - 1) & 0xFFFF) + 1;
    g_tm[n].base = count & 0xFFFF;
    g_tm[n].base_us = now;
    g_tm[n].next_us = ~0ull;
    g_tm[n].on = 0;
    if (!(mode & TM_CUE))
        return;
    /* the compare matters when it interrupts or restarts the counter; else the overflow */
    if ((mode & (TM_CMPE | TM_ZRET)) && to_cmp <= to_ovf) {
        g_tm[n].next_cmp = 1;
        g_tm[n].next_us = now + tm_us(mode, to_cmp);
        g_tm[n].on = 1;
    } else if (mode & (TM_OVFE | TM_CMPE | TM_ZRET)) {
        g_tm[n].next_cmp = 0;
        g_tm[n].next_us = now + tm_us(mode, to_ovf);
        g_tm[n].on = 1;
    }
}

void hw_timers_poll(uint64_t now)
{
    int n, guard;
    for (n = 0; n < 4; n++)
        for (guard = 0; guard < 64 && g_tm[n].on && now >= g_tm[n].next_us; guard++) {
            uint32_t a = T0_MODE + (uint32_t)n * 0x800, mode = REG(a);
            uint64_t at = g_tm[n].next_us;
            if (g_tm[n].next_cmp) {
                REG(a) = mode | TM_EQUF;
                if (mode & TM_CMPE)
                    kernel_raise_intc(INTC_TIM0 + n);
                tm_schedule(n, (mode & TM_ZRET) ? 0 : REG(T0_COUNT + (uint32_t)n * 0x800 + TM_COMP), at);
                if (!(mode & TM_ZRET) && g_tm[n].next_cmp)
                    g_tm[n].next_us = at + tm_us(mode, 0x10000u);   /* the same value: a whole turn later */
            } else {
                REG(a) = mode | TM_OVFF;
                if (mode & TM_OVFE)
                    kernel_raise_intc(INTC_TIM0 + n);
                tm_schedule(n, 0, at);
            }
        }
}

uint64_t hw_timers_next_us(void)
{
    uint64_t t = ~0ull;
    int n;
    for (n = 0; n < 4; n++)
        if (g_tm[n].on && g_tm[n].next_us < t)
            t = g_tm[n].next_us;
    return t;
}

uint32_t g_tm_reads[2];         /* status: reads of a timer's count, those with its event due */

static uint32_t timer_count(int n)
{
    /* What is due first: an overflow's handler counts it before the game adds the two. (Every read
     * polled the kernel for that; it is this timer's own event alone that the read must not
     * overtake.) */
    const uint64_t now = plat_time_us();
    g_tm_reads[0]++;
    if (g_tm[n].on && now >= g_tm[n].next_us) {
        g_tm_reads[1]++;
        kernel_poll();
        return tm_count_at(n, plat_time_us());
    }
    return tm_count_at(n, now);
}

/* a write to a timer's COUNT (0), MODE (0x10) or COMP (0x20) */
static void timer_write(uint32_t a, uint32_t v)
{
    int n = (int)((a & 0xFFFF) >> 11);
    uint64_t now = plat_time_us();
    uint32_t count = tm_count_at(n, now);
    switch (a & 0x7FF) {
    case 0x00:
        count = v & 0xFFFF;
        break;
    case 0x10:
        /* EQUF and OVFF: writing 1 clears them */
        REG(a) = (v & 0x3FFu) | (REG(a) & (TM_EQUF | TM_OVFF) & ~v);
        break;
    default:
        REG(a) = v & 0xFFFF;
        break;
    }
    tm_schedule(n, count, now);
}

static uint32_t ee_read32(uint32_t a)
{
    uint32_t off = a & 0xFFFF;
    if (off < 0x2000) {
        int n = off >> 11;
        if ((off & 0x7FF) == 0)
            return timer_count(n);
        return REG(a);
    }
    if (dma_channel(a) >= 0 && (a & 0xFF) == 0) {
        kernel_poll();
        return REG(a);
    }
    switch (a) {
    case D_STAT:
    case D_CTRL:
    case D_PCR:
    case D_SQWC:
    case D_RBSR:
    case D_RBOR:
    case D_ENABLER:
    case INTC_STAT:
    case INTC_MASK:
        kernel_poll();
        return REG(a);
    case VIF0_STAT:
    case VIF1_STAT:
        return REG(a) & ~0x1F000003u;         /* FIFOs empty, idle */
    /* IPU (MPEG decoder) stand-in: idle, and every bit-stream read sees a
     * sequence end code, so movies end at once. TODO: decode for FMVs. */
    case 0x10002000:                          /* IPU_CMD data */
    case 0x10002030:                          /* IPU_TOP */
        return 0x000001B7u;
    case 0x10002004:
    case 0x10002034:
        return 0;                             /* not busy */
    case 0x10002010:                          /* IPU_CTRL: no reset, not busy, no error */
        return REG(a) & ~0xC0004000u;
    case 0x10002020:                          /* IPU_BP: input FIFO empty */
        return 0;
    case GIF_STAT: {
        extern int g_gif_m3p, g_gif_p3_pending;
        uint32_t v = REG(a) & ~0x1F000C02u;
        if (g_gif_m3p)
            v |= 2u;
        if (g_gif_p3_pending)
            v |= 16u << 24;               /* FIFO full, waiting for the mask to drop */
        return v;
    }
    default:
        note_unknown(a, "read", REG(a));
        return REG(a);
    }
}

/* FIFO windows take 128-bit writes; collect 32-bit pieces into a qword */
static uint32_t g_fifo_acc[3][4];

static int fifo_write32(uint32_t a, uint32_t v)
{
    int f;
    u128 q;
    switch (a & ~0xFu) {
    case VIF0_FIFO: f = 0; break;
    case VIF1_FIFO: f = 1; break;
    case GIF_FIFO: f = 2; break;
    default: return 0;
    }
    g_fifo_acc[f][(a >> 2) & 3] = v;
    if ((a & 0xC) != 0xC)
        return 1;
    q = u128_make(g_fifo_acc[f][0], g_fifo_acc[f][1], g_fifo_acc[f][2], g_fifo_acc[f][3]);
    if (f == 0)
        vif0_fifo_write(q);
    else if (f == 1)
        vif1_fifo_write(q);
    else
        gif_fifo_write(q);
    return 1;
}

static void ee_write32(uint32_t a, uint32_t v)
{
    int ch = dma_channel(a);
    if (fifo_write32(a, v))
        return;
    if ((a & 0xFFFFE000u) == T0_COUNT && (a & 0x7FF) <= TM_COMP) {
        timer_write(a, v);
        return;
    }
    if (ch >= 0) {
        REG(a) = v;
        if ((a & 0xFF) == 0 && (v & 0x100u))
            dma_kick(ch);
        return;
    }
    switch (a) {
    case D_STAT: {
        uint32_t s = REG(a);
        s &= ~(v & 0xFFFFu);                 /* CIS bits: write 1 to clear */
        s ^= v & 0xFFFF0000u;                /* CIM bits: write 1 to toggle */
        REG(a) = s;
        return;
    }
    case INTC_STAT:
        REG(a) &= ~v;
        return;
    case INTC_MASK:
        REG(a) ^= v;
        return;
    case D_CTRL:
    case D_PCR:
    case D_SQWC:
    case D_RBSR:
    case D_RBOR:
    case D_ENABLEW:
        REG(a) = v;
        if (a == D_ENABLEW)
            REG(D_ENABLER) = v;
        return;
    case VIF1_FBRST:
    case VIF0_FBRST:
    case GIF_CTRL:
        REG(a) = 0;
        return;
    default:
        if ((a & 0xFFFF) < 0x2000) {
            REG(a) = v;
            return;
        }
        note_unknown(a, "write", v);
        REG(a) = v;
        return;
    }
}

/* ---- GS privileged ---------------------------------------------------------------- */

#define GS_PRIV(a) g_gs_priv[((a) & 0x1FFF) >> 3]

uint64_t gs_priv_read(uint32_t a)
{
    if ((a & 0x1FFF) == (GS_CSR & 0x1FFF)) {
        kernel_poll();
        return GS_PRIV(a) | 0x551B0000ull << 16;   /* revision/id like a SCPH-5xxxx */
    }
    return GS_PRIV(a);
}

uint32_t g_gs_flips, g_vif1_chains;

void gs_priv_write(uint32_t a, uint64_t v)
{
    if ((a & 0x1FFF) == (GS_CSR & 0x1FFF)) {
        /* SIGNAL/FINISH/HSINT/VSINT/EDWINT bits clear on write 1 */
        GS_PRIV(a) &= ~(v & 0x1Full);
        if (v & 0x200)
            gs_reset();
        return;
    }
    /* a new DISPFB is a new game frame on screen (frame rate in the logs) */
    if (((a & 0x1FFF) == 0x70 || (a & 0x1FFF) == 0x90) && GS_PRIV(a) != v)
        g_gs_flips++;
    GS_PRIV(a) = v;
    if ((a & 0x1FFF) == (GS_IMR & 0x1FFF))
        return;
}


void gs_set_csr_bits(uint32_t bits) { GS_PRIV(GS_CSR) |= bits; }
uint64_t gs_priv_reg(uint32_t a) { return GS_PRIV(a); }

void hw_vblank(void)
{
    GS_PRIV(GS_CSR) |= 0x8;                    /* VSINT */
    GS_PRIV(GS_CSR) ^= 0x2000;                 /* FIELD */
    if (!(GS_PRIV(GS_IMR) & 0x800))
        kernel_raise_intc(INTC_GS);
    gs_vsync();
}

/* ---- dispatch ---------------------------------------------------------------------- */

static int seg_vu(uint32_t a) { return (a >> 16) == 0x1100; }

/* EE store into VU memory: micro memory changes drop the recompiled-program lookups */
static void vu_store(uint32_t a)
{
    uint32_t off = a & 0xFFFF;
    if (off < 0x1000)
        vu_micro_written_at(0, off & ~15u, 16);
    else if (off >= 0x8000 && off < 0xC000) {
        vif_mpg_flush();
        vu_micro_written(1);
    }
}
static int seg_gs(uint32_t a) { return (a >> 16) == 0x1200; }

uint8_t io_read8(R5900 *c, uint32_t a)
{
    (void)c;
    if (seg_vu(a))
        return (uint8_t)(g_vu_mem[(a & 0xFFFF) >> 2] >> ((a & 3) * 8));
    return (uint8_t)(ee_read32(a & ~3u) >> ((a & 3) * 8));
}

uint16_t io_read16(R5900 *c, uint32_t a)
{
    (void)c;
    if (seg_vu(a))
        return (uint16_t)(g_vu_mem[(a & 0xFFFF) >> 2] >> ((a & 2) * 8));
    return (uint16_t)(ee_read32(a & ~3u) >> ((a & 2) * 8));
}

uint32_t io_read32(R5900 *c, uint32_t a)
{
    (void)c;
    if (seg_vu(a))
        return g_vu_mem[(a & 0xFFFF) >> 2];
    if (seg_gs(a))
        return (uint32_t)(gs_priv_read(a & ~7u) >> ((a & 4) * 8));
    return ee_read32(a);
}

uint64_t io_read64(R5900 *c, uint32_t a)
{
    if (seg_gs(a))
        return gs_priv_read(a);
    return J64(io_read32(c, a), io_read32(c, a + 4));
}

u128 io_read128(R5900 *c, uint32_t a)
{
    return u128_make(io_read32(c, a), io_read32(c, a + 4), io_read32(c, a + 8), io_read32(c, a + 12));
}

void io_write8(R5900 *c, uint32_t a, uint8_t v)
{
    (void)c;
    if (seg_vu(a)) {
        uint32_t *w = &g_vu_mem[(a & 0xFFFF) >> 2], sh = (a & 3) * 8;
        *w = (*w & ~(0xFFu << sh)) | (uint32_t)v << sh;
        vu_store(a);
        return;
    }
    note_unknown(a, "write8", v);
}

void io_write16(R5900 *c, uint32_t a, uint16_t v)
{
    (void)c;
    if (seg_vu(a)) {
        uint32_t *w = &g_vu_mem[(a & 0xFFFF) >> 2], sh = (a & 2) * 8;
        *w = (*w & ~(0xFFFFu << sh)) | (uint32_t)v << sh;
        vu_store(a);
        return;
    }
    note_unknown(a, "write16", v);
}

void io_write32(R5900 *c, uint32_t a, uint32_t v)
{
    (void)c;
    if (seg_vu(a)) {
        g_vu_mem[(a & 0xFFFF) >> 2] = v;
        vu_store(a);
        return;
    }
    if (seg_gs(a)) {
        uint64_t old = gs_priv_reg(a & ~7u);
        uint64_t nv = (a & 4) ? (old & 0xFFFFFFFFull) | ((uint64_t)v << 32) : (old & ~0xFFFFFFFFull) | v;
        gs_priv_write(a & ~7u, nv);
        return;
    }
    ee_write32(a, v);
}

void io_write64(R5900 *c, uint32_t a, uint64_t v)
{
    if (seg_gs(a)) {
        gs_priv_write(a, v);
        return;
    }
    io_write32(c, a, (uint32_t)v);
    io_write32(c, a + 4, (uint32_t)(v >> 32));
}

void io_write128(R5900 *c, uint32_t a, u128 v)
{
    if ((a & ~0xFu) == VIF1_FIFO) {
        vif1_fifo_write(v);
        return;
    }
    if ((a & ~0xFu) == GIF_FIFO) {
        gif_fifo_write(v);
        return;
    }
    if ((a & ~0xFu) == VIF0_FIFO) {
        vif0_fifo_write(v);
        return;
    }
    io_write32(c, a, v.w[0]);
    io_write32(c, a + 4, v.w[1]);
    io_write32(c, a + 8, v.w[2]);
    io_write32(c, a + 12, v.w[3]);
}

/* debugging (PC "watch" commands); only checked with PS2_DYNAMIC_IO */
uint32_t g_watch_val, g_watch_span;
uint32_t g_watch_addr = 1;      /* never matches an aligned address */

#if defined(PS2_DYNAMIC_IO)

void ps2_watch_hit(uint32_t a, uint32_t v)
{
    /* the first few hits per storing function (innermost frame), so a copy
     * loop does not use up the log */
    static struct { uint32_t fn, n; } seen[128];
    static int nseen;
    R5900 *c = kernel_current_ctx();
    uint32_t fn = c && c->depth ? c->callstack[(c->depth - 1) & 511] : 0;
    int k;
    for (k = 0; k < nseen && seen[k].fn != fn; k++)
        ;
    if (k == nseen) {
        if (nseen == 128)
            return;
        seen[nseen++].fn = fn;
    }
    if (seen[k].n++ >= 3)
        return;
    LOG("watch: store %08X to %08X", v, a);
    if (c)
        rt_dump_stack(c, "watch");
}
#endif

static void dyn_note(uint32_t a, const char *what)
{
    static uint64_t seen[2048];
    static int n;
    uint64_t key = a;
    int k;
#if defined(PS2_TRACE_CALLS)
    {
        R5900 *c = kernel_current_ctx();
        if (c && c->depth)
            key |= (uint64_t)c->callstack[(c->depth - 1) & 511] << 32;
    }
#endif
    for (k = 0; k < n; k++)
        if (seen[k] == key)
            return;
    if (n < 2048)
        seen[n++] = key;
#if defined(PS2_TRACE_CALLS)
    {
        R5900 *c = kernel_current_ctx();
        uint32_t fn = c && c->depth ? c->callstack[(c->depth - 1) & 511] : 0;
        LOG("dynamic I/O %s %08X in function %08X (IO_FUNCS)", what, a, fn);
    }
#else
    LOG("dynamic I/O %s %08X", what, a);
#endif
}

uint32_t io_dyn_read(uint32_t a, int size)
{
#if defined(PS2_DYNAMIC_IO)
    if ((a >> 28) != 1)                 /* the address watchpoint: plain RAM */
        return size == 4 ? *(uint32_t *)GPTR(a) : size == 2 ? *(uint16_t *)GPTR(a ^ PS2_HXOR)
                                                            : *(uint8_t *)GPTR(a ^ PS2_BXOR);
#endif
    dyn_note(a, "read");
    if (size == 4)
        return io_read32(NULL, a);
    if (size == 2)
        return io_read16(NULL, a);
    return io_read8(NULL, a);
}

float io_dyn_readf(uint32_t a)
{
    return BITS2F(io_dyn_read(a, 4));
}

void io_dyn_write(uint32_t a, uint32_t v, int size)
{
#if defined(PS2_DYNAMIC_IO)
    if ((a >> 28) != 1) {               /* the address watchpoint */
        R5900 *c = kernel_current_ctx();
        LOG("watch: %d-byte store %08X to %08X", size, v, a);
        if (c)
            rt_dump_stack(c, "watch");
        if (size == 4)
            *(uint32_t *)GPTR(a) = v;
        else if (size == 2)
            *(uint16_t *)GPTR(a ^ PS2_HXOR) = (uint16_t)v;
        else
            *(uint8_t *)GPTR(a ^ PS2_BXOR) = (uint8_t)v;
        return;
    }
#endif
    dyn_note(a, "write");
    if (size == 4)
        io_write32(NULL, a, v);
    else if (size == 2)
        io_write16(NULL, a, (uint16_t)v);
    else
        io_write8(NULL, a, (uint8_t)v);
}

void io_dyn_writef(uint32_t a, float v)
{
    io_dyn_write(a, F2BITS(v), 4);
}


void hw_init(void)
{
    memset(g_ee_regs, 0, sizeof g_ee_regs);
    memset(g_gs_priv, 0, sizeof g_gs_priv);
    GS_PRIV(GS_IMR) = 0x7F00;
    /* libkernl installs its alarm patch into the kernel (code copied from .data,
     * driven by timer 3) unless T3 is already counting. Our kernel implements
     * SetAlarm natively, so report the patch as present. */
    REG(0x10001810u) = 0x100;
    gs_init();
    vif_init();
}
