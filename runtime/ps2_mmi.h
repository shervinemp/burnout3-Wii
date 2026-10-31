/*
 * R5900 multimedia instructions (128-bit integer SIMD), lane-wise C.
 * Included from ps2_recomp.h. Lane 0 of a u128 is bits 0..31.
 */
#ifndef PS2_MMI_H
#define PS2_MMI_H

/* element access: halfword i (0..7), byte i (0..15) by value, independent of host order */
PS2_INLINE uint32_t H_(u128 v, int i) { return (v.w[i >> 1] >> ((i & 1) * 16)) & 0xFFFFu; }
PS2_INLINE uint32_t B_(u128 v, int i) { return (v.w[i >> 2] >> ((i & 3) * 8)) & 0xFFu; }

PS2_INLINE u128 mk_h(const uint32_t h[8])
{
    u128 r;
    int k;
    for (k = 0; k < 4; k++)
        r.w[k] = (h[2 * k] & 0xFFFFu) | (h[2 * k + 1] << 16);
    return r;
}

PS2_INLINE u128 mk_b(const uint32_t b[16])
{
    u128 r;
    int k;
    for (k = 0; k < 4; k++)
        r.w[k] = (b[4 * k] & 0xFFu) | ((b[4 * k + 1] & 0xFFu) << 8) |
                 ((b[4 * k + 2] & 0xFFu) << 16) | (b[4 * k + 3] << 24);
    return r;
}

PS2_INLINE int32_t sat32(int64_t v) { return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v; }
PS2_INLINE int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
PS2_INLINE int32_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : v; }

/* ---- word ops ---- */
#define MMI_W(name, expr) \
    PS2_INLINE u128 name(u128 s, u128 t) { u128 r; int k; for (k = 0; k < 4; k++) { uint32_t a = s.w[k], b = t.w[k]; r.w[k] = (expr); } (void)s; (void)t; return r; }

MMI_W(mmi_paddw, a + b)
MMI_W(mmi_psubw, a - b)
MMI_W(mmi_pcgtw, (int32_t)a > (int32_t)b ? 0xFFFFFFFFu : 0u)
MMI_W(mmi_pceqw, a == b ? 0xFFFFFFFFu : 0u)
MMI_W(mmi_pmaxw, (int32_t)a > (int32_t)b ? a : b)
MMI_W(mmi_pminw, (int32_t)a < (int32_t)b ? a : b)
MMI_W(mmi_paddsw, (uint32_t)sat32((int64_t)(int32_t)a + (int32_t)b))
MMI_W(mmi_psubsw, (uint32_t)sat32((int64_t)(int32_t)a - (int32_t)b))
MMI_W(mmi_padduw, (uint64_t)a + b > 0xFFFFFFFFull ? 0xFFFFFFFFu : a + b)
MMI_W(mmi_psubuw, a > b ? a - b : 0u)
MMI_W(mmi_pand, a & b)
MMI_W(mmi_por, a | b)
MMI_W(mmi_pxor, a ^ b)
MMI_W(mmi_pnor, ~(a | b))
MMI_W(mmi_pabsw, (int32_t)b == INT32_MIN ? 0x7FFFFFFFu : (uint32_t)((int32_t)b < 0 ? -(int32_t)b : (int32_t)b))

/* ---- halfword ops ---- */
#define MMI_H(name, expr) \
    PS2_INLINE u128 name(u128 s, u128 t) { uint32_t h[8]; int k; for (k = 0; k < 8; k++) { int32_t a = (int16_t)H_(s, k), b = (int16_t)H_(t, k); uint32_t ua = H_(s, k), ub = H_(t, k); (void)a; (void)b; (void)ua; (void)ub; h[k] = (uint32_t)(expr); } return mk_h(h); }

MMI_H(mmi_paddh, ua + ub)
MMI_H(mmi_psubh, ua - ub)
MMI_H(mmi_pcgth, a > b ? 0xFFFFu : 0u)
MMI_H(mmi_pceqh, ua == ub ? 0xFFFFu : 0u)
MMI_H(mmi_pmaxh, a > b ? ua : ub)
MMI_H(mmi_pminh, a < b ? ua : ub)
MMI_H(mmi_paddsh, sat16(a + b))
MMI_H(mmi_psubsh, sat16(a - b))
MMI_H(mmi_padduh, ua + ub > 0xFFFFu ? 0xFFFFu : ua + ub)
MMI_H(mmi_psubuh, ua > ub ? ua - ub : 0u)
MMI_H(mmi_pabsh, b == -32768 ? 0x7FFF : (b < 0 ? -b : b))

PS2_INLINE u128 mmi_padsbh(u128 s, u128 t)
{
    uint32_t h[8];
    int k;
    for (k = 0; k < 8; k++)
        h[k] = k < 4 ? H_(s, k) - H_(t, k) : H_(s, k) + H_(t, k);
    return mk_h(h);
}

/* ---- byte ops ---- */
#define MMI_B(name, expr) \
    PS2_INLINE u128 name(u128 s, u128 t) { uint32_t b_[16]; int k; for (k = 0; k < 16; k++) { int32_t a = (int8_t)B_(s, k), b = (int8_t)B_(t, k); uint32_t ua = B_(s, k), ub = B_(t, k); (void)a; (void)b; (void)ua; (void)ub; b_[k] = (uint32_t)(expr); } return mk_b(b_); }

MMI_B(mmi_paddb, ua + ub)
MMI_B(mmi_psubb, ua - ub)
MMI_B(mmi_pcgtb, a > b ? 0xFFu : 0u)
MMI_B(mmi_pceqb, ua == ub ? 0xFFu : 0u)
MMI_B(mmi_paddsb, sat8(a + b))
MMI_B(mmi_psubsb, sat8(a - b))
MMI_B(mmi_paddub, ua + ub > 0xFFu ? 0xFFu : ua + ub)
MMI_B(mmi_psubub, ua > ub ? ua - ub : 0u)

/* ---- interleave / pack ---- */
PS2_INLINE u128 mmi_pextlw(u128 s, u128 t) { return u128_make(t.w[0], s.w[0], t.w[1], s.w[1]); }
PS2_INLINE u128 mmi_pextuw(u128 s, u128 t) { return u128_make(t.w[2], s.w[2], t.w[3], s.w[3]); }
PS2_INLINE u128 mmi_ppacw(u128 s, u128 t) { return u128_make(t.w[0], t.w[2], s.w[0], s.w[2]); }

PS2_INLINE u128 mmi_pextlh(u128 s, u128 t)
{
    uint32_t h[8];
    int k;
    for (k = 0; k < 4; k++) { h[2 * k] = H_(t, k); h[2 * k + 1] = H_(s, k); }
    return mk_h(h);
}

PS2_INLINE u128 mmi_pextuh(u128 s, u128 t)
{
    uint32_t h[8];
    int k;
    for (k = 0; k < 4; k++) { h[2 * k] = H_(t, k + 4); h[2 * k + 1] = H_(s, k + 4); }
    return mk_h(h);
}

PS2_INLINE u128 mmi_ppach(u128 s, u128 t)
{
    uint32_t h[8];
    int k;
    for (k = 0; k < 4; k++) { h[k] = H_(t, 2 * k); h[k + 4] = H_(s, 2 * k); }
    return mk_h(h);
}

PS2_INLINE u128 mmi_pextlb(u128 s, u128 t)
{
    uint32_t b[16];
    int k;
    for (k = 0; k < 8; k++) { b[2 * k] = B_(t, k); b[2 * k + 1] = B_(s, k); }
    return mk_b(b);
}

PS2_INLINE u128 mmi_pextub(u128 s, u128 t)
{
    uint32_t b[16];
    int k;
    for (k = 0; k < 8; k++) { b[2 * k] = B_(t, k + 8); b[2 * k + 1] = B_(s, k + 8); }
    return mk_b(b);
}

PS2_INLINE u128 mmi_ppacb(u128 s, u128 t)
{
    uint32_t b[16];
    int k;
    for (k = 0; k < 8; k++) { b[k] = B_(t, 2 * k); b[k + 8] = B_(s, 2 * k); }
    return mk_b(b);
}

PS2_INLINE u128 mmi_pext5(u128 s, u128 t)
{
    u128 r;
    int k;
    (void)s;
    for (k = 0; k < 4; k++) {
        uint32_t h = t.w[k] & 0xFFFFu;
        r.w[k] = ((h & 0x1F) << 3) | (((h >> 5) & 0x1F) << 11) | (((h >> 10) & 0x1F) << 19) | ((h >> 15) << 31);
    }
    return r;
}

PS2_INLINE u128 mmi_ppac5(u128 s, u128 t)
{
    u128 r;
    int k;
    (void)s;
    for (k = 0; k < 4; k++) {
        uint32_t w = t.w[k];
        r.w[k] = ((w >> 3) & 0x1F) | (((w >> 11) & 0x1F) << 5) | (((w >> 19) & 0x1F) << 10) | ((w >> 31) << 15);
    }
    return r;
}

PS2_INLINE u128 mmi_pinth(u128 s, u128 t)
{
    uint32_t h[8];
    int k;
    for (k = 0; k < 4; k++) { h[2 * k] = H_(t, k); h[2 * k + 1] = H_(s, k + 4); }
    return mk_h(h);
}

PS2_INLINE u128 mmi_pinteh(u128 s, u128 t)
{
    uint32_t h[8];
    int k;
    for (k = 0; k < 4; k++) { h[2 * k] = H_(t, 2 * k); h[2 * k + 1] = H_(s, 2 * k); }
    return mk_h(h);
}

/* single-source permutes (rs is $zero) */
#define MMI_PERM_H(name, i0, i1, i2, i3, i4, i5, i6, i7) \
    PS2_INLINE u128 name(u128 s, u128 t) { uint32_t h[8] = {H_(t, i0), H_(t, i1), H_(t, i2), H_(t, i3), H_(t, i4), H_(t, i5), H_(t, i6), H_(t, i7)}; (void)s; return mk_h(h); }
MMI_PERM_H(mmi_pexeh, 2, 1, 0, 3, 6, 5, 4, 7)
MMI_PERM_H(mmi_prevh, 3, 2, 1, 0, 7, 6, 5, 4)
MMI_PERM_H(mmi_pexch, 0, 2, 1, 3, 4, 6, 5, 7)
MMI_PERM_H(mmi_pcpyh, 0, 0, 0, 0, 4, 4, 4, 4)

PS2_INLINE u128 mmi_pexew(u128 s, u128 t) { (void)s; return u128_make(t.w[2], t.w[1], t.w[0], t.w[3]); }
PS2_INLINE u128 mmi_prot3w(u128 s, u128 t) { (void)s; return u128_make(t.w[1], t.w[2], t.w[0], t.w[3]); }
PS2_INLINE u128 mmi_pexcw(u128 s, u128 t) { (void)s; return u128_make(t.w[0], t.w[2], t.w[1], t.w[3]); }

/* ---- shifts ---- */
PS2_INLINE u128 mmi_psllh(u128 t, int sa) { uint32_t h[8]; int k; for (k = 0; k < 8; k++) h[k] = H_(t, k) << (sa & 15); return mk_h(h); }
PS2_INLINE u128 mmi_psrlh(u128 t, int sa) { uint32_t h[8]; int k; for (k = 0; k < 8; k++) h[k] = H_(t, k) >> (sa & 15); return mk_h(h); }
PS2_INLINE u128 mmi_psrah(u128 t, int sa) { uint32_t h[8]; int k; for (k = 0; k < 8; k++) h[k] = (uint32_t)((int16_t)H_(t, k) >> (sa & 15)); return mk_h(h); }
PS2_INLINE u128 mmi_psllw(u128 t, int sa) { u128 r; int k; for (k = 0; k < 4; k++) r.w[k] = t.w[k] << (sa & 31); return r; }
PS2_INLINE u128 mmi_psrlw(u128 t, int sa) { u128 r; int k; for (k = 0; k < 4; k++) r.w[k] = t.w[k] >> (sa & 31); return r; }
PS2_INLINE u128 mmi_psraw(u128 t, int sa) { u128 r; int k; for (k = 0; k < 4; k++) r.w[k] = (uint32_t)((int32_t)t.w[k] >> (sa & 31)); return r; }

/* variable shifts on the two low words of each doubleword, results sign-extended */
PS2_INLINE u128 mmi_psllvw(u128 s, u128 t)
{
    uint32_t a = t.w[0] << (s.w[0] & 31), b = t.w[2] << (s.w[2] & 31);
    return u128_make(a, SX(a), b, SX(b));
}

PS2_INLINE u128 mmi_psrlvw(u128 s, u128 t)
{
    uint32_t a = t.w[0] >> (s.w[0] & 31), b = t.w[2] >> (s.w[2] & 31);
    return u128_make(a, SX(a), b, SX(b));
}

PS2_INLINE u128 mmi_psravw(u128 s, u128 t)
{
    uint32_t a = (uint32_t)((int32_t)t.w[0] >> (s.w[0] & 31)), b = (uint32_t)((int32_t)t.w[2] >> (s.w[2] & 31));
    return u128_make(a, SX(a), b, SX(b));
}

/* QFSRV: low 128 bits of (rs:rt) >> SA, SA in bits */
PS2_INLINE u128 mmi_qfsrv(u128 s, u128 t, uint32_t sa)
{
    uint32_t src[8] = {t.w[0], t.w[1], t.w[2], t.w[3], s.w[0], s.w[1], s.w[2], s.w[3]};
    u128 r;
    uint32_t wsh = (sa & 127) >> 5, bsh = sa & 31;
    int k;
    for (k = 0; k < 4; k++) {
        uint32_t lo = src[k + wsh];
        uint32_t hi = (k + wsh + 1) < 8 ? src[k + wsh + 1] : 0;
        r.w[k] = bsh ? (lo >> bsh) | (hi << (32 - bsh)) : lo;
    }
    return r;
}

/* ---- HI/LO users ---- */
PS2_INLINE u128 mmi_pmfhl(u128 lo, u128 hi, int fmt)
{
    switch (fmt) {
    case 0: return u128_make(lo.w[0], hi.w[0], lo.w[2], hi.w[2]);
    case 1: return u128_make(lo.w[1], hi.w[1], lo.w[3], hi.w[3]);
    case 2: {
        int32_t a = sat32((int64_t)J64(lo.w[0], hi.w[0])), b = sat32((int64_t)J64(lo.w[2], hi.w[2]));
        return u128_make((uint32_t)a, SX(a), (uint32_t)b, SX(b));
    }
    case 3: {
        uint32_t h[8] = {H_(lo, 0), H_(lo, 2), H_(hi, 0), H_(hi, 2), H_(lo, 4), H_(lo, 6), H_(hi, 4), H_(hi, 6)};
        return mk_h(h);
    }
    default: {
        uint32_t h[8] = {(uint32_t)sat16((int32_t)lo.w[0]), (uint32_t)sat16((int32_t)lo.w[1]),
                         (uint32_t)sat16((int32_t)hi.w[0]), (uint32_t)sat16((int32_t)hi.w[1]),
                         (uint32_t)sat16((int32_t)lo.w[2]), (uint32_t)sat16((int32_t)lo.w[3]),
                         (uint32_t)sat16((int32_t)hi.w[2]), (uint32_t)sat16((int32_t)hi.w[3])};
        return mk_h(h);
    }
    }
}

PS2_INLINE u128 mmi_wmul_common(u128 s, u128 t, u128 *hi, u128 *lo, int sign, int acc)
{
    u128 r;
    int i;
    for (i = 0; i < 2; i++) {
        int k = 2 * i;
        uint64_t p = sign ? (uint64_t)((int64_t)(int32_t)s.w[k] * (int32_t)t.w[k])
                          : (uint64_t)s.w[k] * t.w[k];
        if (acc > 0)
            p = J64(lo->w[k], hi->w[k]) + p;
        else if (acc < 0)
            p = J64(lo->w[k], hi->w[k]) - p;
        lo->w[k] = (uint32_t)p; lo->w[k + 1] = SX(lo->w[k]);
        hi->w[k] = (uint32_t)(p >> 32); hi->w[k + 1] = SX(hi->w[k]);
        r.w[k] = (uint32_t)p; r.w[k + 1] = (uint32_t)(p >> 32);
    }
    return r;
}

PS2_INLINE u128 mmi_pmultw(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_wmul_common(s, t, hi, lo, 1, 0); }
PS2_INLINE u128 mmi_pmultuw(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_wmul_common(s, t, hi, lo, 0, 0); }
PS2_INLINE u128 mmi_pmaddw(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_wmul_common(s, t, hi, lo, 1, 1); }
PS2_INLINE u128 mmi_pmadduw(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_wmul_common(s, t, hi, lo, 0, 1); }
PS2_INLINE u128 mmi_pmsubw(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_wmul_common(s, t, hi, lo, 1, -1); }

PS2_INLINE u128 mmi_pdivw(u128 s, u128 t, u128 *hi, u128 *lo)
{
    int i;
    for (i = 0; i < 2; i++) {
        uint32_t q, r;
        ps2_div(s.w[2 * i], t.w[2 * i], &q, &r);
        lo->w[2 * i] = q; lo->w[2 * i + 1] = SX(q);
        hi->w[2 * i] = r; hi->w[2 * i + 1] = SX(r);
    }
    return *lo;
}

PS2_INLINE u128 mmi_pdivuw(u128 s, u128 t, u128 *hi, u128 *lo)
{
    int i;
    for (i = 0; i < 2; i++) {
        uint32_t q, r;
        ps2_divu(s.w[2 * i], t.w[2 * i], &q, &r);
        lo->w[2 * i] = q; lo->w[2 * i + 1] = SX(q);
        hi->w[2 * i] = r; hi->w[2 * i + 1] = SX(r);
    }
    return *lo;
}

PS2_INLINE u128 mmi_pdivbw(u128 s, u128 t, u128 *hi, u128 *lo)
{
    int k;
    int32_t d = (int16_t)H_(t, 0);
    for (k = 0; k < 4; k++) {
        uint32_t q, r;
        ps2_div(s.w[k], (uint32_t)d, &q, &r);
        lo->w[k] = q;
        hi->w[k] = (uint32_t)(int32_t)(int16_t)r;
    }
    return *lo;
}

/* halfword multiplies: products land in LO/HI as described in the EE manual */
PS2_INLINE u128 mmi_hmul_common(u128 s, u128 t, u128 *hi, u128 *lo, int mode)
{
    int32_t p[8];
    int k;
    for (k = 0; k < 8; k++)
        p[k] = (int16_t)H_(s, k) * (int16_t)H_(t, k);
    if (mode == 0 || mode == 1 || mode == -1) {
        /* lo.w0=p0 lo.w1=p1 hi.w0=p2 hi.w1=p3 lo.w2=p4 lo.w3=p5 hi.w2=p6 hi.w3=p7 */
        int32_t *L[4] = {(int32_t *)&lo->w[0], (int32_t *)&lo->w[1], (int32_t *)&lo->w[2], (int32_t *)&lo->w[3]};
        int32_t *H[4] = {(int32_t *)&hi->w[0], (int32_t *)&hi->w[1], (int32_t *)&hi->w[2], (int32_t *)&hi->w[3]};
        int32_t v[8] = {p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]};
        if (mode == 1) {
            v[0] += *L[0]; v[1] += *L[1]; v[2] += *H[0]; v[3] += *H[1];
            v[4] += *L[2]; v[5] += *L[3]; v[6] += *H[2]; v[7] += *H[3];
        } else if (mode == -1) {
            v[0] = *L[0] - v[0]; v[1] = *L[1] - v[1]; v[2] = *H[0] - v[2]; v[3] = *H[1] - v[3];
            v[4] = *L[2] - v[4]; v[5] = *L[3] - v[5]; v[6] = *H[2] - v[6]; v[7] = *H[3] - v[7];
        }
        lo->w[0] = (uint32_t)v[0]; lo->w[1] = (uint32_t)v[1]; hi->w[0] = (uint32_t)v[2]; hi->w[1] = (uint32_t)v[3];
        lo->w[2] = (uint32_t)v[4]; lo->w[3] = (uint32_t)v[5]; hi->w[2] = (uint32_t)v[6]; hi->w[3] = (uint32_t)v[7];
        return u128_make(lo->w[0], hi->w[0], lo->w[2], hi->w[2]);
    }
    /* horizontal: mode 2 = phmadh (sum), 3 = phmsbh (difference) */
    if (mode == 2) {
        lo->w[0] = (uint32_t)(p[1] + p[0]); hi->w[0] = (uint32_t)(p[3] + p[2]);
        lo->w[2] = (uint32_t)(p[5] + p[4]); hi->w[2] = (uint32_t)(p[7] + p[6]);
    } else {
        lo->w[0] = (uint32_t)(p[1] - p[0]); hi->w[0] = (uint32_t)(p[3] - p[2]);
        lo->w[2] = (uint32_t)(p[5] - p[4]); hi->w[2] = (uint32_t)(p[7] - p[6]);
    }
    return u128_make(lo->w[0], hi->w[0], lo->w[2], hi->w[2]);
}

PS2_INLINE u128 mmi_pmulth(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_hmul_common(s, t, hi, lo, 0); }
PS2_INLINE u128 mmi_pmaddh(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_hmul_common(s, t, hi, lo, 1); }
PS2_INLINE u128 mmi_pmsubh(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_hmul_common(s, t, hi, lo, -1); }
PS2_INLINE u128 mmi_phmadh(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_hmul_common(s, t, hi, lo, 2); }
PS2_INLINE u128 mmi_phmsbh(u128 s, u128 t, u128 *hi, u128 *lo) { return mmi_hmul_common(s, t, hi, lo, 3); }

#endif /* PS2_MMI_H */
