/*
 * Statically recompiled VU microprograms.
 *
 * tools/vu/vurecomp.py compiles the microcode images in the executable to
 * gen/vu_progs.c; runtime/vu.c runs a compiled program instead of the
 * interpreter when micro memory holds its code. A program works on local
 * copies of the registers and returns the state the interpreter keeps:
 * flags, a Q/P result still in flight, where MSCNT continues, or a request
 * to continue in the interpreter (a jump the compiler did not foresee).
 *
 * VU_EXACT builds the programs with the interpreter's float handling
 * (operands and results clamped to finite non-denormal values) so the two
 * can be compared bit for bit; without it they use plain float arithmetic.
 */
#ifndef VU_REC_H
#define VU_REC_H

#include "hw.h"
#include "gs.h"
#include "vu.h"

#include <float.h>
#include <math.h>

typedef struct VuCtx {
    float (*vf)[4];
    uint32_t *vi;
    float *acc, *q, *p, *i;
    uint32_t *r;
    uint32_t *mem;              /* data memory, host-order words */
    uint32_t mac, clip;         /* flags, in and out */
    float qn, pn;               /* out: results still in flight at the end */
    int q_left, p_left;         /* out: cycles until they land (0: none) */
    uint32_t tpc;               /* out: where the program stopped */
    int fallback;               /* out: 1 = continue in the interpreter at tpc */
} VuCtx;

typedef void (*VuFn)(VuCtx *ctx, uint32_t entry);

typedef struct VuRange { uint16_t addr, n; } VuRange;          /* n instructions from addr */
typedef struct VuEntry {
    uint16_t addr, nranges;
    const VuRange *ranges;
    /* VU0 routines compiled a second time to leave only what is read after them (fn_lean): the float
     * registers' lanes (x, y, z, w: a bit a register) and the integer registers it writes and does not
     * leave then; lean: there is such a version of this entry */
    uint32_t dead[4];
    uint16_t dead_vi;
    uint8_t lean;
} VuEntry;
typedef struct VuProg {
    uint8_t unit;
    uint32_t chain;             /* guest address of the DMA chain that uploads it */
    const uint32_t *code;       /* the image, as micro memory words from address 0 */
    uint16_t nentries;
    const VuEntry *entries;     /* entry points and the code a run from each can reach */
    VuFn fn;
    VuFn fn_lean;               /* VU0: the same with the lean versions of its entries, or 0 */
} VuProg;

extern const VuProg g_vu_progs[];
extern const int g_vu_nprogs;

void vu_rec_kick(const uint32_t *mem, uint32_t qaddr);
#define VU_XGKICK(mem, a) vu_rec_kick(mem, a)

/* VU_INL, F2U, U2F, vu_clamp, vu_mac1 and vu_div: vu_math.h (shared with the generated EE code) */
#include "vu_math.h"
float ps2_sqrtf(float x) __attribute__((const));

VU_INL int32_t vu_ftoi(float f)
{
    return f >= 2147483647.0f ? 0x7FFFFFFF : f <= -2147483648.0f ? (int32_t)0x80000000u : (int32_t)f;
}

#if defined(VU_EXACT)
#define VIN(x) vu_clamp(x)
#define VCL(x) vu_clamp(x)
#define VFTOI(x) vu_ftoi(x)
#else
#define VIN(x) (x)
#define VCL(x) (x)
#define VFTOI(x) ((int32_t)(x))
#endif

VU_INL uint32_t vu_mac(uint32_t dest, float x, float y, float z, float w)
{
    return (dest & 8 ? vu_mac1(x, 8) : 0) | (dest & 4 ? vu_mac1(y, 4) : 0) |
           (dest & 2 ? vu_mac1(z, 2) : 0) | (dest & 1 ? vu_mac1(w, 1) : 0);
}

/* x/0: +-max; 0/0: +-0 */
VU_INL float vu_rsqrt(float n, float d)
{
    n = vu_clamp(n);
    d = vu_clamp(d);
    if (d == 0.0f) {
        uint32_t sign = (F2U(n) ^ F2U(d)) & 0x80000000u;
        return U2F(n == 0.0f ? sign : sign | 0x7F7FFFFFu);
    }
    return n / ps2_sqrtf(fabsf(d));
}

/* EFU forms as PCSX2 evaluates them (zero stays zero) */
VU_INL float vu_rcp0(float s) { return s != 0.0f ? 1.0f / s : 0.0f; }
VU_INL float vu_rsqrt0(float s) { return s != 0.0f ? 1.0f / ps2_sqrtf(s) : 0.0f; }
VU_INL float vu_esqrt(float s) { return s >= 0.0f ? ps2_sqrtf(s) : s; }
VU_INL float vu_ersqrt(float s) { return s > 0.0f ? 1.0f / ps2_sqrtf(s) : s; }

/* EATAN adds pi/4: microcode passes (x-1)/(x+1) to get atan(x) */
static inline __attribute__((const)) float vu_eatan(float t)
{
    static const float c[9] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f,
                               -0.139085337519646f, 0.096420042216778f, -0.055909886956215f,
                               0.021861229091883f, -0.004054057877511f, 0.785398185253143f};
    float t2 = t * t, p = t, r = 0;
    int k;
    for (k = 0; k < 8; k++, p *= t2)
        r += c[k] * p;
    return vu_clamp(r + c[8]);
}

/* EATANxy / EATANxz: atan series of y/x, 0 when x is 0 */
static inline __attribute__((const)) float vu_eatan2(float y, float x)
{
    return x != 0.0f ? vu_eatan(y / x) : 0.0f;
}

static inline __attribute__((const)) float vu_esin(float x)
{
    static const float c[5] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f,
                               0.000002601886990f};
    float x2 = x * x, p = x, r = 0;
    int k;
    for (k = 0; k < 5; k++, p *= x2)
        r += c[k] * p;
    return vu_clamp(r);
}

static inline __attribute__((const)) float vu_eexp(float x)
{
    static const float c[6] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                               0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
    float p = 1.0f, xn = x;
    int k;
    for (k = 0; k < 6; k++, xn *= x)
        p += c[k] * xn;
    p = p * p;
    p = vu_clamp(p * p);
    return 1.0f / p;
}

#endif
