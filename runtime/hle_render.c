/*
 * Native rendering, game side (docs/native-renderer.md).
 *
 * The game builds each frame as a DMA chain for VU1 and the GS. Render
 * functions replaced here keep the game's own decisions (culling, ordering)
 * but put a native draw command into the chain instead of a VU1 batch; the
 * platform backend executes it when the chain gets there
 * (gs_backend_native_batch), so native and emulated draws stay in order.
 * Platforms that draw natively set g_native_render; otherwise everything goes
 * through the original code.
 */
#include "rt.h"
#include "gs.h"
#include "vu.h"
#include "wtime.h"

#include <stddef.h>
#include <string.h>

int g_native_render;

#define GP 0x004E8670u

void f_0026AF50(R5900 *c);
void ps2_vu0_callms(R5900 *c, uint32_t addr);

static void set_v0(R5900 *c, int32_t v)
{
    c->r[2][0] = (uint32_t)v;
    c->r[2][1] = (uint32_t)(v >> 31);
}

static void load_vf(R5900 *c, int r, uint32_t addr)
{
    int k;
    for (k = 0; k < 4; k++)
        c->vf[r][k] = LWF(addr + 4 * k);
}

/* Pipelines drawn natively, by the microprogram the group setup (0x26B2F0)
 * picked: prelit world (V4-8 colours), instanced props (float UVs, colour and
 * matrices per instance in VU1 memory 932..), lit world (V3-8 colours plus
 * lighting data at TOPS+204). */
int nw_kind(uint32_t code)
{
    switch (code) {
    case 0x0046F0D0u: case 0x004745C0u: case 0x0047E300u:
        return NW_PRELIT;
    case 0x00471FA0u:
        return NW_INST;
    case 0x00470650u: case 0x00472EB0u:
        return NW_LIT;
    default:
        return NW_NONE;
    }
}

/* does the batch have the layout the pipeline's native draw needs? */
static int layout_ok(const NwBlock *b, int kind)
{
    switch (kind) {
    case NW_PRELIT: return !b->uv32 && b->col && !b->rgb && !b->lit;
    case NW_INST: return b->uv32 && !b->col && !b->lit;
    case NW_LIT: return !b->uv32 && b->col && b->rgb && b->lit && (b->nlit == b->n || b->nlit == ((b->n + 3) & ~3u));
    default: return 0;
    }
}

PcSeen *g_pc_seen;

static void batch_start(NwPacket *p, NwBlock *b, uint32_t a)
{
    pc_batch((int)(b - p->b));
    memset(b, 0, sizeof *b);
    b->start = a;
    b->cl = p->cl;
    b->wl = p->wl;
    b->mode = p->mode;
    memcpy(b->srow, p->row, sizeof b->srow);
}

/* World render data: batches that UNPACK n vertices -- positions (V4-32, w =
 * strip restart flag), UVs (V2-16 4.12 or V2-32) and colours (V4-8 or V3-8
 * unsigned, or none) -- to TOPS+0/1/2 with CL 3/WL 1, lit pipelines also
 * V4-8 lighting data to TOPS+204 with CL 1/WL 1, then ITOP n and, inside the
 * packet, MSCNT (the last batch's MSCNT may come after the packet). */
/* why render data was not taken (status: the first few, logged) */
static int nw_reject(int why, uint32_t what)
{
    static int logged;
    if (why == 7 && ((what >> 24) & 0x7F) == 0x4A)
        return 0;                       /* a microprogram upload (MPG): not render data */
    if (logged < 24) {
        logged++;
        LOG("world render data not native: reason %d (%08X)", why, what);
    }
    return 0;
}

int nw_packet_parse(uint32_t a, uint32_t end, int kind, NwPacket *p)
{
    uint32_t got = 0;
    NwBlock *b = &p->b[0];
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
            if ((got & 3) == 1 && kind != NW_INST) {
                /* untextured (garage, a few in races): no UVs. The draw
                 * takes it only with PRIM's TME clear; the arrays need a
                 * TEX0 source anyway, the positions are one */
                b->uv = b->pos;
                b->nouv = 1;
                got |= 2;
            }
            if ((got & 3) != 3 || (code & 0x3FF) != b->n)
                return nw_reject(3, code);
            got |= 8;
            break;
        case 0x17:                                      /* MSCNT: the batch is complete */
            if (got != 11 || !layout_ok(b, kind) || p->nb + 1 >= NW_BATCHES)
                return nw_reject(4, got | (uint32_t)layout_ok(b, kind) << 8 | (uint32_t)kind << 12);
            b->mscnt = 1;
            b = &p->b[++p->nb];
            batch_start(p, b, a);
            got = 0;
            break;
        default:
            pc_dep(3);                  /* CL, WL and MODE as they are */
            if ((cmd & 0x70) == 0x60 && (code & 0x8000) && !(got & 8) && p->mode == 0) {   /* UNPACK to TOPS+... */
                uint32_t fmt = cmd & 0xF, off = code & 0x3FF, usn = (code >> 14) & 1;
                if (!num)
                    num = 256;
                if (off == 204 && fmt == 0xE && !usn && p->cl == 1 && p->wl == 1) {
                    /* lighting data: one per vertex, rounded up to 4 (layout_ok checks) */
                    b->lit = a;
                    b->nlit = num;
                    a += 4 * num;
                    if (a > end)
                        return nw_reject(1, code);
                    break;
                }
                if (off == 2 && fmt == 0xC && kind == NW_INST && p->cl == 3 && p->wl == 1 && b->n &&
                    num == ((b->n + 3) & ~3u)) {
                    /* normals of the lit instanced props (471FA0 lights per instance) */
                    b->nrm = a;
                    a += 16 * num;
                    if (a > end)
                        return nw_reject(1, code);
                    break;
                }
                if (b->n && num != b->n)
                    return nw_reject(2, code);
                b->n = num;
                if (p->cl != 3 || p->wl != 1) {
                    return 0;
                } else if (off == 0 && fmt == 0xC) {
                    b->pos = a;
                    a += 16 * num;
                    got |= 1;
                } else if (off == 1 && fmt == 0x5 && !usn) {
                    b->uv = a;
                    a += 4 * num;
                    got |= 2;
                } else if (off == 1 && fmt == 0x4) {
                    b->uv = a;
                    b->uv32 = 1;
                    a += 8 * num;
                    got |= 2;
                } else if (off == 2 && fmt == 0xE && usn) {
                    b->col = a;
                    a += 4 * num;
                } else if (off == 2 && fmt == 0xA && usn) {
                    b->col = a;
                    b->rgb = 1;
                    a += (3 * num + 3) & ~3u;
                } else {
                    return nw_reject(5, code);
                }
                if (a > end || b->n < 3 || b->n > 220)  /* 8-bit vertex indices */
                    return nw_reject(6, b->n);
                break;
            }
            return nw_reject(7, code);
        }
    }
    if (got) {
        if (got != 11 || !layout_ok(b, kind))
            return 0;
        p->nb++;
    }
    return p->nb > 0;
}

/* The parses remembered. A packet's parse is a function of the VIFcodes it reads (and the STROW
 * words), where they are, and those of the VIF's registers it uses before setting them; the world's
 * and the cars' render data stays where it was loaded, so nearly all of a frame's packets are last
 * frame's (the world's parser walked the same codes for 1.2% of a race's frame, a memset a batch
 * included; its packets hold 1 to 21 batches of 17 to 24 words). What a parse made is kept with the
 * first and the last PC_HALF words it read: when those are still there the packet is taken to be
 * the same one (as a display list is taken to be its batch's by five sampled words: dl_check), its
 * batches are copied out, and the registers a batch had from before the packet are put in from the
 * VIF's now. A packet has two places it may be kept in (a pair of the table); the pool is emptied
 * when it is full. */
#include "nw_car.h"

#define PC_POOL (512u * 1024u)
/* Two cache lines, the first with all a lookup reads (the key, the registers going in but the ROW's, the pool place) and
 * the call number it writes: it was 72 bytes, across two or three lines, and every call read and wrote more than one. */
typedef struct PcEnt {
    uint32_t start, end;
    uint32_t data;                      /* in the pool: the words' addresses, their values, the batches' registers
                                         * from before (a byte each), the batches */
    uint32_t used;                      /* the call that last took it */
    uint16_t nb;
    uint16_t room;                      /* words it has there (a packet parsed again fits where it was) */
    uint8_t kind, nw, set, dep, ocl, owl, omode;
    uint8_t icl, iwl, imode;            /* the VIF's registers going in: CL, WL, MODE (8 bits each: more is not kept) */
    uint16_t pad;
    uint32_t irow[4], orow[4];          /* the ROW going in, and the ROW coming out */
} __attribute__((aligned(32))) PcEnt;
_Static_assert(sizeof(PcEnt) == 64, "PcEnt: two cache lines");
typedef struct PcTab {
    PcEnt *e;
    uint32_t shift;                     /* 32 - log2 of the pairs */
    uint32_t calls, stat[4];            /* status: parses asked for, answered from memory, not kept, pool emptied */
} PcTab;
static PcEnt g_pc_world_e[2048], g_pc_car_e[512];
static PcTab g_pc_world = {g_pc_world_e, 22}, g_pc_car = {g_pc_car_e, 24};
static uint32_t g_pc_pool[PC_POOL / 4], g_pc_used;
int g_nwc_on = 1;                       /* autopilot "parsecache 0": every packet parsed */
int g_trust;                            /* gs.h: autopilot "trust N", the checks that guest memory is unchanged left out */

typedef int (*PcParse)(uint32_t a, uint32_t end, int kind, void *packet);

/* nb, blocks (bsize bytes each, CL, WL, MODE and ROW as seven words at breg in one), regs (the
 * packet's CL, WL, MODE, ROW: the VIF's going in, the packet's coming out). With ref, the batches
 * are not copied: *ref is where they are (the pool's copy, or blocks after a parse), valid until the
 * next call. The words a parse read are kept as offsets in guest RAM (an address with PS2_RAM_MASK
 * applied: what LW reads), so the check of a kept packet reads them without masking each one. */
int g_opt3 = 1;                         /* autopilot "opt3 0": the batches copied out as before, the lit props' colours made
                                         * every frame, the lit world's in the scalar loop (A/B, 2026-10-16) */

static int pc_cached(PcTab *t, PcParse parse, uint32_t a, uint32_t end, int kind, void *packet, int *nb, void *blocks,
                     uint32_t bsize, uint32_t breg, uint32_t *regs, void **ref)
{
    PcEnt *pair = &t->e[(((a >> 4) * 2654435761u) >> t->shift) * 2u], *e;
    static PcSeen seen;
    uint32_t k, need, nw, in[7];
    int ok, way;
    t->stat[0]++;
    if (ref)
        *ref = blocks;
    if (!g_nwc_on)
        return parse(a, end, kind, packet);
    t->calls++;
    memcpy(in, regs, sizeof in);
    for (way = 0; way < 2; way++) {
        const uint32_t *at, *val;
        const uint8_t *ram = ps2_ram;
        e = &pair[way];
        if (!(e->start == a && e->end == end && e->kind == kind && (!(e->dep & 1) || (e->icl == in[0] && e->iwl == in[1])) &&
              (!(e->dep & 2) || e->imode == in[2]) && (!(e->dep & 4) || !memcmp(e->irow, &in[3], 16))))
            continue;
        at = &g_pc_pool[e->data];
        val = at + e->nw;
        __asm__("" : "+r"(ram));        /* the base in a register: indexed loads, not an add of the constant a word */
        if (PS2_UNLIKELY(g_trust & 1)) {
            k = e->nw;                  /* debugging: the words are not read */
        } else {
            /* all of the words, without a branch a word (a difference is rare: the packet changed) */
            const uint32_t *pa = at - 1, *pv = val - 1;
            uint32_t diff = 0;
#pragma GCC unroll 1
            for (k = e->nw; k; k--)
                diff |= *(const uint32_t *)(const void *)(ram + *++pa) ^ *++pv;
            k = diff ? 0 : e->nw;
        }
        if (k == e->nw) {
            const uint8_t *inh = (const uint8_t *)(val + e->nw);
            uint32_t *kept = (uint32_t *)(uintptr_t)(val + e->nw + (e->nb + 3u) / 4u);
            if (ref) {
                blocks = kept;          /* the inherited registers are put into the pool's copy */
                *ref = kept;
            } else {
                memcpy(blocks, kept, e->nb * bsize);
            }
            for (k = 0; k < e->nb && inh[k]; k++) {
                uint32_t *r = (uint32_t *)(void *)((uint8_t *)blocks + k * bsize + breg);
                if (inh[k] & 1) {
                    r[0] = in[0];
                    r[1] = in[1];
                }
                if (inh[k] & 2)
                    r[2] = in[2];
                if (inh[k] & 4)
                    memcpy(&r[3], &in[3], 16);
            }
            *nb = e->nb;
            if (e->set & 1) {
                regs[0] = e->ocl;
                regs[1] = e->owl;
            }
            if (e->set & 2)
                regs[2] = e->omode;
            if (e->set & 4)
                memcpy(&regs[3], e->orow, 16);
            e->used = t->calls;
            t->stat[1]++;
            return 1;
        }
        break;                          /* the packet's own entry, and its words have changed: that one again */
    }
    if (way == 2)
        e = !pair[0].start ? &pair[0] : !pair[1].start ? &pair[1] :
            t->calls - pair[0].used > t->calls - pair[1].used ? &pair[0] : &pair[1];
    e->start = 0;
    seen.n = 0;
    seen.set = seen.dep = 0;
    g_pc_seen = &seen;
    ok = parse(a, end, kind, packet);
    g_pc_seen = NULL;
    if (!ok || *nb < 1)
        return ok;
    nw = seen.n < 2 * PC_HALF ? seen.n : 2 * PC_HALF;
    need = 2 * nw + ((uint32_t)*nb + 3u) / 4u + ((uint32_t)*nb * bsize + 3u) / 4u;
    if (need > PC_POOL / 4 || regs[0] > 255 || regs[1] > 255 || regs[2] > 255 || in[0] > 255 || in[1] > 255 || in[2] > 255) {
        t->stat[2]++;
        return ok;
    }
    for (k = 1; k < (uint32_t)*nb; k++)
        if (!seen.inh[k - 1] && seen.inh[k])
            return ok;                  /* (cannot be: a register once set stays set) */
    if (need > e->room && g_pc_used + need > PC_POOL / 4) {
        memset(g_pc_world_e, 0, sizeof g_pc_world_e);
        memset(g_pc_car_e, 0, sizeof g_pc_car_e);
        g_pc_used = 0;
        t->stat[3]++;
        e = &pair[0];
    }
    if (need > e->room) {
        e->data = g_pc_used;
        e->room = (uint16_t)(need > 65535u ? 0u : need);
        g_pc_used += need;
    }
    {
        uint32_t *at = &g_pc_pool[e->data], *val = at + nw;
        uint8_t *inh = (uint8_t *)(val + nw);
        for (k = 0; k < nw; k++) {
            at[k] = seen.at[k] & PS2_RAM_MASK;
            val[k] = seen.val[k];
        }
        for (k = 0; k < (uint32_t)*nb; k++)
            inh[k] = seen.inh[k];
        memcpy(val + nw + ((uint32_t)*nb + 3u) / 4u, blocks, (uint32_t)*nb * bsize);
    }
    e->nw = (uint8_t)nw;
    e->nb = (uint16_t)*nb;
    e->end = end;
    e->kind = (uint8_t)kind;
    e->set = seen.set;
    e->dep = seen.dep;
    e->icl = (uint8_t)in[0];
    e->iwl = (uint8_t)in[1];
    e->imode = (uint8_t)in[2];
    memcpy(e->irow, &in[3], 16);
    e->ocl = (uint8_t)regs[0];
    e->owl = (uint8_t)regs[1];
    e->omode = (uint8_t)regs[2];
    memcpy(e->orow, &regs[3], 16);
    e->used = t->calls;
    e->start = a;
    return ok;
}

static int nw_parse_any(uint32_t a, uint32_t end, int kind, void *packet)
{
    return nw_packet_parse(a, end, kind, packet);
}

static int nc_parse_any(uint32_t a, uint32_t end, int kind, void *packet)
{
    (void)kind;
    return nc_packet_parse(a, end, packet);
}

int nw_packet_cached(uint32_t a, uint32_t end, int kind, NwPacket *p)
{
    return pc_cached(&g_pc_world, nw_parse_any, a, end, kind, p, &p->nb, p->b, sizeof(NwBlock), offsetof(NwBlock, cl), &p->cl,
                     NULL);
}

int nc_packet_cached(uint32_t a, uint32_t end, NcPacket *p)
{
    return pc_cached(&g_pc_car, nc_parse_any, a, end, 0, p, &p->nb, p->b, sizeof(NcBlock), offsetof(NcBlock, cl), &p->cl, NULL);
}

int nw_packet_ref(uint32_t a, uint32_t end, int kind, NwPacket *p, const NwBlock **b)
{
    void *r;
    int ok = pc_cached(&g_pc_world, nw_parse_any, a, end, kind, p, &p->nb, p->b, sizeof(NwBlock), offsetof(NwBlock, cl), &p->cl,
                       g_opt3 ? &r : NULL);
    *b = g_opt3 ? (const NwBlock *)r : p->b;
    return ok;
}

int nc_packet_ref(uint32_t a, uint32_t end, NcPacket *p, const NcBlock **b)
{
    void *r;
    int ok = pc_cached(&g_pc_car, nc_parse_any, a, end, 0, p, &p->nb, p->b, sizeof(NcBlock), offsetof(NcBlock, cl), &p->cl,
                       g_opt3 ? &r : NULL);
    *b = g_opt3 ? (const NcBlock *)r : p->b;
    return ok;
}

/* status line: for the world's packets and the cars', parses asked for, answered from memory, not
 * kept, and the times the pool was emptied (since the last call) */
void pc_report(uint32_t out[8])
{
    memcpy(out, g_pc_world.stat, sizeof g_pc_world.stat);
    memcpy(out + 4, g_pc_car.stat, sizeof g_pc_car.stat);
    memset(g_pc_world.stat, 0, sizeof g_pc_world.stat);
    memset(g_pc_car.stat, 0, sizeof g_pc_car.stat);
}

int nw_block_parse(uint32_t block, NwBlock *b)
{
    static NwPacket p;
    uint32_t tag = LW(block), qwc = tag & 0xFFFF;
    int kind;
    if (((tag >> 28) & 7) != 6 || !qwc || qwc > 1024)
        return 0;
    for (kind = NW_PRELIT; kind <= NW_LIT; kind++) {
        memset(&p, 0, sizeof p);
        p.cl = 3;
        p.wl = 1;
        if (nw_packet_parse(block + 8, block + 16 + qwc * 16, kind, &p) && p.nb == 1 && !p.b[0].mscnt) {
            *b = p.b[0];
            return 1;
        }
    }
    return 0;
}

/* World batch (0x26AF50): a0 = batch group state, a1 = batch (bounds at +0..+63,
 * render data at +12, an optional second packet at +28), a2 = clip mode
 * (1: none, 2: guard band, 3: near plane), a3 = cull mode. Returns 0 when the
 * batch is outside the view. The game's own code runs: its render data is
 * drawn natively where the VIF meets it (vif_defer_call), which also covers
 * second packets and render data with several batches. Writing a native draw
 * command into the chain here (hle_world_batch_ee) is kept for reference. */
static void hle_world_batch_ee(R5900 *c);
extern int g_wstat;                     /* vif.c ("wstat 1") */
extern uint64_t g_wst[6];
uint64_t plat_ticks(void);

/* ---- 0x26B1C0 and 0x26BAA0 natively (config OVERRIDES; autopilot "cpy" and "vpk": 0 the recompiled function, 1 native,
 * 2 native and checked). The walker (0x21C780) puts a section's batches into a staging list [gp-23648] and 0x26B2B0
 * copies it into the chain; clocks (work/walker_sizing.md, section 9): the copy 198k cycles a frame in 109 calls, the view
 * packets 211k in 319 calls (225 of them followed by no batch, which cannot be skipped exactly: the packet's 17 qwords are
 * the staging list's queue until a batch or the next call takes them, and the environment index is stored at once). */
int g_cpy = 1, g_vpk = 1;
void f_0026B1C0(R5900 *c);
void f_0026BAA0(R5900 *c);
void f_001F3080(R5900 *c);
#define ENV_CUR 0x00667050u

static inline void cp16(uint32_t *d, const uint32_t *s)
{
    const uint32_t a = s[0], b = s[1], e = s[2], f = s[3];
    d[0] = a;
    d[1] = b;
    d[2] = e;
    d[3] = f;
}

/* the recompiled loops where the pointers are not as the fast path wants: eight quadwords loaded, eight stored, then
 * one at a time (lq and sq use the addresses with the low 4 bits cleared) */
static void stage_copy_generic(uint32_t dst, uint32_t src, int32_t n)
{
    int32_t i = 0;
    while (i < n - 7) {
        uint32_t t[32];
        int k;
        for (k = 0; k < 8; k++)
            cp16(t + 4 * k, (const uint32_t *)GPTR((src + 16u * (uint32_t)k) & ~15u));
        for (k = 0; k < 8; k++)
            cp16((uint32_t *)GPTR((dst + 16u * (uint32_t)k) & ~15u), t + 4 * k);
        i += 8;
        src += 128;
        dst += 128;
    }
    for (; i < n; i++) {
        cp16((uint32_t *)GPTR(dst & ~15u), (const uint32_t *)GPTR(src & ~15u));
        src += 16;
        dst += 16;
    }
}

static uint32_t g_cpy_n, g_cpy_generic, g_cpy_bad;

/* 0x26B1C0(a0 = the staging list): reserves the chain room through 0x1F3080 (the recompiled function, called as it
 * calls it), copies the staged quadwords to [gp-24240], resets the list; leaves v1 = the list's start */
void hle_stage_copy(R5900 *c)
{
    const uint32_t P = c->r[4][0], endp = LW(P + 4);
    const int32_t diff = (int32_t)(endp - LW(P + 8));
    uint32_t n, sp0, dst, src;
    WT_MARK(wm);
    if (!g_cpy || diff < 0) {
        f_0026B1C0(c);
        return;
    }
    n = (uint32_t)diff >> 4;
    sp0 = c->r[29][0];
    c->r[29][0] = sp0 - 48u;
    c->r[29][1] = SX(sp0 - 48u);
    c->r[4][0] = 0;
    c->r[4][1] = 0;
    c->r[5][0] = n;
    c->r[5][1] = 0;
    f_001F3080(c);
    c->r[29][0] = sp0;
    c->r[29][1] = SX(sp0);
    dst = LW(GP - 24240);
    src = LW(P + 8);
    if (!((src | dst) & 15) && (dst + n * 16u <= src || src + n * 16u <= dst)) {
        memcpy(GPTR(dst), GPTR(src), n * 16u);
        if (PS2_UNLIKELY(g_cpy == 2) && memcmp(GPTR(dst), GPTR(src), n * 16u))
            g_cpy_bad++;
    } else {
        stage_copy_generic(dst, src, (int32_t)n);
        g_cpy_generic++;
    }
    SW(GP - 24240, dst + n * 16u);
    SW(P + 4, src);
    c->r[3][0] = src;
    c->r[3][1] = SX(src);
    if (PS2_UNLIKELY(g_cpy == 2) && (++g_cpy_n == 100 || g_cpy_n % 5000 == 0))
        LOG("stage copy native: %u calls, %u by the generic loops, %u wrong", g_cpy_n, g_cpy_generic, g_cpy_bad);
    WT_DONE(copy, wm);
}

/* 0x26BAA0(a0 = the section state): the view packet. The staging list's count is set to 0 and 17 quadwords are queued
 * in it: a header, ten from the view's camera block, two fixed ones, two with the state's flags (+160, +176) or-ed in,
 * two more from the camera block; the environment index (the view) is stored at ENV_CUR; leaves v1 = the last slot.
 * The camera block is at 0x665EC0 + 560 * view + 1136. */
static void view_packet_native(R5900 *c, uint32_t S, uint32_t P)
{
    const int32_t view = (int32_t)(int8_t)LB(S + 153);
    const uint32_t cam = 0x00665EC0u + (uint32_t)(view * 560) + 1136u;
    uint32_t *q = (uint32_t *)GPTR(P + 16);
    const uint32_t *s = (const uint32_t *)GPTR(cam + 288), *t = (const uint32_t *)GPTR(cam + 448);
    const uint32_t *f = (const uint32_t *)GPTR(S + 160), *g = (const uint32_t *)GPTR(S + 176);
    int k;
    SW(ENV_CUR, (uint32_t)view);
    q[0] = 0x1000000Au;
    q[1] = 0;
    q[2] = 0x01000404u;
    q[3] = 0x6C0A03F6u;
    for (k = 0; k < 10; k++)
        cp16(q + 4 + 4 * k, s + 4 * k);
    q[44] = 0x10000005u;                /* slot 11 */
    q[45] = 0;
    q[46] = 0;
    q[47] = 0x50000005u;
    q[48] = 0x00008004u;                /* slot 12 */
    q[49] = 0x10000000u;
    q[50] = 14;
    q[51] = 0;
    q[52] = t[0] | f[0];                /* slot 13: the camera block's quadword at +448 with the state's flags */
    q[53] = t[1] | f[1];
    q[54] = 76;
    q[55] = 0;
    q[56] = t[4] | g[0];                /* slot 14: +464 */
    q[57] = t[5] | g[1];
    q[58] = 78;
    q[59] = 0;
    cp16(q + 60, t + 8);                /* slots 15 and 16: +480, +496 */
    cp16(q + 64, t + 12);
    SW(P, 17);
    c->r[3][0] = P + 256u;
    c->r[3][1] = SX(P + 256u);
}

static uint32_t g_vpk_n, g_vpk_bad;

static void __attribute__((noinline)) view_packet_checked(R5900 *c, uint32_t S, uint32_t P)
{
    static uint32_t m0[72], m1[72];
    uint32_t env0, env1, v1[2];
    int k, bad = 0;
    memcpy(m0, GPTR(P), 288);
    env0 = LW(ENV_CUR);
    view_packet_native(c, S, P);
    memcpy(m1, GPTR(P), 288);
    env1 = LW(ENV_CUR);
    v1[0] = c->r[3][0];
    v1[1] = c->r[3][1];
    memcpy(GPTR(P), m0, 288);
    SW(ENV_CUR, env0);
    f_0026BAA0(c);
    if (memcmp(m1, GPTR(P), 288) || env1 != LW(ENV_CUR) || v1[0] != c->r[3][0] || v1[1] != c->r[3][1]) {
        bad = 1;
        if (g_vpk_bad++ < 20) {
            LOG("view packet native differs: view %d, v1 %08X/%08X", (int)(int8_t)LB(S + 153), v1[0], c->r[3][0]);
            for (k = 0; k < 72; k++)
                if (m1[k] != LW(P + 4 * (uint32_t)k)) {
                    LOG("  word %d: native %08X, recompiled %08X", k, m1[k], LW(P + 4 * (uint32_t)k));
                    break;
                }
        }
    }
    (void)bad;
    if (++g_vpk_n == 100 || g_vpk_n % 5000 == 0)
        LOG("view packet native: %u calls checked (%u differ so far)", g_vpk_n, g_vpk_bad);
}

void hle_view_packet(R5900 *c)
{
    const uint32_t S = c->r[4][0], P = LW(GP - 23648);
    WT_MARK(wm);
    if (!g_vpk || ((S | P) & 15)) {
        f_0026BAA0(c);
        return;
    }
    if (PS2_UNLIKELY(g_vpk == 2))
        view_packet_checked(c, S, P);
    else
        view_packet_native(c, S, P);
    WT_DONE(viewpkt, wm);
}

/* ---- 0x26AF50 natively (autopilot "wbn"; 0: the recompiled function, 1: native, the first calls checked, 2: every call
 * checked, for debugging). What the function does (153 guest instructions; work/walker_sizing.md has the walker around
 * it): the box test (VU0 0x0A18) unless mode 1, then into the chain at [[gp-23648]+4]: a CALL tag to the render data
 * [batch+12], the qword at state + 0, the packets waiting at [gp-23648]+16 (their count at [gp-23648], set to 0), two
 * qwords of clip flags when the mode changed or packets were waiting, and the qword at state + 32 or a CALL to the
 * second packet [batch+28] with MSCNT; v0 = 1 (0 outside). The recompiled function leaves nothing else the callers use
 * (v1 and the VU0 registers it loaded are dead: vu0_box_world says which the box test leaves). */
int g_wbn = 1;
int vu0_box_world(R5900 *c, uint32_t b, uint32_t pl);
#define WBN_CHECK 100
static uint32_t g_wbn_checked, g_wbn_bad;
static uint32_t g_wbn_cov[7];           /* check coverage: mode 1, outside, inside, partly, packets waiting, second packet, clip flags */

/* returns 0, with nothing touched, when it does not handle the call */
static int world_batch_native(R5900 *c)
{
    const uint32_t S = c->r[4][0], B = c->r[5][0], cull = c->r[7][0] & 0xFF;
    uint32_t mode = c->r[6][0], P, pending, wp, blk, t1;
    uint32_t *q;
    if (c->r[6][1] || mode > 3 || (S & 15))
        return 0;
    P = LW(GP - 23648);
    pending = LW(P);
    wp = LW(P + 4);
    if ((P & 15) || (wp & 15) || pending > 256)
        return 0;
    if (mode != 1) {
        const int r = vu0_box_world(c, B, 0x01E327D0u + (uint32_t)((int32_t)(int8_t)LB(S + 153) * 96));
        if (r < 0)
            return 0;
        if (PS2_UNLIKELY(g_wbn == 2))
            g_wbn_cov[(r & 0xFFFF) == 0 ? 1 : r == 2 ? 2 : 3]++;
        if (!(r & 0xFFFF)) {
            set_v0(c, 0);
            return 1;
        }
        if (r == 2)
            mode = 1;
    } else if (PS2_UNLIKELY(g_wbn == 2)) {
        g_wbn_cov[0]++;
    }
    if (PS2_UNLIKELY(g_wbn == 2)) {
        g_wbn_cov[4] += pending != 0;
        g_wbn_cov[5] += LW(B + 28) != 0;
        g_wbn_cov[6] += (pending | (mode & ~(uint32_t)(int32_t)(int8_t)LB(S + 152))) != 0;
    }
    SB(S + 155, 1);
    blk = LW(B + 12);
    q = (uint32_t *)GPTR(wp);
    q[0] = 0x50000000u;                                 /* CALL, to the render data */
    q[1] = blk;
    q[2] = 0;
    q[3] = 0;
    cp16(q + 4, (const uint32_t *)GPTR(S));
    t1 = 8;                                             /* words written */
    if (pending) {
        memcpy(q + t1, GPTR(P + 16), pending * 16);
        t1 += pending * 4;
    }
    SW(P, 0);
    {
        const uint32_t v1 = pending | (mode & ~(uint32_t)(int32_t)(int8_t)LB(S + 152));
        SB(S + 152, mode);
        if (v1) {
            cp16(q + t1, (const uint32_t *)GPTR(S + 16));
            q[t1 + 4] = mode == 2 ? 2u : mode == 3 ? 10u : 0u;
            q[t1 + 5] = 0;
            q[t1 + 6] = cull;
            q[t1 + 7] = v1;
            t1 += 8;
        }
    }
    {
        const uint32_t second = LW(B + 28);
        if (second) {
            q[t1] = 0x50000000u;                        /* CALL, to the second packet, MSCNT after it */
            q[t1 + 1] = second;
            q[t1 + 2] = 0x17000000u;
            q[t1 + 3] = 0;
        } else {
            cp16(q + t1, (const uint32_t *)GPTR(S + 32));
        }
    }
    SW(P + 4, wp + (t1 + 4) * 4);
    set_v0(c, 1);
    return 1;
}

/* the native, then the recompiled function from the same state: what the callers read must be equal */
static void __attribute__((noinline)) world_batch_checked(R5900 *c)
{
    static R5900 before;
    static uint32_t m0[1100], m1[1100];
    const uint32_t S = c->r[4][0], P = LW(GP - 23648), pending = LW(P), wp = LW(P + 4);
    const uint32_t words = (pending > 256 ? 0 : pending + 8) * 4;
    uint32_t s0, p0, p4, s1, p1, p5, v0[2], vi1, vi2, k;
    float vf[5][4];
    int diff = 0;
    if (!words || (wp & 15)) {
        f_0026AF50(c);
        return;
    }
    memcpy(&before, c, sizeof before);
    memcpy(m0, GPTR(wp), words * 4);
    s0 = LW(S + 152);
    p0 = LW(P);
    p4 = LW(P + 4);
    if (!world_batch_native(c)) {
        f_0026AF50(c);
        return;
    }
    v0[0] = c->r[2][0];
    v0[1] = c->r[2][1];
    memcpy(vf[0], c->vf[6], 3 * sizeof vf[0]);
    memcpy(vf[3], c->vf[9], sizeof vf[0]);
    vf[4][3] = c->vf[10][3];
    vi1 = c->vi[1];
    vi2 = c->vi[2];
    memcpy(m1, GPTR(wp), words * 4);
    s1 = LW(S + 152);
    p1 = LW(P);
    p5 = LW(P + 4);
    memcpy(c, &before, sizeof before);
    memcpy(GPTR(wp), m0, words * 4);
    SW(S + 152, s0);
    SW(P, p0);
    SW(P + 4, p4);
    f_0026AF50(c);
    g_wbn_checked++;
    if (v0[0] != c->r[2][0] || v0[1] != c->r[2][1])
        diff |= 1;
    if (memcmp(vf[0], c->vf[6], 3 * sizeof vf[0]) || memcmp(vf[3], c->vf[9], sizeof vf[0]) || vf[4][3] != c->vf[10][3])
        diff |= 2;
    if (vi1 != c->vi[1] || vi2 != c->vi[2])
        diff |= 4;
    if (s1 != LW(S + 152) || p1 != LW(P) || p5 != LW(P + 4))
        diff |= 8;
    for (k = 0; k < words; k++)
        if (m1[k] != LW(wp + 4 * k)) {
            diff |= 16;
            break;
        }
    if (diff && g_wbn_bad++ < 20) {
        LOG("world batch native differs (%d): mode %u v0 %d/%d vi1 %u/%u vi2 %X/%X word %u of %u", diff, before.r[6][0],
            (int)v0[0], (int)c->r[2][0], vi1, c->vi[1], vi2, c->vi[2], k, words);
        if (diff & 16)
            LOG("  chain word %u: native %08X, recompiled %08X", k, m1[k], LW(wp + 4 * k));
    }
    if (g_wbn_checked == WBN_CHECK || g_wbn_checked % 5000 == 0)
        LOG("world batch native: %u calls checked (%u differ so far); mode 1 %u, outside %u, inside %u, partly %u, packets waiting %u, "
            "second packet %u, clip flags %u", g_wbn_checked, g_wbn_bad, g_wbn_cov[0], g_wbn_cov[1], g_wbn_cov[2], g_wbn_cov[3],
            g_wbn_cov[4], g_wbn_cov[5], g_wbn_cov[6]);
}

static void world_batch_run(R5900 *c)
{
    if (g_wbn) {
        if (g_wbn == 2 || g_wbn_checked < WBN_CHECK) {
            world_batch_checked(c);
            return;
        }
        if (world_batch_native(c))
            return;
    }
    f_0026AF50(c);
}

void hle_world_batch(R5900 *c)
{
    extern uint32_t g_env_skip;
    /* a reflection view that is not drawn this frame (runtime/hle.c) */
    if (g_env_skip && (g_env_skip >> (LB(c->r[4][0] + 153) & 31) & 1)) {
        set_v0(c, 0);
        return;
    }
    if (PS2_UNLIKELY(g_wstat == 1)) {
        const uint64_t t0 = plat_ticks();
        world_batch_run(c);
        g_wst[5] += plat_ticks() - t0;
    } else {
        WT_MARK(wm);
        world_batch_run(c);
        WT_DONE(batch, wm);
    }
    (void)hle_world_batch_ee;
}

static void hle_world_batch_ee(R5900 *c)
{
    uint32_t a0 = c->r[4][0], a1 = c->r[5][0], mode = c->r[6][0], a3 = c->r[7][0];
    uint32_t code = LW(GP - 28060), p, wp, pending, k, block = LW(a1 + 12);
    int kind = nw_kind(code);
    NwBlock b;
    if (!kind || LW(a1 + 28) || !nw_block_parse(block, &b) || !g_native_render) {
        f_0026AF50(c);
        return;
    }
    if (mode != 1) {
        /* the game's frustum test: VU0 micro 0x0A18 on the batch bounds and
         * the camera's planes; VI1 = 0 outside, 2 inside, else partly */
        int32_t cam = (int8_t)LB(a0 + 153);
        uint32_t planes = 0x01E327D0u + (uint32_t)(cam * 96), r;
        load_vf(c, 1, a1 + 48);
        load_vf(c, 6, a1 + 32);
        load_vf(c, 7, a1 + 0);
        load_vf(c, 8, a1 + 16);
        load_vf(c, 9, planes + 32);
        load_vf(c, 10, planes + 48);
        load_vf(c, 11, planes + 64);
        load_vf(c, 12, planes + 80);
        load_vf(c, 13, planes + 0);
        load_vf(c, 14, planes + 16);
        ps2_vu0_callms(c, 0x0A18);
        r = c->vi[1] & 0xFFFF;
        if (!r) {
            set_v0(c, 0);
            return;
        }
        if (r == 2)
            mode = 1;
    }
    SB(a0 + 155, 1);
    SB(a0 + 152, 0);            /* the next emulated batch uploads its clip flags again */
    p = LW(GP - 23648);
    wp = LW(p + 4);
    /* packets queued for this point of the chain go first, as in the original */
    pending = LW(p);
    for (k = 0; k < pending * 4; k++)
        SW(wp + 4 * k, LW(p + 16 + 4 * k));
    wp += pending * 16;
    SW(p, 0);
    /* CNT, 1 qword; VIF NOP, NATIVE (4 words): render data, clip mode, cull mode, microprogram */
    SW(wp + 0, 0x10000001u);
    SW(wp + 4, 0);
    SW(wp + 8, 0);
    SW(wp + 12, VIF_NATIVE_CODE | 4);
    SW(wp + 16, block);
    SW(wp + 20, mode);
    SW(wp + 24, a3);
    SW(wp + 28, code);
    SW(p + 4, wp + 32);
    set_v0(c, 1);
}
