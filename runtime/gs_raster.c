/*
 * GS rasteriser: draws primitives straight into GS local memory, so
 * render-to-texture, frame buffer reads and display work the way the
 * hardware does. The PC backend (gs_soft.c) draws everything with it; the
 * Wii's GX backend uses it for small targets whose content the CPU reads
 * back (palettes, lookup textures).
 */
#include "gs.h"
#include "hw.h"

#include <math.h>
#include <string.h>

uint64_t g_raster_pixels;       /* pixels written so far */

typedef struct Pix {
    int r, g, b, a;
    uint32_t z;
    float s, t, q;
    float u, v;
    int fog;
} Pix;

/* ---- per-draw state ------------------------------------------------------------------- */

typedef struct Ctx {
    const GsDraw *d;
    uint32_t fbp, fbw, fpsm, fbmsk;
    uint32_t zbp, zpsm, zmsk;
    int ate, atst, aref, afail, date, datm, zte, ztst;
    int abe, a, b, c, dd, fix;
    int tme, fst, iip, fge;
    int tw, th, tcc, tfx;
    int wms, wmt, minu, maxu, minv, maxv;
    int scx0, scx1, scy0, scy1;
    int fba, colclamp, pabe;
    int fogr, fogg, fogb;
    int linear;
} Ctx;

static void setup(Ctx *c, const GsDraw *d)
{
    c->d = d;
    c->fbp = GS_FIELD(d->frame, 0, 9) * 32;
    c->fbw = GS_FIELD(d->frame, 16, 6);
    c->fpsm = GS_FIELD(d->frame, 24, 6);
    c->fbmsk = GS_FIELD(d->frame, 32, 32);
    c->zbp = GS_FIELD(d->zbuf, 0, 9) * 32;
    c->zpsm = GS_FIELD(d->zbuf, 24, 4) | 0x30;
    c->zmsk = GS_FIELD(d->zbuf, 32, 1);
    c->ate = GS_FIELD(d->test, 0, 1);
    c->atst = GS_FIELD(d->test, 1, 3);
    c->aref = GS_FIELD(d->test, 4, 8);
    c->afail = GS_FIELD(d->test, 12, 2);
    c->date = GS_FIELD(d->test, 14, 1);
    c->datm = GS_FIELD(d->test, 15, 1);
    c->zte = GS_FIELD(d->test, 16, 1);
    c->ztst = GS_FIELD(d->test, 17, 2);
    c->iip = (d->prim >> 3) & 1;
    c->tme = (d->prim >> 4) & 1;
    c->fge = (d->prim >> 5) & 1;
    c->abe = ((d->prim >> 6) & 1) | ((d->prim >> 7) & 1);
    c->fst = (d->prim >> 8) & 1;
    c->a = GS_FIELD(d->alpha, 0, 2);
    c->b = GS_FIELD(d->alpha, 2, 2);
    c->c = GS_FIELD(d->alpha, 4, 2);
    c->dd = GS_FIELD(d->alpha, 6, 2);
    c->fix = GS_FIELD(d->alpha, 32, 8);
    c->tw = 1 << GS_FIELD(d->tex0, 26, 4);
    c->th = 1 << GS_FIELD(d->tex0, 30, 4);
    if (c->tw > 1024) c->tw = 1024;
    if (c->th > 1024) c->th = 1024;
    c->tcc = GS_FIELD(d->tex0, 34, 1);
    c->tfx = GS_FIELD(d->tex0, 35, 2);
    c->wms = GS_FIELD(d->clamp, 0, 2);
    c->wmt = GS_FIELD(d->clamp, 2, 2);
    c->minu = GS_FIELD(d->clamp, 4, 10);
    c->maxu = GS_FIELD(d->clamp, 14, 10);
    c->minv = GS_FIELD(d->clamp, 24, 10);
    c->maxv = GS_FIELD(d->clamp, 34, 10);
    c->scx0 = GS_FIELD(d->scissor, 0, 11);
    c->scx1 = GS_FIELD(d->scissor, 16, 11);
    c->scy0 = GS_FIELD(d->scissor, 32, 11);
    c->scy1 = GS_FIELD(d->scissor, 48, 11);
    c->fba = GS_FIELD(d->fba, 0, 1);
    c->colclamp = GS_FIELD(d->colclamp, 0, 1);
    c->pabe = GS_FIELD(d->pabe, 0, 1);
    c->fogr = GS_FIELD(d->fogcol, 0, 8);
    c->fogg = GS_FIELD(d->fogcol, 8, 8);
    c->fogb = GS_FIELD(d->fogcol, 16, 8);
    c->linear = GS_FIELD(d->tex1, 5, 1);
}

static int wrap(int x, int size, int mode, int mn, int mx)
{
    switch (mode) {
    case 0: return x & (size - 1);
    case 1: return x < 0 ? 0 : x >= size ? size - 1 : x;
    case 2: return x < mn ? mn : x > mx ? mx : x;
    default: return (x & mn) | mx;
    }
}

static uint32_t sample(const Ctx *c, float u, float v)
{
    int iu, iv;
    if (!c->linear) {
        iu = (int)floorf(u);
        iv = (int)floorf(v);
        iu = wrap(iu, c->tw, c->wms, c->minu, c->maxu);
        iv = wrap(iv, c->th, c->wmt, c->minv, c->maxv);
        return gs_texel_rgba(c->d->tex0, c->d->texa, iu, iv);
    } else {
        float fu = u - 0.5f, fv = v - 0.5f;
        int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
        float au = fu - (float)u0, av = fv - (float)v0;
        uint32_t t[4];
        int k, out = 0;
        int x0 = wrap(u0, c->tw, c->wms, c->minu, c->maxu), x1 = wrap(u0 + 1, c->tw, c->wms, c->minu, c->maxu);
        int y0 = wrap(v0, c->th, c->wmt, c->minv, c->maxv), y1 = wrap(v0 + 1, c->th, c->wmt, c->minv, c->maxv);
        t[0] = gs_texel_rgba(c->d->tex0, c->d->texa, x0, y0);
        t[1] = gs_texel_rgba(c->d->tex0, c->d->texa, x1, y0);
        t[2] = gs_texel_rgba(c->d->tex0, c->d->texa, x0, y1);
        t[3] = gs_texel_rgba(c->d->tex0, c->d->texa, x1, y1);
        for (k = 0; k < 4; k++) {
            float a = (float)((t[0] >> (8 * k)) & 255), b = (float)((t[1] >> (8 * k)) & 255);
            float cc = (float)((t[2] >> (8 * k)) & 255), dd = (float)((t[3] >> (8 * k)) & 255);
            float top = a + (b - a) * au, bot = cc + (dd - cc) * au;
            int r = (int)(top + (bot - top) * av + 0.5f);
            out |= (r & 255) << (8 * k);
        }
        return (uint32_t)out;
    }
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static uint32_t read_fb(const Ctx *c, int x, int y)
{
    uint32_t v = gs_read_pixel(c->fpsm, c->fbp, c->fbw, (uint32_t)x, (uint32_t)y);
    if (c->fpsm == PSMCT16 || c->fpsm == PSMCT16S)
        return ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) | ((v & 0x8000) ? 0x80000000u : 0);
    if (c->fpsm == PSMCT24)
        return v | 0x80000000u;
    return v;
}

/* Sound meanwhile (runtime/iop_sd.c): a picture drawn here pixel by pixel while a track loads
 * takes a third of a second, and the game cannot tick the sound chip from inside it. */
void iop_sd_pump(void);

static void plot(const Ctx *c, int x, int y, Pix *p)
{
    int r = p->r, g = p->g, b = p->b, a = p->a;
    int write_fb = 1, write_z = c->zmsk == 0, write_alpha = 1;
    uint32_t dst = 0;
    int have_dst = 0;
    if (x < c->scx0 || x > c->scx1 || y < c->scy0 || y > c->scy1)
        return;
    if (c->tme) {
        float u, v;
        uint32_t t;
        int tr, tg, tb, ta;
        if (c->fst) {
            u = p->u;
            v = p->v;
        } else {
            float q = p->q != 0.0f ? p->q : 1e-6f;
            u = p->s / q * (float)c->tw;
            v = p->t / q * (float)c->th;
        }
        t = sample(c, u, v);
        tr = t & 255; tg = (t >> 8) & 255; tb = (t >> 16) & 255; ta = t >> 24;
        switch (c->tfx) {
        case 0:
            r = clamp255((tr * r) >> 7); g = clamp255((tg * g) >> 7); b = clamp255((tb * b) >> 7);
            if (c->tcc) a = clamp255((ta * a) >> 7);
            break;
        case 1:
            r = tr; g = tg; b = tb;
            if (c->tcc) a = ta;
            break;
        case 2:
            r = clamp255(((tr * r) >> 7) + a); g = clamp255(((tg * g) >> 7) + a); b = clamp255(((tb * b) >> 7) + a);
            if (c->tcc) a = clamp255(ta + a);
            break;
        default:
            r = clamp255(((tr * r) >> 7) + a); g = clamp255(((tg * g) >> 7) + a); b = clamp255(((tb * b) >> 7) + a);
            if (c->tcc) a = ta;
            break;
        }
    }
    if (c->fge) {
        int f = p->fog;
        r = (f * r + (255 - f) * c->fogr) >> 8;
        g = (f * g + (255 - f) * c->fogg) >> 8;
        b = (f * b + (255 - f) * c->fogb) >> 8;
    }
    if (c->ate) {
        int pass;
        switch (c->atst) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = a < c->aref; break;
        case 3: pass = a <= c->aref; break;
        case 4: pass = a == c->aref; break;
        case 5: pass = a >= c->aref; break;
        case 6: pass = a > c->aref; break;
        default: pass = a != c->aref; break;
        }
        if (!pass) {
            switch (c->afail) {
            case 0: return;
            case 1: write_z = 0; break;
            case 2: write_fb = 0; break;
            default: write_z = 0; write_alpha = 0; break;
            }
        }
    }
    if (c->date && (c->fpsm == PSMCT32 || c->fpsm == PSMCT16 || c->fpsm == PSMCT16S)) {
        dst = read_fb(c, x, y);
        have_dst = 1;
        if (((dst >> 31) & 1) != (uint32_t)c->datm)
            return;
    }
    if (c->zte) {
        uint32_t zd;
        int pass;
        if (c->ztst == 0)
            return;
        zd = gs_read_pixel(c->zpsm, c->zbp, c->fbw, (uint32_t)x, (uint32_t)y);
        switch (c->ztst) {
        case 1: pass = 1; break;
        case 2: pass = p->z >= zd; break;
        default: pass = p->z > zd; break;
        }
        if (!pass)
            return;
    }
    if (write_z && c->zte) {
        uint32_t z = p->z;
        if (c->zpsm == PSMZ24 && z > 0xFFFFFF) z = 0xFFFFFF;
        if ((c->zpsm == PSMZ16 || c->zpsm == PSMZ16S) && z > 0xFFFF) z = 0xFFFF;
        gs_write_pixel(c->zpsm, c->zbp, c->fbw, (uint32_t)x, (uint32_t)y, z);
    }
    if (!write_fb)
        return;
    if (c->abe && (!c->pabe || (a & 0x80))) {
        int cs[3] = {r, g, b}, cd[3], out[3], k;
        int as = a, ad, cc;
        if (!have_dst)
            dst = read_fb(c, x, y);
        cd[0] = dst & 255; cd[1] = (dst >> 8) & 255; cd[2] = (dst >> 16) & 255;
        ad = (int)(dst >> 24);
        cc = c->c == 0 ? as : c->c == 1 ? ad : c->fix;
        for (k = 0; k < 3; k++) {
            int va = c->a == 0 ? cs[k] : c->a == 1 ? cd[k] : 0;
            int vb = c->b == 0 ? cs[k] : c->b == 1 ? cd[k] : 0;
            int vd = c->dd == 0 ? cs[k] : c->dd == 1 ? cd[k] : 0;
            int v = (((va - vb) * cc) >> 7) + vd;
            out[k] = c->colclamp ? clamp255(v) : (v & 255);
        }
        r = out[0]; g = out[1]; b = out[2];
    }
    {
        uint32_t px;
        if (c->fba)
            a |= 0x80;
        if (c->fpsm == PSMCT16 || c->fpsm == PSMCT16S) {
            px = (uint32_t)(r >> 3) | (uint32_t)(g >> 3) << 5 | (uint32_t)(b >> 3) << 10 | (a & 0x80 ? 0x8000u : 0);
            if (!write_alpha) {
                uint32_t old = gs_read_pixel(c->fpsm, c->fbp, c->fbw, (uint32_t)x, (uint32_t)y);
                px = (px & 0x7FFF) | (old & 0x8000);
            }
            if (c->fbmsk) {
                uint32_t m = ((c->fbmsk >> 3) & 0x1F) | ((c->fbmsk >> 6) & 0x3E0) | ((c->fbmsk >> 9) & 0x7C00) | ((c->fbmsk >> 16) & 0x8000);
                uint32_t old = gs_read_pixel(c->fpsm, c->fbp, c->fbw, (uint32_t)x, (uint32_t)y);
                px = (px & ~m) | (old & m);
            }
        } else {
            px = (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24;
            if (!write_alpha || c->fpsm == PSMCT24) {
                uint32_t old = gs_read_pixel(PSMCT32, c->fbp, c->fbw, (uint32_t)x, (uint32_t)y);
                px = (px & 0xFFFFFF) | (old & 0xFF000000u);
            }
            if (c->fbmsk) {
                uint32_t old = gs_read_pixel(PSMCT32, c->fbp, c->fbw, (uint32_t)x, (uint32_t)y);
                px = (px & ~c->fbmsk) | (old & c->fbmsk);
            }
        }
        gs_write_pixel(c->fpsm == PSMCT24 ? PSMCT32 : c->fpsm, c->fbp, c->fbw, (uint32_t)x, (uint32_t)y, px);
        g_raster_pixels++;
    }
}

/* ---- primitives ---------------------------------------------------------------------- */

static void vert_pix(const GsVertex *v, Pix *p)
{
    p->r = v->r; p->g = v->g; p->b = v->b; p->a = v->a;
    p->z = v->z;
    p->s = v->s; p->t = v->t; p->q = v->q;
    p->u = (float)v->u / 16.0f; p->v = (float)v->v / 16.0f;
    p->fog = v->fog;
}

static void draw_triangle(const Ctx *c, const GsVertex *v0, const GsVertex *v1, const GsVertex *v2)
{
    float x0 = v0->x / 16.0f, y0 = v0->y / 16.0f, x1 = v1->x / 16.0f, y1 = v1->y / 16.0f;
    float x2 = v2->x / 16.0f, y2 = v2->y / 16.0f;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    int minx, maxx, miny, maxy, x, y;
    Pix p0, p1, p2, flat;
    if (area == 0.0f)
        return;
    minx = (int)ceilf(fminf(x0, fminf(x1, x2)));
    maxx = (int)floorf(fmaxf(x0, fmaxf(x1, x2)));
    miny = (int)ceilf(fminf(y0, fminf(y1, y2)));
    maxy = (int)floorf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < c->scx0) minx = c->scx0;
    if (maxx > c->scx1) maxx = c->scx1;
    if (miny < c->scy0) miny = c->scy0;
    if (maxy > c->scy1) maxy = c->scy1;
    if (maxx - minx > 2048 || maxy - miny > 2048)
        return;
    vert_pix(v0, &p0);
    vert_pix(v1, &p1);
    vert_pix(v2, &p2);
    vert_pix(v2, &flat);
    for (y = miny; y <= maxy; y++) {
        if (!(y & 7))
            iop_sd_pump();
        for (x = minx; x <= maxx; x++) {
            float px = (float)x, py = (float)y;
            float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
            float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
            float w2 = 1.0f - w0 - w1;
            Pix p;
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                /* top-left rule approximated: accept exact edges */
                if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f)
                    continue;
            }
            if (c->iip) {
                p.r = (int)(w0 * p0.r + w1 * p1.r + w2 * p2.r + 0.5f);
                p.g = (int)(w0 * p0.g + w1 * p1.g + w2 * p2.g + 0.5f);
                p.b = (int)(w0 * p0.b + w1 * p1.b + w2 * p2.b + 0.5f);
                p.a = (int)(w0 * p0.a + w1 * p1.a + w2 * p2.a + 0.5f);
            } else {
                p.r = flat.r; p.g = flat.g; p.b = flat.b; p.a = flat.a;
            }
            p.z = (uint32_t)((double)w0 * p0.z + (double)w1 * p1.z + (double)w2 * p2.z);
            p.s = w0 * p0.s + w1 * p1.s + w2 * p2.s;
            p.t = w0 * p0.t + w1 * p1.t + w2 * p2.t;
            p.q = w0 * p0.q + w1 * p1.q + w2 * p2.q;
            p.u = w0 * p0.u + w1 * p1.u + w2 * p2.u;
            p.v = w0 * p0.v + w1 * p1.v + w2 * p2.v;
            p.fog = (int)(w0 * p0.fog + w1 * p1.fog + w2 * p2.fog);
            plot(c, x, y, &p);
        }
    }
}

static void draw_sprite(const Ctx *c, const GsVertex *v0, const GsVertex *v1)
{
    int x0 = (v0->x + 15) >> 4, x1 = (v1->x + 15) >> 4, y0 = (v0->y + 15) >> 4, y1 = (v1->y + 15) >> 4;
    float fx0 = v0->x / 16.0f, fx1 = v1->x / 16.0f, fy0 = v0->y / 16.0f, fy1 = v1->y / 16.0f;
    int x, y;
    Pix a, b;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x1 - x0 > 2048 || y1 - y0 > 2048)
        return;
    vert_pix(v0, &a);
    vert_pix(v1, &b);
    for (y = y0 < c->scy0 ? c->scy0 : y0; y < y1 && y <= c->scy1; y++) {
        float ty = fy1 != fy0 ? ((float)y - fy0) / (fy1 - fy0) : 0.0f;
        if (!(y & 7))
            iop_sd_pump();
        for (x = x0 < c->scx0 ? c->scx0 : x0; x < x1 && x <= c->scx1; x++) {
            float tx = fx1 != fx0 ? ((float)x - fx0) / (fx1 - fx0) : 0.0f;
            Pix p = b;
            p.u = a.u + (b.u - a.u) * tx;
            p.v = a.v + (b.v - a.v) * ty;
            p.s = a.s + (b.s - a.s) * tx;
            p.t = a.t + (b.t - a.t) * ty;
            p.q = b.q;
            plot(c, x, y, &p);
        }
    }
}

static void draw_line(const Ctx *c, const GsVertex *v0, const GsVertex *v1)
{
    float x0 = v0->x / 16.0f, y0 = v0->y / 16.0f, x1 = v1->x / 16.0f, y1 = v1->y / 16.0f;
    int n = (int)fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) + 1, k;
    Pix a, b;
    if (n > 4096)
        return;
    vert_pix(v0, &a);
    vert_pix(v1, &b);
    for (k = 0; k < n; k++) {
        float t = n > 1 ? (float)k / (float)(n - 1) : 0.0f;
        Pix p = b;
        if (c->iip) {
            p.r = (int)(a.r + (b.r - a.r) * t); p.g = (int)(a.g + (b.g - a.g) * t);
            p.b = (int)(a.b + (b.b - a.b) * t); p.a = (int)(a.a + (b.a - a.a) * t);
        }
        p.z = (uint32_t)(a.z + ((double)b.z - a.z) * t);
        p.s = a.s + (b.s - a.s) * t; p.t = a.t + (b.t - a.t) * t; p.q = a.q + (b.q - a.q) * t;
        p.u = a.u + (b.u - a.u) * t; p.v = a.v + (b.v - a.v) * t;
        plot(c, (int)(x0 + (x1 - x0) * t), (int)(y0 + (y1 - y0) * t), &p);
    }
}

void gs_raster_draw(const GsDraw *d, int type, const GsVertex *v, int n)
{
    Ctx c;
    setup(&c, d);
    (void)n;
    switch (type) {
    case 0: {
        Pix p;
        vert_pix(&v[0], &p);
        plot(&c, v[0].x >> 4, v[0].y >> 4, &p);
        break;
    }
    case 1: case 2:
        draw_line(&c, &v[0], &v[1]);
        break;
    case 3: case 4: case 5:
        draw_triangle(&c, &v[0], &v[1], &v[2]);
        break;
    case 6:
        draw_sprite(&c, &v[0], &v[1]);
        break;
    }
}
