/* GS backend that draws nothing: the first Wii builds, before the GX backend.
 * It counts what it is given, so the log shows whether the game renders and
 * how fast (frames counted as VIF1 chains, see hw.c). */
#include "gs.h"
#include "rt.h"
#include "hw.h"

int g_dump_every;

static uint32_t g_prims, g_verts, g_vsyncs, g_last_chains;
static uint64_t g_last_us;

void gs_backend_draw(const GsDraw *d, int type, const GsVertex *v, int n)
{
    (void)d; (void)type; (void)v;
    g_prims++;
    g_verts += (uint32_t)n;
}

void gs_backend_prims(const GsDraw *d, int type, const GsVertex *v, int n, const uint8_t *kick)
{
    gs_prims_each(d, type, v, n, kick);
}

void gs_backend_flush(void)
{
}

void gs_backend_sync(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h)
{
    (void)bp; (void)bw; (void)psm; (void)w; (void)h;
}

void gs_backend_vsync(void)
{
    if (++g_vsyncs % 600 == 0) {
        uint64_t now = plat_time_us();
        uint32_t frames = g_vif1_chains - g_last_chains, f = frames ? frames : 1;
        char pt[160];
        pt_report(pt, sizeof pt);
        LOG("gs: %.1f frames/s, %u draws, %u vertices, %u uploads (%u KB) each, over %.1f s; %s",
            frames * 1e6 / (double)(now - g_last_us), g_prims / f, g_verts / f, g_gs_uploads / f,
            g_gs_upload_bytes / f / 1024, (now - g_last_us) / 1e6, pt);
        g_prims = g_verts = g_gs_uploads = g_gs_upload_bytes = 0;
        g_last_chains = g_vif1_chains;
        g_last_us = now;
    }
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
    (void)b;
    return 0;
}

int gs_backend_world(const NwBlock *b, int kind)
{
    (void)b;
    (void)kind;
    return 0;
}
