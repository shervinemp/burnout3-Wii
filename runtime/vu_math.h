/*
 * The VU's float helpers that both the microprogram runner (vu_rec.h, vu.c) and the generated EE code (the
 * inline VU0 entries, vu0_inline.h) use: one definition, so that a native entry run inside a recompiled function
 * rounds, clamps and sets its flags exactly as the same entry run through ps2_vu0_callms does. No macros but VU_INL.
 */
#ifndef VU_MATH_H
#define VU_MATH_H

#include <stdint.h>

/* small helpers must inline: flags most instructions compute are dead, and
 * only inlined code lets the compiler drop them */
#define VU_INL static inline __attribute__((always_inline))

typedef union { float f; uint32_t u; } VuF32;
VU_INL uint32_t F2U(float f) { VuF32 v; v.f = f; return v.u; }
VU_INL float U2F(uint32_t u) { VuF32 v; v.u = u; return v.f; }

/* VU operand/result: no infinities or NaNs (saturate), denormals are zero */
VU_INL float vu_clamp(float f)
{
    uint32_t u = F2U(f);
    if ((u & 0x7F800000u) == 0x7F800000u)
        return U2F((u & 0x80000000u) | 0x7F7FFFFFu);
    if ((u & 0x7F800000u) == 0)
        return U2F(u & 0x80000000u);
    return f;
}

/* MAC flags of an FMAC result: zero, sign and overflow per lane in dest */
VU_INL uint32_t vu_mac1(float r, uint32_t bit)
{
    uint32_t u = F2U(r), m = 0;
    if ((u & 0x7F800000u) == 0)
        m |= bit;
    if (u & 0x80000000u)
        m |= bit << 4;
    if ((u & 0x7FFFFFFFu) >= 0x7F7FFFFFu)
        m |= bit << 12;
    return m;
}

/* FDIV: x/0 is +-max */
VU_INL float vu_div(float n, float d)
{
    n = vu_clamp(n);
    d = vu_clamp(d);
    if (d == 0.0f)
        return U2F(((F2U(n) ^ F2U(d)) & 0x80000000u) | 0x7F7FFFFFu);
    return n / d;
}

#endif
