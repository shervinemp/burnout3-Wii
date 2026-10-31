/*
 * Reference software renderer (PC): every primitive through the rasteriser
 * (gs_raster.c) into GS local memory, displayed from there. Used on PC to
 * validate the GS emulation; the Wii uses the GX backend.
 */
#include "gs.h"
#include "hw.h"
#include "nw_car.h"

#include <math.h>
#include <string.h>

int g_dump_every;            /* write every Nth displayed frame as a PNG (0: off) */
volatile int g_shot_request; /* write the next frame as work/frames/shot.png */
static uint32_t g_frame_no;
static uint64_t g_draws;
extern uint64_t g_raster_pixels;

static int natcar_on(void);
static int carcmp_on(void);
static void carcmp_draw(const GsDraw *d, int type, const GsVertex *v);
static int worldcmp_on(void);
static void worldcmp_draw(const GsDraw *d, int type, const GsVertex *v);

void gs_backend_draw(const GsDraw *d, int type, const GsVertex *v, int n)
{
    g_draws++;
    natcar_on();
    if (carcmp_on())
        carcmp_draw(d, type, v);
    if (worldcmp_on())
        worldcmp_draw(d, type, v);
    gs_raster_draw(d, type, v, n);
}

void gs_backend_prims(const GsDraw *d, int type, const GsVertex *v, int n, const uint8_t *kick)
{
    gs_prims_each(d, type, v, n, kick);
}

void gs_backend_flush(void) {}

void gs_backend_sync(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h)
{
    (void)bp; (void)bw; (void)psm; (void)w; (void)h;   /* drawn into GS memory already */
}

/* ---- display ------------------------------------------------------------------------- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t k;
    if (!crc_table[1]) {
        uint32_t i, j;
        for (i = 0; i < 256; i++) {
            uint32_t c = i;
            for (j = 0; j < 8; j++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    for (k = 0; k < n; k++)
        crc = crc_table[(crc ^ p[k]) & 255] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t hdr[8];
    uint32_t crc;
    put32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n)
        fwrite(data, 1, n, f);
    crc = crc32_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    crc = crc32_update(crc, data, n) ^ 0xFFFFFFFFu;
    put32(hdr, crc);
    fwrite(hdr, 1, 4, f);
}

/* minimal PNG writer: stored (uncompressed) deflate blocks */
static void write_png(const char *path, const uint32_t *rgba, int w, int h)
{
    FILE *f = fopen(path, "wb");
    uint8_t ihdr[13];
    uint8_t *raw, *z;
    size_t rawlen = (size_t)(w * 3 + 1) * (size_t)h, zlen, pos = 0, k;
    uint32_t a = 1, b = 0;
    int y, x;
    if (!f)
        return;
    raw = malloc(rawlen);
    for (y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (size_t)(w * 3 + 1);
        row[0] = 0;
        for (x = 0; x < w; x++) {
            uint32_t p = rgba[y * w + x];
            row[1 + 3 * x] = (uint8_t)p;
            row[2 + 3 * x] = (uint8_t)(p >> 8);
            row[3 + 3 * x] = (uint8_t)(p >> 16);
        }
    }
    zlen = 2 + rawlen + 5 * (rawlen / 65535 + 1) + 4;
    z = malloc(zlen);
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    for (k = 0; k < rawlen; k += 65535) {
        size_t n = rawlen - k > 65535 ? 65535 : rawlen - k;
        z[pos++] = k + n >= rawlen ? 1 : 0;
        z[pos++] = (uint8_t)n;
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n;
        z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + k, n);
        pos += n;
    }
    for (k = 0; k < rawlen; k++) {
        a = (a + raw[k]) % 65521;
        b = (b + a) % 65521;
    }
    put32(z + pos, (b << 16) | a);
    pos += 4;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)pos);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

/* status line every 600 fields: game frames per second (VIF1 chains) and
 * where the host time went */
static void soft_status(void)
{
    static uint32_t vsyncs, last_chains;
    static uint64_t last_us;
    if (++vsyncs % 600 == 0) {
        uint64_t now = plat_time_us();
        uint32_t frames = g_vif1_chains - last_chains;
        char pt[160];
        pt_report(pt, sizeof pt);
        LOG("gs: %.1f frames/s over %.1f s; %s", frames * 1e6 / (double)(now - last_us), (now - last_us) / 1e6, pt);
        last_chains = g_vif1_chains;
        last_us = now;
    }
}

void gs_backend_vsync(void)
{
    uint64_t pmode = gs_priv_reg(GS_PMODE);
    int circ = (pmode & 2) ? 2 : (pmode & 1) ? 1 : 0;
    uint64_t dispfb, display;
    uint32_t fbp, fbw, psm, dbx, dby, dw, dh, magh, magv;
    int w, h, x, y;
    static uint32_t *buf;
    char path[256];
    int shot = g_shot_request;
    soft_status();
    g_frame_no++;
    if (!shot && (!g_dump_every || g_frame_no % (uint32_t)g_dump_every))
        return;
    if (!circ)
        return;
    dispfb = gs_priv_reg(circ == 2 ? GS_DISPFB2 : GS_DISPFB1);
    display = gs_priv_reg(circ == 2 ? GS_DISPLAY2 : GS_DISPLAY1);
    fbp = GS_FIELD(dispfb, 0, 9) * 32;
    fbw = GS_FIELD(dispfb, 9, 6);
    psm = GS_FIELD(dispfb, 15, 5);
    dbx = GS_FIELD(dispfb, 32, 11);
    dby = GS_FIELD(dispfb, 43, 11);
    magh = GS_FIELD(display, 23, 4) + 1;
    magv = GS_FIELD(display, 27, 2) + 1;
    dw = GS_FIELD(display, 32, 12) + 1;
    dh = GS_FIELD(display, 44, 11) + 1;
    w = (int)(dw / magh);
    h = (int)(dh / magv);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
        return;
    if (!buf)
        buf = malloc(1024 * 1024 * 4);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            uint32_t v = gs_read_pixel(psm == 2 || psm == 10 ? psm : PSMCT32, fbp, fbw, dbx + (uint32_t)x, dby + (uint32_t)y);
            if (psm == 2 || psm == 10)
                v = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19);
            buf[y * w + x] = v;
        }
    if (shot) {
        g_shot_request = 0;
        snprintf(path, sizeof path, "work/frames/shot.png");
    } else {
        snprintf(path, sizeof path, "work/frames/f%05u.png", g_frame_no);
    }
    write_png(path, buf, w, h);
    LOG("frame %u: %dx%d from fbp %X psm %u, %llu draws %llu pixels so far", g_frame_no, w, h, fbp, psm,
        (unsigned long long)g_draws, (unsigned long long)g_raster_pixels);
}

void gs_backend_note(uint32_t note)
{
    (void)note;
}

/* native draw commands are only made for backends that set g_native_render */
void gs_backend_native(const uint32_t *payload, int n)
{
    (void)payload;
    (void)n;
}

/* Debugging (B3_CARCMP=1): every car batch shaded natively (nw_car.c) and,
 * as usual, by VU1; the GS triangles of the two compared when the next VU1
 * run starts. Batches that differ are logged. */
static struct {
    int on, pending, runs;
    uint32_t entry;
    uint32_t n, nat_tris, emu_tris;
    int32_t nat[NC_MAX * 3][3];         /* triangle vertices: x, y (12.4), z */
    int32_t emu[NC_MAX * 3][3];
    uint32_t flags, batches, bad;
    uint32_t nat_skin, nat_pos, nat_neg;
    uint32_t sum_vu, sum_pos, sum_neg, sum_all;     /* over batches the VU1 drew any of */
    uint32_t zlogged;
} g_cc;

static void carcmp_finish(void)
{
    uint32_t k, c, worst = 0, at = 0, m;
    if (!g_cc.pending)
        return;
    g_cc.pending = 0;
    g_cc.batches++;
    m = g_cc.nat_tris < g_cc.emu_tris ? g_cc.nat_tris : g_cc.emu_tris;
    for (k = 0; k < m * 3; k++)
        for (c = 0; c < 2; c++) {
            uint32_t e = (uint32_t)abs(g_cc.nat[k][c] - g_cc.emu[k][c]);
            if (e > worst) {
                worst = e;
                at = k;
            }
        }
    if (g_cc.emu_tris) {
        g_cc.sum_vu += g_cc.emu_tris;
        g_cc.sum_pos += g_cc.nat_pos;
        g_cc.sum_neg += g_cc.nat_neg;
        g_cc.sum_all += g_cc.nat_tris;
    }
    if (g_cc.batches % 500 == 0)
        LOG("carcmp: totals over drawn batches: VU1 %u tris; native %u (%u positive, %u negative winding)", g_cc.sum_vu,
            g_cc.sum_all, g_cc.sum_pos, g_cc.sum_neg);
    if (g_cc.nat_tris == g_cc.emu_tris && g_cc.zlogged < 12) {
        g_cc.zlogged++;
        LOG("carcmp: z check batch %u: native z %d %d %d, VU1 z %d %d %d (x %.1f/%.1f)", g_cc.batches, g_cc.nat[0][2],
            g_cc.nat[1][2], g_cc.nat[2][2], g_cc.emu[0][2], g_cc.emu[1][2], g_cc.emu[2][2], g_cc.nat[0][0] / 16.0,
            g_cc.emu[0][0] / 16.0);
    }
    if (g_cc.nat_tris == g_cc.emu_tris && worst <= 32)
        return;
    if (++g_cc.bad <= 60)
        LOG("carcmp: batch %u (n %u, 1013 %08X, skin %u, entry %04X): native %u tris, VU1 %u; worst %.1f px at vertex "
            "%u: native %.1f,%.1f VU1 %.1f,%.1f; native windings %u+ %u-", g_cc.batches, g_cc.n, g_cc.flags,
            g_cc.nat_skin, g_cc.entry, g_cc.nat_tris, g_cc.emu_tris, worst / 16.0, at, g_cc.nat[at][0] / 16.0, g_cc.nat[at][1] / 16.0,
            g_cc.emu[at][0] / 16.0, g_cc.emu[at][1] / 16.0, g_cc.nat_pos, g_cc.nat_neg);
    if (g_cc.batches % 2000 == 0)
        LOG("carcmp: %u batches, %u differ", g_cc.batches, g_cc.bad);
}

static void carcmp_vu1_run(uint32_t addr)
{
    extern uint32_t g_vu1_code;
    if (g_vu1_code != NC_CODE)
        return;
    if (g_cc.pending && g_cc.runs++ >= 1)
        carcmp_finish();
    else if (g_cc.pending)
        g_cc.entry = addr;
}

static void carcmp_draw(const GsDraw *d, int type, const GsVertex *v)
{
    extern uint32_t g_vu1_code;
    int k;
    if (!g_cc.pending || g_vu1_code != NC_CODE || (d->prim & 0x200) || type < 3 || type > 5 ||
        g_cc.emu_tris >= NC_MAX)
        return;
    for (k = 0; k < 3; k++) {
        g_cc.emu[g_cc.emu_tris * 3 + k][0] = v[k].x;
        g_cc.emu[g_cc.emu_tris * 3 + k][1] = v[k].y;
        g_cc.emu[g_cc.emu_tris * 3 + k][2] = (int32_t)v[k].z;
    }
    g_cc.emu_tris++;
}

static int carcmp_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("B3_CARCMP");
        extern void (*g_vu1_run_hook)(uint32_t addr);
        on = e && *e && *e != '0';
        if (on) {
            g_native_render |= NATIVE_CARS;
            g_vu1_run_hook = carcmp_vu1_run;
        }
    }
    return on;
}

/* the native side of a batch: its triangles as the GX backend would draw them */
static void carcmp_batch(const struct NcBlock *b)
{
    static NcIn in;
    static NcOut out;
    uint32_t *vm = g_vu_mem + 0xC000 / 4;
    const float *q = (const float *)vm;
    float m[3][4], ofx, ofy;
    static int32_t xy[NC_MAX][3];
    uint32_t i, k, c;
    GsDraw d;
    carcmp_finish();
    nc_load(b, &in);
    g_cc.flags = vm[1013 * 4 + 2] << 16 | (vm[1013 * 4 + 1] & 0xFFFF);
    g_cc.nat_skin = vm[1013 * 4 + 2] != 0;
    if (!nc_shade(vm, &in, &out))
        return;
    nc_matrix(vm, m);
    gs_capture_draw(&d, 0x05C);
    ofx = (float)GS_FIELD(d.xyoffset, 0, 16) / 16.0f;
    ofy = (float)GS_FIELD(d.xyoffset, 32, 16) / 16.0f;
#define Q(n, c) q[(n) * 4 + (c)]
    for (k = 0; k < out.n; k++) {
        const float *p = out.pos[k];
        float xc = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
        float yc = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
        float w = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3], iw = 1.0f / w;
        xy[k][0] = (int32_t)((Q(1019, 0) - ofx - 0.5f * Q(1018, 0) + Q(1018, 0) * xc * iw) * 16.0f);
        xy[k][1] = (int32_t)((Q(1019, 1) - ofy - 0.5f * Q(1018, 1) + Q(1018, 1) * yc * iw) * 16.0f);
        xy[k][2] = (int32_t)(16.0f * (Q(1019, 2) + Q(1018, 2) * iw));
    }
#undef Q
    g_cc.n = out.n;
    g_cc.nat_tris = g_cc.emu_tris = g_cc.nat_pos = g_cc.nat_neg = 0;
    for (i = 2; i < out.n; i++)
        if (!out.adc[i]) {
            double ax = xy[i - 1][0] - xy[i - 2][0], ay = xy[i - 1][1] - xy[i - 2][1];
            double bx = xy[i][0] - xy[i - 2][0], by = xy[i][1] - xy[i - 2][1], cr = ax * by - ay * bx;
            if (i & 1)
                cr = -cr;
            if (cr > 0)
                g_cc.nat_pos++;
            else
                g_cc.nat_neg++;
            for (k = 0; k < 3; k++)
                for (c = 0; c < 3; c++)
                    g_cc.nat[g_cc.nat_tris * 3 + k][c] = xy[i - 2 + k][c];
            g_cc.nat_tris++;
        }
    g_cc.pending = 1;
    g_cc.runs = 0;
}

/* Debugging (B3_NATCAR=1): cars through the native shader (nw_car.c) with
 * the GX backend's transform, rasterised here, to tell shader problems from
 * GX ones. */
static int natcar_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("B3_NATCAR");
        on = e && *e && *e != '0';
        if (on)
            g_native_render |= NATIVE_CARS;
    }
    return on;
}

int gs_backend_sparks(const uint32_t *mem, uint32_t top, uint32_t itop, int first)
{
    (void)mem;
    (void)top;
    (void)itop;
    (void)first;
    return 0;
}

int gs_backend_puffs(const uint32_t *mem, uint32_t top, uint32_t itop, int first)
{
    (void)mem;
    (void)top;
    (void)itop;
    (void)first;
    return 0;
}

void gs_backend_puffs_kick(const uint32_t *mem, uint32_t qaddr)
{
    (void)mem;
    (void)qaddr;
}

void gs_backend_puffs_done(int last)
{
    (void)last;
}

int gs_backend_embers(const uint32_t *mem, uint32_t top, uint32_t itop, int first)
{
    (void)mem;
    (void)top;
    (void)itop;
    (void)first;
    return 0;
}

void gs_backend_embers_done(void)
{
}

int gs_backend_sky(const uint32_t *mem)
{
    (void)mem;
    return 0;
}

void gs_backend_sparks_done(const uint32_t *mem, uint32_t vi2)
{
    (void)mem;
    (void)vi2;
}

int gs_backend_strip(const uint32_t *q, uint32_t n)
{
    (void)q;
    (void)n;
    return 0;
}

int gs_backend_car(const struct NcBlock *b)
{
    static NcIn in;
    static NcOut out;
    uint32_t *vm = g_vu_mem + 0xC000 / 4;
    const float *q = (const float *)vm;
    float m[3][4];
    int pass;
    uint32_t i, k;
    if (carcmp_on()) {
        carcmp_batch(b);
        return 0;
    }
    if (!natcar_on())
        return 0;
    nc_load(b, &in);
    if (!nc_shade(vm, &in, &out))
        return 0;
    if (vm[1006 * 4] & 0xFFFF) {
        uint32_t pk[16];
        memcpy(pk, &vm[1006 * 4], sizeof pk);
        pk[0] |= 0x8000;
        gif_xgkick(pk, 4, 0);
        vm[1006 * 4] = 0;
    }
    nc_matrix(vm, m);
#define Q(n, c) q[(n) * 4 + (c)]
    for (pass = 0; pass < (out.env ? 2 : 1); pass++) {
        static GsVertex gv[NC_MAX];
        GsDraw d;
        float ofx, ofy;
        gs_capture_draw(&d, pass ? 0x25C : 0x05C);
        ofx = (float)GS_FIELD(d.xyoffset, 0, 16) / 16.0f;
        ofy = (float)GS_FIELD(d.xyoffset, 32, 16) / 16.0f;
        for (k = 0; k < out.n; k++) {
            const float *p = out.pos[k];
            float xc = m[0][0] * p[0] + m[0][1] * p[1] + m[0][2] * p[2] + m[0][3];
            float yc = m[1][0] * p[0] + m[1][1] * p[1] + m[1][2] * p[2] + m[1][3];
            float w = m[2][0] * p[0] + m[2][1] * p[1] + m[2][2] * p[2] + m[2][3], iw = 1.0f / w;
            GsVertex *g = &gv[k];
            memset(g, 0, sizeof *g);
            g->x = (int32_t)((Q(1019, 0) - ofx - 0.5f * Q(1018, 0) + Q(1018, 0) * xc * iw) * 16.0f);
            g->y = (int32_t)((Q(1019, 1) - ofy - 0.5f * Q(1018, 1) + Q(1018, 1) * yc * iw) * 16.0f);
            g->z = (uint32_t)(16.0f * (Q(1019, 2) + Q(1018, 2) * iw));
            g->q = iw;
            if (pass) {
                g->s = out.uv1[k][0] * iw; g->t = out.uv1[k][1] * iw;
                g->r = out.col1[0]; g->g = out.col1[1]; g->b = out.col1[2]; g->a = out.col1[3];
            } else {
                g->s = out.uv0[k][0] * iw; g->t = out.uv0[k][1] * iw;
                g->r = out.col[k][0]; g->g = out.col[k][1]; g->b = out.col[k][2]; g->a = out.col[k][3];
            }
        }
        for (i = 2; i < out.n; i++)
            if (!out.adc[i]) {
                GsVertex t[3] = {gv[i - 2], gv[i - 1], gv[i]};
                gs_raster_draw(&d, 3, t, 3);
            }
    }
#undef Q
    return 1;
}

/* Debugging (B3_WORLDCMP=1): world batches transformed as the GX backend
 * does (camera 1014..1017, screen 1018/1019, Z x16) next to what VU1 kicks;
 * statistics by pipeline and clip flag (1013.x) every 300 batches. */
#define WC_MAX 256
static struct {
    int pending, runs, kind, clip;
    uint32_t n, nat_tris, emu_tris;
    int32_t nat[WC_MAX * 3][3], emu[WC_MAX * 3][3];
    /* [clip]: batches, triangle counts equal, vertices compared, xy within 1 px, z ratio sum */
    uint32_t batches[2], same_count[2], verts[2], xy_ok[2];
    double zratio[2];
    uint32_t total;
} g_wc;

static void worldcmp_finish(void)
{
    uint32_t k, c, m;
    int cl = g_wc.clip;
    if (!g_wc.pending)
        return;
    g_wc.pending = 0;
    g_wc.batches[cl]++;
    if (g_wc.nat_tris == g_wc.emu_tris) {
        g_wc.same_count[cl]++;
        m = g_wc.nat_tris * 3;
        for (k = 0; k < m; k++) {
            int ok = 1;
            for (c = 0; c < 2; c++)
                if (abs(g_wc.nat[k][c] - g_wc.emu[k][c]) > 16)
                    ok = 0;
            g_wc.verts[cl]++;
            g_wc.xy_ok[cl] += ok;
            if (ok && g_wc.nat[k][2] > 0)
                g_wc.zratio[cl] += (double)g_wc.emu[k][2] / (double)g_wc.nat[k][2];
        }
    } else if (cl && g_wc.batches[1] < 12) {
        LOG("worldcmp: clip batch kind %d n %u: native %u tris, VU1 %u; first native %.1f,%.1f z %d, VU1 %.1f,%.1f z %d",
            g_wc.kind, g_wc.n, g_wc.nat_tris, g_wc.emu_tris, g_wc.nat[0][0] / 16.0, g_wc.nat[0][1] / 16.0,
            g_wc.nat[0][2], g_wc.emu[0][0] / 16.0, g_wc.emu[0][1] / 16.0, g_wc.emu[0][2]);
    }
    if (++g_wc.total % 300 == 0)
        for (cl = 0; cl < 2; cl++)
            LOG("worldcmp: clip %d: %u batches, %u same triangle count; %u vertices, %u within 1 px, z VU1/native %.4f",
                cl, g_wc.batches[cl], g_wc.same_count[cl], g_wc.verts[cl], g_wc.xy_ok[cl],
                g_wc.xy_ok[cl] ? g_wc.zratio[cl] / g_wc.xy_ok[cl] : 0.0);
}

static void worldcmp_vu1_run(uint32_t addr)
{
    extern uint32_t g_vu1_code;
    (void)addr;
    if (!nw_kind(g_vu1_code))
        return;
    if (g_wc.pending && g_wc.runs++ >= 1)
        worldcmp_finish();
}

static void worldcmp_draw(const GsDraw *d, int type, const GsVertex *v)
{
    extern uint32_t g_vu1_code;
    int k;
    if (!g_wc.pending || !nw_kind(g_vu1_code) || type < 3 || type > 5 || g_wc.emu_tris >= WC_MAX)
        return;
    (void)d;
    for (k = 0; k < 3; k++) {
        g_wc.emu[g_wc.emu_tris * 3 + k][0] = v[k].x;
        g_wc.emu[g_wc.emu_tris * 3 + k][1] = v[k].y;
        g_wc.emu[g_wc.emu_tris * 3 + k][2] = (int32_t)v[k].z;
    }
    g_wc.emu_tris++;
}

static int worldcmp_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("B3_WORLDCMP");
        extern void (*g_vu1_run_hook)(uint32_t addr);
        on = e && *e && *e != '0';
        if (on) {
            g_native_render |= NATIVE_WORLD;
            g_vu1_run_hook = worldcmp_vu1_run;
        }
    }
    return on;
}

static void worldcmp_batch(const NwBlock *b, int kind)
{
    uint32_t *vm = g_vu_mem + 0xC000 / 4;
    const float *q = (const float *)vm;
    static int32_t xy[WC_MAX][3];
    float ofx, ofy;
    uint32_t i, k, c;
    GsDraw d;
    worldcmp_finish();
    if (kind == NW_INST || b->n > WC_MAX)
        return;
    gs_capture_draw(&d, 0x05C);
    ofx = (float)GS_FIELD(d.xyoffset, 0, 16) / 16.0f;
    ofy = (float)GS_FIELD(d.xyoffset, 32, 16) / 16.0f;
#define Q(n, c) q[(n) * 4 + (c)]
    for (k = 0; k < b->n; k++) {
        float p[3], xc, yc, w, iw;
        for (c = 0; c < 3; c++)
            p[c] = LWF(b->pos + 16 * k + 4 * c);
        xc = Q(1014, 0) * p[0] + Q(1015, 0) * p[1] + Q(1016, 0) * p[2] + Q(1017, 0);
        yc = Q(1014, 1) * p[0] + Q(1015, 1) * p[1] + Q(1016, 1) * p[2] + Q(1017, 1);
        w = Q(1014, 3) * p[0] + Q(1015, 3) * p[1] + Q(1016, 3) * p[2] + Q(1017, 3);
        iw = 1.0f / w;
        xy[k][0] = (int32_t)((Q(1019, 0) - ofx - 0.5f * Q(1018, 0) + Q(1018, 0) * xc * iw) * 16.0f);
        xy[k][1] = (int32_t)((Q(1019, 1) - ofy - 0.5f * Q(1018, 1) + Q(1018, 1) * yc * iw) * 16.0f);
        xy[k][2] = (int32_t)(16.0f * (Q(1019, 2) + Q(1018, 2) * iw));
    }
#undef Q
    g_wc.kind = kind;
    g_wc.clip = (vm[1013 * 4] & 0xFFFF) != 0;
    g_wc.n = b->n;
    g_wc.nat_tris = g_wc.emu_tris = 0;
    for (i = 2; i < b->n; i++)
        if (!(LW(b->pos + 16 * i + 12) & 0x8000)) {
            for (k = 0; k < 3; k++)
                for (c = 0; c < 3; c++)
                    g_wc.nat[g_wc.nat_tris * 3 + k][c] = xy[i - 2 + k][c];
            g_wc.nat_tris++;
        }
    g_wc.pending = 1;
    g_wc.runs = 0;
}

int gs_backend_world(const NwBlock *b, int kind)
{
    if (worldcmp_on())
        worldcmp_batch(b, kind);
    return 0;
}
