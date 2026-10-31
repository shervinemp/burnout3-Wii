/*
 * Interface between recompiled R5900 code and the runtime.
 *
 * Guest memory model
 * ------------------
 * The 32 MB of PS2 RAM is one host buffer. Every aligned 32-bit guest word is
 * stored in host byte order, so lw/sw/lwc1/swc1 are plain host loads and
 * stores on either endianness; byte and halfword accesses flip the low address
 * bits on big-endian hosts (PS2_BXOR/PS2_HXOR). Any data copied in from
 * outside (disc reads, the ELF) goes through ps2_mem_write_bytes, which
 * applies the same layout.
 *
 * Guest addresses are masked to 25 bits, which folds the cached (0x0xxxxxxx),
 * uncached (0x2xxxxxxx) and uncached-accelerated (0x3xxxxxxx) views onto RAM
 * and the scratchpad (0x70000000, 16 KB) onto 0x0000-0x3FFF, which is EE
 * kernel memory a game never touches. Hardware registers are reached through the
 * io_read/io_write functions: the recompiler emits those calls whenever the address is
 * a known constant in segment 0x1xxxxxxx.
 */
#ifndef PS2_RECOMP_H
#define PS2_RECOMP_H

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define PS2_INLINE static inline __attribute__((always_inline))
#if defined(GEKKO)
/* hot recompiled functions (HOT_FUNCS). PS2_HOT_VARIANT (make XDEFS=-DPS2_HOT_VARIANT=n, work/hot_opts_run.sh) tries other
 * options on them alone. Measured 2026-10-17 on the race start: 1 (-O3) -0.2 ms against 0 (-O2), +57 KB, the same world
 * hashes and no fused multiply-add added; 5 (-Os) +0.16 ms and -130 KB; 2, 3, 4, 6 neutral or worse. 1 ships since
 * 2026-10-23 (the +57 KB is nothing against the 4.9 MB MEM1 has free; 0 stays as the A/B). */
#ifndef PS2_HOT_VARIANT
#define PS2_HOT_VARIANT 1
#endif
#if PS2_HOT_VARIANT == 0
#define PS2_HOT __attribute__((optimize("O2")))
#elif PS2_HOT_VARIANT == 1
#define PS2_HOT __attribute__((optimize("O3")))
#elif PS2_HOT_VARIANT == 2
#define PS2_HOT __attribute__((optimize("O2", "-fschedule-insns", "-fsched-pressure")))
#elif PS2_HOT_VARIANT == 3
#define PS2_HOT __attribute__((optimize("O2", "-fira-algorithm=priority")))
#elif PS2_HOT_VARIANT == 4
#define PS2_HOT __attribute__((optimize("O2", "-fno-gcse", "-fno-tree-pre")))
#elif PS2_HOT_VARIANT == 5
#define PS2_HOT __attribute__((optimize("Os")))
#elif PS2_HOT_VARIANT == 6
#define PS2_HOT __attribute__((optimize("O2", "-fno-schedule-insns2")))
#endif
#else
#define PS2_HOT
#endif
#define PS2_NOINLINE __attribute__((noinline))
#define PS2_LIKELY(x) __builtin_expect(!!(x), 1)
#define PS2_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define PS2_INLINE static __forceinline
#define PS2_NOINLINE __declspec(noinline)
#define PS2_LIKELY(x) (x)
#define PS2_UNLIKELY(x) (x)
#define PS2_HOT
#endif

/* ---- register file ------------------------------------------------------------------ */

typedef struct u128 { uint32_t w[4]; } u128;

PS2_INLINE u128 u128_make(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r;
    r.w[0] = a; r.w[1] = b; r.w[2] = c; r.w[3] = d;
    return r;
}

typedef struct R5900 {
    uint32_t r[32][4];      /* GPRs, lane 0 = bits 0..31 */
    uint32_t hi[4], lo[4];  /* lanes 0-1: HI/LO, lanes 2-3: HI1/LO1 */
    uint32_t sa;            /* shift amount in bits, for QFSRV */
    float f[32];
    float facc;
    uint32_t fcc;           /* FPU condition bit */
    float vf[32][4];        /* VU0: vf0 is (0,0,0,1) and never written */
    float vacc[4];
    float vq, vI, vp;
    uint32_t vr;            /* R register (random) */
    uint32_t vi[16];
    uint32_t cop0[32];
    /* runtime-private */
    void *thread;
    uint32_t iop;           /* context runs IOP code */
    uint32_t depth;
#if defined(PS2_TRACE_CALLS)
    uint32_t callstack[512];
#endif
} R5900;

typedef void (*PS2Func)(R5900 *c);
typedef struct PS2FuncEntry { uint32_t addr; PS2Func fn; } PS2FuncEntry;
extern const PS2FuncEntry ps2_func_table[];
extern const unsigned ps2_func_count;

/* Code patches chosen at run time (tools/recomp/patches.py, the title config's CODE_PATCHES): the code at a
 * patch's sites runs the patched instructions while the set's bit is set (runtime/patches.c). */
extern uint32_t g_code_patch;

/* ---- guest memory -------------------------------------------------------------------- */

#define PS2_RAM_SIZE  0x02000000u
#define PS2_RAM_MASK  0x01FFFFFFu

#if defined(PS2_RAM_FIXED_BASE)
#define ps2_ram ((uint8_t *)(uintptr_t)PS2_RAM_FIXED_BASE)
#else
extern uint8_t *ps2_ram;
#endif

#if (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) || defined(__BIG_ENDIAN__)
#define PS2_HOST_BE 1
#define PS2_BXOR 3u
#define PS2_HXOR 2u
#else
#define PS2_HOST_BE 0
#define PS2_BXOR 0u
#define PS2_HXOR 0u
#endif

#if defined(PS2_IOP)
/* IOP modules: 2 MB of IOP RAM with the same word layout; hardware above
 * 0x1F800000 in any segment always goes through the runtime */
extern uint8_t *iop_ram;
#define GPTR(a)   (iop_ram + ((uint32_t)(a) & 0x001FFFFFu))
#define PS2_IS_IO(a) PS2_UNLIKELY(((uint32_t)(a) & 0x1FFFFFFFu) >= 0x1F800000u)
#define io_dyn_read iop_io_read
#define io_dyn_write iop_io_write
#define io_dyn_readf iop_io_readf
#define io_dyn_writef iop_io_writef
#define io_read8(c, a) ((uint8_t)iop_io_read((a), 1))
#define io_read16(c, a) ((uint16_t)iop_io_read((a), 2))
#define io_read32(c, a) iop_io_read((a), 4)
#define io_write8(c, a, v) iop_io_write((a), (v), 1)
#define io_write16(c, a, v) iop_io_write((a), (v), 2)
#define io_write32(c, a, v) iop_io_write((a), (v), 4)
#define ps2_call iop_call
#ifndef PS2_DYNAMIC_IO
#define PS2_DYNAMIC_IO 1
#endif
#else
#define GPTR(a)   (ps2_ram + ((uint32_t)(a) & PS2_RAM_MASK))
#endif

/* Hardware reached through a pointer (an address computed at run time) must
 * go through the runtime. Checking every access costs speed and code size,
 * so the check is compiled only where PS2_FN_IO is 1: in debug builds
 * (PS2_DYNAMIC_IO) everywhere, otherwise in the functions the title config
 * lists as touching hardware (IO_FUNCS), around which the emitter sets it.
 * Elsewhere the condition folds to 0 and accesses go straight to RAM. */
#if !defined(PS2_IOP)
#define PS2_IS_IO(a) PS2_UNLIKELY(((uint32_t)(a) >> 28) == 1u)
#endif
#if defined(PS2_DYNAMIC_IO)
#define PS2_FN_IO 1
#else
#define PS2_FN_IO 0
#endif
#if defined(PS2_DYNAMIC_IO) && !defined(PS2_IOP)
extern uint32_t g_watch_addr;   /* debug: stores to this word go through io_dyn_write and are logged */
#define PS2_CHK(a) (PS2_IS_IO(a) || PS2_UNLIKELY(((uint32_t)(a) & ~3u) == g_watch_addr))
#else
#define PS2_CHK(a) (PS2_FN_IO && PS2_IS_IO(a))
#endif
uint32_t io_dyn_read(uint32_t a, int size);
void io_dyn_write(uint32_t a, uint32_t v, int size);
float io_dyn_readf(uint32_t a);
void io_dyn_writef(uint32_t a, float v);
#define LW(a)     (PS2_CHK(a) ? io_dyn_read((a), 4) : *(uint32_t *)GPTR(a))
#define LWF(a)    (PS2_CHK(a) ? io_dyn_readf(a) : *(float *)GPTR(a))
#define LH(a)     (PS2_CHK(a) ? (uint16_t)io_dyn_read((a), 2) : *(uint16_t *)GPTR((uint32_t)(a) ^ PS2_HXOR))
#define LB(a)     (PS2_CHK(a) ? (uint8_t)io_dyn_read((a), 1) : *(uint8_t *)GPTR((uint32_t)(a) ^ PS2_BXOR))
#if defined(PS2_DYNAMIC_IO) && !defined(PS2_IOP)
/* debug builds: a value watchpoint on 32-bit stores, for values in
 * [g_watch_val, g_watch_val + g_watch_span) ("watch" and "watchrange" commands;
 * a range of positive floats is a range of bit patterns) */
extern uint32_t g_watch_val, g_watch_span;
void ps2_watch_hit(uint32_t a, uint32_t v);
#define PS2_WATCH(a, v) (PS2_UNLIKELY((uint32_t)(v) - g_watch_val < g_watch_span) ? ps2_watch_hit((a), (v)) : (void)0)
#else
#define PS2_WATCH(a, v) ((void)0)
#endif
/* PS2_TRACK (a price test, off by default): every store to guest RAM also marks the RAM's PS2_TRACK_SHIFT-bit page in
 * g_wdirty, so a reader of guest memory could ask "written since I looked?" instead of reading it again. 1 stores a
 * constant (a flag the reader clears); 2 stores g_wtick (a stamp: readers bump the tick and compare). Nothing reads the
 * table yet: this prices the stores. */
#if defined(PS2_TRACK) && !defined(PS2_IOP)
#ifndef PS2_TRACK_SHIFT
#define PS2_TRACK_SHIFT 12
#endif
extern uint8_t g_wdirty[PS2_RAM_SIZE >> PS2_TRACK_SHIFT];
extern uint8_t g_wtick;
#if PS2_TRACK == 2
#define PS2_MARK_V g_wtick
#else
#define PS2_MARK_V 1
#endif
#define PS2_MARK(a) (g_wdirty[((uint32_t)(a) & PS2_RAM_MASK) >> PS2_TRACK_SHIFT] = PS2_MARK_V)
#else
#define PS2_MARK(a) ((void)0)
#endif
#define SW(a, v)  (PS2_CHK(a) ? io_dyn_write((a), (uint32_t)(v), 4) : (PS2_WATCH((a), (v)), PS2_MARK(a), (void)(*(uint32_t *)GPTR(a) = (uint32_t)(v))))
#define SWF(a, v) (PS2_CHK(a) ? io_dyn_writef((a), (v)) : (PS2_WATCH((a), F2BITS(v)), PS2_MARK(a), (void)(*(float *)GPTR(a) = (v))))
#define SH(a, v)  (PS2_CHK(a) ? io_dyn_write((a), (uint16_t)(v), 2) : (PS2_MARK(a), (void)(*(uint16_t *)GPTR((uint32_t)(a) ^ PS2_HXOR) = (uint16_t)(v))))
#define SB(a, v)  (PS2_CHK(a) ? io_dyn_write((a), (uint8_t)(v), 1) : (PS2_MARK(a), (void)(*(uint8_t *)GPTR((uint32_t)(a) ^ PS2_BXOR) = (uint8_t)(v))))

/* copy host bytes in/out of guest memory honouring the word layout */
void ps2_mem_write_bytes(uint32_t addr, const void *src, uint32_t n);
void ps2_mem_read_bytes(void *dst, uint32_t addr, uint32_t n);

/* ---- scalar helpers ------------------------------------------------------------------- */

#define SX(x)       ((uint32_t)((int32_t)(x) >> 31))
#define J64(lo, hi) (((uint64_t)(uint32_t)(hi) << 32) | (uint32_t)(lo))

PS2_INLINE float BITS2F(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
PS2_INLINE uint32_t F2BITS(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }

PS2_INLINE void ps2_div(uint32_t a, uint32_t b, uint32_t *q, uint32_t *r)
{
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (PS2_UNLIKELY(sb == 0)) {
        *q = sa < 0 ? 1u : 0xFFFFFFFFu;
        *r = a;
    } else if (PS2_UNLIKELY(sa == INT32_MIN && sb == -1)) {
        *q = a;
        *r = 0;
    } else {
        *q = (uint32_t)(sa / sb);
        *r = (uint32_t)(sa % sb);
    }
}

PS2_INLINE void ps2_divu(uint32_t a, uint32_t b, uint32_t *q, uint32_t *r)
{
    if (PS2_UNLIKELY(b == 0)) {
        *q = 0xFFFFFFFFu;
        *r = a;
    } else {
        *q = a / b;
        *r = a % b;
    }
}

PS2_INLINE uint32_t ps2_plzcw(uint32_t x)
{
    uint32_t n = 0;
    if (x & 0x80000000u)
        x = ~x;
    x <<= 1;
    while (n < 31 && !(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

/* unaligned access (little-endian guest semantics) */
PS2_INLINE uint32_t ps2_lwl(uint32_t a, uint32_t rt)
{
    static const uint32_t mask[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0};
    uint32_t s = a & 3, mem = LW(a & ~3u);
    return (rt & mask[s]) | (mem << (24 - 8 * s));
}

PS2_INLINE uint32_t ps2_lwr(uint32_t a, uint32_t rt)
{
    static const uint32_t mask[4] = {0, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
    uint32_t s = a & 3, mem = LW(a & ~3u);
    return (rt & mask[s]) | (mem >> (8 * s));
}

PS2_INLINE void ps2_swl(uint32_t a, uint32_t rt)
{
    static const uint32_t mask[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0};
    uint32_t s = a & 3, al = a & ~3u;
    SW(al, (rt >> (24 - 8 * s)) | (LW(al) & mask[s]));
}

PS2_INLINE void ps2_swr(uint32_t a, uint32_t rt)
{
    static const uint32_t mask[4] = {0, 0xFFu, 0xFFFFu, 0xFFFFFFu};
    uint32_t s = a & 3, al = a & ~3u;
    SW(al, (rt << (8 * s)) | (LW(al) & mask[s]));
}

PS2_INLINE uint64_t ps2_ld64(uint32_t a) { return J64(LW(a), LW(a + 4)); }
PS2_INLINE void ps2_sd64(uint32_t a, uint64_t v) { SW(a, (uint32_t)v); SW(a + 4, (uint32_t)(v >> 32)); }

PS2_INLINE uint64_t ps2_ldl(uint32_t a, uint64_t rt)
{
    uint32_t s = a & 7;
    uint64_t mem = ps2_ld64(a & ~7u);
    uint64_t mask = s == 7 ? 0 : (0x00FFFFFFFFFFFFFFull >> (8 * s));
    return (rt & mask) | (mem << (56 - 8 * s));
}

PS2_INLINE uint64_t ps2_ldr(uint32_t a, uint64_t rt)
{
    uint32_t s = a & 7;
    uint64_t mem = ps2_ld64(a & ~7u);
    uint64_t mask = s == 0 ? 0 : (0xFFFFFFFFFFFFFFFFull << (64 - 8 * s));
    return (rt & mask) | (mem >> (8 * s));
}

PS2_INLINE void ps2_sdl(uint32_t a, uint64_t rt)
{
    uint32_t s = a & 7, al = a & ~7u;
    uint64_t mask = s == 7 ? 0 : (0xFFFFFFFFFFFFFF00ull << (8 * s));
    ps2_sd64(al, (rt >> (56 - 8 * s)) | (ps2_ld64(al) & mask));
}

PS2_INLINE void ps2_sdr(uint32_t a, uint64_t rt)
{
    uint32_t s = a & 7, al = a & ~7u;
    uint64_t mask = s == 0 ? 0 : (0xFFFFFFFFFFFFFFFFull >> (64 - 8 * s));
    ps2_sd64(al, (rt << (8 * s)) | (ps2_ld64(al) & mask));
}

/* ---- FPU (EE semantics: no infinities from division or roots) ------------------------- */

PS2_INLINE float ps2_fdiv(float a, float b)
{
    if (PS2_UNLIKELY(b == 0.0f)) {
        uint32_t s = (F2BITS(a) ^ F2BITS(b)) & 0x80000000u;
        return BITS2F(s | 0x7F7FFFFFu);
    }
    return a / b;
}

float ps2_sqrtf(float x) __attribute__((const));   /* host-tuned square root (PowerPC 750 has no fsqrt) */

PS2_INLINE float ps2_fsqrt(float x) { return ps2_sqrtf(fabsf(x)); }

PS2_INLINE float ps2_frsqrt(float a, float b)
{
    float r = ps2_sqrtf(fabsf(b));
    return ps2_fdiv(a, r);
}

/* MAX/MIN on the EE FPU and the VUs compare the raw bits as sign-magnitude
 * integers (no denormal flush or clamping; code uses them on bit patterns). */
PS2_INLINE uint32_t fp_max(uint32_t a, uint32_t b)
{
    return ((int32_t)a < 0 && (int32_t)b < 0) ? ((int32_t)a < (int32_t)b ? a : b) : ((int32_t)a > (int32_t)b ? a : b);
}
PS2_INLINE uint32_t fp_min(uint32_t a, uint32_t b)
{
    return ((int32_t)a < 0 && (int32_t)b < 0) ? ((int32_t)a > (int32_t)b ? a : b) : ((int32_t)a < (int32_t)b ? a : b);
}
PS2_INLINE float ps2_fmax(float a, float b) { return BITS2F(fp_max(F2BITS(a), F2BITS(b))); }
PS2_INLINE float ps2_fmin(float a, float b) { return BITS2F(fp_min(F2BITS(a), F2BITS(b))); }

PS2_INLINE int32_t ps2_cvtws(float x)
{
    if (x >= 2147483647.0f)
        return INT32_MAX;
    if (x <= -2147483648.0f)
        return INT32_MIN;
    return (int32_t)x;
}

PS2_INLINE float ps2_itof0(float x) { return (float)(int32_t)F2BITS(x); }
PS2_INLINE float ps2_itof4(float x) { return (float)(int32_t)F2BITS(x) * (1.0f / 16.0f); }
PS2_INLINE float ps2_itof12(float x) { return (float)(int32_t)F2BITS(x) * (1.0f / 4096.0f); }
PS2_INLINE float ps2_itof15(float x) { return (float)(int32_t)F2BITS(x) * (1.0f / 32768.0f); }
PS2_INLINE float ps2_ftoi0(float x) { return BITS2F((uint32_t)ps2_cvtws(x)); }
PS2_INLINE float ps2_ftoi4(float x) { return BITS2F((uint32_t)ps2_cvtws(x * 16.0f)); }
PS2_INLINE float ps2_ftoi12(float x) { return BITS2F((uint32_t)ps2_cvtws(x * 4096.0f)); }
PS2_INLINE float ps2_ftoi15(float x) { return BITS2F((uint32_t)ps2_cvtws(x * 32768.0f)); }

PS2_INLINE uint32_t ps2_vu_rnext(uint32_t r)
{
    uint32_t x = (r >> 4) & 1, y = (r >> 22) & 1;
    r = (r << 1) ^ (x ^ y);
    return (r & 0x7FFFFFu) | 0x3F800000u;
}

/* ---- MMI ---------------------------------------------------------------------------------- */
#include "ps2_mmi.h"

/* ---- runtime services --------------------------------------------------------------------- */

/* function-entry breadcrumbs for debugging hangs (PC builds) */
#if defined(PS2_TRACE_CALLS)
extern uint32_t ps2_trace_ring[256];
extern uint32_t ps2_trace_pos;
extern int ps2_calltrace_n;             /* "calltrace N": log the next N entries */
void ps2_calltrace_log(R5900 *c, uint32_t a);
static inline R5900 *ps2_trace_push(R5900 *c, uint32_t a)
{
    ps2_trace_ring[ps2_trace_pos++ & 255] = a;
    if (ps2_calltrace_n)
        ps2_calltrace_log(c, a);
    c->callstack[c->depth++ & 511] = a;
    return c;
}
static inline void ps2_trace_pop(R5900 **c) { (*c)->depth--; }
#define PS2_TRACE_ENTRY(a) R5900 *_trc __attribute__((cleanup(ps2_trace_pop))) = ps2_trace_push(c, (a))
#else
#define PS2_TRACE_ENTRY(a) ((void)0)
#endif

/* Profiling builds count guest instructions executed per function (index into
 * ps2_func_table); see the "prof" command of the PC host. */
#if defined(PS2_PROFILE)
extern uint64_t g_prof[];
#define PS2_PROF(id, n) (g_prof[id] += (n))
#else
#define PS2_PROF(id, n) ((void)0)
#endif

/* Debug builds report float arithmetic that yields Inf/NaN, which the EE FPU
 * and the VUs never produce (they saturate at +-0x7F7FFFFF). */
#if defined(PS2_FPU_CHECK)
float ps2_fchk(float f, uint32_t pc);
#define PS2_FCHK(x, pc) ps2_fchk((x), (pc))
#else
#define PS2_FCHK(x, pc) (x)
#endif

void ps2_call(R5900 *c, uint32_t addr);                 /* indirect call/jump */

/* Loops poll for interrupts at their backward branches: guest code that
 * spins on memory an interrupt handler will change (RenderWare's DMA queue,
 * vsync counters) must let the handler run. A host ticker and every raised
 * interrupt set the flag; the check costs one load per iteration. */
extern volatile int g_poll_request;
void ps2_poll(void);
#define PS2_POLL() do { if (__builtin_expect(g_poll_request, 0)) ps2_poll(); } while (0)
void ps2_dbg_enter(R5900 *c, uint32_t addr);            /* DEBUG_WRAP logging */
void ps2_dbg_leave(R5900 *c, uint32_t addr);
void ps2_syscall(R5900 *c, uint32_t pc);
void ps2_break(R5900 *c, uint32_t pc, uint32_t code);
void ps2_trap(R5900 *c, uint32_t pc);
void ps2_bad_insn(R5900 *c, uint32_t pc);
void ps2_bad_jump(R5900 *c, uint32_t pc, uint32_t target);
uint32_t ps2_cop0_count(R5900 *c);
void ps2_cop0_status_written(R5900 *c, uint32_t status);
int ps2_cpcond0(R5900 *c);

void ps2_vu0_callms(R5900 *c, uint32_t addr);
void ps2_vu0_callmsr(R5900 *c);
void ps2_vu0_ctc2(R5900 *c, uint32_t reg, uint32_t v);
uint32_t ps2_vu0_cfc2(R5900 *c, uint32_t reg);
void ps2_vu0_clip(R5900 *c, float x, float y, float z, float w);
void ps2_vu0_unhandled(R5900 *c, uint32_t pc, const char *what);

#if !defined(PS2_IOP)
uint8_t io_read8(R5900 *c, uint32_t a);
uint16_t io_read16(R5900 *c, uint32_t a);
uint32_t io_read32(R5900 *c, uint32_t a);
uint64_t io_read64(R5900 *c, uint32_t a);
u128 io_read128(R5900 *c, uint32_t a);
void io_write8(R5900 *c, uint32_t a, uint8_t v);
void io_write16(R5900 *c, uint32_t a, uint16_t v);
void io_write32(R5900 *c, uint32_t a, uint32_t v);
void io_write64(R5900 *c, uint32_t a, uint64_t v);
void io_write128(R5900 *c, uint32_t a, u128 v);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS2_RECOMP_H */
