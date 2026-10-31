/* cliptest.c: the clipper's geometry (platform/wii/clip.h), tested on the host.
 *
 * The clipper cuts the world's triangles to the screen, and to the near plane, the way the PS2's GS
 * cut them to its drawing area, before they reach the Wii's GPU (which would interpolate them over a
 * triangle many screens big instead).
 *
 * The camera is the port's own: X, Y and W of a world point are three affine functions of it (the
 * camera's rows), and the screen's position is (X/W, Y/W).
 *
 * The tests are:
 *  - a triangle inside the volume comes out unchanged;
 *  - a triangle crossing a side comes out cut exactly on it, a vertex on the side is not doubled;
 *  - a triangle completely outside is dropped, one completely behind the eye too;
 *  - a triangle that crosses the near plane keeps what is in front: every vertex that comes out is in
 *    front of the near plane and inside the screen's rectangle;
 *  - the polygon is the triangle's part inside the volume, and nothing else changes.  With the camera
 *    at big coordinates and triangles that reach far outside the screen (the guard band's case) or
 *    cross the near plane, against a double precision reference: every vertex has the texture
 *    coordinate, the depth and (where the whole triangle is in front of the eye) the colour that the
 *    original triangle has at that place, and the polygon covers the screen points that the triangle
 *    does.
 *
 * Build: gcc -O2 -I. -o cliptest tools/clip/cliptest.c -lm && cliptest */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/wii/clip.h"

static int failed;

static void check(int ok, const char *what, double got, double want)
{
    if (!ok) {
        printf("FAIL %-60s got %g want %g\n", what, got, want);
        failed++;
    } else {
        printf("ok   %-60s %g\n", what, got);
    }
}

/* the camera: X = r[0].p + o and so on */
typedef struct Cam {
    double r[3][4];
} Cam;

static void project(const Cam *c, const float *p, float *X, float *Y, float *W)
{
    *X = (float)(c->r[0][0] * p[0] + c->r[0][1] * p[1] + c->r[0][2] * p[2] + c->r[0][3]);
    *Y = (float)(c->r[1][0] * p[0] + c->r[1][1] * p[1] + c->r[1][2] * p[2] + c->r[1][3]);
    *W = (float)(c->r[2][0] * p[0] + c->r[2][1] * p[1] + c->r[2][2] * p[2] + c->r[2][3]);
}

static ClipV vertex(const Cam *c, float x, float y, float z, float u, float v)
{
    ClipV r;
    memset(&r, 0, sizeof r);
    r.p[0] = x;
    r.p[1] = y;
    r.p[2] = z;
    r.u = u;
    r.v = v;
    project(c, r.p, &r.X, &r.Y, &r.W);
    return r;
}

/* the camera at the origin looking down -z: W = -z, X = x, Y = y */
static void cam_origin(Cam *c)
{
    memset(c, 0, sizeof *c);
    c->r[0][0] = 1.0;
    c->r[1][1] = 1.0;
    c->r[2][2] = -1.0;
}

/* a world point that lands at (hx, hy) with the depth W */
static ClipV at(const Cam *c, float hx, float hy, float W, float u, float v)
{
    return vertex(c, hx * W, hy * W, -W, u, v);
}

static double rnd(void)
{
    return rand() / (double)RAND_MAX;
}

static double rr(double a, double b)
{
    return a + (b - a) * rnd();
}

/* a random camera at big coordinates, looking somewhere, with the screen X/W, Y/W in 0..1 */
static void cam_random(Cam *c, double *eye)
{
    const double yaw = rr(-3.14, 3.14), pitch = rr(-0.5, 0.2);
    const double cy = cos(yaw), sy = sin(yaw), cp = cos(pitch), sp = sin(pitch);
    const double fwd[3] = {sy * cp, sp, -cy * cp};
    const double right[3] = {cy, 0, sy};
    const double up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                          right[0] * fwd[1] - right[1] * fwd[0]};
    const double *rows[3] = {right, up, fwd};
    double e[3][4];
    int i, k;
    eye[0] = rr(-3000, 3000);
    eye[1] = rr(-100, 500);
    eye[2] = rr(-3000, 3000);
    for (i = 0; i < 3; i++) {
        double off = 0;
        for (k = 0; k < 3; k++) {
            e[i][k] = rows[i][k];
            off -= rows[i][k] * eye[k];
        }
        e[i][3] = off;
    }
    /* X = (W + 0.75 x_cam) / 2 and Y = (W + y_cam) / 2: the screen is X/W, Y/W in 0..1 */
    for (k = 0; k < 4; k++) {
        c->r[2][k] = e[2][k];
        c->r[0][k] = 0.5 * (e[2][k] + 0.75 * e[0][k]);
        c->r[1][k] = 0.5 * (e[2][k] + e[1][k]);
    }
}

static double dW(const Cam *c, const double *p)
{
    return c->r[2][0] * p[0] + c->r[2][1] * p[1] + c->r[2][2] * p[2] + c->r[2][3];
}

static void dproj(const Cam *c, const double *p, double *hx, double *hy)
{
    const double W = dW(c, p);
    *hx = (c->r[0][0] * p[0] + c->r[0][1] * p[1] + c->r[0][2] * p[2] + c->r[0][3]) / W;
    *hy = (c->r[1][0] * p[0] + c->r[1][1] * p[1] + c->r[1][2] * p[2] + c->r[1][3]) / W;
}

static int inside_rect(const ClipRect *r, const ClipV *v, float slack)
{
    const float W = v->W, e = slack * fabsf(W);
    return W >= r->eps * (1.0f - slack) && v->X >= r->hx0 * W - e && v->X <= r->hx1 * W + e && v->Y >= r->hy0 * W - e &&
           v->Y <= r->hy1 * W + e;
}

/* barycentrics of the point q in the plane of the triangle P (in doubles): m1 and m2 */
static int bary3(const double P[3][3], const double *q, double *m1, double *m2)
{
    double e1[3], e2[3], d[3], d11, d12, d22, q1, q2, det;
    int j;
    for (j = 0; j < 3; j++) {
        e1[j] = P[1][j] - P[0][j];
        e2[j] = P[2][j] - P[0][j];
        d[j] = q[j] - P[0][j];
    }
    d11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
    d12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
    d22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
    q1 = d[0] * e1[0] + d[1] * e1[1] + d[2] * e1[2];
    q2 = d[0] * e2[0] + d[1] * e2[1] + d[2] * e2[2];
    det = d11 * d22 - d12 * d12;
    if (fabs(det) < 1e-12 * d11 * d22)
        return 0;
    *m1 = (q1 * d22 - q2 * d12) / det;
    *m2 = (q2 * d11 - q1 * d12) / det;
    return 1;
}

/* is the screen point (hx, hy) inside the convex polygon (the vertices' X/W, Y/W)? */
static int in_polygon(const ClipV *p, int n, double hx, double hy, double *margin)
{
    int k, pos = 0, neg = 0;
    double least = 1e30;
    for (k = 0; k < n; k++) {
        const ClipV *a = &p[k], *b = &p[(k + 1) % n];
        const double ax = (double)a->X / a->W, ay = (double)a->Y / a->W, bx = (double)b->X / b->W, by = (double)b->Y / b->W;
        const double cr = (bx - ax) * (hy - ay) - (by - ay) * (hx - ax);
        const double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        if (len > 1e-12 && fabs(cr) / len < least)
            least = fabs(cr) / len;
        if (cr > 0)
            pos = 1;
        if (cr < 0)
            neg = 1;
    }
    *margin = least;
    return !(pos && neg);
}

int main(void)
{
    Cam cam;
    ClipRect r = {0.05f, -1.0f, 1.0f, -1.0f, 1.0f};
    ClipV tri[3], out[CLIP_MAX_OUT];
    int n, i;

    cam_origin(&cam);

    /* 1. a triangle inside: unchanged */
    tri[0] = at(&cam, -0.5f, -0.5f, 10.0f, 0, 0);
    tri[1] = at(&cam, 0.5f, -0.5f, 10.0f, 1, 0);
    tri[2] = at(&cam, 0.0f, 0.5f, 10.0f, 0.5f, 1);
    n = clip_poly(&r, tri, out);
    check(n == 3, "inside: three vertices", n, 3);
    for (i = 0; i < n; i++)
        if (memcmp(&out[i], &tri[i], sizeof out[i]))
            break;
    check(i == 3, "inside: the vertices are the same, in order", i, 3);

    /* 2. crossing the right side */
    tri[0] = at(&cam, 0.5f, -0.5f, 10.0f, 0, 0);
    tri[1] = at(&cam, 2.0f, -0.5f, 10.0f, 1, 0);
    tri[2] = at(&cam, 0.9f, 0.5f, 10.0f, 0.5f, 1);
    n = clip_poly(&r, tri, out);
    check(n == 4, "crossing: four vertices", n, 4);
    {
        int onedge = 0, in = 1;
        for (i = 0; i < n; i++) {
            if (fabsf(out[i].X / out[i].W - 1.0f) < 1e-4f)
                onedge++;
            if (!inside_rect(&r, &out[i], 1e-4f))
                in = 0;
        }
        check(onedge == 2, "crossing: two vertices on the side", onedge, 2);
        check(in, "crossing: all inside the rectangle", in, 1);
    }

    /* 3. a vertex exactly on the side: three vertices, none twice */
    tri[2] = at(&cam, 1.0f, 0.5f, 10.0f, 0.5f, 1);
    n = clip_poly(&r, tri, out);
    check(n == 3, "on the side: three vertices", n, 3);

    /* 4. completely outside, and completely behind the eye */
    tri[0] = at(&cam, 2.0f, -0.5f, 10.0f, 0, 0);
    tri[1] = at(&cam, 3.0f, -0.5f, 10.0f, 1, 0);
    tri[2] = at(&cam, 2.5f, 0.5f, 10.0f, 0.5f, 1);
    check(clip_poly(&r, tri, out) == 0, "outside: dropped", clip_poly(&r, tri, out), 0);
    tri[0] = at(&cam, 0.0f, 0.0f, -5.0f, 0, 0);
    tri[1] = at(&cam, 0.5f, 0.0f, -5.0f, 1, 0);
    tri[2] = at(&cam, 0.0f, 0.5f, -7.0f, 0.5f, 1);
    check(clip_poly(&r, tri, out) == 0, "behind the eye: dropped", clip_poly(&r, tri, out), 0);

    /* 5. a vertex behind the eye: what comes out is in front and on the screen */
    tri[0] = vertex(&cam, -3.0f, -2.0f, -10.0f, 0, 0);      /* W = 10 */
    tri[1] = vertex(&cam, 3.0f, -2.0f, -10.0f, 4, 0);
    tri[2] = vertex(&cam, 0.0f, -2.0f, 5.0f, 2, 8);         /* W = -5: behind */
    n = clip_poly(&r, tri, out);
    check(n >= 3, "behind: the visible part is there", n, 3);
    {
        int in = 1;
        for (i = 0; i < n; i++)
            if (!inside_rect(&r, &out[i], 1e-4f))
                in = 0;
        check(in, "behind: every vertex in front and on the screen", in, 1);
    }

    /* 6. random big triangles: at most CLIP_MAX_OUT vertices, all of them inside, the polygon convex */
    {
        int most = 0, bad = 0, nonconvex = 0, t;
        srand(3);
        for (t = 0; t < 300000; t++) {
            int k;
            for (k = 0; k < 3; k++)
                tri[k] = at(&cam, (float)rr(-4, 4), (float)rr(-4, 4), (float)rr(0.5, 30), (float)k, 0);
            n = clip_poly(&r, tri, out);
            if (n > most)
                most = n;
            for (k = 0; k < n; k++)
                if (!inside_rect(&r, &out[k], 1e-4f))
                    bad++;
            if (n >= 3) {
                int sgn = 0;
                for (k = 0; k < n; k++) {
                    const ClipV *a = &out[k], *b = &out[(k + 1) % n], *c = &out[(k + 2) % n];
                    const float ax = a->X / a->W, ay = a->Y / a->W, bx = b->X / b->W, by = b->Y / b->W;
                    const float cx = c->X / c->W, cy = c->Y / c->W;
                    const float cr = (bx - ax) * (cy - by) - (by - ay) * (cx - bx);
                    if ((cr > 1e-5f && sgn < 0) || (cr < -1e-5f && sgn > 0))
                        nonconvex++;
                    if (cr > 1e-5f)
                        sgn = 1;
                    if (cr < -1e-5f)
                        sgn = -1;
                }
            }
        }
        check(most == 7, "big triangles: the most vertices is seven", most, 7);
        check(bad == 0, "big triangles: every vertex inside the volume", bad, 0);
        check(nonconvex == 0, "big triangles: the polygon stays convex", nonconvex, 0);
    }

    /* 7. against a double reference: a camera at big coordinates, ground triangles that reach far
     * outside the screen, some across the near plane */
    {
        int trials = 0, behind_trials = 0, polys = 0, verts = 0, bad_uv = 0, bad_w = 0, bad_col = 0, bad_range = 0, cover = 0, miscover = 0;
        double worst_uv = 0, worst_w = 0, worst_col = 0, worst_range = 0;
        int t;
        srand(7);
        for (t = 0; t < 40000; t++) {
            Cam c;
            double eye[3], P[3][3], U[3][2], Wd[3];
            ClipV tv[3];
            ClipRect rect = {0.05f, 0.0f, 1.0f, 0.0f, 1.0f};
            int k, allfront = 1;
            cam_random(&c, eye);
            {
                const double ground = eye[1] - rr(1, 30);
                const double dir[3] = {c.r[2][0], c.r[2][1], c.r[2][2]};
                const double tilt = rnd() < 0.5 ? 0.0 : rr(-0.2, 0.2);
                const double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                const double side[3] = {-dir[2] / len, 0.0, dir[0] / len};      /* sideways in the ground plane */
                for (k = 0; k < 3; k++) {
                    const double depth = k == 2 && rnd() < 0.7 ? rr(5, 3000) : rr(-2, 400);
                    double lat = (k == 0 ? rr(-1.5, 1.5) : rr(-20, 20)) * (fabs(depth) + 3.0) * rr(0.2, 1.2);
                    lat = lat > 2500 ? 2500 : lat < -2500 ? -2500 : lat;       /* a world of a few kilometres */
                    P[k][0] = eye[0] + dir[0] / len * depth + side[0] * lat;
                    P[k][2] = eye[2] + dir[2] / len * depth + side[2] * lat;
                    P[k][1] = ground + tilt * (P[k][0] - eye[0]);
                    U[k][0] = rr(0, 64);
                    U[k][1] = rr(0, 64);
                }
            }
            for (k = 0; k < 3; k++) {
                float fp[3];
                int j;
                for (j = 0; j < 3; j++) {
                    fp[j] = (float)P[k][j];
                    P[k][j] = fp[j];
                }
                Wd[k] = dW(&c, P[k]);
                if (Wd[k] <= rect.eps)
                    allfront = 0;
                tv[k] = vertex(&c, fp[0], fp[1], fp[2], (float)U[k][0], (float)U[k][1]);
                tv[k].c[0] = (float)(k * 100);
                tv[k].c[1] = (float)(255 - k * 100);
                tv[k].c[2] = (float)(k * 50);
                tv[k].c[3] = 255.0f;
            }
            if (Wd[0] <= 0 && Wd[1] <= 0 && Wd[2] <= 0)
                continue;
            trials++;
            behind_trials += !allfront;
            n = clip_poly(&rect, tv, out);
            if (n)
                polys++;
            /* each vertex: where it is in the original triangle gives its texture coordinate, its depth and
             * (all in front) its colour */
            for (k = 0; k < n; k++) {
                double m1, m2, q[3] = {out[k].p[0], out[k].p[1], out[k].p[2]}, hx, hy, ref[2], refW = dW(&c, q);
                int j;
                if (!bary3(P, q, &m1, &m2))
                    continue;
                verts++;
                for (j = 0; j < 3; j++) {
                    /* a colour is a mix of the triangle's three (the near plane's cuts included: a cut vertex is on an
                     * edge, however far behind the eye its end is) */
                    const double lo = fmin(fmin(tv[0].c[j], tv[1].c[j]), tv[2].c[j]), hi = fmax(fmax(tv[0].c[j], tv[1].c[j]), tv[2].c[j]);
                    const double out_by = fmax(lo - out[k].c[j], out[k].c[j] - hi) / 255.0;
                    if (out_by > worst_range)
                        worst_range = out_by;
                    if (out_by > 1e-3)
                        bad_range++;
                }
                for (j = 0; j < 2; j++)
                    ref[j] = U[0][j] * (1 - m1 - m2) + U[1][j] * m1 + U[2][j] * m2;
                {
                    const double eu = fmax(fabs(out[k].u - ref[0]), fabs(out[k].v - ref[1])) / 64.0;
                    const double ew = fabs(out[k].W - refW) / fmax(fabs(refW), 1e-3);
                    if (eu > worst_uv)
                        worst_uv = eu;
                    if (ew > worst_w)
                        worst_w = ew;
                    if (eu > 3e-3)
                        bad_uv++;
                    if (ew > 1e-3 && fabs(refW) > 0.05)
                        bad_w++;
                }
                if (allfront && refW > 0.05) {
                    double hxa[3], hya[3], det, s0, s1, s2, rc[3];
                    int q2;
                    for (q2 = 0; q2 < 3; q2++)
                        dproj(&c, P[q2], &hxa[q2], &hya[q2]);
                    dproj(&c, q, &hx, &hy);
                    det = (hxa[1] - hxa[0]) * (hya[2] - hya[0]) - (hya[1] - hya[0]) * (hxa[2] - hxa[0]);
                    if (fabs(det) > 1e-9) {
                        s1 = ((hx - hxa[0]) * (hya[2] - hya[0]) - (hy - hya[0]) * (hxa[2] - hxa[0])) / det;
                        s2 = ((hxa[1] - hxa[0]) * (hy - hya[0]) - (hya[1] - hya[0]) * (hx - hxa[0])) / det;
                        s0 = 1 - s1 - s2;
                        for (q2 = 0; q2 < 3; q2++)
                            rc[q2] = s0 * tv[0].c[q2] + s1 * tv[1].c[q2] + s2 * tv[2].c[q2];
                        {
                            const double ec = fmax(fmax(fabs(out[k].c[0] - rc[0]), fabs(out[k].c[1] - rc[1])),
                                                   fabs(out[k].c[2] - rc[2])) / 255.0;
                            if (ec > worst_col)
                                worst_col = ec;
                            if (ec > 4e-3)
                                bad_col++;
                        }
                    }
                }
            }
            /* coverage: random points of the screen, in the triangle or not, in the polygon or not */
            for (k = 0; k < 10; k++) {
                const double hx = rnd(), hy = rnd();
                double Mx[3][3], bx[3], det;
                double pw[3], m1, m2, margin, Wp;
                int j, inside_tri, inside_poly;
                /* the ray's point in the triangle's plane: X - hx W = 0, Y - hy W = 0, plane n.p = d */
                {
                    double e1[3], e2[3], nn[3], dd, rows[3][3];
                    for (j = 0; j < 3; j++) {
                        e1[j] = P[1][j] - P[0][j];
                        e2[j] = P[2][j] - P[0][j];
                    }
                    nn[0] = e1[1] * e2[2] - e1[2] * e2[1];
                    nn[1] = e1[2] * e2[0] - e1[0] * e2[2];
                    nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
                    dd = nn[0] * P[0][0] + nn[1] * P[0][1] + nn[2] * P[0][2];
                    for (j = 0; j < 3; j++) {
                        rows[0][j] = c.r[0][j] - hx * c.r[2][j];
                        rows[1][j] = c.r[1][j] - hy * c.r[2][j];
                        rows[2][j] = nn[j];
                    }
                    bx[0] = -(c.r[0][3] - hx * c.r[2][3]);
                    bx[1] = -(c.r[1][3] - hy * c.r[2][3]);
                    bx[2] = dd;
                    memcpy(Mx, rows, sizeof Mx);
                    det = Mx[0][0] * (Mx[1][1] * Mx[2][2] - Mx[1][2] * Mx[2][1]) - Mx[0][1] * (Mx[1][0] * Mx[2][2] - Mx[1][2] * Mx[2][0]) +
                          Mx[0][2] * (Mx[1][0] * Mx[2][1] - Mx[1][1] * Mx[2][0]);
                    if (fabs(det) < 1e-12)
                        continue;
                    for (j = 0; j < 3; j++) {
                        double M2[3][3];
                        int a2, b2;
                        for (a2 = 0; a2 < 3; a2++)
                            for (b2 = 0; b2 < 3; b2++)
                                M2[a2][b2] = b2 == j ? bx[a2] : Mx[a2][b2];
                        pw[j] = (M2[0][0] * (M2[1][1] * M2[2][2] - M2[1][2] * M2[2][1]) - M2[0][1] * (M2[1][0] * M2[2][2] - M2[1][2] * M2[2][0]) +
                                 M2[0][2] * (M2[1][0] * M2[2][1] - M2[1][1] * M2[2][0])) / det;
                    }
                }
                Wp = dW(&c, pw);
                if (Wp <= 2.0 * rect.eps || !bary3(P, pw, &m1, &m2))
                    continue;
                {
                    /* how far inside the triangle (in barycentric units) */
                    const double mm = fmin(fmin(m1, m2), 1 - m1 - m2);
                    if (fabs(mm) < 1e-5)
                        continue;
                    inside_tri = mm > 0;
                }
                inside_poly = n >= 3 && in_polygon(out, n, hx, hy, &margin);
                if (n >= 3 && margin < 1e-4)
                    continue;
                cover++;
                if (inside_tri != inside_poly)
                    miscover++;
            }
        }
        printf("     stress: %d triangles (%d crossing the near plane or behind it), %d with something on the screen, %d vertices, "
               "%d screen points\n", trials, behind_trials, polys, verts, cover);
        printf("     over the limits: %d texture coordinates (0.3%% of the range), %d depths (0.1%%), %d colours (0.4%%)\n", bad_uv,
               bad_w, bad_col);
        check(bad_uv * 1000 <= verts, "stress: the texture coordinate is the original's (0.1% of the vertices may be off; worst)", worst_uv, 0.0);
        check(bad_w * 1000 <= verts, "stress: the depth W is that of its place (0.1% may be off; worst, relative)", worst_w, 0.0);
        check(bad_col * 1000 <= verts, "stress: the colour is the screen's interpolation's (0.1% may be off; worst)", worst_col, 0.0);
        check(bad_range == 0, "stress: no colour is outside the range of the triangle's (worst, of the range)", worst_range, 0.0);
        check(miscover * 1000 <= cover, "stress: the polygon covers the screen points the triangle does (0.1% at most)", miscover, 0);
    }

    printf("\n%s\n", failed ? "FAILURES" : "all tests passed");
    return failed ? 1 : 0;
}
