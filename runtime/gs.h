/*
 * Graphics Synthesizer: GIF packet processing, register state, local memory.
 *
 * The block-order tables in this header are derived from PCSX2's
 * GSTables.cpp (Copyright (C) 2002-2026 PCSX2 Dev Team, GPL-3.0-or-later).
 * See THIRD_PARTY_NOTICES.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GS_H
#define GS_H

#include "rt.h"

/* pixel storage modes */
enum {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A
};

/* GS registers (A+D addresses) */
enum {
    GS_PRIM = 0x00, GS_RGBAQ = 0x01, GS_ST = 0x02, GS_UV = 0x03, GS_XYZF2 = 0x04, GS_XYZ2 = 0x05,
    GS_TEX0_1 = 0x06, GS_TEX0_2 = 0x07, GS_CLAMP_1 = 0x08, GS_CLAMP_2 = 0x09, GS_FOG = 0x0A,
    GS_XYZF3 = 0x0C, GS_XYZ3 = 0x0D, GS_TEX1_1 = 0x14, GS_TEX1_2 = 0x15, GS_TEX2_1 = 0x16,
    GS_TEX2_2 = 0x17, GS_XYOFFSET_1 = 0x18, GS_XYOFFSET_2 = 0x19, GS_PRMODECONT = 0x1A,
    GS_PRMODE = 0x1B, GS_TEXCLUT = 0x1C, GS_SCANMSK = 0x22, GS_MIPTBP1_1 = 0x34, GS_MIPTBP1_2 = 0x35,
    GS_MIPTBP2_1 = 0x36, GS_MIPTBP2_2 = 0x37, GS_TEXA = 0x3B, GS_FOGCOL = 0x3D, GS_TEXFLUSH = 0x3F,
    GS_SCISSOR_1 = 0x40, GS_SCISSOR_2 = 0x41, GS_ALPHA_1 = 0x42, GS_ALPHA_2 = 0x43, GS_DIMX = 0x44,
    GS_DTHE = 0x45, GS_COLCLAMP = 0x46, GS_TEST_1 = 0x47, GS_TEST_2 = 0x48, GS_PABE = 0x49,
    GS_FBA_1 = 0x4A, GS_FBA_2 = 0x4B, GS_FRAME_1 = 0x4C, GS_FRAME_2 = 0x4D, GS_ZBUF_1 = 0x4E,
    GS_ZBUF_2 = 0x4F, GS_BITBLTBUF = 0x50, GS_TRXPOS = 0x51, GS_TRXREG = 0x52, GS_TRXDIR = 0x53,
    GS_HWREG = 0x54, GS_SIGNAL = 0x60, GS_FINISH = 0x61, GS_LABEL = 0x62
};

#define GS_FIELD(v, lo, bits) ((uint32_t)(((v) >> (lo)) & ((1ull << (bits)) - 1)))

typedef struct GsVertex {
    int32_t x, y;           /* 12.4 window coordinates (offset removed) */
    uint32_t z;
    float s, t, q;
    uint32_t u, v;          /* 10.4 texel coordinates */
    uint8_t r, g, b, a;
    uint8_t fog;
} GsVertex;

/* everything a draw needs, captured when the primitive kicks */
typedef struct GsDraw {
    uint32_t seq;           /* changes whenever any of the rest may have: equal seq, same state */
    uint32_t prim;          /* PRIM (or PRMODE fields when PRMODECONT=0) incl. type */
    uint64_t frame, zbuf, test, alpha, tex0, tex1, clamp, scissor, xyoffset;
    uint64_t fba, texa, fogcol, dthe, colclamp, pabe, dimx;
} GsDraw;

extern uint32_t *gs_mem;    /* 4 MB local memory as 32-bit words */
/* change tracking for renderers that cache what GS memory holds: a write to
 * an 8 KB page sets gs_page_ver[page] = ++gs_ver; a CLUT load bumps gs_clut_ver */
extern uint32_t gs_page_ver[512], gs_ver, gs_clut_ver;
void gs_mark_rect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* lazy uploads (renderers that set g_gs_lazy): make GS memory current for a
 * rectangle before reading it; or take a texture straight from the upload
 * that holds exactly it (pixels as sent, row by row) */
extern int g_gs_lazy;
typedef struct GsUpload GsUpload;
void gs_sync_rect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
const GsUpload *gs_upload_for(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t *hash);
uint32_t gs_upload_pixel(const GsUpload *u, uint32_t x, uint32_t y);
/* the first n pixels of row y of an upload / of an 8- or 4-bit texture in GS
 * memory, one a byte (texture decoders: no call per pixel) */
void gs_upload_row8(const GsUpload *u, uint32_t y, uint32_t n, uint8_t *out);
void gs_read_row8(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t y, uint32_t n, uint8_t *out);
uint32_t gs_upload_ident(const GsUpload *u);   /* identity of its data and shape, not of where it went */
uint32_t gs_block_ident(uint32_t block);
/* Debugging (autopilot "trust N"): the checks that guest memory is what it was are left out, to price what write tracking
 * could save. Bits: 1 the parse cache takes a kept packet without reading its words (hle_render.c), 2 an upload source is
 * taken for the same by its kept samples and the full re-reads stop (gs.c), 4 a display list is taken for its block's
 * without the five sampled words (gs_gx.c dl_get). The picture is only right while the data does not change. */
extern int g_trust;
/* debug, off: autopilot "wskip". Bit 1: the world batches after the first of a call (g_world_rep, set by vif_defer_call_in
 * just before the call) take the first's GS state, target and GX state without looking: nothing writes a GS register or a
 * VU1 row between the batches of one call. Bit 2: the work bit 1 leaves out is done, and the runs that put anything in
 * the GX FIFO are counted (logged with the benchmark frames, "wstat 5"): none is what makes bit 1 exact. Bit 4: the
 * same count for every batch, not only the ones bit 1 would skip (the control: the first batch of a call changes state). */
extern int g_wskip, g_world_rep;
const GsUpload *gs_upload_covering(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t *hash);

void gif_path_qword(int path, const uint32_t *qw);
int gif_path_at_eop(int path);
uint32_t gif_path_image(int path, uint32_t gaddr, uint32_t n);
uint32_t gif_path_image_host(int path, const uint32_t *src, uint32_t n);
/* PATH2/PATH3 qwords in bulk (host-order words): returns the qwords taken, at least 1 */
uint32_t gif_path_bulk(int path, const uint32_t *q, uint32_t n);
extern uint32_t gs_blk_ver[];       /* per 256-byte block, like gs_page_ver */
void gif_xgkick(const uint32_t *mem, uint32_t mem_qwords, uint32_t qaddr);
void gs_write_reg(uint32_t reg, uint64_t v);
uint64_t gs_reg(uint32_t reg);

/* block order within a page (PCSX2 GSTables.cpp) */
static const uint8_t gs_block32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t gs_block16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t gs_block16s[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};

/* the 256-byte block (of 16384) holding a pixel: the gs_addr* formulas without the column part */
static inline uint32_t gs_block_of(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t b;
    switch (psm) {
    case PSMCT16: b = ((y >> 6) * bw + (x >> 6)) * 32 + gs_block16[(y >> 3) & 7][(x >> 4) & 3]; break;
    case PSMCT16S: b = ((y >> 6) * bw + (x >> 6)) * 32 + gs_block16s[(y >> 3) & 7][(x >> 4) & 3]; break;
    case PSMZ16: b = ((y >> 6) * bw + (x >> 6)) * 32 + (gs_block16[(y >> 3) & 7][(x >> 4) & 3] ^ 24u); break;
    case PSMZ16S: b = ((y >> 6) * bw + (x >> 6)) * 32 + (gs_block16s[(y >> 3) & 7][(x >> 4) & 3] ^ 24u); break;
    case PSMT8: b = ((y >> 6) * ((bw + 1) >> 1) + (x >> 7)) * 32 + gs_block32[(y >> 4) & 3][(x >> 4) & 7]; break;
    case PSMT4: b = ((y >> 7) * ((bw + 1) >> 1) + (x >> 7)) * 32 + gs_block16[(y >> 4) & 7][(x >> 5) & 3]; break;
    case PSMZ32: case PSMZ24: b = ((y >> 5) * bw + (x >> 6)) * 32 + (gs_block32[(y >> 3) & 3][(x >> 3) & 7] ^ 24u); break;
    default: b = ((y >> 5) * bw + (x >> 6)) * 32 + gs_block32[(y >> 3) & 3][(x >> 3) & 7]; break;
    }
    return (bp + b) & 16383u;
}

/* The blocks of a rectangle of one format in raster order without a switch
 * per block: gs_block_of specialised once per rectangle. Rows: gs_bw_row(w,
 * y), then gs_bw_blk(w, row, x) for each x. Block size: 1 << bws by 1 << bhs. */
typedef struct GsBlkWalk {
    const uint8_t *tab;
    uint32_t cols, cmask, rmask, bws, bhs, pws, phs, ppr, xr, bp;
} GsBlkWalk;
typedef struct GsBlkRow { const uint8_t *t; uint32_t base; } GsBlkRow;

static inline void gs_bw_init(GsBlkWalk *w, uint32_t psm, uint32_t bp, uint32_t bw)
{
    w->bp = bp;
    w->xr = (psm & 0x30) == 0x30 ? 24u : 0u;      /* Z formats */
    switch (psm) {
    case PSMCT16: case PSMZ16: case PSMCT16S: case PSMZ16S:
        w->tab = &(psm == PSMCT16S || psm == PSMZ16S ? gs_block16s : gs_block16)[0][0];
        w->cols = 4; w->cmask = 3; w->rmask = 7; w->bws = 4; w->bhs = 3; w->pws = 6; w->phs = 6; w->ppr = bw;
        break;
    case PSMT8:
        w->tab = &gs_block32[0][0];
        w->cols = 8; w->cmask = 7; w->rmask = 3; w->bws = 4; w->bhs = 4; w->pws = 7; w->phs = 6; w->ppr = (bw + 1) >> 1;
        break;
    case PSMT4:
        w->tab = &gs_block16[0][0];
        w->cols = 4; w->cmask = 3; w->rmask = 7; w->bws = 5; w->bhs = 4; w->pws = 7; w->phs = 7; w->ppr = (bw + 1) >> 1;
        break;
    default:
        w->tab = &gs_block32[0][0];
        w->cols = 8; w->cmask = 7; w->rmask = 3; w->bws = 3; w->bhs = 3; w->pws = 6; w->phs = 5; w->ppr = bw;
        break;
    }
}

static inline GsBlkRow gs_bw_row(const GsBlkWalk *w, uint32_t y)
{
    GsBlkRow r;
    r.base = w->bp + (y >> w->phs) * w->ppr * 32;
    r.t = w->tab + ((y >> w->bhs) & w->rmask) * w->cols;
    return r;
}

static inline uint32_t gs_bw_blk(const GsBlkWalk *w, GsBlkRow r, uint32_t x)
{
    return (r.base + (x >> w->pws) * 32 + (r.t[(x >> w->bws) & w->cmask] ^ w->xr)) & 16383u;
}

/* block identity (gs_block_ident) without a call when it is known */
extern uint32_t gs_blk_hash[], gs_blk_hver[];
static inline uint32_t gs_block_ident_fast(uint32_t b)
{
    return gs_blk_hver[b] == gs_blk_ver[b] + 1 ? gs_blk_hash[b] : gs_block_ident(b);
}

/* swizzled addressing: returns a word index, halfword index, byte index or nibble index */
uint32_t gs_addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
uint32_t gs_addr32z(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
uint32_t gs_addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
uint32_t gs_addr16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
uint32_t gs_addr16z(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
uint32_t gs_addr16sz(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
uint32_t gs_addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
uint32_t gs_addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);

/* pixel access by format (values in the format's natural width) */
uint32_t gs_read_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);
void gs_write_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v);

/* texture lookup helpers used by renderers */
extern uint32_t gs_clut[512];       /* CLUT buffer: 256 x 32-bit or 512 x 16-bit entries */
uint32_t gs_texel_rgba(uint64_t tex0, uint64_t texa, int x, int y);   /* -> 0xAABBGGRR */
void gs_load_clut(uint64_t tex0);
void gs_clut_sync(void);                        /* a load's copy that waits is made: before the buffer is read */
uint32_t gs_clut_hash(uint32_t base, int n);    /* of entries of the CLUT buffer */

/* rasterise a primitive into GS memory (gs_raster.c) */
void gs_raster_draw(const GsDraw *d, int type, const GsVertex *v, int n);

/* renderer backend (gs_soft.c on PC, gx on Wii) */
void gs_backend_draw(const GsDraw *d, int type, const GsVertex *v, int n);
/* A run of n vertices for one PRIM type (3 triangles, 4 strip, 5 fan, 6
 * sprites), the vertex queue's rules applied: kick[i] is set when vertex i
 * completes a primitive the GS draws -- strip v[i-2], v[i-1], v[i]; fan
 * v[0], v[i-1], v[i]; triangles and sprites the group ending at i. */
void gs_backend_prims(const GsDraw *d, int type, const GsVertex *v, int n, const uint8_t *kick);
/* A triangle strip of n vertices as the game keeps it in memory, a REGLIST of
 * ST and XYZ2 (16 bytes a vertex at q), all with the RGBAQ register's colour:
 * 1 when the backend drew it (it may keep a display list by address and
 * content), 0 to have it drawn a vertex at a time. gs_strip_vertex: vertex k
 * of it as the GS has it with the registers as they are. */
int gs_backend_strip(const uint32_t *q, uint32_t n);

/* The particle microprogram (VU1 0x46D090: sparks and their trails) without
 * VU1. A run draws every live particle of its batch (ITOP & 15 groups of 4
 * at TOP: birth times, then position and velocity each) once per trail
 * segment (913.x of them, each between two entries of the camera history at
 * 916 + 6s), as a quad of its own. nw_spark_segment: the quads of segment
 * seg, four vertices each in the microprogram's strip order (A+, A-, B+, B-),
 * as the GS gets them before the conversion to fixed point (window x and y,
 * offset included; Z; ST with Q 1). Returns the number of quads (at most
 * NS_MAX_QUADS).
 * gs_backend_sparks: 1 when the backend drew the run (first: the entry at 0,
 * which starts an emitter; what it answers there holds for the MSCNT runs
 * after it, whose state the microprogram would otherwise lack);
 * gs_backend_sparks_done: after a run the microprogram did (vi2: its output
 * buffer for the next kick), for checking. */
typedef struct NsVert {
    float x, y, z, s, t;
} NsVert;
#define NS_MAX_QUADS 44
uint32_t nw_spark_segment(const uint32_t *mem, uint32_t top, uint32_t itop, uint32_t seg, NsVert *out);
/* All of a run's quads, a particle at a time (the order does not show: they
 * are added to the picture, and write no Z): a trail point is worked out
 * once, not once for each of the two segments it ends. buf takes cap quads
 * at a time, handed to flush as they fill. Returns the number of quads. */
uint32_t nw_spark_run(const uint32_t *mem, uint32_t top, uint32_t itop, NsVert *buf, uint32_t cap,
                      void (*flush)(const NsVert *q, uint32_t n, void *ctx), void *ctx);
int gs_backend_sparks(const uint32_t *mem, uint32_t top, uint32_t itop, int first);
/* The particle systems' microprogram (VU1 0x46D750: smoke, dust, gravel, fire,
 * glass, debris) without VU1. A pool is a header at 1000..1016 and batches of
 * ITOP & 255 particles at TOP, each run by MSCNT (256 in ITOP with the pool's
 * last). nw_puff_setup: the pool's constants from the header as the game sent
 * it (0: a kind left to the microprogram). nw_puff_batch: the batch's
 * particles that are drawn, four vertices each in the microprogram's strip
 * order as the GS gets them before the conversion to fixed point (a sprite's
 * corners in that order too); returns their number (at most n).
 * gs_backend_puffs: 1 when the backend drew the batch (first: a pool's first
 * batch; what it answers there holds for the pool). gs_backend_puffs_kick, a
 * packet the microprogram kicked (g_kick_watch set), and gs_backend_puffs_done,
 * after a run it did (last: the pool's last), are for checking. */
typedef struct NpVert {
    float x, y, z, s, t;
    uint32_t rgba;              /* the GS's bytes, R in the top one; A 0x80 is 1 */
} NpVert;
typedef struct NpPool {
    uint32_t type;
    float R[4][4];              /* the rows for x, y, z and 1: into x, y, the depth for the test, w */
    float half, kL, now, nowh;  /* half the lifetime, the age where the second part begins, the time, time - half */
    float A[3];                 /* position += A age^2 */
    float C1[4], dCa[4], dCb[4];        /* the colour at age kL, its slope before and after */
    float s1, dsa, dsb;                 /* the size the same way */
    float nXn, niX, fY, iY;             /* the fades to the near and far limits */
    float athr, smin, smax;
    float Vx, Vy, Vz, o[3], Px, Py;     /* to the window; a size in pixels */
    float ex, ey, Lc[3], D, K5;         /* kind 4's shading */
    float lit[4], dlit[4];
} NpPool;
#define NP_BATCH 60
int nw_puff_setup(const uint32_t *mem, NpPool *p);
uint32_t nw_puff_batch(const uint32_t *mem, uint32_t top, uint32_t n, const NpPool *p, NpVert *out);
int gs_backend_puffs(const uint32_t *mem, uint32_t top, uint32_t itop, int first);
void gs_backend_puffs_kick(const uint32_t *mem, uint32_t qaddr);
void gs_backend_puffs_done(int last);
extern int g_kick_watch;
/* The embers' microprogram (VU1 0x47C6F0: the sparks' particles as small
 * sprites) without VU1. nw_ember_setup: an emitter's constants from the
 * header at 1008..1023 as the game sent it, at the entry at 0.
 * nw_ember_run: a batch's sprites (ITOP groups of four at TOP), four
 * vertices each as nw_puff_batch has a sprite's; returns their number.
 * gs_backend_embers: 1 when the backend drew the batch (first: the entry at
 * 0; what it answers there holds for the MSCNT runs after it);
 * gs_backend_embers_done: after a run the microprogram did, for checking. */
typedef struct NeState {
    float R[4][4];
    float sx, sy, zs, ox, oy, oz, hsx, hsy;
    float G[4], c0[4], slope[4], k[4], uv[4];
} NeState;
#define NE_GROUPS 20
int nw_ember_setup(const uint32_t *mem, NeState *e);
uint32_t nw_ember_run(const uint32_t *mem, uint32_t top, uint32_t itop, const NeState *e, const float *stale, NpVert *out);
int gs_backend_embers(const uint32_t *mem, uint32_t top, uint32_t itop, int first);
void gs_backend_embers_done(void);
/* A run of the sky microprogram (VU1 0x46A590, entry 0: one layer of the dome
 * for one view) with VU1 data memory as the game left it: 1 when the backend
 * drew it. */
int gs_backend_sky(const uint32_t *mem);
void gs_backend_sparks_done(const uint32_t *mem, uint32_t vi2);
void gs_strip_vertex(const uint32_t *q, uint32_t k, GsVertex *o);
/* the same, primitive by primitive through gs_backend_draw */
void gs_prims_each(const GsDraw *d, int type, const GsVertex *v, int n, const uint8_t *kick);
void gs_backend_flush(void);
void gs_backend_vsync(void);
/* make GS memory current for a rectangle about to be read (a CLUT) when the
 * backend draws elsewhere (render targets on the GPU) */
void gs_backend_sync(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h);

/* ---- native rendering (runtime/hle_render.c, docs/native-renderer.md) ---- */

/* Private VIFcode: native draw command, (code & 0xFFFF) payload words follow.
 * Native render functions put these into the game's DMA chains. */
#define VIF_NATIVE_CODE 0x0BB30000u
extern int g_native_render;     /* set by backends that draw natively: NATIVE_WORLD | NATIVE_CARS */
#define NATIVE_WORLD 1
#define NATIVE_CARS 2

/* a world batch's vertex arrays in guest memory (n vertices) */
typedef struct NwBlock {
    uint32_t n;
    uint32_t pos;               /* V4-32: x, y, z, w (w bit 15: no triangle ends here) */
    uint32_t uv;                /* V2-16: u, v in 4.12, or V2-32 floats (uv32) */
    uint32_t col;               /* V4-8: r, g, b, a (0x80 = 1.0), V3-8 r, g, b (rgb), or 0 */
    uint32_t lit;               /* lit pipelines: V4-8 lighting data (4 per 4 vertices), or 0 */
    uint32_t nlit;              /* its count: n rounded up to 4 */
    uint32_t nrm;               /* lit instanced props: V4-32 normals, x, y and z of vertices 4g..4g+3
                                 * in the third qwords of vertices 4g, 4g+1, 4g+2, or 0 */
    uint8_t uv32, rgb;
    uint8_t nouv;               /* no UVs (untextured): uv is pos, which GX reads for them unused */
    uint32_t start;             /* the batch's first VIFcode */
    uint32_t cl, wl, mode, srow[4];     /* VIF registers there */
    int mscnt;                  /* the packet starts the microprogram after it */
} NwBlock;
#define NW_BATCHES 192
/* a packet of world render data: its batches, and the VIF registers it leaves */
typedef struct NwPacket {
    int nb;
    NwBlock b[NW_BATCHES];
    uint32_t cl, wl, mode, row[4];
} NwPacket;
/* VIFcodes from a to end for a pipeline kind; p's registers: the VIF's on entry */
int nw_packet_parse(uint32_t a, uint32_t end, int kind, NwPacket *p);
/* the same, answered from the last parse of the packet while the words that one read are unchanged */
int nw_packet_cached(uint32_t a, uint32_t end, int kind, NwPacket *p);
/* the same, the batches left where they are (*b: valid until the next call; p gets nb and the registers) */
int nw_packet_ref(uint32_t a, uint32_t end, int kind, NwPacket *p, const NwBlock **b);

/* What a parse of render data read, for the parses remembered (runtime/hle_render.c; the cars'
 * parser, nw_car.c, reports the same way): the first PC_HALF words and the last PC_HALF, which of
 * the VIF's registers the packet set (1 CL and WL, 2 MODE, 4 ROW), which of them it used as it found
 * them, and for every batch which it still had from before. */
#define PC_HALF 24
typedef struct PcSeen {
    uint32_t n;
    uint32_t at[2 * PC_HALF], val[2 * PC_HALF];
    uint8_t set, dep;
    uint8_t inh[NW_BATCHES];
} PcSeen;
extern PcSeen *g_pc_seen;               /* NULL: nobody is asking */
#ifdef LW
static inline uint32_t pc_word(uint32_t a)
{
    const uint32_t w = LW(a);
    PcSeen *s = g_pc_seen;
    if (PS2_UNLIKELY(s != NULL)) {
        const uint32_t k = s->n < PC_HALF ? s->n : PC_HALF + s->n % PC_HALF;
        s->at[k] = a;
        s->val[k] = w;
        s->n++;
    }
    return w;
}

static inline void pc_set(uint32_t bits)
{
    if (PS2_UNLIKELY(g_pc_seen != NULL))
        g_pc_seen->set |= (uint8_t)bits;
}

static inline void pc_dep(uint32_t bits)
{
    if (PS2_UNLIKELY(g_pc_seen != NULL))
        g_pc_seen->dep |= (uint8_t)(~g_pc_seen->set & bits);
}

static inline void pc_batch(int k)
{
    if (PS2_UNLIKELY(g_pc_seen != NULL))
        g_pc_seen->inh[k] = (uint8_t)(~g_pc_seen->set & 7);
}
#endif
/* one RET-tagged block with one batch (VIFcodes in the tag, CL 3 / WL 1 / mode 0 before) */
int nw_block_parse(uint32_t block, NwBlock *b);

/* native world pipelines, by VU1 microprogram */
enum { NW_NONE, NW_PRELIT, NW_INST, NW_LIT, NW_CAR };
int nw_kind(uint32_t code);

/* run a native draw command: payload as put into the chain */
void gs_backend_native(const uint32_t *payload, int n);
/* Notes from the game side (runtime/hle.c) to the backend, in the chain's
 * order: a write of the GS's NOP register (0x7F) with GS_NOTE_MAGIC in the
 * high word. HOLD_BEGIN: no target is held from here; HOLD: the small target
 * at FRAME.FBP * 32 (bits 0-19) keeps its picture until the next HOLD_BEGIN
 * (its draws and clears are dropped); when it has none, bit `view` (bits
 * 20-23) + 8 x `player` (bits 24-27) of g_gs_hold_missed is set instead.
 * Split-screen sets up each player's views in turn, into the same targets:
 * VIEW (player in bits 24-27, FBP * 32 in bits 0-19) says the target is that
 * player's view from here on, and it shows that player's last picture (each
 * player's is kept). Its HOLD_BEGIN carries the player as well. */
#define GS_NOTE_MAGIC 0xB3A0E570u
#define GS_NOTE_HOLD_BEGIN 0x10000000u
#define GS_NOTE_HOLD 0x20000000u
#define GS_NOTE_VIEW 0x60000000u
/* The game placed a texture (runtime/hle.c, hle_tex_send): two notes, the
 * game's texture (guest address / 4), then the TBP it is at.
 * Taken by gs.c itself, not the backend. */
#define GS_NOTE_TEX_ENTRY 0x30000000u
#define GS_NOTE_TEX_AT 0x40000000u
/* the same in one note (where there is room for one only: the texture's
 * packet goes on with its palette, which is still sent): the TBP is in the
 * low 14 bits of the note's upper word, in place of the magic's */
#define GS_NOTE_TEX_HERE 0x50000000u
/* a texture the game placed at tbp and nothing has written over since: its
 * identity (what it holds and its shape, not where) */
int gs_placed_for(uint32_t tbp, uint32_t *ident);
/* write that texture to GS memory (to decode it from there) */
void gs_placed_load(uint32_t tbp);
int gs_placed_source(uint32_t tbp, uint32_t *dbw, uint32_t *dpsm, uint32_t *w, uint32_t *h, const uint32_t **data);
/* a script entry hle_tex_send may replace by notes: 1, or 2 when its packet
 * holds more after the raster; *qwords: the raster's part of the packet */
int gs_placed_entry_ok(uint32_t entry, uint32_t *qwords);
void gs_backend_note(uint32_t note);
extern uint32_t g_gs_hold_missed;
/* draw a car batch (runtime/nw_car.c) natively; 0: not handled, run VU1 */
struct NcBlock;
int gs_backend_car(const struct NcBlock *b);
/* draw a world batch of a pipeline kind natively, constants from VU1 memory */
int gs_backend_world(const NwBlock *b, int kind);
/* lit world: the per-vertex alpha the microprogram computes (nw_world.c) */
void nw_lit_alpha(const uint32_t *vu, uint32_t n, const float (*pos)[3], const int8_t (*lit)[4], uint8_t *alpha);
void nw_lit_rgba(const uint32_t *vu, const NwBlock *b, uint32_t *out);
/* the lit world's alpha where there is no highlight, by the intensity byte (nw_world.c) */
typedef struct NwLitFloor {
    uint32_t kc, kp, kamin;     /* the constants it was made for, as bits */
    uint32_t serial;            /* which table this is (0: none) */
    float x1, x1q;              /* up to here x gives the table's alpha; a little less, for tests by squares */
    uint8_t a[256];
} NwLitFloor;
const NwLitFloor *nw_lit_floor(const uint32_t *vu);
void nw_lit_rbounds(const uint32_t *vu, const NwBlock *b, float *rb);
int nw_lit_all_floor(const uint32_t *vu, const NwLitFloor *f, const float *rb, const float *bmin, const float *bmax);
void nw_lit_floor_rgba(const NwBlock *b, const NwLitFloor *f, uint32_t *out);
/* the draw state for a primitive with this PRIM, from the current registers */
void gs_capture_draw(GsDraw *d, uint32_t prim);
const GsDraw *gs_draw_state(uint32_t prim);     /* the same in place, valid until the next register write */

#endif
