/*
 * VU0 entries run inside the generated EE code (the title's VCALLMS_INLINE, tools/recomp): the collision code's
 * segment against a triangle, entries 0x0388 / 0x0390 / 0x0550 of the program at 0x4787D0. The recompiled function
 * keeps the VU0 registers in C locals and, at a VCALLMS, would store the dirty ones to the context, call
 * ps2_vu0_callms (which finds the native entry, vu0_seg_* in vu.c, that reads the context and writes it back) and
 * read back what is live; with a site inline the registers the entry reads go into a Vu0I, the entry's function
 * works on that, and the registers it writes come back into the locals: no stores, no dispatch, no loads, and the
 * compiler sees the whole of it. The functions are the entries' natives with the context replaced by the Vu0I and
 * are the same code as vu.c's (seg_tri_in there calls vu0i_seg), so they do what the natives do, which are checked
 * against the microprogram (the first 100 calls of each entry, and vu0chk 1 for all).
 *
 * vu0_inl_enter says whether the call may be taken this way now: the entry's program is in VU0, its native has had
 * its checks, no debugging switch is on and nothing is in flight in the VU0 (a division still running, flags in the
 * pipeline); else the generated code makes the call as before (only the registers the site names go through the
 * context). It also does the bookkeeping ps2_vu0_callms does before a native entry.
 */
#ifndef VU0_INLINE_H
#define VU0_INLINE_H

#include "vu_math.h"

typedef struct Vu0I {
    float vf[32][4];
    float q;
    uint32_t vi[16];
} Vu0I;

int vu0_inl_enter(R5900 *c, uint32_t entry);

/* a vertex lane as the collision entries decode it: itof0, then (acc = 0 + offset) + v * scale, each rounded */
VU_INL float vu0i_dec16(float bits, float o, float w)
{
    const float t = (float)(int32_t)F2U(bits);
    const float acc = 0.0f + o;
    const float m = t * w;
    return acc + m;
}

/* What the segment test computes (see vu.c's seg_tri_in for the program's reading of it): the segment starts at p
 * and runs along d, the triangle is a, b, c. v is VF19.x, q Q, m VI8, hit the entry's VI1 / VI2, ok that the
 * determinant passed (r is then t, u, v over det, and p, if wanted, the point). */
typedef struct Vu0Seg {
    float v, q, r[3], p[3];
    uint32_t m, hit;
    int ok;
} Vu0Seg;

VU_INL Vu0Seg vu0i_seg(float px, float py, float pz, float dx, float dy, float dz, float ax, float ay, float az,
                       float bx, float by, float bz, float cx, float cy, float cz, int want_pt)
{
    const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;                /* vf12 */
    const float sx = px - ax, sy = py - ay, sz = pz - az;                   /* vf15 */
    const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;                /* vf11 */
    const float pvx = dy * e2z - e2y * dz, pvy = dz * e2x - e2z * dx, pvz = dx * e2y - e2x * dy;  /* vf13 */
    const float qvx = sy * e1z - e1y * sz, qvy = sz * e1x - e1z * sx, qvz = sx * e1y - e1x * sy;  /* vf14 */
    const float det = (e1x * pvx + e1y * pvy) + e1z * pvz;                  /* vf16.x */
    const float u = (sx * pvx + sy * pvy) + sz * pvz;                       /* vf18.x */
    const float t = (e2x * qvx + e2y * qvy) + e2z * qvz;                    /* vf17.x */
    const float v = (dx * qvx + dy * qvy) + dz * qvz;                       /* vf19.x */
    Vu0Seg s;
    uint32_t m = vu_mac1(det - U2F(0x322BCC77u), 8) & 0x88;
    s.v = v;
    s.q = vu_div(1.0f, det);
    s.hit = 0;
    s.ok = !m;
    if (!m) {
        const float hi = U2F(0x3F800054u) * det, lo = U2F(0xB727C5ACu) * det, uv = u + v;
        const float rx = t * s.q, ry = 0.0f + u * s.q, rz = 0.0f + v * s.q;
        m = vu_mac1(u - lo, 8) | vu_mac1(hi - u, 8) | vu_mac1(v - lo, 8) | vu_mac1(hi - uv, 8) |
            vu_mac1(t - lo, 8) | vu_mac1(hi - t, 8);
        s.hit = !(m & 0x88);
        s.r[0] = rx;
        s.r[1] = ry;
        s.r[2] = rz;
        if (want_pt) {
            s.p[0] = (px + 0.0f) + dx * rx;
            s.p[1] = (py + 0.0f) + dy * rx;
            s.p[2] = (pz + 0.0f) + dz * rx;
        }
    }
    s.m = m;
    return s;
}

/* 0x0390: the triangle in vf5..vf7 as integers (vf6 and vf7 left decoded), the segment from vf2 along vf4; the answer
 * in vf24.xyz when the determinant passes, VI1 the hit */
VU_INL void vu0i_390(Vu0I *s)
{
    const float ox = s->vf[23][0], oy = s->vf[23][1], oz = s->vf[23][2], w = s->vf[23][3];
    const float ax = vu0i_dec16(s->vf[5][0], ox, w), ay = vu0i_dec16(s->vf[5][1], oy, w), az = vu0i_dec16(s->vf[5][2], oz, w);
    const float bx = vu0i_dec16(s->vf[6][0], ox, w), by = vu0i_dec16(s->vf[6][1], oy, w), bz = vu0i_dec16(s->vf[6][2], oz, w);
    const float cx = vu0i_dec16(s->vf[7][0], ox, w), cy = vu0i_dec16(s->vf[7][1], oy, w), cz = vu0i_dec16(s->vf[7][2], oz, w);
    const Vu0Seg r = vu0i_seg(s->vf[2][0], s->vf[2][1], s->vf[2][2], s->vf[4][0], s->vf[4][1], s->vf[4][2], ax, ay, az,
                              bx, by, bz, cx, cy, cz, 0);
    s->vf[6][0] = bx;
    s->vf[6][1] = by;
    s->vf[6][2] = bz;
    s->vf[7][0] = cx;
    s->vf[7][1] = cy;
    s->vf[7][2] = cz;
    s->vf[19][0] = r.v;
    s->q = r.q;
    s->vi[1] = r.hit;
    if (r.ok) {
        s->vf[24][0] = r.r[0];
        s->vf[24][1] = r.r[1];
        s->vf[24][2] = r.r[2];
    }
    s->vi[8] = r.m;
}

/* 0x0388: the segment's vector from its end first */
VU_INL void vu0i_388(Vu0I *s)
{
    s->vf[4][0] = s->vf[4][0] - s->vf[2][0];
    s->vf[4][1] = s->vf[4][1] - s->vf[2][1];
    s->vf[4][2] = s->vf[4][2] - s->vf[2][2];
    vu0i_390(s);
}

/* 0x0550: the triangle in vf8..vf10 as integers; the result in VI2 and vf8, the point in vf9 */
VU_INL void vu0i_550(Vu0I *s)
{
    const float ox = s->vf[23][0], oy = s->vf[23][1], oz = s->vf[23][2], w = s->vf[23][3];
    const float ax = vu0i_dec16(s->vf[8][0], ox, w), ay = vu0i_dec16(s->vf[8][1], oy, w), az = vu0i_dec16(s->vf[8][2], oz, w);
    const float bx = vu0i_dec16(s->vf[9][0], ox, w), by = vu0i_dec16(s->vf[9][1], oy, w), bz = vu0i_dec16(s->vf[9][2], oz, w);
    const float cx = vu0i_dec16(s->vf[10][0], ox, w), cy = vu0i_dec16(s->vf[10][1], oy, w), cz = vu0i_dec16(s->vf[10][2], oz, w);
    const Vu0Seg r = vu0i_seg(s->vf[2][0], s->vf[2][1], s->vf[2][2], s->vf[4][0], s->vf[4][1], s->vf[4][2], ax, ay, az,
                              bx, by, bz, cx, cy, cz, 1);
    s->vf[8][0] = ax;
    s->vf[8][1] = ay;
    s->vf[8][2] = az;
    s->vf[9][0] = bx;
    s->vf[9][1] = by;
    s->vf[9][2] = bz;
    s->vf[19][0] = r.v;
    s->q = r.q;
    s->vi[2] = r.hit;
    if (r.ok) {
        s->vf[8][0] = r.r[0];
        s->vf[8][1] = r.r[1];
        s->vf[8][2] = r.r[2];
        s->vf[9][0] = r.p[0];
        s->vf[9][1] = r.p[1];
        s->vf[9][2] = r.p[2];
    }
    s->vi[8] = r.m;
}

#endif
