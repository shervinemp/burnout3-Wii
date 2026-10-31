/*
 * Native world pipelines (docs/native-renderer.md): per-vertex work the
 * microprograms do every frame, in C.
 */
#include "rt.h"
#include "gs.h"

#include <math.h>

#define V(n, c) BITS2F(vu[(n) * 4 + (c)])

extern int g_opt3;                      /* hle_render.c (autopilot "opt3 0": lit_rgba_c alone, lit_floor's search) */

static int32_t ftoi0(float f)
{
    if (f >= 2147483647.0f)
        return 0x7FFFFFFF;
    if (f <= -2147483648.0f)
        return (int32_t)0x80000000u;
    return (int32_t)f;
}

/* ftoi0 where fctiwz saturates the same way (NaN: 0x80000000 both): no compares */
static inline int32_t ftoi0_fast(float f)
{
#if defined(GEKKO)
    return (int32_t)f;
#else
    return ftoi0(f);
#endif
}

/* signed bytes as floats (indexed by the unsigned byte): no integer moved
 * through memory into a float register per component */
static float g_sbyte_f[256];

/* Lit world (470650, 472EB0): the vertex colour's alpha is a specular term,
 * which later passes use from the framebuffer. With 1013.z clear, per vertex
 * (normal n and intensity s as signed bytes at TOPS+204, four qwords for four
 * vertices: n.x, n.y, n.z, s of each in its lane):
 *   R = (n . d) * 0.000124 * n - d          (d = 1007.xyz, |n| ~ 127)
 *   x = max(R . (p - e) / |p - e|, 0.01 * 1007.w)       (e = 1008.xyz)
 *   alpha = max(s * 2 * 1008.w * x^1007.w, 1009.x)
 * The microprogram raises to the power with float bit patterns taken as
 * integers (a log2/exp2 approximation); so does this. */
void nw_lit_alpha(const uint32_t *vu, uint32_t n, const float (*pos)[3], const int8_t (*lit)[4], uint8_t *alpha)
{
    uint32_t k;
    float kx = V(1007, 0) * 0.000124000246f, ky = V(1007, 1) * 0.000124000246f, kz = V(1007, 2) * 0.000124000246f;
    float p = V(1007, 3), lo = p * 0.00999999978f, w2 = V(1008, 3) + V(1008, 3);
    float c = (float)(int32_t)F2BITS(w2) - p * (float)(int32_t)0x3F800000;
    if (vu[1013 * 4 + 2] & 0xFFFF) {
        for (k = 0; k < n; k++)
            alpha[k] = 0;
        return;
    }
    for (k = 0; k < n; k++) {
        const int8_t *l = lit[k & ~3u];
        uint32_t r = k & 3;
        float nx = l[0 * 4 + r], ny = l[1 * 4 + r], nz = l[2 * 4 + r], s = l[3 * 4 + r];
        float nd = nx * kx + ny * ky + nz * kz;
        float rx = nd * nx - V(1007, 0), ry = nd * ny - V(1007, 1), rz = nd * nz - V(1007, 2);
        float dx = pos[k][0] - V(1008, 0), dy = pos[k][1] - V(1008, 1), dz = pos[k][2] - V(1008, 2);
        float x = (rx * dx + ry * dy + rz * dz) * rt_rsqrtf(dx * dx + dy * dy + dz * dz), v;
        if (x < lo)
            x = lo;
        v = BITS2F((uint32_t)ftoi0(c + (float)(int32_t)F2BITS(x) * p)) * s;
        if (v < V(1009, 0))
            v = V(1009, 0);
        alpha[k] = (uint8_t)ftoi0(v);
    }
}

/* the microprogram's power: x^p times 2 * 1008.w, through the bit patterns (c holds the factor) */
static inline float lit_pow(float c, float p, float x)
{
    return BITS2F((uint32_t)ftoi0_fast(c + (float)(int32_t)F2BITS(x) * p));
}

/* A vertex with no highlight (x at its floor: most of them) has an alpha that
 * depends on its intensity byte alone: a table by that byte, for the last
 * few sets of the lighting's constants (a frame has six or so: with four
 * tables they were made 1800 times in 300 frames).
 *
 * x1: how far above the floor x may be and the alpha, a byte, is still the
 * table's for every intensity. The floor is a hundredth of the power's
 * exponent, 0.16 or so, and a vertex is above it wherever its reflected
 * light points within 80 degrees of the eye's line: half the world with the
 * sun behind the camera, each vertex through the root and the bit patterns
 * for an alpha that came out as the least one anyway. Found by halving the
 * interval between the floor and one, on the bit patterns (the power does
 * not fall as x rises, nor does an alpha). */
static NwLitFloor g_lit_floor[16];
static uint32_t g_lit_floor_turn, g_lit_floor_serial;
uint32_t g_lit_floor_made;              /* status line */

static const NwLitFloor *lit_floor(float c, float p, float lo, float klo, float amin)
{
    /* (the table found last first: a set of constants is in one table at most, so it is the one the search finds) */
    static NwLitFloor *last;
    const uint32_t cb = F2BITS(c), pb = F2BITS(p), ab = F2BITS(amin);
    NwLitFloor *f;
    int32_t fl[256];
    uint32_t k, a, b;
    if (last && last->kc == cb && last->kp == pb && last->kamin == ab && last->serial && g_opt3)
        return last;
    for (k = 0; k < 16; k++)
        if (g_lit_floor[k].serial && g_lit_floor[k].kc == cb && g_lit_floor[k].kp == pb && g_lit_floor[k].kamin == ab)
            return last = &g_lit_floor[k];
    g_lit_floor_turn = (g_lit_floor_turn + 1) & 15;
    last = f = &g_lit_floor[g_lit_floor_turn];
    f->kc = cb;
    f->kp = pb;
    f->kamin = ab;
    f->serial = ++g_lit_floor_serial;
    g_lit_floor_made++;
    for (k = 0; k < 256; k++) {
        float v = klo * (float)(int8_t)k;
        if (v < amin)
            v = amin;
        fl[k] = ftoi0_fast(v);
        a = (uint8_t)fl[k];
        f->a[k] = (uint8_t)(a >= 0x80 ? 255 : a * 2);
    }
    f->x1 = lo;
    if (lo > 0.0f && lo < 1.0f && p > 0.0f && klo >= 0.0f) {
        a = F2BITS(lo);
        b = 0x3F800000u;
        while (b - a > 1) {
            const uint32_t m = a + (b - a) / 2;
            const float pw = lit_pow(c, p, BITS2F(m));
            int same = pw >= 0.0f;
            for (k = 0; k < 256 && same; k++) {
                float v = pw * (float)(int8_t)k;
                if (v < amin)
                    v = amin;
                same = ftoi0_fast(v) == fl[k];
            }
            if (same)
                a = m;
            else
                b = m;
        }
        f->x1 = BITS2F(a);
    }
    /* (the tests below compare squares, and the root's last bits may differ from theirs) */
    f->x1q = f->x1 * 0.9999f;
    return f;
}

/* the constants' table for the microprogram's data now; NULL: unlit, or no floor to speak of */
const NwLitFloor *nw_lit_floor(const uint32_t *vu)
{
    const float p = V(1007, 3), lo = p * 0.00999999978f, w2 = V(1008, 3) + V(1008, 3), amin = V(1009, 0);
    const float c = (float)(int32_t)F2BITS(w2) - p * (float)(int32_t)0x3F800000;
    if ((vu[1013 * 4 + 2] & 0xFFFF) || !(lo > 0.0f))
        return NULL;
    return lit_floor(c, p, lo, lit_pow(c, p, lo), amin);
}

/* The reflected light R of a batch's vertices, as the lighting below makes
 * it: its least and greatest components (rb[0..2], rb[3..5]). It depends on
 * the normals and the light's direction (1007.xyz) alone. */
void nw_lit_rbounds(const uint32_t *vu, const NwBlock *b, float *rb)
{
    const uint32_t *lit = (const uint32_t *)GPTR(b->lit);
    const float kx = V(1007, 0) * 0.000124000246f, ky = V(1007, 1) * 0.000124000246f, kz = V(1007, 2) * 0.000124000246f;
    const float dx0 = V(1007, 0), dy0 = V(1007, 1), dz0 = V(1007, 2);
    uint32_t k, c;
    for (k = 0; k < b->n; k++) {
        const uint32_t *l = lit + (k & ~3u);
        const uint32_t sh = (k & 3) * 8;
        const float nx = (float)(int8_t)(l[0] >> sh), ny = (float)(int8_t)(l[1] >> sh), nz = (float)(int8_t)(l[2] >> sh);
        const float nd = nx * kx + ny * ky + nz * kz;
        const float r[3] = {nd * nx - dx0, nd * ny - dy0, nd * nz - dz0};
        for (c = 0; c < 3; c++) {
            if (!k || r[c] < rb[c])
                rb[c] = r[c];
            if (!k || r[c] > rb[3 + c])
                rb[3 + c] = r[c];
        }
    }
}

/* Is every vertex of a batch at its floor or within x1 of it, by the bounds
 * of its reflected light and of its positions: R . D is at most its greatest
 * value over both boxes, |D| at least the eye's distance from the positions'
 * box. Then the batch's colours are its vertex colours with the table's
 * alpha, the same every frame. */
int nw_lit_all_floor(const uint32_t *vu, const NwLitFloor *f, const float *rb, const float *bmin, const float *bmax)
{
    float s = 0.0f, t = 0.0f, d2 = 0.0f;
    uint32_t c;
    for (c = 0; c < 3; c++) {
        const float h = (bmax[c] - bmin[c]) * 0.5f, v = (bmax[c] + bmin[c]) * 0.5f - V(1008, c);
        const float lo = rb[c], hi = rb[3 + c], m = fabsf(lo) > fabsf(hi) ? fabsf(lo) : fabsf(hi);
        const float g = fabsf(v) - h;
        s += (v >= 0.0f ? hi * v : lo * v) + m * h;
        t += m * (fabsf(v) + h);
        if (g > 0.0f)
            d2 += g * g;
    }
    s += t * 1e-5f;                     /* what the sums of a vertex may be off by */
    if (s <= 0.0f)
        return 1;
    return s * s <= f->x1q * f->x1q * d2 * 0.998f;
}

/* those colours */
void nw_lit_floor_rgba(const NwBlock *b, const NwLitFloor *f, uint32_t *out)
{
    const uint32_t *lit = (const uint32_t *)GPTR(b->lit);
    const uint32_t n = b->n, col = b->col;
    uint32_t k;
    for (k = 0; k < n; k++)
        out[k] = (uint32_t)LB(col + 3 * k) << 24 | (uint32_t)LB(col + 3 * k + 1) << 16 | (uint32_t)LB(col + 3 * k + 2) << 8 |
                 f->a[(lit[(k & ~3u) + 3] >> ((k & 3) * 8)) & 0xFF];
}

/* The same per vertex, from the render data in guest memory (positions
 * stride 16, lighting data as unpacked: four words per four vertices, word j
 * holding component j of each), with the vertex colour (V3-8 RGB): RGBA8 as
 * GX takes it (alpha 0x80 = 1.0 becomes 255). (lit_rgba_c: the scalar way; nw_lit_rgba below.) */
static void __attribute__((noinline)) lit_rgba_c(const uint32_t *vu, const NwBlock *b, uint32_t *out)
{
    const float *pos = (const float *)GPTR(b->pos);
    const uint32_t *lit = (const uint32_t *)GPTR(b->lit);
    uint32_t k, n = b->n, col = b->col;
    float kx = V(1007, 0) * 0.000124000246f, ky = V(1007, 1) * 0.000124000246f, kz = V(1007, 2) * 0.000124000246f;
    float dx0 = V(1007, 0), dy0 = V(1007, 1), dz0 = V(1007, 2), ex = V(1008, 0), ey = V(1008, 1), ez = V(1008, 2);
    float p = V(1007, 3), lo = p * 0.00999999978f, w2 = V(1008, 3) + V(1008, 3), amin = V(1009, 0);
    float c = (float)(int32_t)F2BITS(w2) - p * (float)(int32_t)0x3F800000;
    /* the power's value at the floor: most vertices are at it (no highlight), and the way through the bit
     * patterns is three trips through memory between the float and integer registers */
    const float klo = BITS2F((uint32_t)ftoi0_fast(c + (float)(int32_t)F2BITS(lo) * p));
    /* At the floor, or as near it as makes no difference to the byte (the
     * table's x1), without the root: R.D <= x1 |D| is R.D <= 0 or its square
     * <= x1^2 D.D. */
    const int quick = lo > 0.0f;
    const NwLitFloor *ft;
    const uint8_t *fl;
    float lo2;
    int unlit = (vu[1013 * 4 + 2] & 0xFFFF) != 0;
    if (g_sbyte_f[255] == 0.0f)
        for (k = 0; k < 256; k++)
            g_sbyte_f[k] = (float)(int8_t)k;
    if (unlit) {
        for (k = 0; k < n; k++)
            out[k] = (uint32_t)LB(col + 3 * k) << 24 | (uint32_t)LB(col + 3 * k + 1) << 16 |
                     (uint32_t)LB(col + 3 * k + 2) << 8;
        return;
    }
    ft = lit_floor(c, p, lo, klo, amin);
    fl = ft->a;
    lo2 = ft->x1q * ft->x1q;
    for (k = 0; k < n; k++, pos += 4) {
        uint32_t a, rgb = (uint32_t)LB(col + 3 * k) << 24 | (uint32_t)LB(col + 3 * k + 1) << 16 |
                          (uint32_t)LB(col + 3 * k + 2) << 8;
        const uint32_t *l = lit + (k & ~3u);
        uint32_t sh = (k & 3) * 8;
        float nx = g_sbyte_f[(l[0] >> sh) & 0xFF], ny = g_sbyte_f[(l[1] >> sh) & 0xFF];
        float nz = g_sbyte_f[(l[2] >> sh) & 0xFF];
        float nd = nx * kx + ny * ky + nz * kz;
        float rx = nd * nx - dx0, ry = nd * ny - dy0, rz = nd * nz - dz0;
        float dx = pos[0] - ex, dy = pos[1] - ey, dz = pos[2] - ez;
        float rd = rx * dx + ry * dy + rz * dz, dd = dx * dx + dy * dy + dz * dz;
        if (quick && (rd <= 0.0f || rd * rd <= lo2 * dd)) {
            a = fl[(l[3] >> sh) & 0xFF];
        } else {
            float s = g_sbyte_f[(l[3] >> sh) & 0xFF], x = rd * rt_rsqrtf(dd), v;
            if (x > lo)
                v = BITS2F((uint32_t)ftoi0_fast(c + (float)(int32_t)F2BITS(x) * p)) * s;
            else if (x <= lo)
                v = klo * s;
            else
                v = BITS2F((uint32_t)ftoi0_fast(c + (float)(int32_t)F2BITS(x) * p)) * s;    /* a NaN, as it was */
            if (v < amin)
                v = amin;
            a = (uint8_t)ftoi0_fast(v);
            a = a >= 0x80 ? 255 : a * 2;
        }
        out[k] = rgb | a;
    }
}

/* The same in paired singles, two vertices a register (lane 0 the odd vertex k + 1, lane 1 the even k: the order
 * their bytes have in a host-order word), the quick test's sums by the operations the scalar loop above compiles to,
 * in its order and with its fusing (gcc's contraction there: nd = kz nz + (kx nx + ky ny), r = n nd - d, D.D = dz dz +
 * (dx dx + dy dy), R.D = rz dz + (rx dx + ry dy), each a multiply-add rounded once, as ps_madd is), and the normals'
 * bytes taken as floats by a quantised load (GQR 6: bytes with a sign, scale 0: exactly (float)(int8_t)). Four
 * vertices a turn. The vertex colour's three bytes of four vertices are three words read byte-reversed (lwbrx: the
 * guest's byte order) and shifted into place. A vertex at its floor gets its table's alpha; one above it gets its
 * colour alone, its R.D and D.D are stored (scr[v ^ 1] and scr[256 + (v ^ 1)]) and its place in out goes on a list
 * (from scr + 2048 bytes; the end is returned) for the C after it, which finishes it as the scalar loop does.
 *   K (float pairs): 0 kx, 8 ky, 16 kz, 24 dx0, 32 dy0, 40 dz0, 48 ex, 56 ey, 64 ez, 72 x1q^2, 80 zero.
 *   r4 positions (vertex 0), r5 the lighting words (host pointer), r6 out, r7 groups of four, r8 the table, r9 scr,
 *   r10 the colours (host pointer, 4-byte aligned). r26, r27: the pair's colours; r28..r30 the group's three words. */
#if defined(GEKKO)
uint32_t **nw_lit_ps_quads(const float *K, const float *pos, const uint32_t *lit, uint32_t *out, uint32_t nq,
                           const uint8_t *fl, float *scr, const uint8_t *colp);

/* a pair: the normals' components at ox, oy, oz of the group's words, the intensity bytes of k and k + 1 at sk, sk1,
 * rgb: what puts the pair's colours in r26 (k) and r27 (k + 1) */
#define NWL_PAIR(ox, oy, oz, sk, sk1, rgb) \
    "\tpsq_l 0," ox "(5),0,6\n" \
    "\tpsq_l 1," oy "(5),0,6\n" \
    "\tpsq_l 2," oz "(5),0,6\n" \
    "\tpsq_l 7,0(4),0,0\n" \
    "\tpsq_l 8,16(4),0,0\n" \
    "\tlfs 9,8(4)\n" \
    "\tlfs 10,24(4)\n" \
    "\tps_mul 3,15,1\n"               /* nd = ky ny */ \
    "\tps_merge00 11,8,7\n"           /* x */ \
    "\tps_merge11 12,8,7\n"           /* y */ \
    "\tps_madd 3,14,0,3\n"            /* + kx nx */ \
    "\tps_merge00 13,10,9\n"          /* z */ \
    "\tps_sub 11,11,20\n"             /* dx = x - ex */ \
    "\tps_madd 3,16,2,3\n"            /* + kz nz */ \
    "\tps_sub 12,12,21\n" \
    "\tps_sub 13,13,22\n" \
    "\tps_msub 4,0,3,17\n"            /* rx = nx nd - dx0 */ \
    "\tps_msub 5,1,3,18\n" \
    "\tps_msub 6,2,3,19\n" \
    "\tps_mul 7,12,12\n"              /* D.D = dy dy */ \
    "\tps_mul 8,5,12\n"               /* R.D = ry dy */ \
    "\tps_madd 7,11,11,7\n"           /* + dx dx */ \
    "\tps_madd 8,4,11,8\n"            /* + rx dx */ \
    "\tps_madd 7,13,13,7\n"           /* + dz dz */ \
    "\tps_madd 8,6,13,8\n"            /* + rz dz */ \
    "\tps_mul 9,8,8\n"                /* (R.D)^2 */ \
    "\tps_mul 10,23,7\n"              /* x1q^2 D.D */ \
    "\tps_cmpu0 0,8,24\n" \
    "\tps_cmpu1 1,8,24\n" \
    "\tps_cmpu0 5,9,10\n" \
    "\tps_cmpu1 6,9,10\n" \
    "\tcror 2,0,2\n"                  /* R.D <= 0 (a NaN: no) */ \
    "\tcror 6,4,6\n" \
    "\tcror 22,20,22\n"               /* (R.D)^2 <= x1q^2 D.D */ \
    "\tcror 26,24,26\n" \
    "\tcror 2,2,22\n"                 /* at the floor: k + 1 (cr0 eq), k (cr1 eq) */ \
    "\tcror 6,6,26\n" \
    "\tlbz 10," sk "(5)\n" \
    "\tlbz 11," sk1 "(5)\n" \
    rgb \
    "\tcrand 30,2,6\n" \
    "\tlbzx 10,8,10\n" \
    "\tlbzx 11,8,11\n" \
    "\tbne 7,3f\n" \
    "\tor 10,10,26\n" \
    "\tor 11,11,27\n" \
    "\tstw 10,0(6)\n" \
    "\tstw 11,4(6)\n" \
    "\tb 4f\n" \
    "3:\n"                            /* one or both above the floor */ \
    "\tpsq_st 8,0(9),0,0\n" \
    "\tpsq_st 7,1024(9),0,0\n" \
    "\tor 10,10,26\n" \
    "\tbt 6,1f\n" \
    "\tmr 10,26\n" \
    "\tstw 6,0(12)\n" \
    "\taddi 12,12,4\n" \
    "1:\n" \
    "\tstw 10,0(6)\n" \
    "\tor 11,11,27\n" \
    "\tbt 2,2f\n" \
    "\tmr 11,27\n" \
    "\taddi 0,6,4\n" \
    "\tstw 0,0(12)\n" \
    "\taddi 12,12,4\n" \
    "2:\n" \
    "\tstw 11,4(6)\n" \
    "4:\n"

/* the colours of a group's first pair (bytes 0-2 and 3-5 of its twelve: A = b0 b1 b2 b3, B = b4 .. b7, C = b8 .. b11
 * as big-endian words) and of its second (b6-b8, b9-b11) */
#define NWL_RGB01 \
    "\trlwinm 26,28,0,0,23\n"         /* b0 b1 b2 */ \
    "\trlwinm 27,29,24,8,23\n"        /* _ b4 b5 */ \
    "\trlwimi 27,28,24,0,7\n"         /* b3 */
#define NWL_RGB23 \
    "\trlwinm 26,30,16,16,23\n"       /* _ _ b8 */ \
    "\trlwimi 26,29,16,0,15\n"        /* b6 b7 */ \
    "\trlwinm 27,30,8,0,23\n"         /* b9 b10 b11 */

__asm__("\t.pushsection .text\n"
        "\t.align 2\n"
        "\t.globl nw_lit_ps_quads\n"
        "\t.type nw_lit_ps_quads, @function\n"
        "nw_lit_ps_quads:\n"
        "\tstwu 1,-144(1)\n"
        "\tstfd 14,16(1)\n\tstfd 15,24(1)\n\tstfd 16,32(1)\n\tstfd 17,40(1)\n\tstfd 18,48(1)\n\tstfd 19,56(1)\n"
        "\tstfd 20,64(1)\n\tstfd 21,72(1)\n\tstfd 22,80(1)\n\tstfd 23,88(1)\n\tstfd 24,96(1)\n"
        "\tstmw 26,104(1)\n"
        "\tlis 0,6\n"                     /* GQR 6: loads of bytes with a sign, scale 0 */
        "\tmtspr 918,0\n"
        "\tisync\n"
        "\tpsq_l 14,0(3),0,0\n\tpsq_l 15,8(3),0,0\n\tpsq_l 16,16(3),0,0\n\tpsq_l 17,24(3),0,0\n"
        "\tpsq_l 18,32(3),0,0\n\tpsq_l 19,40(3),0,0\n\tpsq_l 20,48(3),0,0\n\tpsq_l 21,56(3),0,0\n"
        "\tpsq_l 22,64(3),0,0\n\tpsq_l 23,72(3),0,0\n\tpsq_l 24,80(3),0,0\n"
        "\tmtctr 7\n"
        "\tmr 31,10\n"
        "\taddi 12,9,2048\n"              /* the list of vertices above their floor */
        "\tli 3,4\n"
        "\tli 7,8\n"
        "5:\n"
        /* the streams ahead into the cache (hints, only where the console's cache hints are on): a group is 64 bytes of
         * positions (two lines), 16 of lighting words and 12 of colours */
        "\tlis 11,g_dcbz_on@ha\n\tlwz 11,g_dcbz_on@l(11)\n\tcmpwi 11,0\n\tbeq 8f\n"
        "\taddi 0,4,128\n\tdcbt 0,0\n"
        "\taddi 0,4,160\n\tdcbt 0,0\n"
        "\taddi 0,31,48\n\tdcbt 0,0\n"
        "\taddi 0,5,48\n\tdcbt 0,0\n"
        "8:\n"
        "\tlwbrx 28,0,31\n"               /* the group's colours: twelve bytes in the guest's order */
        "\tlwbrx 29,31,3\n"
        "\tlwbrx 30,31,7\n"
        "\taddi 31,31,12\n"
        NWL_PAIR("2", "6", "10", "15", "14", NWL_RGB01)     /* vertices 4g, 4g + 1: bytes 3 and 2 of each word */
        "\taddi 4,4,32\n"
        "\taddi 6,6,8\n"
        "\taddi 9,9,8\n"
        NWL_PAIR("0", "4", "8", "13", "12", NWL_RGB23)      /* 4g + 2, 4g + 3: bytes 1 and 0 */
        "\taddi 4,4,32\n"
        "\taddi 6,6,8\n"
        "\taddi 9,9,8\n"
        "\taddi 5,5,16\n"
        "\tbdnz 5b\n"
        "\tmr 3,12\n"
        "\tlmw 26,104(1)\n"
        "\tlfd 14,16(1)\n\tlfd 15,24(1)\n\tlfd 16,32(1)\n\tlfd 17,40(1)\n\tlfd 18,48(1)\n\tlfd 19,56(1)\n"
        "\tlfd 20,64(1)\n\tlfd 21,72(1)\n\tlfd 22,80(1)\n\tlfd 23,88(1)\n\tlfd 24,96(1)\n"
        "\taddi 1,1,144\n"
        "\tblr\n"
        "\t.size nw_lit_ps_quads, .-nw_lit_ps_quads\n"
        "\t.popsection\n");
#endif

/* status line (gs_gx.c): lists through the paired loop, those checked against lit_rgba_c and found different (the
 * first difference turns the loop off), vertices above their floor */
uint32_t g_lit_ps_lists, g_lit_ps_checked, g_lit_ps_bad, g_lit_ps_slow;
static int g_lit_ps_off;

#if defined(GEKKO)
/* a vertex above its floor, from its R.D and D.D: the scalar loop's other branch */
static inline uint32_t lit_above(float rd, float dd, float s, float c, float p, float lo, float klo, float amin)
{
    float x = rd * rt_rsqrtf(dd), v;
    uint32_t a;
    if (x > lo)
        v = BITS2F((uint32_t)ftoi0_fast(c + (float)(int32_t)F2BITS(x) * p)) * s;
    else if (x <= lo)
        v = klo * s;
    else
        v = BITS2F((uint32_t)ftoi0_fast(c + (float)(int32_t)F2BITS(x) * p)) * s;    /* a NaN, as it was */
    if (v < amin)
        v = amin;
    a = (uint8_t)ftoi0_fast(v);
    return a >= 0x80 ? 255 : a * 2;
}

/* 0: not taken (lit_rgba_c does it) */
static int __attribute__((noinline)) lit_rgba_ps(const uint32_t *vu, const NwBlock *b, uint32_t *out)
{
    static float K[22] __attribute__((aligned(8)));
    static float scr[768] __attribute__((aligned(8)));     /* R.D, D.D, then the list (nw_lit_ps_quads) */
    const float *pos = (const float *)GPTR(b->pos);
    const uint32_t *lit = (const uint32_t *)GPTR(b->lit);
    uint32_t k, n = b->n, col = b->col, nq;
    float kx = V(1007, 0) * 0.000124000246f, ky = V(1007, 1) * 0.000124000246f, kz = V(1007, 2) * 0.000124000246f;
    float dx0 = V(1007, 0), dy0 = V(1007, 1), dz0 = V(1007, 2), ex = V(1008, 0), ey = V(1008, 1), ez = V(1008, 2);
    float p = V(1007, 3), lo = p * 0.00999999978f, w2 = V(1008, 3) + V(1008, 3), amin = V(1009, 0);
    float c = (float)(int32_t)F2BITS(w2) - p * (float)(int32_t)0x3F800000;
    const float klo = BITS2F((uint32_t)ftoi0_fast(c + (float)(int32_t)F2BITS(lo) * p));
    const NwLitFloor *ft;
    const uint8_t *fl;
    float lo2;
    /* (the colours' bytes read from one host pointer: not where the masked addresses would wrap) */
    if ((vu[1013 * 4 + 2] & 0xFFFF) || !(lo > 0.0f) || n < 4 || n > 256 || (b->pos & 15) || (col & 3) ||
        (col & PS2_RAM_MASK) + 3 * n > PS2_RAM_SIZE)
        return 0;
    if (g_sbyte_f[255] == 0.0f)
        for (k = 0; k < 256; k++)
            g_sbyte_f[k] = (float)(int8_t)k;
    ft = lit_floor(c, p, lo, klo, amin);
    fl = ft->a;
    lo2 = ft->x1q * ft->x1q;
    K[0] = K[1] = kx;
    K[2] = K[3] = ky;
    K[4] = K[5] = kz;
    K[6] = K[7] = dx0;
    K[8] = K[9] = dy0;
    K[10] = K[11] = dz0;
    K[12] = K[13] = ex;
    K[14] = K[15] = ey;
    K[16] = K[17] = ez;
    K[18] = K[19] = lo2;
    K[20] = K[21] = 0.0f;
    nq = n / 4;
    {
        /* (the colours from one host pointer: LB(col + i) is the host's byte at that address xor 3, RAM being 4-byte
         * aligned, and lwbrx of a word reads its four in that order) */
        uint32_t **end = nw_lit_ps_quads(K, pos, lit, out, nq, fl, scr, ps2_ram + (col & PS2_RAM_MASK));
        uint32_t **sl = (uint32_t **)(void *)((uint8_t *)scr + 2048);
        for (; sl < end; sl++) {
            uint32_t *o = *sl;
            const uint32_t v = (uint32_t)(o - out), *l = lit + (v & ~3u);
            g_lit_ps_slow++;
            *o |= lit_above(scr[v ^ 1], scr[256 + (v ^ 1)], g_sbyte_f[(l[3] >> ((v & 3) * 8)) & 0xFF], c, p, lo, klo,
                            amin);
        }
    }
    /* the last vertices of a batch not a multiple of four: the scalar loop's body */
    for (k = nq * 4, pos += 4 * k; k < n; k++, pos += 4) {
        uint32_t a, rgb = (uint32_t)LB(col + 3 * k) << 24 | (uint32_t)LB(col + 3 * k + 1) << 16 |
                          (uint32_t)LB(col + 3 * k + 2) << 8;
        const uint32_t *l = lit + (k & ~3u);
        uint32_t sh = (k & 3) * 8;
        float nx = g_sbyte_f[(l[0] >> sh) & 0xFF], ny = g_sbyte_f[(l[1] >> sh) & 0xFF];
        float nz = g_sbyte_f[(l[2] >> sh) & 0xFF];
        float nd = nx * kx + ny * ky + nz * kz;
        float rx = nd * nx - dx0, ry = nd * ny - dy0, rz = nd * nz - dz0;
        float dx = pos[0] - ex, dy = pos[1] - ey, dz = pos[2] - ez;
        float rd = rx * dx + ry * dy + rz * dz, dd = dx * dx + dy * dy + dz * dz;
        if (rd <= 0.0f || rd * rd <= lo2 * dd)
            a = fl[(l[3] >> sh) & 0xFF];
        else
            a = lit_above(rd, dd, g_sbyte_f[(l[3] >> sh) & 0xFF], c, p, lo, klo, amin);
        out[k] = rgb | a;
    }
    return 1;
}
#endif

/* The lit world's colours: the paired loop, its first 6000 lists and one in 64 after checked against the scalar
 * loop (any difference is logged and turns the paired loop off for good) */
void nw_lit_rgba(const uint32_t *vu, const NwBlock *b, uint32_t *out)
{
#if defined(GEKKO)
    if (g_opt3 && !g_lit_ps_off && lit_rgba_ps(vu, b, out)) {
        g_lit_ps_lists++;
        if (g_lit_ps_checked < 6000 || !(g_lit_ps_lists & 63)) {
            static uint32_t ref[256];
            uint32_t k;
            g_lit_ps_checked++;
            lit_rgba_c(vu, b, ref);
            for (k = 0; k < b->n && ref[k] == out[k]; k++)
                ;
            if (k < b->n) {
                g_lit_ps_bad++;
                g_lit_ps_off = 1;
                LOG("lit world: the paired loop's colour of vertex %u of %u is %08X, the scalar loop's %08X: paired loop "
                    "off", k, b->n, out[k], ref[k]);
                memcpy(out, ref, b->n * 4);
            }
        }
        return;
    }
#endif
    lit_rgba_c(vu, b, out);
}

/* ---- particles (VU1 0x46D090) ---------------------------------------------------------------------
 *
 * Read from the microprogram (addresses in it): 912 = (U0, Va, Vb, dU/dt),
 * 913 = (segments, life unit, half width, acceleration), 914 = (ox, oy, oz,
 * z scale), 915 the GIF tag, 916 + 6j the camera history (j = 0 now: three
 * matrix rows for x, y, z, the translation, (sx, sy, px, py), the time in x).
 * A particle lives while t0 - L < birth < t1 (0x0100: both ends of the
 * segment, L = ITOP times the life unit); its two points are the position at
 * the segment's times, y (and w, its floor) with the acceleration, y not
 * below w; each through its end's matrix, and dropped when either is outside
 * the view (0x02E0: no clipping). The quad is the two points in (x sx, y sy,
 * w) pushed apart by the half width along the normal of the plane through
 * the eye (0x0410), each corner then divided by its third component. */
#define SQ(n) ((const float *)(mem + (((n) & 1023u) << 2)))

/* 1 / x to a few parts in 10^8 (an estimate and one step): four of these a quad, where a division is 17 cycles */
static inline float spark_recip(float x)
{
#if defined(GEKKO)
    double e, d = x;
    float y;
    if (x == 0.0f)
        return 0.0f;
    __asm__("ps_res %0,%1" : "=f"(e) : "f"(d));
    y = (float)e;
    return y * (2.0f - x * y);
#else
    return x != 0.0f ? 1.0f / x : 0.0f;
#endif
}

static inline void spark_vertex(const float *o, const float *sc, float qx, float qy, float qz, float s, float t, NsVert *v)
{
    const float r = spark_recip(qz);
    v->x = o[0] + (qx * sc[2]) * r;
    v->y = o[1] + (qy * sc[3]) * r;
    v->z = o[2] + o[3] * r;
    v->s = s;
    v->t = t;
}

uint32_t nw_spark_segment(const uint32_t *mem, uint32_t top, uint32_t itop, uint32_t seg, NsVert *out)
{
    const float *k = SQ(913), *o = SQ(914), *uv = SQ(912);
    const float *m0 = SQ(916 + 6 * seg), *m1 = SQ(922 + 6 * seg), *s0 = m0 + 16, *s1 = m1 + 16;
    const float life = (float)(int32_t)(itop & 0x3FF) * k[1], width = k[2], h = 0.5f * k[3];
    const float t0 = m0[20], t1 = m1[20], dt = t0 - t1, t1b = t0 - dt, dead = t0 - life;
    const float u0 = uv[0] + (SQ(921)[0] - t0) * uv[3], u1 = uv[0] + (SQ(921)[0] - t1) * uv[3];
    uint32_t ngrp = itop & 15, g, j, c, n = 0;
    if (ngrp > 11)
        ngrp = 11;
    for (g = 0; g < ngrp; g++) {
        const float *grp = SQ(top + 9 * g);
        for (j = 0; j < 4; j++) {
            const float *p = grp + 4 + 8 * j, *v = p + 4;
            const float b = grp[j], a0 = t0 - b, a1 = t1b - b;
            float A[4], B[4], ca[4], cb[4];
            float x1, y1, z1, x2, y2, z2, dx, dy, dz, nx, ny, nz, l2, q, fx, fy, fz;
            NsVert *w;
            if (!(b - dead > 0.0f) || !(t1 - b > 0.0f))
                continue;
            for (c = 0; c < 4; c++) {
                const float gc = (c & 1) ? h : 0.0f, acc = p[c] + v[c] * a0;
                A[c] = acc + gc * (a0 * a0);
                B[c] = (acc - v[c] * dt) + gc * (a1 * a1);
            }
            if (A[1] < A[3])
                A[1] = A[3];
            if (B[1] < B[3])
                B[1] = B[3];
            for (c = 0; c < 4; c++) {
                ca[c] = ((m0[12 + c] + m0[c] * A[0]) + m0[4 + c] * A[1]) + m0[8 + c] * A[2];
                cb[c] = ((m1[12 + c] + m1[c] * B[0]) + m1[4 + c] * B[1]) + m1[8 + c] * B[2];
            }
            if (fabsf(ca[0]) > fabsf(ca[3]) || fabsf(ca[1]) > fabsf(ca[3]) || fabsf(ca[2]) > fabsf(ca[3]) ||
                fabsf(cb[0]) > fabsf(cb[3]) || fabsf(cb[1]) > fabsf(cb[3]) || fabsf(cb[2]) > fabsf(cb[3]))
                continue;
            x1 = ca[0] * s0[0];
            y1 = ca[1] * s0[1];
            z1 = ca[3];
            x2 = cb[0] * s1[0];
            y2 = cb[1] * s1[1];
            z2 = cb[3];
            dx = x2 - x1;
            dy = y2 - y1;
            dz = z2 - z1;
            nx = dy * z2 - y2 * dz;
            ny = dz * x2 - z2 * dx;
            nz = dx * y2 - x2 * dy;
            l2 = (nz * nz + nx * nx) + ny * ny;
            q = l2 != 0.0f ? width / sqrtf(l2) : 0.0f;
            fx = nx * q;
            fy = ny * q;
            fz = nz * q;
            w = out + 4 * n++;
            spark_vertex(o, s0, x1 + fx, y1 + fy, z1 + fz, u0, uv[1], w);
            spark_vertex(o, s0, x1 - fx, y1 - fy, z1 - fz, u0, uv[2], w + 1);
            spark_vertex(o, s1, x2 + fx, y2 + fy, z2 + fz, u1, uv[1], w + 2);
            spark_vertex(o, s1, x2 - fx, y2 - fy, z2 - fz, u1, uv[2], w + 3);
            if (n == NS_MAX_QUADS)
                return n;
        }
    }
    return n;
}

uint32_t nw_spark_run(const uint32_t *mem, uint32_t top, uint32_t itop, NsVert *buf, uint32_t cap,
                      void (*flush)(const NsVert *q, uint32_t n, void *ctx), void *ctx)
{
    const float *k = SQ(913), *o = SQ(914), *uv = SQ(912), *m[18];
    const float life = (float)(int32_t)(itop & 0x3FF) * k[1], width = k[2], h = 0.5f * k[3];
    float tm[18], u[18];
    uint32_t nseg = mem[913 * 4] & 0xFFFF, ngrp = itop & 15, g, j, s, e, c, n = 0, total = 0;
    if (nseg > 17)
        nseg = 17;
    if (ngrp > 11)
        ngrp = 11;
    for (s = 0; s <= nseg; s++) {
        m[s] = SQ(916 + 6 * s);
        tm[s] = m[s][20];
        u[s] = uv[0] + (SQ(921)[0] - tm[s]) * uv[3];
    }
    for (g = 0; g < ngrp; g++) {
        const float *grp = SQ(top + 9 * g);
        for (j = 0; j < 4; j++) {
            const float *p = grp + 4 + 8 * j, *v = p + 4;
            const float b = grp[j];
            float x[18], y[18], w[18];
            uint8_t st[18];             /* 0 not worked out, 1 in view, 2 outside */
            memset(st, 0, sizeof st);
            for (s = 0; s < nseg; s++) {
                float dx, dy, dz, nx, ny, nz, l2, q, fx, fy, fz;
                NsVert *out;
                if (!(b - (tm[s] - life) > 0.0f) || !(tm[s + 1] - b > 0.0f))
                    continue;
                for (e = s; e <= s + 1; e++) {
                    const float *me = m[e];
                    const float a = tm[e] - b, a2 = a * a;
                    float pt[4], cl[4];
                    if (st[e])
                        continue;
                    for (c = 0; c < 4; c++)
                        pt[c] = (p[c] + v[c] * a) + ((c & 1) ? h : 0.0f) * a2;
                    if (pt[1] < pt[3])
                        pt[1] = pt[3];
                    for (c = 0; c < 4; c++)
                        cl[c] = ((me[12 + c] + me[c] * pt[0]) + me[4 + c] * pt[1]) + me[8 + c] * pt[2];
                    st[e] = fabsf(cl[0]) > fabsf(cl[3]) || fabsf(cl[1]) > fabsf(cl[3]) || fabsf(cl[2]) > fabsf(cl[3]) ? 2 : 1;
                    x[e] = cl[0] * me[16];
                    y[e] = cl[1] * me[17];
                    w[e] = cl[3];
                }
                if (st[s] != 1 || st[s + 1] != 1)
                    continue;
                dx = x[s + 1] - x[s];
                dy = y[s + 1] - y[s];
                dz = w[s + 1] - w[s];
                nx = dy * w[s + 1] - y[s + 1] * dz;
                ny = dz * x[s + 1] - w[s + 1] * dx;
                nz = dx * y[s + 1] - x[s + 1] * dy;
                l2 = (nz * nz + nx * nx) + ny * ny;
                q = width * rt_rsqrtf(l2);      /* 0 for 0; sqrtf is a library call here, and was most of a quad's time */
                fx = nx * q;
                fy = ny * q;
                fz = nz * q;
                out = buf + 4 * n;
                spark_vertex(o, m[s] + 16, x[s] + fx, y[s] + fy, w[s] + fz, u[s], uv[1], out);
                spark_vertex(o, m[s] + 16, x[s] - fx, y[s] - fy, w[s] - fz, u[s], uv[2], out + 1);
                spark_vertex(o, m[s + 1] + 16, x[s + 1] + fx, y[s + 1] + fy, w[s + 1] + fz, u[s + 1], uv[1], out + 2);
                spark_vertex(o, m[s + 1] + 16, x[s + 1] - fx, y[s + 1] - fy, w[s + 1] - fz, u[s + 1], uv[2], out + 3);
                if (++n == cap) {
                    flush(buf, n, ctx);
                    total += n;
                    n = 0;
                }
            }
        }
    }
    if (n) {
        flush(buf, n, ctx);
        total += n;
    }
    return total;
}

/* ---- particle systems (VU1 0x46D750) --------------------------------------------------------------
 *
 * Smoke, dust, gravel, fire, glass and debris: a pool at a time, its header
 * at 1000..1016 and batches of at most 60 particles at TOP, three qwords
 * each: position and birth time; velocity and spin (in 1/512); four bytes
 * (shade, the shade's range, factors for alpha and size). Read from the
 * microprogram (addresses in it): a pool's set-up 0x0020 (the view's rows at
 * 1017..1020 with a guard of half the largest size around the screen, the
 * slopes of colour and size); the position at the particle's age and the
 * tests 0x0720 (alive: half the lifetime over |age - half|; inside the
 * guarded view: nothing is clipped); colour and size along the age in two
 * linear parts, the fade towards the near and far limits and the tests on
 * alpha and size 0x0950; the corners by kind from 0x0E58 (0 a sprite, 2 a
 * turned quad, 4 the same shaded at each corner as a ball would be, 5 a
 * tumbling shard; 3, snow, and 1, which nothing uses, are left to it). A
 * division by 0 there is the largest number, not infinity: it meets a 0 in
 * the products that follow. Products and sums stay two operations here as
 * they are there: a particle's depth is what is left of terms a thousand
 * times its size, and with fused ones it came out 1 in 10^4 apart, enough to
 * tip the tests on alpha and size for a particle at the limit. */
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
static inline float np_div(float a, float b)
{
    return b != 0.0f ? a / b : a < 0.0f ? -3.4028235e38f : 3.4028235e38f;
}

int nw_puff_setup(const uint32_t *mem, NpPool *p)
{
    const float *h7 = SQ(1007), *h8 = SQ(1008), *h9 = SQ(1009), *sc = SQ(1021), *vp = SQ(1022);
    const float W = sc[0], H = sc[1], vw = sc[2], vh = sc[3], S = h9[0], n = h8[2], f = h8[3];
    const float L = h7[1], kL = h7[2] * L, ikL = np_div(1.0f, kL), irest = np_div(1.0f, L - kL);
    const float gx = np_div(W, S + W), gy = np_div(vh * W, W * vh + S * vw);
    const float inf = np_div(1.0f, n - f), zA = (n + f) * inf, zB = (f * n + f * n) * inf;
    const float ivhW = np_div(1.0f, vh * W), ivv = np_div(1.0f, vw * vh), vwW = np_div(vw, W);
    const float iX = np_div(1.0f, h8[0]), iY = np_div(1.0f, h8[1]);
    uint32_t i, c;
    p->type = mem[1007 * 4] & 0xFFFF;
    if (p->type != 0 && p->type != 2 && p->type != 4 && p->type != 5)
        return 0;
    for (i = 0; i < 4; i++) {
        const float *r = SQ(1017 + i);
        p->R[i][0] = ((r[0] + r[0]) - r[3]) * gx;
        p->R[i][1] = ((r[1] + r[1]) - r[3]) * gy;
        p->R[i][2] = zA * r[3] - (i == 3 ? zB : 0.0f);
        p->R[i][3] = r[3];
    }
    p->half = L * 0.5f;
    p->kL = kL;
    p->now = h7[3];
    p->nowh = h7[3] - p->half;
    for (c = 0; c < 4; c++) {
        p->C1[c] = SQ(1001)[c];
        p->dCa[c] = (SQ(1001)[c] - SQ(1000)[c]) * ikL;
        p->dCb[c] = (SQ(1002)[c] - SQ(1001)[c]) * irest;
        p->lit[c] = SQ(1003)[c];
        p->dlit[c] = SQ(1004)[c] - SQ(1003)[c];
    }
    for (c = 0; c < 3; c++) {
        p->A[c] = SQ(1006)[c];
        p->Lc[c] = SQ(1005)[c];
        p->o[c] = vp[c];
    }
    p->s1 = h9[2];
    p->dsa = (h9[2] - h9[1]) * ikL;
    p->dsb = (h9[3] - h9[2]) * irest;
    p->niX = -iX;
    p->nXn = -iX * n;
    p->iY = iY;
    p->fY = iY * f;
    p->athr = SQ(1006)[3];
    p->smin = (SQ(1005)[3] + SQ(1005)[3]) * vwW;
    p->smax = (S + S) * vwW;
    p->Vx = (S + W) * 0.5f;
    p->Vy = H * 0.5f + (H * vw * S * 0.5f) * ivhW;
    p->Vz = vp[3];
    p->Px = (W * vh * 0.25f) * ivv;
    p->Py = (H * vw * 0.25f) * ivv;
    p->ex = vw + (S * vw * vh) * ivhW;
    p->ey = vh + (S * vw * vh) * ivhW;
    p->D = SQ(1010)[0];
    {
        /* a square root is a library call here, and every pool is set up, drawn or not */
        const float od = fabsf(1.0f - p->D);
        p->K5 = 0.69f * (od * rt_rsqrtf(od));
    }
    return 1;
}

/* 0x1780: sqrt 2 times the cosine of (1/2 - the fraction of x) turns. The
 * fraction is x less what x + 2^23 keeps of it, rounded down: whole numbers
 * from 0 up, halves below 0 (a spin of many turns backwards: its shard then
 * turns half the way round and jumps back, there as here) */
static inline float np_turn(float x)
{
    float fl, u, q;
    if (x >= 0.0f) {
        fl = (float)(int32_t)x;
    } else {
        const float x2 = x + x;
        fl = (float)(int32_t)x2;
        if (fl > x2)
            fl -= 1.0f;
        fl *= 0.5f;
    }
    u = 0.5f - (x - fl);
    q = u * u;
    return 1.41421354f + q * (-27.9154568f + q * (91.8381729f + q * (-120.854187f + q * 85.1987915f)));
}

/* a colour component as the GS takes it; the microprogram hands over the low
 * byte of the integer, so one over 255 would wrap there */
static inline uint32_t np_byte(float v)
{
    const int32_t i = (int32_t)v;
    return i < 0 ? 0 : i > 255 ? 255 : (uint32_t)i;
}

uint32_t nw_puff_batch(const uint32_t *mem, uint32_t top, uint32_t n, const NpPool *p, NpVert *out)
{
    static const float us[4] = {1.0f, 1.0f, 0.0f, 0.0f}, vs[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    uint32_t i, j, c, nq = 0;
    for (i = 0; i < n; i++) {
        const float *q0 = SQ(top + 3 * i);
        const int32_t *q1 = (const int32_t *)SQ(top + 3 * i + 1);
        const uint32_t *q2 = (const uint32_t *)SQ(top + 3 * i + 2);
        const float age = p->now - q0[3], d = age - p->kL, dn = d < 0.0f ? d : 0.0f, dp = d > 0.0f ? d : 0.0f;
        float pos[3], cl[4], col[4], dx[4], dy[4], t[4];
        float iw, nx, ny, fade, fw, alpha, sz, cx, cy, cz, hx, hy, r0, r1, C, S;
        NpVert *v = out + 4 * nq;
        if (!(p->half > fabsf(p->nowh - q0[3])))
            continue;                   /* 0x07D8: not born yet, or past its lifetime */
        {
            const float a2 = age * age;
            for (c = 0; c < 3; c++)
                pos[c] = (q0[c] + (float)q1[c] * (1.0f / 512.0f) * age) + p->A[c] * a2;
        }
        for (c = 0; c < 4; c++)
            cl[c] = ((p->R[3][c] + p->R[0][c] * pos[0]) + p->R[1][c] * pos[1]) + p->R[2][c] * pos[2];
        if (fabsf(cl[0]) > fabsf(cl[3]) || fabsf(cl[1]) > fabsf(cl[3]) || fabsf(cl[2]) > fabsf(cl[3]))
            continue;                   /* 0x07E0 */
        iw = np_div(1.0f, cl[3]);       /* the tests on size below are too close for an estimate's 1 in 10^5 */
        fade = p->nXn - p->niX * cl[3];
        fw = p->fY - p->iY * cl[3];
        if (fw < fade)
            fade = fw;
        if (fade > 1.0f)
            fade = 1.0f;
        alpha = (((p->C1[3] + p->dCa[3] * dn) + p->dCb[3] * dp) * ((float)q2[2] * (1.0f / 255.0f))) * fade;
        sz = (((float)q2[3] * (1.0f / 255.0f)) * iw) * ((p->s1 + p->dsa * dn) + p->dsb * dp);
        if (!(alpha > p->athr) || !(sz > p->smin))
            continue;                   /* 0x0A80 */
        if (sz > p->smax)
            sz = p->smax;
        nx = cl[0] * iw;
        ny = cl[1] * iw;
        cx = p->o[0] + p->Vx * nx;
        cy = p->o[1] + p->Vy * ny;
        cz = p->o[2] + p->Vz * iw;
        hx = p->Px * sz;
        hy = p->Py * sz;
        r0 = (float)q2[0] * (1.0f / 255.0f);
        r1 = (float)q2[1] * (1.0f / 255.0f);
        for (c = 0; c < 3; c++)
            col[c] = (p->C1[c] + p->dCa[c] * dn) + p->dCb[c] * dp;
        col[3] = alpha;
        if (p->type == 0) {
            /* a sprite: the corners of its rectangle in the order of the quads' */
            uint32_t rgba = 0;
            for (c = 0; c < 4; c++)
                rgba = rgba << 8 | np_byte(col[c] * (p->lit[c] + p->dlit[c] * r0));
            for (j = 0; j < 4; j++) {
                v[j].x = (j & 1) ? cx + hx : cx - hx;
                v[j].y = (j & 2) ? cy + hy : cy - hy;
                v[j].z = cz;
                v[j].s = (j & 1) ? 1.0f : 0.0f;
                v[j].t = (j & 2) ? 1.0f : 0.0f;
                v[j].rgba = rgba;
            }
            nq++;
            continue;
        }
        {
            const float a = ((float)q1[3] * (1.0f / 512.0f)) * age;
            C = np_turn(a + 63.375f);   /* the turn's cosine + sine */
            S = np_turn(a + 63.125f);   /* sine - cosine */
        }
        dx[0] = C;
        dy[0] = S;
        dx[1] = S;
        dy[1] = -C;
        dx[2] = -S;
        dy[2] = C;
        dx[3] = -C;
        dy[3] = -S;
        t[0] = t[1] = t[2] = t[3] = r0;
        if (p->type == 5) {
            dy[0] = C * S * S;          /* 0x1618 */
            dy[3] = -dy[0];
            t[0] = t[1] = t[2] = t[3] = r0 + r1 * (C * S);
        } else if (p->type == 4) {
            /* the light's direction in the frame of the ray to the particle (0x14B8) */
            /* (len - 1) / m2 there, which is 1 / (len + 1) without the 0 / 0 straight ahead */
            const float ex = nx * p->ex, ey = ny * p->ey, m2 = ey * ey + ex * ex, il = rt_rsqrtf(1.0f + m2);
            const float len = (1.0f + m2) * il, k = 1.0f / (len + 1.0f);
            const float l0 = p->Lc[0] * il, l1 = p->Lc[1] * il, l2 = p->Lc[2] * il, xy = -(ex * ey) * k;
            const float lx = (((l0 + ex * l2) + (ey * ey * k) * l0) + xy * l1) * p->K5;
            const float ly = (((l1 + ey * l2) + xy * l0) + (ex * ex * k) * l1) * p->K5;
            const float lz = (l2 - ex * l0) - ey * l1;
            for (j = 0; j < 4; j++)
                t[j] = r0 + ((p->D * lz + dy[j] * ly) + dx[j] * lx) * r1;
        }
        if (p->type == 4) {
            for (j = 0; j < 4; j++) {
                uint32_t rgba = 0;
                for (c = 0; c < 4; c++)
                    rgba = rgba << 8 | np_byte(col[c] * p->lit[c] + (col[c] * p->dlit[c]) * t[j]);
                v[j].rgba = rgba;
            }
        } else {
            uint32_t rgba = 0;
            for (c = 0; c < 4; c++)
                rgba = rgba << 8 | np_byte(col[c] * (p->lit[c] + p->dlit[c] * t[0]));
            v[0].rgba = v[1].rgba = v[2].rgba = v[3].rgba = rgba;
        }
        for (j = 0; j < 4; j++) {
            v[j].x = cx + hx * dx[j];
            v[j].y = cy + hy * dy[j];
            v[j].z = cz;
            v[j].s = us[j];
            v[j].t = vs[j];
        }
        nq++;
    }
    return nq;
}

/* ---- embers (VU1 0x47C6F0) -------------------------------------------------------------------------
 *
 * The sparks' particles again (the same batches: ITOP groups of four at TOP,
 * birth times, then position and velocity each), each as a small sprite where
 * it is now. Read from the microprogram (addresses in it): the set-up at 0
 * with an emitter's first batch (the view's rows at 1008..1011, their z the
 * depth for the test from 1009.z and 1008.z; 1012 to 1014 to the window; 1015
 * gravity and the time; 1016 the size and the lifetime; 1017 and 1018 the
 * colour at birth and at the end, between them by the age squared; 1019 the
 * colour's factor at the distance 1010.z; 1021 the tag; 1022 the UVs), MSCNT
 * (0x01A8) with the others. A particle is drawn while it is born and not
 * past its lifetime (0x0238), its position the sparks' (y not below w, its
 * floor), in view (0x02D8: nothing is clipped); its half size on the screen
 * falls with the square root of the distance (0x04B8). 1023.y more passes a
 * run, each a step of 1023.z later, are there too, but they take the colour
 * times the last particle's (0x0678: the register with 1020 in it is the
 * colour's scratch): the game asks for none, and none is done here. The
 * microprogram does not load a batch's second position (0x0230 loads the
 * register after it): that particle is drawn where the one read past the
 * end of the run before would be. stale: that position, to compare with it;
 * NULL: its own. */
int nw_ember_setup(const uint32_t *mem, NeState *e)
{
    const float zA = SQ(1009)[2], zB = SQ(1008)[2], far = SQ(1010)[2];
    const float iq = np_div(1.0f, SQ(1016)[3]), ip = far != 0.0f ? 1.0f / far : 0.0f;
    uint32_t i, c;
    for (i = 0; i < 4; i++) {
        const float *r = SQ(1008 + i);
        const float hx = r[0] - r[3] * 0.5f, hy = r[1] - r[3] * 0.5f;
        e->R[i][0] = hx + hx;
        e->R[i][1] = hy + hy;
        e->R[i][2] = i == 3 ? zA * r[3] + zB : zA * r[3];
        e->R[i][3] = r[3];
    }
    e->sx = SQ(1012)[0] * 0.5f;
    e->sy = SQ(1012)[1] * 0.5f;
    e->zs = SQ(1012)[2];
    e->ox = SQ(1013)[0] + SQ(1014)[0];
    e->oy = SQ(1013)[1] + SQ(1014)[1];
    e->oz = SQ(1013)[2];
    for (c = 0; c < 3; c++)
        e->G[c] = SQ(1015)[c] * 0.5f;
    e->G[3] = SQ(1015)[1] * 0.5f;
    e->hsx = (SQ(1016)[0] * SQ(1016)[2]) * e->sx;
    e->hsy = (SQ(1016)[1] * SQ(1016)[2]) * e->sy;
    for (c = 0; c < 4; c++) {
        e->c0[c] = SQ(1017)[c];
        e->slope[c] = ((SQ(1018)[c] - SQ(1017)[c]) * iq) * iq;
        e->k[c] = (SQ(1019)[c] - 1.0f) * ip;
        e->uv[c] = SQ(1022)[c];
    }
    return 1;
}

uint32_t nw_ember_run(const uint32_t *mem, uint32_t top, uint32_t itop, const NeState *e, const float *stale, NpVert *out)
{
    const float T = SQ(1015)[3], Td = T - SQ(1016)[3];
    uint32_t g, j, c, n = 0;
    if (itop > NE_GROUPS)
        itop = NE_GROUPS;
    for (g = 0; g < itop; g++) {
        const float *grp = SQ(top + 9 * g);
        for (j = 0; j < 4; j++) {
            const float *p = g == 0 && j == 1 && stale ? stale : grp + 4 + 8 * j, *v = grp + 8 + 8 * j;
            const float b = grp[j], ma = b - T, a2 = ma * ma;
            float pos[4], cl[4], w, P, Q, ax, ay, x0, y0, x1, y1, z;
            uint32_t rgba = 0;
            NpVert *o = out + 4 * n;
            if (!(ma < 0.0f) || b - Td < 0.0f)
                continue;
            for (c = 0; c < 4; c++)
                pos[c] = (p[c] - v[c] * ma) + e->G[c] * a2;
            if (pos[1] < pos[3])
                pos[1] = pos[3];
            for (c = 0; c < 4; c++)
                cl[c] = ((e->R[3][c] + e->R[0][c] * pos[0]) + e->R[1][c] * pos[1]) + e->R[2][c] * pos[2];
            if (fabsf(cl[0]) > fabsf(cl[3]) || fabsf(cl[1]) > fabsf(cl[3]) || fabsf(cl[2]) > fabsf(cl[3]))
                continue;
            w = cl[3];
            P = w != 0.0f ? 1.0f / w : 0.0f;
            Q = fabsf(w) * rt_rsqrtf(fabsf(w));
            ax = e->sx * cl[0];
            ay = e->sy * cl[1];
            x0 = e->ox + (ax - e->hsx * Q) * P;
            y0 = e->oy + (ay - e->hsy * Q) * P;
            x1 = e->ox + (ax + e->hsx * Q) * P;
            y1 = e->oy + (ay + e->hsy * Q) * P;
            z = e->oz + e->zs * P;
            for (c = 0; c < 4; c++)
                rgba = rgba << 8 | np_byte((1.0f + e->k[c] * w) * (e->c0[c] + e->slope[c] * a2));
            for (c = 0; c < 4; c++) {
                o[c].x = (c & 1) ? x1 : x0;
                o[c].y = (c & 2) ? y1 : y0;
                o[c].z = z;
                o[c].s = (c & 1) ? e->uv[2] : e->uv[0];
                o[c].t = (c & 2) ? e->uv[3] : e->uv[1];
                o[c].rgba = rgba;
            }
            n++;
        }
    }
    return n;
}
#pragma GCC pop_options
