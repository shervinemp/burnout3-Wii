/*
 * VU0/VU1 micro mode interpreter.
 *
 * Each 64-bit instruction is an upper (FMAC) and a lower operation issued
 * together. Both read the registers as they were before the pair; the lower
 * writes its results first, then the upper, so the upper wins when both
 * write the same register. VF reads are interlocked in hardware (sequential
 * semantics), but three things are visible to timing-scheduled microcode
 * and are modelled with a cycle counter:
 *   - Q (DIV/SQRT/RSQRT) and P (EFU) arrive after their latency; reads
 *     before that see the old value, WAITQ/WAITP stall;
 *   - MAC, status and clip flags appear 4 cycles after the FMAC that sets
 *     them;
 *   - a stall on a VF register that is still in flight delays everything.
 * Programs end one instruction after the E bit. VU0 runs on the EE's COP2
 * register file (the context of the calling thread); VU1 has its own.
 */
#include "hw.h"
#include "gs.h"
#include "vu.h"
#include "vu_rec.h"
#include "vu0_inline.h"
#include "wtime.h"

#include <float.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

typedef union { float f; uint32_t u; int32_t s; } F32;

#define FLAGQ 16

typedef struct VU {
    int unit;
    float (*vf)[4];
    uint32_t *vi;
    float *acc;
    float *q, *p, *ireg;
    uint32_t *r;
    uint32_t *mem;             /* data memory, host-order words */
    uint32_t mem_qw;           /* size in qwords (power of two) */
    const uint32_t *micro;     /* micro memory, words: lower at 2n, upper at 2n+1 */
    uint32_t micro_bytes;
    uint32_t tpc;              /* where MSCNT continues */
    uint32_t mac, status, clip;
    /* timing */
    uint64_t cycle;
    uint64_t vf_ready[32][4];  /* per component: FMAC results land 4 cycles after issue */
    float q_next, p_next;
    uint64_t q_ready, p_ready;
    int q_pending, p_pending;
    uint32_t q_status;         /* I/D bits that come with the pending division */
    struct { uint64_t ready; uint32_t mac, status, clip, what; } fq[FLAGQ];
    int fq_head, fq_n;
    unsigned runs;
    /* a conditional branch right after an integer op that writes its VI
     * register reads the value from before that write */
    int bk_reg, bk_cycles;
    uint16_t bk_val;
} VU;

enum { F_MAC = 1, F_STATUS = 2, F_CLIP = 4 };

static VU g_vu[2];
static float g_vu1_vf[32][4];
static uint32_t g_vu1_vi[16];
static float g_vu1_acc[4], g_vu1_q, g_vu1_p, g_vu1_i;
static uint32_t g_vu1_r;

/* ---- float semantics: no infinities or NaNs, denormals are zero -------------------------- */

/* Operand as the FMAC/FDIV/EFU sees it: exponent 255 is a large number, not
 * Inf/NaN (saturate to +-max), and denormals are zero (PCSX2 vuDouble). */
static inline float vin(float f)
{
    F32 v;
    v.f = f;
    if ((v.u & 0x7F800000u) == 0x7F800000u)
        v.u = (v.u & 0x80000000u) | 0x7F7FFFFFu;
    else if ((v.u & 0x7F800000u) == 0)
        v.u &= 0x80000000u;
    return v.f;
}

static inline float clampf(float f)
{
    F32 v;
    v.f = f;
    if ((v.u & 0x7F800000u) == 0x7F800000u)
        v.u = (v.u & 0x80000000u) | 0x7F7FFFFFu;
    else if ((v.u & 0x7F800000u) == 0)
        v.u &= 0x80000000u;
    return v.f;
}

static inline float fbits(uint32_t u) { F32 v; v.u = u; return v.f; }
static inline uint32_t ubits(float f) { F32 v; v.f = f; return v.u; }

/* ---- pipelined state ---------------------------------------------------------------------- */

static void q_sync(VU *vu)
{
    if (vu->q_pending && vu->cycle >= vu->q_ready) {
        *vu->q = vu->q_next;
        vu->status = (vu->status & ~0x30u) | vu->q_status | (vu->q_status << 6);
        vu->q_pending = 0;
    }
}

static void p_sync(VU *vu)
{
    if (vu->p_pending && vu->cycle >= vu->p_ready) {
        *vu->p = vu->p_next;
        vu->p_pending = 0;
    }
}

static void flags_sync(VU *vu)
{
    while (vu->fq_n && vu->fq[vu->fq_head].ready <= vu->cycle) {
        typeof(vu->fq[0]) *e = &vu->fq[vu->fq_head];
        if (e->what & F_MAC)
            vu->mac = e->mac;
        if (e->what & F_STATUS)
            vu->status = (vu->status & 0xFF0u) | e->status | (e->status << 6);
        if (e->what & F_CLIP)
            vu->clip = e->clip;
        vu->fq_head = (vu->fq_head + 1) % FLAGQ;
        vu->fq_n--;
    }
}

static void flags_push(VU *vu, uint32_t what, uint32_t mac, uint32_t status, uint32_t clip)
{
    int k;
    if (vu->fq_n == FLAGQ) {            /* more updates in flight than the pipeline holds */
        uint64_t c = vu->cycle;
        vu->cycle = vu->fq[vu->fq_head].ready;
        flags_sync(vu);
        vu->cycle = c;
    }
    k = (vu->fq_head + vu->fq_n) % FLAGQ;
    vu->fq[k].ready = vu->cycle + 4;
    vu->fq[k].what = what;
    vu->fq[k].mac = mac;
    vu->fq[k].status = status;
    vu->fq[k].clip = clip;
    vu->fq_n++;
}

static void flags_drain(VU *vu)
{
    vu->cycle += 8;
    q_sync(vu);
    p_sync(vu);
    flags_sync(vu);
}

/* ---- decoding helpers ------------------------------------------------------------------------ */

#define DEST(op) (((op) >> 21) & 15)              /* bit 3 = x ... bit 0 = w */
#define FT(op) (((op) >> 16) & 31)
#define FS(op) (((op) >> 11) & 31)
#define FD(op) (((op) >> 6) & 31)
#define LANE(d, k) ((d) & (8 >> (k)))              /* k: 0 = x ... 3 = w */

static inline uint32_t imm11(uint32_t op)
{
    return (uint32_t)((int32_t)(op << 21) >> 21);
}

static inline uint16_t ivi(VU *vu, uint32_t n) { n &= 15; return n ? (uint16_t)vu->vi[n] : 0; }

/* Integer ALU results reach the branch unit a cycle late: a branch directly
 * after IADD/ISUB/.../MTIR or an LQI/LQD/SQI/SQD address update sees the old
 * register (PCSX2: VI_BACKUP). Programs written on hardware count on it, e.g.
 * "isubiu vi2, vi2, 1; ibne vi2, vi0, loop" runs the loop once more. */
static inline void vi_backup(VU *vu, uint32_t n)
{
    n &= 15;
    if (!n)
        return;
    if (vu->bk_cycles && vu->bk_reg == (int)n) {
        vu->bk_cycles = 2;              /* repeated writes keep the oldest value */
        return;
    }
    vu->bk_cycles = 2;
    vu->bk_reg = (int)n;
    vu->bk_val = (uint16_t)vu->vi[n];
}

static inline void set_vi(VU *vu, uint32_t n, uint32_t v)
{
    n &= 15;
    if (n)
        vu->vi[n] = v & 0xFFFF;
}

/* VI as a conditional branch reads it (see vi_backup) */
static inline uint16_t bvi(VU *vu, uint32_t n)
{
    n &= 15;
    if (vu->bk_cycles && vu->bk_reg == (int)n)
        return vu->bk_val;
    return ivi(vu, n);
}

static inline uint32_t *memq(VU *vu, uint32_t qaddr)
{
    return vu->mem + 4 * (qaddr & (vu->mem_qw - 1));
}

/* Wait for pending writes to the components of VF[reg] an instruction reads
 * (mask: 8 = x ... 1 = w). Hardware only stalls when the components overlap
 * (PCSX2 _vuFMACTestStall); stalling on the whole register moves later
 * instructions to other cycles, and with them which FMAC's MAC flags an
 * FMAND sees -- Burnout 3's VU0 collision code branches on exactly that. */
static void stall_on(VU *vu, uint32_t reg, uint32_t mask)
{
    int k;
    if (!reg)
        return;
    for (k = 0; k < 4; k++)
        if (mask & (8u >> k) && vu->vf_ready[reg][k] > vu->cycle)
            vu->cycle = vu->vf_ready[reg][k];
}

static void vf_mark(VU *vu, uint32_t reg, uint32_t dest)
{
    int k;
    for (k = 0; k < 4; k++)
        if (LANE(dest, k))
            vu->vf_ready[reg][k] = vu->cycle + 4;
}

/* ---- upper (FMAC) ---------------------------------------------------------------------------- */

typedef struct Upper {
    int write;              /* 0 none, 1 VF[fd], 2 ACC */
    uint32_t reg, dest;
    float v[4];
    int flags;              /* sets MAC/status */
    int clip;
    uint32_t clipbits;
} Upper;

static float op_add(float a, float b) { return a + b; }
static float op_sub(float a, float b) { return a - b; }
static float op_mul(float a, float b) { return a * b; }

static void fmac_flags(VU *vu, const Upper *u)
{
    uint32_t mac = 0, st = 0;
    int k;
    for (k = 0; k < 4; k++) {
        float r;
        uint32_t bit = 8u >> k;
        if (!LANE(u->dest, k))
            continue;
        r = u->v[k];
        if (r == 0.0f)
            mac |= bit;
        if (ubits(r) & 0x80000000u)
            mac |= bit << 4;
        if ((ubits(r) & 0x7FFFFFFFu) == 0x7F7FFFFFu)
            mac |= bit << 12;               /* clamped: overflow */
    }
    if (mac & 0x000F) st |= 1;
    if (mac & 0x00F0) st |= 2;
    if (mac & 0x0F00) st |= 4;
    if (mac & 0xF000) st |= 8;
    flags_push(vu, F_MAC | F_STATUS, mac, st, 0);
}

/* operand b of an FMAC: ft lane k, ft.bc, Q or I */
enum { SRC_VEC, SRC_BC, SRC_Q, SRC_I };

static void upper(VU *vu, uint32_t op, Upper *u)
{
    uint32_t dest = DEST(op), ft = FT(op), fs = FS(op), fd = FD(op);
    uint32_t code = op & 0x3F;
    float (*vf)[4] = vu->vf;
    float b[4], a[4];
    int k, src = SRC_VEC, acc_out = 0, kind;
    float (*fn)(float, float) = NULL;
    enum { K_PLAIN, K_MADD, K_MSUB, K_MAX, K_MIN } k_op = K_PLAIN;

    u->write = 0;
    u->dest = dest;
    u->flags = 0;
    u->clip = 0;
    if (code >= 0x3C) {
        /* special: ADDA/SUBA/... ITOF FTOI ABS CLIP NOP, index by bits 10:6 and 1:0 */
        uint32_t x = ((op >> 4) & 0x7C) | (op & 3);
        uint32_t bc = op & 3;
        acc_out = 1;
        switch (x) {
        case 0x00: case 0x01: case 0x02: case 0x03: fn = op_add; src = SRC_BC; break;
        case 0x04: case 0x05: case 0x06: case 0x07: fn = op_sub; src = SRC_BC; break;
        case 0x08: case 0x09: case 0x0A: case 0x0B: fn = op_mul; src = SRC_BC; k_op = K_MADD; break;
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: fn = op_mul; src = SRC_BC; k_op = K_MSUB; break;
        case 0x10: case 0x11: case 0x12: case 0x13: {      /* ITOF0/4/12/15 */
            static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
            stall_on(vu, fs, dest);
            for (k = 0; k < 4; k++)
                u->v[k] = (float)(int32_t)ubits(vf[fs][k]) * sc[bc];
            u->write = ft ? 1 : 0;
            u->reg = ft;
            return;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {      /* FTOI0/4/12/15 */
            static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            stall_on(vu, fs, dest);
            for (k = 0; k < 4; k++) {
                float f = vf[fs][k] * sc[bc];
                int32_t i = f >= 2147483647.0f ? 0x7FFFFFFF : f <= -2147483648.0f ? (int32_t)0x80000000u : (int32_t)f;
                u->v[k] = fbits((uint32_t)i);
            }
            u->write = ft ? 1 : 0;
            u->reg = ft;
            return;
        }
        case 0x18: case 0x19: case 0x1A: case 0x1B: fn = op_mul; src = SRC_BC; break;
        case 0x1C: fn = op_mul; src = SRC_Q; break;                  /* MULAq */
        case 0x1D:                                                   /* ABS */
            stall_on(vu, fs, dest);
            for (k = 0; k < 4; k++)
                u->v[k] = fabsf(vf[fs][k]);
            u->write = ft ? 1 : 0;
            u->reg = ft;
            return;
        case 0x1E: fn = op_mul; src = SRC_I; break;                  /* MULAi */
        case 0x1F: {                                                 /* CLIP fs.xyz, ft.w */
            float w;
            uint32_t f = 0;
            stall_on(vu, fs, 0xE);
            stall_on(vu, ft, 0x1);
            w = fabsf(vf[ft][3]);
            if (vf[fs][0] > +w) f |= 0x01;
            if (vf[fs][0] < -w) f |= 0x02;
            if (vf[fs][1] > +w) f |= 0x04;
            if (vf[fs][1] < -w) f |= 0x08;
            if (vf[fs][2] > +w) f |= 0x10;
            if (vf[fs][2] < -w) f |= 0x20;
            u->clip = 1;
            u->clipbits = f;
            return;
        }
        case 0x20: fn = op_add; src = SRC_Q; break;
        case 0x21: fn = op_mul; src = SRC_Q; k_op = K_MADD; break;
        case 0x22: fn = op_add; src = SRC_I; break;
        case 0x23: fn = op_mul; src = SRC_I; k_op = K_MADD; break;
        case 0x24: fn = op_sub; src = SRC_Q; break;
        case 0x25: fn = op_mul; src = SRC_Q; k_op = K_MSUB; break;
        case 0x26: fn = op_sub; src = SRC_I; break;
        case 0x27: fn = op_mul; src = SRC_I; k_op = K_MSUB; break;
        case 0x28: fn = op_add; break;
        case 0x29: fn = op_mul; k_op = K_MADD; break;
        case 0x2A: fn = op_mul; break;
        case 0x2C: fn = op_sub; break;
        case 0x2D: fn = op_mul; k_op = K_MSUB; break;
        case 0x2E:                                                   /* OPMULA */
            stall_on(vu, fs, 0xE);
            stall_on(vu, ft, 0xE);
            u->v[0] = clampf(vf[fs][1] * vf[ft][2]);
            u->v[1] = clampf(vf[fs][2] * vf[ft][0]);
            u->v[2] = clampf(vf[fs][0] * vf[ft][1]);
            u->v[3] = vu->acc[3];
            u->write = 2;
            u->flags = 1;
            return;
        case 0x2F:                                                   /* NOP */
            return;
        default:
            LOG("VU%d: unknown upper special %02X (%08X)", vu->unit, x, op);
            return;
        }
    } else {
        uint32_t bc = op & 3;
        switch (code >> 2) {
        case 0x0: fn = op_add; src = SRC_BC; break;
        case 0x1: fn = op_sub; src = SRC_BC; break;
        case 0x2: fn = op_mul; src = SRC_BC; k_op = K_MADD; break;
        case 0x3: fn = op_mul; src = SRC_BC; k_op = K_MSUB; break;
        case 0x4: k_op = K_MAX; src = SRC_BC; break;
        case 0x5: k_op = K_MIN; src = SRC_BC; break;
        case 0x6: fn = op_mul; src = SRC_BC; break;
        case 0x7:
            switch (bc) {
            case 0: fn = op_mul; src = SRC_Q; break;                 /* MULq */
            case 1: k_op = K_MAX; src = SRC_I; break;                /* MAXi */
            case 2: fn = op_mul; src = SRC_I; break;                 /* MULi */
            default: k_op = K_MIN; src = SRC_I; break;               /* MINIi */
            }
            break;
        case 0x8:
            switch (bc) {
            case 0: fn = op_add; src = SRC_Q; break;
            case 1: fn = op_mul; src = SRC_Q; k_op = K_MADD; break;
            case 2: fn = op_add; src = SRC_I; break;
            default: fn = op_mul; src = SRC_I; k_op = K_MADD; break;
            }
            break;
        case 0x9:
            switch (bc) {
            case 0: fn = op_sub; src = SRC_Q; break;
            case 1: fn = op_mul; src = SRC_Q; k_op = K_MSUB; break;
            case 2: fn = op_sub; src = SRC_I; break;
            default: fn = op_mul; src = SRC_I; k_op = K_MSUB; break;
            }
            break;
        case 0xA:
            switch (bc) {
            case 0: fn = op_add; break;
            case 1: fn = op_mul; k_op = K_MADD; break;
            case 2: fn = op_mul; break;
            default: k_op = K_MAX; break;
            }
            break;
        case 0xB:
            switch (bc) {
            case 0: fn = op_sub; break;
            case 1: fn = op_mul; k_op = K_MSUB; break;
            case 2:                                                  /* OPMSUB */
                stall_on(vu, fs, 0xE);
                stall_on(vu, ft, 0xE);
                u->v[0] = clampf(vu->acc[0] - vf[fs][1] * vf[ft][2]);
                u->v[1] = clampf(vu->acc[1] - vf[fs][2] * vf[ft][0]);
                u->v[2] = clampf(vu->acc[2] - vf[fs][0] * vf[ft][1]);
                u->v[3] = vf[fd][3];
                u->write = fd ? 1 : 0;
                u->reg = fd;
                u->flags = 1;
                return;
            default: k_op = K_MIN; break;
            }
            break;
        default:
            LOG("VU%d: unknown upper %02X (%08X)", vu->unit, code, op);
            return;
        }
    }

    /* MAX/MINI compare the raw bits as sign-magnitude integers: no denormal
     * flush, no clamping, no flags. Microcode builds GIF tags this way, e.g.
     * "maxi.z vf1, vf0, I" with I = 0x412 (a denormal) for the REGS field;
     * as a float max that became 0 and Burnout 3's cars drew nothing. */
    if (k_op == K_MAX || k_op == K_MIN) {
        stall_on(vu, fs, dest);
        if (src == SRC_VEC || src == SRC_BC)
            stall_on(vu, ft, src == SRC_BC ? 8u >> (op & 3) : dest);
        for (k = 0; k < 4; k++) {
            uint32_t x, y;
            if (!LANE(dest, k)) {
                u->v[k] = vf[fd][k];
                continue;
            }
            x = ubits(vf[fs][k]);
            y = src == SRC_VEC ? ubits(vf[ft][k]) : src == SRC_BC ? ubits(vf[ft][op & 3]) : ubits(*vu->ireg);
            u->v[k] = fbits(k_op == K_MAX ? fp_max(x, y) : fp_min(x, y));
        }
        u->write = fd ? 1 : 0;
        u->reg = fd;
        return;
    }

    /* operands */
    stall_on(vu, fs, dest);
    if (src == SRC_VEC || src == SRC_BC)
        stall_on(vu, ft, src == SRC_BC ? 8u >> (op & 3) : dest);
    for (k = 0; k < 4; k++) {
        a[k] = vin(vf[fs][k]);
        switch (src) {
        case SRC_VEC: b[k] = vin(vf[ft][k]); break;
        case SRC_BC: b[k] = vin(vf[ft][op & 3]); break;
        case SRC_Q: q_sync(vu); b[k] = vin(*vu->q); break;
        default: b[k] = vin(*vu->ireg); break;
        }
    }
    kind = k_op;
    for (k = 0; k < 4; k++) {
        float r;
        if (!LANE(dest, k)) {
            u->v[k] = acc_out ? vu->acc[k] : vf[fd][k];
            continue;
        }
        switch (kind) {
        case K_MAX: r = a[k] > b[k] ? a[k] : b[k]; break;
        case K_MIN: r = a[k] < b[k] ? a[k] : b[k]; break;
        case K_MADD: r = vin(vu->acc[k]) + clampf(a[k] * b[k]); break;
        case K_MSUB: r = vin(vu->acc[k]) - clampf(a[k] * b[k]); break;
        default: r = fn(a[k], b[k]); break;
        }
        u->v[k] = clampf(r);
    }
    if (acc_out) {
        u->write = 2;
    } else {
        u->write = fd ? 1 : 0;
        u->reg = fd;
    }
    u->flags = kind != K_MAX && kind != K_MIN;
}

static void upper_commit(VU *vu, const Upper *u, uint32_t pc)
{
    int k;
#if defined(PS2_FPU_CHECK)
    if (u->write) {
        int ps2_vu_watch(int unit, uint32_t pc, float v);
        for (k = 0; k < 4; k++)
            if (LANE(u->dest, k) && ps2_vu_watch(vu->unit, pc, u->v[k])) {
                int r;                      /* first hit at this pc: show the registers */
                for (r = 0; r < 32; r++)
                    LOG("  vf%-2d %12g %12g %12g %12g", r, (double)vu->vf[r][0], (double)vu->vf[r][1],
                        (double)vu->vf[r][2], (double)vu->vf[r][3]);
                LOG("  acc %12g %12g %12g %12g  Q %g I %g", (double)vu->acc[0], (double)vu->acc[1],
                    (double)vu->acc[2], (double)vu->acc[3], (double)*vu->q, (double)*vu->ireg);
                break;
            }
    }
#endif
    if (u->write == 1) {
        for (k = 0; k < 4; k++)
            if (LANE(u->dest, k))
                vu->vf[u->reg][k] = u->v[k];
        vf_mark(vu, u->reg, u->dest);
    } else if (u->write == 2) {
        for (k = 0; k < 4; k++)
            if (LANE(u->dest, k))
                vu->acc[k] = u->v[k];
    }
    if (u->flags)
        fmac_flags(vu, u);
    if (u->clip) {
        /* CLIP shifts onto the newest clip value, which may still be in flight */
        uint32_t prev = vu->clip;
        for (k = 0; k < vu->fq_n; k++) {
            int e = (vu->fq_head + k) % FLAGQ;
            if (vu->fq[e].what & F_CLIP)
                prev = vu->fq[e].clip;
        }
        flags_push(vu, F_CLIP, 0, 0, (prev << 6 | u->clipbits) & 0xFFFFFF);
    }
}

/* ---- lower --------------------------------------------------------------------------------- */

typedef struct Lower {
    int branch;             /* taken branch: target in bytes */
    uint32_t target;
    int xgkick;
    uint32_t kick_addr;
} Lower;

static void vf_write(VU *vu, uint32_t reg, uint32_t dest, const float v[4])
{
    int k;
    if (!reg)
        return;
    for (k = 0; k < 4; k++)
        if (LANE(dest, k))
            vu->vf[reg][k] = v[k];
    vf_mark(vu, reg, dest);
}

static void load_q(VU *vu, uint32_t ft, uint32_t dest, uint32_t qaddr)
{
    const uint32_t *m = memq(vu, qaddr);
    float v[4];
    int k;
    for (k = 0; k < 4; k++)
        v[k] = fbits(m[k]);
    vf_write(vu, ft, dest, v);
}

static void store_q(VU *vu, uint32_t fs, uint32_t dest, uint32_t qaddr)
{
    uint32_t *m = memq(vu, qaddr);
    int k;
    stall_on(vu, fs, dest);
    for (k = 0; k < 4; k++)
        if (LANE(dest, k))
            m[k] = ubits(vu->vf[fs][k]);
}

static int first_lane(uint32_t dest)
{
    /* with several lanes set the last one wins, as in PCSX2 */
    return dest & 1 ? 3 : dest & 2 ? 2 : dest & 4 ? 1 : 0;
}

static void div_issue(VU *vu, float v, uint32_t st, int latency)
{
    if (vu->q_pending && vu->cycle < vu->q_ready)
        vu->cycle = vu->q_ready;        /* the divider is busy */
    q_sync(vu);
    vu->q_next = clampf(v);
    vu->q_status = st;
    vu->q_ready = vu->cycle + (uint64_t)latency;
    vu->q_pending = 1;
}

/* EFU series as PCSX2 evaluates them (checked against hardware). EATAN adds
 * pi/4: microcode passes (x-1)/(x+1) to get atan(x). */
static float eatan(float t)
{
    static const float c[9] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f,
                               -0.139085337519646f, 0.096420042216778f, -0.055909886956215f,
                               0.021861229091883f, -0.004054057877511f, 0.785398185253143f};
    float t2 = t * t, p = t, r = 0;
    int k;
    for (k = 0; k < 8; k++, p *= t2)
        r += c[k] * p;
    return vin(r + c[8]);
}

static float esin(float x)
{
    static const float c[5] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f,
                               0.000002601886990f};
    float x2 = x * x, p = x, r = 0;
    int k;
    for (k = 0; k < 5; k++, p *= x2)
        r += c[k] * p;
    return vin(r);
}

static float eexp(float x)
{
    static const float c[6] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                               0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
    float p = 1.0f, xn = x;
    int k;
    for (k = 0; k < 6; k++, xn *= x)
        p += c[k] * xn;
    p = p * p;
    p = vin(p * p);
    return 1.0f / p;
}

static void efu_issue(VU *vu, float v, int latency)
{
    if (vu->p_pending && vu->cycle < vu->p_ready)
        vu->cycle = vu->p_ready;
    p_sync(vu);
    vu->p_next = clampf(v);
    vu->p_ready = vu->cycle + (uint64_t)latency;
    vu->p_pending = 1;
}

/* The pair issues as a unit: the lower op's stalls (operand writes still in
 * flight, a busy divider or EFU, WAITQ/WAITP) delay the upper op too, so they
 * are applied before either executes (PCSX2 _vuTestLowerStalls runs before
 * both). "mulq.xyz vf9, vf15, Q | rsqrt Q, ..." then reads the Q of the
 * previous RSQRT, not an older one. lower() repeats these checks harmlessly. */
static void lower_stalls(VU *vu, uint32_t op)
{
    uint32_t opc = op >> 25, dest = DEST(op), ft = FT(op), fs = FS(op);
    uint32_t x, fsf, ftf;
    if (opc == 0x01) {                      /* SQ */
        stall_on(vu, fs, dest);
        return;
    }
    if (opc != 0x40 || (op & 0x3C) != 0x3C)
        return;
    x = ((op >> 4) & 0x7C) | (op & 3);
    fsf = (op >> 21) & 3;
    ftf = (op >> 23) & 3;
    switch (x) {
    case 0x30: case 0x35: case 0x37:        /* MOVE SQI SQD */
        stall_on(vu, fs, dest);
        break;
    case 0x31:                              /* MR32 */
        stall_on(vu, fs, ((dest >> 1) | (dest << 3)) & 15);
        break;
    case 0x38: case 0x3A:                   /* DIV RSQRT */
        stall_on(vu, fs, 8u >> fsf);
        stall_on(vu, ft, 8u >> ftf);
        /* fall through: the divider must be free */
    case 0x3B:                              /* WAITQ */
        if (vu->q_pending && vu->cycle < vu->q_ready)
            vu->cycle = vu->q_ready;
        break;
    case 0x39:                              /* SQRT */
        stall_on(vu, ft, 8u >> ftf);
        if (vu->q_pending && vu->cycle < vu->q_ready)
            vu->cycle = vu->q_ready;
        break;
    case 0x3C: case 0x42: case 0x43:        /* MTIR RINIT RXOR */
        stall_on(vu, fs, 8u >> fsf);
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76:
    case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E:
        stall_on(vu, fs, x == 0x76 ? 0xFu : x == 0x74 ? 0xCu : x == 0x75 ? 0xAu : x < 0x78 ? 0xEu : 8u >> fsf);
        /* fall through: the EFU must be free */
    case 0x7B:                              /* WAITP */
        if (vu->p_pending && vu->cycle < vu->p_ready)
            vu->cycle = vu->p_ready;
        break;
    default:
        break;
    }
}

static void lower(VU *vu, uint32_t op, uint32_t pc, Lower *lo)
{
    uint32_t opc = op >> 25, dest = DEST(op), ft = FT(op), fs = FS(op);
    float (*vf)[4] = vu->vf;
    lo->branch = 0;
    lo->xgkick = 0;
    switch (opc) {
    case 0x00:                  /* LQ ft, imm(is) */
        load_q(vu, ft, dest, ivi(vu, fs) + imm11(op));
        return;
    case 0x01:                  /* SQ fs, imm(it) */
        store_q(vu, fs, dest, ivi(vu, ft) + imm11(op));
        return;
    case 0x04: {                /* ILW it, imm(is) */
        uint32_t *m = memq(vu, ivi(vu, fs) + imm11(op));
        set_vi(vu, ft, m[first_lane(dest)]);
        return;
    }
    case 0x05: {                /* ISW it, imm(is) */
        uint32_t *m = memq(vu, ivi(vu, fs) + imm11(op));
        int k;
        for (k = 0; k < 4; k++)
            if (LANE(dest, k))
                m[k] = ivi(vu, ft);
        return;
    }
    case 0x08:                  /* IADDIU */
        vi_backup(vu, ft);
        set_vi(vu, ft, ivi(vu, fs) + (((op >> 10) & 0x7800) | (op & 0x7FF)));
        return;
    case 0x09:                  /* ISUBIU */
        vi_backup(vu, ft);
        set_vi(vu, ft, ivi(vu, fs) - (((op >> 10) & 0x7800) | (op & 0x7FF)));
        return;
    case 0x10:                  /* FCEQ */
        flags_sync(vu);
        set_vi(vu, 1, (vu->clip & 0xFFFFFF) == (op & 0xFFFFFF));
        return;
    case 0x11:                  /* FCSET */
        flags_sync(vu);
        vu->clip = op & 0xFFFFFF;
        return;
    case 0x12:                  /* FCAND */
        flags_sync(vu);
        set_vi(vu, 1, (vu->clip & op & 0xFFFFFF) != 0);
        return;
    case 0x13:                  /* FCOR */
        flags_sync(vu);
        set_vi(vu, 1, ((vu->clip | op) & 0xFFFFFF) == 0xFFFFFF);
        return;
    case 0x14: case 0x15: case 0x16: case 0x17: {   /* FSEQ FSSET FSAND FSOR */
        uint32_t imm = ((op >> 10) & 0x800) | (op & 0x7FF);
        flags_sync(vu);
        q_sync(vu);
        switch (opc) {
        case 0x14: set_vi(vu, ft, (vu->status & 0xFFF) == imm); break;
        case 0x15: vu->status = (vu->status & 0x3F) | (imm & 0xFC0); break;
        case 0x16: set_vi(vu, ft, vu->status & imm); break;
        default: set_vi(vu, ft, (vu->status & 0xFFF) | imm); break;          /* bitwise, unlike FCOR */
        }
        return;
    }
    case 0x18: case 0x1A: case 0x1B: {              /* FMEQ FMAND FMOR */
        flags_sync(vu);
        if (opc == 0x18)
            set_vi(vu, ft, (vu->mac & 0xFFFF) == ivi(vu, fs));
        else if (opc == 0x1A)
            set_vi(vu, ft, vu->mac & ivi(vu, fs));
        else                            /* FMOR: bitwise OR (Burnout 3's collision tests OR sign flags) */
            set_vi(vu, ft, (vu->mac & 0xFFFF) | ivi(vu, fs));
        return;
    }
    case 0x1C:                  /* FCGET */
        flags_sync(vu);
        set_vi(vu, ft, vu->clip & 0xFFF);
        return;
    case 0x20:                  /* B */
        lo->branch = 1;
        lo->target = pc + 8 + imm11(op) * 8;
        return;
    case 0x21:                  /* BAL */
        set_vi(vu, ft, (pc + 16) / 8);
        lo->branch = 1;
        lo->target = pc + 8 + imm11(op) * 8;
        return;
    case 0x24:                  /* JR */
        lo->branch = 1;
        lo->target = ivi(vu, fs) * 8u;
        return;
    case 0x25: {                /* JALR */
        uint32_t t = ivi(vu, fs) * 8u;
        set_vi(vu, ft, (pc + 16) / 8);
        lo->branch = 1;
        lo->target = t;
        return;
    }
    case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
        uint16_t a = bvi(vu, fs), b = bvi(vu, ft);
        int16_t s = (int16_t)a;
        int take;
        switch (opc) {
        case 0x28: take = b == a; break;
        case 0x29: take = b != a; break;
        case 0x2C: take = s < 0; break;
        case 0x2D: take = s > 0; break;
        case 0x2E: take = s <= 0; break;
        default: take = s >= 0; break;
        }
        if (take) {
            lo->branch = 1;
            lo->target = pc + 8 + imm11(op) * 8;
        }
        return;
    }
    case 0x40:
        break;
    default:
        LOG("VU%d: unknown lower %02X (%08X) at %04X", vu->unit, opc, op, pc);
        return;
    }

    /* 0x40: arithmetic and the 0x3C-0x3F extended group */
    switch (op & 0x3F) {
    case 0x30: case 0x31: case 0x34: case 0x35:
        vi_backup(vu, FD(op));
        break;
    case 0x32:
        vi_backup(vu, ft);
        break;
    }
    switch (op & 0x3F) {
    case 0x30: set_vi(vu, FD(op), ivi(vu, fs) + ivi(vu, ft)); return;            /* IADD */
    case 0x31: set_vi(vu, FD(op), ivi(vu, fs) - ivi(vu, ft)); return;            /* ISUB */
    case 0x32: set_vi(vu, ft, ivi(vu, fs) + (uint32_t)((int32_t)(op << 21) >> 27)); return;  /* IADDI */
    case 0x34: set_vi(vu, FD(op), ivi(vu, fs) & ivi(vu, ft)); return;            /* IAND */
    case 0x35: set_vi(vu, FD(op), ivi(vu, fs) | ivi(vu, ft)); return;            /* IOR */
    case 0x3C: case 0x3D: case 0x3E: case 0x3F:
        break;
    default:
        LOG("VU%d: unknown lower op %02X (%08X) at %04X", vu->unit, op & 0x3F, op, pc);
        return;
    }
    {
        uint32_t x = ((op >> 4) & 0x7C) | (op & 3);
        uint32_t fsf = (op >> 21) & 3, ftf = (op >> 23) & 3;
        float v[4];
        int k;
        switch (x) {
        case 0x30:              /* MOVE */
            stall_on(vu, fs, dest);
            vf_write(vu, ft, dest, vf[fs]);
            return;
        case 0x31:              /* MR32 */
            stall_on(vu, fs, ((dest >> 1) | (dest << 3)) & 15);
            v[0] = vf[fs][1]; v[1] = vf[fs][2]; v[2] = vf[fs][3]; v[3] = vf[fs][0];
            vf_write(vu, ft, dest, v);
            return;
        case 0x34:              /* LQI ft, (is++) */
            load_q(vu, ft, dest, ivi(vu, fs));
            vi_backup(vu, fs);
            set_vi(vu, fs, ivi(vu, fs) + 1);
            return;
        case 0x35:              /* SQI fs, (it++) */
            store_q(vu, fs, dest, ivi(vu, ft));
            vi_backup(vu, ft);
            set_vi(vu, ft, ivi(vu, ft) + 1);
            return;
        case 0x36:              /* LQD ft, (--is) */
            vi_backup(vu, fs);
            set_vi(vu, fs, ivi(vu, fs) - 1);
            load_q(vu, ft, dest, ivi(vu, fs));
            return;
        case 0x37:              /* SQD fs, (--it) */
            vi_backup(vu, ft);
            set_vi(vu, ft, ivi(vu, ft) - 1);
            store_q(vu, fs, dest, ivi(vu, ft));
            return;
        case 0x38: {            /* DIV Q, fs.fsf, ft.ftf */
            float n, d;
            uint32_t st = 0;
            stall_on(vu, fs, 8u >> fsf);
            stall_on(vu, ft, 8u >> ftf);
            n = vin(vf[fs][fsf]);
            d = vin(vf[ft][ftf]);
            if (d == 0.0f) {
                st = n == 0.0f ? 0x10 : 0x20;
                v[0] = fbits(((ubits(n) ^ ubits(d)) & 0x80000000u) | 0x7F7FFFFFu);
            } else {
                v[0] = n / d;
            }
            div_issue(vu, v[0], st, 7);
            return;
        }
        case 0x39: {            /* SQRT Q, ft.ftf */
            float d;
            stall_on(vu, ft, 8u >> ftf);
            d = vin(vf[ft][ftf]);
            div_issue(vu, ps2_sqrtf(fabsf(d)), d < 0 ? 0x10 : 0, 7);
            return;
        }
        case 0x3A: {            /* RSQRT Q, fs.fsf, ft.ftf */
            float n, d;
            uint32_t st = 0;
            stall_on(vu, fs, 8u >> fsf);
            stall_on(vu, ft, 8u >> ftf);
            n = vin(vf[fs][fsf]);
            d = vin(vf[ft][ftf]);
            if (d == 0.0f) {                /* x/0: +-max (D); 0/0: +-0 (D and I) */
                uint32_t sign = (ubits(n) ^ ubits(d)) & 0x80000000u;
                st = n == 0.0f ? 0x30 : 0x20;
                v[0] = fbits(n == 0.0f ? sign : sign | 0x7F7FFFFFu);
            } else {
                if (d < 0)
                    st = 0x10;
                v[0] = n / ps2_sqrtf(fabsf(d));
            }
            div_issue(vu, v[0], st, 13);
            return;
        }
        case 0x3B:              /* WAITQ */
            if (vu->q_pending && vu->cycle < vu->q_ready)
                vu->cycle = vu->q_ready;
            q_sync(vu);
            return;
        case 0x3C:              /* MTIR it, fs.fsf */
            stall_on(vu, fs, 8u >> fsf);
            vi_backup(vu, ft);
            set_vi(vu, ft, ubits(vf[fs][fsf]));
            return;
        case 0x3D:              /* MFIR ft, is */
            for (k = 0; k < 4; k++)
                v[k] = fbits((uint32_t)(int32_t)(int16_t)ivi(vu, fs));
            vf_write(vu, ft, dest, v);
            return;
        case 0x3E: {            /* ILWR it, (is) */
            uint32_t *m = memq(vu, ivi(vu, fs));
            set_vi(vu, ft, m[first_lane(dest)]);
            return;
        }
        case 0x3F: {            /* ISWR it, (is) */
            uint32_t *m = memq(vu, ivi(vu, fs));
            for (k = 0; k < 4; k++)
                if (LANE(dest, k))
                    m[k] = ivi(vu, ft);
            return;
        }
        case 0x40:              /* RNEXT */
            *vu->r = ps2_vu_rnext(*vu->r);
            /* fall through */
        case 0x41:              /* RGET */
            for (k = 0; k < 4; k++)
                v[k] = fbits(*vu->r);
            vf_write(vu, ft, dest, v);
            return;
        case 0x42:              /* RINIT R, fs.fsf */
            stall_on(vu, fs, 8u >> fsf);
            *vu->r = 0x3F800000u | (ubits(vf[fs][fsf]) & 0x7FFFFF);
            return;
        case 0x43:              /* RXOR */
            stall_on(vu, fs, 8u >> fsf);
            *vu->r = 0x3F800000u | ((*vu->r ^ ubits(vf[fs][fsf])) & 0x7FFFFF);
            return;
        case 0x64:              /* MFP ft, P */
            p_sync(vu);
            for (k = 0; k < 4; k++)
                v[k] = *vu->p;
            vf_write(vu, ft, dest, v);
            return;
        case 0x68:              /* XTOP it */
            set_vi(vu, ft, vif_top(vu->unit));
            return;
        case 0x69:              /* XITOP it */
            set_vi(vu, ft, vif_itop(vu->unit));
            return;
        case 0x6C:              /* XGKICK is */
            lo->xgkick = 1;
            lo->kick_addr = ivi(vu, fs);
            return;
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x76: {   /* ESADD ERSADD ELENG ERLENG ESUM */
            float x0 = vin(vf[fs][0]), y0 = vin(vf[fs][1]), z0 = vin(vf[fs][2]), sq;
            stall_on(vu, fs, x == 0x76 ? 0xFu : 0xEu);
            sq = x0 * x0 + y0 * y0 + z0 * z0;
            switch (x) {                    /* zero stays zero (PCSX2) */
            case 0x70: efu_issue(vu, sq, 11); break;
            case 0x71: efu_issue(vu, sq != 0 ? 1.0f / sq : 0.0f, 18); break;
            case 0x72: efu_issue(vu, ps2_sqrtf(sq), 18); break;
            case 0x73: efu_issue(vu, sq != 0 ? 1.0f / ps2_sqrtf(sq) : 0.0f, 24); break;
            default: efu_issue(vu, x0 + y0 + z0 + vin(vf[fs][3]), 12); break;
            }
            return;
        }
        case 0x74:              /* EATANxy: atan series of y/x (+pi/4), 0 when x is 0 */
            stall_on(vu, fs, 0xC);
            efu_issue(vu, vin(vf[fs][0]) != 0 ? eatan(vin(vf[fs][1]) / vin(vf[fs][0])) : 0.0f, 54);
            return;
        case 0x75:              /* EATANxz */
            stall_on(vu, fs, 0xA);
            efu_issue(vu, vin(vf[fs][0]) != 0 ? eatan(vin(vf[fs][2]) / vin(vf[fs][0])) : 0.0f, 54);
            return;
        case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7D: case 0x7E: {
            float s;
            stall_on(vu, fs, 8u >> fsf);
            s = vin(vf[fs][fsf]);
            switch (x) {                    /* PCSX2's hardware-checked forms */
            case 0x78: efu_issue(vu, s >= 0 ? ps2_sqrtf(s) : s, 12); break;                     /* ESQRT */
            case 0x79: efu_issue(vu, s > 0 ? 1.0f / ps2_sqrtf(s) : s, 18); break;               /* ERSQRT */
            case 0x7A: efu_issue(vu, s != 0 ? 1.0f / s : 0.0f, 12); break;                  /* ERCPR */
            case 0x7C: efu_issue(vu, esin(s), 29); break;                                   /* ESIN */
            case 0x7D: efu_issue(vu, eatan(s), 54); break;                                  /* EATAN */
            default: efu_issue(vu, eexp(s), 44); break;                                     /* EEXP */
            }
            return;
        }
        case 0x7B:              /* WAITP */
            if (vu->p_pending && vu->cycle < vu->p_ready)
                vu->cycle = vu->p_ready;
            p_sync(vu);
            return;
        default:
            LOG("VU%d: unknown lower ext %02X (%08X) at %04X", vu->unit, x, op, pc);
            return;
        }
    }
}

/* ---- execution ------------------------------------------------------------------------------- */

#define MAX_STEPS 4000000

#if defined(PS2_PROFILE)
/* Instructions executed per (unit, start address), for the "prof" report. */
/* VU1 runs are also keyed by the microcode chain last uploaded (vif.c). */
static struct { uint64_t key; uint64_t insns, runs, kicks; } g_vprof[256];

static void vprof_add(int unit, uint32_t start, long steps, unsigned kicks)
{
    extern uint32_t g_vu1_code;
    uint64_t key = (uint64_t)(unit ? g_vu1_code : 0) << 32 | (uint32_t)unit << 16 | start;
    for (int k = 0; k < 256; k++) {
        if (!g_vprof[k].runs || g_vprof[k].key == key) {
            g_vprof[k].key = key;
            g_vprof[k].insns += (uint64_t)steps;
            g_vprof[k].runs++;
            g_vprof[k].kicks += kicks;
            return;
        }
    }
}

void vu_prof_dump(FILE *f)
{
    for (int k = 0; k < 256 && g_vprof[k].runs; k++)
        fprintf(f, "VU%u code %06X %04X  runs %8llu  insns %10llu  xgkicks %8llu\n",
                (unsigned)(g_vprof[k].key >> 16) & 1, (unsigned)(g_vprof[k].key >> 32),
                (unsigned)g_vprof[k].key & 0xFFFF, (unsigned long long)g_vprof[k].runs,
                (unsigned long long)g_vprof[k].insns, (unsigned long long)g_vprof[k].kicks);
    memset(g_vprof, 0, sizeof g_vprof);
}
#endif

/* "vucap N" (PC): the next N VU1 runs are written to work/vucap/ for
 * studying the microprograms: registers and data memory before and after,
 * TOP/ITOP and the XGKICK addresses */
int g_vucap_n;
uint32_t g_vucap_code;          /* only runs of this microcode chain, if set */
static int g_vucap_on, g_vucap_seq;
static uint32_t g_vucap_kicks[64];
static int g_vucap_nkicks;

/* kick addresses of the run being checked (vu_rec_check) */
static int g_kick_rec;
static uint32_t g_kicks[2][64];
static int g_nkicks[2];

extern uint32_t g_vu1_code;

static void interp(VU *vu, uint32_t pc)
{
#if defined(PS2_PROFILE)
    uint32_t start = pc;
    unsigned kicks = 0;
#endif
    int branch_in = 0, end_in = 0;
    uint32_t branch_to = 0;
    long steps = 0;
    vu->runs++;
    vu->bk_cycles = 0;
    if (vu->unit == 1) {
        extern uint32_t g_vu1_prog;
        g_vu1_prog = pc;
        vif_mpg_flush();                /* it reads micro memory */
    }
    for (;;) {
        uint32_t idx = (pc & (vu->micro_bytes - 1)) >> 2;
        uint32_t lop = vu->micro[idx], uop = vu->micro[idx + 1];
        Upper u;
        Lower lo;
        lo.branch = 0;
        lo.xgkick = 0;
        if (!(uop & 0x80000000u))
            lower_stalls(vu, lop);
        upper(vu, uop, &u);
        if (uop & 0x80000000u)              /* I bit: the lower word is the I register */
            *vu->ireg = fbits(lop);
        else
            lower(vu, lop, pc, &lo);
        upper_commit(vu, &u, pc);
        if (lo.xgkick && vu->unit == 1) {
            if (g_vucap_on && g_vucap_nkicks < 64)
                g_vucap_kicks[g_vucap_nkicks++] = lo.kick_addr & 1023;
            if (g_kick_rec) {
                if (g_nkicks[0] < 64)
                    g_kicks[0][g_nkicks[0]] = lo.kick_addr & 1023;
                g_nkicks[0]++;
            }
            gif_xgkick(vu->mem, vu->mem_qw, lo.kick_addr);
#if defined(PS2_PROFILE)
            kicks++;
#endif
        }
        if (vu->bk_cycles)
            vu->bk_cycles--;
        vu->cycle++;
        if (++steps > MAX_STEPS) {
            LOG("VU%d: program at %04X does not end, stopped", vu->unit, pc);
            break;
        }
        if (end_in && --end_in == 0) {
            pc += 8;
            if (branch_in)
                pc = branch_to;
            break;
        }
        if (uop & 0x40000000u)              /* E bit: one more instruction */
            end_in = 1;
        if (branch_in && --branch_in == 0) {
            pc = branch_to;
            continue;
        }
        if (lo.branch) {
            branch_in = 1;
            branch_to = lo.target & (vu->micro_bytes - 1);
        }
        pc += 8;
    }
    vu->tpc = pc & (vu->micro_bytes - 1);
    flags_drain(vu);
#if defined(PS2_PROFILE)
    vprof_add(vu->unit, start, steps, kicks);
#endif
}

/* ---- recompiled programs ------------------------------------------------------------------- */

int g_vu_mode = 1;                      /* 0: interpret, 1: run recompiled programs, 2: run both and compare */
extern int g_opt_new;                   /* gs.c (autopilot "newopt 0") */

static uint32_t g_micro_gen[2] = {1, 1};
static struct { uint32_t gen; const VuProg *prog; } g_rec_cache[2][2048];
/* When micro memory was last written, by 64-byte block (g_micro_gen's value then), and when last written where it is
 * not known (g_micro_all): a program found for an entry still holds when none of its ranges' blocks was written
 * since (the game swaps VU0's programs 7 to 19 times a frame, and every entry was searched for again after each). */
static uint32_t g_micro_blk[2][256], g_micro_all[2] = {1, 1};
uint32_t g_rec_kept[2];                 /* status line: lookups answered so */

static int rec_unwritten(int unit, const VuProg *p, uint32_t pc, uint32_t since)
{
    int e, r;
    if (g_micro_all[unit] > since)
        return 0;
    for (e = 0; e < p->nentries && p->entries[e].addr != pc; e++)
        ;
    if (e == p->nentries)
        return 0;
    for (r = 0; r < p->entries[e].nranges; r++) {
        const VuRange *g = &p->entries[e].ranges[r];
        uint32_t b = g->addr / 64, b1 = (g->addr + g->n * 8 - 1) / 64;
        if (!g->n)
            continue;
        for (; b <= b1; b++)
            if (g_micro_blk[unit][b & 255] > since)
                return 0;
    }
    return 1;
}
static unsigned g_rec_runs[2], g_rec_misses[2], g_rec_fallbacks[2];

void vu_rec_kick(const uint32_t *mem, uint32_t qaddr)
{
    if (g_vucap_on && g_vucap_nkicks < 64)
        g_vucap_kicks[g_vucap_nkicks++] = qaddr & 1023;
    if (g_kick_rec) {
        if (g_nkicks[1] < 64)
            g_kicks[1][g_nkicks[1]] = qaddr & 1023;
        g_nkicks[1]++;
        return;
    }
    gif_xgkick(mem, 1024, qaddr);
}

/* the compiled program for a run from pc, if micro memory holds its code */
static struct RecChain {
    uint32_t chain, pc;
    const VuProg *prog;
} g_rec_chain[64];

void vif_mpg_flush(void);                                       /* vif.c: VU1 microprograms noted, not copied */
const uint32_t *vif_micro_words(uint32_t off, uint32_t n);

/* 16 bytes of the unit's micro memory at word a against the program's */
static inline int micro_differs(VU *vu, const VuProg *p, uint32_t a, uint32_t bytes)
{
    const uint32_t *m = vu->micro + a;
    if (!bytes)
        return 0;
    if (vu->unit == 1) {
        m = vif_micro_words(a, (bytes + 3) / 4);
        if (!m) {
            vif_mpg_flush();
            m = vu->micro + a;
        }
    }
    return memcmp(m, p->code + a, bytes) != 0;
}

static const VuProg *rec_find(VU *vu, uint32_t pc)
{
    extern uint32_t g_vu1_code;
    int unit = vu->unit, k, e, r;
    uint32_t slot = (pc >> 3) & 2047;
    const VuProg *found = NULL;
    struct RecChain *c = NULL;
    if (g_rec_cache[unit][slot].gen == g_micro_gen[unit])
        return g_rec_cache[unit][slot].prog;
    if (unit == 0 && g_rec_cache[0][slot].prog && rec_unwritten(0, g_rec_cache[0][slot].prog, pc, g_rec_cache[0][slot].gen)) {
        /* nothing it runs was written since it was found */
        g_rec_cache[0][slot].gen = g_micro_gen[0];
        g_rec_kept[0]++;
        return g_rec_cache[0][slot].prog;
    }
    if (unit == 1) {
        /* The game loads a microprogram before nearly every group of batches
         * (about 70 times a frame), each time the same few from the same
         * places, and each time the whole program was compared here (1% of a
         * frame). A program found once for this chain and entry is taken
         * again when 12 words of each of its ranges still match. */
        c = &g_rec_chain[((g_vu1_code >> 4) ^ (pc >> 3) * 0x9E3779B1u) & 63];
        if (c->prog && c->chain == g_vu1_code && c->pc == pc) {
            const VuProg *p = c->prog;
            for (e = 0; e < p->nentries && p->entries[e].addr != pc; e++)
                ;
            if (e < p->nentries) {
                const VuEntry *en = &p->entries[e];
                for (r = 0; r < en->nranges; r++) {
                    const uint32_t a = en->ranges[r].addr / 4, n = en->ranges[r].n * 2, m = n > 8 ? (n / 2) & ~1u : 0;
                    const uint32_t t = n > 4 ? n - 4 : 0;
                    if (micro_differs(vu, p, a, n < 4 ? n * 4 : 16) || micro_differs(vu, p, a + m, n < 4 ? 0 : 16) ||
                        micro_differs(vu, p, a + t, n < 4 ? 0 : 16))
                        break;
                }
                if (r == en->nranges) {
                    g_rec_cache[unit][slot].gen = g_micro_gen[unit];
                    g_rec_cache[unit][slot].prog = p;
                    return p;
                }
            }
        }
    }
    if (unit == 1)
        vif_mpg_flush();                /* every program against the memory itself */
    for (k = 0; k < g_vu_nprogs && !found; k++) {
        const VuProg *p = &g_vu_progs[k];
        if (p->unit != unit)
            continue;
        for (e = 0; e < p->nentries; e++) {
            const VuEntry *en = &p->entries[e];
            if (en->addr != pc)
                continue;
            for (r = 0; r < en->nranges; r++)
                if (memcmp(vu->micro + en->ranges[r].addr / 4, p->code + en->ranges[r].addr / 4,
                           en->ranges[r].n * 8))
                    break;
            if (r == en->nranges)
                found = p;
            break;
        }
    }
    if (!found) {
        g_rec_misses[unit]++;
        if (g_rec_misses[unit] <= 20)
            LOG("VU%d: no recompiled program for a run from %04X (code %06X), interpreting", unit, pc,
                unit ? g_vu1_code : 0);
    }
    g_rec_cache[unit][slot].gen = g_micro_gen[unit];
    g_rec_cache[unit][slot].prog = found;
    if (c && found) {
        c->chain = g_vu1_code;
        c->pc = pc;
        c->prog = found;
    }
    return found;
}

/* VU0 routines in their lean versions (vurecomp.py, config VU0_LIVE): a routine stores only what the
 * game reads after it. g_vu0_lean 0 (autopilot "vu0lean 0"): the full versions. With "vu0chk 1" every
 * lean run is followed by the full one from the same state and what the lean one left is compared
 * (rec_lean_check); with "vu0poison 1" what it did not leave is overwritten. */
int g_vu0_lean = 1;
extern int g_vu0_chk_all, g_vu0_poison;
static uint32_t g_lean_runs, g_lean_chk, g_lean_bad;

static const VuEntry *rec_entry(const VuProg *p, uint32_t pc)
{
    int e;
    for (e = 0; e < p->nentries; e++)
        if (p->entries[e].addr == pc)
            return &p->entries[e];
    return NULL;
}

static void rec_lean_check(VU *vu, const VuProg *p, uint32_t pc, VuCtx *c)
{
    static float vf0[32][4], vfl[32][4], acc0[4];
    static uint32_t vi0[16], vil[16], mem0[1024];
    const VuEntry *en = rec_entry(p, pc);
    const VuCtx c0 = *c;
    const float q0 = *vu->q, i0 = *vu->ireg;
    float ql;
    uint32_t r, k, bad = 0;
    memcpy(vf0, vu->vf, sizeof vf0);
    memcpy(vi0, vu->vi, sizeof vi0);
    memcpy(acc0, vu->acc, sizeof acc0);
    memcpy(mem0, vu->mem, sizeof mem0);
    p->fn_lean(c, pc);
    memcpy(vfl, vu->vf, sizeof vfl);
    memcpy(vil, vu->vi, sizeof vil);
    ql = *vu->q;
    /* from the same state, in full: that is what stays */
    memcpy(vu->vf, vf0, sizeof vf0);
    memcpy(vu->vi, vi0, sizeof vi0);
    memcpy(vu->acc, acc0, sizeof acc0);
    memcpy(vu->mem, mem0, sizeof mem0);
    *vu->q = q0;
    *vu->ireg = i0;
    *c = c0;
    p->fn(c, pc);
    g_lean_chk++;
    if (!en || !en->lean)
        return;
    for (r = 1; r < 32; r++)
        for (k = 0; k < 4; k++)
            if (!(en->dead[k] >> r & 1) && memcmp(&vfl[r][k], &vu->vf[r][k], 4))
                bad++;
    for (r = 1; r < 16; r++)
        if (!(en->dead_vi >> r & 1) && (vil[r] & 0xFFFF) != (vu->vi[r] & 0xFFFF))
            bad++;
    if (memcmp(&ql, vu->q, 4))
        bad++;
    if (bad && g_lean_bad++ < 10)
        LOG("VU0: lean routine %06X@%04X leaves %u registers other than the full one", p->chain, pc, bad);
}

static void rec_lean_poison(VU *vu, const VuProg *p, uint32_t pc)
{
    const VuEntry *en = rec_entry(p, pc);
    uint32_t r, k;
    if (!en || !en->lean)
        return;
    for (r = 1; r < 32; r++)
        for (k = 0; k < 4; k++)
            if (en->dead[k] >> r & 1)
                vu->vf[r][k] = fbits(0x7FC0DEADu);
    for (r = 1; r < 16; r++)
        if (en->dead_vi >> r & 1)
            vu->vi[r] = 0xDEAD;
}

/* "vu0time 1": recompiled VU0 calls timed whole (ps2_vu0_callms's fast path) and their routines alone (rec_run):
 * what the calls cost besides the microprogram's work */
int g_vu0_time;
uint64_t g_vu0t_whole, g_vu0t_body;
uint32_t g_vu0t_n, g_vu0t_coll[7][2];
uint32_t g_coll_stat[4];                /* 0x273070's sphere tests (hle_coll_sphere): looked at, decided, checked, wrong */

/* the context a unit's compiled code gets: its pointers stay while the unit's registers are where they were (VU0's
 * move with the EE context that calls it, vu0_bind); the outputs are written by the code at every exit */
static VuCtx g_rec_ctx[2];

static void rec_run(VU *vu, const VuProg *p, uint32_t pc)
{
    VuCtx *const cp = &g_rec_ctx[vu->unit & 1];
#define c (*cp)
    uint64_t tb = 0;
    /* compiled code starts from a settled pipeline */
    if (vu->q_pending) {
        *vu->q = vu->q_next;
        vu->q_pending = 0;
    }
    if (vu->p_pending) {
        *vu->p = vu->p_next;
        vu->p_pending = 0;
    }
    /* flags_drain: Q and P are settled above; the flags only when some are in flight */
    vu->cycle += 8;
    if (vu->fq_n || !g_opt_new)
        flags_sync(vu);
    if (c.vf != vu->vf || c.vi != vu->vi || c.mem != vu->mem || !g_opt_new) {
        c.vf = vu->vf;
        c.vi = vu->vi;
        c.acc = vu->acc;
        c.q = vu->q;
        c.p = vu->p;
        c.i = vu->ireg;
        c.r = vu->r;
        c.mem = vu->mem;
    }
    c.mac = vu->mac;
    c.clip = vu->clip;
    if (p->fn_lean && g_vu0_lean) {
        g_lean_runs++;
        if (PS2_UNLIKELY(g_vu0_chk_all))
            rec_lean_check(vu, p, pc, cp);
        else if (PS2_UNLIKELY(g_vu0_time) && vu->unit == 0) {
            tb = plat_ticks();
            p->fn_lean(cp, pc);
            g_vu0t_body += plat_ticks() - tb;
        } else
            p->fn_lean(cp, pc);
        if (PS2_UNLIKELY(g_vu0_poison) && !c.fallback)
            rec_lean_poison(vu, p, pc);
    } else {
        p->fn(cp, pc);
    }
    g_rec_runs[vu->unit]++;
    vu->mac = c.mac;
    vu->clip = c.clip;
    vu->cycle += 64;
    if (c.q_left) {
        vu->q_next = c.qn;
        vu->q_ready = vu->cycle + (uint64_t)c.q_left;
        vu->q_pending = 1;
    }
    if (c.p_left) {
        vu->p_next = c.pn;
        vu->p_ready = vu->cycle + (uint64_t)c.p_left;
        vu->p_pending = 1;
    }
    vu->tpc = c.tpc & (vu->micro_bytes - 1);
    if (c.fallback) {
        if (++g_rec_fallbacks[vu->unit] <= 20)
            LOG("VU%d: program %06X left its code at %04X (run from %04X), interpreting", vu->unit,
                p->chain, c.tpc, pc);
        interp(vu, vu->tpc);
    }
#undef c
}

/* VU state for vu_rec_check */
typedef struct VuSnap {
    float vf[32][4], acc[4], q, p, i;
    uint32_t vi[16], r, mac, clip, tpc;
    int q_pending, p_pending;
    float q_next, p_next;
    uint32_t mem[1024 * 4];
} VuSnap;

static void snap_take(VU *vu, VuSnap *s)
{
    memcpy(s->vf, vu->vf, sizeof s->vf);
    memcpy(s->vi, vu->vi, sizeof s->vi);
    memcpy(s->acc, vu->acc, sizeof s->acc);
    s->q = *vu->q;
    s->p = *vu->p;
    s->i = *vu->ireg;
    s->r = *vu->r;
    s->mac = vu->mac;
    s->clip = vu->clip;
    s->tpc = vu->tpc;
    s->q_pending = vu->q_pending;
    s->p_pending = vu->p_pending;
    s->q_next = vu->q_next;
    s->p_next = vu->p_next;
    memcpy(s->mem, vu->mem, vu->mem_qw * 16);
}

static void snap_put(VU *vu, const VuSnap *s)
{
    memcpy(vu->vf, s->vf, sizeof s->vf);
    memcpy(vu->vi, s->vi, sizeof s->vi);
    memcpy(vu->acc, s->acc, sizeof s->acc);
    *vu->q = s->q;
    *vu->p = s->p;
    *vu->ireg = s->i;
    *vu->r = s->r;
    vu->mac = s->mac;
    vu->clip = s->clip;
    vu->tpc = s->tpc;
    vu->q_pending = s->q_pending;
    vu->p_pending = s->p_pending;
    vu->q_next = s->q_next;
    vu->p_next = s->p_next;
    memcpy(vu->mem, s->mem, vu->mem_qw * 16);
}

static unsigned g_check_runs, g_check_bad;

/* run the interpreter (whose results stand) and the compiled program from the
 * same state and report where they differ */
static void rec_check(VU *vu, const VuProg *p, uint32_t pc)
{
    static VuSnap s0, s1;
    char what[160];
    int k, n = 0;
    snap_take(vu, &s0);
    g_kick_rec = 1;
    g_nkicks[0] = g_nkicks[1] = 0;
    interp(vu, pc);
    snap_take(vu, &s1);
    snap_put(vu, &s0);
    rec_run(vu, p, pc);
    g_kick_rec = 0;
    what[0] = 0;
#define DIFF(...) do { if (!n++) snprintf(what, sizeof what, __VA_ARGS__); } while (0)
    for (k = 0; k < 32 * 4; k++)
        if (memcmp(&vu->vf[k >> 2][k & 3], &s1.vf[k >> 2][k & 3], 4))
            DIFF("vf%d%c %g vs %g", k >> 2, "xyzw"[k & 3], (double)vu->vf[k >> 2][k & 3], (double)s1.vf[k >> 2][k & 3]);
    for (k = 1; k < 16; k++)
        if (vu->vi[k] != s1.vi[k])
            DIFF("vi%d %04X vs %04X", k, vu->vi[k], s1.vi[k]);
    for (k = 0; k < 4; k++)
        if (memcmp(&vu->acc[k], &s1.acc[k], 4))
            DIFF("acc%c %g vs %g", "xyzw"[k], (double)vu->acc[k], (double)s1.acc[k]);
    if (memcmp(vu->q, &s1.q, 4))
        DIFF("Q %g vs %g", (double)*vu->q, (double)s1.q);
    if (vu->clip != s1.clip)
        DIFF("clip %06X vs %06X", vu->clip, s1.clip);
    if (vu->mac != s1.mac)
        DIFF("mac %04X vs %04X", vu->mac, s1.mac);
    if (vu->tpc != s1.tpc)
        DIFF("tpc %04X vs %04X", vu->tpc, s1.tpc);
    for (k = 0; k < (int)vu->mem_qw * 4; k++)
        if (vu->mem[k] != s1.mem[k])
            DIFF("mem qw %03X.%c %08X vs %08X", k >> 2, "xyzw"[k & 3], vu->mem[k], s1.mem[k]);
    if (g_nkicks[0] != g_nkicks[1] || memcmp(g_kicks[0], g_kicks[1], sizeof g_kicks[0][0] *
                                             (g_nkicks[0] < 64 ? g_nkicks[0] : 64)))
        DIFF("kicks %d vs %d", g_nkicks[1], g_nkicks[0]);
#undef DIFF
    g_check_runs++;
    if (n) {
        g_check_bad++;
        if (g_check_bad <= 200)
            LOG("VU%d check: program %06X from %04X: %d differences (recompiled vs interpreter), first %s",
                vu->unit, p->chain, pc, n, what);
    }
    if ((g_check_runs & 0xFFFF) == 0)
        LOG("VU check: %u runs, %u differ", g_check_runs, g_check_bad);
    snap_put(vu, &s1);
}

#if defined(PS2_PROFILE)
typedef struct VuCapRegs {
    float vf[32][4], acc[4], q, p, i;
    uint32_t vi[16];
} VuCapRegs;

static void vucap_regs(VU *vu, VuCapRegs *r)
{
    memcpy(r->vf, vu->vf, sizeof r->vf);
    memcpy(r->acc, vu->acc, sizeof r->acc);
    memcpy(r->vi, vu->vi, sizeof r->vi);
    r->q = *vu->q;
    r->p = *vu->p;
    r->i = *vu->ireg;
}
#endif

static void run2(VU *vu, uint32_t pc);

static void run(VU *vu, uint32_t pc)
{
#if defined(PS2_PROFILE)
    if (g_vucap_n > 0 && vu->unit == 1 && (!g_vucap_code || g_vucap_code == g_vu1_code)) {
        /* file: header words (code chain, entry, top, itop, kick count, kicks[64]),
         * registers before, memory before, registers after, memory after */
        static VuCapRegs r0, r1;
        static uint32_t m0[4096];
        uint32_t hdr[5];
        char path[64];
        FILE *f;
        vucap_regs(vu, &r0);
        memcpy(m0, vu->mem, sizeof m0);
        g_vucap_on = 1;
        g_vucap_nkicks = 0;
        run2(vu, pc);
        g_vucap_on = 0;
        vucap_regs(vu, &r1);
        hdr[0] = g_vu1_code;
        hdr[1] = pc;
        hdr[2] = vif_top(1);
        hdr[3] = vif_itop(1);
        hdr[4] = (uint32_t)g_vucap_nkicks;
        snprintf(path, sizeof path, "work/vucap/%04d_%06X_%04X.bin", g_vucap_seq++, g_vu1_code, pc);
        f = fopen(path, "wb");
        if (f) {
            fwrite(hdr, 4, 5, f);
            fwrite(g_vucap_kicks, 4, 64, f);
            fwrite(&r0, sizeof r0, 1, f);
            fwrite(m0, 4, 4096, f);
            fwrite(&r1, sizeof r1, 1, f);
            fwrite(vu->mem, 4, 4096, f);
            fclose(f);
        }
        if (--g_vucap_n == 0)
            LOG("vucap: done (%d runs)", g_vucap_seq);
        return;
    }
#endif
    run2(vu, pc);
}

/* 16-byte copies as words (memcpy calls were not inlined) */
static inline void cp4(void *d, const void *s)
{
    uint32_t *dw = d;
    const uint32_t *sw = s;
    dw[0] = sw[0];
    dw[1] = sw[1];
    dw[2] = sw[2];
    dw[3] = sw[3];
}

/* MAC sign flags (x = 0x80 ... w = 0x10, within mask) of d - n.p for the
 * four planes whose normals and distances are the columns of pl[0..3], as
 * the program computes them: ACC = d, ACC -= nx px, ACC -= ny py, result =
 * ACC - nz pz. ACC is left in acc. */
static uint32_t box_corner(const float (*pl)[4], const float *pt, float *acc, uint32_t mask)
{
    uint32_t m = 0;
    int k;
    for (k = 0; k < 4; k++) {
        float a = pl[3][k] - pl[0][k] * pt[0];
        a = a - pl[1][k] * pt[1];
        acc[k] = a;
        if (a - pl[2][k] * pt[2] < 0.0f)
            m |= 0x80u >> k;
    }
    return m & mask;
}

/* Are the eight corners c[0..7] all outside the plane (n, d) (2), some (1)
 * or none (0)? Each corner as the program tests it (d - n.p in its order,
 * the sign of the result). Taking the extremes from the box's edges
 * instead (four dot products) differed at a hair's breadth from a plane:
 * the corners went through the matrix one by one and are no exact box, and
 * objects at the edge of a view were dropped for a frame. */
static inline int box_plane8(float nx, float ny, float nz, float d, const float (*c)[4])
{
    int i, out = 0;
    for (i = 0; i < 8; i++) {
        float a = d - nx * c[i][0];
        a = a - ny * c[i][1];
        out += a - nz * c[i][2] < 0.0f;
    }
    return out == 8 ? 2 : out > 0;
}

/* The sphere around eight corners: their mean, the largest squared distance
 * from it, and the mean's squared length. */
typedef struct BoxSphere { float c[3], r2, cc; } BoxSphere;

static inline void box_sphere(const float (*c)[4], BoxSphere *s)
{
    float x = 0.0f, y = 0.0f, z = 0.0f, m = 0.0f;
    int i;
    for (i = 0; i < 8; i++) {
        x += c[i][0];
        y += c[i][1];
        z += c[i][2];
    }
    x *= 0.125f;
    y *= 0.125f;
    z *= 0.125f;
    for (i = 0; i < 8; i++) {
        float dx = c[i][0] - x, dy = c[i][1] - y, dz = c[i][2] - z, q = dx * dx + dy * dy + dz * dz;
        if (q > m)
            m = q;
    }
    s->c[0] = x;
    s->c[1] = y;
    s->c[2] = z;
    s->r2 = m;
    s->cc = x * x + y * y + z * z;
}

/* box_plane8 with the corners' sphere: most boxes are far from most planes,
 * and then the centre decides for all eight corners. Only where that is
 * beyond doubt: the centre's distance clear of the radius by 2.5% and of a
 * thousandth of the terms' sizes (rounding is a millionth of them); else
 * corner by corner as the program does, so the result is the program's
 * always (NaNs fail the comparison and go that way too). */
extern int g_vu0_stat;
static uint32_t g_box_planes[3];        /* "vu0stat": planes the sphere decided, the box's extent, the corners */

static inline int box_plane8s(float nx, float ny, float nz, float d, const float (*c)[4], const BoxSphere *s)
{
    float a = d - nx * s->c[0] - ny * s->c[1] - nz * s->c[2];
    float nn = nx * nx + ny * ny + nz * nz;
    if (a * a > s->r2 * nn * 1.05f + 1e-6f * (d * d + nn * s->cc)) {
        if (PS2_UNLIKELY(g_vu0_stat))
            g_box_planes[0]++;
        return a < 0.0f ? 2 : 0;
    }
    if (PS2_UNLIKELY(g_vu0_stat))
        g_box_planes[2]++;
    return box_plane8(nx, ny, nz, d, c);
}

/* VU0 program 0x4787D0, entry 0x0A18: is the box with corner vf1 and edges
 * vf6, vf7, vf8 inside the frustum (planes: columns of vf9..vf12, then vf13
 * and vf14 as rows)? VI1 = 0 outside, 2 inside, 1 partly. The eight corners
 * are tested against each plane, as the program does (box_plane8).
 * box_test_planes (entry 0x0A50 below still goes through it) leaves the
 * registers as the program does: corners in vf2..vf8, the row planes in the x
 * and y lanes of vf9..vf12, VI2..VI6, ACC and MAC. */
static void box_test_planes(VU *vu, const BoxSphere *sph);

/* (490 calls a frame in a race. With the lanes in arrays and the sphere from
 * all eight corners it was 1080 cycles a call: a variable a lane, and the
 * sphere from the box's four diagonals, which meet in its middle: e8 +- e7
 * +- e6. The sphere only decides where it is beyond doubt, see box_plane8s,
 * so it need not be the one the corners' mean gives.)
 *
 * What a VU0 routine has to leave behind is what something may read before
 * writing it again. work/vu0live.py works that out over the whole executable
 * (every COP2 instruction and VCALLMS, every microprogram entry, calls and
 * returns followed): after this entry's two calls that is the corners in
 * vf6..vf8, the row planes in the x and y lanes of vf9..vf12, VI1 and VI2.
 * Nothing reads the corners in vf2..vf5, VI3..VI6, ACC or MAC, so they are
 * not made: where the sphere does not decide a plane, the box's extent
 * along the plane's normal does (the middle's distance against half the
 * edges' projections, clear of a hundred-thousandth of the terms' sizes:
 * all inside, all outside, or corners on both sides), and only a box
 * within that of touching a plane has its corners tested one by one. */
typedef struct BoxLazy {
    int have;
    float c[8][4];                      /* the corners as the program has them */
} BoxLazy;

static int __attribute__((noinline)) box_plane_slow(float (*vf)[4], float nx, float ny, float nz, float d, float a,
                                                    BoxLazy *z)
{
    const float *o = vf[1], *e6 = vf[6], *e7 = vf[7], *e8 = vf[8];
    const float ra = nx * e6[0] + ny * e6[1] + nz * e6[2], rb = nx * e7[0] + ny * e7[1] + nz * e7[2];
    const float rc = nx * e8[0] + ny * e8[1] + nz * e8[2];
    const float r = 0.5f * (fabsf(ra) + fabsf(rb) + fabsf(rc));
    const float mg = 1e-5f * (fabsf(d) + fabsf(nx) * (fabsf(o[0]) + fabsf(e6[0]) + fabsf(e7[0]) + fabsf(e8[0])) +
                              fabsf(ny) * (fabsf(o[1]) + fabsf(e6[1]) + fabsf(e7[1]) + fabsf(e8[1])) +
                              fabsf(nz) * (fabsf(o[2]) + fabsf(e6[2]) + fabsf(e7[2]) + fabsf(e8[2])));
    if (fabsf(a) - r > mg) {
        if (PS2_UNLIKELY(g_vu0_stat))
            g_box_planes[1]++;
        return a < 0.0f ? 2 : 0;
    }
    if (r - fabsf(a) > mg) {
        if (PS2_UNLIKELY(g_vu0_stat))
            g_box_planes[1]++;
        return 1;
    }
    if (!z->have) {
        int k;
        for (k = 0; k < 3; k++) {
            const float c1 = o[k], c2 = c1 + e6[k], c4 = c1 + e7[k], c3 = c2 + e7[k];
            z->c[0][k] = c1;
            z->c[1][k] = c2;
            z->c[2][k] = c3;
            z->c[3][k] = c4;
            z->c[4][k] = c1 + e8[k];
            z->c[5][k] = c2 + e8[k];
            z->c[6][k] = c3 + e8[k];
            z->c[7][k] = c4 + e8[k];
        }
        z->have = 1;
    }
    if (PS2_UNLIKELY(g_vu0_stat))
        g_box_planes[2]++;
    return box_plane8(nx, ny, nz, d, (const float (*)[4])z->c);
}

static inline void box_core(float (*vf)[4], uint32_t *vi)
{
    const float ox = vf[1][0], oy = vf[1][1], oz = vf[1][2];
    const float ax = vf[6][0], ay = vf[6][1], az = vf[6][2];
    const float bx = vf[7][0], by = vf[7][1], bz = vf[7][2];
    const float cx = vf[8][0], cy = vf[8][1], cz = vf[8][2];
    const float c2x = ox + ax, c2y = oy + ay, c2z = oz + az;
    const float c4x = ox + bx, c4y = oy + by, c4z = oz + bz;
    const float c3x = c2x + bx, c3y = c2y + by, c3z = c2z + bz;
    const float sx = cx + bx, sy = cy + by, sz = cz + bz, tx = cx - bx, ty = cy - by, tz = cz - bz;
    const float d1x = sx + ax, d1y = sy + ay, d1z = sz + az, d2x = sx - ax, d2y = sy - ay, d2z = sz - az;
    const float d3x = tx - ax, d3y = ty - ay, d3z = tz - az, d4x = tx + ax, d4y = ty + ay, d4z = tz + az;
    const float q1 = d1x * d1x + d1y * d1y + d1z * d1z, q2 = d2x * d2x + d2y * d2y + d2z * d2z;
    const float q3 = d3x * d3x + d3y * d3y + d3z * d3z, q4 = d4x * d4x + d4y * d4y + d4z * d4z;
    const float qa = q1 > q2 ? q1 : q2, qb = q3 > q4 ? q3 : q4;
    const float mx = ox + 0.5f * d1x, my = oy + 0.5f * d1y, mz = oz + 0.5f * d1z;
    const float rr = 1.05f * (0.25f * (qa > qb ? qa : qb));
    const float mm = mx * mx + my * my + mz * mz;
    BoxLazy z;
    int k, r, any = 0, out = 0;
    z.have = 0;
#define BOX_PLANE(r_, nx_, ny_, nz_, d_) do {                                   \
        const float n0_ = (nx_), n1_ = (ny_), n2_ = (nz_), dd_ = (d_);          \
        const float a_ = dd_ - n0_ * mx - n1_ * my - n2_ * mz;                  \
        const float nn_ = n0_ * n0_ + n1_ * n1_ + n2_ * n2_;                    \
        if (a_ * a_ > nn_ * rr + 1e-6f * (dd_ * dd_ + nn_ * mm)) {              \
            (r_) = a_ < 0.0f ? 2 : 0;                                           \
            if (PS2_UNLIKELY(g_vu0_stat))                                       \
                g_box_planes[0]++;                                              \
        } else {                                                                \
            (r_) = box_plane_slow(vf, n0_, n1_, n2_, dd_, a_, &z);              \
        }                                                                       \
    } while (0)
    /* the planes in the columns of vf9..vf12, then (unless one has the whole box outside) the two rows */
    for (k = 0; k < 4; k++) {
        BOX_PLANE(r, vf[9][k], vf[10][k], vf[11][k], vf[12][k]);
        if (r == 2) {
            out = 1;
            break;
        }
        any |= r;
    }
    vi[2] = 0xF0;
    if (!out) {
        vi[2] = 0xC0;
        for (k = 0; k < 2; k++) {
            const float *p = vf[13 + k];
            BOX_PLANE(r, p[0], p[1], p[2], p[3]);
            if (r == 2) {
                out = 1;
                break;
            }
            any |= r;
        }
    }
#undef BOX_PLANE
    vi[1] = out ? 0 : any ? 1 : 2;
    vf[6][0] = c2x + cx;
    vf[6][1] = c2y + cy;
    vf[6][2] = c2z + cz;
    vf[7][0] = c3x + cx;
    vf[7][1] = c3y + cy;
    vf[7][2] = c3z + cz;
    vf[8][0] = c4x + cx;
    vf[8][1] = c4y + cy;
    vf[8][2] = c4z + cz;
    for (k = 0; k < 4; k++) {
        vf[9 + k][0] = vf[13][k];
        vf[9 + k][1] = vf[14][k];
    }
}

static void vu0_box_test(VU *vu)
{
    box_core(vu->vf, vu->vi);
}

/* Entry 0x0A50 of the same: the box is its four lower corners vf1..vf4 and a
 * height (vf1.w, along y); the upper four go to vf5..vf8. 40 calls a frame,
 * 2000 instructions each as recompiled code. */
static void vu0_box_test_y(VU *vu)
{
    float (*vf)[4] = vu->vf;
    const float h = vf[1][3];
    int j, k;
    for (j = 0; j < 4; j++) {
        for (k = 0; k < 3; k++)
            vf[5 + j][k] = 0.0f + vf[1 + j][k];
        vf[5 + j][1] = vf[5 + j][1] + h;
    }
    {
        BoxSphere sph;
        box_sphere((const float (*)[4])&vf[1], &sph);
        box_test_planes(vu, &sph);
    }
}

/* the corners vf1..vf8 (within the sphere) against the planes */
static void box_test_planes(VU *vu, const BoxSphere *sph)
{
    float (*vf)[4] = vu->vf;
    float pl[4][4], acc[4];
    uint32_t all = 0, any = 0, all2 = 0, m;
    int k, r;
    for (k = 0; k < 4; k++) {
        r = box_plane8s(vf[9][k], vf[10][k], vf[11][k], vf[12][k], (const float (*)[4])&vf[1], sph);
        all |= r == 2 ? 0x80u >> k : 0;
        any |= r ? 0x80u >> k : 0;
    }
    for (k = 0; k < 4; k++)
        cp4(pl[k], vf[9 + k]);
    if (all) {
        /* outside after the first four planes */
        m = box_corner((const float (*)[4])pl, vf[8], acc, 0xF0);
        vu->vi[2] = 0xF0;
        vu->vi[4] = all;
        vu->vi[6] = 1;
    } else {
        for (k = 0; k < 2; k++) {
            const float *p = vf[13 + k];
            r = box_plane8s(p[0], p[1], p[2], p[3], (const float (*)[4])&vf[1], sph);
            all2 |= r == 2 ? 0x80u >> k : 0;
            any |= r ? 0x80u >> k : 0;
        }
        for (k = 0; k < 4; k++) {
            pl[k][0] = vf[13][k];
            pl[k][1] = vf[14][k];
        }
        m = box_corner((const float (*)[4])pl, vf[8], acc, 0xC0);
        vu->vi[2] = 0xC0;
        vu->vi[4] = all2;
        vu->vi[6] = 0;
    }
    for (k = 0; k < 4; k++) {
        vf[9 + k][0] = vf[13][k];
        vf[9 + k][1] = vf[14][k];
        vu->acc[k] = acc[k];
    }
    vu->vi[1] = all || all2 ? 0 : any ? 1 : 2;
    vu->vi[3] = m;
    vu->vi[5] = any;
    {
        /* the last flag-setting instruction: addz.y vf11, vf0, vf14z */
        union { float f; uint32_t u; } u;
        u.f = vf[14][2];
        vu->mac = ((u.u & 0x7F800000u) == 0 ? 4u : 0u) | (u.u & 0x80000000u ? 0x40u : 0u) |
                  ((u.u & 0x7FFFFFFFu) >= 0x7F7FFFFFu ? 0x4000u : 0u);
    }
}

/* VU0 program 0x479610, entry 0x0920: VI2 = word x of data qword 71 * VI3
 * (the loop counts VI3 down, adding 71) */
static void vu0_row71(VU *vu)
{
    uint32_t v3 = vu->vi[3] & 0xFFFF, v2 = 0;
    if ((int16_t)v3 > 0) {
        v2 = (71 * v3) & 0xFFFF;
        v3 = 0;
    }
    vu->vi[3] = (v3 - 1) & 0xFFFF;
    vu->vi[2] = vu->mem[(v2 & 255) * 4] & 0xFFFF;
}

/* VU0 program 0x479610, entry 0x00C0: vf1..vf6 to data qwords 229 + 6 * VI1,
 * VI1 + 1 (VI2, VI3 as the loop leaves them) */
static void vu0_store6(VU *vu)
{
    uint32_t v1 = vu->vi[1] & 0xFFFF, v3 = v1, v2 = 229, k;
    if ((int16_t)v3 > 0) {
        v2 = (229 + 6 * v3) & 0xFFFF;
        v3 = 0;
    }
    for (k = 0; k < 6; k++)
        memcpy(&vu->mem[((v2 + k) & 255) * 4], vu->vf[1 + k], 16);
    vu->vi[1] = (v1 + 1) & 0xFFFF;
    vu->vi[2] = v2;
    vu->vi[3] = (v3 - 1) & 0xFFFF;
}

/* MAC flags of an FMAC result in x, y and z (zero, sign, overflow) */
static uint32_t mac_xyz(const float *v)
{
    uint32_t m = 0;
    int k;
    for (k = 0; k < 3; k++) {
        uint32_t u = ubits(v[k]), bit = 8u >> k;
        if ((u & 0x7F800000u) == 0)
            m |= bit;
        if (u & 0x80000000u)
            m |= bit << 4;
        if ((u & 0x7FFFFFFFu) >= 0x7F7FFFFFu)
            m |= bit << 12;
    }
    return m;
}

static inline void vu0_lq(VU *vu, int r, uint32_t qw)
{
    cp4(vu->vf[r], &vu->mem[(qw & 255) * 4]);
}

static inline void vu0_sq(VU *vu, uint32_t qw, int r)
{
    cp4(&vu->mem[(qw & 255) * 4], vu->vf[r]);
}

/* VU0 program 0x479610, entry 0x0000: the corners of the box vf1..vf2 into
 * vf4..vf10 and data qwords 213..220; vf3, vf4 and vf13..vf16 saved at
 * 247..252; the record counts at 0, 71 and 142 cleared; VI1 = 0 */
static void vu0_box_corners(VU *vu)
{
    float (*vf)[4] = vu->vf;
    uint32_t *m = vu->mem;
    int k;
    for (k = 0; k < 3; k++)
        vf[11][k] = vf[2][k] - vf[1][k];
    vf[7][1] = vf[7][2] = 0.0f;
    vu->vi[1] = 0;
    vf[12][0] = vf[12][2] = 0.0f;
    m[0] = m[1] = 0;
    vf[9][0] = vf[9][1] = 0.0f;
    m[71 * 4] = m[71 * 4 + 1] = 0;
    vf[7][0] = 0.0f + vf[11][0];       /* adds of vf0: -0 becomes +0 */
    m[142 * 4] = m[142 * 4 + 1] = 0;
    vf[12][1] = 0.0f + vf[11][1];
    vu0_sq(vu, 247, 3);
    vf[9][2] = 0.0f + vf[11][2];
    vu0_sq(vu, 248, 4);
    for (k = 0; k < 3; k++)
        vu->acc[k] = 0.0f + vf[1][k];
    vu0_sq(vu, 249, 13);
    for (k = 0; k < 3; k++)
        vf[4][k] = vf[1][k] + vf[7][k];
    vu0_sq(vu, 250, 14);
    for (k = 0; k < 3; k++)
        vf[5][k] = vf[1][k] + vf[9][k];
    vu0_sq(vu, 251, 15);
    for (k = 0; k < 3; k++)
        vf[7][k] = vf[1][k] + vf[12][k];
    vu0_sq(vu, 252, 16);
    vu0_sq(vu, 213, 1);
    for (k = 0; k < 3; k++) {
        vf[6][k] = vf[4][k] + vf[9][k];
        vf[8][k] = vf[4][k] + vf[12][k];
        vf[9][k] = vf[5][k] + vf[12][k];
    }
    for (k = 0; k < 3; k++)
        vf[10][k] = vf[6][k] + vf[12][k];
    vu0_sq(vu, 214, 4);
    vu0_sq(vu, 215, 5);
    vu0_sq(vu, 216, 6);
    vu0_sq(vu, 217, 7);
    vu0_sq(vu, 218, 8);
    vu0_sq(vu, 219, 9);
    vu0_sq(vu, 220, 10);
    vu->mac = mac_xyz(vf[10]);
}

/* VU0 program 0x479610, entry 0x0968: VI2 = VI3 = 71 * VI3 (record list of
 * camera VI3), its first record (5 qwords) into vf1..vf5, VI2 past it */
static void vu0_rec_first(VU *vu)
{
    uint32_t v3 = vu->vi[3] & 0xFFFF, v2 = (int16_t)v3 > 0 ? (71 * v3) & 0xFFFF : 0;
    int k;
    vu->vi[3] = v2;
    for (k = 0; k < 5; k++)
        vu0_lq(vu, 1 + k, v2 + 1 + (uint32_t)k);
    vu->vi[2] = (v2 + 5) & 0xFFFF;
}

/* entries 0x0998 / 0x09D8: the next record into vf1..vf5 / vf6..vf10 */
static void vu0_rec_next(VU *vu, int r)
{
    uint32_t v2 = vu->vi[2] & 0xFFFF;
    int k;
    for (k = 0; k < 5; k++)
        vu0_lq(vu, r + k, v2 + 1 + (uint32_t)k);
    vu->vi[2] = (v2 + 5) & 0xFFFF;
}

static void vu0_rec_next1(VU *vu) { vu0_rec_next(vu, 1); }
static void vu0_rec_next6(VU *vu) { vu0_rec_next(vu, 6); }

/* entry 0x0A18: clear data qword VI3 */
static void vu0_rec_clear(VU *vu)
{
    memset(&vu->mem[(vu->vi[3] & 255) * 4], 0, 16);
}

/* VU0 program 0x479610, entry 0x0138: an object's visibility for VI1
 * cameras and its instance record. vf15..vf18 its matrix (with VI4 set,
 * times the one at data 249..252 first); its box corners at data 213..220
 * go through it; VI15: 0 no distance test, 1 out when farther than 248.w,
 * else out when nearer (the distance as the program has it: 1 + dy^2 +
 * dz^2 to data 248). Per camera (planes at data 229 + 6k, tested as in the
 * frustum test; records at data 71k, their count in its x): outside, or the
 * record appended: matrix rows and colour (data 247), made once (VI8 set:
 * normalised rows and a brightness; else a 128.5 scale), the clip flag
 * (partly inside) in vf18.z.
 *
 * Left behind is what something may read before writing it again
 * (work/vu0live.py, see vu0_box_test): the last corner in vf8, the w lanes
 * of vf5..vf7, vf9, vf10.w, the matrix and the record's registers, vf22, Q,
 * VI2, VI7, and the records in data memory. Not the other corners, the
 * planes, VI3..VI6, VI9..VI14, ACC, I or MAC. So two opposite corners go
 * through the matrix, not eight: the box is their middle and the three
 * edges between them (data 213..220 are the corners of a box along the
 * axes: entry 0x0000 makes them; checked here, and anything else goes the
 * old way). A plane is decided by the sphere around that, else by the
 * box's extent along the plane's normal, and only for a box within a
 * hundred-thousandth of the terms' sizes of touching a plane do all eight
 * corners go through the matrix as the program has them. It was 1180
 * cycles a call with the eight corners and every register, 540 calls a
 * frame. */
typedef struct VisLazy {
    const uint32_t *m;                  /* data memory: the corners at 213..220 */
    float m0x, m0y, m0z, m1x, m1y, m1z, m2x, m2y, m2z, tx, ty, tz;
    float ax, ay, az, bx, by, bz, ex, ey, ez;   /* the box's edges through the matrix */
    float esum;                         /* the sizes of the terms a corner is summed from */
    int have;
    float c[8][4];                      /* the corners as the program has them */
} VisLazy;

static int __attribute__((noinline)) vis_plane_slow(float nx, float ny, float nz, float d, float a, VisLazy *z)
{
    const float ra = nx * z->ax + ny * z->ay + nz * z->az, rb = nx * z->bx + ny * z->by + nz * z->bz;
    const float rc = nx * z->ex + ny * z->ey + nz * z->ez;
    const float r = 0.5f * (fabsf(ra) + fabsf(rb) + fabsf(rc));
    const float mg = 1e-5f * (fabsf(d) + (fabsf(nx) + fabsf(ny) + fabsf(nz)) * z->esum);
    if (fabsf(a) - r > mg) {
        if (PS2_UNLIKELY(g_vu0_stat))
            g_box_planes[1]++;
        return a < 0.0f ? 2 : 0;
    }
    if (r - fabsf(a) > mg) {
        if (PS2_UNLIKELY(g_vu0_stat))
            g_box_planes[1]++;
        return 1;
    }
    if (!z->have) {
        int i;
        for (i = 0; i < 8; i++) {
            const uint32_t *w = &z->m[(213 + i) * 4];
            const float x = fbits(w[0]), y = fbits(w[1]), zz = fbits(w[2]);
            float px = z->m0x * x, py = z->m0y * x, pz = z->m0z * x;
            px = px + z->m1x * y;
            py = py + z->m1y * y;
            pz = pz + z->m1z * y;
            px = px + z->m2x * zz;
            py = py + z->m2y * zz;
            pz = pz + z->m2z * zz;
            z->c[i][0] = px + z->tx * 1.0f;
            z->c[i][1] = py + z->ty * 1.0f;
            z->c[i][2] = pz + z->tz * 1.0f;
        }
        z->have = 1;
    }
    if (PS2_UNLIKELY(g_vu0_stat))
        g_box_planes[2]++;
    return box_plane8(nx, ny, nz, d, (const float (*)[4])z->c);
}

static void vu0_vis(VU *vu)
{
    float (*vf)[4] = vu->vf;
    uint32_t *vi = vu->vi, *m = vu->mem;
    uint32_t v2, v7, v9, v10, v11, v12, v13;
    float cx, cy, cz, rr, cc, I, acc[4];
    VisLazy z;
    int k;
    if (vi[4] & 0xFFFF) {
        /* the matrix times the one at data 249..252 (the adds of vf0: -0 becomes +0) */
        const float a0x = 0.0f + vf[15][0], a0y = 0.0f + vf[15][1], a0z = 0.0f + vf[15][2];
        const float a1x = 0.0f + vf[16][0], a1y = 0.0f + vf[16][1], a1z = 0.0f + vf[16][2];
        const float a2x = 0.0f + vf[17][0], a2y = 0.0f + vf[17][1], a2z = 0.0f + vf[17][2];
        const float a3x = 0.0f + vf[18][0], a3y = 0.0f + vf[18][1], a3z = 0.0f + vf[18][2];
        for (k = 0; k < 4; k++) {
            const uint32_t *w = &m[(249 + k) * 4];
            const float bx = fbits(w[0]), by = fbits(w[1]), bz = fbits(w[2]);
            float rx = a0x * bx, ry = a0y * bx, rz = a0z * bx;
            rx = rx + a1x * by;
            ry = ry + a1y * by;
            rz = rz + a1z * by;
            rx = rx + a2x * bz;
            ry = ry + a2y * bz;
            rz = rz + a2z * bz;
            if (k == 3) {
                rx = rx + a3x * 1.0f;
                ry = ry + a3y * 1.0f;
                rz = rz + a3z * 1.0f;
            }
            vf[15 + k][0] = rx;
            vf[15 + k][1] = ry;
            vf[15 + k][2] = rz;
        }
    }
    if (vi[15] & 0xFFFF) {
        /* the distance test: 1 + dy^2 + dz^2 against data 248's w */
        const int far = (vi[15] & 0xFFFF) == 1;
        const uint32_t *w = &m[248 * 4];
        float e1 = fbits(w[1]) - vf[18][1], e2 = fbits(w[2]) - vf[18][2], x14, d;
        e1 = e1 * e1;
        e2 = e2 * e2;
        x14 = (1.0f + e1) + 1.0f * e2;
        d = far ? fbits(w[3]) - x14 : x14 - fbits(w[3]);
        if (ubits(d) & 0x80000000u) {
            /* out: the corners in vf5..vf8 as they were loaded */
            ((uint32_t *)vf[5])[3] = m[217 * 4 + 3];
            ((uint32_t *)vf[6])[3] = m[218 * 4 + 3];
            ((uint32_t *)vf[7])[3] = m[219 * 4 + 3];
            cp4(vf[8], &m[220 * 4]);
            return;
        }
    }
    {
        /* The first and the last corner through the matrix as the program does it (ACC = m0 x, += m1 y,
         * += m2 z; the corner = ACC + vf18): the box is their middle and the edges m0 dx, m1 dy, m2 dz. */
        const float m0x = vf[15][0], m0y = vf[15][1], m0z = vf[15][2], m1x = vf[16][0], m1y = vf[16][1], m1z = vf[16][2];
        const float m2x = vf[17][0], m2y = vf[17][1], m2z = vf[17][2], tx = vf[18][0], ty = vf[18][1], tz = vf[18][2];
        const uint32_t *w1 = &m[213 * 4], *w8 = &m[220 * 4];
        const float x1 = fbits(w1[0]), y1 = fbits(w1[1]), z1 = fbits(w1[2]);
        const float x8 = fbits(w8[0]), y8 = fbits(w8[1]), z8 = fbits(w8[2]);
        const float p1ax = m0x * x1, p1ay = m0y * x1, p1az = m0z * x1, p1bx = m1x * y1, p1by = m1y * y1, p1bz = m1z * y1;
        const float p1cx = m2x * z1, p1cy = m2y * z1, p1cz = m2z * z1;
        const float p8ax = m0x * x8, p8ay = m0y * x8, p8az = m0z * x8, p8bx = m1x * y8, p8by = m1y * y8, p8bz = m1z * y8;
        const float p8cx = m2x * z8, p8cy = m2y * z8, p8cz = m2z * z8;
        const float p1x = ((p1ax + p1bx) + p1cx) + tx * 1.0f, p1y = ((p1ay + p1by) + p1cy) + ty * 1.0f;
        const float p1z = ((p1az + p1bz) + p1cz) + tz * 1.0f;
        const float p8x = ((p8ax + p8bx) + p8cx) + tx * 1.0f, p8y = ((p8ay + p8by) + p8cy) + ty * 1.0f;
        const float p8z = ((p8az + p8bz) + p8cz) + tz * 1.0f;
        const float ax = p8ax - p1ax, ay = p8ay - p1ay, az = p8az - p1az, bx = p8bx - p1bx, by = p8by - p1by;
        const float bz = p8bz - p1bz, ex = p8cx - p1cx, ey = p8cy - p1cy, ez = p8cz - p1cz;
        const float aa = ax * ax + ay * ay + az * az, bb = bx * bx + by * by + bz * bz, ee = ex * ex + ey * ey + ez * ez;
        const float ab = ax * bx + ay * by + az * bz, ae = ax * ex + ay * ey + az * ez, be = bx * ex + by * ey + bz * ez;
        /* the sizes of the terms the corners are summed from: their rounding is a ten-millionth of these, and
         * the tests below keep a hundred times that away from a plane */
        float esum = fabsf(p1ax) + fabsf(p1ay) + fabsf(p1az) + fabsf(p1bx) + fabsf(p1by) + fabsf(p1bz) + fabsf(p1cx) +
                     fabsf(p1cy) + fabsf(p1cz) + fabsf(p8ax) + fabsf(p8ay) + fabsf(p8az) + fabsf(p8bx) + fabsf(p8by) +
                     fabsf(p8bz) + fabsf(p8cx) + fabsf(p8cy) + fabsf(p8cz) + fabsf(tx) + fabsf(ty) + fabsf(tz);
        /* the six corners between them: each coordinate the first corner's or the last's */
        const uint32_t odd = (m[214 * 4] ^ w8[0]) | (m[214 * 4 + 1] ^ w1[1]) | (m[214 * 4 + 2] ^ w1[2]) |
                             (m[215 * 4] ^ w1[0]) | (m[215 * 4 + 1] ^ w1[1]) | (m[215 * 4 + 2] ^ w8[2]) |
                             (m[216 * 4] ^ w8[0]) | (m[216 * 4 + 1] ^ w1[1]) | (m[216 * 4 + 2] ^ w8[2]) |
                             (m[217 * 4] ^ w1[0]) | (m[217 * 4 + 1] ^ w8[1]) | (m[217 * 4 + 2] ^ w1[2]) |
                             (m[218 * 4] ^ w8[0]) | (m[218 * 4 + 1] ^ w8[1]) | (m[218 * 4 + 2] ^ w1[2]) |
                             (m[219 * 4] ^ w1[0]) | (m[219 * 4 + 1] ^ w8[1]) | (m[219 * 4 + 2] ^ w8[2]);
        cx = (p1x + p8x) * 0.5f;
        cy = (p1y + p8y) * 0.5f;
        cz = (p1z + p8z) * 0.5f;
        cc = cx * cx + cy * cy + cz * cz;
        /* the sphere: half the longest diagonal at most (the edges' squares and twice their products'
         * sizes), 2.5% more, and the corners' rounding */
        rr = 1.05f * (0.25f * (aa + bb + ee + 2.0f * (fabsf(ab) + fabsf(ae) + fabsf(be)))) + 1e-8f * (esum * esum);
        if (PS2_UNLIKELY(odd)) {
            /* not such a box: every plane corner by corner */
            rr = INFINITY;
            esum = INFINITY;
        }
        z.m = m;
        z.m0x = m0x;
        z.m0y = m0y;
        z.m0z = m0z;
        z.m1x = m1x;
        z.m1y = m1y;
        z.m1z = m1z;
        z.m2x = m2x;
        z.m2y = m2y;
        z.m2z = m2z;
        z.tx = tx;
        z.ty = ty;
        z.tz = tz;
        z.ax = ax;
        z.ay = ay;
        z.az = az;
        z.bx = bx;
        z.by = by;
        z.bz = bz;
        z.ex = ex;
        z.ey = ey;
        z.ez = ez;
        z.esum = esum;
        z.have = 0;
        /* left behind: the last corner, and the w lanes of the three before it */
        vf[8][0] = p8x;
        vf[8][1] = p8y;
        vf[8][2] = p8z;
        ((uint32_t *)vf[8])[3] = w8[3];
        ((uint32_t *)vf[5])[3] = m[217 * 4 + 3];
        ((uint32_t *)vf[6])[3] = m[218 * 4 + 3];
        ((uint32_t *)vf[7])[3] = m[219 * 4 + 3];
    }
#define VIS_PLANE(r_, nx_, ny_, nz_, d_) do {                                   \
        const float n0_ = (nx_), n1_ = (ny_), n2_ = (nz_), dd_ = (d_);          \
        const float a_ = dd_ - n0_ * cx - n1_ * cy - n2_ * cz;                  \
        const float nn_ = n0_ * n0_ + n1_ * n1_ + n2_ * n2_;                    \
        if (a_ * a_ > nn_ * rr + 1e-6f * (dd_ * dd_ + nn_ * cc)) {              \
            (r_) = a_ < 0.0f ? 2 : 0;                                           \
            if (PS2_UNLIKELY(g_vu0_stat))                                       \
                g_box_planes[0]++;                                              \
        } else {                                                                \
            (r_) = vis_plane_slow(n0_, n1_, n2_, dd_, a_, &z);                  \
        }                                                                       \
    } while (0)
    v9 = 0;
    v2 = 229;
    v10 = 0;
    v13 = (vi[1] - 1) & 0xFFFF;
    for (;;) {
        /* a camera: four planes in the columns of its first four qwords, two in the rows of the others; out
         * when a plane has every corner outside, the clip flag when one has some */
        const uint32_t loop13 = v13;
        const uint32_t *p0 = &m[(v2 & 255) * 4], *p1 = &m[((v2 + 1) & 255) * 4], *p2 = &m[((v2 + 2) & 255) * 4];
        const uint32_t *p3 = &m[((v2 + 3) & 255) * 4], *p4 = &m[((v2 + 4) & 255) * 4], *p5 = &m[((v2 + 5) & 255) * 4];
        int out = 0, some = 0, r;
        for (k = 0; k < 4; k++) {
            VIS_PLANE(r, fbits(p0[k]), fbits(p1[k]), fbits(p2[k]), fbits(p3[k]));
            if (r == 2) {
                out = 1;
                break;
            }
            some |= r;
        }
        if (!out) {
            VIS_PLANE(r, 0.0f + fbits(p4[0]), 0.0f + fbits(p4[1]), 0.0f + fbits(p4[2]), 0.0f + fbits(p4[3]));
            if (r == 2) {
                out = 1;
            } else {
                some |= r;
                VIS_PLANE(r, 0.0f + fbits(p5[0]), 0.0f + fbits(p5[1]), 0.0f + fbits(p5[2]), 0.0f + fbits(p5[3]));
                if (r == 2)
                    out = 1;
                else
                    some |= r;
            }
        }
        /* the planes' registers where they are read again: vf9 (its x and y the rows' first lanes) and vf10.w */
        vf[9][0] = 0.0f + fbits(p4[0]);
        vf[9][1] = 0.0f + fbits(p5[0]);
        ((uint32_t *)vf[9])[2] = p0[2];
        ((uint32_t *)vf[9])[3] = p0[3];
        ((uint32_t *)vf[10])[3] = p1[3];
        v11 = m[(v10 & 255) * 4] & 0xFFFF;
        v7 = 42;
        if (!out) {
            v7 = some ? 1 : 0;
            v12 = (v10 + v11) & 0xFFFF;
            if (!v9) {
                I = 128.5f;
                if (!(vi[8] & 0xFFFF)) {
                    vf[19][0] = vf[19][1] = vf[19][2] = 0.0f + I;
                    vf[19][3] = fbits(m[247 * 4 + 3]);
                    vf[19][0] = vf[19][0] * vf[15][3];
                    vf[19][1] = vf[19][1] * vf[16][3];
                    vf[19][2] = vf[19][2] * vf[17][3];
                    v9 = 1;
                    for (k = 0; k < 4; k++)
                        vf[15 + k][3] = 0.0f + vf[15 + k][2];
                    for (k = 0; k < 4; k++)
                        vf[19][k] = fbits((uint32_t)(int32_t)vf[19][k]);
                } else {
                    float q;
                    vu0_lq(vu, 19, 247);
                    for (k = 0; k < 3; k++) {
                        vf[9][k] = vf[15][k] * vf[15][k];
                        vf[10][k] = vf[16][k] * vf[16][k];
                        vf[11][k] = vf[17][k] * vf[17][k];
                    }
                    I = fbits(0x3C000000u);             /* 0.0078125 */
                    v9 = 1;
                    vf[19][3] = vf[19][3] * I;
                    vf[9][0] = vf[9][0] + vf[9][1];
                    vf[10][0] = vf[10][0] + vf[10][1];
                    vf[11][0] = vf[11][0] + vf[11][1];
                    I = 65536.0f;
                    vf[19][3] = vf[19][3] + I;
                    vf[9][0] = vf[9][0] + vf[9][2];
                    vf[10][0] = vf[10][0] + vf[10][2];
                    vf[11][0] = vf[11][0] + vf[11][2];
                    I = fbits(0x3EAAAAABu);             /* 0.333333 */
                    acc[3] = vf[15][3] * I;
                    acc[3] = acc[3] + vf[16][3] * I;
                    q = vu_rsqrt(1.0f, vf[9][0]);
                    vf[22][3] = acc[3] + vf[17][3] * I;
                    vf[15][3] = 0.0f + vf[15][2];
                    vf[16][3] = 0.0f + vf[16][2];
                    vf[17][3] = 0.0f + vf[17][2];
                    vf[22][0] = 0.0f + vf[22][3];
                    I = 0.5f;
                    vf[22][3] = vf[22][3] * I;
                    vf[18][3] = 0.0f + vf[18][2];
                    I = 65536.0f;
                    vf[22][0] = vf[22][0] + I;
                    for (k = 0; k < 3; k++)
                        vf[9][k] = vf[15][k] * q;
                    q = vu_rsqrt(1.0f, vf[10][0]);
                    vf[12][0] = 0.0f + vf[9][0];
                    vf[13][0] = 0.0f + vf[9][1];
                    vf[14][0] = 0.0f + vf[9][2];
                    for (k = 0; k < 3; k++)
                        vf[10][k] = vf[16][k] * q;
                    q = vu_rsqrt(1.0f, vf[11][0]);
                    vf[12][1] = 0.0f + vf[10][0];
                    vf[13][1] = 0.0f + vf[10][1];
                    vf[14][1] = 0.0f + vf[10][2];
                    for (k = 0; k < 3; k++)
                        vf[11][k] = vf[17][k] * q;
                    *vu->q = q;
                    vf[15][2] = 0.0f + vf[22][0];
                    vf[16][2] = 0.0f + vf[22][3];
                    vf[12][2] = 0.0f + vf[11][0];
                    vf[13][2] = 0.0f + vf[11][1];
                    vf[14][2] = 0.0f + vf[11][2];
                    for (k = 0; k < 3; k++) {
                        float a = vf[19][k] * vf[12][k];
                        a = a + vf[19][k] * vf[13][k];
                        acc[k] = a;
                        vf[19][k] = a + vf[19][k] * vf[14][k];
                    }
                }
            }
            vf[18][2] = fbits((uint32_t)(int32_t)(int16_t)v7);
            for (k = 0; k < 5; k++)
                vu0_sq(vu, v12 + 1 + (uint32_t)k, 15 + k);
            v11 = (v11 + 5) & 0xFFFF;
            m[(v10 & 255) * 4] = v11;
        }
        v10 = (v10 + 71) & 0xFFFF;
        v2 = (v2 + 6) & 0xFFFF;
        v13 = (v13 - 1) & 0xFFFF;
        if ((int16_t)loop13 <= 0)
            break;
    }
#undef VIS_PLANE
    vi[2] = v2;
    vi[7] = v7;
    (void)acc;
    (void)I;
}

int g_vu0_coll = 1;             /* autopilot "vu0coll": the collision tests decided natively (0: the routines) */

/* a vertex lane as the collision entries decode it: itof0, then (acc = 0 + offset) + v * scale, each rounded */
static inline float vu0_dec16(float bits, float o, float w)
{
    const float t = (float)(int32_t)F2BITS(bits);
    const float acc = 0.0f + o;
    const float m = t * w;
    return acc + m;
}

/* VU0 program 0x4787D0, entries 0x0388, 0x0390, 0x0550, 0x03D8 and 0x03E0: the collision code's segment against a
 * triangle (f_00273400, f_00273BF0 and f_00274C90; about 900 calls a frame at the race start, 1% to 24% of them
 * hits). The segment starts at vf2 and runs along vf4 (0x0388 and 0x03D8 make that from its end: vf4 - vf2); the
 * triangle is vf5..vf7 (0x0550: vf8..vf10), 16-bit integers through vf23 (0x03D8 and 0x03E0: floats). With e1, e2
 * the edges from the first vertex, s the start from it, p = d x e2 and q = s x e1: det = e1.p (det - 1e-8 <= 0: no
 * hit, the triangle faces away), u = s.p, v = d.q, t = e2.q; a hit when u, v, u + v and t are all within -1e-5 det
 * and 1.00001 det (strictly). Then vf24 (0x0550: vf8) = t, u, v over det, and 0x0550 puts the point (vf2 + 0) +
 * vf4 t/det in vf9. Q = 1/det; VI8 the MAC flags of the first test, or those the six bounds OR together.
 * Left behind is what the lean routine leaves (gen/vu0_4787D0.c: what something may read before writing it,
 * work/vu0live.py), each value by the program's operations in its order (fp-contract off; the FPU rounds as the
 * EE). Not the edges, products and sums in vf1, vf3, vf11..vf18, vf20..vf22 and vf25 (0x0550: vf10), the decoded
 * first vertex (but in 0x0550's vf8 when the triangle faces away), VI3, VI9, I, ACC or MAC. (A native that left
 * every register as the program does was no faster than the recompiled code; this one does a third of the stores.) */
static inline void seg_tri_in(VU *vu, float px, float py, float pz, float dx, float dy, float dz, float ax, float ay,
                              float az, float bx, float by, float bz, float cx, float cy, float cz, int vi, float *res,
                              float *pt)
{
    const Vu0Seg r = vu0i_seg(px, py, pz, dx, dy, dz, ax, ay, az, bx, by, bz, cx, cy, cz, pt != NULL);
    vu->vf[19][0] = r.v;
    *vu->q = r.q;
    vu->vi[vi] = r.hit;
    if (r.ok) {
        res[0] = r.r[0];
        res[1] = r.r[1];
        res[2] = r.r[2];
        if (pt) {
            pt[0] = r.p[0];
            pt[1] = r.p[1];
            pt[2] = r.p[2];
        }
    }
    vu->vi[8] = r.m;
}

/* (the segment from vf2 along vf4, as the entries have it) */
static inline void seg_tri(VU *vu, float ax, float ay, float az, float bx, float by, float bz, float cx, float cy,
                           float cz, int vi, float *res, float *pt)
{
    float (*vf)[4] = vu->vf;
    seg_tri_in(vu, vf[2][0], vf[2][1], vf[2][2], vf[4][0], vf[4][1], vf[4][2], ax, ay, az, bx, by, bz, cx, cy, cz, vi,
               res, pt);
}

/* 0x0390: the triangle in vf5..vf7 as integers; vf6 and vf7 left decoded */
static void vu0_seg_0390(VU *vu)
{
    float (*vf)[4] = vu->vf;
    const float ox = vf[23][0], oy = vf[23][1], oz = vf[23][2], w = vf[23][3];
    const float ax = vu0_dec16(vf[5][0], ox, w), ay = vu0_dec16(vf[5][1], oy, w), az = vu0_dec16(vf[5][2], oz, w);
    const float bx = vu0_dec16(vf[6][0], ox, w), by = vu0_dec16(vf[6][1], oy, w), bz = vu0_dec16(vf[6][2], oz, w);
    const float cx = vu0_dec16(vf[7][0], ox, w), cy = vu0_dec16(vf[7][1], oy, w), cz = vu0_dec16(vf[7][2], oz, w);
    vf[6][0] = bx;
    vf[6][1] = by;
    vf[6][2] = bz;
    vf[7][0] = cx;
    vf[7][1] = cy;
    vf[7][2] = cz;
    seg_tri(vu, ax, ay, az, bx, by, bz, cx, cy, cz, 1, vf[24], NULL);
}

/* 0x0388: the segment's vector from its end first */
static void vu0_seg_0388(VU *vu)
{
    float (*vf)[4] = vu->vf;
    vf[4][0] = vf[4][0] - vf[2][0];
    vf[4][1] = vf[4][1] - vf[2][1];
    vf[4][2] = vf[4][2] - vf[2][2];
    vu0_seg_0390(vu);
}

/* 0x0550: the triangle in vf8..vf10 as integers; the result in VI2 and vf8, the point in vf9 */
static void vu0_seg_0550(VU *vu)
{
    float (*vf)[4] = vu->vf;
    const float ox = vf[23][0], oy = vf[23][1], oz = vf[23][2], w = vf[23][3];
    const float ax = vu0_dec16(vf[8][0], ox, w), ay = vu0_dec16(vf[8][1], oy, w), az = vu0_dec16(vf[8][2], oz, w);
    const float bx = vu0_dec16(vf[9][0], ox, w), by = vu0_dec16(vf[9][1], oy, w), bz = vu0_dec16(vf[9][2], oz, w);
    const float cx = vu0_dec16(vf[10][0], ox, w), cy = vu0_dec16(vf[10][1], oy, w), cz = vu0_dec16(vf[10][2], oz, w);
    vf[8][0] = ax;
    vf[8][1] = ay;
    vf[8][2] = az;
    vf[9][0] = bx;
    vf[9][1] = by;
    vf[9][2] = bz;
    seg_tri(vu, ax, ay, az, bx, by, bz, cx, cy, cz, 2, vf[8], vf[9]);
}

/* 0x03E0: the triangle in vf5..vf7 as floats */
static void vu0_seg_03E0(VU *vu)
{
    float (*vf)[4] = vu->vf;
    seg_tri(vu, vf[5][0], vf[5][1], vf[5][2], vf[6][0], vf[6][1], vf[6][2], vf[7][0], vf[7][1], vf[7][2], 1, vf[24],
            NULL);
}

/* 0x03D8: the segment's vector from its end first */
static void vu0_seg_03D8(VU *vu)
{
    float (*vf)[4] = vu->vf;
    vf[4][0] = vf[4][0] - vf[2][0];
    vf[4][1] = vf[4][1] - vf[2][1];
    vf[4][2] = vf[4][2] - vf[2][2];
    vu0_seg_03E0(vu);
}

/* VU0 entries done natively: where the program stops, and how many first
 * calls also run the recompiled code to compare (then logged) */
typedef void (*Vu0Native)(VU *vu);
/* dead, dead_xyz, dead_vi: the registers (a bit each; of dead_xyz the w lane is kept) the native does not leave as
 * the program does, because nothing reads them (work/vu0live.py); then neither ACC nor MAC. coll: switched by
 * g_vu0_coll. */
#define VR(n_) (1u << (n_))
#define SEG_DEAD (VR(1) | VR(3) | VR(11) | VR(12) | VR(13) | VR(14) | VR(15) | VR(16) | VR(17) | VR(18) | VR(20) | \
                  VR(21) | VR(22))
static const struct { uint32_t chain, pc, tpc; Vu0Native fn; uint32_t dead, dead_xyz, dead_vi; int coll; } k_vu0_native[] = {
    {0x004787D0u, 0x0A18, 0x0A18, vu0_box_test, VR(2) | VR(3) | VR(4) | VR(5), 0, VR(3) | VR(4) | VR(5) | VR(6)},
    {0x00479610u, 0x0920, 0x0968, vu0_row71},
    {0x00479610u, 0x00C0, 0x0138, vu0_store6},
    {0x00479610u, 0x0000, 0x00C0, vu0_box_corners},
    {0x00479610u, 0x0968, 0x09D8, vu0_rec_first},
    {0x00479610u, 0x0998, 0x09D8, vu0_rec_next1},
    {0x00479610u, 0x09D8, 0x0A18, vu0_rec_next6},
    {0x00479610u, 0x0A18, 0x0A38, vu0_rec_clear},
    {0x00479610u, 0x0138, 0x0920, vu0_vis, VR(1) | VR(2) | VR(3) | VR(4) | VR(11) | VR(12) | VR(13) | VR(14) | VR(20),
     VR(5) | VR(6) | VR(7) | VR(10),
     VR(3) | VR(4) | VR(5) | VR(6) | VR(9) | VR(10) | VR(11) | VR(12) | VR(13) | VR(14)},
    {0x004787D0u, 0x0A50, 0x0A18, vu0_box_test_y},
    {0x004787D0u, 0x0388, 0x0550, vu0_seg_0388, 0, SEG_DEAD | VR(5) | VR(25), VR(3) | VR(9), 1},
    {0x004787D0u, 0x0390, 0x0550, vu0_seg_0390, 0, SEG_DEAD | VR(5) | VR(25), VR(3) | VR(9), 1},
    {0x004787D0u, 0x0550, 0x0708, vu0_seg_0550, 0, SEG_DEAD | VR(10), VR(3) | VR(9), 1},
    {0x004787D0u, 0x03D8, 0x0550, vu0_seg_03D8, 0, SEG_DEAD | VR(25), VR(3) | VR(9), 1},
    {0x004787D0u, 0x03E0, 0x0550, vu0_seg_03E0, 0, SEG_DEAD | VR(25), VR(3) | VR(9), 1},
};
#define VU0_CHECK 100

/* VU0 runs by entry, without the search: program (or native code) per entry
 * slot, valid while micro memory is unchanged */
static struct {
    uint32_t gen;
    const VuProg *prog;
    Vu0Native fn;
    uint16_t tpc, checked;
    uint8_t nat;                /* its row of k_vu0_native */
} g_vu0_fast[512];
int g_vu0_poison;               /* debugging ("vu0poison 1"): what a native does not leave behind is overwritten */

/* debugging: the registers a native leaves as they were get values nothing can use unnoticed (a NaN, an
 * address out of every table): whatever read them after all shows */
static void __attribute__((noinline)) vu0_poison(VU *vu, unsigned nat)
{
    const uint32_t dead = k_vu0_native[nat].dead, xyz = k_vu0_native[nat].dead_xyz, dvi = k_vu0_native[nat].dead_vi;
    int r, k;
    if (!(dead | xyz | dvi))
        return;
    for (r = 1; r < 32; r++)
        for (k = 0; k < 4; k++)
            if ((dead & VR(r)) || (k < 3 && (xyz & VR(r))))
                vu->vf[r][k] = fbits(0x7FC0DEADu);
    for (r = 1; r < 16; r++)
        if (dvi & VR(r))
            vu->vi[r] = 0xDEAD;
    for (k = 0; k < 4; k++)
        vu->acc[k] = fbits(0x7FC0DEADu);
}
uint32_t g_vu0_runs;           /* VU0 calls so far (the frame history's column) */
int g_vu0_chk_all;              /* debugging ("vu0chk 1"): every native VU0 call checked, not the first 100 */
static uint32_t g_vu0_chk_n, g_vu0_chk_bad, g_vu0_chk_out;  /* status line: calls checked, differing, in VI/data */

static void __attribute__((noinline)) vu0_native_check(VU *vu, const VuProg *p, uint32_t pc, Vu0Native fn, int n,
                                                       uint32_t tpc, unsigned nat)
{
    static float vf0[32][4], vf1[32][4];
    static uint32_t m0[1024], m1[1024];
    static int bad;
    uint32_t vi0[16], vi1[16], mac1;
    float acc0[4], acc1[4], q0 = *vu->q, q1;
    memcpy(vf0, vu->vf, sizeof vf0);
    memcpy(vi0, vu->vi, sizeof vi0);
    memcpy(acc0, vu->acc, sizeof acc0);
    memcpy(m0, vu->mem, sizeof m0);
    {
        /* against the full routine: the lean one leaves what it holds dead as it was, which a native may write
         * (that was every run's "34 differences" at the first box tests) */
        const int lean = g_vu0_lean;
        g_vu0_lean = 0;
        rec_run(vu, p, pc);
        g_vu0_lean = lean;
    }
    memcpy(vf1, vu->vf, sizeof vf1);
    memcpy(vi1, vu->vi, sizeof vi1);
    memcpy(acc1, vu->acc, sizeof acc1);
    memcpy(m1, vu->mem, sizeof m1);
    mac1 = vu->mac;
    q1 = *vu->q;
    if (vu->tpc != tpc)
        LOG("VU0 %06X@%04X: the recompiled code stops at %04X, the native table says %04X", p->chain, pc, vu->tpc, tpc);
    memcpy(vu->vf, vf0, sizeof vf0);
    memcpy(vu->vi, vi0, sizeof vi0);
    memcpy(vu->acc, acc0, sizeof acc0);
    memcpy(vu->mem, m0, sizeof m0);
    *vu->q = q0;
    fn(vu);
    g_vu0_chk_n++;
    if (memcmp(&q1, vu->q, 4)) {
        static int bad_q;
        if (bad_q++ < 20)
            LOG("VU0 %06X@%04X native leaves Q %08X, recompiled %08X", p->chain, pc, F2BITS(*vu->q), F2BITS(q1));
    }
    if (k_vu0_native[nat].dead | k_vu0_native[nat].dead_xyz | k_vu0_native[nat].dead_vi) {
        /* what the native does not leave behind is not compared */
        int r;
        for (r = 1; r < 32; r++) {
            if (k_vu0_native[nat].dead & VR(r))
                memcpy(vf1[r], vu->vf[r], 16);
            else if (k_vu0_native[nat].dead_xyz & VR(r))
                memcpy(vf1[r], vu->vf[r], 12);
        }
        for (r = 1; r < 16; r++)
            if (k_vu0_native[nat].dead_vi & VR(r))
                vi1[r] = vu->vi[r];
        memcpy(acc1, vu->acc, sizeof acc1);
        mac1 = vu->mac;
    }
    {
        /* what the game reads back: VI registers and data memory (the
         * visibility results); VF and ACC are mostly scratch */
        static int bad_out;
        if (memcmp(vi1, vu->vi, sizeof vi1) || memcmp(m1, vu->mem, sizeof m1)) {
            g_vu0_chk_out++;
            if (bad_out++ < 20) {
                int r;
                LOG("VU0 %06X@%04X native differs in VI/data", p->chain, pc);
                for (r = 0; r < 16; r++)
                    if (vi1[r] != vu->vi[r])
                        LOG("  vi%d %08X, recompiled %08X", r, vu->vi[r], vi1[r]);
                for (r = 0; r < 1024; r++)
                    if (m1[r] != vu->mem[r]) {
                        LOG("  data word %d %08X, recompiled %08X", r, vu->mem[r], m1[r]);
                        break;
                    }
            }
        }
    }
    if ((memcmp(vf1, vu->vf, sizeof vf1) || memcmp(vi1, vu->vi, sizeof vi1) || memcmp(acc1, vu->acc, sizeof acc1) ||
         memcmp(m1, vu->mem, sizeof m1) || (mac1 & 0xFFFF) != (vu->mac & 0xFFFF)) && (g_vu0_chk_bad++, bad++ < 20)) {
        int r, k;
        LOG("VU0 %06X@%04X native differs: mac %04X (recompiled %04X)", p->chain, pc, vu->mac, mac1);
        for (r = 0; r < 16; r++)
            if (vi1[r] != vu->vi[r])
                LOG("  vi%d %08X, recompiled %08X", r, vu->vi[r], vi1[r]);
        for (r = 0; r < 32; r++)
            for (k = 0; k < 4; k++)
                if (memcmp(&vf1[r][k], &vu->vf[r][k], 4))
                    LOG("  vf%d[%d] %g, recompiled %g", r, k, (double)vu->vf[r][k], (double)vf1[r][k]);
        for (k = 0; k < 4; k++)
            if (memcmp(&acc1[k], &vu->acc[k], 4))
                LOG("  acc[%d] %g, recompiled %g", k, (double)vu->acc[k], (double)acc1[k]);
        for (r = 0; r < 1024; r++)
            if (m1[r] != vu->mem[r]) {
                LOG("  data word %d %08X, recompiled %08X", r, vu->mem[r], m1[r]);
                break;
            }
    }
    if (n == VU0_CHECK)
        LOG("VU0 %06X@%04X native: %d calls checked (%d differences so far)", p->chain, pc, n, bad);
}

/* VU1 subroutines a backend may do natively (config VU_NATIVE_SUBS):
 * without one (the PC host), the microcode runs. Not a weak symbol: the PC
 * host's linker does not resolve those. */
#ifndef GEKKO
int vu1_sky_strip(VuCtx *ctx)
{
    (void)ctx;
    return 0;
}
#endif

static uint32_t g_micro_writes[2];     /* status line: micro memory changes */


/* status line: VU0 calls by program and entry */
static struct { uint32_t chain, pc, n; uint64_t ticks; } g_vu0_calls_by[24];
/* ... and with "vu0stat 2" by the place the game called from (host address after the call) */
static struct { uint32_t chain, pc, site, n; uint64_t ticks; } g_vu0_sites[96];
static uintptr_t g_vu0_site;
int g_vu0_stat;                 /* "vu0stat": count calls by entry (status line); 2: and by call site */

void vu0_calls_report(uint32_t frames)
{
    char line[640];
    size_t m = 0;
    int k;
    line[0] = 0;
    /* (a tick of the time base is 12 cycles of the CPU) */
    for (k = 0; k < 24 && g_vu0_calls_by[k].n && m < sizeof line - 48; k++)
        m += (size_t)snprintf(line + m, sizeof line - m, " %06X@%04X %u (%u cycles)", g_vu0_calls_by[k].chain,
                              g_vu0_calls_by[k].pc, g_vu0_calls_by[k].n / (frames ? frames : 1),
                              (unsigned)(g_vu0_calls_by[k].ticks * 12 / g_vu0_calls_by[k].n));
    memset(g_vu0_calls_by, 0, sizeof g_vu0_calls_by);
    if (m)
        LOG("vu0 calls per frame, and what one costs:%s", line);
    if (g_vu0_stat > 1) {
        /* "vu0stat 2": the 14 call sites that cost the most cycles a frame (the host address after the call:
         * powerpc-eabi-nm names the guest function it is in) */
        int j, n;
        for (n = 0; n < 14; n++) {
            int best = -1;
            for (k = 0; k < 96; k++)
                if (g_vu0_sites[k].n && (best < 0 || g_vu0_sites[k].ticks > g_vu0_sites[best].ticks))
                    best = k;
            if (best < 0)
                break;
            j = best;
            LOG("vu0 site %08X: %06X@%04X %u calls a frame, %u cycles each, %u cycles a frame", g_vu0_sites[j].site,
                g_vu0_sites[j].chain, g_vu0_sites[j].pc, g_vu0_sites[j].n / (frames ? frames : 1),
                (unsigned)(g_vu0_sites[j].ticks * 12 / g_vu0_sites[j].n),
                (unsigned)(g_vu0_sites[j].ticks * 12 / (frames ? frames : 1)));
            g_vu0_sites[j].n = 0;
        }
        memset(g_vu0_sites, 0, sizeof g_vu0_sites);
    }
    if (g_box_planes[0] | g_box_planes[1] | g_box_planes[2])
        LOG("vu0: box planes per frame: %u decided by the sphere, %u by the box's extent along the normal, %u corner "
            "by corner", g_box_planes[0] / (frames ? frames : 1), g_box_planes[1] / (frames ? frames : 1),
            g_box_planes[2] / (frames ? frames : 1));
    g_box_planes[0] = g_box_planes[1] = g_box_planes[2] = 0;
    if (g_coll_stat[0])
        LOG("vu0: collision sphere tests per frame %u, %u decided as misses (%u checked against the routine, %u wrong)",
            g_coll_stat[0] / (frames ? frames : 1), g_coll_stat[1] / (frames ? frames : 1), g_coll_stat[2],
            g_coll_stat[3]);
    g_coll_stat[0] = g_coll_stat[1] = 0;
    if (g_vu0_time && g_vu0t_n)
        LOG("vu0: recompiled calls per frame %u, %u cycles each in rec_run, %u of them in the routine", g_vu0t_n /
            (frames ? frames : 1), (uint32_t)(g_vu0t_whole * 12 / g_vu0t_n), (uint32_t)(g_vu0t_body * 12 / g_vu0t_n));
    if (g_vu0_time)
        LOG("vu0: collision entries per frame (calls/hits): 0000 %u/%u 0028 %u/%u 0388 %u/%u 0390 %u/%u 0550 %u/%u 03D8 %u/%u other %u/%u",
            g_vu0t_coll[0][0] / (frames ? frames : 1), g_vu0t_coll[0][1] / (frames ? frames : 1),
            g_vu0t_coll[1][0] / (frames ? frames : 1), g_vu0t_coll[1][1] / (frames ? frames : 1),
            g_vu0t_coll[2][0] / (frames ? frames : 1), g_vu0t_coll[2][1] / (frames ? frames : 1),
            g_vu0t_coll[3][0] / (frames ? frames : 1), g_vu0t_coll[3][1] / (frames ? frames : 1),
            g_vu0t_coll[4][0] / (frames ? frames : 1), g_vu0t_coll[4][1] / (frames ? frames : 1),
            g_vu0t_coll[5][0] / (frames ? frames : 1), g_vu0t_coll[5][1] / (frames ? frames : 1),
            g_vu0t_coll[6][0] / (frames ? frames : 1), g_vu0t_coll[6][1] / (frames ? frames : 1));
    memset(g_vu0t_coll, 0, sizeof g_vu0t_coll);
    g_vu0t_whole = g_vu0t_body = 0;
    g_vu0t_n = 0;
    LOG("vu: micro memory changed per frame: VU0 %u, VU1 %u; VU0 programs kept across the changes %u", g_micro_writes[0] /
        (frames ? frames : 1), g_micro_writes[1] / (frames ? frames : 1), g_rec_kept[0] / (frames ? frames : 1));
    g_rec_kept[0] = 0;
    if (g_vu0_chk_n)
        LOG("vu: native VU0 calls checked against the recompiled code: %u, %u differ (%u in VI or data)", g_vu0_chk_n,
            g_vu0_chk_bad, g_vu0_chk_out);
    if (g_lean_chk || g_lean_bad)
        LOG("vu: lean VU0 routines: %u runs a frame, %u checked against the full ones, %u differ so far",
            g_lean_runs / (frames ? frames : 1), g_lean_chk, g_lean_bad);
    g_lean_runs = g_lean_chk = 0;
    g_vu0_chk_n = g_vu0_chk_bad = g_vu0_chk_out = 0;
    g_micro_writes[0] = g_micro_writes[1] = 0;
}

static void __attribute__((noinline)) vu0_count(const VuProg *p, uint32_t pc, uint64_t ticks)
{
    int k;
    if (g_vu0_stat > 1)
        for (k = 0; k < 96; k++)
            if (!g_vu0_sites[k].n || (g_vu0_sites[k].chain == p->chain && g_vu0_sites[k].pc == pc &&
                                      g_vu0_sites[k].site == (uint32_t)g_vu0_site)) {
                g_vu0_sites[k].chain = p->chain;
                g_vu0_sites[k].pc = pc;
                g_vu0_sites[k].site = (uint32_t)g_vu0_site;
                g_vu0_sites[k].n++;
                g_vu0_sites[k].ticks += ticks;
                break;
            }
    for (k = 0; k < 24; k++)
        if (!g_vu0_calls_by[k].n || (g_vu0_calls_by[k].chain == p->chain && g_vu0_calls_by[k].pc == pc)) {
            g_vu0_calls_by[k].chain = p->chain;
            g_vu0_calls_by[k].pc = pc;
            g_vu0_calls_by[k].n++;
            g_vu0_calls_by[k].ticks += ticks;
            return;
        }
}

/* a VU0 run from pc by its g_vu0_fast slot; 0 when the slot is not known */
static inline int vu0_fast(VU *vu, uint32_t pc)
{
    typeof(g_vu0_fast[0]) *f = &g_vu0_fast[(pc >> 3) & 511];
    if (f->gen != g_micro_gen[0] || !f->prog || g_vu_mode != 1)
        return 0;
    g_vu0_runs++;
    if (PS2_UNLIKELY(g_vu0_stat)) {
        /* counted and timed ("vu0stat 1"; no checking then) */
        const uint64_t t0 = plat_ticks();
        if (!f->fn) {
            rec_run(vu, f->prog, pc);
        } else {
            flags_drain(vu);
            f->fn(vu);
            vu->tpc = f->tpc;
        }
        vu0_count(f->prog, pc, plat_ticks() - t0);
        return 1;
    }
    if (!f->fn) {
        rec_run(vu, f->prog, pc);
        return 1;
    }
    flags_drain(vu);
    if (PS2_UNLIKELY(f->checked < VU0_CHECK || g_vu0_chk_all))
        vu0_native_check(vu, f->prog, pc, f->fn, f->checked < VU0_CHECK ? ++f->checked : VU0_CHECK + 1, f->tpc, f->nat);
    else
        f->fn(vu);
    if (PS2_UNLIKELY(g_vu0_poison))
        vu0_poison(vu, f->nat);
    vu->tpc = f->tpc;
    return 1;
}

static void run2(VU *vu, uint32_t pc)
{
    const VuProg *p;
    if (g_vu_mode && (p = rec_find(vu, pc)) != NULL) {
        if (vu->unit == 0) {
            typeof(g_vu0_fast[0]) *f = &g_vu0_fast[(pc >> 3) & 511];
            unsigned k;
            f->gen = g_micro_gen[0];
            f->prog = p;
            f->fn = NULL;
            for (k = 0; k < sizeof k_vu0_native / sizeof k_vu0_native[0]; k++)
                if (k_vu0_native[k].chain == p->chain && k_vu0_native[k].pc == pc &&
                    (g_vu0_coll || !k_vu0_native[k].coll)) {
                    f->fn = k_vu0_native[k].fn;
                    f->tpc = (uint16_t)k_vu0_native[k].tpc;
                    f->nat = (uint8_t)k;
                }
            if (vu0_fast(vu, pc))
                return;
        }
        if (vu->unit == 1) {
            extern uint32_t g_vu1_prog;
            g_vu1_prog = pc;
        }
        if (g_vu_mode == 2)
            rec_check(vu, p, pc);
        else
            rec_run(vu, p, pc);
        return;
    }
    interp(vu, pc);
}

static void vu1_setup(void)
{
    VU *vu = &g_vu[1];
    if (vu->vf)
        return;
    vu->unit = 1;
    vu->vf = g_vu1_vf;
    vu->vi = g_vu1_vi;
    vu->acc = g_vu1_acc;
    vu->q = &g_vu1_q;
    vu->p = &g_vu1_p;
    vu->ireg = &g_vu1_i;
    vu->r = &g_vu1_r;
    vu->mem = g_vu_mem + 0xC000 / 4;
    vu->mem_qw = 1024;
    vu->micro = g_vu_mem + 0x8000 / 4;
    vu->micro_bytes = 0x4000;
    g_vu1_vf[0][3] = 1.0f;
}

static void vu0_bind(R5900 *c)
{
    VU *vu = &g_vu[0];
    vu->unit = 0;
    vu->vf = c->vf;
    vu->vi = c->vi;
    vu->acc = c->vacc;
    vu->q = &c->vq;
    vu->p = &c->vp;
    vu->ireg = &c->vI;
    vu->r = &c->vr;
    vu->mem = g_vu_mem + 0x4000 / 4;
    vu->mem_qw = 256;
    vu->micro = g_vu_mem;
    vu->micro_bytes = 0x1000;
}

/* VIF1 MSCAL/MSCNT (addr = ~0: continue) and VIF0 */
/* VU1 time by microcode chain (status line) */
#define VU1_PROF 32
static struct { uint32_t code, runs; uint64_t ticks; } g_vu1_prof[VU1_PROF];

static void vu1_prof_add(uint32_t code, uint64_t ticks)
{
    int k;
    for (k = 0; k < VU1_PROF; k++)
        if (g_vu1_prof[k].code == code || !g_vu1_prof[k].runs) {
            g_vu1_prof[k].code = code;
            g_vu1_prof[k].runs++;
            g_vu1_prof[k].ticks += ticks;
            return;
        }
}

void (*g_vu1_run_hook)(uint32_t addr);    /* debugging: told of every VU1 start */

uint32_t vu_vi(int unit, int k)
{
    return g_vu[unit].vi[k] & 0xFFFF;
}

const float *vu_vf(int unit, int k)
{
    return g_vu[unit].vf[k];
}

void vu_run(int unit, uint32_t addr)
{
    VU *vu = &g_vu[unit];
    if (unit && g_vu1_run_hook)
        g_vu1_run_hook(addr == 0xFFFFFFFFu ? vu->tpc : addr);
    if (unit == 0) {
        R5900 *c = kernel_current_ctx();
        if (!c)
            return;
        vu0_bind(c);
    } else {
        vu1_setup();
    }
    {
        static unsigned logged[2];
        if (g_trace > 1 || logged[unit] < 4) {
            logged[unit]++;
            LOG("VU%d start at %04X", unit, addr == 0xFFFFFFFFu ? vu->tpc : addr);
        }
    }
    {
        int prev = pt_enter(unit ? PT_VU1 : PT_VU0);
        uint64_t t0 = g_pt[PT_VU1];
        run(vu, addr == 0xFFFFFFFFu ? vu->tpc : addr);
        pt_leave(prev);
        if (unit)
            vu1_prof_add(g_vu1_code, g_pt[PT_VU1] - t0);
    }
}

void vu1_prof_report(char *out, size_t size)
{
    uint64_t total = 0;
    size_t n = 0;
    int k, j;
    for (k = 0; k < VU1_PROF; k++)
        total += g_vu1_prof[k].ticks;
    out[0] = 0;
    for (j = 0; j < 6; j++) {
        int best = -1;
        for (k = 0; k < VU1_PROF; k++)
            if (g_vu1_prof[k].ticks && (best < 0 || g_vu1_prof[k].ticks > g_vu1_prof[best].ticks))
                best = k;
        if (best < 0)
            break;
        n += (size_t)snprintf(out + n, size > n ? size - n : 0, "%06X %.0f%% (%u runs) ", g_vu1_prof[best].code,
                              100.0 * (double)g_vu1_prof[best].ticks / (double)(total ? total : 1), g_vu1_prof[best].runs);
        g_vu1_prof[best].ticks = 0;
    }
    memset(g_vu1_prof, 0, sizeof g_vu1_prof);
}

/* micro memory changed: the recompiled program for each entry is looked up again */
void vu_micro_written(int unit)
{
    g_micro_gen[unit & 1]++;
    g_micro_writes[unit & 1]++;
    g_micro_all[unit & 1] = g_micro_gen[unit & 1];
}

void vu_micro_written_at(int unit, uint32_t off, uint32_t n)
{
    const uint32_t size = unit & 1 ? 0x4000 : 0x1000;
    uint32_t b, b1;
    unit &= 1;
    g_micro_gen[unit]++;
    g_micro_writes[unit]++;
    if (!n)
        return;
    if (n >= size) {
        g_micro_all[unit] = g_micro_gen[unit];
        return;
    }
    off &= size - 1;
    b = off / 64;
    b1 = (off + n - 1) / 64;
    for (; b <= b1; b++)
        g_micro_blk[unit][b % (size / 64)] = g_micro_gen[unit];
}

/* ---- COP2 interface (VU0 micro calls and control registers) ---------------------------------- */

static uint32_t g_cmsar0, g_fbrst;

int g_vu0_disabled;          /* debug: skip VU0 micro calls */
static unsigned g_vu0_calls[512];

/* ---- the collision code's sphere tests, decided without the routine where it says miss ----------------------
 *
 * 0x273070 walks a patch's triangle strips and calls VU0 4787D0 for each triangle: entry 0x0028 for a strip's first
 * (three vertices as 16-bit integers), 0x0000 for each next one (the new vertex; the two before as floats). The
 * routine (about 100 instructions, 700 to 780 cycles a call; 6% of the calls hit) decodes the vertices (itof0, then
 * (0 + vf1) + v * vf1.w); with a, b, d the vertices less the sphere's centre vf2 and vf3 the squared radius by lane,
 * it decides in this order:
 *   - the plane's distance squared at least vf3.x: miss (this one needs the normal, a square root and a division);
 *   - a vertex within its lane's radius (|a|^2 - vf3.x <= 0, ...): hit;
 *   - the centre in the nearest vertex's region (the vertex's two edge projections both <= 0): miss;
 *   - per edge, its line's distance (|a|^2|b|^2 - (a.b)^2 against |a-b|^2 r^2) and its side (against the opposite
 *     vertex): an edge that is both beyond the radius and on the outer side is a miss, else a hit.
 * Each "<= 0" is a MAC flag (sign, or exponent 0) and the program ANDs flag words, so sign meets sign, zero zero.
 * coll_sphere_miss does the last three with the program's operations in its order (fp-contract off; the FPU rounds
 * as the EE does), so where it says miss the routine says miss whatever the plane test gives; else the routine runs.
 * Every path of the routine writes the same registers (vu0live: "always writes"), so after a call the state depends
 * on its inputs alone.
 * After each call 0x273070 reads VI1 and stores vf8, vf4, vf5, vf6 in a record on its stack (the next strip step
 * reads vf5 and vf6 back from there; vf8, the normal, only the callback a hit calls reads), and it writes no COP2
 * register between its last call and its return. Its two VCALLMS call hle_coll_sphere_call (config VCALLMS_SITES:
 * the recompiled code keeps the guest's registers in C locals, so the stack pointer cannot tell a native which
 * function called; a hit's callback may call these entries from elsewhere, where more of what they leave is read).
 * While 0x273070 runs through hle_coll_sphere, a call decided as a miss gets VI1 0 and the decoded vertices without
 * the routine; its inputs are kept, and when 0x273070 returns after such a call the routine is run on them, which
 * leaves every register (and the record's vf8) as they were. The first COLL_CHECK decided calls, and one in
 * 256 after, also run the routine, and what it says against the decision is counted ("collision sphere tests ...
 * wrong"); a disagreement turns the deciding off for the rest of the run. */
static int g_coll_on;                   /* 0x273070 runs through hle_coll_sphere, deciding (not nested in a callback) */
static int g_coll_off;
static uint32_t g_coll_n;
static struct { int on; uint32_t entry; float in[7][4]; } g_coll_def;
#define COLL_CHECK 20000

/* a lane's MAC flags as vu_mac1 sets them: 2 sign, 1 zero (exponent 0) */
static inline uint32_t coll_fl(float x)
{
    const uint32_t u = F2BITS(x);
    return (u >> 30 & 2) | ((u & 0x7F800000u) == 0);
}

/* 1: the routine says miss (v the decoded vertices, c2 the centre vf2, r2 vf3); 0: it may say hit */
static int coll_sphere_miss(const float v[3][3], const float *c2, const float *r2)
{
    /* vf10, vf11, vf12 */
    const float ax = v[0][0] - c2[0], ay = v[0][1] - c2[1], az = v[0][2] - c2[2];
    const float bx = v[1][0] - c2[0], by = v[1][1] - c2[1], bz = v[1][2] - c2[2];
    const float dx = v[2][0] - c2[0], dy = v[2][1] - c2[1], dz = v[2][2] - c2[2];
    /* vf13 (squares) and vf14 (dot products), each sum in the program's order */
    const float aa = (ax * ax + ay * ay) + az * az;
    const float bb = (by * by + bx * bx) + bz * bz;
    const float dd = (dz * dz + dx * dx) + dy * dy;
    const float ab = (ax * bx + ay * by) + az * bz;
    const float bd = (by * dy + bx * dx) + bz * dz;
    const float da = (dz * az + dx * ax) + dy * ay;
    /* vf22 = vf13 - vf14 and vf23 = vf15 - vf14 (vf15 = bb, dd, 0 + aa); VI6 = flags(vf22) and VI7 =
     * flags(vf13 - vf17) with vf17 = da, 0 + ab, 0 + bd are the same values: e1 and (e2z, e2x, e2y) */
    const float e1x = aa - ab, e1y = bb - bd, e1z = dd - da;
    const float e2x = bb - ab, e2y = dd - bd, e2z = aa - da;
    float ab0, bd0, px, py, pz, qx, qy, qz, sx, sy, sz;
    uint32_t reg;
    /* VI4: a vertex inside the sphere (a hit unless the plane test says miss) */
    if (coll_fl(aa - r2[0]) | coll_fl(bb - r2[1]) | coll_fl(dd - r2[2]))
        return 0;
    /* VI5 (vf13 - vf15) picks the nearest vertex, VI6 & VI7 its region */
    if (coll_fl(aa - bb))
        reg = coll_fl(dd - aa) ? coll_fl(e1z) & coll_fl(e2y) : coll_fl(e1x) & coll_fl(e2z);
    else if (coll_fl(bb - dd))
        reg = coll_fl(e1y) & coll_fl(e2x);
    else
        reg = coll_fl(e1z) & coll_fl(e2y);
    if (reg)
        return 1;
    /* 0x0328: vf24 = vf13 * vf15 - vf14 * vf14, vf25 = vf16 * vf22 + vf17 * vf23 (vf16 = bd, da, 0 + ab),
     * vf20 = vf22 * vf3 + vf23 * vf3; a lane with flags(vf20 - vf24) & flags(vf24 - vf25) is a miss */
    ab0 = 0.0f + ab;
    bd0 = 0.0f + bd;
    px = aa * bb - ab * ab;
    py = bb * dd - bd * bd;
    pz = dd * aa - da * da;
    qx = bd * e1x + da * e2x;
    qy = da * e1y + ab0 * e2y;
    qz = ab0 * e1z + bd0 * e2z;
    sx = e1x * r2[0] + e2x * r2[0];
    sy = e1y * r2[1] + e2y * r2[1];
    sz = e1z * r2[2] + e2z * r2[2];
    return ((coll_fl(sx - px) & coll_fl(px - qx)) | (coll_fl(sy - py) & coll_fl(py - qy)) |
            (coll_fl(sz - pz) & coll_fl(pz - qz))) != 0;
}

/* 0: run the routine; 1: a miss, decided; 2: decided, and to be checked: run the routine, then coll_check */
static int coll_sphere_try(R5900 *c, uint32_t addr, float out[3][3])
{
    VU *vu = &g_vu[0];
    const typeof(g_vu0_fast[0]) *f = &g_vu0_fast[(addr >> 3) & 511];
    const float *v1 = c->vf[1];
    int k;
    if (vu->vf != c->vf || f->gen != g_micro_gen[0] || !f->prog || f->prog->chain != 0x004787D0u || f->fn ||
        (vu->q_pending | vu->p_pending | vu->fq_n) || g_vu_mode != 1 ||
        ((g_trace > 1) | g_vu0_disabled | g_vu0_stat | g_vu0_chk_all | g_vu0_poison | g_coll_off))
        return 0;
    g_coll_stat[0]++;
    for (k = 0; k < 3; k++) {
        if (addr == 0x0028) {
            out[0][k] = vu0_dec16(c->vf[4][k], v1[k], v1[3]);
            out[1][k] = vu0_dec16(c->vf[5][k], v1[k], v1[3]);
        } else {
            out[0][k] = c->vf[4][k];
            out[1][k] = c->vf[5][k];
        }
        out[2][k] = vu0_dec16(c->vf[6][k], v1[k], v1[3]);
    }
    if (!coll_sphere_miss((const float (*)[3])out, c->vf[2], c->vf[3]))
        return 0;
    g_coll_n++;
    return g_coll_n <= COLL_CHECK || !(g_coll_n & 255) ? 2 : 1;
}

static void coll_check(R5900 *c, float want[3][3])
{
    int k, bad = (c->vi[1] & 0xFFFF) != 0;
    for (k = 0; k < 3; k++)
        bad |= F2BITS(c->vf[4][k]) != F2BITS(want[0][k]) || F2BITS(c->vf[5][k]) != F2BITS(want[1][k]) ||
               F2BITS(c->vf[6][k]) != F2BITS(want[2][k]);
    g_coll_stat[2]++;
    if (bad) {
        g_coll_off = 1;
        if (g_coll_stat[3]++ < 8)
            LOG("collision sphere test decided wrong (deciding now off): VI1 %u, vertex %08X/%08X against %08X/%08X",
                c->vi[1] & 0xFFFF, F2BITS(c->vf[6][0]), F2BITS(c->vf[6][1]), F2BITS(want[2][0]),
                F2BITS(want[2][1]));
    }
    if (g_coll_stat[2] == COLL_CHECK)
        LOG("collision sphere tests decided as misses: %u checked against the routine, %u wrong", COLL_CHECK,
            g_coll_stat[3]);
}

/* 0x273070's two VCALLMS (config VCALLMS_SITES): entry 0x0028 or 0x0000 */
void hle_coll_sphere_call(R5900 *c, uint32_t addr)
{
    float v[3][3];
    int r, k;
    if (!g_coll_on) {
        ps2_vu0_callms(c, addr);
        return;
    }
    r = coll_sphere_try(c, addr, v);
    if (r == 1) {
        /* what the routine leaves that 0x273070 reads: VI1 0, the decoded vertices; the inputs for later */
        g_coll_def.on = 1;
        g_coll_def.entry = addr;
        memcpy(g_coll_def.in, c->vf[0], sizeof g_coll_def.in);
        for (k = 0; k < 3; k++) {
            c->vf[4][k] = v[0][k];
            c->vf[5][k] = v[1][k];
            c->vf[6][k] = v[2][k];
        }
        c->vi[1] = 0;
        g_coll_stat[1]++;
        return;
    }
    g_coll_def.on = 0;                  /* the routine runs: what it leaves stands */
    ps2_vu0_callms(c, addr);
    if (r == 2) {
        g_coll_stat[1]++;
        coll_check(c, v);
    }
}

void f_00273070(R5900 *c);

/* 0x273070 (config OVERRIDES): its sphere tests through coll_sphere_try; the last one run if it was decided */
#ifdef WTIME
static void hle_coll_sphere_inner(R5900 *c);
void hle_coll_sphere(R5900 *c)
{
    WT_MARK(wm);
    hle_coll_sphere_inner(c);
    WT_DONE(coll070, wm);
}
static void hle_coll_sphere_inner(R5900 *c)
#else
void hle_coll_sphere(R5900 *c)
#endif
{
    const uint32_t sp = c->r[29][0];
    const int outer = g_coll_on;
    int k;
    if (!g_vu0_coll || g_coll_off || outer) {
        /* (switched off, or from a hit's callback: the routine runs for every call of this one) */
        g_coll_on = 0;
        f_00273070(c);
        g_coll_on = outer;
        return;
    }
    g_coll_on = 1;
    g_coll_def.on = 0;
    f_00273070(c);
    g_coll_on = 0;
    if (g_coll_def.on) {
        g_coll_def.on = 0;
        memcpy(c->vf[1], g_coll_def.in[1], 6 * sizeof c->vf[0]);
        ps2_vu0_callms(c, g_coll_def.entry);
        /* the record's first qword: the last call's vf8 (0x2732DC: sp - 528 + 368 + 16) */
        for (k = 0; k < 4; k++)
            SWF(sp - 528u + 384u + 4u * (uint32_t)k, c->vf[8][k]);
    }
}

/* ---- 0x274C90 natively (config OVERRIDES and OVERRIDE_VU0; autopilot "collc90": 0 the recompiled function, 1 the
 * native with its first COLL90_CHECK calls checked, 2 every call checked, 3 the registers it does not leave poisoned). The collision code's segment against one
 * triangle: a0 points to the segment's start and end (two qwords), a1..a3 to the triangle's vertices (qwords), t0..t2
 * to the three floats of the answer. The recompiled function copies the vertices through its stack (sq/lq), moves the
 * five qwords into VU0's registers (qmtc2), calls the entry 0x03D8 and reads vf24 and VI1 back; the entry is native
 * already (vu0_seg_03D8) and what is left is this traffic through c->vf, the dispatch and the frame: 416 cycles a
 * call, 199 a frame at the race start. The native keeps the segment and the vertices in locals, runs seg_tri_in
 * (the same operations in the same order) and writes of VU0's state what work/vu0live.py says something reads after
 * the call (`sites`, f_00274C90 @03D8): vf5.w, vf6, vf7 (the vertices as the function loaded them), vf19.x, vf24, VI1,
 * VI8 and Q. Not the segment in vf2 and vf4, nor vf5.xyz and the entry's dead registers (SEG_DEAD, vf25, VI3, VI9);
 * the generator is told the call may change any (OVERRIDE_VU0). It leaves v0 = 1 and the answer in the three floats
 * on a hit, else v0 = 0, v1 = VI1 and f0 as the function leaves them, lanes 2 and 3 of v0 and v1 too.
 * When VU0 does not hold the collision program (the function loads it first, by DMA) or a debugging switch is on,
 * the recompiled function runs. */
int g_coll_c90 = 1;
void f_00274C90(R5900 *c);
#define COLL90_CHECK 5000
static uint32_t g_c90_checked, g_c90_bad, g_c90_hits, g_c90_deadmask[2];

/* returns 0, with nothing touched, when it does not handle the call */
static int coll_c90_native(R5900 *c)
{
    VU *vu = &g_vu[0];
    const typeof(g_vu0_fast[0]) *f = &g_vu0_fast[(0x03D8 >> 3) & 511];
    const uint32_t *s, *e, *a, *b, *d;
    const uint32_t gp = c->r[28][0];
    float *vf24 = c->vf[24];
    uint32_t hit;
    if (PS2_UNLIKELY(LW(gp - 22808u) != 0x004787D0u || vu->vf != c->vf || !vu->mem || g_vu_mode != 1 ||
                     f->gen != g_micro_gen[0] || !f->prog || f->prog->chain != 0x004787D0u || !f->fn ||
                     f->checked < VU0_CHECK ||
                     ((g_trace > 1) | g_vu0_disabled | g_vu0_stat | g_vu0_chk_all | g_vu0_poison)))
        return 0;
    g_vu0_runs++;
    vu->cycle += 8;
    if (vu->q_pending | vu->p_pending | vu->fq_n) {
        q_sync(vu);
        p_sync(vu);
        flags_sync(vu);
    }
    s = (const uint32_t *)GPTR(c->r[4][0] & ~15u);
    e = (const uint32_t *)GPTR((c->r[4][0] + 16u) & ~15u);
    a = (const uint32_t *)GPTR(c->r[5][0] & ~15u);
    b = (const uint32_t *)GPTR(c->r[6][0] & ~15u);
    d = (const uint32_t *)GPTR(c->r[7][0] & ~15u);
#define W(p_, k_) fbits((p_)[k_])
    {
        const float px = W(s, 0), py = W(s, 1), pz = W(s, 2);
        seg_tri_in(vu, px, py, pz, W(e, 0) - px, W(e, 1) - py, W(e, 2) - pz, W(a, 0), W(a, 1), W(a, 2), W(b, 0), W(b, 1),
                   W(b, 2), W(d, 0), W(d, 1), W(d, 2), 1, vf24, NULL);
    }
    c->vf[5][3] = W(a, 3);
    c->vf[6][0] = W(b, 0);
    c->vf[6][1] = W(b, 1);
    c->vf[6][2] = W(b, 2);
    c->vf[6][3] = W(b, 3);
    c->vf[7][0] = W(d, 0);
    c->vf[7][1] = W(d, 1);
    c->vf[7][2] = W(d, 2);
    c->vf[7][3] = W(d, 3);
#undef W
    vu->tpc = f->tpc;
    hit = c->vi[1];
    c->r[3][0] = hit;
    c->r[3][1] = SX(hit);
    c->r[3][2] = b[2];
    c->r[3][3] = b[3];
    c->r[2][0] = hit ? 1u : 0u;
    c->r[2][1] = 0;
    c->r[2][2] = F2BITS(vf24[2]);
    c->r[2][3] = F2BITS(vf24[3]);
    if (hit) {
        SWF(c->r[8][0], vf24[0]);
        SWF(c->r[9][0], vf24[1]);
        SWF(c->r[10][0], vf24[2]);
        c->f[0] = vf24[2];
    }
    if (PS2_UNLIKELY(g_coll_c90 == 3)) {
        /* debugging: what the native does not leave (the segment, vf5.xyz) gets values nothing can use unnoticed */
        int k;
        for (k = 0; k < 4; k++)
            c->vf[2][k] = c->vf[4][k] = fbits(0x7FC0DEADu);
        for (k = 0; k < 3; k++)
            c->vf[5][k] = fbits(0x7FC0DEADu);
    }
    return 1;
}

/* the native, then the recompiled function from the same state: what the callers read must be equal (the VU0 state
 * that differs although vu0live says nothing reads it is only counted) */
static void __attribute__((noinline)) coll_c90_checked(R5900 *c)
{
    static R5900 before, after;
    static VU vu0;
    uint32_t w0[3], w1[3], diff = 0, k, r;
    const uint32_t t[3] = {c->r[8][0], c->r[9][0], c->r[10][0]};
    VU *vu = &g_vu[0];
    memcpy(&before, c, sizeof before);
    memcpy(&vu0, vu, sizeof vu0);
    for (k = 0; k < 3; k++)
        w0[k] = LW(t[k]);
    if (!coll_c90_native(c)) {
        f_00274C90(c);
        return;
    }
    memcpy(&after, c, sizeof after);
    for (k = 0; k < 3; k++)
        w1[k] = LW(t[k]);
    memcpy(c, &before, sizeof before);
    memcpy(vu, &vu0, sizeof vu0);
    for (k = 3; k-- > 0;)
        SW(t[k], w0[k]);
    f_00274C90(c);
    g_c90_checked++;
    g_c90_hits += c->vi[1] != 0;
    for (k = 0; k < 4; k++)
        diff |= (after.r[2][k] != c->r[2][k]) << 0 | (after.r[3][k] != c->r[3][k]) << 1;
    for (r = 16; r < 32; r++)
        if (r < 24 || r >= 28)
            for (k = 0; k < 4; k++)
                diff |= (after.r[r][k] != c->r[r][k]) << 2;
    if (F2BITS(after.f[0]) != F2BITS(c->f[0]))
        diff |= 1 << 3;
    for (k = 0; k < 3; k++)
        if (w1[k] != LW(t[k]))
            diff |= 1 << 4;
    /* VU0: what is read after (vf5.w, vf6, vf7, vf19, vf24, vf8, vf9, vf10.w, vf22.w, vf23.w, VI1, VI2, VI7, VI8, VI15, Q) */
    if (F2BITS(after.vf[5][3]) != F2BITS(c->vf[5][3]) || memcmp(after.vf[6], c->vf[6], 2 * sizeof c->vf[6]) ||
        memcmp(after.vf[19], c->vf[19], sizeof c->vf[19]) || memcmp(after.vf[24], c->vf[24], sizeof c->vf[24]) ||
        memcmp(after.vf[8], c->vf[8], 2 * sizeof c->vf[8]) || F2BITS(after.vf[10][3]) != F2BITS(c->vf[10][3]) ||
        F2BITS(after.vf[22][3]) != F2BITS(c->vf[22][3]) || F2BITS(after.vf[23][3]) != F2BITS(c->vf[23][3]))
        diff |= 1 << 5;
    if (after.vi[1] != c->vi[1] || after.vi[2] != c->vi[2] || after.vi[7] != c->vi[7] || after.vi[8] != c->vi[8] ||
        after.vi[15] != c->vi[15] || F2BITS(after.vq) != F2BITS(c->vq))
        diff |= 1 << 6;
    for (r = 0; r < 32; r++)
        if (memcmp(after.vf[r], c->vf[r], sizeof c->vf[r]))
            g_c90_deadmask[0] |= 1u << r;
    for (r = 0; r < 16; r++)
        if (after.vi[r] != c->vi[r])
            g_c90_deadmask[1] |= 1u << r;
    if (diff && g_c90_bad++ < 20)
        LOG("collision 0x274C90 native differs (%X): hit %u/%u v0 %08X/%08X v1 %08X/%08X out %08X/%08X vi8 %X/%X", diff,
            after.vi[1], c->vi[1], after.r[2][0], c->r[2][0], after.r[3][0], c->r[3][0], w1[0], LW(t[0]), after.vi[8],
            c->vi[8]);
    if (g_c90_checked == 200 || g_c90_checked == COLL90_CHECK || g_c90_checked % 50000 == 0)
        LOG("collision 0x274C90 native: %u calls checked (%u differ so far), %u hits; VU0 state the native leaves "
            "otherwise: vf %08X, VI %04X", g_c90_checked, g_c90_bad, g_c90_hits, g_c90_deadmask[0], g_c90_deadmask[1]);
}

void hle_coll_c90(R5900 *c)
{
    if (!g_coll_c90 || !g_vu0_coll) {
        f_00274C90(c);
    } else if (PS2_UNLIKELY(g_coll_c90 == 2 || g_c90_checked < COLL90_CHECK)) {
        coll_c90_checked(c);
    } else if (!coll_c90_native(c)) {
        f_00274C90(c);
    }
}

/* ---- 0x273BF0 and 0x273400: the collision code's segment against a strip of triangles. 0x273BF0 keeps the nearest hit
 * of the strip's triangles (entries 0x0388, 0x0550 and 0x0390 of the VU0 program, ~10 a call, one in four calls hits);
 * 0x273400 calls a function of the caller's for each hit. Both run as their inline variants, n_00273BF0 and
 * n_00273400: the recompiled function again, with the VCALLMS sites run inline on locals (config VCALLMS_INLINE,
 * runtime/vu0_inline.h) instead of through c->vf and the dispatcher. Autopilot "collbf0" and "coll400": 0 the
 * recompiled function, 1 the inline variant with its first COLL_PAIR_CHECK calls checked, 2 every call checked;
 * "vu0inl 0" runs the variants with their sites taking the old way (the A/B in one binary).
 *
 * The check runs the variant, takes what it left, puts the machine back (the registers, VU0, the function's frame
 * on the guest stack, the result structure) and runs the recompiled function; then everything must be equal: all
 * GPRs, the FPU registers, the frame, the result, and the VU0 state something reads after (work/vu0live.py: the
 * entries' live registers); the VU0 registers that differ otherwise are collected and logged (what the inline
 * entries do not leave). 0x273400's callback is called once: the variant calls it through a stub that records
 * what it was given and what it returned, and the recompiled function's call is answered from that record. */
void f_00273BF0(R5900 *c);
void f_00273400(R5900 *c);
void n_00273BF0(R5900 *c);
void n_00273400(R5900 *c);

int g_vu0_inl = 1;                      /* autopilot "vu0inl": VCALLMS_INLINE sites taken inline (0: the old way) */
int g_coll_bf0 = 1, g_coll_400 = 1;

/* whether the call at a VCALLMS_INLINE site may run as the entry's native on the locals, and the bookkeeping that
 * ps2_vu0_callms does before a native entry */
static uint32_t g_inl_n[4];             /* taken, refused for what is in flight, refused otherwise, calls */

int vu0_inl_enter(R5900 *c, uint32_t entry)
{
    VU *vu = &g_vu[0];
    const typeof(g_vu0_fast[0]) *f = &g_vu0_fast[(entry >> 3) & 511];
    g_inl_n[3]++;
    if (!(g_inl_n[3] & 0xFFFF))
        LOG("VU0 inline entries: %u calls, %u taken, %u refused for state in flight, %u refused otherwise", g_inl_n[3],
            g_inl_n[0], g_inl_n[1], g_inl_n[2]);
    if (PS2_UNLIKELY(!g_vu0_inl || vu->vf != c->vf || !vu->mem || g_vu_mode != 1 || f->gen != g_micro_gen[0] || !f->prog ||
                     f->prog->chain != 0x004787D0u || !f->fn || f->checked < VU0_CHECK ||
                     ((g_trace > 1) | g_vu0_disabled | g_vu0_stat | g_vu0_chk_all | g_vu0_poison))) {
        g_inl_n[2]++;
        return 0;
    }
    if (vu->q_pending | vu->p_pending | vu->fq_n) {
        g_inl_n[1]++;
        return 0;
    }
    g_inl_n[0]++;
    g_vu0_runs++;
    vu->cycle += 8;
    vu->tpc = f->tpc;
    return 1;
}

int g_coll_chk = 3000;                  /* autopilot "collchk": how many calls of each function run checked (mode 1) */
int g_coll_time;                        /* autopilot "colltime": clock reads around each call, a line every 5000 */
int g_coll_mut;                         /* autopilot "collmut" N: the segment of the next N checked 0x273400 calls is stretched
                                         * (debugging: the game goes on from what the stretched call did) */
static struct { uint32_t calls, with_cb, cbs, bad; } g_mut;
#define COLL_PAIR_CHECK g_coll_chk
typedef struct CollCk {
    uint32_t checked, bad, hits, callbacks;
    uint32_t dead_vf, dead_vi;          /* VU0 registers that differ and nothing reads */
} CollCk;
static CollCk g_ck[2];
static int g_ck_depth;

/* 0x273400's callback, in the check: (the callback's own address stays in a2 for the function to read from its
 * frame; the stub stands in for it) */
#define CB_STUB 0x7FFF0F00u
static struct { int mode; uint32_t real, n, i; struct { uint32_t a0, a1, v0[2]; uint8_t rec[128]; } e[16]; int bad; } g_cb;

static void coll_cb_stub(R5900 *c)
{
    const uint32_t a0 = c->r[4][0], a1 = c->r[5][0];
    if (g_cb.mode == 1) {                       /* the variant's pass: the real callback, recorded */
        if (g_cb.n < 16) {
            g_cb.e[g_cb.n].a0 = a0;
            g_cb.e[g_cb.n].a1 = a1;
            memcpy(g_cb.e[g_cb.n].rec, GPTR(a0), sizeof g_cb.e[0].rec);
        }
        ps2_call(c, g_cb.real);
        if (g_cb.n < 16) {
            g_cb.e[g_cb.n].v0[0] = c->r[2][0];
            g_cb.e[g_cb.n].v0[1] = c->r[2][1];
        }
        g_cb.n++;
    } else {                                    /* the recompiled function's pass: answered from the record */
        if (g_cb.i >= g_cb.n || g_cb.i >= 16 || g_cb.e[g_cb.i].a0 != a0 || g_cb.e[g_cb.i].a1 != a1 ||
            memcmp(g_cb.e[g_cb.i].rec, GPTR(a0), sizeof g_cb.e[0].rec))
            g_cb.bad++;
        if (g_cb.i < 16 && g_cb.i < g_cb.n) {
            c->r[2][0] = g_cb.e[g_cb.i].v0[0];
            c->r[2][1] = g_cb.e[g_cb.i].v0[1];
        } else {
            c->r[2][0] = c->r[2][1] = 0;
        }
        g_cb.i++;
    }
}

typedef void (*CollFn)(R5900 *);
void rt_set_stub(uint32_t addr, PS2Func fn);

/* kind 0: 0x273BF0 (frame 1552, the result structure at a2, 112 bytes), kind 1: 0x273400 (frame 1280, a2 the callback) */
static void __attribute__((noinline)) coll_pair_check(R5900 *c, CollFn nat, CollFn ref, int kind)
{
    static R5900 before, after;
    static VU vu0, vu1;
    static uint8_t fr0[1552], fr1[1552], rs0[112], rs1[112];
    static const uint8_t zero[1] = {0};
    const uint32_t frame = kind ? 1280 : 1552, rlen = kind ? 0 : 112;
    const uint32_t sp = c->r[29][0] - frame, res = kind ? 0 : c->r[6][0];
    uint32_t diff = 0, r, k, off, mut_at = 0, mut_k = 0;
    uint8_t mut_save[32];
    VU *vu = &g_vu[0];
    CollCk *ck = &g_ck[kind];
    if (g_ck_depth || (kind && g_cb.mode) || LW(c->r[28][0] - 22808u) != 0x004787D0u) {
        /* from inside a check (a callback that collides again), or the call that has the collision program loaded
         * into VU0 first (the DMA, the guest's note of it at gp-22808: the second run of a check would not do it) */
        nat(c);
        return;
    }
    g_ck_depth++;
    if (kind && g_coll_mut > 0) {
        /* 0x273400 has no hit in the checks of a game (the segment is a car's probe, the strip is not under it), so its callback's
         * record and replay has not run (its own segments are short probes that miss the strips). The segment (a0: its start p,
         * then its end q, two qwords) becomes the ray from p along one of the six axes in turn, K = 1, 2, 4 ... 2048 long, for
         * both passes, and is put back after. */
        float *sg = (float *)GPTR(c->r[4][0] & ~15u);
        mut_at = c->r[4][0] & ~15u;
        mut_k = 1u << (g_mut.calls / 6 % 12);
        memcpy(mut_save, sg, 32);
        if (g_mut.calls < 14) {
            const uint8_t *hd = GPTR(c->r[5][0]);
            LOG("collision 0x273400 ray %u: p %g %g %g, q %g %g %g; strip %08X: %08X %08X %02X %02X %02X %02X %02X", mut_k, sg[0], sg[1],
                sg[2], sg[4], sg[5], sg[6], c->r[5][0], *(const uint32_t *)hd, *(const uint32_t *)(hd + 4), hd[10], hd[11], hd[12], hd[13],
                hd[14]);
        }
        for (k = 0; k < 3; k++)
            sg[4 + k] = sg[k] + (k == g_mut.calls % 3 ? (g_mut.calls % 6 < 3 ? 1.0f : -1.0f) * (float)mut_k : 0.0f);
        g_coll_mut--;
        g_mut.calls++;
    }
    memcpy(&before, c, sizeof before);
    memcpy(&vu0, vu, sizeof vu0);
    memcpy(fr0, GPTR(sp), frame);
    memcpy(rs0, rlen ? GPTR(res) : zero, rlen);
    if (kind) {
        rt_set_stub(CB_STUB, coll_cb_stub);
        g_cb.mode = 1;
        g_cb.real = c->r[6][0];
        g_cb.n = g_cb.i = 0;
        c->r[6][0] = CB_STUB;
    }
    nat(c);
    memcpy(&after, c, sizeof after);
    memcpy(&vu1, vu, sizeof vu1);
    memcpy(fr1, GPTR(sp), frame);
    memcpy(rs1, rlen ? GPTR(res) : zero, rlen);
    memcpy(c, &before, sizeof before);
    memcpy(vu, &vu0, sizeof vu0);
    memcpy(GPTR(sp), fr0, frame);
    if (rlen)
        memcpy(GPTR(res), rs0, rlen);
    if (kind) {
        g_cb.mode = 2;
        g_cb.bad = 0;
        c->r[6][0] = CB_STUB;
    }
    ref(c);
    if (kind) {
        g_cb.mode = 0;
        ck->callbacks += g_cb.n;
        if (g_cb.bad || g_cb.i != g_cb.n)
            diff |= 1 << 7;
        if (mut_at) {
            g_mut.with_cb += g_cb.n != 0;
            g_mut.cbs += g_cb.n;
            g_mut.bad += g_cb.bad != 0 || g_cb.i != g_cb.n;
            if (g_cb.n && g_mut.with_cb <= 8)
                LOG("collision 0x273400 callback replay: ray length %u, %u callbacks recorded, %u answered, %u mismatched, "
                    "variant returned %08X", mut_k, g_cb.n, g_cb.i, g_cb.bad, after.r[2][0]);
            if (g_mut.calls % 50 == 0 || !g_coll_mut)
                LOG("collision 0x273400 rays: %u calls, %u with callbacks (%u callbacks), %u replays differ", g_mut.calls,
                    g_mut.with_cb, g_mut.cbs, g_mut.bad);
        }
    }
    ck->checked++;
    ck->hits += c->r[2][0] != 0;
    if (memcmp(after.r, c->r, sizeof c->r) || memcmp(after.hi, c->hi, sizeof c->hi) || memcmp(after.lo, c->lo, sizeof c->lo))
        diff |= 1;
    if (memcmp(after.f, c->f, sizeof c->f) || after.fcc != c->fcc)
        diff |= 1 << 1;
    /* VU0: what is read after the entries (work/vu0live.py sites): vf2.xyz vf4.xyz vf5.w vf6 vf7 vf8 vf9 vf10.w vf19
     * vf22.w vf23 vf24, VI1 VI2 VI7 VI8 VI15, Q */
    if (memcmp(after.vf[2], c->vf[2], 12) || memcmp(after.vf[4], c->vf[4], 12) || after.vf[5][3] != c->vf[5][3] ||
        memcmp(after.vf[6], c->vf[6], 4 * sizeof c->vf[6]) || memcmp(after.vf[19], c->vf[19], sizeof c->vf[19]) ||
        memcmp(after.vf[23], c->vf[23], 2 * sizeof c->vf[23]) || after.vf[10][3] != c->vf[10][3] ||
        after.vf[22][3] != c->vf[22][3])
        diff |= 1 << 2;
    if (after.vi[1] != c->vi[1] || after.vi[2] != c->vi[2] || after.vi[7] != c->vi[7] || after.vi[8] != c->vi[8] ||
        after.vi[15] != c->vi[15] || F2BITS(after.vq) != F2BITS(c->vq))
        diff |= 1 << 3;
    for (off = 0; off < frame; off++)
        if (fr1[off] != GPTR(sp)[off]) {
            diff |= 1 << 4;
            break;
        }
    if (rlen && memcmp(rs1, GPTR(res), rlen))
        diff |= 1 << 5;
    if (kind && c->r[6][0] == CB_STUB)
        c->r[6][0] = g_cb.real;         /* (a2, a temporary, is the caller's again) */
    for (r = 0; r < 32; r++)
        if (memcmp(after.vf[r], c->vf[r], sizeof c->vf[r]))
            ck->dead_vf |= 1u << r;
    for (r = 0; r < 16; r++)
        if (after.vi[r] != c->vi[r])
            ck->dead_vi |= 1u << r;
    if (diff && ck->bad++ < 12) {
        for (off = 0; off < frame && fr1[off] == GPTR(sp)[off]; off++)
            ;
        LOG("collision 0x%s inline variant differs (%X): v0 %08X/%08X, vi1 %u/%u, vi2 %u/%u; first frame byte %u of %u",
            kind ? "273400" : "273BF0", diff, after.r[2][0], c->r[2][0], after.vi[1], c->vi[1], after.vi[2], c->vi[2], off,
            frame);
        for (r = 0; r < 32; r++) {
            for (k = 0; k < 4; k++) {
                if (F2BITS(after.vf[r][k]) != F2BITS(c->vf[r][k]) && ck->bad < 6)
                    LOG("  vf%u.%u: variant %08X, recompiled %08X", r, k, F2BITS(after.vf[r][k]), F2BITS(c->vf[r][k]));
                if (after.r[r][k] != c->r[r][k] && ck->bad < 6)
                    LOG("  r%u.%u: variant %08X, recompiled %08X", r, k, after.r[r][k], c->r[r][k]);
            }
            if (F2BITS(after.f[r]) != F2BITS(c->f[r]) && ck->bad < 6)
                LOG("  f%u: variant %08X, recompiled %08X", r, F2BITS(after.f[r]), F2BITS(c->f[r]));
        }
    }
    if (ck->checked == 200 || ck->checked == COLL_PAIR_CHECK || ck->checked % 20000 == 0)
        LOG("collision 0x%s inline variant: %u calls checked (%u differ so far), %u returned a hit, %u callbacks; VU0 state it "
            "leaves otherwise: vf %08X, VI %04X", kind ? "273400" : "273BF0", ck->checked, ck->bad, ck->hits, ck->callbacks,
            ck->dead_vf, ck->dead_vi);
    /* the game goes on from what the variant left, as if it had run alone (the recompiled function ran to be compared
     * with, and for 0x273400 on the callback's recorded answers): the check cannot change what happens */
    memcpy(c, &after, sizeof after);
    memcpy(vu, &vu1, sizeof vu1);
    memcpy(GPTR(sp), fr1, frame);
    if (rlen)
        memcpy(GPTR(res), rs1, rlen);
    if (kind && c->r[6][0] == CB_STUB)
        c->r[6][0] = g_cb.real;
    if (mut_at)
        memcpy(GPTR(mut_at), mut_save, 32);
    g_ck_depth--;
}

static inline void coll_run_bf0(R5900 *c)
{
    if (!g_coll_bf0 || !g_vu0_coll)
        f_00273BF0(c);
    else if (PS2_UNLIKELY(g_coll_bf0 == 2 || g_ck[0].checked < COLL_PAIR_CHECK))
        coll_pair_check(c, n_00273BF0, f_00273BF0, 0);
    else
        n_00273BF0(c);
}

static inline void coll_run_400(R5900 *c)
{
    if (!g_coll_400 || !g_vu0_coll)
        f_00273400(c);
    else if (PS2_UNLIKELY(g_coll_400 == 2 || g_ck[1].checked < COLL_PAIR_CHECK))
        coll_pair_check(c, n_00273400, f_00273400, 1);
    else
        n_00273400(c);
}

static uint64_t g_ct[2][2];             /* ticks, calls */

static void coll_timed(R5900 *c, int kind)
{
    const uint64_t t0 = plat_ticks();
    if (kind)
        coll_run_400(c);
    else
        coll_run_bf0(c);
    g_ct[kind][0] += plat_ticks() - t0;
    if (++g_ct[kind][1] % 5000 == 0)
        LOG("collision 0x%s: %u calls, %u ticks a call (mode %d)", kind ? "273400" : "273BF0", (uint32_t)g_ct[kind][1],
            (uint32_t)(g_ct[kind][0] / g_ct[kind][1]), kind ? g_coll_400 : g_coll_bf0);
}

void hle_coll_bf0(R5900 *c)
{
    if (PS2_UNLIKELY(g_coll_time))
        coll_timed(c, 0);
    else
        coll_run_bf0(c);
}

void hle_coll_400(R5900 *c)
{
    if (PS2_UNLIKELY(g_coll_time))
        coll_timed(c, 1);
    else
        coll_run_400(c);
}

/* The world batch's box test (VU0 entry 0x0A18) for f_0026AF50 done natively (runtime/hle_render.c, autopilot "wbn"):
 * the batch's box at b (the corner at +48, the edges at +32, +0, +16) against the camera's planes at pl. What the
 * call does when the entry is native and checked (ps2_vu0_callms), without the registers going through c->vf both
 * ways. Leaves what work/vu0live.py sites says something reads after this call of f_0026AF50: vf6..vf8, vf9, vf10.w,
 * VI1 and VI2 (the corners, the row planes in the x and y lanes of vf9..vf12, their z and w lanes as loaded).
 * Returns VI1 (0 outside, 2 inside, 1 partly), or -1 when the call would not be this native (another program in VU0, the
 * entry not checked yet, a debugging switch): nothing has been touched then. */
int vu0_box_world(R5900 *c, uint32_t b, uint32_t pl)
{
    VU *vu = &g_vu[0];
    typeof(g_vu0_fast[0]) *f = &g_vu0_fast[(0x0A18 >> 3) & 511];
    float vf[15][4];
    uint32_t vi[3];
    if (PS2_UNLIKELY(!(vu->vf == c->vf && vu->mem && g_vu_mode == 1 && f->gen == g_micro_gen[0] && f->prog &&
                       f->fn == vu0_box_test && f->checked >= VU0_CHECK) ||
                     ((g_trace > 1) | g_vu0_disabled | g_vu0_stat | g_vu0_chk_all | g_vu0_poison)))
        return -1;
    g_vu0_runs++;
    vu->cycle += 8;
    if (vu->q_pending | vu->p_pending | vu->fq_n) {
        q_sync(vu);
        p_sync(vu);
        flags_sync(vu);
    }
#define LQ(r_, a_) do { const float *p_ = (const float *)GPTR((a_) & ~15u); vf[r_][0] = p_[0]; vf[r_][1] = p_[1]; \
                        vf[r_][2] = p_[2]; vf[r_][3] = p_[3]; } while (0)
    LQ(1, b + 48u); LQ(6, b + 32u); LQ(7, b); LQ(8, b + 16u);
    LQ(9, pl + 32u); LQ(10, pl + 48u); LQ(11, pl + 64u); LQ(12, pl + 80u); LQ(13, pl); LQ(14, pl + 16u);
#undef LQ
    box_core(vf, vi);
    memcpy(c->vf[6], vf[6], 3 * sizeof vf[0]);          /* vf6..vf8 */
    memcpy(c->vf[9], vf[9], sizeof vf[0]);
    c->vf[10][3] = vf[10][3];
    c->vi[1] = vi[1];
    c->vi[2] = vi[2];
    vu->tpc = f->tpc;
    return (int)vi[1];
}

void ps2_vu0_callms(R5900 *c, uint32_t addr)
{
    VU *vu = &g_vu[0];
    /* An entry whose program is known (and, if it is native, has had its checks) with none of the debugging
     * switches on: vu0_fast's steps without the layers between (3700 calls a frame). The pipelined state (Q, P,
     * flags in flight) is drained only when there is some. */
    if (PS2_LIKELY(vu->vf == c->vf && vu->mem && g_vu_mode == 1)) {
        typeof(g_vu0_fast[0]) *f = &g_vu0_fast[(addr >> 3) & 511];
        if (f->gen == g_micro_gen[0] && f->prog && (f->fn ? f->checked >= VU0_CHECK : 1) &&
            !((g_trace > 1) | g_vu0_disabled | g_vu0_stat | g_vu0_chk_all | g_vu0_poison)) {
            g_vu0_runs++;
            if (f->fn) {
                vu->cycle += 8;
                if (vu->q_pending | vu->p_pending | vu->fq_n) {
                    q_sync(vu);
                    p_sync(vu);
                    flags_sync(vu);
                }
                {
                    WT_MARK(wm);
                    f->fn(vu);
                    if (addr == 0x138)
                        WT_DONE(vis138, wm);
                    else if (addr == 0x920)
                        WT_DONE(row920, wm);
                }
                vu->tpc = f->tpc;
            } else if (PS2_UNLIKELY(g_vu0_time)) {
                const uint64_t t0 = plat_ticks();
                rec_run(vu, f->prog, addr);
                g_vu0t_whole += plat_ticks() - t0;
                g_vu0t_n++;
                if (f->prog->chain == 0x004787D0u) {
                    /* the collision entries: calls and those whose VI1 (the hit) came back non-zero */
                    const uint32_t k = addr == 0x0000 ? 0 : addr == 0x0028 ? 1 : addr == 0x0388 ? 2 : addr == 0x0390 ? 3 :
                                       addr == 0x0550 ? 4 : addr == 0x03D8 ? 5 : 6;
                    g_vu0t_coll[k][0]++;
                    g_vu0t_coll[k][1] += (vu->vi[1] & 0xFFFF) != 0;
                }
            } else {
                rec_run(vu, f->prog, addr);
            }
            return;
        }
    }
    if (g_vu0_disabled)
        return;
    if (g_trace > 1 && !g_vu0_calls[(addr >> 3) & 511]++)
        LOG("VU0 micro call at %04X", addr);
    if (vu->vf != c->vf || !vu->mem)
        vu0_bind(c);
    if (PS2_UNLIKELY(g_vu0_stat > 1))
        g_vu0_site = (uintptr_t)__builtin_return_address(0);
    if (!vu0_fast(vu, addr))
        run(vu, addr);
}

void ps2_vu0_callmsr(R5900 *c)
{
    ps2_vu0_callms(c, g_cmsar0 * 8);
}

void ps2_vu0_ctc2(R5900 *c, uint32_t reg, uint32_t v)
{
    VU *vu0 = &g_vu[0];
    (void)c;
    switch (reg) {
    case 16: vu0->status = (vu0->status & 0x3F) | (v & 0xFC0); break;
    case 18: vu0->clip = v & 0xFFFFFF; break;
    case 27: g_cmsar0 = v & 0xFFFF; break;
    case 28:
        g_fbrst = v & 0x0C0C;
        if (v & 0x002) memset(vu0->fq, 0, sizeof vu0->fq), vu0->fq_n = 0;
        if (v & 0x200) g_vu[1].fq_n = 0;
        break;
    case 31:                                /* CMSAR1: start VU1 */
        vu1_setup();
        run(&g_vu[1], (v & 0xFFFF) * 8);
        break;
    default:
        if (g_trace > 1)
            LOG("ctc2 vu0 control %u = %08X", reg, v);
        break;
    }
}

uint32_t ps2_vu0_cfc2(R5900 *c, uint32_t reg)
{
    VU *vu0 = &g_vu[0];
    (void)c;
    switch (reg) {
    case 16: return vu0->status & 0xFFF;
    case 17: return vu0->mac & 0xFFFF;
    case 18: return vu0->clip & 0xFFFFFF;
    case 26: return vu0->tpc / 8;
    case 27: return g_cmsar0;
    case 28: return g_fbrst;
    case 29: return 0;                      /* VPU-STAT: both units idle */
    default: return 0;
    }
}

void ps2_vu0_clip(R5900 *c, float x, float y, float z, float w)
{
    VU *vu0 = &g_vu[0];
    uint32_t f = 0;
    (void)c;
    w = fabsf(w);
    if (x > +w) f |= 0x01;
    if (x < -w) f |= 0x02;
    if (y > +w) f |= 0x04;
    if (y < -w) f |= 0x08;
    if (z > +w) f |= 0x10;
    if (z < -w) f |= 0x20;
    vu0->clip = ((vu0->clip << 6) | f) & 0xFFFFFF;
}

void ps2_vu0_unhandled(R5900 *c, uint32_t pc, const char *what)
{
    (void)c;
    LOG("VU0 macro %s at %08X not emulated", what, pc);
}

void vu0_init(void)
{
    memset(g_vu, 0, sizeof g_vu);
    vu1_setup();
}
