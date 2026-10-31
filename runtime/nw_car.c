/*
 * Native car shader (docs/native-renderer.md): what VU1 microprogram 46BB80
 * computes, in C, for the platform backend to draw. Cars (0x21FBB0 per mesh,
 * set up by 0x220540) send batches of up to ~52 vertices: positions,
 * normals, UVs and skin weights. Per vertex the microprogram
 *   - blends two deformation matrices (crash damage) when 1013.z is set,
 *   - lights: L = (n,1)^T M (n,1) with M = 966..969, colour = 1010 + (1011 -
 *     1010) * L, alpha from a fresnel term of n and 970,
 *   - unless 1013.y is set, makes sphere-map UVs from the eye vector
 *     (971..973 .w) reflected about n and rotated by 971..973,
 * and draws pass 1 (GS context 1: texture x lit colour) and, with the
 * sphere map, pass 2 (context 2: reflection x colour 1012).
 */
#include "rt.h"
#include "gs.h"
#include "nw_car.h"

#include <math.h>
#include <string.h>

/* The render data: a RET-tagged VIF packet with one or more batches, each
 * UNPACKing n positions (V4-32, TOPS+0), normals (V3-8 plus ROW, TOPS+2) and
 * UV pairs / skin data (V4-16, TOPS+1, every vertex, or with CL 6 every other
 * one), then ITOP n and, inside the packet, MSCNT; the last batch's MSCNT may
 * come after the packet. STROW, STMOD and STCYCL may come anywhere; what is
 * not set inside comes from the VIF (p's registers on entry). */
static void batch_start(NcPacket *p, NcBlock *b, uint32_t a)
{
    pc_batch((int)(b - p->b));
    memset(b, 0, sizeof *b);
    b->start = a;
    b->cl = p->cl;
    b->wl = p->wl;
    b->mode = p->mode;
    memcpy(b->srow, p->row, sizeof b->srow);
}

int nc_packet_parse(uint32_t a, uint32_t end, NcPacket *p)
{
    uint32_t got = 0;
    NcBlock *b = &p->b[0];
    int k;
    p->nb = 0;
    batch_start(p, b, a);
    while (a < end) {
        uint32_t code = pc_word(a), cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF;
        a += 4;
        switch (cmd) {
        case 0x00: case 0x10: case 0x11: break;         /* NOP, FLUSHE, FLUSH */
        case 0x01: p->cl = code & 0xFF; p->wl = (code >> 8) & 0xFF; pc_set(1); break;
        case 0x05: p->mode = code & 3; pc_set(2); break;
        case 0x30:                                      /* STROW */
            if (a + 16 > end)
                return 0;
            for (k = 0; k < 4; k++)
                p->row[k] = pc_word(a + 4 * k);
            a += 16;
            pc_set(4);
            break;
        case 0x04:                                      /* ITOP */
            if (got != 7 || (code & 0x3FF) != b->n)
                return 0;
            got |= 8;
            break;
        case 0x17:                                      /* MSCNT: the batch is complete */
            if (got != 15 || p->nb + 1 >= NC_BATCHES)
                return 0;
            b->mscnt = 1;
            b = &p->b[++p->nb];
            batch_start(p, b, a);
            got = 0;
            break;
        default:
            pc_dep(3);                  /* CL, WL and MODE as they are */
            if ((cmd & 0x70) == 0x60 && (code & 0x8000) && !(got & 8)) {
                uint32_t fmt = cmd & 0xF, off = code & 0x3FF, usn = (code >> 14) & 1;
                if (!num)
                    num = 256;
                if (off == 0 && fmt == 0xC && p->cl == 3 && p->wl == 1 && p->mode == 0 && !(got & 1)) {
                    b->n = num;
                    b->pos = a;
                    a += 16 * num;
                    got |= 1;
                } else if (off == 2 && fmt == 0xA && usn && p->cl == 3 && p->wl == 1 && p->mode == 1 &&
                           (got & 1) && num == b->n) {
                    b->nrm = a;
                    pc_dep(4);          /* and ROW */
                    memcpy(b->row, p->row, sizeof b->row);
                    a += (3 * num + 3) & ~3u;
                    got |= 2;
                } else if (off == 1 && fmt == 0xD && usn && p->wl == 1 && (p->cl == 3 || p->cl == 6) &&
                           p->mode == 0) {
                    b->uv = a;
                    b->uvn = num;
                    b->uvstep = p->cl / 3;
                    a += 8 * num;
                    got |= 4;
                } else {
                    return 0;
                }
                if (a > end)
                    return 0;
                break;
            }
            return 0;
        }
    }
    for (k = 0; k <= p->nb; k++) {
        b = &p->b[k];
        if (k == p->nb && !got)
            break;                                      /* nothing after the last MSCNT */
        if (k == p->nb && got != 15)
            return 0;
        if (b->n < 3 || b->n > NC_MAX || b->uvn * b->uvstep < ((b->n + 1) & ~1u) - 1)
            return 0;
    }
    if (got)
        p->nb++;
    return p->nb > 0;
}

/* slot 1 (V4-16) of vertex k, as the microprogram finds it */
static void slot1(const NcBlock *b, uint32_t k, uint32_t q[4])
{
    uint32_t e, a;
    q[0] = q[1] = q[2] = q[3] = 0;
    if (b->uvstep == 2) {               /* 1 or 2: no division */
        if (k & 1)
            return;
        e = k >> 1;
    } else {
        e = k;
    }
    if (e >= b->uvn)
        return;
    a = b->uv + 8 * e;
    q[0] = LW(a) & 0xFFFF;
    q[1] = LW(a) >> 16;
    q[2] = LW(a + 4) & 0xFFFF;
    q[3] = LW(a + 4) >> 16;
}

void nc_load(const NcBlock *b, NcIn *in)
{
    /* the render data is guest RAM: host-order words, bytes at XORed addresses */
    const uint32_t *pos = (const uint32_t *)GPTR(b->pos);
    const uint8_t *ram = ps2_ram;
    uint32_t k, c, na = b->nrm, r0 = b->row[0], r1 = b->row[1], r2 = b->row[2];
    in->n = b->n;
    in->q1_odd = b->uvstep == 1;
    for (k = b->n; k < ((b->n + 3) & ~3u); k++)
        slot1(b, k, in->q1[k]);
    for (k = 0; k < b->n; k++, pos += 4, na += 3) {
        in->pos[k][0] = BITS2F(pos[0]);
        in->pos[k][1] = BITS2F(pos[1]);
        in->pos[k][2] = BITS2F(pos[2]);
        in->posw[k] = pos[3];
        in->nrm[k][0] = BITS2F(r0 + ram[(na & PS2_RAM_MASK) ^ PS2_BXOR]);
        in->nrm[k][1] = BITS2F(r1 + ram[((na + 1) & PS2_RAM_MASK) ^ PS2_BXOR]);
        in->nrm[k][2] = BITS2F(r2 + ram[((na + 2) & PS2_RAM_MASK) ^ PS2_BXOR]);
        slot1(b, k, in->q1[k]);
    }
    (void)c;
}

#define V(n, c) BITS2F(vu[(n) * 4 + (c)])
#define VI(n, c) (vu[(n) * 4 + (c)] & 0xFFFF)

static int32_t ftoi0(float f)
{
    if (f >= 2147483647.0f)
        return 0x7FFFFFFF;
    if (f <= -2147483648.0f)
        return (int32_t)0x80000000u;
    return (int32_t)f;
}

/* Crash deformation (1013.z): vertex 4g+r blends the matrices at 974 + idx
 * (qword offsets, wrapping) with weights from slot 1 of vertices 4g+1 (first
 * matrix) and 4g+3 (second), lane r: weight = value / 65535, idx = value. */
static void skin(const uint32_t *vu, NcIn *in)
{
    uint32_t g, r, j, c;
    float off = V(970, 3);
    for (g = 0; g + 1 < in->n; g += 4) {
        const uint32_t *a = in->q1[g + 1], *bq = g + 3 < in->n ? in->q1[g + 3] : in->q1[g + 1];
        for (r = 0; r < 4 && g + r < in->n; r++) {
            uint32_t ia = 974 + (a[r] & 0x3FF), ib = 974 + (bq[r] & 0x3FF);
            float wa = (float)a[r] / 65535.0f, wb = (float)bq[r] / 65535.0f, w0 = 1.0f - wa - wb;
            float m[4][3], p[3], n[3];
            for (j = 0; j < 4; j++)
                for (c = 0; c < 3; c++)
                    m[j][c] = V((ia + j) & 1023, c) * wa + V((ib + j) & 1023, c) * wb + (j == c ? w0 : 0.0f);
            for (c = 0; c < 3; c++) {
                p[c] = in->pos[g + r][c];
                n[c] = in->nrm[g + r][c] - off;
            }
            for (c = 0; c < 3; c++) {
                in->pos[g + r][c] = m[0][c] * p[0] + m[1][c] * p[1] + m[2][c] * p[2] + m[3][c];
                in->nrm[g + r][c] = m[0][c] * n[0] + m[1][c] * n[1] + m[2][c] * n[2] + off;
            }
        }
    }
}

/* FTOI0 as the GS takes it: the low 8 bits of the integer */
static inline uint8_t ftoi_u8(float f)
{
    return (uint8_t)ftoi0(f);
}

/* the same where fctiwz saturates as ftoi0 does (NaN: 0x80000000 both): no
 * compares */
static inline uint32_t ftoi_u8_fast(float f)
{
#if defined(GEKKO)
    return (uint32_t)(int32_t)f & 0xFF;
#else
    return ftoi_u8(f);
#endif
}

/* Two floats as bytes straight into memory: the paired singles' quantised
 * store (through GQR 3, set to unsigned bytes by nc_gqr), which cuts the
 * fraction off and keeps the value within 0..255. A conversion by fctiwz
 * goes through the stack into an integer register, masks and shifts: four
 * of those a vertex for a colour were a quarter of nc_shade_direct. Out of
 * range the microprogram's byte wraps (the GS takes the low eight bits) and
 * this one stops at the end: the stores are for vertices whose light term
 * keeps the colour in range (nc_lrange). */
#if defined(GEKKO)
#define NC_PSQ 1
static inline void nc_gqr(void)
{
    __asm__ volatile("mtspr 915,%0" : : "r"(0x00040004u));
}

static inline void st_u8x2(uint8_t *dst, float a, float b)
{
    register float t;
    __asm__ volatile("ps_merge00 %0,%1,%2\n\tpsq_st %0,0(%3),0,3" : "=&f"(t) : "f"(a), "f"(b), "b"(dst) : "memory");
}

/* The light term L within (lo, hi): each of c0 + d L for red, green and blue
 * is within 0..255 and a little, where the store's byte is the conversion's
 * (1 vertex in 80 of a race is outside: those go the old way and wrap). */
static void nc_lrange(const float *c0, const float *d, float *lo, float *hi)
{
    float a = -1.0e30f, b = 1.0e30f;
    int k;
    for (k = 0; k < 3; k++) {
        if (d[k] > 0.0f || d[k] < 0.0f) {
            const float x = (0.0f - c0[k]) / d[k], y = (256.0f - c0[k]) / d[k];
            const float l = x < y ? x : y, h = x < y ? y : x, m = 1.0e-4f * (fabsf(l) + fabsf(h) + 1.0f);
            if (l + m > a)
                a = l + m;
            if (h - m < b)
                b = h - m;
        } else if (!(c0[k] >= 0.0f && c0[k] < 256.0f)) {
            b = -1.0e30f;                   /* never (a NaN comes here too) */
        }
    }
    *lo = a;
    *hi = b;
}
#else
#define NC_PSQ 0
#endif

/* A normal's component for each byte: the float whose bits are the ROW plus
 * the byte, less the offset in 970.w, as the microprogram has it (the cars'
 * ROW is 65536.0 and the offset 65537: byte / 128 - 1). Summing the bits for
 * every vertex takes each component through memory from an integer register
 * into a float one; the rows are the same for every car, so this is filled
 * once. (A shortcut for rows of 2^23 was here before: no car has those.) */
static float g_nrm_f[3][256];

uint32_t g_nc_rows_made;                /* status line: the normals' table made again */

/* GQR 5 for the normals' copy, or 0: the rows make every normal byte k exactly (k - 128) / 2^s (the game's ROW 65536
 * and offset 65537: s = 7), so a host-order copy of the bytes xor 0x80, read as signed bytes scaled by 2^-s, is the
 * same floats (nc_shade_ps_loop3) */
static uint32_t g_nrm_s8;

static void nc_rows(uint32_t r0, uint32_t r1, uint32_t r2, float off)
{
    static uint32_t key[4] = {1, 0, 0, 0};
    const uint32_t r[3] = {r0, r1, r2};
    uint32_t c, k, sc;
    if (key[0] == r0 && key[1] == r1 && key[2] == r2 && key[3] == F2BITS(off))
        return;
    g_nc_rows_made++;
    for (c = 0; c < 3; c++)
        for (k = 0; k < 256; k++)
            g_nrm_f[c][k] = BITS2F(r[c] + k) - off;
    g_nrm_s8 = 0;
    for (sc = 0; sc < 24 && !g_nrm_s8; sc++) {
        const float scale = 1.0f / (float)(1u << sc);
        int same = 1;
        for (c = 0; c < 3 && same; c++)
            for (k = 0; k < 256 && same; k++)
                same = F2BITS(g_nrm_f[c][k]) == F2BITS((float)(int8_t)(k ^ 0x80) * scale);
        if (same)
            g_nrm_s8 = sc << 24 | 0x00060000u;      /* loads: bytes with a sign, times 2^-sc */
    }
    {
        static int logged;
        if (logged < 4) {
            logged++;
            LOG("car normals: rows %08X %08X %08X, offset %08X: %s", r0, r1, r2, F2BITS(off),
                g_nrm_s8 ? "signed bytes through GQR 5" : "the table");
        }
    }
    key[0] = r0;
    key[1] = r1;
    key[2] = r2;
    key[3] = F2BITS(off);
}

/* debugging: the bytes the old way too, and how far apart (the backend's gxflags2 8192) */
int g_nc_psq_check;
uint32_t g_nc_psq_n, g_nc_psq_bad, g_nc_psq_max[3];

int nc_shade(const uint32_t *vu, NcIn *in, NcOut *out)
{
    uint32_t n = in->n, k, c;
    float c0[4], d[4], amax;
    /* the constants in locals: the stores below could alias VU memory otherwise */
    const float off = V(970, 3);
    const float m00 = V(966, 0), m01 = V(966, 1), m02 = V(966, 2), m03 = V(966, 3);
    const float m10 = V(967, 0), m11 = V(967, 1), m12 = V(967, 2), m13 = V(967, 3);
    const float m20 = V(968, 0), m21 = V(968, 1), m22 = V(968, 2), m23 = V(968, 3);
    const float m30 = V(969, 0), m31 = V(969, 1), m32 = V(969, 2), m33 = V(969, 3);
    const float fx = V(970, 0), fy = V(970, 1), fz = V(970, 2);
    const float e00 = V(971, 0), e01 = V(971, 1), e02 = V(971, 2), ex0 = V(971, 3);
    const float e10 = V(972, 0), e11 = V(972, 1), e12 = V(972, 2), ey0 = V(972, 3);
    const float e20 = V(973, 0), e21 = V(973, 1), e22 = V(973, 2), ez0 = V(973, 3);
    int env;
    out->n = n;
    out->env = env = VI(1013, 1) == 0;
    if (VI(1013, 2)) {
        if (!in->q1_odd)
            return 0;                   /* the weights would come from stale memory */
        skin(vu, in);
    }
    for (c = 0; c < 4; c++) {
        c0[c] = V(1010, c);
        d[c] = V(1011, c) - V(1010, c);
    }
    amax = V(1011, 3) > 0.0f ? V(1011, 3) : 0.0f;
    for (k = 0; k < n; k++) {
        const float px = in->pos[k][0], py = in->pos[k][1], pz = in->pos[k][2];
        const float nx = in->nrm[k][0] - off, ny = in->nrm[k][1] - off, nz = in->nrm[k][2] - off;
        const float X = m30 + nx * m00 + ny * m10 + nz * m20;
        const float Y = m31 + nx * m01 + ny * m11 + nz * m21;
        const float Z = m32 + nx * m02 + ny * m12 + nz * m22;
        const float E = fabsf(nx * fx + ny * fy + nz * fz);
        const float F = E + E - E * E;
        const float L = m33 + nx * m03 + nz * m23 + ny * m13 + X * nx + Y * ny + Z * nz;
        out->col[k][0] = ftoi_u8(c0[0] + d[0] * L);
        out->col[k][1] = ftoi_u8(c0[1] + d[1] * L);
        out->col[k][2] = ftoi_u8(c0[2] + d[2] * L);
        out->col[k][3] = ftoi_u8(amax - d[3] * F);
        out->pos[k][0] = px;
        out->pos[k][1] = py;
        out->pos[k][2] = pz;
        out->adc[k] = (in->posw[k] & 0x8000) != 0;
        if (env) {
            const float vx = px - ex0, vy = py - ey0, vz = pz - ez0;
            const float dn = 2.0f * (vx * nx + vy * ny + vz * nz);
            const float rx = vx - dn * nx, ry = vy - dn * ny, rz = vz - dn * nz;
            const float ex = rx * e00 + ry * e10 + rz * e20;
            const float ey = rx * e01 + ry * e11 + rz * e21;
            const float l2 = vx * vx + vy * vy + vz * vz;
            const float ez = l2 * rt_rsqrtf(l2) + rx * e02 + ry * e12 + rz * e22;
            const float m = 0.5f * rt_rsqrtf(ex * ex + ey * ey + ez * ez);
            out->uv1[k][0] = 0.5f + ex * m;
            out->uv1[k][1] = 0.5f + ey * m;
        }
    }
    /* UV pairs: vertex 4g+r takes slot 1 of 4g (r 0: xy, r 2: zw) or of
     * 4g+2 (r 1: xy, r 3: zw), in 4.12 */
    for (k = 0; k < n; k++) {
        uint32_t g = k & ~3u, r = k & 3;
        const uint32_t *q = in->q1[g + ((r & 1) ? 2 : 0)];
        out->uv0[k][0] = (float)(int32_t)q[(r & 2) ? 2 : 0] * (1.0f / 4096.0f);
        out->uv0[k][1] = (float)(int32_t)q[(r & 2) ? 3 : 1] * (1.0f / 4096.0f);
    }
    for (c = 0; c < 4; c++)
        out->col1[c] = (uint8_t)vu[1012 * 4 + c];
    return 1;
}

/* nc_shade for a batch without crash deformation, straight from the render
 * data in guest memory (no NcIn copy): the pass 1 colours as GX takes them
 * (RGBA8, GS alpha 0x80 = 1.0 as 255) and, with the sphere map (returns 2),
 * the pass 2 UVs. Returns 0 when the batch is deformed (nc_shade does those). */
/* vertices k0 to k1 of it */
static int nc_shade_direct_c(const uint32_t *vu, const NcBlock *b, uint32_t *col, float (*uv1)[2], uint8_t (*uv8)[2],
                             uint32_t k0, uint32_t k1)
{
    const float *pos = (const float *)GPTR(b->pos) + 4 * k0;
    const uint8_t *ram = ps2_ram;
    uint32_t k, na = b->nrm + 3 * k0, r0 = b->row[0], r1 = b->row[1], r2 = b->row[2];
    const float off = V(970, 3);
    const float m00 = V(966, 0), m01 = V(966, 1), m02 = V(966, 2), m03 = V(966, 3);
    const float m10 = V(967, 0), m11 = V(967, 1), m12 = V(967, 2), m13 = V(967, 3);
    const float m20 = V(968, 0), m21 = V(968, 1), m22 = V(968, 2), m23 = V(968, 3);
    const float m30 = V(969, 0), m31 = V(969, 1), m32 = V(969, 2), m33 = V(969, 3);
    const float fx = V(970, 0), fy = V(970, 1), fz = V(970, 2);
    const float e00 = V(971, 0), e01 = V(971, 1), e02 = V(971, 2), ex0 = V(971, 3);
    const float e10 = V(972, 0), e11 = V(972, 1), e12 = V(972, 2), ey0 = V(972, 3);
    const float e20 = V(973, 0), e21 = V(973, 1), e22 = V(973, 2), ez0 = V(973, 3);
    const float c00 = V(1010, 0), c01 = V(1010, 1), c02 = V(1010, 2);
    const float d0 = V(1011, 0) - V(1010, 0), d1 = V(1011, 1) - V(1010, 1), d2 = V(1011, 2) - V(1010, 2);
    const float d3 = V(1011, 3) - V(1010, 3), amax = V(1011, 3) > 0.0f ? V(1011, 3) : 0.0f;
    const int env = VI(1013, 1) == 0;
#if NC_PSQ
    const float amax2 = amax + amax, d32 = d3 + d3;     /* GS alpha 0x80 is 255 here: twice it, stopped at 255 */
    float Llo, Lhi;
#endif
    if (VI(1013, 2))
        return 0;
    nc_rows(r0, r1, r2, off);
#if NC_PSQ
    nc_gqr();
    {
        const float cc[3] = {c00, c01, c02}, dd[3] = {d0, d1, d2};
        nc_lrange(cc, dd, &Llo, &Lhi);
    }
#endif
    for (k = k0; k < k1; k++, pos += 4, na += 3) {
        const float px = pos[0], py = pos[1], pz = pos[2];
        const uint32_t b0 = ram[(na & PS2_RAM_MASK) ^ PS2_BXOR], b1 = ram[((na + 1) & PS2_RAM_MASK) ^ PS2_BXOR];
        const uint32_t b2 = ram[((na + 2) & PS2_RAM_MASK) ^ PS2_BXOR];
        const float nx = g_nrm_f[0][b0], ny = g_nrm_f[1][b1], nz = g_nrm_f[2][b2];
        const float X = m30 + nx * m00 + ny * m10 + nz * m20;
        const float Y = m31 + nx * m01 + ny * m11 + nz * m21;
        const float Z = m32 + nx * m02 + ny * m12 + nz * m22;
        const float E = fabsf(nx * fx + ny * fy + nz * fz);
        const float F = E + E - E * E;
        const float L = m33 + nx * m03 + nz * m23 + ny * m13 + X * nx + Y * ny + Z * nz;
#if NC_PSQ
        if (__builtin_expect(L > Llo && L < Lhi, 1)) {
            st_u8x2((uint8_t *)&col[k], c00 + d0 * L, c01 + d1 * L);
            st_u8x2((uint8_t *)&col[k] + 2, c02 + d2 * L, amax2 - d32 * F);
        } else {
            const uint32_t a = ftoi_u8_fast(amax - d3 * F);
            col[k] = ftoi_u8_fast(c00 + d0 * L) << 24 | ftoi_u8_fast(c01 + d1 * L) << 16 | ftoi_u8_fast(c02 + d2 * L) << 8 |
                     (a >= 0x80 ? 255 : a * 2);
        }
        if (__builtin_expect(g_nc_psq_check, 0)) {
            const uint32_t a = ftoi_u8_fast(amax - d3 * F);
            const uint32_t want = ftoi_u8_fast(c00 + d0 * L) << 24 | ftoi_u8_fast(c01 + d1 * L) << 16 |
                                  ftoi_u8_fast(c02 + d2 * L) << 8 | (a >= 0x80 ? 255 : a * 2);
            uint32_t c, worst = 0;
            for (c = 0; c < 4; c++) {
                const int dd = (int)((want >> (8 * c)) & 0xFF) - (int)((col[k] >> (8 * c)) & 0xFF);
                const uint32_t ad = (uint32_t)(dd < 0 ? -dd : dd);
                if (ad > g_nc_psq_max[c ? 0 : 1])
                    g_nc_psq_max[c ? 0 : 1] = ad;
                if (ad > worst)
                    worst = ad;
            }
            g_nc_psq_n++;
            g_nc_psq_bad += worst > 1;
        }
#else
        uint32_t a = ftoi_u8_fast(amax - d3 * F);
        col[k] = ftoi_u8_fast(c00 + d0 * L) << 24 | ftoi_u8_fast(c01 + d1 * L) << 16 | ftoi_u8_fast(c02 + d2 * L) << 8 |
                 (a >= 0x80 ? 255 : a * 2);
#endif
        if (env) {
            const float vx = px - ex0, vy = py - ey0, vz = pz - ez0;
            const float dn = 2.0f * (vx * nx + vy * ny + vz * nz);
            const float rx = vx - dn * nx, ry = vy - dn * ny, rz = vz - dn * nz;
            const float ex = rx * e00 + ry * e10 + rz * e20;
            const float ey = rx * e01 + ry * e11 + rz * e21;
            const float l2 = vx * vx + vy * vy + vz * vz;
            const float ez = l2 * rt_rsqrtf(l2) + rx * e02 + ry * e12 + rz * e22;
            const float m = 0.5f * rt_rsqrtf(ex * ex + ey * ey + ez * ez);
            if (uv8) {
                /* 0..256 rounded, 256 as 255 */
#if NC_PSQ
                const float m256 = 256.0f * m;
                st_u8x2(uv8[k], 128.5f + ex * m256, 128.5f + ey * m256);
                if (__builtin_expect(g_nc_psq_check, 0)) {
                    const uint32_t s = (uint32_t)(int32_t)(128.5f + 256.0f * ex * m) & 0x1FF;
                    const uint32_t t = (uint32_t)(int32_t)(128.5f + 256.0f * ey * m) & 0x1FF;
                    const int ds = (int)(s - (s >> 8)) - (int)uv8[k][0], dt = (int)(t - (t >> 8)) - (int)uv8[k][1];
                    const uint32_t ad = (uint32_t)(ds < 0 ? -ds : ds), bd = (uint32_t)(dt < 0 ? -dt : dt);
                    if (ad > g_nc_psq_max[2])
                        g_nc_psq_max[2] = ad;
                    if (bd > g_nc_psq_max[2])
                        g_nc_psq_max[2] = bd;
                    g_nc_psq_bad += ad > 1 || bd > 1;
                }
#else
                const uint32_t s = (uint32_t)(int32_t)(128.5f + 256.0f * ex * m) & 0x1FF;
                const uint32_t t = (uint32_t)(int32_t)(128.5f + 256.0f * ey * m) & 0x1FF;
                uv8[k][0] = (uint8_t)(s - (s >> 8));
                uv8[k][1] = (uint8_t)(t - (t >> 8));
#endif
            } else {
                uv1[k][0] = 0.5f + ex * m;
                uv1[k][1] = 0.5f + ey * m;
            }
        }
    }
    return env ? 2 : 1;
}

#if NC_PSQ
/* The cars' lighting in paired singles: two floats a register, so the
 * light's three rows are six multiply-adds for X, Y, Z and the linear term,
 * the reflection's two UVs come out of one register, and eighteen pairs of
 * constants stay in registers (the C loop, with one float a register,
 * reloaded 37 of them from the stack for every vertex: 165 instructions a
 * vertex, 95 here). Two loops from the same pieces:
 *   nc_shade_ps_loop, for nc_shade_direct: the normal's bytes from guest
 *   memory through the tables, positions 16 bytes apart, the sphere UVs as
 *   bytes;
 *   nc_shade_ps_loop2, for nc_shade_skin: normals and positions as floats
 *   12 bytes apart (skinned already), the UVs as floats.
 * K: the constants (nc_ps_consts fills it); a vertex whose light term is
 * outside (Llo, Lhi) has its red, green and blue converted the old way
 * after the stores (label 4). uv 0: no sphere map. Registers 14..31 are
 * saved as doubles: nothing compiled keeps a second float in one.
 *   K (floats): 0 m00 m01, 2 m10 m11, 4 m20 m21, 6 m30 m31, 8 m02 m03,
 *   10 m12 m13, 12 m22 m23, 14 m32 m33, 16 fx fy, 18 c00 c01, 20 d0 d1,
 *   22 c02 and the alpha's top, 24 d2 and minus the alpha's slope,
 *   28 ex0 ey0, 30 e00 e01, 32 e10 e11, 34 e20 e21, 36 e02 e12, 38 fz,
 *   40 ez0, 41 e22, 42 1, 43 0.5, 44 1.5, 45 the UV's scale (128 for bytes,
 *   0.5), 46 its middle twice (128.5, 0.5), 48 Llo, 49 Lhi, 50 1e-30, 52
 *   the address of the normals' tables (g_nrm_f) */
void nc_shade_ps_loop(const float *K, const float *pos, const uint8_t *ram, uint32_t na, uint32_t n, uint8_t *col,
                      uint8_t *uv8);
void nc_shade_ps_loop2(const float *K, const float *pos, const float *nrm, uint32_t unused, uint32_t n, uint8_t *col,
                       float *uv);

#define NCPS_HEAD(name) \
    "\t.pushsection .text\n" \
    "\t.align 2\n" \
    "\t.globl " name "\n" \
    "\t.type " name ", @function\n" \
    name ":\n" \
    "\tstwu 1,-160(1)\n" \
    "\tstfd 14,16(1)\n\tstfd 15,24(1)\n\tstfd 16,32(1)\n\tstfd 17,40(1)\n\tstfd 18,48(1)\n\tstfd 19,56(1)\n" \
    "\tstfd 20,64(1)\n\tstfd 21,72(1)\n\tstfd 22,80(1)\n\tstfd 23,88(1)\n\tstfd 24,96(1)\n\tstfd 25,104(1)\n" \
    "\tstfd 26,112(1)\n\tstfd 27,120(1)\n\tstfd 28,128(1)\n\tstfd 29,136(1)\n\tstfd 30,144(1)\n\tstfd 31,152(1)\n" \
    "\tlwz 11,208(3)\n" \
    "\tpsq_l 14,0(3),0,0\n\tpsq_l 15,8(3),0,0\n\tpsq_l 16,16(3),0,0\n\tpsq_l 17,24(3),0,0\n" \
    "\tpsq_l 18,32(3),0,0\n\tpsq_l 19,40(3),0,0\n\tpsq_l 20,48(3),0,0\n\tpsq_l 21,56(3),0,0\n" \
    "\tpsq_l 22,64(3),0,0\n\tpsq_l 23,72(3),0,0\n\tpsq_l 24,80(3),0,0\n\tpsq_l 25,88(3),0,0\n" \
    "\tpsq_l 26,96(3),0,0\n\tpsq_l 27,192(3),0,0\n\tpsq_l 28,112(3),0,0\n\tpsq_l 29,120(3),0,0\n" \
    "\tpsq_l 30,128(3),0,0\n\tpsq_l 31,136(3),0,0\n" \
    "\tmtctr 7\n" \
    "1:\n"

/* the normal's three bytes (guest order: the host's byte is at the address xor 3), each through its table:
 * f0 = (nx, ny), f2 = nz */
#define NCPS_NRM_BYTES \
    "\txori 0,6,3\n" \
    "\tlbzx 0,5,0\n" \
    "\taddi 7,6,1\n" \
    "\txori 7,7,3\n" \
    "\tlbzx 7,5,7\n" \
    "\taddi 12,6,2\n" \
    "\txori 12,12,3\n" \
    "\tlbzx 12,5,12\n" \
    "\tslwi 0,0,2\n" \
    "\tslwi 7,7,2\n" \
    "\tslwi 12,12,2\n" \
    "\taddi 7,7,1024\n" \
    "\taddi 12,12,2048\n" \
    "\tlfsx 0,11,0\n" \
    "\tlfsx 1,11,7\n" \
    "\tlfsx 2,11,12\n" \
    "\taddi 6,6,3\n" \
    "\tps_merge00 0,0,1\n" \
    "\tlfs 3,168(3)\n" \
    "\tps_merge00 3,2,3\n"           /* f3 = (nz, 1) */

/* the same from three floats */
#define NCPS_NRM_FLOATS \
    "\tpsq_l 0,0(5),0,0\n" \
    "\tlfs 2,8(5)\n" \
    "\taddi 5,5,12\n" \
    "\tlfs 3,168(3)\n" \
    "\tps_merge00 3,2,3\n"

/* from the host-order copy, four signed bytes a vertex through GQR 5 (bytes with a sign): f0 = (nx, ny), f3 = (nz, 1)
 * (a one-element load sets the second to 1), f2 = nz */
#define NCPS_NRM_S8 \
    "\tpsq_l 0,0(5),0,5\n" \
    "\tpsq_l 3,2(5),1,5\n" \
    "\tfmr 2,3\n" \
    "\taddi 5,5,4\n"

#define NCPS_CORE_A \
    /* X, Y in f4; Z and the linear term in f5; L in f4 */ \
    "\tps_madds0 4,14,0,17\n" \
    "\tps_madds0 5,18,0,21\n" \
    "\tps_madds1 4,15,0,4\n" \
    "\tps_madds1 5,19,0,5\n" \
    "\tps_madds0 4,16,2,4\n" \
    "\tps_madds0 5,20,2,5\n" \
    "\tps_mul 4,4,0\n" \
    "\tps_mul 5,5,3\n" \
    "\tps_sum0 4,4,4,4\n" \
    "\tps_sum0 5,5,5,5\n" \
    "\tfadds 4,4,5\n" \
    /* E = |n . f|, F = 2E - E^2 in f6 */ \
    "\tps_mul 6,0,22\n" \
    "\tlfs 7,152(3)\n" \
    "\tps_sum0 6,6,6,6\n" \
    "\tfmadds 6,2,7,6\n" \
    "\tfabs 6,6\n" \
    "\tfadds 7,6,6\n" \
    "\tfnmsubs 6,6,6,7\n" \
    /* the colour: (c00 + d0 L, c01 + d1 L), (c02 + d2 L, the alpha's top - its slope F) */ \
    "\tps_madds0 7,24,4,23\n" \
    "\tps_merge00 8,4,6\n" \
    "\tps_madd 8,26,8,25\n" \
    "\tpsq_st 7,0(8),0,3\n" \
    "\tpsq_st 8,2(8),0,3\n" \
    "\tlfs 8,196(3)\n" \
    "\tfcmpu 0,4,27\n" \
    "\tfcmpu 1,4,8\n" \
    "\tbng- 0,4f\n" \
    "\tbnl- 1,4f\n" \
    "2:\n" \
    "\tcmpwi 9,0\n" \
    "\tbeq 3f\n" \
    /* v = p - eye in f7 (x, y), f8 (z); v . n, then twice it, in f9; |v|^2 in f10; r = v - 2 (v . n) n in f11, f9 */ \
    "\tpsq_l 7,0(4),0,0\n" \
    "\tlfs 8,8(4)\n" \
    "\tlfs 9,160(3)\n" \
    "\tps_sub 7,7,28\n" \
    "\tfsubs 8,8,9\n" \
    "\tps_mul 9,7,0\n" \
    "\tps_mul 10,7,7\n" \
    "\tps_sum0 9,9,9,9\n" \
    "\tps_sum0 10,10,10,10\n" \
    "\tfmadds 9,8,2,9\n" \
    "\tfmadds 10,8,8,10\n" \
    "\tfadds 9,9,9\n" \
    "\tps_merge00 11,9,9\n" \
    "\tps_nmsub 11,0,11,7\n" \
    "\tfnmsubs 9,2,9,8\n"

/* |v| = |v|^2 / sqrt(|v|^2): the estimate and one step, as rt_rsqrtf (a hair added: 0 stays 0) */
#define NCPS_RSQ1 \
    "\tlfs 12,200(3)\n" \
    "\tfadds 10,10,12\n" \
    "\tfrsqrte 13,10\n" \
    "\tlfs 12,172(3)\n" \
    "\tfmuls 1,13,13\n" \
    "\tfmuls 1,1,10\n" \
    "\tlfs 5,176(3)\n" \
    "\tfnmsubs 1,1,12,5\n" \
    "\tfmuls 13,13,1\n" \
    "\tfmuls 10,10,13\n"

/* the same from the estimate alone (nc_shade_ps_loop4: the UVs are bytes, 8 bits of fraction; the estimate is good to
 * about 1 in 4096) */
#define NCPS_RSQ1_EST \
    "\tlfs 12,200(3)\n" \
    "\tfadds 10,10,12\n" \
    "\tfrsqrte 13,10\n" \
    "\tfmuls 10,10,13\n"

#define NCPS_CORE_B \
    /* (ex, ey) in f1, ez in f5 */ \
    "\tps_muls0 1,29,11\n" \
    "\tpsq_l 5,144(3),0,0\n" \
    "\tps_madds1 1,30,11,1\n" \
    "\tps_mul 5,5,11\n" \
    "\tps_madds0 1,31,9,1\n" \
    "\tlfs 6,164(3)\n" \
    "\tps_sum0 5,5,5,5\n" \
    "\tfmadds 5,9,6,5\n" \
    "\tfadds 5,5,10\n" \
    /* the scale over |e| in f7, the two UVs in f1 */ \
    "\tps_mul 6,1,1\n" \
    "\tps_sum0 6,6,6,6\n" \
    "\tfmadds 6,5,5,6\n" \
    "\tlfs 7,200(3)\n" \
    "\tfadds 6,6,7\n"

/* the scale: 128 / |e| (the estimate and one step), and the UVs */
#define NCPS_RSQ2 \
    "\tfrsqrte 7,6\n" \
    "\tfmuls 8,7,7\n" \
    "\tfmuls 8,8,6\n" \
    "\tlfs 9,176(3)\n" \
    "\tfnmsubs 8,8,12,9\n" \
    "\tfmuls 7,7,8\n" \
    "\tlfs 8,180(3)\n" \
    "\tfmuls 7,7,8\n"
#define NCPS_RSQ2_EST \
    "\tfrsqrte 7,6\n" \
    "\tlfs 8,180(3)\n" \
    "\tfmuls 7,7,8\n"
#define NCPS_UV \
    "\tpsq_l 8,184(3),0,0\n" \
    "\tps_madds0 1,1,7,8\n"

#define NCPS_CORE NCPS_CORE_A NCPS_RSQ1 NCPS_CORE_B NCPS_RSQ2 NCPS_UV
#define NCPS_CORE_EST NCPS_CORE_A NCPS_RSQ1_EST NCPS_CORE_B NCPS_RSQ2_EST NCPS_UV

/* the UVs out (GQR 3: bytes; 0: floats), the next vertex (the positions' stride), the end, and the vertex whose
 * light term is out of range: red, green and blue as the conversion has them (the integer's low byte) */
#define NCPS_TAIL(name, gqr, uvstep, posstep) \
    "\tpsq_st 1,0(9),0," gqr "\n" \
    "\taddi 9,9," uvstep "\n" \
    "3:\n" \
    "\taddi 4,4," posstep "\n" \
    "\taddi 8,8,4\n" \
    "\tbdnz 1b\n" \
    "\tlfd 14,16(1)\n\tlfd 15,24(1)\n\tlfd 16,32(1)\n\tlfd 17,40(1)\n\tlfd 18,48(1)\n\tlfd 19,56(1)\n" \
    "\tlfd 20,64(1)\n\tlfd 21,72(1)\n\tlfd 22,80(1)\n\tlfd 23,88(1)\n\tlfd 24,96(1)\n\tlfd 25,104(1)\n" \
    "\tlfd 26,112(1)\n\tlfd 27,120(1)\n\tlfd 28,128(1)\n\tlfd 29,136(1)\n\tlfd 30,144(1)\n\tlfd 31,152(1)\n" \
    "\taddi 1,1,160\n" \
    "\tblr\n" \
    "4:\n" \
    "\tps_madds0 7,24,4,23\n" \
    "\tfmadds 8,26,4,25\n" \
    "\tfctiwz 12,7\n" \
    "\tstfd 12,8(1)\n" \
    "\tlbz 12,15(1)\n" \
    "\tstb 12,0(8)\n" \
    "\tps_merge11 7,7,7\n" \
    "\tfctiwz 12,7\n" \
    "\tstfd 12,8(1)\n" \
    "\tlbz 12,15(1)\n" \
    "\tstb 12,1(8)\n" \
    "\tfctiwz 12,8\n" \
    "\tstfd 12,8(1)\n" \
    "\tlbz 12,15(1)\n" \
    "\tstb 12,2(8)\n" \
    "\tb 2b\n" \
    "\t.size " name ", .-" name "\n" \
    "\t.popsection\n"

__asm__(NCPS_HEAD("nc_shade_ps_loop") NCPS_NRM_BYTES NCPS_CORE NCPS_TAIL("nc_shade_ps_loop", "3", "2", "16"));
__asm__(NCPS_HEAD("nc_shade_ps_loop2") NCPS_NRM_FLOATS NCPS_CORE NCPS_TAIL("nc_shade_ps_loop2", "0", "8", "12"));
__asm__(NCPS_HEAD("nc_shade_ps_loop3") NCPS_NRM_S8 NCPS_CORE NCPS_TAIL("nc_shade_ps_loop3", "3", "2", "16"));
void nc_shade_ps_loop3(const float *K, const float *pos, const int8_t *nrm8, uint32_t unused, uint32_t n, uint8_t *col,
                       uint8_t *uv8);
/* loop3 with the sphere map's two reciprocal square roots from the estimate alone (11 instructions a vertex fewer): the
 * UVs are bytes with 8 bits of fraction and the estimate is good to about 1 in 4096, so a UV byte moves by one now and
 * then; the colours are the same. Autopilot "carest 0": loop3. */
__asm__(NCPS_HEAD("nc_shade_ps_loop4") NCPS_NRM_S8 NCPS_CORE_EST NCPS_TAIL("nc_shade_ps_loop4", "3", "2", "16"));
void nc_shade_ps_loop4(const float *K, const float *pos, const int8_t *nrm8, uint32_t unused, uint32_t n, uint8_t *col,
                       uint8_t *uv8);
/* (autopilot "carest 2": |v| with its step, the scale from the estimate alone) */
__asm__(NCPS_HEAD("nc_shade_ps_loop5") NCPS_NRM_S8 NCPS_CORE_A NCPS_RSQ1 NCPS_CORE_B NCPS_RSQ2_EST NCPS_UV
        NCPS_TAIL("nc_shade_ps_loop5", "3", "2", "16"));
void nc_shade_ps_loop5(const float *K, const float *pos, const int8_t *nrm8, uint32_t unused, uint32_t n, uint8_t *col,
                       uint8_t *uv8);
/* Two vertices a register (nc_shade_ps_pairs): lane 0 vertex 2i, lane 1 vertex 2i + 1; the normals' and positions'
 * components transposed by merges after the loads. Each lane is loop5's arithmetic (loop3's without a sphere map):
 * the same operations, fused where loop5 fuses (ps_madds0/1 take the packed constants of K as the scalar, so X =
 * nz m20 + (ny m10 + (nx m00 + m30)) as ps_madds0 4,14,0,17 and the two after it make it), the sums in loop5's
 * order, and the two reciprocal square roots per lane in scalar instructions: frsqrte's estimate is a double with
 * more bits than a pair's second slot keeps, and loop5 multiplies it as such. A constant added to a pair is loaded
 * with lfs, which fills both slots (checked once: nc_pairs_ok). A pair with a light term out of (Llo, Lhi) (its
 * colours wrap) has its red, green and blue made again as loop5's label 4 makes them. An odd batch's last pair has
 * a second vertex past the end: its inputs are readable (the render data in guest RAM, the normals' copy rounded up
 * to 32 bytes) and its outputs land in the arrays' rounding (litcol_alloc's 32 bytes), unused.
 *   K: nc_ps_consts' block (offsets in bytes below); r4 positions, r5 the normals' copy (4 bytes a vertex, GQR 5),
 *   r6 the colours, r7 pairs, r8 the UV bytes or 0 (no sphere map: loop3's); returns the pairs made again. */
uint32_t nc_shade_ps_pairs(const float *K, const float *pos, const int8_t *nrm8, uint32_t *col, uint32_t npairs,
                           uint8_t *uv8);

/* how far ahead (bytes) the loop touches the positions (32 a pair) and the normals (8 a pair); only where the console's
 * cache hints are on (g_dcbz_on: Dolphin's JIT pays for every touch) */
#define NCPS_PF_POS "64"
#define NCPS_PF_NRM "64"
__asm__("\t.pushsection .text\n"
        "\t.align 2\n"
        "\t.globl nc_shade_ps_pairs\n"
        "\t.type nc_shade_ps_pairs, @function\n"
        "nc_shade_ps_pairs:\n"
        "\tstwu 1,-160(1)\n"
        "\tstfd 14,16(1)\n\tstfd 15,24(1)\n\tstfd 16,32(1)\n\tstfd 17,40(1)\n\tstfd 18,48(1)\n\tstfd 19,56(1)\n"
        "\tstfd 20,64(1)\n\tstfd 21,72(1)\n\tstfd 22,80(1)\n\tstfd 23,88(1)\n\tstfd 24,96(1)\n\tstfd 25,104(1)\n"
        "\tstfd 26,112(1)\n\tstfd 27,120(1)\n\tstfd 28,128(1)\n\tstfd 29,136(1)\n\tstfd 30,144(1)\n\tstfd 31,152(1)\n"
        "\tpsq_l 14,0(3),0,0\n"           /* (m00, m01) */
        "\tpsq_l 15,8(3),0,0\n"           /* (m10, m11) */
        "\tpsq_l 16,16(3),0,0\n"          /* (m20, m21) */
        "\tpsq_l 17,32(3),0,0\n"          /* (m02, m03) */
        "\tpsq_l 18,40(3),0,0\n"          /* (m12, m13) */
        "\tpsq_l 19,48(3),0,0\n"          /* (m22, m23) */
        "\tpsq_l 20,64(3),0,0\n"          /* (fx, fy) */
        "\tpsq_l 21,184(3),0,0\n"         /* (128.5, 128.5) */
        "\tpsq_l 22,80(3),0,0\n"          /* (d0, d1) */
        "\tpsq_l 23,96(3),0,0\n"          /* (d2, -2 d3) */
        "\tpsq_l 24,120(3),0,0\n"         /* (e00, e01) */
        "\tpsq_l 25,128(3),0,0\n"         /* (e10, e11) */
        "\tpsq_l 26,136(3),0,0\n"         /* (e20, e21) */
        "\tpsq_l 27,144(3),0,0\n"         /* (e02, e12) */
        "\tlfs 28,172(3)\n"               /* 0.5 */
        "\tlfs 29,176(3)\n"               /* 1.5 */
        "\tlfs 30,180(3)\n"               /* 128 */
        "\tlfs 31,200(3)\n"               /* the hair, both slots */
        "\tmtctr 7\n"
        "\tli 9,0\n"
        "\tlis 12,g_dcbz_on@ha\n\tlwz 12,g_dcbz_on@l(12)\n"
        "1:\n"
        /* the streams a few pairs ahead into the cache (hints; Broadway has one line fill outstanding, so no deeper:
         * a pair's positions are a whole line, the normals' line lasts four pairs) */
        "\tcmpwi 12,0\n\tbeq 5f\n"
        "\taddi 10,4," NCPS_PF_POS "\n\tdcbt 0,10\n"
        "\taddi 11,5," NCPS_PF_NRM "\n\tdcbt 0,11\n"
        "5:\n"
        /* the normals: f4 = nx, f5 = ny, f6 = nz of the two */
        "\tpsq_l 0,0(5),0,5\n"
        "\tpsq_l 1,4(5),0,5\n"
        "\tpsq_l 2,2(5),1,5\n"
        "\tpsq_l 3,6(5),1,5\n"
        "\tps_merge00 4,0,1\n"
        "\tps_merge11 5,0,1\n"
        "\tps_merge00 6,2,3\n"
        /* X, Y in f7, f8; Z, W in f9, f10; L in f7 */
        "\tlfs 7,24(3)\n"                 /* m30 */
        "\tlfs 8,28(3)\n"                 /* m31 */
        "\tps_madds0 7,4,14,7\n"
        "\tps_madds1 8,4,14,8\n"
        "\tps_madds0 7,5,15,7\n"
        "\tps_madds1 8,5,15,8\n"
        "\tps_madds0 7,6,16,7\n"
        "\tps_madds1 8,6,16,8\n"
        "\tlfs 9,56(3)\n"                 /* m32 */
        "\tlfs 10,60(3)\n"                /* m33 */
        "\tps_madds0 9,4,17,9\n"
        "\tps_madds1 10,4,17,10\n"
        "\tps_madds0 9,5,18,9\n"
        "\tps_madds1 10,5,18,10\n"
        "\tps_madds0 9,6,19,9\n"
        "\tps_madds1 10,6,19,10\n"
        "\tps_mul 7,7,4\n"
        "\tps_mul 8,8,5\n"
        "\tps_mul 9,9,6\n"
        "\tps_add 7,7,8\n"
        "\tps_add 9,9,10\n"
        "\tps_add 7,7,9\n"
        /* F = 2E - E^2, E = |n . f|, in f8 */
        "\tlfs 11,152(3)\n"               /* fz */
        "\tps_muls0 8,4,20\n"
        "\tps_muls1 9,5,20\n"
        "\tps_add 8,8,9\n"
        "\tps_madd 8,6,11,8\n"
        "\tps_abs 8,8\n"
        "\tps_add 9,8,8\n"
        "\tps_nmsub 8,8,8,9\n"
        /* the colours: R, G, B, A in f9..f12 */
        "\tlfs 9,72(3)\n"                 /* c00 */
        "\tlfs 10,76(3)\n"                /* c01 */
        "\tlfs 11,88(3)\n"                /* c02 */
        "\tlfs 12,92(3)\n"                /* the alpha's top, twice */
        "\tps_madds0 9,7,22,9\n"
        "\tps_madds1 10,7,22,10\n"
        "\tps_madds0 11,7,23,11\n"
        "\tps_madds1 12,8,23,12\n"
        "\tps_merge00 0,9,10\n"
        "\tps_merge00 1,11,12\n"
        "\tps_merge11 2,9,10\n"
        "\tps_merge11 3,11,12\n"
        "\tpsq_st 0,0(6),0,3\n"
        "\tpsq_st 1,2(6),0,3\n"
        "\tpsq_st 2,4(6),0,3\n"
        "\tpsq_st 3,6(6),0,3\n"
        /* both light terms in (Llo, Lhi), else red, green and blue as the conversion has them (label 4) */
        "\tlfs 0,192(3)\n"
        "\tlfs 1,196(3)\n"
        "\tps_cmpu0 0,7,0\n"
        "\tps_cmpu1 1,7,0\n"
        "\tps_cmpu0 5,7,1\n"
        "\tps_cmpu1 6,7,1\n"
        "\tcrand 1,1,5\n"
        "\tcrand 20,20,24\n"
        "\tcrand 1,1,20\n"
        "\tbf- 1,4f\n"
        "2:\n"
        "\tcmpwi 8,0\n"
        "\tbeq 3f\n"
        /* v = p - eye in f7, f8, f9 */
        "\tpsq_l 0,0(4),0,0\n"
        "\tpsq_l 1,16(4),0,0\n"
        "\tlfs 2,8(4)\n"
        "\tlfs 3,24(4)\n"
        "\tps_merge00 7,0,1\n"
        "\tps_merge11 8,0,1\n"
        "\tps_merge00 9,2,3\n"
        "\tlfs 0,112(3)\n"                /* ex0 */
        "\tlfs 1,116(3)\n"                /* ey0 */
        "\tlfs 2,160(3)\n"                /* ez0 */
        "\tps_sub 7,7,0\n"
        "\tps_sub 8,8,1\n"
        "\tps_sub 9,9,2\n"
        /* v . n in f0, |v|^2 in f2; twice v . n; r = v - 2 (v . n) n in f10, f11, f12 */
        "\tps_mul 0,7,4\n"
        "\tps_mul 1,8,5\n"
        "\tps_mul 2,7,7\n"
        "\tps_mul 3,8,8\n"
        "\tps_add 0,0,1\n"
        "\tps_add 2,2,3\n"
        "\tps_madd 0,9,6,0\n"
        "\tps_madd 2,9,9,2\n"
        "\tps_add 0,0,0\n"
        "\tps_nmsub 10,4,0,7\n"
        "\tps_nmsub 11,5,0,8\n"
        "\tps_nmsub 12,6,0,9\n"
        /* |v| = |v|^2 / sqrt(|v|^2) in f13, lane by lane as loop5 */
        "\tps_add 2,2,31\n"
        "\tfrsqrte 3,2\n"
        "\tfmuls 4,3,3\n"
        "\tfmuls 4,4,2\n"
        "\tfnmsubs 4,4,28,29\n"
        "\tfmuls 3,3,4\n"
        "\tfmuls 3,2,3\n"
        "\tps_merge11 5,2,2\n"
        "\tfrsqrte 6,5\n"
        "\tfmuls 7,6,6\n"
        "\tfmuls 7,7,5\n"
        "\tfnmsubs 7,7,28,29\n"
        "\tfmuls 6,6,7\n"
        "\tfmuls 6,5,6\n"
        "\tps_merge00 13,3,6\n"
        /* (ex, ey) in f0, f1; ez in f2 */
        "\tps_muls0 0,10,24\n"
        "\tps_muls1 1,10,24\n"
        "\tps_madds0 0,11,25,0\n"
        "\tps_madds1 1,11,25,1\n"
        "\tps_madds0 0,12,26,0\n"
        "\tps_madds1 1,12,26,1\n"
        "\tlfs 4,164(3)\n"                /* e22 */
        "\tps_muls0 2,10,27\n"
        "\tps_muls1 3,11,27\n"
        "\tps_add 2,2,3\n"
        "\tps_madd 2,12,4,2\n"
        "\tps_add 2,2,13\n"
        /* |e|^2 and the hair in f3; 128 / |e| from the estimate, lane by lane, in f4 */
        "\tps_mul 3,0,0\n"
        "\tps_mul 4,1,1\n"
        "\tps_add 3,3,4\n"
        "\tps_madd 3,2,2,3\n"
        "\tps_add 3,3,31\n"
        "\tfrsqrte 4,3\n"
        "\tfmuls 4,4,30\n"
        "\tps_merge11 5,3,3\n"
        "\tfrsqrte 5,5\n"
        "\tfmuls 5,5,30\n"
        "\tps_merge00 4,4,5\n"
        /* the UVs */
        "\tps_madd 0,0,4,21\n"
        "\tps_madd 1,1,4,21\n"
        "\tps_merge00 2,0,1\n"
        "\tps_merge11 3,0,1\n"
        "\tpsq_st 2,0(8),0,3\n"
        "\tpsq_st 3,2(8),0,3\n"
        "\taddi 8,8,4\n"
        "3:\n"
        "\taddi 4,4,32\n"
        "\taddi 5,5,8\n"
        "\taddi 6,6,8\n"
        "\tbdnz 1b\n"
        "\tmr 3,9\n"
        "\tlfd 14,16(1)\n\tlfd 15,24(1)\n\tlfd 16,32(1)\n\tlfd 17,40(1)\n\tlfd 18,48(1)\n\tlfd 19,56(1)\n"
        "\tlfd 20,64(1)\n\tlfd 21,72(1)\n\tlfd 22,80(1)\n\tlfd 23,88(1)\n\tlfd 24,96(1)\n\tlfd 25,104(1)\n"
        "\tlfd 26,112(1)\n\tlfd 27,120(1)\n\tlfd 28,128(1)\n\tlfd 29,136(1)\n\tlfd 30,144(1)\n\tlfd 31,152(1)\n"
        "\taddi 1,1,160\n"
        "\tblr\n"
        "4:\n"                            /* out of range: the integers' low bytes, both vertices (loop5's label 4; in */
        "\taddi 9,9,1\n"                  /* range the store's byte is the same) */
        "\tfctiwz 12,9\n\tstfd 12,8(1)\n\tlbz 0,15(1)\n\tstb 0,0(6)\n"
        "\tfctiwz 12,10\n\tstfd 12,8(1)\n\tlbz 0,15(1)\n\tstb 0,1(6)\n"
        "\tfctiwz 12,11\n\tstfd 12,8(1)\n\tlbz 0,15(1)\n\tstb 0,2(6)\n"
        "\tps_merge11 9,9,9\n"
        "\tps_merge11 10,10,10\n"
        "\tps_merge11 11,11,11\n"
        "\tfctiwz 12,9\n\tstfd 12,8(1)\n\tlbz 0,15(1)\n\tstb 0,4(6)\n"
        "\tfctiwz 12,10\n\tstfd 12,8(1)\n\tlbz 0,15(1)\n\tstb 0,5(6)\n"
        "\tfctiwz 12,11\n\tstfd 12,8(1)\n\tlbz 0,15(1)\n\tstb 0,6(6)\n"
        "\tb 2b\n"
        "\t.size nc_shade_ps_pairs, .-nc_shade_ps_pairs\n"
        "\t.popsection\n");

/* does lfs fill both slots of a register (as nc_shade_ps_pairs takes it)? */
static int nc_pairs_ok(void)
{
    static int ok = -1;
    if (ok < 0) {
        static const float one = 3.0f;
        float out[2] __attribute__((aligned(8)));
        __asm__ volatile("lfs 0,0(%1)\n\tpsq_st 0,0(%0),0,0" : : "b"(out), "b"(&one) : "fr0", "memory");
        ok = out[0] == 3.0f && out[1] == 3.0f;
        LOG("car shading two vertices a register: %s", ok ? "lfs fills both slots" : "lfs does not fill both slots: not used");
    }
    return ok;
}

int g_carpair = 1;                      /* autopilot "carpair 0": loop5 and loop3 vertex by vertex */
/* status line: batches through the pairs, pairs listed (light term out of range); and the check against loop5 /
 * loop3 (the first 2000 batches and one in 64 after): batches compared, colour words and UV bytes different */
uint32_t g_carpair_batches, g_carpair_redo, g_carpair_checked, g_carpair_coldiff, g_carpair_uvdiff;
int g_carest = 2;                       /* 1: both roots from the estimate (UV bytes up to 90 apart at the rim), 2: the scale only */
/* status line: batches compared with loop3 (the first 2000 and one in 64 after), their UV bytes, those different and
 * the most they were apart; colour words different (must stay 0) */
uint32_t g_carest_batches, g_carest_uv, g_carest_uvdiff, g_carest_uvmax, g_carest_coldiff, g_carest_hist[4];

int g_nc_ps = 1;                /* 0: the loops in C (the backend's gxflags2 32768) */
int g_nc_ps_check;              /* debugging (gxflags2 16384): the C loop too, and how far apart */
uint32_t g_nc_ps_n, g_nc_ps_bad, g_nc_ps_max[3];

/* the loops' constants from VU memory. skin: for nc_shade_ps_loop2 (alpha as the GS has it, UVs as floats).
 * lkey: the colours the range in K[48], K[49] is for (six divisions: a car's batches have the same colours one
 * after the other). 0: the loops can not be used (GQR 0 is not floats). */
static int nc_ps_consts(const uint32_t *vu, float *K, float *lkey, int skin)
{
    const float d3 = V(1011, 3) - V(1010, 3), amax = V(1011, 3) > 0.0f ? V(1011, 3) : 0.0f;
    float cc[3], dd[3];
    uint32_t j, gqr0;
    __asm__("mfspr %0,912" : "=r"(gqr0));       /* the pairs are loaded through it */
    if (gqr0)
        return 0;
    for (j = 0; j < 4; j++) {
        K[2 * j] = V(966 + j, 0);
        K[2 * j + 1] = V(966 + j, 1);
        K[8 + 2 * j] = V(966 + j, 2);
        K[9 + 2 * j] = V(966 + j, 3);
    }
    K[16] = V(970, 0);
    K[17] = V(970, 1);
    for (j = 0; j < 3; j++) {
        cc[j] = V(1010, j);
        dd[j] = V(1011, j) - V(1010, j);
    }
    K[18] = cc[0];
    K[19] = cc[1];
    K[20] = dd[0];
    K[21] = dd[1];
    K[22] = cc[2];
    K[23] = skin ? amax : amax + amax;          /* GS alpha 0x80 is 255 for the direct colours: twice it, stopped at 255 */
    K[24] = dd[2];
    K[25] = skin ? -d3 : -(d3 + d3);
    for (j = 0; j < 3; j++) {
        K[30 + 2 * j] = V(971 + j, 0);
        K[31 + 2 * j] = V(971 + j, 1);
    }
    K[28] = V(971, 3);
    K[29] = V(972, 3);
    K[36] = V(971, 2);
    K[37] = V(972, 2);
    K[38] = V(970, 2);
    K[40] = V(973, 3);
    K[41] = V(973, 2);
    K[42] = 1.0f;
    K[43] = 0.5f;
    K[44] = 1.5f;
    K[45] = skin ? 0.5f : 128.0f;
    K[46] = K[47] = skin ? 0.5f : 128.5f;
    if (memcmp(lkey, cc, 12) || memcmp(lkey + 3, dd, 12) || K[50] == 0.0f) {
        nc_lrange(cc, dd, &K[48], &K[49]);
        memcpy(lkey, cc, 12);
        memcpy(lkey + 3, dd, 12);
    }
    K[50] = 1.0e-30f;
    *(uint32_t *)&K[52] = (uint32_t)g_nrm_f;
    nc_gqr();
    return 1;
}
#endif

int nc_shade_direct(const uint32_t *vu, const NcBlock *b, uint32_t *col, float (*uv1)[2], uint8_t (*uv8)[2])
{
    return nc_shade_direct8(vu, b, col, uv1, uv8, NULL);
}

int g_nc_s8_used;                       /* status line: batches whose normals came from the host-order copy */

int nc_shade_direct8(const uint32_t *vu, const NcBlock *b, uint32_t *col, float (*uv1)[2], uint8_t (*uv8)[2],
                     const int8_t *nrm8)
{
#if NC_PSQ
    const uint32_t n = b->n, na = b->nrm & PS2_RAM_MASK;
    const int env = VI(1013, 1) == 0;
    static float K[54] __attribute__((aligned(8)));
    static float lkey[6];
    if (g_nc_ps && !VI(1013, 2) && n && n <= NC_MAX && (!env || uv8) && na + 3 * n + 8 < PS2_RAM_SIZE &&
        (b->pos & PS2_RAM_MASK) + 16 * n + 16 < PS2_RAM_SIZE && nc_ps_consts(vu, K, lkey, 0)) {
        nc_rows(b->row[0], b->row[1], b->row[2], V(970, 3));
        extern int g_opt_new;
        if (nrm8 && g_nrm_s8 && g_opt_new) {
            __asm__ volatile("mtspr 917,%0" : : "r"(g_nrm_s8));        /* GQR 5: loads of bytes with a sign, scaled */
            if (g_carpair && (!env || g_carest == 2) && nc_pairs_ok()) {
                /* two vertices a register (an odd batch's last pair padded: see nc_shade_ps_pairs) */
                void (*one)(const float *, const float *, const int8_t *, uint32_t, uint32_t, uint8_t *, uint8_t *) =
                    env ? nc_shade_ps_loop5 : nc_shade_ps_loop3;
                const float *pos = (const float *)GPTR(b->pos);
                g_carpair_redo += nc_shade_ps_pairs(K, pos, nrm8, col, (n + 1) / 2, env ? &uv8[0][0] : NULL);
                if (g_carpair_checked < 2000 || !(++g_carpair_batches & 63)) {
                    static uint32_t tc[NC_MAX];
                    static uint8_t tu[NC_MAX][2];
                    uint32_t k;
                    g_carpair_checked++;
                    one(K, pos, nrm8, 0, n, (uint8_t *)tc, env ? &tu[0][0] : NULL);
                    for (k = 0; k < n; k++) {
                        g_carpair_coldiff += tc[k] != col[k];
                        g_carpair_uvdiff += env && (tu[k][0] != uv8[k][0] || tu[k][1] != uv8[k][1]);
                    }
                }
            } else if (g_carest && env) {
                static uint32_t calls;
                (g_carest == 2 ? nc_shade_ps_loop5 : nc_shade_ps_loop4)(K, (const float *)GPTR(b->pos), nrm8, 0, n,
                                                                        (uint8_t *)col, (uint8_t *)uv8);
                if (g_carest_batches < 2000 || !(++calls & 63)) {
                    static uint32_t tc[NC_MAX];
                    static uint8_t tu[NC_MAX][2];
                    uint32_t k, c;
                    g_carest_batches++;
                    nc_shade_ps_loop3(K, (const float *)GPTR(b->pos), nrm8, 0, n, (uint8_t *)tc, &tu[0][0]);
                    for (k = 0; k < n; k++) {
                        g_carest_coldiff += tc[k] != col[k];
                        for (c = 0; c < 2; c++) {
                            const int e = (int)tu[k][c] - (int)uv8[k][c];
                            const uint32_t a = (uint32_t)(e < 0 ? -e : e);
                            g_carest_uv++;
                            g_carest_uvdiff += a != 0;
                            g_carest_hist[a < 3 ? a : 3]++;
                            if (a > g_carest_uvmax)
                                g_carest_uvmax = a;
                        }
                    }
                }
            } else {
                nc_shade_ps_loop3(K, (const float *)GPTR(b->pos), nrm8, 0, n, (uint8_t *)col, env ? (uint8_t *)uv8 : NULL);
            }
            g_nc_s8_used++;
            {
                /* the first 300 batches through the table too: the same bytes, colours and UVs */
                static uint32_t checked, bad;
                if (checked < 300 && !(g_carest && env) && !g_carpair) {
                    static uint32_t tc[NC_MAX];
                    static uint8_t tu[NC_MAX][2];
                    checked++;
                    nc_shade_ps_loop(K, (const float *)GPTR(b->pos), ps2_ram, na, n, (uint8_t *)tc, env ? &tu[0][0] : NULL);
                    if (memcmp(tc, col, n * 4) || (env && memcmp(tu, uv8, n * 2)))
                        bad++;
                    if (checked == 300)
                        LOG("car normals' copies: 300 batches checked against the table (%u differ)", bad);
                }
            }
        } else {
            nc_shade_ps_loop(K, (const float *)GPTR(b->pos), ps2_ram, na, n, (uint8_t *)col, env ? (uint8_t *)uv8 : NULL);
        }
        if (__builtin_expect(g_nc_ps_check, 0)) {
            static uint32_t tc[NC_MAX];
            static uint8_t tu[NC_MAX][2];
            uint32_t k, c;
            nc_shade_direct_c(vu, b, tc, uv1, tu, 0, n);
            for (k = 0; k < n; k++) {
                uint32_t worst = 0;
                for (c = 0; c < 4; c++) {
                    const int e = (int)((tc[k] >> (8 * c)) & 0xFF) - (int)((col[k] >> (8 * c)) & 0xFF);
                    const uint32_t a = (uint32_t)(e < 0 ? -e : e);
                    if (a > g_nc_ps_max[c ? 0 : 1])
                        g_nc_ps_max[c ? 0 : 1] = a;
                    if (a > worst)
                        worst = a;
                }
                for (c = 0; env && c < 2; c++) {
                    const int e = (int)tu[k][c] - (int)uv8[k][c];
                    const uint32_t a = (uint32_t)(e < 0 ? -e : e);
                    if (a > g_nc_ps_max[2])
                        g_nc_ps_max[2] = a;
                    if (a > worst)
                        worst = a;
                }
                g_nc_ps_n++;
                g_nc_ps_bad += worst > 1;
            }
        }
        return env ? 2 : 1;
    }
#endif
    return nc_shade_direct_c(vu, b, col, uv1, uv8, 0, b->n);
}

static int nc_shade_skin_c(const uint32_t *vu, const NcBlock *b, NcOut *out)
{
    const float *pos = (const float *)GPTR(b->pos);
    const uint32_t *posw = (const uint32_t *)GPTR(b->pos);
    const uint8_t *ram = ps2_ram;
    uint32_t n = b->n, g, r, j, c, k, na = b->nrm, r0 = b->row[0], r1 = b->row[1], r2 = b->row[2];
    const float off = V(970, 3);
    const float m00 = V(966, 0), m01 = V(966, 1), m02 = V(966, 2), m03 = V(966, 3);
    const float m10 = V(967, 0), m11 = V(967, 1), m12 = V(967, 2), m13 = V(967, 3);
    const float m20 = V(968, 0), m21 = V(968, 1), m22 = V(968, 2), m23 = V(968, 3);
    const float m30 = V(969, 0), m31 = V(969, 1), m32 = V(969, 2), m33 = V(969, 3);
    const float fx = V(970, 0), fy = V(970, 1), fz = V(970, 2);
    const float e00 = V(971, 0), e01 = V(971, 1), e02 = V(971, 2), ex0 = V(971, 3);
    const float e10 = V(972, 0), e11 = V(972, 1), e12 = V(972, 2), ey0 = V(972, 3);
    const float e20 = V(973, 0), e21 = V(973, 1), e22 = V(973, 2), ez0 = V(973, 3);
    const float c00 = V(1010, 0), c01 = V(1010, 1), c02 = V(1010, 2);
    const float d0 = V(1011, 0) - V(1010, 0), d1 = V(1011, 1) - V(1010, 1), d2 = V(1011, 2) - V(1010, 2);
    const float d3 = V(1011, 3) - V(1010, 3), amax = V(1011, 3) > 0.0f ? V(1011, 3) : 0.0f;
    const int env = VI(1013, 1) == 0;
#if NC_PSQ
    float Llo, Lhi;
#endif
    if (b->uvstep != 1 || !VI(1013, 2) || n > NC_MAX)
        return 0;                       /* the weights are with the odd vertices */
    nc_rows(r0, r1, r2, off);       /* the normals' bytes as nc_shade_direct has them */
    out->n = n;
    out->env = env;
#if NC_PSQ
    nc_gqr();
    {
        const float cc[3] = {c00, c01, c02}, dd[3] = {d0, d1, d2};
        nc_lrange(cc, dd, &Llo, &Lhi);
    }
#endif
    for (g = 0; g < n; g += 4) {
        uint32_t qa[4] = {0, 0, 0, 0}, qb[4] = {0, 0, 0, 0};
        if (g + 1 < n) {
            slot1(b, g + 1, qa);
            slot1(b, g + 3 < n ? g + 3 : g + 1, qb);
        }
        for (r = 0; r < 4 && g + r < n; r++, na += 3) {
            float px, py, pz, nx, ny, nz;
            k = g + r;
            px = pos[4 * k];
            py = pos[4 * k + 1];
            pz = pos[4 * k + 2];
            {
                const uint32_t b0 = ram[(na & PS2_RAM_MASK) ^ PS2_BXOR], b1 = ram[((na + 1) & PS2_RAM_MASK) ^ PS2_BXOR];
                const uint32_t b2 = ram[((na + 2) & PS2_RAM_MASK) ^ PS2_BXOR];
                nx = g_nrm_f[0][b0];
                ny = g_nrm_f[1][b1];
                nz = g_nrm_f[2][b2];
            }
            if (qa[r] | qb[r]) {
                const uint32_t ia = 974 + (qa[r] & 0x3FF), ib = 974 + (qb[r] & 0x3FF);
                /* a weight is 16 bits: as a signed word it converts without the unsigned's detour, and times
                 * 1/65535 it is within a last bit of the division */
                const float wa = (float)(int32_t)qa[r] * (1.0f / 65535.0f), wb = (float)(int32_t)qb[r] * (1.0f / 65535.0f);
                const float w0 = 1.0f - wa - wb;
                float m[4][3], p[3], q[3];
                if (ia + 3 < 1024 && ib + 3 < 1024) {
                    /* the two bones' rows as floats where they are, not word by word through the bit cast */
                    const float *ma = (const float *)(vu + ia * 4), *mb = (const float *)(vu + ib * 4);
                    for (j = 0; j < 4; j++, ma += 4, mb += 4) {
                        m[j][0] = ma[0] * wa + mb[0] * wb;
                        m[j][1] = ma[1] * wa + mb[1] * wb;
                        m[j][2] = ma[2] * wa + mb[2] * wb;
                    }
                    m[0][0] += w0;
                    m[1][1] += w0;
                    m[2][2] += w0;
                } else
                for (j = 0; j < 4; j++)
                    for (c = 0; c < 3; c++)
                        m[j][c] = V((ia + j) & 1023, c) * wa + V((ib + j) & 1023, c) * wb + (j == c ? w0 : 0.0f);
                for (c = 0; c < 3; c++) {
                    p[c] = m[0][c] * px + m[1][c] * py + m[2][c] * pz + m[3][c];
                    q[c] = m[0][c] * nx + m[1][c] * ny + m[2][c] * nz;
                }
                px = p[0];
                py = p[1];
                pz = p[2];
                /* the microprogram keeps the normal with its offset: added and taken off again */
                nx = (q[0] + off) - off;
                ny = (q[1] + off) - off;
                nz = (q[2] + off) - off;
            }
            {
                const float X = m30 + nx * m00 + ny * m10 + nz * m20;
                const float Y = m31 + nx * m01 + ny * m11 + nz * m21;
                const float Z = m32 + nx * m02 + ny * m12 + nz * m22;
                const float E = fabsf(nx * fx + ny * fy + nz * fz);
                const float F = E + E - E * E;
                const float L = m33 + nx * m03 + nz * m23 + ny * m13 + X * nx + Y * ny + Z * nz;
#if NC_PSQ
                if (__builtin_expect(L > Llo && L < Lhi, 1)) {
                    st_u8x2(out->col[k], c00 + d0 * L, c01 + d1 * L);
                    st_u8x2(out->col[k] + 2, c02 + d2 * L, amax - d3 * F);
                } else {
                    out->col[k][0] = (uint8_t)ftoi_u8_fast(c00 + d0 * L);
                    out->col[k][1] = (uint8_t)ftoi_u8_fast(c01 + d1 * L);
                    out->col[k][2] = (uint8_t)ftoi_u8_fast(c02 + d2 * L);
                    out->col[k][3] = (uint8_t)ftoi_u8_fast(amax - d3 * F);
                }
#else
                out->col[k][0] = (uint8_t)ftoi_u8_fast(c00 + d0 * L);
                out->col[k][1] = (uint8_t)ftoi_u8_fast(c01 + d1 * L);
                out->col[k][2] = (uint8_t)ftoi_u8_fast(c02 + d2 * L);
                out->col[k][3] = (uint8_t)ftoi_u8_fast(amax - d3 * F);
#endif
            }
            out->pos[k][0] = px;
            out->pos[k][1] = py;
            out->pos[k][2] = pz;
            out->adc[k] = (posw[4 * k + 3] & 0x8000) != 0;
            if (env) {
                const float vx = px - ex0, vy = py - ey0, vz = pz - ez0;
                const float dn = 2.0f * (vx * nx + vy * ny + vz * nz);
                const float rx = vx - dn * nx, ry = vy - dn * ny, rz = vz - dn * nz;
                const float ex = rx * e00 + ry * e10 + rz * e20;
                const float ey = rx * e01 + ry * e11 + rz * e21;
                const float l2 = vx * vx + vy * vy + vz * vz;
                const float ez = l2 * rt_rsqrtf(l2) + rx * e02 + ry * e12 + rz * e22;
                const float mm = 0.5f * rt_rsqrtf(ex * ex + ey * ey + ez * ez);
                out->uv1[k][0] = 0.5f + ex * mm;
                out->uv1[k][1] = 0.5f + ey * mm;
            }
        }
    }
    /* UV pairs, as nc_shade has them: slot 1 of 4g (r 0: xy, r 2: zw) or of 4g+2 (r 1, r 3) */
    for (g = 0; g < n; g += 4) {
        uint32_t q0[4], q2[4];
        slot1(b, g, q0);
        slot1(b, g + 2, q2);
        for (r = 0; r < 4 && g + r < n; r++) {
            const uint32_t *q = (r & 1) ? q2 : q0;
            out->uv0[g + r][0] = (float)(int32_t)q[(r & 2) ? 2 : 0] * (1.0f / 4096.0f);
            out->uv0[g + r][1] = (float)(int32_t)q[(r & 2) ? 3 : 1] * (1.0f / 4096.0f);
        }
    }
    for (c = 0; c < 4; c++)
        out->col1[c] = (uint8_t)vu[1012 * 4 + c];
    return 1;
}

#if NC_PSQ
/* slot 1 of vertex e as slot1 has it, from the words where they are */
static inline void sl_get(const uint32_t *sl, uint32_t uvn, uint32_t e, uint32_t q[4])
{
    if (e >= uvn) {
        q[0] = q[1] = q[2] = q[3] = 0;
    } else {
        const uint32_t w0 = sl[2 * e], w1 = sl[2 * e + 1];
        q[0] = w0 & 0xFFFF;
        q[1] = w0 >> 16;
        q[2] = w1 & 0xFFFF;
        q[3] = w1 >> 16;
    }
}

/* A crashed car's skinned vertices, kept from frame to frame. Skinning is
 * 300 cycles a vertex (two bones' matrices blended by the vertex's weights,
 * then the position and the normal through the result) for 5700 vertices of
 * one car and 13000 when three have crashed: 2.3 to 8 ms a frame, the most
 * expensive thing in a crash. The bones move at impacts only: between them
 * a batch's bones, weights and positions are the same from one frame to the
 * next (19 batches in 20 of a crash), so what skinning made of them is too.
 * A batch's entry is found by its positions' address and holds a sum of all
 * that goes into the skinning (nc_sk_rest, sk_bone); the positions, normals and UVs
 * are in three pools (12, 12 and 8 bytes a vertex: the lighting's loop reads
 * the first two where they are). A full pool starts over.
 *
 * When a batch's sum has changed it is nearly always a bone or two of its
 * six or eight (a loose part swinging or dragging, for seconds on end; a
 * heavy crash has 50 such batches a frame, 400 cycles a vertex): the entry
 * also holds a sum of each bone and one of everything else, and while the
 * latter is the same only the vertices whose weights name a changed bone are
 * skinned again. */
#define NC_SK_VERTS 12288               /* 384 KB */
#define NC_SK_ENTS 512
#define NC_SK_BONES 12
void *plat_alloc(size_t size);
static struct NcSkEnt {
    uint32_t pos, sum, off, n;          /* sum: of all the bones together (for nb 255) */
    uint32_t rest, nb;                  /* nb: the bones the weights name; 255: more than are kept apart */
    uint32_t bh[NC_SK_BONES];           /* each one's sum, */
    uint16_t bi[NC_SK_BONES];           /* and number */
} g_sk[NC_SK_ENTS];
uint32_t g_nc_sk_part;                  /* status line: batches skinned again in part */
/* debugging (the backend's gxflags2 2097152): a crashed batch's time by step: its sum and entry, the copies of a
 * kept one, the skinning, the lighting's loop, the constants before all that (ticks of the time base) */
int g_nc_sk_time;
uint64_t g_nc_sk_t[5];
uint64_t plat_ticks(void);
static float *g_sk_pos, *g_sk_nrm, *g_sk_uv;
static uint32_t g_sk_used;
uint32_t g_nc_sk_hits, g_nc_sk_made, g_nc_sk_resets;    /* status line: batches taken from the pools, skinned, pools started over */
uint32_t g_nc_sk_verts, g_nc_sk_blends; /* status line: vertices skinned, bone matrices blended for them */
int g_nc_sk = 1;                        /* 0: every batch skinned every frame (the backend's gxflags2 65536) */

static inline uint32_t sk_mix(uint32_t h, uint32_t w)
{
    return (h ^ w) * 0x01000193u;
}

/* A kept batch costs its sums every frame, and that was more than its lighting (95 cycles a vertex against
 * 70): the bones were found again from the weights each time (a bitmap of 1024, four updates for every second
 * slot) and summed with a multiply a word, sixteen words each. Now the entry keeps the bones' numbers: while
 * the rest's sum is the same, they are the same bones, and each is summed by adds over the twelve words the
 * skinning reads (three of a row). */
static uint32_t nc_sk_rest(const uint32_t *vu, const NcBlock *b, const uint32_t *pw, const uint32_t *sl)
{
    uint32_t h = 0x811C9DC5u, k, ne = b->n + 3 < b->uvn ? b->n + 3 : b->uvn;
    /* the slot and position words (five a vertex) by two running sums, an add each */
    uint32_t a = 0, s2 = 0;
    h = sk_mix(h, b->n);
    h = sk_mix(h, b->nrm);
    h = sk_mix(h, b->uv);
    h = sk_mix(h, b->row[0] ^ b->row[1] << 1 ^ b->row[2] << 2);
    h = sk_mix(h, vu[970 * 4 + 3]);
    for (k = 0; k < 2 * ne; k++) {
        a += sl[k];
        s2 += a;
    }
    for (k = 0; k < b->n; k++, pw += 4) {
        a += pw[0];
        s2 += a;
        a += pw[1];
        s2 += a;
        a += pw[2];
        s2 += a;
    }
    return sk_mix(sk_mix(h, a), s2);
}

/* a bone's sum: the three used words of each of its four rows */
static inline uint32_t sk_bone(const uint32_t *vu, uint32_t id)
{
    const uint32_t at = (974 + id) * 4;
    uint32_t a = 0, s2 = 0, j;
    if (PS2_LIKELY(at + 16 <= 4096)) {
        const uint32_t *w = vu + at;
        for (j = 0; j < 16; j += 4) {
            a += w[j];
            s2 += a;
            a += w[j + 1];
            s2 += a;
            a += w[j + 2];
            s2 += a;
        }
    } else {
        for (j = 0; j < 16; j++)
            if ((j & 3) != 3) {
                a += vu[(at + j) & 4095];
                s2 += a;
            }
    }
    return sk_mix(a, s2);
}

/* the bones a batch's weights name, into its entry (numbers and sums); the sum of them all */
static uint32_t nc_sk_bones(const uint32_t *vu, const NcBlock *b, const uint32_t *sl, struct NcSkEnt *e)
{
    uint32_t used[32], hb = 0, nb = 0, k, ne = b->n + 3 < b->uvn ? b->n + 3 : b->uvn;
    memset(used, 0, sizeof used);
    for (k = 1; k < ne; k += 2) {       /* the odd slots are weights: the bone is in their low bits */
        const uint32_t w0 = sl[2 * k], w1 = sl[2 * k + 1];
        used[(w0 & 0x3FF) >> 5] |= 1u << (w0 & 31);
        used[((w0 >> 16) & 0x3FF) >> 5] |= 1u << ((w0 >> 16) & 31);
        used[(w1 & 0x3FF) >> 5] |= 1u << (w1 & 31);
        used[((w1 >> 16) & 0x3FF) >> 5] |= 1u << ((w1 >> 16) & 31);
    }
    for (k = 0; k < 32; k++) {
        uint32_t m = used[k];
        while (m) {
            const uint32_t bit = 31 - (uint32_t)__builtin_clz(m), one = sk_bone(vu, 32 * k + bit);
            m &= ~(1u << bit);
            hb = sk_mix(hb, one);
            if (nb < NC_SK_BONES) {
                e->bh[nb] = one;
                e->bi[nb] = (uint16_t)(32 * k + bit);
            }
            nb++;
        }
    }
    e->nb = nb <= NC_SK_BONES ? nb : 255;
    return hb;
}

/* debugging (the backend's gxflags2 524288): what changed in a batch that is skinned again. A few batches are
 * followed (by their positions' address): the bones their weights name and a sum of the rest, as they were the
 * last time the batch was skinned. */
int g_nc_sk_dbg;
uint32_t g_nc_sk_why[4];                /* status line: skinned again for the bones, for the rest, for both, new */

static void __attribute__((noinline)) nc_sk_diag(const uint32_t *vu, const NcBlock *b, const uint32_t *pw, const uint32_t *sl)
{
    static struct {
        uint32_t pos, rest, nb, seen;
        uint16_t bi[12];
        float bv[12][16];
    } t[8], *e;
    static uint32_t calls, lines;
    uint32_t used[32], k, i, nb = 0, rest = b->n * 31 + b->uvn, ne = b->n + 3 < b->uvn ? b->n + 3 : b->uvn;
    uint16_t bi[12];
    float md = 0.0f, mv = 0.0f;
    int mk = -1, mi = -1, nch = 0, same_set = 1;
    memset(used, 0, sizeof used);
    for (k = 0; k < ne; k++) {
        const uint32_t w0 = sl[2 * k], w1 = sl[2 * k + 1];
        rest = (rest ^ w0) * 0x01000193u;
        rest = (rest ^ w1) * 0x01000193u;
        if (k & 1) {
            used[(w0 & 0x3FF) >> 5] |= 1u << (w0 & 31);
            used[((w0 >> 16) & 0x3FF) >> 5] |= 1u << ((w0 >> 16) & 31);
            used[(w1 & 0x3FF) >> 5] |= 1u << (w1 & 31);
            used[((w1 >> 16) & 0x3FF) >> 5] |= 1u << ((w1 >> 16) & 31);
        }
    }
    for (k = 0; k < b->n; k++)
        for (i = 0; i < 3; i++)
            rest = (rest ^ pw[4 * k + i]) * 0x01000193u;
    rest = (rest ^ vu[970 * 4 + 3] ^ b->row[0] ^ b->row[1] << 1 ^ b->row[2] << 2 ^ b->nrm ^ b->uv) * 0x01000193u;
    for (k = 0; k < 32; k++)
        for (i = 0; i < 32; i++)
            if ((used[k] >> i & 1) && nb < 12)
                bi[nb++] = (uint16_t)(32 * k + i);
    calls++;
    e = &t[(b->pos >> 4) & 7];
    if (e->pos != b->pos) {
        g_nc_sk_why[3]++;
        if (e->pos && calls - e->seen < 4000)
            return;                     /* the slot follows another batch still */
    } else {
        if (e->nb != nb)
            same_set = 0;
        for (k = 0; k < nb && same_set; k++)
            if (e->bi[k] != bi[k])
                same_set = 0;
        for (k = 0; k < nb && same_set; k++) {
            const float *now = (const float *)&vu[((974 + bi[k]) * 4) & 4095];
            int ch = 0;
            for (i = 0; i < 16; i++) {
                const float d = fabsf(now[i] - e->bv[k][i]);
                if (now[i] != e->bv[k][i])
                    ch = 1;
                if (d > md) {
                    md = d;
                    mv = now[i];
                    mk = (int)k;
                    mi = (int)i;
                }
            }
            nch += ch;
        }
        g_nc_sk_why[(nch || !same_set ? 0 : 1) + (e->rest != rest && (nch || !same_set) ? 2 : 0)]++;
        if (lines < 80) {
            lines++;
            LOG("skin again: batch %08X (%u vertices, %u bones%s): %d bones changed, most bone %d element %d by %g to %g; "
                "the rest %s; %u calls since it was skinned", b->pos, b->n, nb, same_set ? "" : ", another set", nch,
                mk >= 0 ? (int)bi[mk] : -1, mi, (double)md, (double)mv, e->rest != rest ? "changed" : "the same",
                calls - e->seen);
        }
    }
    e->pos = b->pos;
    e->rest = rest;
    e->nb = nb;
    e->seen = calls;
    for (k = 0; k < nb; k++) {
        e->bi[k] = bi[k];
        memcpy(e->bv[k], &vu[((974 + bi[k]) * 4) & 4095], 64);
    }
}

/* the batch's entry (*had: the pools hold what was made of the batch before); NULL: no room */
static struct NcSkEnt *nc_sk_find(const NcBlock *b, int *had)
{
    uint32_t h = (b->pos >> 4) & (NC_SK_ENTS - 1), k;
    struct NcSkEnt *e;
    if (!g_sk_pos) {
        g_sk_pos = plat_alloc(NC_SK_VERTS * 32);
        if (!g_sk_pos) {
            g_nc_sk = 0;
            return NULL;
        }
        g_sk_nrm = g_sk_pos + 3 * NC_SK_VERTS;
        g_sk_uv = g_sk_nrm + 3 * NC_SK_VERTS;
    }
    for (k = 0; k < 8; k++) {
        e = &g_sk[(h + k) & (NC_SK_ENTS - 1)];
        if (!e->pos)
            break;
        if (e->pos == b->pos && e->n == b->n) {
            *had = 1;
            return e;
        }
    }
    if (k == 8 || g_sk_used + b->n > NC_SK_VERTS) {
        /* the pools (or this corner of the table) are full of batches of cars long gone: all over again */
        memset(g_sk, 0, sizeof g_sk);
        g_sk_used = 0;
        g_nc_sk_resets++;
        e = &g_sk[h];
        if (b->n > NC_SK_VERTS)
            return NULL;
    }
    e->pos = b->pos;
    e->n = b->n;
    e->off = g_sk_used;
    g_sk_used += b->n;
    *had = 0;
    return e;
}

/* nc_shade_skin with the lighting in nc_shade_ps_loop2: the positions and normals skinned first, as floats */
static int nc_shade_skin_ps2(const uint32_t *vu, const NcBlock *b, NcOut *out, const float *K)
{
    static float nrm_tmp[NC_MAX + 4][3];
    const float *pos = (const float *)GPTR(b->pos);
    const uint32_t *posw = (const uint32_t *)GPTR(b->pos), *sl = (const uint32_t *)GPTR(b->uv);
    const uint8_t *ram = ps2_ram;
    const uint32_t n = b->n, uvn = b->uvn;
    uint32_t g, r, j, k, c, na = b->nrm;
    const int env = VI(1013, 1) == 0;
    struct NcSkEnt *e = NULL;
    const float *lpos = &out->pos[0][0], *lnrm = &nrm_tmp[0][0];
    const float off = V(970, 3);
    uint32_t chg[32];                   /* the bones that changed, when only their vertices are skinned again */
    int hit = 0, had = 0, part = 0;
    out->n = n;
    out->env = env;
    uint64_t t0 = PS2_UNLIKELY(g_nc_sk_time) ? plat_ticks() : 0, t1;
#define SK_T(k_) do { if (PS2_UNLIKELY(g_nc_sk_time)) { t1 = plat_ticks(); g_nc_sk_t[k_] += t1 - t0; t0 = t1; } } while (0)
    if (g_nc_sk && (e = nc_sk_find(b, &had)) != NULL) {
        const uint32_t rest = nc_sk_rest(vu, b, posw, sl);
        if (had && e->rest == rest && e->nb <= NC_SK_BONES) {
            /* the same slots and positions: the same bones, by the entry's list */
            for (k = 0; k < e->nb; k++) {
                const uint32_t one = sk_bone(vu, e->bi[k]);
                if (one != e->bh[k]) {
                    if (!part)
                        memset(chg, 0, sizeof chg);
                    chg[e->bi[k] >> 5] |= 1u << (e->bi[k] & 31);
                    e->bh[k] = one;
                    part = 1;
                }
            }
            hit = !part;
            if (g_nc_sk == 2)
                part = 0;               /* (debugging: skinned again whole) */
        } else {
            /* new, or its slots or positions changed, or more bones than are kept apart */
            const uint32_t all = nc_sk_bones(vu, b, sl, e);
            hit = had && e->rest == rest && e->sum == all;
            e->rest = rest;
            e->sum = all;
        }
    }
    if (PS2_UNLIKELY(g_nc_sk_dbg) && !hit)
        nc_sk_diag(vu, b, posw, sl);
    if (hit) {
        /* as the frame before made them */
        lpos = g_sk_pos + 3 * e->off;
        lnrm = g_sk_nrm + 3 * e->off;
        memcpy(out->pos, lpos, n * 12);
        memcpy(out->uv0, g_sk_uv + 2 * e->off, n * 8);
        for (k = 0; k < n; k++)
            out->adc[k] = (posw[4 * k + 3] & 0x8000) != 0;
        g_nc_sk_hits++;
        SK_T(1);
    } else {
        SK_T(0);
        nc_rows(b->row[0], b->row[1], b->row[2], V(970, 3));
        if (part) {
            memcpy(out->pos, g_sk_pos + 3 * e->off, n * 12);
            memcpy(nrm_tmp, g_sk_nrm + 3 * e->off, n * 12);
            memcpy(out->uv0, g_sk_uv + 2 * e->off, n * 8);
            g_nc_sk_part++;
        }
        for (g = 0; g < n; g += 4) {
            uint32_t qa[4] = {0, 0, 0, 0}, qb[4] = {0, 0, 0, 0};
            if (g + 1 < n) {
                sl_get(sl, uvn, g + 1, qa);
                sl_get(sl, uvn, g + 3 < n ? g + 3 : g + 1, qb);
            }
            for (r = 0; r < 4 && g + r < n; r++, na += 3) {
                float px, py, pz, nx, ny, nz;
                k = g + r;
                out->adc[k] = (posw[4 * k + 3] & 0x8000) != 0;
                /* (a weight of nothing names bone 0 and takes nothing from it) */
                if (part && !((qa[r] && (chg[(qa[r] & 0x3FF) >> 5] >> (qa[r] & 31) & 1)) ||
                              (qb[r] && (chg[(qb[r] & 0x3FF) >> 5] >> (qb[r] & 31) & 1))))
                    continue;
                px = pos[4 * k];
                py = pos[4 * k + 1];
                pz = pos[4 * k + 2];
                nx = g_nrm_f[0][ram[(na & PS2_RAM_MASK) ^ PS2_BXOR]];
                ny = g_nrm_f[1][ram[((na + 1) & PS2_RAM_MASK) ^ PS2_BXOR]];
                nz = g_nrm_f[2][ram[((na + 2) & PS2_RAM_MASK) ^ PS2_BXOR]];
                if (qa[r] | qb[r]) {
                    /* The two bones blended by the vertex's weights, element by element in variables of
                     * their own (as an array the compiler kept the matrix in memory and looped over it:
                     * 400 cycles a vertex), then the position and the normal through it. */
                    const uint32_t ia = 974 + (qa[r] & 0x3FF), ib = 974 + (qb[r] & 0x3FF);
                    const float wa = (float)(int32_t)qa[r] * (1.0f / 65535.0f), wb = (float)(int32_t)qb[r] * (1.0f / 65535.0f);
                    const float w0 = 1.0f - wa - wb;
                    const float *ma = (const float *)(vu + ia * 4), *mb = (const float *)(vu + ib * 4);
                    float ta[16], tb[16];
                    g_nc_sk_verts++;
                    if (PS2_UNLIKELY(ia + 3 >= 1024 || ib + 3 >= 1024)) {
                        for (j = 0; j < 16; j++) {      /* a bone across the end of VU memory */
                            ta[j] = V((ia + (j >> 2)) & 1023, j & 3);
                            tb[j] = V((ib + (j >> 2)) & 1023, j & 3);
                        }
                        ma = ta;
                        mb = tb;
                    }
                    {
                        const float m00 = ma[0] * wa + mb[0] * wb + w0, m01 = ma[1] * wa + mb[1] * wb;
                        const float m02 = ma[2] * wa + mb[2] * wb;
                        const float m10 = ma[4] * wa + mb[4] * wb, m11 = ma[5] * wa + mb[5] * wb + w0;
                        const float m12 = ma[6] * wa + mb[6] * wb;
                        const float m20 = ma[8] * wa + mb[8] * wb, m21 = ma[9] * wa + mb[9] * wb;
                        const float m22 = ma[10] * wa + mb[10] * wb + w0;
                        const float m30 = ma[12] * wa + mb[12] * wb, m31 = ma[13] * wa + mb[13] * wb;
                        const float m32 = ma[14] * wa + mb[14] * wb;
                        const float x = m00 * px + m10 * py + m20 * pz + m30;
                        const float y = m01 * px + m11 * py + m21 * pz + m31;
                        const float z = m02 * px + m12 * py + m22 * pz + m32;
                        const float q0 = m00 * nx + m10 * ny + m20 * nz;
                        const float q1 = m01 * nx + m11 * ny + m21 * nz;
                        const float q2 = m02 * nx + m12 * ny + m22 * nz;
                        px = x;
                        py = y;
                        pz = z;
                        /* the microprogram keeps the normal with its offset: added and taken off again */
                        nx = (q0 + off) - off;
                        ny = (q1 + off) - off;
                        nz = (q2 + off) - off;
                    }
                }
                out->pos[k][0] = px;
                out->pos[k][1] = py;
                out->pos[k][2] = pz;
                nrm_tmp[k][0] = nx;
                nrm_tmp[k][1] = ny;
                nrm_tmp[k][2] = nz;
            }
        }
        /* UV pairs, as nc_shade has them: slot 1 of 4g (r 0: xy, r 2: zw) or of 4g+2 (r 1, r 3) */
        for (g = 0; g < n && !part; g += 4) {
            uint32_t q0[4], q2[4];
            sl_get(sl, uvn, g, q0);
            sl_get(sl, uvn, g + 2, q2);
            for (r = 0; r < 4 && g + r < n; r++) {
                const uint32_t *q = (r & 1) ? q2 : q0;
                out->uv0[g + r][0] = (float)(int32_t)q[(r & 2) ? 2 : 0] * (1.0f / 4096.0f);
                out->uv0[g + r][1] = (float)(int32_t)q[(r & 2) ? 3 : 1] * (1.0f / 4096.0f);
            }
        }
        if (e) {
            memcpy(g_sk_pos + 3 * e->off, out->pos, n * 12);
            memcpy(g_sk_nrm + 3 * e->off, nrm_tmp, n * 12);
            memcpy(g_sk_uv + 2 * e->off, out->uv0, n * 8);
        }
        g_nc_sk_made++;
        SK_T(2);
    }
    nc_shade_ps_loop2(K, lpos, lnrm, 0, n, &out->col[0][0], env ? &out->uv1[0][0] : NULL);
    SK_T(3);
#undef SK_T
    for (c = 0; c < 4; c++)
        out->col1[c] = (uint8_t)vu[1012 * 4 + c];
    return 1;
}
#endif

int nc_shade_skin(const uint32_t *vu, const NcBlock *b, NcOut *out)
{
#if NC_PSQ
    static float K[54] __attribute__((aligned(8)));
    static float lkey[6];
    if (g_nc_ps && b->uvstep == 1 && VI(1013, 2) && b->n && b->n <= NC_MAX && !(b->uv & 3) &&
        (b->pos & PS2_RAM_MASK) + 16 * b->n + 16 < PS2_RAM_SIZE && (b->uv & PS2_RAM_MASK) + 8 * b->uvn + 8 < PS2_RAM_SIZE) {
        const uint64_t t0 = PS2_UNLIKELY(g_nc_sk_time) ? plat_ticks() : 0;
        const int ok = nc_ps_consts(vu, K, lkey, 1);
        if (PS2_UNLIKELY(g_nc_sk_time))
            g_nc_sk_t[4] += plat_ticks() - t0;
        if (ok)
            return nc_shade_skin_ps2(vu, b, out, K);
    }
#endif
    return nc_shade_skin_c(vu, b, out);
}

/* Object to screen: the camera (1014..1017) after the object matrix (962..965),
 * as rows X, Y and W such that screen = 1019 + 1018 * (X / W - 0.5, Y / W -
 * 0.5, 1 / W). The microprogram centres with half the camera's z row. */
void nc_matrix(const uint32_t *vu, float m[3][4])
{
    int j, r;
    for (j = 0; j < 4; j++) {
        float cam[4];
        for (r = 0; r < 4; r++)
            cam[r] = V(1014, r) * V(962 + j, 0) + V(1015, r) * V(962 + j, 1) + V(1016, r) * V(962 + j, 2) +
                     (j == 3 ? V(1017, r) : 0.0f);
        m[0][j] = cam[0] + 0.5f * (cam[3] - cam[2]);
        m[1][j] = cam[1] + 0.5f * (cam[3] - cam[2]);
        m[2][j] = cam[3];
    }
}
