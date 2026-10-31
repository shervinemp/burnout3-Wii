/* Dispatch between recompiled functions and fault reporting. */
#include "rt.h"

#include <stdio.h>
#include <string.h>

#if defined(PS2_TRACE_CALLS)
uint32_t ps2_trace_ring[256];
uint32_t ps2_trace_pos;
#endif

void rt_dump_stack(R5900 *c, const char *what)
{
#if defined(PS2_TRACE_CALLS)
    uint32_t k, d = c->depth;
    char line[256];
    int n = 0;
    LOG("%s call stack (depth %u, innermost last):", what, d);
    for (k = d > 64 ? d - 64 : 0; k < d; k++) {
        n += snprintf(line + n, sizeof line - (size_t)n, " %08X", c->callstack[k & 511]);
        if (n > 200 || k + 1 == d) {
            LOG("%s", line);
            n = 0;
        }
    }
#else
    (void)c; (void)what;
#endif
}

int ps2_calltrace_n;            /* debugging; only used with PS2_TRACE_CALLS */

#if defined(PS2_TRACE_CALLS)

/* one line per function entry: depth, address, a0..a3 (work/calltrace.txt) */
void ps2_calltrace_log(R5900 *c, uint32_t a)
{
    static FILE *f;
    if (!f)
        f = fopen("work/calltrace.txt", "w");
    if (f) {
        fprintf(f, "%*s%08X %08X %08X %08X %08X\n", (int)(c->depth & 63) * 2, "", a, c->r[4][0],
                c->r[5][0], c->r[6][0], c->r[7][0]);
        if (--ps2_calltrace_n == 0) {
            fclose(f);
            f = NULL;
            LOG("calltrace: written");
        }
    }
}
#endif

void rt_dump_trace(void)
{
#if defined(PS2_TRACE_CALLS)
    uint32_t k, p = ps2_trace_pos;
    void hw_dump_state(void);
    hw_dump_state();
    kernel_dump_threads();
    char line[256];
    LOG("last function entries (oldest first):");
    for (k = 0; k < 256; k += 8) {
        uint32_t j;
        int n = 0;
        for (j = 0; j < 8; j++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %08X", ps2_trace_ring[(p + k + j) & 255]);
        LOG("%s", line);
    }
#endif
}

/* Indirect calls (ps2_call, ~1000s a frame through the game's dispatchers)
 * find their function in a direct-mapped cache before the binary search over
 * the whole table (14 steps, each a cache miss on the Wii). */
#define LOOKUP_CACHE 1024
static struct { uint32_t addr; PS2Func fn; } g_lookup[LOOKUP_CACHE];

/* one address that is answered by a host function though no guest function is there (the runtime's checks use it to
 * stand in for a callback); found only after the table's search has failed, so it costs the ordinary lookups nothing */
static uint32_t g_stub_addr;
static PS2Func g_stub_fn;

void rt_set_stub(uint32_t addr, PS2Func fn)
{
    g_stub_addr = addr;
    g_stub_fn = fn;
}

PS2Func rt_lookup(uint32_t addr)
{
    /* functions start on 8-byte boundaries, mostly; fold in the higher bits */
    unsigned lo = 0, hi = ps2_func_count, slot = ((addr >> 3) ^ (addr >> 13)) & (LOOKUP_CACHE - 1);
    if (g_lookup[slot].addr == addr && g_lookup[slot].fn)
        return g_lookup[slot].fn;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t a = ps2_func_table[mid].addr;
        if (a == addr) {
            g_lookup[slot].addr = addr;
            g_lookup[slot].fn = ps2_func_table[mid].fn;
            return ps2_func_table[mid].fn;
        }
        if (a < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return addr == g_stub_addr ? g_stub_fn : NULL;
}

const char *rt_func_name(uint32_t addr)
{
    static char buf[4][16];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof buf[k], "f_%08X", addr);
    return buf[k];
}

static void dump_regs(R5900 *c)
{
    static const char *n[32] = {"zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
                                "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                                "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    int i;
    for (i = 0; i < 32; i += 4)
        LOG("  %s=%08X %s=%08X %s=%08X %s=%08X", n[i], c->r[i][0], n[i + 1], c->r[i + 1][0],
            n[i + 2], c->r[i + 2][0], n[i + 3], c->r[i + 3][0]);
}

extern volatile int g_irq_flag;

void ps2_call(R5900 *c, uint32_t addr)
{
    PS2Func fn;
    static int countdown;
    /* indirect calls double as interrupt delivery points, so busy loops that
     * call through pointers still see their interrupt handlers run */
    if (g_irq_flag || --countdown <= 0) {
        countdown = 512;
        kernel_poll();
    }
    fn = rt_lookup(addr);
    if (!fn && addr >= 0x80000000u) {
        static int logged;
        if (logged++ < 8)
            LOG("call into kernel memory %08X (ra %08X) ignored", addr, c->r[31][0]);
        c->r[2][0] = 0;
        c->r[2][1] = 0;
        return;
    }
    if (!fn) {
        LOG("call to unknown address %08X (ra %08X)", addr, c->r[31][0]);
        dump_regs(c);
        FATAL("call to unknown address %08X", addr);
    }
    fn(c);
}

void iop_call(R5900 *c, uint32_t addr);

void rt_run_guest(R5900 *c, uint32_t addr)
{
    if (c->iop)
        iop_call(c, addr);
    else
        ps2_call(c, addr);
}

void ps2_break(R5900 *c, uint32_t pc, uint32_t code)
{
    LOG("break %u at %08X", code, pc);
    dump_regs(c);
    rt_dump_stack(c, "break");
    FATAL("guest break at %08X", pc);
}

void ps2_trap(R5900 *c, uint32_t pc)
{
    LOG("trap at %08X", pc);
    dump_regs(c);
    FATAL("guest trap at %08X", pc);
}

void ps2_bad_insn(R5900 *c, uint32_t pc)
{
    dump_regs(c);
    FATAL("untranslatable instruction executed at %08X", pc);
}

void ps2_bad_jump(R5900 *c, uint32_t pc, uint32_t target)
{
    dump_regs(c);
    FATAL("jump table at %08X: target %08X not in table", pc, target);
}

/* The 750 has no fsqrt, and newlib's sqrtf works bit by bit (a tenth of a
 * race frame went there). The paired-single estimate y (1/sqrt x to 12 bits)
 * and two corrections of x * y in double precision (s + (x - s * s) * y / 2,
 * each one the square of the estimate's error: about 35 bits from 12) give the
 * root to far below a float's last place; truncated to float (the FPU runs in
 * round-toward-zero, as the EE does) it is within one step of the exact
 * result, which the last check settles: the largest float whose square is at
 * most x (float squares are exact in double). Two dependent operations fewer
 * than two Newton steps on y and then x * y; right for an estimate off by up to
 * 1 / 1024 (the 750's is within 1 / 4096), tested with squares of floats too. */
/* d - t * t as one fnmsub (the compiler made it an fneg and an fmadd: a dependent operation more) */
#if defined(GEKKO)
#define SQ_RESID(d, t) ({ double r_; __asm__("fnmsub %0,%1,%1,%2" : "=f"(r_) : "f"(t), "f"(d)); r_; })
#endif
float ps2_sqrtf(float x)
{
#if defined(GEKKO)
    double d = x, y, hy, t;
    float s, n;
    if (!(x > 0.0f))
        return x == x ? 0.0f : x;
    __asm__("ps_rsqrte %0,%1" : "=f"(y) : "f"(d));
    hy = 0.5 * y;
    t = d * y;
    t = t + SQ_RESID(d, t) * hy;
    t = t + SQ_RESID(d, t) * hy;
    s = (float)t;
    if ((double)s * (double)s > d)
        return BITS2F(F2BITS(s) - 1);
    n = BITS2F(F2BITS(s) + 1);
    return (double)n * (double)n <= d ? n : s;
#else
    return sqrtf(x);
#endif
}

/* startup check of ps2_sqrtf: s * s <= x < next(s)^2 over a spread of inputs */
void ps2_sqrtf_selftest(void)
{
#if defined(GEKKO)
    uint32_t k, bad = 0, b = 0x00800000u;
    for (k = 0; k < 20000; k++, b += 0x0003F9A7u) {
        /* every other input the square of a float of 12 bits (its root is a float: the case that needs the last check) */
        float x = BITS2F(b & 0x7F7FFFFFu), s, n;
        if (k & 1) {
            float f = BITS2F((((b >> 9) % 120 + 60) << 23) | (b & 0x7FF000u));
            x = f * f;
        }
        s = ps2_sqrtf(x);
        n = BITS2F(F2BITS(s) + 1);
        if (x < 1.2e-38f)
            continue;
        if (!((double)s * s <= (double)x && (double)n * n > (double)x)) {
            if (bad < 4)
                LOG("sqrt self-test: sqrt(%08X) = %08X", F2BITS(x), F2BITS(s));
            bad++;
        }
    }
    if (bad)
        LOG("sqrt self-test: %u of 20000 wrong", bad);
#endif
}
