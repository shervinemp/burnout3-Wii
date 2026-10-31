/* Clock-read counters for the world walker and its callees: see wtime.h. Empty without -DWTIME. */
#include "wtime.h"

#ifdef WTIME
#include "rt.h"
#include <string.h>

extern int g_wstat;                     /* vif.c */

uint64_t g_wt_foreign, g_wt_t0;
int g_wt_depth;

typedef struct { uint64_t ticks, foreign; uint32_t calls; } WtRow;
static WtRow g_wt[WT_N];

static const char *const k_wt_name[WT_N] = {
#define X(a, nm) #nm,
    WT_LIST(X)
#undef X
    "batch(26AF50)", "instview(26A710)", "texsend(221E80)", "vu0 0138 (vis)", "vu0 0920 (row71)", "coll070(273070)", "gs_load_clut", "xfer_run", "lazy_finish",
};

/* off (zero mark) when "wstat 9" is not set or the call is itself inside a foreign interval */
WtMark wt_mark(void)
{
    WtMark m = {0, 0};
    if (g_wstat == 9 && !g_wt_depth) {
        m.f0 = g_wt_foreign;
        m.t0 = plat_ticks();
    }
    return m;
}

WtMark wt_mark_in(void)
{
    WtMark m = {0, 0};
    if (g_wstat == 9)
        m.t0 = plat_ticks();
    return m;
}

void wt_done_in(int row, WtMark m)
{
    if (m.t0) {
        WtRow *r = &g_wt[row];
        r->ticks += plat_ticks() - m.t0;
        r->calls++;
    }
}

void wt_done(int row, WtMark m)
{
    if (m.t0 && !g_wt_depth) {
        WtRow *r = &g_wt[row];
        r->ticks += plat_ticks() - m.t0;
        r->foreign += g_wt_foreign - m.f0;
        r->calls++;
    }
}

#define X(a, nm) \
    void f_##a(R5900 *c); \
    void hle_wt_##a(R5900 *c) { WT_MARK(m); f_##a(c); WT_DONE(nm, m); }
WT_LIST(X)
#undef X

/* per frame: calls, cycles less the foreign time, the foreign time inside (12 cycles a tick) */
void wt_report(uint32_t frames)
{
    const uint32_t f = frames ? frames : 1;
    int k;
    for (k = 0; k < WT_N; k++) {
        const WtRow *r = &g_wt[k];
        if (r->calls)
            LOG("wt %-16s %5u calls, %8u cycles a frame (%8u foreign inside)", k_wt_name[k], r->calls / f,
                (uint32_t)((r->ticks > r->foreign ? r->ticks - r->foreign : 0) * 12 / f), (uint32_t)(r->foreign * 12 / f));
    }
    memset(g_wt, 0, sizeof g_wt);
}
#endif
