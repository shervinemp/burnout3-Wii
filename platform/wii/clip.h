/* The world's clip: the PS2 GS's drawing area (the clipper's core).
 *
 * The PS2's GS clipped every triangle to its drawing area (the screen) before it rasterised it, and
 * interpolated the texture coordinates over the triangle it kept.  The Wii's GPU clips to a guard band
 * instead - much bigger - and rasterises (and interpolates) the triangle it keeps, so a triangle that
 * reaches far outside the screen, or one that crosses the near plane (its cut vertices land at W = near,
 * thousands of screens away), comes out with its texture warped.  This clips the triangle to the screen
 * first, on the CPU, and hands the GPU a polygon that stays on it.
 *
 * The clip is done in the world's space, against the five planes through the eye that bound what is
 * drawn: the near plane (W = eps) and the four sides of the screen.  Everything a vertex has is affine
 * in the world's position over a triangle (X, Y and W are the camera's rows applied to it; the texture
 * coordinates are an affine map of the triangle's plane), so the vertex that cuts an edge is the linear
 * interpolation of the edge's two, at the parameter t where the plane's distance is zero: the same
 * point, the same W, the same texture coordinate, the same depth that the GPU would have worked out at
 * that place of the whole triangle.  The picture does not change; only the triangle the GPU
 * interpolates over gets smaller.
 *
 * The colours are the exception: the GPU interpolates them linearly in the screen's space, not the
 * world's, so a cut vertex's colour is taken at the screen's parameter s = t W1 / (W0 + t (W1 - W0)).
 * That is for an edge with both ends in front of the eye; a cut of the near plane (an end behind it, the cut at
 * W = eps) has its colour at t, in the world's space, which is also where the GPU's own near clip and the
 * game's microprogram put it (s would be hundreds there: a colour thrown out of its range, black or white).
 *
 * The port uses this in platform/wii/gs_gx.c (clip_batch); tools/clip/cliptest.c tests it on the host. */

#ifndef CLIP_H
#define CLIP_H

#include <math.h>

/* A vertex: the world position, the camera's X, Y and W of it (X/W, Y/W: where it lands on the screen),
 * the texture coordinates and the colour. */
typedef struct ClipV {
    float p[3];
    float X, Y, W;
    float u, v;
    float c[4];
} ClipV;

/* The most vertices a clipped triangle has: the three sides and the five planes, one side each. */
#define CLIP_MAX_OUT 8

/* The clip volume: the near plane W >= eps, and the rectangle hx0 <= X/W <= hx1, hy0 <= Y/W <= hy1. */
typedef struct ClipRect {
    float eps;
    float hx0, hx1, hy0, hy1;
} ClipRect;

/* How far a vertex is inside each plane (zero on it, negative outside). */
static void clip_dist(const ClipRect *r, const ClipV *v, float *d)
{
    d[0] = v->W - r->eps;
    d[1] = v->X - r->hx0 * v->W;
    d[2] = r->hx1 * v->W - v->X;
    d[3] = v->Y - r->hy0 * v->W;
    d[4] = r->hy1 * v->W - v->Y;
}

/* The vertex that cuts the edge a-b at t (a is inside the plane, b outside, or the other way round). */
static void clip_cut(const ClipV *a, const ClipV *b, float t, ClipV *o)
{
    const float W = a->W + (b->W - a->W) * t;
    const float s = a->W > 0.0f && b->W > 0.0f && W > 0.0f ? t * b->W / W : t;
    int k;
    for (k = 0; k < 3; k++)
        o->p[k] = a->p[k] + (b->p[k] - a->p[k]) * t;
    o->X = a->X + (b->X - a->X) * t;
    o->Y = a->Y + (b->Y - a->Y) * t;
    o->W = W;
    o->u = a->u + (b->u - a->u) * t;
    o->v = a->v + (b->v - a->v) * t;
    for (k = 0; k < 4; k++)
        o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * s;
}

/* Clip the triangle (three vertices, in the order that gives its winding) to the volume.  The polygon
 * that is left comes out in `out`, in the same order round, and its number of vertices is returned: 0
 * when nothing of the triangle is inside.  The vertices of `tri` need X, Y and W; a vertex with W <= 0 is
 * fine (it is outside the near plane, which is cut first). */
static int clip_poly(const ClipRect *r, const ClipV *tri, ClipV *out)
{
    ClipV bufa[CLIP_MAX_OUT], bufb[CLIP_MAX_OUT];
    ClipV *in = bufa, *o = bufb, *sw;
    float dp[CLIP_MAX_OUT][5];
    int n = 3, plane, i, m;
    in[0] = tri[0];
    in[1] = tri[1];
    in[2] = tri[2];
    for (plane = 0; plane < 5 && n; plane++) {
        for (i = 0; i < n; i++)
            clip_dist(r, &in[i], dp[i]);
        m = 0;
        for (i = 0; i < n; i++) {
            const int j = i + 1 == n ? 0 : i + 1;
            const float dc = dp[i][plane], dn = dp[j][plane];
            /* a vertex exactly on the plane is inside and makes no cut of its own (the cut would be the
             * vertex again) */
            if (dc >= 0.0f) {
                if (m < CLIP_MAX_OUT)
                    o[m++] = in[i];
                if (dc > 0.0f && dn < 0.0f && m < CLIP_MAX_OUT)
                    clip_cut(&in[i], &in[j], dc / (dc - dn), &o[m++]);
            } else if (dn > 0.0f && m < CLIP_MAX_OUT) {
                clip_cut(&in[i], &in[j], dc / (dc - dn), &o[m++]);
            }
        }
        n = m;
        sw = in;
        in = o;
        o = sw;
    }
    for (i = 0; i < n; i++)
        out[i] = in[i];
    return n;
}

#endif
