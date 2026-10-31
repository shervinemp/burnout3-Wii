/* The SPU2's effect unit, the reverb (runtime/iop_sd.c; tested on the host by tools/iop/revtest.c).
 *
 * The presets, coefficients and algorithm are the ones libsd writes into the chip (ps2sdk
 * iop/sound/libsd/src/effect.c) and PCSX2 runs (pcsx2/SPU2/Reverb.cpp): the work area at ESA/EEA, the
 * same/diff/comb/APF delays. Each side runs at half the rate (the left on even samples, the right on
 * odd): its input is the average of its last two samples, and its output is held between its steps
 * and averaged across them.
 *
 * rev_step is the algorithm one sample at a time, as it was written first: fourteen places in the work
 * area, each an offset from the delay line's base taken round its end. rev_run does the same over a
 * chunk with the places as pointers that move one sample a pair, between the points where one of them
 * (or the base) comes round the end; so a sample is its arithmetic and its loads and stores, not the
 * places' sums and the side's choice of fields (~290 instructions a sample on Broadway before). Its
 * loads and stores are in rev_step's order, sample by sample: the two sides' places can be a few samples
 * apart (hall: the left's second APF is written 4 samples before the right's first is read), so they
 * cannot be done side by side. Its results are rev_step's bit for bit. See CREDITS.md for the attribution. */
#ifndef IOP_REV_H
#define IOP_REV_H

#include <stdint.h>

typedef struct Reverb {
    int mode;
    int on;
    int16_t iir_vol, comb1_vol, comb2_vol, comb3_vol, comb4_vol;
    int16_t wall_vol, apf1_vol, apf2_vol, in_coef_l, in_coef_r;
    uint32_t apf1_size, apf2_size;
    uint32_t same_l_src, same_r_src, diff_l_src, diff_r_src;
    uint32_t same_l_dst, same_r_dst, diff_l_dst, diff_r_dst;
    uint32_t comb1_l_src, comb1_r_src, comb2_l_src, comb2_r_src;
    uint32_t comb3_l_src, comb3_r_src, comb4_l_src, comb4_r_src;
    uint32_t apf1_l_dst, apf1_r_dst, apf2_l_dst, apf2_r_dst;
    uint32_t apf1_l_src, apf1_r_src, apf2_l_src, apf2_r_src;
    uint32_t same_l_prv, same_r_prv, diff_l_prv, diff_r_prv;
    uint32_t esa, eea, start, len, base;
    int16_t in_prev[2], in_cur[2], out_prev[2], out_cur[2];
} Reverb;

static inline int32_t rev_mul(int32_t a, int32_t b)
{
    return (a * b) >> 15;
}

/* an offset already reduced modulo len, added to the delay line's base (one subtract) */
static inline uint32_t rev_at(const Reverb *r, uint32_t base, uint32_t offset)
{
    uint32_t x = base + offset;
    if (x >= r->len)
        x -= r->len;
    return r->start + x;
}

/* one sample (cycle: its number, even for the left side's step) */
static void rev_step(Reverb *r, int16_t *mem, uint64_t cycle, int16_t in_l, int16_t in_r, int16_t *out_l, int16_t *out_r)
{
    const int ch = (int)(cycle & 1);
    int32_t in, same, diff, out, apf1, apf2;
    uint32_t same_src, same_dst, same_prv, diff_src, diff_dst, diff_prv;
    uint32_t comb1, comb2, comb3, comb4, apf1_src, apf1_dst, apf2_src, apf2_dst;

    r->in_prev[0] = r->in_cur[0];
    r->in_prev[1] = r->in_cur[1];
    r->in_cur[0] = in_l;
    r->in_cur[1] = in_r;
    in = rev_mul(ch ? r->in_coef_r : r->in_coef_l, (r->in_cur[ch] + r->in_prev[ch]) >> 1);
    same_src = rev_at(r, r->base, ch ? r->same_r_src : r->same_l_src);
    same_dst = rev_at(r, r->base, ch ? r->same_r_dst : r->same_l_dst);
    same_prv = rev_at(r, r->base, ch ? r->same_r_prv : r->same_l_prv);
    diff_src = rev_at(r, r->base, ch ? r->diff_l_src : r->diff_r_src);
    diff_dst = rev_at(r, r->base, ch ? r->diff_r_dst : r->diff_l_dst);
    diff_prv = rev_at(r, r->base, ch ? r->diff_r_prv : r->diff_l_prv);
    comb1 = rev_at(r, r->base, ch ? r->comb1_r_src : r->comb1_l_src);
    comb2 = rev_at(r, r->base, ch ? r->comb2_r_src : r->comb2_l_src);
    comb3 = rev_at(r, r->base, ch ? r->comb3_r_src : r->comb3_l_src);
    comb4 = rev_at(r, r->base, ch ? r->comb4_r_src : r->comb4_l_src);
    apf1_src = rev_at(r, r->base, ch ? r->apf1_r_src : r->apf1_l_src);
    apf1_dst = rev_at(r, r->base, ch ? r->apf1_r_dst : r->apf1_l_dst);
    apf2_src = rev_at(r, r->base, ch ? r->apf2_r_src : r->apf2_l_src);
    apf2_dst = rev_at(r, r->base, ch ? r->apf2_r_dst : r->apf2_l_dst);

    same = rev_mul(r->iir_vol, in + rev_mul(r->wall_vol, mem[same_src]) - mem[same_prv]) + mem[same_prv];
    diff = rev_mul(r->iir_vol, in + rev_mul(r->wall_vol, mem[diff_src]) - mem[diff_prv]) + mem[diff_prv];
    out = rev_mul(r->comb1_vol, mem[comb1]) + rev_mul(r->comb2_vol, mem[comb2]) +
          rev_mul(r->comb3_vol, mem[comb3]) + rev_mul(r->comb4_vol, mem[comb4]);
    apf1 = out - rev_mul(r->apf1_vol, mem[apf1_src]);
    out = mem[apf1_src] + rev_mul(r->apf1_vol, apf1);
    apf2 = out - rev_mul(r->apf2_vol, mem[apf2_src]);
    out = mem[apf2_src] + rev_mul(r->apf2_vol, apf2);

    if (r->on) {
        mem[same_dst] = (int16_t)sat16(same);
        mem[diff_dst] = (int16_t)sat16(diff);
        mem[apf1_dst] = (int16_t)sat16(apf1);
        mem[apf2_dst] = (int16_t)sat16(apf2);
    }
    out = sat16(out);
    r->out_prev[ch] = r->out_cur[ch];
    r->out_cur[ch] = (int16_t)out;
    *out_l = ch == 0 ? (int16_t)((r->out_prev[0] + r->out_cur[0]) >> 1) : r->out_cur[0];
    *out_r = ch == 1 ? (int16_t)((r->out_prev[1] + r->out_cur[1]) >> 1) : r->out_cur[1];
    if (ch) {
        if (++r->base >= r->len)
            r->base = 0;
    }
}

/* sat16 with one compare where nothing is stopped */
static inline int32_t rev_sat(int32_t v)
{
    return (uint32_t)v + 0x8000u <= 0xFFFFu ? v : v < 0 ? -32768 : 32767;
}

/* a side's places in rev_step's order (and its choices: the diff source is the other side's) */
enum {
    RT_SAME_SRC, RT_SAME_PRV, RT_DIFF_SRC, RT_DIFF_PRV, RT_COMB1, RT_COMB2, RT_COMB3, RT_COMB4,
    RT_APF1_SRC, RT_APF2_SRC, RT_SAME_DST, RT_DIFF_DST, RT_APF1_DST, RT_APF2_DST, RT_PLACES
};

static void rev_places(const Reverb *r, int ch, uint32_t *o)
{
    o[RT_SAME_SRC] = ch ? r->same_r_src : r->same_l_src;
    o[RT_SAME_PRV] = ch ? r->same_r_prv : r->same_l_prv;
    o[RT_DIFF_SRC] = ch ? r->diff_l_src : r->diff_r_src;
    o[RT_DIFF_PRV] = ch ? r->diff_r_prv : r->diff_l_prv;
    o[RT_COMB1] = ch ? r->comb1_r_src : r->comb1_l_src;
    o[RT_COMB2] = ch ? r->comb2_r_src : r->comb2_l_src;
    o[RT_COMB3] = ch ? r->comb3_r_src : r->comb3_l_src;
    o[RT_COMB4] = ch ? r->comb4_r_src : r->comb4_l_src;
    o[RT_APF1_SRC] = ch ? r->apf1_r_src : r->apf1_l_src;
    o[RT_APF2_SRC] = ch ? r->apf2_r_src : r->apf2_l_src;
    o[RT_SAME_DST] = ch ? r->same_r_dst : r->same_l_dst;
    o[RT_DIFF_DST] = ch ? r->diff_r_dst : r->diff_l_dst;
    o[RT_APF1_DST] = ch ? r->apf1_r_dst : r->apf1_l_dst;
    o[RT_APF2_DST] = ch ? r->apf2_r_dst : r->apf2_l_dst;
}

/* the volumes, as values (the stores into the work area would otherwise make them be read again) */
typedef struct RevVol {
    int32_t iir, wall, comb1, comb2, comb3, comb4, apf1, apf2;
} RevVol;

/* one side's step at its places p[k][j], its input in (scaled): rev_step's arithmetic, its loads before its
 * stores; its output, stopped at 16 bits */
static inline __attribute__((always_inline)) int32_t rev_side(int16_t *const *p, uint32_t j, int32_t in, int on,
                                                             const RevVol v)
{
    const int32_t same_prv = p[RT_SAME_PRV][j], diff_prv = p[RT_DIFF_PRV][j];
    const int32_t apf1_src = p[RT_APF1_SRC][j], apf2_src = p[RT_APF2_SRC][j];
    const int32_t same = rev_mul(v.iir, in + rev_mul(v.wall, p[RT_SAME_SRC][j]) - same_prv) + same_prv;
    const int32_t diff = rev_mul(v.iir, in + rev_mul(v.wall, p[RT_DIFF_SRC][j]) - diff_prv) + diff_prv;
    int32_t out = rev_mul(v.comb1, p[RT_COMB1][j]) + rev_mul(v.comb2, p[RT_COMB2][j]) +
                  rev_mul(v.comb3, p[RT_COMB3][j]) + rev_mul(v.comb4, p[RT_COMB4][j]);
    int32_t apf1, apf2;
    apf1 = out - rev_mul(v.apf1, apf1_src);
    out = apf1_src + rev_mul(v.apf1, apf1);
    apf2 = out - rev_mul(v.apf2, apf2_src);
    out = apf2_src + rev_mul(v.apf2, apf2);
    if (on) {
        p[RT_SAME_DST][j] = (int16_t)rev_sat(same);
        p[RT_DIFF_DST][j] = (int16_t)rev_sat(diff);
        p[RT_APF1_DST][j] = (int16_t)rev_sat(apf1);
        p[RT_APF2_DST][j] = (int16_t)rev_sat(apf2);
    }
    return rev_sat(out);
}

/* n samples from `first`: in and out are pairs (left, right) of 16-bit samples */
static void rev_run(Reverb *r, int16_t *mem, uint64_t first, uint32_t n, const int16_t *in, int16_t *out)
{
    uint32_t i = 0;
    if (!n)
        return;
    if (first & 1) {
        /* a right side's step first: alone */
        rev_step(r, mem, first, in[0], in[1], &out[0], &out[1]);
        i = 1;
    }
    if (n - i >= 2) {
        const uint32_t len = r->len;
        const int on = r->on;
        const int32_t kl = r->in_coef_l, kr = r->in_coef_r;
        RevVol v;
        uint32_t o[2][RT_PLACES], base = r->base, k;
        int32_t pl = r->in_cur[0], c0 = r->out_cur[0], c1 = r->out_cur[1], p0 = c0, p1 = c1;
        v.iir = r->iir_vol;
        v.wall = r->wall_vol;
        v.comb1 = r->comb1_vol;
        v.comb2 = r->comb2_vol;
        v.comb3 = r->comb3_vol;
        v.comb4 = r->comb4_vol;
        v.apf1 = r->apf1_vol;
        v.apf2 = r->apf2_vol;
        rev_places(r, 0, o[0]);
        rev_places(r, 1, o[1]);
        while (n - i >= 2) {
            /* the pairs up to the next point where a place, or the base, comes round the end */
            int16_t *p[2][RT_PLACES];
            uint32_t pairs = (n - i) / 2, j;
            if (len - base < pairs)
                pairs = len - base;
            for (k = 0; k < 2 * RT_PLACES; k++) {
                uint32_t x = base + o[k / RT_PLACES][k % RT_PLACES];
                if (x >= len)
                    x -= len;
                if (len - x < pairs)
                    pairs = len - x;
                p[k / RT_PLACES][k % RT_PLACES] = mem + r->start + x;
            }
            for (j = 0; j < pairs; j++, i += 2) {
                const int32_t l0 = in[2 * i], r0 = in[2 * i + 1], l1 = in[2 * i + 2], r1 = in[2 * i + 3];
                int32_t v0, v1;
                /* the left's step, then the right's (each sees what the other wrote) */
                v0 = rev_side(p[0], j, rev_mul(kl, (l0 + pl) >> 1), on, v);
                out[2 * i] = (int16_t)((c0 + v0) >> 1);
                out[2 * i + 1] = (int16_t)c1;
                p0 = c0;
                c0 = v0;
                v1 = rev_side(p[1], j, rev_mul(kr, (r1 + r0) >> 1), on, v);
                out[2 * i + 2] = (int16_t)c0;
                out[2 * i + 3] = (int16_t)((c1 + v1) >> 1);
                p1 = c1;
                c1 = v1;
                pl = l1;
            }
            base += pairs;
            if (base >= len)
                base = 0;
        }
        r->base = base;
        r->in_prev[0] = in[2 * i - 4];
        r->in_prev[1] = in[2 * i - 3];
        r->in_cur[0] = in[2 * i - 2];
        r->in_cur[1] = in[2 * i - 1];
        r->out_prev[0] = (int16_t)p0;
        r->out_prev[1] = (int16_t)p1;
        r->out_cur[0] = (int16_t)c0;
        r->out_cur[1] = (int16_t)c1;
    }
    if (i < n)
        /* a left side's step last */
        rev_step(r, mem, first + i, in[2 * i], in[2 * i + 1], &out[2 * i], &out[2 * i + 1]);
}

#endif
