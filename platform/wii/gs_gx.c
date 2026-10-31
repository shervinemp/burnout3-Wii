/*
 * GX backend: GS primitives drawn by the Wii GPU.
 *
 * Coordinates: the EFB holds the render target 1:1 in GS pixels. Positions go
 * through an orthographic projection with GS Z mapped to GX depth; the
 * texture coordinates S, T, Q travel as the vertex normal and a projective
 * texgen (GX_TG_MTX3x4 from GX_TG_NRM) divides S/Q, T/Q per pixel, which is
 * what the GS does (it interpolates S, T, Q linearly in screen space).
 *
 * Colours: GS alpha 0x80 means 1.0, so vertex and texture alpha are doubled
 * (saturating) on the way in; TEV then works in GX units. MODULATE colour is
 * texture * vertex * 2 (vertex 0x80 = 1.0).
 *
 * Render targets: the EFB holds one GS frame buffer at a time. When the game
 * draws to another FBP, the EFB is copied out to that target's texture and
 * the new target's texture (if any) is drawn back in. Textures whose TBP is a
 * render target sample that copy.
 *
 * Textures from GS memory are decoded to RGBA8 and cached by content (see
 * tex_lookup).
 */
#include "gs.h"
#include "nw_car.h"
#include "vu_rec.h"
#include "hw.h"
#include "wtime.h"
#include "wii/clip.h"

#include <gccore.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

int g_dump_every;

/* ---- video ------------------------------------------------------------------------------ */

#define EFB_W 640
#define EFB_H 528
#define FIFO_SIZE (512 * 1024)
/* The texture arena: a cache (full: the least recently used leave, tex_evict).
 * Driving in a race has 2.3 MB in it (1.1 MB standing still, which is what
 * misled a cut to 2.5 MB: 5000 to 8000 decodes a frame then, 7 frames/s). */
#define TEXMEM_SIZE (4 * 1024 * 1024)

static GXRModeObj *g_mode;

/* Display lists of native draws come from one arena: DL_SEGS segments filled
 * in turn for the world's and the cars' lists (dl_room), then SDL_SLOTS
 * buffers for the game's static strips (gs_backend_strip). */
#define DL_ARENA (1024u * 1024)
#define SDL_SLOTS 6
#define SDL_MAXV 448
#define SDL_CAP ((SDL_MAXV * 32 + 64 + 31) & ~31u)
#define DL_SEGS 8
#define DL_SEG (((DL_ARENA - SDL_SLOTS * SDL_CAP) / DL_SEGS) & ~31u)
static uint8_t *g_dl_arena;
static uint32_t g_dl_used;              /* where the next list goes (in segment g_dl_seg) */

/* The GPU has read everything sent so far. Not GX_DrawDone: CPU and GPU
 * overlap by a frame (gs_backend_vsync sets draw done and waits for it a
 * frame later), so a GX_DrawDone in between returned at the last frame's
 * finish when that was still on its way, and its own finish then ended the
 * next frame's wait early. A token of its own has neither problem. */
static void gp_drain(void)
{
    static u16 token;
    const uint64_t t0 = gettime();
    if (++token == 0)
        token = 1;
    GX_SetDrawSync(token);
    while (GX_GetDrawSync() != token) {
        if (diff_msec(t0, gettime()) > 500) {
            static int logged;
            if (!logged++)
                LOG("gx: no draw sync token after 500 ms: waiting for draw done instead");
            GX_DrawDone();
            return;
        }
        LWP_YieldThread();
    }
}
static int g_native_mode;               /* GX is set up for native draws of this kind (NW_...) */
static uint32_t g_native_draws, g_dl_built;     /* status line */
static void ortho_mode(void);
static void *g_xfb[3];
/* debugging (gxflags 1073741824): frames in which something happened, shown
 * as squares under the frame meter: 0 speed blur, 1 bloom, 2 particles (VU1
 * 46D090), 3 colour arrays full, 4 bloom decay above its rest value, 5 a
 * list segment turned, 6 the frame number's low bit, 7 the haze pass set up */
static uint32_t g_mark_frame[8];
static int g_xfb_cur;           /* the next display copy goes there */
/* status line: frame pacing. Per frame shown, how many real retraces it
 * stayed (1, 2, 3, 4+; 0: replaced before it was ever shown) and how many
 * emulated vblanks the game took for it (2: 30 fps, 3: 20 fps) */
static uint32_t g_pace_shown[5], g_pace_game[5], g_pace_rc, g_pace_vb;
int g_gx_meter = 0;             /* debugging: the frame time meter in the corner (autopilot "meter 1": on) */
/* debugging (autopilot "freeze N", with "wstat 5"): the first frame of a race whose game clock (0x4E273C) is N or
 * more stays on the screen and nothing runs after it ("gx: frozen at ..." in the log): pictures of the same moment
 * of two builds, compared pixel by pixel (work/frz.sh). The frames from the clock's 600th on are the same ones in
 * every run at the game's own interval (pace 0). */
int g_gx_freeze;
extern int g_opt_new;                   /* gs.c (autopilot "newopt 0": the old ways) */
extern int g_opt3;                      /* hle_render.c (autopilot "opt3 0": the old ways, 2026-10-16) */
int g_frame_hist = 0;           /* the frames around a collision's first sparks logged (autopilot "fhist 1": on, with the time split) */
static uint32_t g_meter_vblanks;
static volatile uint32_t g_retraces;    /* real VI retraces (post-retrace callback) */

void pmc_poll(void);                    /* pmc.c: the performance counters' sums, 60 times a second */
void pmc_report(uint32_t frames);

static void retrace_cb(u32 count)
{
    (void)count;
    g_retraces++;
    pmc_poll();
}
static int g_gx_ready;
static void efb_fill_test(void);
static void gxc_drop(void);
static int g_state_valid;       /* GX state matches g_cur_* (defined below) */
static uint32_t g_vsyncs;
static int g_gx_bisect = 0;      /* debugging: GX up, nothing drawn */
int g_gx_maxdraw;               /* debugging (autopilot "maxdraw"): draws per frame, 0 all (display copies always) */
int g_gx_rtdump;                /* debugging (autopilot "rtdump"): write the targets and GS memory at the next frame */
/* debugging (autopilot "gxflags"): 1 no reshaping, 2 no rasterising, 4 no
 * readbacks, 8 no point tables, 16 the old cull mapping, 32 no native cull,
 * 64 the game's bloom chain emulated instead of post_bloom, 128 clipped world
 * batches left to VU1, 256 no lit world display lists, 512 no native sky,
 * 1024 untextured world batches left to VU1, 2048 no car display lists, 4096 log
 * CLUT flicker (tlut_flip_note), 8192 log car batch flicker (carsig_note), 16384 log
 * the post chain of two frames (post_log; with 32768 only speed blur frames), 65536
 * no speed blur, 131072 no car reflection pass, 524288 sphere map UVs pulled in (experiment),
 * 1048576 the final blit drawn, not aliased (blit_alias), 67108864 the player's car and the
 * frames it is not drawn as usual in (carwatch), 134217728 the lake's mirrored scenery (47E300)
 * drawn unmirrored, as it was, 268435456 the cars' two passes always as two draws (car_combo),
 * 536870912 no display lists for the game's static strips (gs_backend_strip) */
int g_gx_flags;
/* more of them (autopilot "gxflags2"): 1 the particles through their microprogram, 2 the same with
 * what the native path would draw compared to it (spark_check), 4 deformed car batches compared
 * with nc_load and nc_shade (car_skin_check), 8 deformed car batches through those, 16 the
 * particles as the microprogram's separate quads, not as ribbons (spark_strips), 32 the sky's
 * runs through the microprogram (its strip subroutine native: vu1_sky_strip), 64 with that the
 * strips compared with the lists' (sky_check), 128 lists whose arrays changed in place counted,
 * 256 the lists' arrays written out every frame (dl_touch), 512 the particle systems (smoke,
 * debris, glass: VU1 46D750) through their microprogram with what the native path would draw
 * compared to it (gs_backend_puffs_kick), 1024 through the microprogram alone, 2048 and 4096
 * the same for the embers (VU1 47C6F0), 8192 the car colours' and sphere UVs' bytes from the
 * quantised stores compared with the conversions they replace (nc_shade_direct), 16384 the car
 * lighting's loop in paired singles compared with the C loop, 32768 the C loop alone, 65536
 * crashed cars' batches skinned every frame (nc_sk_find: not kept), 131072 spark trails with
 * every point (spark_strips: not by their length on the screen), 262144 the points left out
 * measured against the trail as drawn, 524288 what changed in crashed cars' batches that are
 * skinned again (nc_sk_diag), 1048576 those batches skinned again whole (not only the vertices
 * of the bones that changed), 2097152 the native paths timed by part for the slow frames' lines,
 * 4194304 every spark drawn however many there are, 8388608 the world's
 * clipping counted (how many triangles cross the near plane, how many reach
 * far outside the screen), 16777216 the world's batches with a triangle over
 * the near plane not drawn at all (the near plane diagnostic), 33554432 those
 * batches drawn with the other cull sense, 67108864 the vertex arrays watched
 * and written out when they change (the game rewrites them in place),
 * 134217728 the EFB filled with a colour at the start (efb_fill_test),
 * 536870912 the calibration pattern (the BP's own interpolation) */
int g_gx_flags2;
/* gxflags2 2097152: the native paths timed by part (a slow frame's line then says where its "native" went):
 * car lighting, crashed cars (skinning and lighting), spark trails, smoke and embers, world lighting */
static uint64_t g_nat_t[5];
#define NAT_TIME(k_, stmt_) do {                                \
        if (PS2_UNLIKELY(g_gx_flags2 & 2097152)) {              \
            const uint64_t t_ = gettime();                      \
            stmt_;                                              \
            g_nat_t[k_] += gettime() - t_;                      \
        } else {                                                \
            stmt_;                                              \
        }                                                       \
    } while (0)
int g_tlog_fbp = 0x2D60;        /* gxflags 262144: the target whose draws are logged (autopilot "tlog", in blocks, decimal) */
int g_gs_statdraws;             /* debugging (autopilot "statdraws"): gsstat after a frame of more draws */
static uint32_t g_frame_draws;
static uint8_t *g_texmem;
int g_tv_fit = 1;                 /* the picture in the frame as on the PS2 (args "--tvfit 0": stretched) */

void plat_video_takeover(GXRModeObj **mode, void **xfb0, void **xfb1, void **xfb2);

static void vrun_hook(uint32_t addr);

/* The position matrix index GX has now (-1: not known). libogc marks its state dirty at every GX_SetCurrentMtx, the same
 * index or not, and writes the index register out at the next draw: a draw in the camera's matrix, the usual, did that
 * 500 times a frame. All callers go through here (the only GX_Init is gx_init's). */
static int g_cur_mtx = -1;
static inline void set_mtx(int m)
{
    if (g_cur_mtx != m) {
        g_cur_mtx = m;
        GX_SetCurrentMtx((u32)m);
    }
}

static void rcp_tables_check(void);

static void *g_fifo_ring;               /* the CPU FIFO's memory ("wskip" 2) */

static void gx_init(void)
{
    void *fifo;
    Mtx44 proj;
    Mtx id;
    rcp_tables_check();
    plat_video_takeover(&g_mode, &g_xfb[0], &g_xfb[1], &g_xfb[2]);
    VIDEO_SetPostRetraceCallback(retrace_cb);
    g_texmem = plat_alloc(TEXMEM_SIZE);
    g_gs_lazy = 1;
    if (!g_texmem || ((uintptr_t)g_texmem & 31))
        FATAL("gx: no memory for the texture cache");
    fifo = memalign(32, FIFO_SIZE);
    memset(fifo, 0, FIFO_SIZE);
    g_fifo_ring = fifo;
    GX_Init(fifo, FIFO_SIZE);
    GX_SetCopyClear((GXColor){0, 0, 0, 0}, 0);
    GX_SetViewport(0, 0, EFB_W, EFB_H, 0, 1);
    GX_SetScissor(0, 0, EFB_W, EFB_H);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetCopyFilter(g_mode->aa, g_mode->sample_pattern, GX_TRUE, g_mode->vfilter);
    GX_SetFieldMode(g_mode->field_rendering, g_mode->viHeight == 2 * g_mode->xfbHeight ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetClipMode(GX_CLIP_DISABLE);
    GX_SetZCompLoc(GX_FALSE);           /* Z after texturing: alpha test discards before Z writes */
    GX_SetDither(GX_FALSE);

    /* orthographic: x, y in GS pixels; eye z = -depth (0..1) */
    memset(proj, 0, sizeof proj);
    proj[0][0] = 2.0f / EFB_W;
    proj[0][3] = -1.0f;
    proj[1][1] = -2.0f / EFB_H;
    proj[1][3] = 1.0f;
    proj[2][2] = -1.0f;
    proj[2][3] = -1.0f;
    proj[3][3] = 1.0f;
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    guMtxIdentity(id);
    GX_LoadPosMtxImm(id, GX_PNMTX0);
    set_mtx(GX_PNMTX0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(2);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX0);
    /* native draws (gs_backend_native): object-space positions, colour, 4.12 UVs */
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_S16, 12);
    /* instanced props: positions and float UVs, colour per instance */
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT2, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);    /* lit ones: per vertex */
    /* cars: shaded on the CPU, sent directly */
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT3, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);      /* the sphere map's (car_pass_both) */
    GX_SetVtxAttrFmt(GX_VTXFMT6, GX_VA_POS, GX_POS_XYZ, GX_S16, 14);     /* the sky's lists (sky_mesh) */
    GX_SetVtxAttrFmt(GX_VTXFMT6, GX_VA_TEX0, GX_TEX_ST, GX_S16, 12);
    /* cars as display lists: pass 1 indexes positions, colours and the
     * render data's unsigned 4.12 UVs; pass 2 sends float UVs */
    GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_TEX0, GX_TEX_ST, GX_U16, 12);
    GX_SetVtxAttrFmt(GX_VTXFMT4, GX_VA_TEX1, GX_TEX_ST, GX_U8, 8);       /* the sphere map's (car_combo) */
    GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    {
        /* guest UV words hold (v, u) as GX reads the halves: swap s and t
         * (TEXMTX1 also scrolls world UVs; TEXMTX2 only swaps) */
        Mtx m = {{0, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 1, 0}};
        GX_LoadTexMtxImm(m, GX_TEXMTX1, GX_MTX2x4);
        GX_LoadTexMtxImm(m, GX_TEXMTX2, GX_MTX2x4);
    }
    g_dl_arena = plat_alloc(DL_ARENA);
    if (g_dl_arena && !((uintptr_t)g_dl_arena & 31))
        g_native_render = NATIVE_WORLD | NATIVE_CARS;
    {
        uint32_t plat_mem2_free(void);
        LOG("gx: native rendering %s, MEM2 free %u KB", g_native_render ? "on" : "off (no memory for display lists)",
            plat_mem2_free() / 1024);
    }
    {
        extern void (*g_vu1_run_hook)(uint32_t addr);
        g_vu1_run_hook = vrun_hook;
    }
    if (g_gx_flags2 & 134217728)
        efb_fill_test();               /* the console's EFB is not zeroed at the start */
    g_gx_ready = 1;
}

/* gxflags2 134217728: fill the EFB with a colour, like the console's EFB
 * (which holds whatever it had at the start: the loading screens show it
 * around the load image, because the game clears only what it draws into).
 * Dolphin starts with a zeroed EFB, so without this the difference cannot be
 * seen there. */
static void efb_fill_test(void)
{
    Mtx m;
    guMtxIdentity(m);
    GX_LoadPosMtxImm(m, GX_PNMTX0);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    /* the port's untextured recipe (apply_state): the rasterised colour in the fourth input with ADD.
     * GX_SetTevOp(GX_REPLACE) puts the texture's colour there, and with no texture bound that is black. */
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetScissor(0, 0, EFB_W, EFB_H);
    LOG("gx: the EFB filled with a colour (gxflags2 134217728)");
    /* the vertex format (gx_init) has position, normal and two colours: all four per vertex */
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(0, 0, 0); GX_Normal3f32(0, 0, 1); GX_Color1u32(0xFF00FFFFu); GX_Color1u32(0xFFFFFFFFu);
    GX_Position3f32((float)EFB_W, 0, 0); GX_Normal3f32(0, 0, 1); GX_Color1u32(0xFF00FFFFu); GX_Color1u32(0xFFFFFFFFu);
    GX_Position3f32((float)EFB_W, (float)EFB_H, 0); GX_Normal3f32(0, 0, 1); GX_Color1u32(0xFF00FFFFu); GX_Color1u32(0xFFFFFFFFu);
    GX_Position3f32(0, (float)EFB_H, 0); GX_Normal3f32(0, 0, 1); GX_Color1u32(0xFF00FFFFu); GX_Color1u32(0xFFFFFFFFu);
    GX_End();
    /* the state the port's draws assume: one texture coordinate (gx_init's), and nothing of what it keeps is the
     * GX's now (without this every textured draw after the fill came out black) */
    GX_SetNumTexGens(1);
    g_state_valid = 0;
    gxc_drop();
}

/* ---- vertex batches ------------------------------------------------------------------------ */

typedef struct BVert {
    float x, y, z;
    float s, t, q;
    uint32_t rgba, fog;         /* 0xRRGGBBAA as GX reads it */
} BVert;

#define BATCH_MAX 3000          /* vertices, a multiple of 3 */
static BVert g_batch[BATCH_MAX];
static int g_nb;

static int fix2_passes(void);
static void fix2_set(int alpha_pass);
static void gxc_drop(void);

static void batch_flush(void)
{
    int k, prev, pass;
    if (!g_nb)
        return;
    prev = pt_enter(PT_FLUSH);
    ortho_mode();
    for (pass = 0; pass < fix2_passes(); pass++) {
        fix2_set(pass);
        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)g_nb);
        for (k = 0; k < g_nb; k++) {
            const BVert *v = &g_batch[k];
            GX_Position3f32(v->x, v->y, v->z);
            GX_Normal3f32(v->s, v->t, v->q);
            GX_Color1u32(v->rgba);
            GX_Color1u32(v->fog);
        }
        GX_End();
    }
    fix2_set(0);
    g_nb = 0;
    pt_leave(prev);
}

/* ---- texture memory ------------------------------------------------------------------------ */

/* Decoded textures come from one arena in MEM2, behind fixed slots for the
 * TLUTs: spans in address order, handed out first fit. When none fits, the
 * textures unused the longest go (tex_evict; never one used in the last two
 * frames, which the GPU may still read), and only when that does not help
 * the whole cache is dropped (after the GPU is done with it). It used to be
 * a ring that dropped everything each time it wrapped, every 30 to 60
 * seconds of a race: some 150 textures decoded again in one frame. */
#define TLUT_MAX 512
#define TLUT_SLOT 512u                  /* 256 entries of RGB5A3 */
#define TLUT_AREA (TLUT_MAX * TLUT_SLOT)
#define TEX_MAX 2048
#define SPAN_MAX (2 * TEX_MAX + 2)

typedef struct Span {
    uint32_t off, size;
    int16_t prev, next;                 /* in address order, -1: none */
    uint8_t used;
    uint8_t stale;                      /* the GPU may still have this memory's old texels cached */
} Span;
static Span g_spans[SPAN_MAX];
static int16_t g_span_free[SPAN_MAX];
static int g_nspan_free = -1;           /* -1: not set up yet */
static uint32_t g_texmem_used;          /* bytes of the arena in use, for the status line */
static uint32_t g_tex_evicted, g_tex_dropped;
static uint32_t g_tex_inv;               /* texture cache invalidations (the memory reused) */
static uint32_t g_frame_no;

static void tex_cache_clear(void);
static int tex_evict(uint32_t size);

static void span_reset(void)
{
    int k;
    g_nspan_free = 0;
    for (k = SPAN_MAX - 1; k >= 1; k--)
        g_span_free[g_nspan_free++] = (int16_t)k;
    g_spans[0].off = TLUT_AREA;
    g_spans[0].size = TEXMEM_SIZE - TLUT_AREA;
    {
        extern int g_dbg;
        if (g_dbg & 4)
            g_spans[0].size = 2304 * 1024;      /* testing: a small arena, so that textures are dropped often */
    }
    g_spans[0].prev = g_spans[0].next = -1;
    g_spans[0].used = 0;
    g_spans[0].stale = 0;
    g_texmem_used = 0;
}

/* a free span of at least size bytes: its index, or -1 */
static int span_fit(uint32_t size)
{
    int k;
    for (k = 0; k >= 0; k = g_spans[k].next)
        if (!g_spans[k].used && g_spans[k].size >= size)
            return k;
    return -1;
}

static int span_take(int k, uint32_t size)
{
    Span *s = &g_spans[k];
    if (s->size > size && g_nspan_free > 0) {
        int n = g_span_free[--g_nspan_free];
        Span *r = &g_spans[n];
        r->off = s->off + size;
        r->size = s->size - size;
        r->used = 0;
        r->stale = s->stale;               /* what was there before is still cached */
        r->prev = (int16_t)k;
        r->next = s->next;
        if (s->next >= 0)
            g_spans[s->next].prev = (int16_t)n;
        s->next = (int16_t)n;
        s->size = size;
    }
    s->used = 1;
    g_texmem_used += s->size;
    return k;
}

static void span_release(int k)
{
    Span *s = &g_spans[k];
    int n;
    s->used = 0;
    s->stale = 1;                       /* the GPU may still have its texels cached */
    g_texmem_used -= s->size;
    n = s->next;
    if (n >= 0 && !g_spans[n].used) {
        s->size += g_spans[n].size;
        s->next = g_spans[n].next;
        if (s->next >= 0)
            g_spans[s->next].prev = (int16_t)k;
        g_span_free[g_nspan_free++] = (int16_t)n;
    }
    n = s->prev;
    if (n >= 0 && !g_spans[n].used) {
        g_spans[n].size += s->size;
        g_spans[n].next = s->next;
        if (s->next >= 0)
            g_spans[s->next].prev = (int16_t)n;
        g_span_free[g_nspan_free++] = (int16_t)k;
    }
}

/* memory for a texture: the span's index (its data at g_texmem + off), or -1 */
static int texmem_alloc(uint32_t size)
{
    int k;
    size = (size + 31) & ~31u;
    if (g_nspan_free < 0)
        span_reset();
    {
        extern int g_dbg;
        static int small;
        if (((g_dbg & 4) != 0) != small) {
            /* testing: the arena's size changes (span_reset) */
            small = (g_dbg & 4) != 0;
            batch_flush();
            gp_drain();
            tex_cache_clear();
        }
    }
    if (size > TEXMEM_SIZE - TLUT_AREA)
        return -1;
    k = span_fit(size);
    if (k < 0 && tex_evict(size))
        k = span_fit(size);
    if (k < 0) {
        batch_flush();
        gp_drain();
        tex_cache_clear();
        k = span_fit(size);
        if (k < 0)
            return -1;
    }
    return span_take(k, size);
}

static uint32_t mix(uint32_t h, uint32_t w)
{
    h ^= w;
    h *= 0x9E3779B1u;
    return h ^ (h >> 15);
}

/* ---- GS memory textures ------------------------------------------------------------------- */

/* block size in texels */
static void psm_block(uint32_t psm, uint32_t *bw, uint32_t *bh)
{
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *bw = 16; *bh = 8; break;
    case PSMT8: *bw = 16; *bh = 16; break;
    case PSMT4: *bw = 32; *bh = 16; break;
    default: *bw = 8; *bh = 8; break;
    }
}

/* word index of a texel */
static uint32_t texel_word(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    switch (psm) {
    case PSMCT16: return gs_addr16(bp, bw, x, y) >> 1;
    case PSMCT16S: return gs_addr16s(bp, bw, x, y) >> 1;
    case PSMZ16: return gs_addr16z(bp, bw, x, y) >> 1;
    case PSMZ16S: return gs_addr16sz(bp, bw, x, y) >> 1;
    case PSMT8: return gs_addr8(bp, bw, x, y) >> 2;
    case PSMT4: return gs_addr4(bp, bw, x, y) >> 3;
    case PSMZ32: case PSMZ24: return gs_addr32z(bp, bw, x, y);
    default: return gs_addr32(bp, bw, x, y);
    }
}

/* hash of the GS memory blocks a texture covers */
static uint32_t tex_hash(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th)
{
    uint32_t bw, bh, bx, by, h = 0x811C9DC5u, k;
    psm_block(psm, &bw, &bh);
    for (by = 0; by < th; by += bh)
        for (bx = 0; bx < tw; bx += bw) {
            const uint32_t *b = gs_mem + (texel_word(psm, tbp, tbw, bx, by) & ~63u);
            for (k = 0; k < 64; k += 4)
                h = mix(h, b[k] + b[k + 1] * 3 + b[k + 2] * 5 + b[k + 3] * 7);
        }
    return h;
}

/* newest page version among the pages a texture can touch */
static uint32_t tex_pages_ver(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th)
{
    uint32_t pw, ph, ppr, rows, cols, r, c, v = 0;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;
    }
    ppr = pw == 128 ? (tbw + 1) / 2 : tbw;
    if (!ppr)
        ppr = 1;
    rows = (th + ph - 1) / ph;
    cols = (tw + pw - 1) / pw;
    if (cols > ppr)
        cols = ppr;
    for (r = 0; r < rows; r++)
        for (c = 0; c <= cols; c++) {           /* +1: a base inside a page spills over */
            uint32_t pv = gs_page_ver[(tbp / 32 + r * ppr + c) & 511];
            if (pv > v)
                v = pv;
        }
    return v;
}

typedef struct Tex {
    uint32_t key, hash;         /* format key, content hash */
    void *data;
    uint16_t w, h;
    uint8_t fmt, ci;
    int16_t span;               /* its memory */
    uint32_t used;              /* frame it was last drawn with */
    struct Tex *next;
} Tex;

#define TEX_HASH 1024
static Tex g_texs[TEX_MAX];
static int g_ntex;              /* textures cached */
static Tex *g_tex_bucket[TEX_HASH];
static Tex *g_tex_spare;        /* unused entries of g_texs, through next */
static int g_tex_fresh;         /* entries of g_texs never used yet start here */

/* where a TEX0 location was last validated: skips rehashing unchanged memory */
typedef struct TexLoc {
    uint32_t tbp, tbw, psm, tw, th;
    uint32_t checked_ver, hash;
    uint16_t nblk;              /* its blocks (when few), in raster order: hashed in this order */
    uint16_t blk[128];
    uint16_t sblk[128];         /* the same sorted: checked page by page */
    uint8_t npg;                /* the pages they are in (when few), each with its first block's index */
    uint16_t pg[16];
    uint8_t pg_first[17];
} TexLoc;
#define LOC_N 512
static TexLoc g_locs[LOC_N];
static uint32_t g_tex_from_upload, g_tex_from_mem;     /* decodes, for the status line */
static uint32_t g_lk_upload, g_lk_known, g_lk_cover, g_lk_hash, g_lk_hash_kb;      /* lookups by path */
static uint32_t g_lk_placed;

/* TLUTs: slot k of the arena's start is g_tluts[k]'s, reused in turn */
typedef struct Tlut {
    uint32_t hash;
    uint32_t used;              /* frame it was last loaded */
    uint8_t live;
    struct Tlut *next;
} Tlut;
static Tlut g_tluts[TLUT_MAX];
static int g_tlut_turn;
static Tlut *g_tlut_bucket[256];
/* apply_state's texture memo (TexMemo) holds while this is unchanged: it changes when a texture or a TLUT slot
 * is given up (its entry and memory may be something else's then) */
static uint32_t g_tmemo_gen = 1;

static void tex_cache_clear(void)
{
    /* the memory is decoded into again: nothing may come from the GPU's texture cache */
    GX_InvalidateTexAll();
    gxc_drop();
    g_ntex = 0;
    g_tex_spare = NULL;
    g_tex_fresh = 0;
    memset(g_tex_bucket, 0, sizeof g_tex_bucket);
    memset(g_tluts, 0, sizeof g_tluts);
    memset(g_tlut_bucket, 0, sizeof g_tlut_bucket);
    span_reset();
    g_tex_dropped++;
    g_tmemo_gen++;
}

/* Make room for size bytes (or for an entry, size 0): drop the textures not
 * drawn with for the longest, in ever younger classes until a span fits.
 * Returns 0 when nothing could go. */
static int tex_evict(uint32_t size)
{
    uint32_t age;
    int any = 0, b;
    for (age = 4096; age >= 2; age >>= 1) {
        int freed = 0;
        for (b = 0; b < TEX_HASH; b++) {
            Tex **pp = &g_tex_bucket[b], *t;
            while ((t = *pp) != NULL) {
                if (g_frame_no - t->used < age) {
                    pp = &t->next;
                    continue;
                }
                *pp = t->next;
                span_release(t->span);
                t->next = g_tex_spare;
                g_tex_spare = t;
                g_ntex--;
                g_tex_evicted++;
                freed++;
            }
        }
        any |= freed;
        if (freed && (!size || span_fit(size) >= 0))
            break;
    }
    if (any) {
        g_tmemo_gen++;
        /* their memory is decoded into again: draw what is batched with the
         * old binding, and nothing may come from the GPU's texture cache */
        batch_flush();
        GX_InvalidateTexAll();
        gxc_drop();
    }
    return any;
}

/* GX texture layouts: byte offset of a texel */
static inline uint32_t ci8_off(uint32_t x, uint32_t y, uint32_t w) { return ((y >> 2) * (w >> 3) + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7); }
static inline uint32_t ci4_off(uint32_t x, uint32_t y, uint32_t w) { return ((y >> 3) * (w >> 3) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1); }
static inline uint32_t rgba8_off(uint32_t x, uint32_t y, uint32_t w) { return ((y >> 2) * (w >> 2) + (x >> 2)) * 64 + ((y & 3) * 4 + (x & 3)) * 2; }

/* GS alpha (0x80 = 1.0) as GX alpha */
static inline uint32_t a2(uint32_t a) { return a >= 0x80 ? 255 : a * 2; }

static inline uint32_t expand16(uint32_t c, uint64_t texa)
{
    uint32_t a = (c & 0x8000) ? GS_FIELD(texa, 32, 8) : GS_FIELD(texa, 0, 8);
    if (GS_FIELD(texa, 15, 1) && !(c & 0x7FFF))
        a = 0;
    return ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19) | a << 24;
}

/* A texture the game placed (gs.c), straight from its transfer in guest RAM.
 * The game sends an 8-bit texture as a 32-bit image of half its width and
 * height and a 4-bit one as a 16-bit image: the bytes GS memory would hold,
 * and block for block the same blocks (a pair of formats shares the order
 * of blocks in a page), so a block of the image is a block of the texture
 * with its texels in another order. One table a pair, made from the GS's
 * address functions; no GS memory in between. Through GS memory (the
 * transfer written there row by row, the texture read back row by row) a
 * 256x256 texture took about 3 ms, and a frame that met several new ones
 * was a vblank late: a collision's first frames, a vehicle seen first. */
typedef struct PlacedSrc {
    const uint32_t *data;
    uint32_t w, h;              /* the image's size in its own pixels */
} PlacedSrc;
static uint8_t g_pl8[64][4];    /* 32-bit pixel y * 8 + x of a block, byte b: the texel py * 16 + px of the 16x16 block */
static uint16_t g_pl4[128][4];  /* 16-bit pixel y * 16 + x of a block, nibble j: the texel py * 32 + px of the 32x16 block */
static uint32_t g_tex_direct, g_tex_direct_bad;         /* status line: decoded so, found different by the check */

static void placed_tables(void)
{
    static uint16_t inv[512];
    uint32_t x, y, b;
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            inv[gs_addr8(0, 2, x, y) & 255] = (uint16_t)(y * 16 + x);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            for (b = 0; b < 4; b++)
                g_pl8[y * 8 + x][b] = (uint8_t)inv[((gs_addr32(0, 1, x, y) & 63) * 4 + b) & 255];
    for (y = 0; y < 16; y++)
        for (x = 0; x < 32; x++)
            inv[gs_addr4(0, 2, x, y) & 511] = (uint16_t)(y * 32 + x);
    for (y = 0; y < 8; y++)
        for (x = 0; x < 16; x++)
            for (b = 0; b < 4; b++)
                g_pl4[y * 16 + x][b] = inv[((gs_addr16(0, 1, x, y) & 127) * 4 + b) & 511];
}

/* can the texture of tex0 be taken from what is placed at its base */
static int placed_src(uint64_t tex0, PlacedSrc *ps)
{
    uint32_t tbp = GS_FIELD(tex0, 0, 14), tbw = GS_FIELD(tex0, 14, 6), psm = GS_FIELD(tex0, 20, 6);
    uint32_t tw = 1u << GS_FIELD(tex0, 26, 4), th = 1u << GS_FIELD(tex0, 30, 4), dbw, dpsm;
    if ((g_gx_flags2 & 8388608) || tw > 1024 || th > 1024)
        return 0;
    if (!gs_placed_source(tbp, &dbw, &dpsm, &ps->w, &ps->h, &ps->data))
        return 0;
    if (ps->w * 2 != tw || ps->h * 2 != th || ((tbw + 1) >> 1) != dbw)
        return 0;
    if (psm == PSMT8)
        return dpsm == PSMCT32 && !(tw & 15) && !(th & 15);
    if (psm == PSMT4)
        return dpsm == PSMCT16 && !(tw & 31) && !(th & 15);
    return 0;
}

/* d: tw x th texels as GX tiles (CI8: 8x4 texels of a byte; CI4: 8x8 of a nibble, the first texel the high one) */
static void placed_tiles(const PlacedSrc *ps, int is4, uint32_t tw, uint32_t th, uint8_t *d)
{
    const uint32_t stride = (tw >> 3) * 32;     /* a row of tiles */
    const uint32_t *s = ps->data;
    static int made;
    uint32_t bx, by, x, y, k;
    if (!made) {
        made = 1;
        placed_tables();
    }
    if (!is4) {
        uint16_t off[64][4];
        for (k = 0; k < 256; k++) {
            const uint32_t t = g_pl8[k >> 2][k & 3], px = t & 15, py = t >> 4;
            off[k >> 2][k & 3] = (uint16_t)((py >> 2) * stride + (px >> 3) * 32 + (py & 3) * 8 + (px & 7));
        }
        for (by = 0; by < th / 16; by++)
            for (bx = 0; bx < tw / 16; bx++) {
                uint8_t *o = d + by * 4 * stride + bx * 64;
                const uint16_t (*f)[4] = (const uint16_t (*)[4])off;
                for (y = 0; y < 8; y++) {
                    const uint32_t *r = s + (by * 8 + y) * ps->w + bx * 8;
                    for (x = 0; x < 8; x++, f++) {
                        const uint32_t v = r[x];
                        o[(*f)[0]] = (uint8_t)v;
                        o[(*f)[1]] = (uint8_t)(v >> 8);
                        o[(*f)[2]] = (uint8_t)(v >> 16);
                        o[(*f)[3]] = (uint8_t)(v >> 24);
                    }
                }
            }
    } else {
        /* a block's 512 texels by their place in the tiles: byte, and which half of it */
        static uint8_t tile[32 * 16 / 2];
        uint16_t off[128][4];
        for (k = 0; k < 512; k++) {
            const uint32_t t = g_pl4[k >> 2][k & 3], px = t & 31, py = t >> 5;
            off[k >> 2][k & 3] = (uint16_t)((py * 16 + (px >> 1)) << 1 | (px & 1));
        }
        for (by = 0; by < th / 16; by++)
            for (bx = 0; bx < tw / 32; bx++) {
                const uint16_t (*f)[4] = (const uint16_t (*)[4])off;
                uint8_t *o = d + by * 2 * stride + bx * 128;
                memset(tile, 0, sizeof tile);
                for (y = 0; y < 8; y++) {
                    const uint32_t i0 = (by * 8 + y) * ps->w + bx * 16;
                    for (x = 0; x < 16; x++, f++) {
                        const uint32_t v = (s[(i0 + x) >> 1] >> (((i0 + x) & 1) * 16)) & 0xFFFF;
                        for (k = 0; k < 4; k++) {
                            const uint32_t n = (v >> (4 * k)) & 15, q = (*f)[k];
                            tile[q >> 1] |= (uint8_t)((q & 1) ? n : n << 4);
                        }
                    }
                }
                /* tile[]: 16 rows of 16 bytes (32 texels); the tiles: 8 texels (4 bytes) a row, 8 rows */
                for (y = 0; y < 16; y++)
                    for (x = 0; x < 4; x++)
                        memcpy(o + (y >> 3) * stride + x * 32 + (y & 7) * 4, tile + y * 16 + x * 4, 4);
            }
    }
}

/* gxflags2 16777216: every texture decoded so against the way through GS memory (else the first 48) */
static void placed_check(uint64_t tex0, const uint8_t *d)
{
    static uint8_t row[1024 + 8];
    static uint32_t checked;
    uint32_t tbp = GS_FIELD(tex0, 0, 14), tbw = GS_FIELD(tex0, 14, 6), psm = GS_FIELD(tex0, 20, 6);
    uint32_t tw = 1u << GS_FIELD(tex0, 26, 4), th = 1u << GS_FIELD(tex0, 30, 4), x, y, bad = 0;
    if (checked >= 48 && !(g_gx_flags2 & 16777216))
        return;
    checked++;
    gs_placed_load(tbp);
    gs_sync_rect(tbp, tbw, psm, 0, 0, tw, th);
    for (y = 0; y < th; y++) {
        gs_read_row8(psm, tbp, tbw, y, tw, row);
        for (x = 0; x < tw; x++) {
            const uint32_t v = psm == PSMT4 ? (d[ci4_off(x, y, tw)] >> ((x & 1) ? 0 : 4)) & 15 : d[ci8_off(x, y, tw)];
            bad += v != row[x];
        }
    }
    if (bad) {
        g_tex_direct_bad++;
        LOG("gx: placed texture %X/%u/%02X %ux%u decoded directly differs in %u texels", tbp, tbw, psm, tw, th, bad);
    }
}

/* u: the pending upload holding exactly this texture (pixels in rows), or
 * NULL to read GS memory; ps: what the game placed there, taken from guest RAM */
void iop_sd_pump(void);          /* runtime/iop_sd.c: sound during long native work */

static Tex *tex_decode(uint64_t tex0, uint64_t texa, uint32_t key, uint32_t hash, const GsUpload *u, const PlacedSrc *ps)
{
    uint32_t tbp = GS_FIELD(tex0, 0, 14), tbw = GS_FIELD(tex0, 14, 6), psm = GS_FIELD(tex0, 20, 6);
    uint32_t tw = 1u << GS_FIELD(tex0, 26, 4), th = 1u << GS_FIELD(tex0, 30, 4);
    uint32_t pw, ph, x, y, size;
    Tex *t;
    uint8_t *d;
    int span;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;
    /* an entry first: making room for one may drop textures */
    if (!g_tex_spare && g_tex_fresh == TEX_MAX && !tex_evict(0)) {
        batch_flush();
        gp_drain();
        tex_cache_clear();
    }
    if (psm == PSMT8 || psm == PSMT4 || psm == PSMT8H) {
        /* indices a row at a time into GX's 8x4 (CI8) or 8x8 (CI4) tiles (T8H: the sky's clouds,
         * 1024x256 in the alpha bytes of the depth buffer's memory -- a megabyte as RGBA8, which
         * the arena could not place: it dropped every texture every frame) */
        static uint8_t row[1024 + 8];
        int is4 = psm == PSMT4;
        pw = (tw + 7) & ~7u;
        ph = (th + (is4 ? 7 : 3)) & ~(is4 ? 7u : 3u);
        size = is4 ? pw * ph / 2 : pw * ph;
        span = texmem_alloc(size);
        if (span < 0)
            return NULL;
        d = g_texmem + g_spans[span].off;
        if (pw != tw || ph != th)
            memset(d, 0, size);
        memset(row + tw, 0, 8);
        if (ps) {
            placed_tiles(ps, is4, tw, th, d);
            g_tex_direct++;
            placed_check(tex0, d);
        }
        for (y = 0; y < th && !ps; y++) {
            if (u && psm != PSMT8H)
                gs_upload_row8(u, y, tw, row);
            else
                gs_read_row8(psm, tbp, tbw, y, tw, row);
            if (is4) {
                uint8_t *o = d + (y >> 3) * (pw >> 3) * 32 + (y & 7) * 4;
                for (x = 0; x < tw; x += 8, o += 32) {
                    o[0] = (uint8_t)(row[x] << 4 | row[x + 1]);
                    o[1] = (uint8_t)(row[x + 2] << 4 | row[x + 3]);
                    o[2] = (uint8_t)(row[x + 4] << 4 | row[x + 5]);
                    o[3] = (uint8_t)(row[x + 6] << 4 | row[x + 7]);
                }
            } else {
                uint8_t *o = d + (y >> 2) * (pw >> 3) * 32 + (y & 3) * 8;
                for (x = 0; x < tw; x += 8, o += 32)
                    memcpy(o, row + x, 8);
            }
        }
    } else {
        /* everything else as RGBA8 through the generic texel path */
        pw = (tw + 3) & ~3u;
        ph = (th + 3) & ~3u;
        size = pw * ph * 4;
        span = texmem_alloc(size);
        if (span < 0)
            return NULL;
        d = g_texmem + g_spans[span].off;
        for (y = 0; y < th; y++)
            for (x = 0; x < tw; x++) {
                uint32_t c;
                if (!u)
                    c = gs_texel_rgba(tex0, texa, (int)x, (int)y);
                else if (psm == PSMCT32)
                    c = gs_upload_pixel(u, x, y);
                else
                    c = expand16(gs_upload_pixel(u, x, y), texa);
                uint8_t *p = d + rgba8_off(x, y, pw);
                p[0] = (uint8_t)a2(c >> 24);
                p[1] = (uint8_t)c;
                p[32] = (uint8_t)(c >> 8);
                p[33] = (uint8_t)(c >> 16);
            }
    }
    DCFlushRange(d, size);
    if (span >= 0 && g_spans[span].stale) {
        /* this memory held another texture: the GPU's texture cache may still
         * have its old texels (the console reads the texture from RAM, not
         * from the CPU's cache) */
        g_spans[span].stale = 0;
        GX_InvalidateTexAll();
        g_tex_inv++;
    }
    if (g_tex_spare) {
        t = g_tex_spare;
        g_tex_spare = t->next;
    } else {
        t = &g_texs[g_tex_fresh++];
    }
    g_ntex++;
    t->span = (int16_t)span;
    t->used = g_frame_no;
    t->key = key;
    t->hash = hash;
    t->w = (uint16_t)tw;
    t->h = (uint16_t)th;
    t->ci = psm == PSMT8 || psm == PSMT4 || psm == PSMT8H;
    t->fmt = psm == PSMT4 ? GX_TF_CI4 : t->ci ? GX_TF_CI8 : GX_TF_RGBA8;
    t->data = d;
    t->next = g_tex_bucket[hash % TEX_HASH];
    g_tex_bucket[hash % TEX_HASH] = t;
    iop_sd_pump();
    return t;
}


/* the blocks a location covers, sorted, grouped by page (when few) */
static void loc_blocks(TexLoc *l)
{
    uint32_t bw, bh, bx, by, n = 0, k;
    psm_block(l->psm, &bw, &bh);
    l->npg = 0;
    if ((l->tw / bw) * (l->th / bh) > 128) {
        l->nblk = 0;
        return;
    }
    for (by = 0; by < l->th; by += bh)
        for (bx = 0; bx < l->tw; bx += bw)
            l->blk[n++] = (uint16_t)gs_block_of(l->psm, l->tbp, l->tbw ? l->tbw : 1, bx, by);
    l->nblk = (uint16_t)n;
    /* raster order is sorted but within pages: insertion sort is quick */
    for (k = 0; k < n; k++) {
        uint16_t b = l->blk[k];
        uint32_t j = k;
        for (; j > 0 && l->sblk[j - 1] > b; j--)
            l->sblk[j] = l->sblk[j - 1];
        l->sblk[j] = b;
    }
    for (k = 0; k < n; k++)
        if (!k || (l->sblk[k] >> 5) != (l->sblk[k - 1] >> 5)) {
            if (l->npg == 16) {
                l->npg = 0;             /* too many pages: block by block */
                return;
            }
            l->pg[l->npg] = (uint16_t)(l->sblk[k] >> 5);
            l->pg_first[l->npg++] = (uint8_t)k;
        }
    l->pg_first[l->npg] = (uint8_t)n;
}

/* has any block it covers been written since it was checked: pages first,
 * then the blocks of the pages written since */
static int loc_changed(const TexLoc *l)
{
    uint32_t k, j;
    if (!l->nblk)
        return tex_pages_ver(l->tbp, l->tbw, l->psm, l->tw, l->th) > l->checked_ver;
    if (l->npg) {
        for (j = 0; j < l->npg; j++)
            if (gs_page_ver[l->pg[j] & 511] > l->checked_ver)
                for (k = l->pg_first[j]; k < l->pg_first[j + 1]; k++)
                    if (gs_blk_ver[l->sblk[k]] > l->checked_ver)
                        return 1;
        return 0;
    }
    for (k = 0; k < l->nblk; k++)
        if (gs_blk_ver[l->blk[k]] > l->checked_ver)
            return 1;
    return 0;
}

static Tex *tex_lookup(uint64_t tex0, uint64_t texa)
{
    uint32_t tbp = GS_FIELD(tex0, 0, 14), tbw = GS_FIELD(tex0, 14, 6), psm = GS_FIELD(tex0, 20, 6);
    uint32_t tw = 1u << GS_FIELD(tex0, 26, 4), th = 1u << GS_FIELD(tex0, 30, 4);
    uint32_t key, hash, lh, uh, pid = 0;
    const GsUpload *up;
    TexLoc *l;
    Tex *t;
    int placed;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;
    /* paletted textures are cached by their indices; the TLUT is separate */
    key = psm | GS_FIELD(tex0, 26, 4) << 8 | GS_FIELD(tex0, 30, 4) << 12;
    if (psm != PSMT8 && psm != PSMT4 && psm != PSMT8H) {
        key ^= (uint32_t)(texa * 0x9E3779B1u);
        if (psm == PSMT4HL || psm == PSMT4HH)
            key ^= gs_clut_ver * 0x85EBCA6Bu;   /* decoded through the CLUT */
    }
    /* what this location holds: the pending upload of exactly this texture
     * (the usual: textures stream in every frame), else known while none
     * of its pages is written, else from the blocks' identities */
    l = &g_locs[((tbp | psm << 14 | GS_FIELD(tex0, 26, 8) << 20) * 0x9E3779B1u) >> 23];
    /* first of all what the game itself placed there (gs.c: no upload happened) */
    placed = gs_placed_for(tbp, &pid);
    up = !placed && g_gs_lazy ? gs_upload_for(tbp, tbw, psm, tw, th, &uh) : NULL;
    if (placed) {
        lh = mix(mix(pid, tbw), 0x55AA0002u);
        g_lk_placed++;
        l->tbp = ~0u;
    } else if (up) {
        lh = mix(gs_upload_ident(up), 0x55AA0001u);
        g_lk_upload++;
        l->tbp = ~0u;                   /* the location's own record is stale now */
    } else if (l->tbp != tbp || l->tbw != tbw || l->psm != psm || l->tw != tw || l->th != th || loc_changed(l)) {
        if (l->tbp != tbp || l->tbw != tbw || l->psm != psm || l->tw != tw || l->th != th) {
            l->tbp = tbp; l->tbw = tbw; l->psm = psm; l->tw = tw; l->th = th;
            loc_blocks(l);
        }
        if (g_gs_lazy) {
            /* block by block, by content (pending uploads count as their
             * data, no sync); not by tbp: the same texture moves around */
            uint32_t h = mix(0x5BD1E995u, tbw), k;
            if (l->nblk) {
                for (k = 0; k < l->nblk; k++)
                    h = mix(h, gs_block_ident_fast(l->blk[k]));
            } else {
                uint32_t bw, bh, bx, by;
                GsBlkWalk wk;
                psm_block(psm, &bw, &bh);
                gs_bw_init(&wk, psm, tbp, tbw ? tbw : 1);
                for (by = 0; by < th; by += bh) {
                    GsBlkRow row = gs_bw_row(&wk, by);
                    for (bx = 0; bx < tw; bx += bw)
                        h = mix(h, gs_block_ident_fast(gs_bw_blk(&wk, row, bx)));
                }
            }
            l->hash = h;
            g_lk_cover++;
        } else {
            gs_sync_rect(tbp, tbw, psm, 0, 0, tw, th);
            l->hash = tex_hash(tbp, tbw, psm, tw, th);
            g_lk_hash++;
            g_lk_hash_kb += tw * th / 1024;
            if (g_lk_hash < 30 && g_vsyncs > 1200)
                LOG("gx: hashed tex %X/%u/%02X %ux%u", tbp, tbw, psm, tw, th);
        }
        l->checked_ver = gs_ver;
        lh = l->hash;
    } else {
        g_lk_known++;
        lh = l->hash;
    }
    hash = mix(lh, key);
    for (t = g_tex_bucket[hash % TEX_HASH]; t; t = t->next)
        if (t->hash == hash && t->key == key) {
            t->used = g_frame_no;
            return t;
        }
    /* decode: from the upload when it is exactly this texture, else from memory */
    if (up) {
        g_tex_from_upload++;
        return tex_decode(tex0, texa, key, hash, up, NULL);
    }
    if (placed) {
        PlacedSrc ps;
        if (placed_src(tex0, &ps)) {
            g_tex_from_mem++;
            return tex_decode(tex0, texa, key, hash, NULL, &ps);
        }
        gs_placed_load(tbp);            /* now it is sent: decoded from memory as the others */
    }
    gs_sync_rect(tbp, tbw, psm, 0, 0, tw, th);
    g_tex_from_mem++;
    {
        static int logged;
        if (!placed && g_vsyncs > 15000 && logged < 60) {
            logged++;
            LOG("gx: decode %X/%u/%02X %ux%u from memory (%u blocks, hash %08X, key %08X, clut ver %u)", tbp, tbw,
                psm, tw, th, l->nblk, l->hash, key, gs_clut_ver);
        }
    }
    return tex_decode(tex0, texa, key, hash, NULL, NULL);
}

/* TLUT (RGB5A3) for the CLUT entries a paletted texture uses */
static uint32_t g_tlut_clut_ver = 0xFFFFFFFFu, g_tlut_last_key, g_tlut_last_hash;

static inline uint16_t rgb5a3(uint32_t c)
{
    uint32_t a = a2(c >> 24), r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    if (a >= 0xF0)
        return (uint16_t)(0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
    return (uint16_t)((a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
}

/* debugging (gxflags 4096): paletted textures whose sequence of palettes in
 * a frame differs from the frame before but equals the one two frames ago
 * (A B A: a CLUT that flickers; animated palettes step through more),
 * logged with the texture and CLUT addresses */
static uint32_t g_frame_no;
static uint32_t g_tlut_h;               /* hash of the last TLUT looked up */
static struct { uint32_t key, frame, n, prev_n, prev2_n, cur[8], prev[8], prev2[8]; } g_flip[256];

static void __attribute__((noinline)) tlut_flip_note(uint64_t tex0)
{
    static int logged;
    uint32_t key = (uint32_t)(GS_FIELD(tex0, 0, 14) | GS_FIELD(tex0, 20, 6) << 14) ^
                   (uint32_t)(GS_FIELD(tex0, 37, 14) << 18 | GS_FIELD(tex0, 56, 5) << 27), k;
    uint32_t slot = (key * 0x9E3779B1u) >> 24;
    typeof(g_flip[0]) *f = &g_flip[slot];
    if (f->key != key) {
        memset(f, 0, sizeof *f);
        f->key = key;
        f->frame = g_frame_no;
    }
    if (f->frame != g_frame_no) {
        if (f->frame + 1 == g_frame_no && f->prev_n && (f->n != f->prev_n || memcmp(f->cur, f->prev, f->n * 4)) &&
            f->n == f->prev2_n && !memcmp(f->cur, f->prev2, f->n * 4) && logged < 60) {
            char line[200];
            size_t m = 0;
            logged++;
            for (k = 0; k < f->prev_n && m < 90; k++)
                m += (size_t)snprintf(line + m, sizeof line - m, " %08X", f->prev[k]);
            m += (size_t)snprintf(line + m, sizeof line - m, " ->");
            for (k = 0; k < f->n && m < 190; k++)
                m += (size_t)snprintf(line + m, sizeof line - m, " %08X", f->cur[k]);
            LOG("CLUT sequence changed: tex %X/%02X cbp %X csa %u, frame %u:%s", (unsigned)GS_FIELD(tex0, 0, 14),
                (unsigned)GS_FIELD(tex0, 20, 6), (unsigned)GS_FIELD(tex0, 37, 14), (unsigned)GS_FIELD(tex0, 56, 5),
                g_frame_no, line);
        }
        if (f->frame + 1 == g_frame_no) {
            memcpy(f->prev2, f->prev, sizeof f->prev);
            f->prev2_n = f->prev_n;
        } else
            f->prev2_n = 0;
        memcpy(f->prev, f->cur, sizeof f->cur);
        f->prev_n = f->n;
        f->n = 0;
        f->frame = g_frame_no;
    }
    if (f->n < 8 && (!f->n || f->cur[f->n - 1] != g_tlut_h))
        f->cur[f->n++] = g_tlut_h;
}

/* shift: index scale for an index read from EFB alpha (T8H from a render target) */
static void *tlut_lookup(uint64_t tex0, uint64_t texa, int entries, int shift)
{
    uint32_t cpsm = GS_FIELD(tex0, 51, 4), csa = GS_FIELD(tex0, 56, 5);
    uint32_t base = entries == 16 ? (cpsm == 0 ? (csa & 15) * 16 : csa * 16) : 0;
    uint32_t key = base | (uint32_t)entries << 12 | cpsm << 24 | (uint32_t)shift << 28, h, k;
    uint16_t *d;
    Tlut *t;
    int replaced;
    if (cpsm)
        key ^= (uint32_t)(texa * 0x9E3779B1u);
    if (gs_clut_ver == g_tlut_clut_ver && key == g_tlut_last_key) {
        h = g_tlut_last_hash;
    } else {
        h = mix(key, gs_clut_hash(base, entries));     /* gs.c knows it for a whole load */
        g_tlut_clut_ver = gs_clut_ver;
        g_tlut_last_key = key;
        g_tlut_last_hash = h;
    }
    g_tlut_h = h;
    for (t = g_tlut_bucket[h & 255]; t; t = t->next)
        if (t->hash == h) {
            t->used = g_frame_no;
            return g_texmem + (uint32_t)(t - g_tluts) * TLUT_SLOT;
        }
    /* the next slot in turn; the one it held leaves its chain */
    t = &g_tluts[g_tlut_turn];
    g_tlut_turn = (g_tlut_turn + 1) % TLUT_MAX;
    replaced = t->live;
    if (replaced) {
        g_tmemo_gen++;
        Tlut **pp = &g_tlut_bucket[t->hash & 255];
        while (*pp && *pp != t)
            pp = &(*pp)->next;
        if (*pp)
            *pp = t->next;
        if (g_frame_no - t->used < 2) {
            /* so many TLUTs in two frames: the GPU may not have loaded this one yet */
            batch_flush();
            gp_drain();
        }
    }
    d = (uint16_t *)(g_texmem + (uint32_t)(t - g_tluts) * TLUT_SLOT);
    gs_clut_sync();
    for (k = 0; k < (uint32_t)entries; k++) {
        uint32_t c = gs_clut[(base + (k >> shift)) & 511];
        if (cpsm) {
            /* 16-bit CLUT entry: expand with TEXA */
            uint32_t a = (c & 0x8000) ? GS_FIELD(texa, 32, 8) : GS_FIELD(texa, 0, 8);
            if (GS_FIELD(texa, 15, 1) && !(c & 0x7FFF))
                a = 0;
            c = ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19) | a << 24;
        }
        d[k] = rgb5a3(c);
    }
    DCFlushRange(d, (uint32_t)entries * 2);
    if (replaced) {
        /* the slot held another palette: the GPU may have it cached */
        GX_InvalidateTexAll();
        g_tex_inv++;
    }
    t->hash = h;
    t->used = g_frame_no;
    t->live = 1;
    t->next = g_tlut_bucket[h & 255];
    g_tlut_bucket[h & 255] = t;
    return d;
}

/* ---- render targets ------------------------------------------------------------------------- */

/* The EFB is split: a 640x448 region at the top for the big target (the
 * scene, the display buffer) and an 80-line strip below for small targets
 * (reflection views, blur buffers), drawn there at 1/2 or 1/4 scale so
 * several stay resident while the game switches between them. A target that
 * loses its place is copied out to its texture and drawn back in later. */
#define BIG_W 640
#define BIG_H 448
#define STRIP_Y 448
#define STRIP_H 80
#define RT_MAX 24

typedef struct RT {
    uint32_t fbp, fbw;          /* FBP in blocks (FRAME.FBP * 32), width in 64s */
    int w, h;                   /* GS size */
    int big, resident, sh;      /* EFB scale: size >> sh */
    int ex, ey;                 /* EFB origin when resident */
    int dirty;                  /* EFB holds newer content than tex */
    int tex_valid;
    void *tex, *tex_a8;         /* RGBA8 copy; alpha as I8 (read as CI8 for T8H views) */
    int tw, th;                 /* EFB size, multiple of 4 */
    int ctw, cth, csh;          /* tex: its size and the scale it was copied at */
    uint32_t tex_cap, a8_cap;
    uint32_t a8_ver;            /* draw count when tex_a8 was made */
    uint32_t used, draws, presented;
    uint32_t ver;               /* gs_ver at the last draw: later uploads there end it */
    uint32_t mem_draws;         /* draws when GS memory last got the content (rt_readback) */
    int dx0, dy0, dx1, dy1;     /* area drawn (GS pixels, inclusive), since its layout was set */
    int soft;                   /* small: also rasterised into GS memory (gs_raster.c) */
    uint32_t zalpha;            /* frame its alpha was given Z bits 8-15 (the haze) */
    uint8_t awrite;             /* a draw into it wrote alpha (FBMSK let it) since memory last got its content */
    uint32_t clear;             /* last untextured sprite colour drawn into it (0xBBGGRR | 1 << 24) */
    uint32_t soft_frame, soft_streak;   /* last frame drawn; frames in a row drawn */
    uint32_t soft_frames;       /* frames it was drawn in, in all */
    uint32_t mem_read;          /* frame GS memory was last read inside its drawn area */
    uint32_t soft_n, soft_px;   /* status line: rasterised draws and their pixels */
    int alias;                  /* holds what another target drew (blit_alias): leaves the EFB cheaply */
    uint32_t st_draws;          /* status line: draws into it, and the time from each to the next draw */
    uint64_t st_ticks;
    uint32_t zbp;               /* the depth buffer its draws name (blocks) */
    uint8_t pend;               /* cleared by the game through other memory views (mem_clear): 1 colour, 2 depth; */
    uint32_t pend_col, pend_z;  /* done when it is next drawn into */
    uint8_t hold;               /* this frame its draws and clears are dropped: it keeps its picture (gs_backend_hold) */
    uint8_t pic;                /* it has a whole frame's picture */
    uint32_t hold_draws;        /* draws when gs_backend_hold last looked */
    uint8_t minsh;              /* placed in the strip at this scale or smaller */
    /* a reflection view in split-screen: whose picture it shows (owner), the other player's kept as a copy */
    uint8_t owner, alt_valid, alt_pic;
    void *alt_tex;
    uint32_t alt_cap;
    int alt_ctw, alt_cth, alt_csh;
} RT;

static RT g_rts[RT_MAX];
static int g_nrt;
static uint32_t g_rt_pages[16];         /* pages of GS memory the small targets span (gs_backend_sync) */
static int g_rt_pages_stale = 1;
static uint32_t g_sync_quick, g_sync_slow;      /* status line */
static RT *g_big;               /* owner of the big region */
static int g_strip_x;           /* next free x in the strip */
static uint32_t g_strip_fulls;  /* status line: times the strip was full (every small target moved out) */
static uint32_t g_draw_no;
/* frame monitor (user reports: the car missing for a frame): per frame, car
 * batches drawn, their first pass skipped, failed native draws, and big
 * targets evicted or reshaped (the scene's depth goes with them) */
static uint32_t g_fm_car, g_fm_car_skip, g_fm_car_fail, g_fm_big_out;
static uint32_t g_draws_before;         /* draws counted before g_draw_no was last reset */

/* draws since boot (the test autopilot waits on screens by their draws) */
uint32_t gs_backend_draw_count(void)
{
    return g_draws_before + g_draw_no;
}
static int g_state_valid;       /* GX state matches g_cur_* */
static int g_next_big_keeps;    /* the blit's source, bound next, takes the EFB region as it is (blit_alias) */
static struct RT *g_blit_src;

static void rt_place_strip(RT *r);

/* FBP (a multiple of 32 blocks, under 16384) -> the first target with it, + 1 (0: none): what the search through
 * g_rts finds, kept so where a target's FBP is set (rt_fbp_set). 2500 lookups a frame (every texture asks whether
 * it is a target), 180 cycles each as a search. */
static uint8_t g_rt_at[512];

static void rt_at_fix(uint32_t fbp)
{
    int k;
    if ((fbp & 31) || fbp >= 16384)
        return;
    for (k = 0; k < g_nrt; k++)
        if (g_rts[k].fbp == fbp) {
            g_rt_at[fbp >> 5] = (uint8_t)(k + 1);
            return;
        }
    g_rt_at[fbp >> 5] = 0;
}

static void rt_fbp_set(RT *r, uint32_t old, uint32_t fbp)
{
    r->fbp = fbp;
    rt_at_fix(old);
    rt_at_fix(fbp);
}

static RT *rt_find(uint32_t fbp)
{
    int k;
    if (!(fbp & 31) && fbp < 16384) {
        const uint32_t i = g_rt_at[fbp >> 5];
        return i ? &g_rts[i - 1] : NULL;
    }
    for (k = 0; k < g_nrt; k++)         /* (the bloom's strip target: no GS address) */
        if (g_rts[k].fbp == fbp)
            return &g_rts[k];
    return NULL;
}

/* texture memory for render target copies: big ones from MEM2, kept for reuse */
static void *gx_alloc(uint32_t size)
{
    void *p = size >= 256 * 1024 ? plat_alloc(size) : memalign(32, size);
    if (!p || ((uintptr_t)p & 31)) {
        extern void plat_mem_report(char *out, size_t size);
        char m[160];
        plat_mem_report(m, sizeof m);
        FATAL("gx: cannot allocate %u bytes of texture memory (%s)", size, m);
    }
    return p;
}

static uint32_t g_rt_copies, g_rt_restores, g_rt_readbacks, g_big_copies;

/* debugging (gxflags 67108864, "carwatch"): the order of things in a frame,
 * by draw number: the big targets leaving and coming back to the EFB, the
 * scene's world batches and the player's car batches (first and last) */
static struct {
    uint32_t n, at[12], what[12];       /* what: fbp | 1 << 20 evicted, 2 << 20 restored, 3 << 20 bound unrestored */
    uint32_t world[2], car[2], nworld, ncar;
} g_cwrt;

static void cw_rt_note(uint32_t what)
{
    if (!(g_gx_flags & 67108864) || g_cwrt.n >= 12)
        return;
    g_cwrt.at[g_cwrt.n] = g_draw_no;
    g_cwrt.what[g_cwrt.n++] = what;
}

/* debugging (gxflags 4194304): what happens to the small targets, by frame */
static void __attribute__((noinline)) rt_event(const RT *r, const char *what, int v)
{
    extern uint32_t g_vif1_chains;
    static uint32_t lines;
    if (!(g_gx_flags & 4194304) || lines >= 6000)
        return;
    lines++;
    if (r)
        LOG("rtev %u %05X %s %d: %dx%d fbw %u at %d sh %d res %d valid %d dirty %d draw %u", g_vif1_chains, r->fbp, what, v,
            r->w, r->h, r->fbw, r->ex, r->sh, r->resident, r->tex_valid, r->dirty, g_draw_no);
    else
        LOG("rtev %u ----- %s %d draw %u", g_vif1_chains, what, v, g_draw_no);
}

/* copy a resident target's EFB region to its texture */
static void rt_copy(RT *r, int clear)
{
    uint32_t size;
    batch_flush();
    rt_event(r, "copy", clear);
    g_rt_copies++;
    g_big_copies += r->big != 0;
    size = GX_GetTexBufferSize((u16)r->tw, (u16)r->th, GX_TF_RGBA8, GX_FALSE, 0);
    if (r->tex_cap < size) {
        r->tex = gx_alloc(size);
        r->tex_cap = size;
    }
    if (clear) {
        /* The clear writes what the pixel state lets through: with depth
         * writes off (whatever was drawn last) the region kept its depth, and
         * the next target placed there failed its depth tests against it --
         * a reflection view came out black in some frames, and the cars'
         * reflections flickered. Colour, alpha and depth all cleared. */
        GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_TRUE);
        g_state_valid = 0;
        gxc_drop();
    }
    GX_SetTexCopySrc((u16)r->ex, (u16)r->ey, (u16)r->tw, (u16)r->th);
    GX_SetTexCopyDst((u16)r->tw, (u16)r->th, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(r->tex, clear ? GX_TRUE : GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();
    r->ctw = r->tw;
    r->cth = r->th;
    r->csh = r->sh;
    r->tex_valid = 1;
    r->dirty = 0;
    r->a8_ver = 0;
}

static void present(RT *r);
static RT *display_rt(void);

static void rt_evict(RT *r)
{
    if (!r->resident)
        return;
    if (r->big) {
        static int logged;
        cw_rt_note(r->fbp | 1u << 20);
        g_fm_big_out++;
        if (logged < 30 && g_frame_no > 3000) {
            logged++;
            LOG("gx: big target %05X evicted at frame %u draw %u", r->fbp, g_frame_no, g_draw_no);
        }
    }
    if (r == display_rt() && r->draws != r->presented)
        present(r);                     /* show it before it leaves the EFB */
    if (r->alias && r->big && !(g_gx_flags & 1048576)) {
        /* the display buffer of a blit, shown: kept at half size only (as a
         * texture it is rarely read), the region left as it is -- the next
         * scene starts from it as the game's would from its memory */
        uint32_t size = GX_GetTexBufferSize((u16)(r->tw / 2), (u16)(r->th / 2), GX_TF_RGBA8, GX_FALSE, 0);
        batch_flush();
        if (r->tex_cap < size) {
            r->tex = gx_alloc(size);
            r->tex_cap = size;
        }
        GX_SetTexCopySrc((u16)r->ex, (u16)r->ey, (u16)r->tw, (u16)r->th);
        GX_SetTexCopyDst((u16)(r->tw / 2), (u16)(r->th / 2), GX_TF_RGBA8, GX_TRUE);
        GX_CopyTex(r->tex, GX_FALSE);
        GX_PixModeSync();
        GX_InvalidateTexAll();
        r->ctw = r->tw / 2;
        r->cth = r->th / 2;
        r->csh = r->sh + 1;
        r->tex_valid = 1;
        r->dirty = 0;
        r->a8_ver = 0;
        r->alias = 0;
        r->resident = 0;
        if (g_big == r)
            g_big = NULL;
        g_next_big_keeps = 1;
        return;
    }
    if (r->dirty || !r->tex_valid)
        rt_copy(r, 1);
    else {
        /* still clear the region for the next owner */
        rt_copy(r, 1);
    }
    r->resident = 0;
    if (g_big == r)
        g_big = NULL;
}

/* draw a target's saved texture back into its region */
static void rt_restore(RT *r)
{
    g_rt_restores++;
    rt_event(r, "restore", 0);
    if (r->big)
        cw_rt_note(r->fbp | 2u << 20);
    GXTexObj to;
    Mtx m;
    float x0 = (float)r->ex, y0 = (float)r->ey;
    float x1 = x0 + (float)((r->ctw << r->csh) >> r->sh), y1 = y0 + (float)((r->cth << r->csh) >> r->sh);
    batch_flush();
    g_state_valid = 0;
    gxc_drop();
    guMtxIdentity(m);
    GX_LoadPosMtxImm(m, GX_PNMTX0);
    GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX3x4);
    GX_SetScissor((u32)r->ex, (u32)r->ey, (u32)r->tw, (u32)r->th);
    GX_InitTexObj(&to, r->tex, (u16)r->ctw, (u16)r->cth, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&to, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&to, GX_TEXMAP0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    ortho_mode();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x0, y0, 0); GX_Normal3f32(0, 0, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x1, y0, 0); GX_Normal3f32(1, 0, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x1, y1, 0); GX_Normal3f32(1, 1, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x0, y1, 0); GX_Normal3f32(0, 1, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_End();
}

static void rt_place_strip(RT *r)
{
    int k, w;
    r->sh = r->minsh;
    while ((r->h >> r->sh) > STRIP_H || (r->w >> r->sh) > 320)
        r->sh++;
    w = ((r->w >> r->sh) + 7) & ~7;
    if (g_strip_x + w > EFB_W) {
        /* strip full: move every small target out and start over */
        rt_event(NULL, "strip full", g_strip_x);
        g_strip_fulls++;
        for (k = 0; k < g_nrt; k++)
            if (!g_rts[k].big && g_rts[k].resident)
                rt_evict(&g_rts[k]);
        g_strip_x = 0;
    }
    r->ex = g_strip_x;
    r->ey = STRIP_Y;
    g_strip_x += w;
}

/* make a target resident in the EFB */
static void rt_bind(RT *r)
{
    if (r->resident)
        return;
    batch_flush();
    if (r->big) {
        if (g_big)
            rt_evict(g_big);
        g_big = r;
        r->sh = 0;
        r->ex = 0;
        r->ey = 0;
    } else {
        rt_place_strip(r);
    }
    r->tw = ((r->w >> r->sh) + 3) & ~3;
    r->th = ((r->h >> r->sh) + 3) & ~3;
    r->resident = 1;
    rt_event(r, "bind", 0);
    if (r->big && g_next_big_keeps && r == g_blit_src) {
        /* the region holds last frame's picture, this target's own content
         * then (blit_alias): nothing to restore */
        g_next_big_keeps = 0;
        r->tex_valid = 0;
        r->dirty = 1;
        cw_rt_note(r->fbp | 3u << 20);
        return;
    }
    g_next_big_keeps = 0;
    if (r->tex_valid) {
        rt_restore(r);
    } else if (r->big) {
        /* Nothing to restore: the region holds whatever it had - on the
         * console the EFB is not zeroed at the start, and the game clears
         * only what it draws into, so the rest shows as noise around the
         * load image.  Clear it (and the depth with it). */
        rt_copy(r, 1);
        r->tex_valid = 0;
    }
}

/* A target's layout changed: a new buffer width (the same memory used as
 * another buffer, or cleared through a narrow view) drops the content; a
 * bigger scissor keeps it, restored at the new size. */
static void rt_reshape(RT *r, uint32_t fbw, int w, int h)
{
    int keep = fbw == r->fbw;
    static int logged;
    if (logged < 40 && g_frame_no > 3000) {
        logged++;
        LOG("gx: reshape %05X %ux%d -> %ux%dx%d (%s) at frame %u", r->fbp, r->fbw * 64, r->w, fbw * 64, w, h,
            keep ? "kept" : "dropped", g_frame_no);
    }
    rt_event(r, keep ? "reshape, kept" : "reshape, dropped", w);
    if (r->resident) {
        if (r->big)
            g_fm_big_out++;
        rt_copy(r, 1);                  /* saved, and the region cleared */
        r->resident = 0;
        if (g_big == r)
            g_big = NULL;
    }
    if (!keep) {
        r->tex_valid = 0;
        r->pic = 0;
        r->alt_valid = r->alt_pic = 0;
        r->dx0 = r->dy0 = 1 << 30;
        r->dx1 = r->dy1 = -1;
    }
    r->fbw = fbw;
    r->w = w;
    r->h = h;
    r->big = w >= 512 && h >= 256;
    r->soft = w * h <= 64 * 64;
    r->dirty = 0;
    g_rt_pages_stale = 1;
    g_state_valid = 0;
    gxc_drop();
}

static RT *rt_get(const GsDraw *d)
{
    uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, fbw = GS_FIELD(d->frame, 16, 6);
    int w = (int)GS_FIELD(d->scissor, 16, 11) + 1, h = (int)GS_FIELD(d->scissor, 48, 11) + 1;
    RT *r = rt_find(fbp);
    if (w > (int)fbw * 64)
        w = (int)fbw * 64;
    if (w > BIG_W) w = BIG_W;
    if (h > BIG_H) h = BIG_H;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    if (!r) {
        if (g_nrt == RT_MAX) {
            /* reuse the least recently used target */
            int k, lru = 0;
            for (k = 1; k < g_nrt; k++)
                if (g_rts[k].used < g_rts[lru].used && &g_rts[k] != g_big)
                    lru = k;
            r = &g_rts[lru];
            if (r->resident) {
                rt_copy(r, 1);
                r->resident = 0;
                if (g_big == r)
                    g_big = NULL;
            }
        } else {
            r = &g_rts[g_nrt++];
        }
        {
            void *t = r->tex, *a = r->tex_a8, *t2 = r->alt_tex;
            uint32_t tc = r->tex_cap, ac = r->a8_cap, tc2 = r->alt_cap, old = r->fbp;
            memset(r, 0, sizeof *r);
            r->tex = t; r->tex_a8 = a; r->tex_cap = tc; r->a8_cap = ac;
            r->alt_tex = t2; r->alt_cap = tc2;
            rt_fbp_set(r, old, fbp);
        }
        r->fbw = fbw;
        r->w = w;
        r->h = h;
        r->dx0 = r->dy0 = 1 << 30;
        r->dx1 = r->dy1 = -1;
        r->big = w >= 512 && h >= 256;     /* screen sized; the rest fit the strip scaled */
        r->soft = w * h <= 64 * 64;
        g_rt_pages_stale = 1;
        LOG("gx: render target %05X (%u wide) %dx%d%s", fbp, fbw * 64, w, h, r->big ? " (big)" : r->soft ? " (soft)" : "");
    } else if ((fbw != r->fbw || w > r->w || h > r->h) && !(g_gx_flags & 1)) {
        rt_reshape(r, fbw, fbw != r->fbw ? w : w > r->w ? w : r->w, fbw != r->fbw ? h : h > r->h ? h : r->h);
    }
    r->used = g_frame_no;
    return r;
}

/* a texture that is a render target (and not uploaded over since) */
static uint32_t g_disp_tex_read;      /* the display buffer's picture read as a texture */
static RT *rt_for_texture(uint32_t tbp)
{
    RT *r = rt_find(tbp);
    if (!r || !(r->resident || r->tex_valid))
        return NULL;
    if (r == display_rt())
        g_disp_tex_read++;
    if (r->hold)
        return r;                       /* kept from an earlier frame, whatever its memory was used for since */
    if (gs_page_ver[(r->fbp / 32) & 511] > r->ver)
        return NULL;
    return r;
}

/* block of a 32-bit target -> its 8x8 pixel square: the inverse of the GS
 * block order within a page */
static const uint8_t k_blk32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static uint8_t g_blk32_x[32], g_blk32_y[32];

/* does a target's drawn area cover any of a GS memory block */
static int rt_has_block(const RT *r, uint32_t blk)
{
    uint32_t fbw = r->fbw ? r->fbw : 1, off, page;
    int x, y;
    if (blk < r->fbp || r->dx1 < r->dx0)
        return 0;
    off = blk - r->fbp;
    page = off / 32;
    if (page / fbw >= 64)
        return 0;
    x = (int)((page % fbw) * 64 + g_blk32_x[off % 32] * 8u);
    y = (int)((page / fbw) * 32 + g_blk32_y[off % 32] * 8u);
    return x + 7 >= r->dx0 && x <= r->dx1 && y + 7 >= r->dy0 && y <= r->dy1;
}

/* GS memory from a target's content, for reads the target texture path
 * cannot serve: CLUTs, textures inside a target. Copy, wait for the GPU,
 * convert the drawn area, block by block, except blocks uploaded over since
 * the target was drawn; once per change of the target. */
static void rt_readback(RT *r)
{
    const uint8_t *t;
    int x, y, bx, by, x1 = r->dx1, y1 = r->dy1;
    if (r->resident && (r->dirty || !r->tex_valid))
        rt_copy(r, 0);
    r->mem_draws = r->draws;
    if (!r->tex_valid)
        return;
    g_rt_readbacks++;
    if (g_rt_readbacks < 40)
        LOG("gx: readback %05X (%u wide) %d,%d-%d,%d at frame %u", r->fbp, r->fbw * 64, r->dx0, r->dy0, x1, y1,
            g_frame_no);
    gp_drain();
    DCInvalidateRange(r->tex, (uint32_t)(r->ctw * r->cth * 4));
    t = r->tex;
    if (x1 >= (r->ctw << r->csh)) x1 = (r->ctw << r->csh) - 1;
    if (y1 >= (r->cth << r->csh)) y1 = (r->cth << r->csh) - 1;
    for (by = r->dy0 & ~7; by <= y1; by += 8)
        for (bx = r->dx0 & ~7; bx <= x1; bx += 8) {
            uint32_t blk = gs_addr32(r->fbp, r->fbw, (uint32_t)bx, (uint32_t)by) >> 6;
            if (gs_blk_ver[blk] > r->ver)
                continue;               /* uploaded over since the target drew */
            gs_sync_rect(r->fbp, r->fbw, PSMCT32, (uint32_t)bx, (uint32_t)by, 8, 8);
            for (y = by; y < by + 8 && y <= y1; y++)
                for (x = bx; x < bx + 8 && x <= x1; x++) {
                    const uint8_t *p = t + rgba8_off((uint32_t)(x >> r->csh), (uint32_t)(y >> r->csh), (uint32_t)r->ctw);
                    /* alpha only when the target wrote any: memory's may be something else's
                     * (the sky's clouds under the bloom's buffers) */
                    uint32_t a = r->awrite ? (((uint32_t)p[0] + 1) >> 1) << 24
                                           : gs_read_pixel(PSMCT32, r->fbp, r->fbw, (uint32_t)x, (uint32_t)y) & 0xFF000000u;
                    gs_write_pixel(PSMCT32, r->fbp, r->fbw, (uint32_t)x, (uint32_t)y,
                                   (uint32_t)p[1] | (uint32_t)p[32] << 8 | (uint32_t)p[33] << 16 | a);
                }
            gs_mark_rect(r->fbp, r->fbw, PSMCT32, (uint32_t)bx, (uint32_t)by, 8, 8);
        }
    r->awrite = 0;                      /* memory has it now */
}

/* blocks a rectangle of a format spans from its base, roughly (whole pages) */
static uint32_t span_blocks(uint32_t psm, uint32_t bw, uint32_t h)
{
    uint32_t pws = 6, phs = 5;          /* page size as shifts: 64x32, 64x64, 128x64, 128x128 */
    if (psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S) phs = 6;
    else if (psm == PSMT8) { pws = 7; phs = 6; }
    else if (psm == PSMT4) { pws = 7; phs = 7; }
    if (!bw) bw = 1;
    return ((h + (1u << phs) - 1) >> phs) * ((bw * 64 + (1u << pws) - 1) >> pws) * 32;
}

/* does the target hold one of the blocks (drawn after their last upload)? */
static int __attribute__((noinline)) rt_reads(const RT *r, const uint16_t *blks, uint32_t nb)
{
    uint32_t i;
    for (i = 0; i < nb; i++)
        if (rt_has_block(r, blks[i]) && gs_blk_ver[blks[i]] <= r->ver)
            return 1;
    return 0;
}

/* gs_backend_sync: small targets drawn since GS memory last had their
 * content, whose drawn area the rectangle reads, oldest first */
void gs_backend_sync(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h)
{
    static uint16_t blks[512];
    RT *cand[RT_MAX], *todo[RT_MAX];
    uint32_t end = bp + span_blocks(psm, bw, h), bw_, bh_, x, y, nb = 0;
    int k, j, i, nc = 0, n = 0, coarse = 0;
    if (!g_gx_ready || (g_gx_flags & 4))
        return;
    /* First by page: nearly every read (a texture's, a palette's at every
     * bind) is of memory no small target spans. */
    if (g_rt_pages_stale) {
        memset(g_rt_pages, 0, sizeof g_rt_pages);
        for (k = 0; k < g_nrt; k++) {
            const RT *r = &g_rts[k];
            if (r->big)
                continue;
            y = (r->fbp + span_blocks(PSMCT32, r->fbw, (uint32_t)r->h) + 31) >> 5;
            for (x = r->fbp >> 5; x < y && x < 512; x++)
                g_rt_pages[x >> 5] |= 1u << (x & 31);
        }
        g_rt_pages_stale = 0;
    }
    y = (end + 31) >> 5;
    if (y > 512)
        y = 512;
    {
        /* pages bp >> 5 .. y - 1 of the bitmap, a word at a time */
        uint32_t lo = bp >> 5, any = 0;
        while (lo < y && !any) {
            const uint32_t w = lo >> 5, hi = y < (w + 1) * 32 ? y : (w + 1) * 32;
            const uint32_t n = hi - lo, m = (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1)) << (lo & 31);
            any = g_rt_pages[w] & m;
            lo = hi;
        }
        if (!any) {
            g_sync_quick++;
            return;
        }
    }
    g_sync_slow++;
    for (k = 0; k < g_nrt; k++) {
        RT *r = &g_rts[k];
        if (r->big || !(r->resident || r->tex_valid) || r->dx1 < r->dx0 || r->fbp >= end ||
            r->fbp + span_blocks(PSMCT32, r->fbw, (uint32_t)r->h) <= bp)
            continue;
        /* A read of the alpha bytes alone and a target whose draws all left
         * alpha as it was (the bloom's buffers and the sky's clouds share the
         * depth buffer's memory that way): nothing of the target is read.
         * Without this the cloud texture had those targets read back and
         * rasterised into memory every frame, and was hashed again each time. */
        if ((psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH) && !r->awrite)
            continue;
        cand[nc++] = r;
    }
    if (!nc)
        return;
    if (!g_blk32_x[31])
        for (y = 0; y < 4; y++)
            for (x = 0; x < 8; x++) {
                g_blk32_x[k_blk32[y][x]] = (uint8_t)x;
                g_blk32_y[k_blk32[y][x]] = (uint8_t)y;
            }
    /* the blocks the read covers */
    psm_block(psm, &bw_, &bh_);
    if (w > 1024) w = 1024;
    if (h > 1024) h = 1024;
    if (((w + bw_ - 1) / bw_) * ((h + bh_ - 1) / bh_) > 512)
        coarse = 1;
    else
        for (y = 0; y < h; y += bh_)
            for (x = 0; x < w; x += bw_)
                blks[nb++] = (uint16_t)(texel_word(psm, bp, bw ? bw : 1, x, y) >> 6);
    for (k = 0; k < nc; k++) {
        RT *r = cand[k];
        if (!coarse && !rt_reads(r, blks, nb))
            continue;
        r->mem_read = g_frame_no;
        if (r->draws == r->mem_draws)
            continue;
        for (j = n++; j > 0 && todo[j - 1]->ver > r->ver; j--)
            todo[j] = todo[j - 1];
        todo[j] = r;
    }
    if (n)
        batch_flush();
    for (k = 0; k < n; k++)
        rt_readback(todo[k]);
}

/* ---- draw state ------------------------------------------------------------------------------ */

typedef struct Cur {
    GsDraw d;
    RT *rt;
    uint32_t seq, gs_ver, clut_ver;  /* seq: GsDraw.seq this state was set up for */
    int skip;                   /* draws not reproduced (logged once) */
    /* per-vertex conversion */
    float zscale, uscale, vscale;
    int fst, iip, fge;
    /* a FIX blend that writes alpha: the PS2 writes the source alpha, GX the
     * blend factor; a second pass writes the alpha (fix2_pass) */
    int fix2, fix_stage, zte2, color_upd, zupd1;
    u8 zfunc1, zfunc2, bm, bs, bd;
} Cur;
static Cur g_cur;

static void log_once(const char *what, uint64_t v)
{
    static struct { const char *w; uint64_t v; } seen[64];
    static int n;
    int k;
    for (k = 0; k < n; k++)
        if (seen[k].w == what && seen[k].v == v)
            return;
    if (n < 64) {
        seen[n].w = what;
        seen[n].v = v;
        n++;
    }
    LOG("gx: %s %llX", what, (unsigned long long)v);
}

/* GX state apply_state sent last, by group; a group is sent again only when
 * it changes. Code that sets these registers itself calls gxc_drop(). */
static struct {
    uint32_t tev;               /* TEV configuration key, 0: unknown */
    uint32_t kc[3];             /* K colours */
    uint32_t pix[4];            /* blend, alpha test, Z, colour/alpha update; pix[0] 0: unknown */
    float place[3];             /* scale, x, y of PNMTX0 */
    int place_valid;
    uint32_t scis[4];
    int scis_valid;
    GXTexObj tex;
    float tm[2];
    int tex_valid;
    int tm_valid;               /* TEXMTX0 holds tm[] (scale) */
    void *tlut;                 /* TLUT0 holds tlut_n entries from here, loaded at TLUT generation tlut_gen */
    uint32_t tlut_n, tlut_gen;
    int tlut_valid;
} g_gxc;

#define TEVC(call) do { if (!tev_same) call; } while (0)

/* g_gxc.tev while the three stages of a car's two passes in one draw are
 * loaded (car_combo); g_combo_call: draw_setup is called for such a draw.
 * Any other draw has its state applied anew: the same GS state as a car's
 * pass 1 would else be drawn through those stages. */
#define COMBO_TEV 0x40000000u
#define NW_CAR_COMBO 20          /* native_mode: both car passes in one draw (two textures) */
#define NW_CAR_COMBO_IMM 21      /* the same with every attribute sent (deformed cars) */
#define NW_SPARK 22              /* the particles' strips: 2D positions and UVs sent, the colour a register (spark_strips) */
#define NW_SKY 23                /* native_mode: the sky's lists: positions and UVs in them, the colour a register */
#define NW_CAR_ENV_DL 24         /* car pass 2 through the batch's list: its sphere map UVs (bytes) by TEX1's index */
static int g_combo_call;

static void gxc_drop(void)
{
    g_gxc.tev = 0;
    memset(g_gxc.pix, 0, sizeof g_gxc.pix);
    g_gxc.place_valid = g_gxc.scis_valid = g_gxc.tex_valid = 0;
    g_gxc.tm_valid = g_gxc.tlut_valid = 0;
    g_gxc.kc[0] = g_gxc.kc[1] = g_gxc.kc[2] = 0xFFFFFFFFu;
}

static const u8 k_atst[8] = {GX_NEVER, GX_ALWAYS, GX_LESS, GX_LEQUAL, GX_EQUAL, GX_GEQUAL, GX_GREATER, GX_NEQUAL};
static const u8 k_ztst[4] = {GX_NEVER, GX_ALWAYS, GX_GEQUAL, GX_GREATER};

static int same_state(const GsDraw *a, const GsDraw *b)
{
    return a->frame == b->frame && a->zbuf == b->zbuf && a->test == b->test && a->alpha == b->alpha &&
           a->tex0 == b->tex0 && a->tex1 == b->tex1 && a->clamp == b->clamp && a->scissor == b->scissor &&
           a->texa == b->texa && a->fogcol == b->fogcol && ((a->prim ^ b->prim) & 0x7F8) == 0 && a->fba == b->fba;
}

/* tex_a8 (I8, 8x4 tiles) from the alpha of a target's RGBA8 copy (4x4 tiles: 16 AR pairs, then 16 GB pairs) */
static void __attribute__((noinline)) rt_alpha_from_tex(RT *r, int w, int h)
{
    const uint8_t *src = r->tex;
    uint8_t *dst = r->tex_a8;
    const int tiles4 = (w + 3) / 4, tiles8 = (w + 7) / 8;
    int x, y;
    batch_flush();
    gp_drain();                         /* the copy is the GPU's: wait for it, and read memory, not the cache */
    DCInvalidateRange(r->tex, GX_GetTexBufferSize((u16)w, (u16)h, GX_TF_RGBA8, GX_FALSE, 0));
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            dst[((y >> 2) * tiles8 + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)] =
                src[((y >> 2) * tiles4 + (x >> 2)) * 64 + ((y & 3) * 4 + (x & 3)) * 2];
    DCFlushRange(dst, GX_GetTexBufferSize((u16)w, (u16)h, GX_TF_I8, GX_FALSE, 0));
    GX_InvalidateTexAll();
    LOG("gx: alpha of target %05X (%dx%d) taken from its copy for a T8H view", r->fbp, w, h);
}

/* What a texture state (TEX0, TEXA, TEX1, CLAMP) last came to, for the next draw with the same one while GS memory
 * and the CLUT are unchanged (gs_ver, gs_clut_ver) and no texture or TLUT was given up (g_tmemo_gen): the texture,
 * its TLUT and the texture object, as tex_lookup, tlut_lookup and the InitTexObj calls made them. Car batches
 * alternate between their passes' states and the single and combined draws (the combined one's TEV makes the next
 * state be applied whole): the same few textures set up again and again. Not with gxflags 4096 (TLUT flips
 * noted). */
typedef struct TexMemo {
    uint64_t tex0, texa, tex1, clamp;
    uint32_t gs_ver, clut_ver, gen;
    Tex *t;
    void *tl;                           /* the TLUT's data, NULL when not paletted */
    Tlut *tls;
    uint16_t n;                         /* its entries */
    GXTexObj to;                        /* after GX_InitTexObjLOD */
} TexMemo;
static TexMemo g_tmemo[4];
static uint32_t g_tmemo_next, g_tmemo_hits, g_tmemo_misses;     /* status line */

#ifndef NO_TMEMO2
/* The validated texture memo (autopilot "tmemo", "tmemon"; docs/plan-60fps.md, "What a texture-memo entry must check"): the second
 * level behind the four entries above, for the draws whose key (TEX0, TEXA, TEX1, CLAMP) came back after the global versions
 * moved on (gs_ver changes 14 times a frame, gs_clut_ver 133: a key is stale before it is used again, and 97 % of such misses
 * came to the texture and TLUT they had). An entry holds what tex_lookup and tlut_lookup came to and is kept while what they
 * depend on is the same:
 *   - g_tmemo_gen: no texture or TLUT given up since;
 *   - the placed record at TBP (gs_placed_for): the same ident, or none then and none now. A placed texture's content is guest
 *     RAM (gs_place marks no page), so its entry needs no page check; the lookup's result on that path is a function of the ident;
 *   - otherwise the pages of the texture's GS memory (tex_pages_ver, from the gs_ver the entry was made or last checked at: the
 *     superset of loc_changed's test, whose blocks lie in them);
 *   - a paletted texture: the content hash of the CLUT entries it uses (gs_clut_hash), taken only when one load put exactly
 *     those entries there (else the entries would be folded and the lazy copy forced: the entry is not used);
 *   - PSMT4HL/HH (decoded through the CLUT, keyed by gs_clut_ver): gs_clut_ver as before.
 * Mode 1 uses it, 2 uses the full lookup and compares its result with the entry's (a texture object that differs, another texture
 * with other texels, another TLUT with other entries are logged), 0 is the four entries alone. tmemon: entries, a power of two up to
 * TM2_MAXN, in sets of four. A three-line entry (key, checks, texture object). Not with gxflags 4096, as the four entries. */
int g_tmemo2 = 1, g_tmemo2_n = 1024;            /* on by default; args.txt "--tmemo 0" or the autopilot's "tmemo 0" turns it off */
#define TM2_MAXN 1024
typedef struct Tm2 {
    uint64_t tex0, texa, tex1, clamp;           /* line 0: the key */
    uint32_t gen, ver, pid, chash;              /* line 1: the checks (chash: the palette's hash; gs_clut_ver for PSMT4HL/HH) */
    Tex *t;
    Tlut *tls;
    void *tl;                                   /* the TLUT's data, NULL when not paletted */
    uint32_t meta;                              /* bit 0 placed, 1 paletted, 2 16 entries (else 256), 3 PSMT4HL/HH, 4..: the CLUT base */
    GXTexObj to;                                /* line 2: after GX_InitTexObjLOD */
} Tm2;
_Static_assert(sizeof(Tm2) == 96, "an entry is three 32-byte lines");
static Tm2 g_tm2[TM2_MAXN] __attribute__((aligned(32)));
static uint32_t g_tm2_tag[TM2_MAXN] __attribute__((aligned(32)));
static uint8_t g_tm2_rr[TM2_MAXN / 4];
enum { TM2_PROBE, TM2_HIT, TM2_NOENT, TM2_GEN, TM2_PLACED, TM2_PAGES, TM2_H4, TM2_NOT_O1, TM2_PAL, TM2_INS, TM2_SKIP, TM2_SAME,
       TM2_BEN, TM2_BAD, TM2_N };
static uint32_t g_tm2_c[TM2_N], g_tm2_bad_total;
static uint32_t g_tm2_tot[3];                   /* since boot (g_tm2_c is the benchmark window's): uses, checked the same objects, same texels */
extern int gs_clut_hash_cheap_h(uint32_t base, int n, uint32_t *h);

static inline uint32_t tm2_hash(const GsDraw *d)
{
    uint32_t h = (uint32_t)d->tex0 * 0x9E3779B1u ^ (uint32_t)(d->tex0 >> 32) * 0x85EBCA6Bu;
    h = (h ^ (h >> 15)) + (uint32_t)d->texa * 0xC2B2AE35u + (uint32_t)d->tex1 * 0x27D4EB2Fu + (uint32_t)d->clamp * 0x165667B1u;
    h ^= h >> 13;
    h *= 0x9E3779B1u;
    return h ^ (h >> 16);
}

/* the pages tex_pages_ver looks at */
static uint32_t tm2_npages(uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th)
{
    uint32_t pw, ph, ppr, rows, cols;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;
    }
    ppr = pw == 128 ? (tbw + 1) / 2 : tbw;
    if (!ppr)
        ppr = 1;
    rows = (th + ph - 1) / ph;
    cols = (tw + pw - 1) / pw;
    if (cols > ppr)
        cols = ppr;
    return rows * (cols + 1);
}

/* the CLUT entry a paletted texture's palette starts at (tlut_lookup's) */
static inline uint32_t tm2_cbase(uint64_t tex0, uint32_t n)
{
    const uint32_t cpsm = GS_FIELD(tex0, 51, 4), csa = GS_FIELD(tex0, 56, 5);
    return n == 16 ? (cpsm == 0 ? (csa & 15) * 16 : csa * 16) : 0;
}

/* the entry for a draw's key if everything it depends on is as it was; pl, pid: the placed record at TBP now */
static inline __attribute__((always_inline)) Tm2 *tm2_find(const GsDraw *d, uint32_t h, uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th,
                     uint32_t ver0, uint32_t cver0, int pl, uint32_t pid)
{
    const uint32_t mask = (((uint32_t)g_tmemo2_n >> 2) - 1) & (TM2_MAXN / 4 - 1), set = (h >> 12) & mask, tag = h | 1;
    const uint32_t *tg = &g_tm2_tag[set * 4];
    Tm2 *e = &g_tm2[set * 4];
    uint32_t w;
    g_tm2_c[TM2_PROBE]++;
    for (w = 0; w < 4; w++)
        if (tg[w] == tag && e[w].tex0 == d->tex0 && e[w].texa == d->texa && e[w].tex1 == d->tex1 && e[w].clamp == d->clamp)
            break;
    if (w == 4) {
        g_tm2_c[TM2_NOENT]++;
        return NULL;
    }
    e += w;
    if (e->gen != g_tmemo_gen) {
        g_tm2_c[TM2_GEN]++;
        return NULL;
    }
    if ((e->meta & 1) != (uint32_t)(pl != 0) || (pl && e->pid != pid)) {
        g_tm2_c[TM2_PLACED]++;
        return NULL;
    }
    if (!pl && tex_pages_ver(tbp, tbw, psm, tw, th) > e->ver) {
        g_tm2_c[TM2_PAGES]++;
        return NULL;
    }
    if (e->meta & 8) {
        if (e->chash != cver0) {
            g_tm2_c[TM2_H4]++;
            return NULL;
        }
    } else if (e->meta & 2) {
        uint32_t gh;
        if (!gs_clut_hash_cheap_h(e->meta >> 4, e->meta & 4 ? 16 : 256, &gh)) {
            g_tm2_c[TM2_NOT_O1]++;
            return NULL;
        }
        if (gh != e->chash) {
            g_tm2_c[TM2_PAL]++;
            return NULL;
        }
    }
    e->ver = ver0;                              /* (it holds as of now) */
    return e;
}

static __attribute__((noinline)) void tm2_insert(const GsDraw *d, uint32_t h, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t ver0,
                       uint32_t cver0, int pl, uint32_t pid, Tex *t, void *tl, int n, const GXTexObj *to)
{
    const uint32_t mask = (((uint32_t)g_tmemo2_n >> 2) - 1) & (TM2_MAXN / 4 - 1), set = (h >> 12) & mask, tag = h | 1;
    uint32_t *tg = &g_tm2_tag[set * 4];
    Tm2 *e = &g_tm2[set * 4];
    uint32_t w, meta = pl ? 1 : 0, chash = 0;
    if (!pl && tm2_npages(tbw, psm, tw, th) > 16) {
        g_tm2_c[TM2_SKIP]++;
        return;
    }
    for (w = 0; w < 4; w++)
        if (tg[w] == tag && e[w].tex0 == d->tex0 && e[w].texa == d->texa && e[w].tex1 == d->tex1 && e[w].clamp == d->clamp)
            break;
    if (w == 4) {
        for (w = 0; w < 4 && tg[w]; w++)
            ;
        if (w == 4)
            w = g_tm2_rr[set]++ & 3;
    }
    e += w;
    if (tl) {
        const uint32_t base = tm2_cbase(d->tex0, (uint32_t)n);
        meta |= 2 | (n == 16 ? 4 : 0) | base << 4;
        chash = gs_clut_hash(base, n);
    } else if (psm == PSMT4HL || psm == PSMT4HH) {
        meta |= 8;
        chash = cver0;
    }
    tg[w] = 0;
    e->tex0 = d->tex0;
    e->texa = d->texa;
    e->tex1 = d->tex1;
    e->clamp = d->clamp;
    e->gen = g_tmemo_gen;
    e->ver = ver0;
    e->pid = pid;
    e->chash = chash;
    e->t = t;
    e->tl = tl;
    e->tls = tl ? &g_tluts[((uint8_t *)tl - (uint8_t *)g_texmem) / TLUT_SLOT] : NULL;
    e->meta = meta;
    e->to = *to;
    tg[w] = tag;
    g_tm2_c[TM2_INS]++;
}

/* mode 2: an entry that would have been used, against what the lookups came to */
static __attribute__((noinline)) void tm2_compare(const Tm2 *m, const Tex *t, const void *tl, int n, const GXTexObj *to, const GsDraw *d)
{
    const char *why = NULL;
    const int tex_same = m->t == t, tl_same = m->tl == tl;
    if (!tex_same && (m->t->w != t->w || m->t->h != t->h || m->t->fmt != t->fmt ||
                      memcmp(m->t->data, t->data, GX_GetTexBufferSize((u16)t->w, (u16)t->h, t->fmt, GX_FALSE, 0))))
        why = "another texture with other texels";
    else if (!tl_same && (!m->tl != !tl || (tl && memcmp(m->tl, tl, (uint32_t)n * 2))))
        why = "another TLUT with other entries";
    else if (tex_same && tl_same && memcmp(&m->to, to, sizeof *to))
        why = "a texture object that differs";
    if (!why) {
        g_tm2_c[tex_same && tl_same ? TM2_SAME : TM2_BEN]++;
        g_tm2_tot[tex_same && tl_same ? 1 : 2]++;
        return;
    }
    g_tm2_c[TM2_BAD]++;
    if (++g_tm2_bad_total <= 30)
        LOG("tmemo2: MISMATCH (%s): tex0 %016llX texa %016llX tex1 %llX clamp %llX placed %u gen %u/%u clut_ver %u", why,
            (unsigned long long)d->tex0, (unsigned long long)d->texa, (unsigned long long)d->tex1,
            (unsigned long long)d->clamp, m->meta & 1, m->gen, g_tmemo_gen, gs_clut_ver);
}

static void tm2_reset(void)
{
    memset(g_tm2_c, 0, sizeof g_tm2_c);
}

static void tm2_report(uint32_t frames)
{
    const uint32_t *c = g_tm2_c;
    if (!g_tmemo2)
        return;
    LOG("tmemo2: mode %d, %d entries, %u frames (totals): probes %u, used %u (check mode: %u of them the same objects, %u other objects "
        "with the same texels, %u MISMATCHES, %u since boot); not used: no entry %u, generation %u, placed record %u, pages %u, "
        "PSMT4H clut_ver %u, palette hash not from a load %u, palette changed %u; entries made %u, too many pages %u", g_tmemo2,
        g_tmemo2_n, frames, c[TM2_PROBE], c[TM2_HIT], c[TM2_SAME], c[TM2_BEN], c[TM2_BAD], g_tm2_bad_total, c[TM2_NOENT],
        c[TM2_GEN], c[TM2_PLACED], c[TM2_PAGES], c[TM2_H4], c[TM2_NOT_O1], c[TM2_PAL], c[TM2_INS], c[TM2_SKIP]);
}

/* the status block's line (every 10 s): the totals since boot, so a check-mode run over any scene shows what was checked */
static void tm2_status(void)
{
    if (!g_tmemo2)
        return;
    LOG("gx: texture memo %s, %d entries: %u uses so far%s", g_tmemo2 == 2 ? "in check mode" : "on", g_tmemo2_n, g_tm2_tot[0],
        g_tmemo2 == 2 ? "" : " (not in check mode: no counts)");
    if (g_tmemo2 == 2)
        LOG("gx: texture memo checks so far: %u the same objects as the full lookup, %u other objects with the same texels, %u MISMATCHES",
            g_tm2_tot[1], g_tm2_tot[2], g_tm2_bad_total);
}
#endif

#ifdef MEMOSTAT
/* -DMEMOSTAT (debug, off by default; the model runs of docs/plan-60fps.md): what the memo's misses are, and what a memo validated by
 * the texture's own pages and palette would have made of them. Every draw that goes through the memo is noted (ms_note) with its key
 * (TEX0, TEXA, TEX1, CLAMP, as a 64-bit hash) and the versions the memo checks (gs_ver, gs_clut_ver, g_tmemo_gen). A miss is new (the
 * key was never noted: a bit set of 2^21 bits), old (it was, but not among the MS_N keys most recently used, which a table of 4 ways
 * keeps in LRU order), a capacity miss (it is in the table with the versions it has now: an LRU memo of more entries would have hit if
 * fewer keys were used since; counted by that distance), or stale (the versions differ: 1 gs_ver, 2 clut_ver, 4 gen); a stale one is
 * counted again when the lookup came to the same texture and TLUT as the entry's.
 * For the stale ones with an entry (the entry is what the key last came to) the checks of the validated memo are made in order: the
 * generation is the same, the entry was made by lookups that wrote nothing, none of the texture's pages was written since (the entry's
 * gs_ver against tex_pages_ver), the placed record is the same (its version, 0: none), clut_ver is the same for PSMT4HL/HH, the TLUT
 * hash is the same for a paletted texture, and gs_clut_hash would answer from a load's hash (gs_clut_hash_cheap). When the first six
 * hold the memo would have given the entry's texture: if the lookup came to another object, its texels and TLUT bytes are compared
 * (a difference is a violation: a check that missed a change). The reuse of the stale entries is counted in lookups, in distinct keys
 * used since (the entries of the table used later) and in frames. Counted from the benchmark's first frame to its last (wh_bench).
 * The memory is MEM1's bss: 450 KB, near what the other bigger tables ran out of (the "mem:" line's arena margin). */
#define MS_N 4096
#define MS_SETS (MS_N / 4)
#define MS_BITS (1u << 21)
enum { MS_LOOK, MS_HIT, MS_MISS, MS_NEW, MS_CAP, MS_STALE, MS_OLD, MS_KEYS, MS_CAPD = 8 /* 8 */, MS_MASK = 16 /* 8 */,
       MS_SAME = 24 /* 8 */, MS_VCHG = 32 /* 3 */, MS_W = 36 /* 9: stale, same result, then the checks one after the other */,
       MS_BEN = 45, MS_VIO = 46, MS_D7 = 47, MS_D8 = 48, MS_PATH = 49 /* 5 x (misses, stale, convertible, decoded) */,
       MS_FMT = 69 /* 9 x (lookups, misses, convertible) */, MS_CI = 96 /* 4 */, MS_PG = 100 /* 6 */, MS_PGD = 106 /* 6 */,
       MS_AL = 112 /* 2 */, MS_LS = 114 /* 8 */, MS_LSD = 122 /* 8 */, MS_DK = 130 /* 11 */, MS_DKD = 141 /* 11 */,
       MS_FR = 152 /* 8 */, MS_FRD = 160 /* 8 */, MS_IND = 168 /* 12: the checks one by one */, MS_NB = 180 /* 8 */,
       MS_NP = 188 /* 6 */, MS_NC = 194 };
typedef struct MsEnt {
    uint64_t kh;                        /* a 64-bit hash of the key; 0: empty */
    uint32_t gs_ver, clut_ver, gen, use;
    uint32_t res;                       /* a hash of what the lookup came to: the texture, its content, the TLUT, its content */
    uint32_t tlh, t, tl, pver, pid;     /* the TLUT's hash, the texture's and the TLUT's addresses, the placed record's version and ident */
    uint16_t frame;
    uint8_t okv, placed;                /* the lookups wrote nothing (the memo would have kept the entry); the lookup took the placed path */
} MsEnt;
static MsEnt g_ms[MS_N];
static uint32_t g_ms_seen[MS_BITS / 32];
static uint32_t g_ms_clock, g_ms_c[MS_NC], g_ms_last[3];
static uint32_t g_ms_snap[6], g_ms_pver, g_ms_pid;
static int g_ms_cheap, g_ms_placed;
extern int gs_clut_hash_cheap(uint32_t base, int n);
extern uint32_t gs_placed_ver(uint32_t tbp);

/* before the memo is looked at: the counters of the lookup paths, the placed record, whether the palette's hash is a load's */
static void ms_pre(const GsDraw *d)
{
    const uint32_t psm = GS_FIELD(d->tex0, 20, 6), cpsm = GS_FIELD(d->tex0, 51, 4), csa = GS_FIELD(d->tex0, 56, 5);
    g_ms_snap[0] = g_lk_placed;
    g_ms_snap[1] = g_lk_upload;
    g_ms_snap[2] = g_lk_known;
    g_ms_snap[3] = g_lk_cover;
    g_ms_snap[4] = g_lk_hash;
    g_ms_snap[5] = g_tex_from_upload + g_tex_from_mem;
    g_ms_pver = gs_placed_ver(GS_FIELD(d->tex0, 0, 14));
    g_ms_pid = 0;
    g_ms_placed = gs_placed_for(GS_FIELD(d->tex0, 0, 14), &g_ms_pid);
    g_ms_cheap = 1;
    if (psm == PSMT4 || psm == PSMT8 || psm == PSMT8H) {
        const int n = psm == PSMT4 ? 16 : 256;
        g_ms_cheap = gs_clut_hash_cheap(n == 16 ? (cpsm == 0 ? (csa & 15) * 16 : csa * 16) : 0, n);
    }
}

static int ms_fmt(uint32_t psm)
{
    switch (psm) {
    case PSMCT32: return 0;
    case PSMCT24: return 1;
    case PSMCT16: case PSMCT16S: return 2;
    case PSMT8: return 3;
    case PSMT4: return 4;
    case PSMT8H: return 5;
    case PSMT4HL: return 6;
    case PSMT4HH: return 7;
    default: return 8;
    }
}

/* the pages tex_pages_ver looks at */
static uint32_t ms_npages(uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th)
{
    uint32_t pw, ph, ppr, rows, cols;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: pw = 64; ph = 64; break;
    case PSMT8: pw = 128; ph = 64; break;
    case PSMT4: pw = 128; ph = 128; break;
    default: pw = 64; ph = 32; break;
    }
    ppr = pw == 128 ? (tbw + 1) / 2 : tbw;
    if (!ppr)
        ppr = 1;
    rows = (th + ph - 1) / ph;
    cols = (tw + pw - 1) / pw;
    if (cols > ppr)
        cols = ppr;
    return rows * (cols + 1);
}

static int ms_b6(uint32_t n)
{
    return n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : 5;
}

static void __attribute__((noinline)) ms_note(const GsDraw *d, int hit, uint32_t ver0, uint32_t cver0, uint32_t gen0, Tex *t,
                                              void *tl, int valid)
{
    const uint32_t tlh = tl ? g_tluts[((uint8_t *)tl - (uint8_t *)g_texmem) / TLUT_SLOT].hash : 0;
    const uint32_t res = (uint32_t)(uintptr_t)t * 0x9E3779B1u ^ t->hash * 0x85EBCA6Bu ^ (uint32_t)(uintptr_t)tl * 0xC2B2AE35u ^
                         tlh * 0x27D4EB2Fu;
    const uint32_t psm = GS_FIELD(d->tex0, 20, 6), tbp = GS_FIELD(d->tex0, 0, 14), tbw = GS_FIELD(d->tex0, 14, 6);
    const int ci = tl != NULL, fc = ms_fmt(psm);
    uint32_t tw = 1u << GS_FIELD(d->tex0, 26, 4), th = 1u << GS_FIELD(d->tex0, 30, 4);
    uint32_t npg, bit;
    uint64_t kh = d->tex0 * 0x9E3779B97F4A7C15ull;
    MsEnt *set, *e = NULL, *victim;
    int k, dist = 0, seen, path = 0, decoded = 0;
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;
    npg = ms_npages(tbw, psm, tw, th);
    kh = (kh ^ (kh >> 29)) + d->texa * 0xC2B2AE3D27D4EB4Full;
    kh = (kh ^ (kh >> 31)) + d->tex1 * 0x165667B19E3779F9ull;
    kh = (kh ^ (kh >> 27)) + d->clamp * 0x27D4EB2F165667C5ull;
    kh ^= kh >> 32;
    kh |= 1;
    g_ms_c[MS_LOOK]++;
    g_ms_c[MS_FMT + fc * 3]++;
    g_ms_c[MS_PG + ms_b6(npg)]++;
    if (!(tbp & 31))
        g_ms_c[MS_AL]++;
    if (ci) {
        g_ms_c[MS_CI]++;
        g_ms_c[MS_CI + 1] += g_ms_cheap;
    }
    if (g_ms_last[0] != ver0) { g_ms_c[MS_VCHG]++; g_ms_last[0] = ver0; }
    if (g_ms_last[1] != cver0) { g_ms_c[MS_VCHG + 1]++; g_ms_last[1] = cver0; }
    if (g_ms_last[2] != gen0) { g_ms_c[MS_VCHG + 2]++; g_ms_last[2] = gen0; }
    bit = (uint32_t)(kh >> 11) & (MS_BITS - 1);
    seen = (g_ms_seen[bit >> 5] >> (bit & 31)) & 1;
    g_ms_seen[bit >> 5] |= 1u << (bit & 31);
    set = &g_ms[((uint32_t)(kh >> 40) % MS_SETS) * 4];
    victim = set;
    for (k = 0; k < 4; k++) {
        if (set[k].kh == kh)
            e = &set[k];
        if (set[k].kh == 0 || (victim->kh != 0 && set[k].use < victim->use))
            victim = &set[k];
    }
    if (hit) {
        g_ms_c[MS_HIT]++;
    } else {
        g_ms_c[MS_MISS]++;
        g_ms_c[MS_FMT + fc * 3 + 1]++;
        path = g_lk_placed != g_ms_snap[0] ? 0 : g_lk_upload != g_ms_snap[1] ? 1 : g_lk_known != g_ms_snap[2] ? 2 :
               g_lk_cover != g_ms_snap[3] ? 3 : 4;
        decoded = g_tex_from_upload + g_tex_from_mem != g_ms_snap[5];
        g_ms_c[MS_PATH + path * 4]++;
        g_ms_c[MS_PATH + path * 4 + 3] += decoded;
        if (!seen) {
            g_ms_c[MS_NEW]++;
        } else if (!e) {
            g_ms_c[MS_OLD]++;
        } else {
            const uint32_t mask = (e->gs_ver != ver0) | (e->clut_ver != cver0) << 1 | (e->gen != gen0) << 2;
            if (!mask) {
                for (k = 0; k < MS_N; k++)
                    if (g_ms[k].kh && g_ms[k].use > e->use)
                        dist++;
                g_ms_c[MS_CAP]++;
                g_ms_c[MS_CAPD + (dist < 4 ? 0 : dist < 8 ? 1 : dist < 16 ? 2 : dist < 32 ? 3 : dist < 64 ? 4 : dist < 128 ? 5 :
                                  dist < 256 ? 6 : 7)]++;
            } else {
                /* the checks of a memo validated by the texture's own memory and palette, in order; i: how many hold. A texture the
                 * lookup took from a placed record (no page of GS memory is its content) is checked by the record's ident; any
                 * other by the blocks it covers (loc_changed's test, from the gs_ver the entry was made at) and no record now. */
                const int pre = !(mask & 4) && e->okv;
                const int cp = e->placed == (g_ms_placed != 0) && (!g_ms_placed || e->pid == g_ms_pid);
                int tm = 1, blk_ok = 1, pg_lvl = 1;
                int c[7];
                const int R = e->res == res;
                const uint32_t since = g_ms_clock - e->use, fs = (uint16_t)((uint16_t)g_frame_no - e->frame);
                int i, convertible = 0, ls, fr, b;
                if (pre && !e->placed) {
                    TexLoc ml;
                    ml.tbp = tbp; ml.tbw = tbw; ml.psm = psm; ml.tw = tw; ml.th = th;
                    loc_blocks(&ml);
                    ml.checked_ver = e->gs_ver;
                    blk_ok = !loc_changed(&ml);
                    pg_lvl = tex_pages_ver(tbp, tbw, psm, tw, th) <= e->gs_ver;
                    tm = blk_ok;
                    g_ms_c[MS_NB + (!ml.nblk ? 0 : ml.nblk == 1 ? 1 : ml.nblk <= 4 ? 2 : ml.nblk <= 8 ? 3 : ml.nblk <= 16 ? 4 :
                                    ml.nblk <= 32 ? 5 : ml.nblk <= 64 ? 6 : 7)]++;
                    g_ms_c[MS_NP + (!ml.npg ? 0 : ml.npg == 1 ? 1 : ml.npg == 2 ? 2 : ml.npg <= 4 ? 3 : ml.npg <= 8 ? 4 : 5)]++;
                }
                c[0] = !(mask & 4);
                c[1] = e->okv;
                c[2] = cp;
                c[3] = tm;
                c[4] = !((psm == PSMT4HL || psm == PSMT4HH) && (mask & 2));
                c[5] = !ci || e->tlh == tlh;
                c[6] = g_ms_cheap;
                if (pre) {
                    g_ms_c[MS_IND]++;
                    g_ms_c[MS_IND + 1] += cp;
                    g_ms_c[MS_IND + 7] += ci && e->tlh == tlh;
                    g_ms_c[MS_IND + 11] += ci;
                    if (e->placed) {
                        g_ms_c[MS_IND + 2]++;
                        g_ms_c[MS_IND + 8] += g_ms_placed && e->pid == g_ms_pid;
                        g_ms_c[MS_IND + 9] += g_ms_placed && e->pver == g_ms_pver;
                        g_ms_c[MS_IND + 10] += !g_ms_placed;
                    } else {
                        g_ms_c[MS_IND + 3]++;
                        g_ms_c[MS_IND + 4] += pg_lvl;
                        g_ms_c[MS_IND + 5] += blk_ok;
                        g_ms_c[MS_IND + 6] += !g_ms_placed;
                    }
                }
                g_ms_c[MS_STALE]++;
                g_ms_c[MS_MASK + mask]++;
                if (R)
                    g_ms_c[MS_SAME + mask]++;
                for (k = 0; k < MS_N; k++)
                    if (g_ms[k].kh && g_ms[k].use > e->use)
                        dist++;
                g_ms_c[MS_W]++;
                g_ms_c[MS_W + 1] += R;
                for (i = 0; i < 7 && c[i]; i++)
                    g_ms_c[MS_W + 2 + i]++;
                if (i >= 6) {
                    int benign = 0;
                    if (ci) {
                        g_ms_c[MS_CI + 2]++;
                        g_ms_c[MS_CI + 3] += g_ms_cheap;
                    }
                    if (!R) {
                        const Tex *et = (const Tex *)(uintptr_t)e->t;
                        benign = et->w == t->w && et->h == t->h && et->fmt == t->fmt &&
                                 !memcmp(et->data, t->data, GX_GetTexBufferSize((u16)t->w, (u16)t->h, t->fmt, GX_FALSE, 0)) &&
                                 (!ci || !memcmp((const void *)(uintptr_t)e->tl, tl, (t->fmt == GX_TF_CI4 ? 16u : 256u) * 2));
                        g_ms_c[benign ? MS_BEN : MS_VIO]++;
                    }
                    if (R || benign) {
                        g_ms_c[MS_D7]++;
                        if (g_ms_cheap) {
                            convertible = 1;
                            g_ms_c[MS_D8]++;
                        }
                    }
                }
                ls = 0;
                while (ls < 7 && since >= (4u << (2 * ls)))
                    ls++;
                b = 0;
                while (b < 10 && (uint32_t)dist >= (4u << b))
                    b++;
                fr = fs == 0 ? 0 : fs == 1 ? 1 : fs == 2 ? 2 : fs <= 4 ? 3 : fs <= 8 ? 4 : fs <= 16 ? 5 : fs <= 32 ? 6 : 7;
                g_ms_c[MS_PATH + path * 4 + 1]++;
                g_ms_c[MS_LS + ls]++;
                g_ms_c[MS_DK + b]++;
                g_ms_c[MS_FR + fr]++;
                if (convertible) {
                    g_ms_c[MS_PATH + path * 4 + 2]++;
                    g_ms_c[MS_FMT + fc * 3 + 2]++;
                    g_ms_c[MS_PGD + ms_b6(npg)]++;
                    if (!(tbp & 31))
                        g_ms_c[MS_AL + 1]++;
                    g_ms_c[MS_LSD + ls]++;
                    g_ms_c[MS_DKD + b]++;
                    g_ms_c[MS_FRD + fr]++;
                }
            }
        }
    }
    if (!e) {
        e = victim;
        e->kh = kh;
    }
    e->gs_ver = valid ? ver0 : 0xFFFFFFFFu;
    e->clut_ver = valid ? cver0 : 0xFFFFFFFFu;
    e->gen = g_tmemo_gen;
    e->use = ++g_ms_clock;
    e->res = res;
    e->tlh = tlh;
    e->t = (uint32_t)(uintptr_t)t;
    e->tl = (uint32_t)(uintptr_t)tl;
    e->pver = g_ms_pver;
    e->pid = g_ms_pid;
    e->placed = (uint8_t)(g_ms_placed != 0);
    e->frame = (uint16_t)g_frame_no;
    e->okv = (uint8_t)valid;
}

static void ms_reset(void)
{
    memset(g_ms_c, 0, sizeof g_ms_c);
}

static void ms_report(uint32_t frames)
{
    static const char *const path[5] = {"placed", "upload", "known", "blocks", "hashed"};
    static const char *const fmt[9] = {"CT32", "CT24", "CT16", "T8", "T4", "T8H", "T4HL", "T4HH", "other"};
    const uint32_t *c = g_ms_c;
    uint32_t m, k, set = 0;
    for (k = 0; k < MS_BITS / 32; k++)
        set += (uint32_t)__builtin_popcount(g_ms_seen[k]);
    LOG("memostat over %u frames (totals): lookups %u, hits %u, misses %u = new %u + old %u + capacity %u + stale %u; keys ever seen "
        "about %u (%u of %u bits set); versions changed between lookups: gs_ver %u, clut_ver %u, gen %u", frames, c[MS_LOOK],
        c[MS_HIT], c[MS_MISS], c[MS_NEW], c[MS_OLD], c[MS_CAP], c[MS_STALE],
        (uint32_t)(-(double)MS_BITS * __builtin_log(1.0 - (double)set / MS_BITS)), set, MS_BITS, c[MS_VCHG], c[MS_VCHG + 1],
        c[MS_VCHG + 2]);
    LOG("memostat capacity misses by the keys used since (an LRU memo of N entries hits when fewer than N): <4 %u, <8 %u, <16 %u, "
        "<32 %u, <64 %u, <128 %u, <256 %u, more %u", c[MS_CAPD], c[MS_CAPD + 1], c[MS_CAPD + 2], c[MS_CAPD + 3], c[MS_CAPD + 4],
        c[MS_CAPD + 5], c[MS_CAPD + 6], c[MS_CAPD + 7]);
    for (m = 1; m < 8; m++)
        if (c[MS_MASK + m])
            LOG("memostat stale, differing versions %u (1 gs_ver, 2 clut_ver, 4 gen): %u, of them the same texture and TLUT after "
                "the lookup %u", m, c[MS_MASK + m], c[MS_SAME + m]);
    LOG("memostat checks, stale misses with an entry %u, of them the same texture and TLUT after the lookup %u; then cumulative: gen "
        "same %u, entry made without a write %u, placed state the same %u, texture blocks unchanged (entries not placed) %u",
        c[MS_W], c[MS_W + 1], c[MS_W + 2], c[MS_W + 3], c[MS_W + 4], c[MS_W + 5]);
    LOG("memostat checks one by one, stale with gen same and a clean entry %u: placed state the same %u; placed entries %u (same ident "
        "%u, same record version %u, no record now %u); other entries %u (pages unchanged %u, blocks unchanged %u, no record now %u); "
        "paletted %u, TLUT hash same %u", c[MS_IND], c[MS_IND + 1], c[MS_IND + 2], c[MS_IND + 8], c[MS_IND + 9], c[MS_IND + 10],
        c[MS_IND + 3], c[MS_IND + 4], c[MS_IND + 5], c[MS_IND + 6], c[MS_IND + 11], c[MS_IND + 7]);
    LOG("memostat other entries: blocks the texture covers (over 128, 1, 2-4, 5-8, 9-16, 17-32, 33-64, 65-128): %u %u %u %u %u %u %u "
        "%u; pages they are in (none or over 16, 1, 2, 3-4, 5-8, 9-16): %u %u %u %u %u %u", c[MS_NB], c[MS_NB + 1], c[MS_NB + 2],
        c[MS_NB + 3], c[MS_NB + 4], c[MS_NB + 5], c[MS_NB + 6], c[MS_NB + 7], c[MS_NP], c[MS_NP + 1], c[MS_NP + 2], c[MS_NP + 3],
        c[MS_NP + 4], c[MS_NP + 5]);
    LOG("memostat checks, cumulative: + clut_ver same for 4HL/HH %u, + TLUT hash same %u (the exactness checks), + gs_clut_hash O(1) "
        "%u; of those through the TLUT hash: same result %u, other object with the same texels %u, DIFFERENT %u (violations); "
        "convertible without/with the O(1) check %u/%u", c[MS_W + 6], c[MS_W + 7], c[MS_W + 8], c[MS_D7] - c[MS_BEN], c[MS_BEN],
        c[MS_VIO], c[MS_D7], c[MS_D8]);
    LOG("memostat paletted: lookups %u, gs_clut_hash O(1) at the check %u; among the stale misses through the exactness checks "
        "%u, O(1) %u", c[MS_CI], c[MS_CI + 1], c[MS_CI + 2], c[MS_CI + 3]);
    for (k = 0; k < 5; k++)
        LOG("memostat path %s: misses %u, stale with an entry %u, convertible %u, decoded a texture %u", path[k], c[MS_PATH + k * 4],
            c[MS_PATH + k * 4 + 1], c[MS_PATH + k * 4 + 2], c[MS_PATH + k * 4 + 3]);
    LOG("memostat formats lookups/misses/convertible: %s %u/%u/%u, %s %u/%u/%u, %s %u/%u/%u, %s %u/%u/%u, %s %u/%u/%u, %s %u/%u/%u, "
        "%s %u/%u/%u, %s %u/%u/%u, %s %u/%u/%u", fmt[0], c[MS_FMT], c[MS_FMT + 1], c[MS_FMT + 2], fmt[1], c[MS_FMT + 3],
        c[MS_FMT + 4], c[MS_FMT + 5], fmt[2], c[MS_FMT + 6], c[MS_FMT + 7], c[MS_FMT + 8], fmt[3], c[MS_FMT + 9], c[MS_FMT + 10],
        c[MS_FMT + 11], fmt[4], c[MS_FMT + 12], c[MS_FMT + 13], c[MS_FMT + 14], fmt[5], c[MS_FMT + 15], c[MS_FMT + 16],
        c[MS_FMT + 17], fmt[6], c[MS_FMT + 18], c[MS_FMT + 19], c[MS_FMT + 20], fmt[7], c[MS_FMT + 21], c[MS_FMT + 22],
        c[MS_FMT + 23], fmt[8], c[MS_FMT + 24], c[MS_FMT + 25], c[MS_FMT + 26]);
    LOG("memostat pages a texture covers (1, 2, 3-4, 5-8, 9-16, 17+): all lookups %u %u %u %u %u %u; convertible %u %u %u %u %u %u; "
        "base page aligned: lookups %u of %u, convertible %u of %u", c[MS_PG], c[MS_PG + 1], c[MS_PG + 2], c[MS_PG + 3],
        c[MS_PG + 4], c[MS_PG + 5], c[MS_PGD], c[MS_PGD + 1], c[MS_PGD + 2], c[MS_PGD + 3], c[MS_PGD + 4], c[MS_PGD + 5],
        c[MS_AL], c[MS_LOOK], c[MS_AL + 1], c[MS_D8]);
    LOG("memostat lookups since the key's last use (<4 <16 <64 <256 <1k <4k <16k more), stale: %u %u %u %u %u %u %u %u; convertible: "
        "%u %u %u %u %u %u %u %u", c[MS_LS], c[MS_LS + 1], c[MS_LS + 2], c[MS_LS + 3], c[MS_LS + 4], c[MS_LS + 5], c[MS_LS + 6],
        c[MS_LS + 7], c[MS_LSD], c[MS_LSD + 1], c[MS_LSD + 2], c[MS_LSD + 3], c[MS_LSD + 4], c[MS_LSD + 5], c[MS_LSD + 6],
        c[MS_LSD + 7]);
    LOG("memostat distinct keys used since (<4 <8 <16 <32 <64 <128 <256 <512 <1k <2k more), stale: %u %u %u %u %u %u %u %u %u %u %u",
        c[MS_DK], c[MS_DK + 1], c[MS_DK + 2], c[MS_DK + 3], c[MS_DK + 4], c[MS_DK + 5], c[MS_DK + 6], c[MS_DK + 7], c[MS_DK + 8],
        c[MS_DK + 9], c[MS_DK + 10]);
    LOG("memostat distinct keys used since, convertible: %u %u %u %u %u %u %u %u %u %u %u", c[MS_DKD], c[MS_DKD + 1], c[MS_DKD + 2],
        c[MS_DKD + 3], c[MS_DKD + 4], c[MS_DKD + 5], c[MS_DKD + 6], c[MS_DKD + 7], c[MS_DKD + 8], c[MS_DKD + 9], c[MS_DKD + 10]);
    LOG("memostat frames since the key's last use (0 1 2 3-4 5-8 9-16 17-32 more), stale: %u %u %u %u %u %u %u %u; convertible: %u %u "
        "%u %u %u %u %u %u", c[MS_FR], c[MS_FR + 1], c[MS_FR + 2], c[MS_FR + 3], c[MS_FR + 4], c[MS_FR + 5], c[MS_FR + 6],
        c[MS_FR + 7], c[MS_FRD], c[MS_FRD + 1], c[MS_FRD + 2], c[MS_FRD + 3], c[MS_FRD + 4], c[MS_FRD + 5], c[MS_FRD + 6],
        c[MS_FRD + 7]);
}
#endif

/* n entries of TLUT data into TLUT0, unless it holds them already (the slot not given up since: the memo's
 * generation); a load makes the texture object be loaded again, as it always did */
static void tlut_load(void *tl, uint32_t n)
{
    GXTlutObj lo;
    if (g_gxc.tlut_valid && g_gxc.tlut == tl && g_gxc.tlut_n == n && g_gxc.tlut_gen == g_tmemo_gen && g_opt_new)
        return;
    GX_InitTlutObj(&lo, tl, GX_TL_RGB5A3, (u16)n);
    GX_LoadTlut(&lo, GX_TLUT0);
    g_gxc.tex_valid = 0;
    g_gxc.tlut = tl;
    g_gxc.tlut_n = n;
    g_gxc.tlut_gen = g_tmemo_gen;
    g_gxc.tlut_valid = 1;
}

/* The scales apply_state divides by a power of two: a render target's shift (1 / 2^sh) and a texture's side (1 / (16 * 2^k),
 * k = 0..10, the GS's TW and TH clamped to 1024). A division by a power of two is exact, so these are the quotients;
 * fdivs takes 17 cycles and nothing else runs in the FPU meanwhile (gx_init checks the tables against the divisions). */
static const float k_rcp_pow2[31] = {
    0x1p-0f, 0x1p-1f, 0x1p-2f, 0x1p-3f, 0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f, 0x1p-8f, 0x1p-9f, 0x1p-10f,
    0x1p-11f, 0x1p-12f, 0x1p-13f, 0x1p-14f, 0x1p-15f, 0x1p-16f, 0x1p-17f, 0x1p-18f, 0x1p-19f, 0x1p-20f, 0x1p-21f,
    0x1p-22f, 0x1p-23f, 0x1p-24f, 0x1p-25f, 0x1p-26f, 0x1p-27f, 0x1p-28f, 0x1p-29f, 0x1p-30f};
static const float k_rcp16_pow2[11] = {0x1p-4f, 0x1p-5f, 0x1p-6f, 0x1p-7f, 0x1p-8f, 0x1p-9f,
                                       0x1p-10f, 0x1p-11f, 0x1p-12f, 0x1p-13f, 0x1p-14f};

static void rcp_tables_check(void)
{
    volatile float one = 1.0f, sixteen = 16.0f;     /* (the divisions as the processor does them) */
    int k, bad = 0;
    for (k = 0; k < 31; k++)
        bad += k_rcp_pow2[k] != one / (float)(1 << k);
    for (k = 0; k <= 10; k++)
        bad += k_rcp16_pow2[k] != one / (sixteen * (float)(1u << k));
    if (bad)
        LOG("gx: the reciprocal tables differ from the divisions in %d places", bad);
}

/* set up GX for a draw; returns 0 when the draw is skipped */
extern int g_wstat;
static uint64_t g_as_t[12], g_as_n;
#define AST(k) do { if (g_wstat == 7) { const uint64_t n_ = gettime(); g_as_t[k] += n_ - as_t; as_t = n_; } } while (0)
static int apply_state(const GsDraw *d, RT *rt)
{
    uint64_t as_t = g_wstat == 7 ? gettime() : 0;
    uint32_t tme = (d->prim >> 4) & 1, fge = (d->prim >> 5) & 1, abe = (d->prim >> 6) & 1;
    uint32_t fst = (d->prim >> 8) & 1;
    uint32_t A = GS_FIELD(d->alpha, 0, 2), B = GS_FIELD(d->alpha, 2, 2), C = GS_FIELD(d->alpha, 4, 2);
    uint32_t D = GS_FIELD(d->alpha, 6, 2), fix = GS_FIELD(d->alpha, 32, 8);
    uint32_t ate = GS_FIELD(d->test, 0, 1), atst = GS_FIELD(d->test, 1, 3), aref = GS_FIELD(d->test, 4, 8);
    uint32_t afail = GS_FIELD(d->test, 12, 2), zte = GS_FIELD(d->test, 16, 1), ztst = GS_FIELD(d->test, 17, 2);
    uint32_t zmsk = GS_FIELD(d->zbuf, 32, 1), zpsm = GS_FIELD(d->zbuf, 24, 4);
    uint32_t fpsm = GS_FIELD(d->frame, 24, 6), fbmsk = GS_FIELD(d->frame, 32, 32);
    uint32_t tfx = GS_FIELD(d->tex0, 35, 2), tcc = GS_FIELD(d->tex0, 34, 1);
    int color_upd = 1, alpha_upd = 1, z_upd = !zmsk, a_test = ate, fix_alpha = 0, sub_premul = 0, stage = 0;
    uint32_t tev_key;
    int tev_same;
    u8 bm = GX_BM_NONE, bs = GX_BL_ONE, bd = GX_BL_ZERO;
    Mtx m;
    float s = (unsigned)rt->sh < 31u ? k_rcp_pow2[rt->sh] : 1.0f / (float)(1 << rt->sh);

    /* frame buffer write mask */
    if (fpsm == PSMCT16 || fpsm == PSMCT16S) {
        uint32_t tpsm = GS_FIELD(d->tex0, 20, 6);
        if (tme && fbmsk == 0x3FFF && (tpsm == PSMZ32 || tpsm == PSMZ24 || tpsm == PSMZ16 || tpsm == PSMZ16S)) {
            /* The depth haze: 16-bit views of the colour and Z buffers put
             * Z bits 8-15 into the alpha bytes (measured on the reference
             * renderer), then the alpha is read as T8H through a palette.
             * The EFB depth copy (Z8M) gives that byte directly when the
             * alpha is read: remember it, draw nothing. */
            rt->zalpha = g_frame_no;
            return 0;
        }
        if (fbmsk & 0x7FFF) {
            log_once("skipped 16-bit masked draw, fbmsk", fbmsk);
            return 0;
        }
        rt->awrite = 1;
    } else {
        if ((fbmsk & 0xFFFFFF) == 0xFFFFFF)
            color_upd = 0;
        else if (fbmsk & 0xFFFFFF)
            log_once("partial colour mask", fbmsk);
        if ((fbmsk >> 24) == 0xFF)
            alpha_upd = 0;
        else
            rt->awrite = 1;
    }

    /* alpha test, with the fail modes that need no second pass */
    if (ate && atst == 0) {             /* NEVER: everything takes the fail path */
        a_test = 0;
        switch (afail) {
        case 0: return 0;
        case 1: z_upd = 0; break;
        case 2: color_upd = 0; alpha_upd = 0; break;
        case 3: alpha_upd = 0; z_upd = 0; break;
        }
    } else if (ate && atst == 1) {
        a_test = 0;
    } else if (ate && afail != 0) {
        log_once("alpha test fail mode", afail | atst << 4);
        a_test = 0;                     /* draw everything */
    }

    /* blending: (A - B) * C + D */
    if (abe) {
        u8 x = C == 1 ? GX_BL_DSTALPHA : GX_BL_SRCALPHA, ix = C == 1 ? GX_BL_INVDSTALPHA : GX_BL_INVSRCALPHA;
        if (C == 2) {
            if (fix >= 0x80) {          /* factor 1 */
                x = GX_BL_ONE;
                ix = GX_BL_ZERO;
            } else {
                fix_alpha = 1;
            }
        }
        if (A == B) {
            if (D == 1) {
                color_upd = 0;          /* RGB stays, alpha is written */
            } else if (D == 2) {
                bm = GX_BM_BLEND; bs = GX_BL_ZERO; bd = GX_BL_ZERO;
            }
            fix_alpha = 0;
        } else if (A == 0 && B == 1 && D == 1) {
            bm = GX_BM_BLEND; bs = x; bd = ix;
        } else if (A == 0 && B == 2 && D == 1) {
            bm = GX_BM_BLEND; bs = x; bd = GX_BL_ONE;
        } else if (A == 0 && B == 2 && D == 2) {
            bm = GX_BM_BLEND; bs = x; bd = GX_BL_ZERO;
        } else if (A == 1 && B == 0 && D == 0) {
            bm = GX_BM_BLEND; bs = ix; bd = x;
            if (x == GX_BL_ONE) { bs = GX_BL_ZERO; bd = GX_BL_ONE; }
        } else if (A == 1 && B == 2 && D == 0) {
            bm = GX_BM_BLEND; bs = GX_BL_ONE; bd = x;
        } else if (A == 2 && B == 0 && D == 1) {
            bm = GX_BM_SUBTRACT;
            sub_premul = x != GX_BL_ONE;
        } else {
            log_once("blend mode", d->alpha & 0xFF);
            if (A == 0 && B == 1 && D == 2) { bm = GX_BM_BLEND; bs = x; bd = GX_BL_ZERO; }
        }
        if (fix_alpha && a_test) {
            log_once("FIX blend with alpha test", d->test);
            fix_alpha = 0;
        }
    }

    AST(0);
    g_as_n++;
    /* texture */
    if (tme) {
        uint32_t psm = GS_FIELD(d->tex0, 20, 6), tbp = GS_FIELD(d->tex0, 0, 14);
        uint32_t tw = 1u << GS_FIELD(d->tex0, 26, 4), th = 1u << GS_FIELD(d->tex0, 30, 4);
        uint32_t wms = GS_FIELD(d->clamp, 0, 2), wmt = GS_FIELD(d->clamp, 2, 2);
        int lin = (int)GS_FIELD(d->tex1, 5, 1);
        RT *src = rt_for_texture(tbp);
        GXTexObj to;
        Mtx tm;
        if (tw > 1024) tw = 1024;
        if (th > 1024) th = 1024;
        guMtxIdentity(tm);
        AST(6);
        if (src) {
            if (psm == PSMZ32 || psm == PSMZ24 || psm == PSMZ16 || psm == PSMZ16S) {
                log_once("Z buffer read as texture, psm", psm);
                return 0;
            }
            /* (a T8H view of a target in the EFB takes its alpha or depth from
             * there: no colour copy for it -- the haze made a full-screen one a frame) */
            if (src->resident && (src->dirty || !src->tex_valid) && psm != PSMT8H)
                rt_copy(src, 0);
            if (!src->tex_valid && !(psm == PSMT8H && src->resident))
                return 0;
            if (psm == PSMT8H) {
                /* alpha as palette index: copy the alpha channel, read it as
                 * CI8; or Z bits 8-15 where the game put those in the alpha */
                void *tl;
                GXTlutObj lo;
                int z8 = src->zalpha == g_frame_no;
                /* in the EFB: its alpha copied; else from the copy made when it left */
                const int aw = src->resident ? src->tw : src->ctw, ah = src->resident ? src->th : src->cth;
                const int ash = src->resident ? src->sh : src->csh;
                uint32_t a8 = GX_GetTexBufferSize((u16)aw, (u16)ah, GX_TF_I8, GX_FALSE, 0);
                if (src->a8_cap < a8) {
                    src->tex_a8 = gx_alloc(a8);
                    src->a8_cap = a8;
                    src->a8_ver = 0;
                }
                if (!src->resident) {
                    /* The sky's clouds: the game draws them once into the alpha
                     * bytes of the memory its depth buffer has (24 bits of
                     * depth, the fourth byte free) and reads them as a T8H
                     * texture while the scene is drawn. The scene has the EFB
                     * then: the cloud target is a texture, and its alpha was
                     * never taken from it -- no clouds. Once per content. */
                    if (src->a8_ver != 0xFFFFFFFFu) {
                        rt_alpha_from_tex(src, aw, ah);
                        src->a8_ver = 0xFFFFFFFFu;
                    }
                } else if (src->a8_ver != g_draw_no) {
                    batch_flush();
                    GX_SetTexCopySrc((u16)src->ex, (u16)src->ey, (u16)src->tw, (u16)src->th);
                    GX_SetTexCopyDst((u16)src->tw, (u16)src->th, z8 ? GX_CTF_Z8M : GX_CTF_A8, GX_FALSE);
                    GX_CopyTex(src->tex_a8, GX_FALSE);
                    GX_PixModeSync();
                    GX_InvalidateTexAll();
                    src->a8_ver = g_draw_no;
                }
                tl = tlut_lookup(d->tex0, d->texa, 256, z8 ? 0 : 1);
                if (src->fbp == 0x008C0)
                    g_mark_frame[7] = g_frame_no;
                if (!tl)
                    return 0;
                tlut_load(tl, 256);
                GX_InitTexObjCI(&to, src->tex_a8, (u16)aw, (u16)ah, GX_TF_CI8, GX_CLAMP, GX_CLAMP, GX_FALSE, GX_TLUT0);
                tm[0][0] = (float)tw / (float)(aw << ash);
                tm[1][1] = (float)th / (float)(ah << ash);
            } else {
                if (psm != PSMCT32 && psm != PSMCT24)
                    log_once("render target read as psm", psm);
                GX_InitTexObj(&to, src->tex, (u16)src->ctw, (u16)src->cth, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
            }
            /* GS coordinates are relative to TWxTH; the copy covers (ctw x cth) << csh */
            if (psm != PSMT8H) {
                tm[0][0] = (float)tw / (float)(src->ctw << src->csh);
                tm[1][1] = (float)th / (float)(src->cth << src->csh);
            }
        } else {
            Tex *t;
            TexMemo *mo = NULL;
            void *tl = NULL;
            int n = 0, k;
            uint32_t ver0, cver0;
#ifndef NO_TMEMO2
            Tm2 *m2 = NULL;
            uint32_t tm_h = 0, tm_pid = 0, tm_up0 = 0;
            int tm_pl = 0;
#endif
#ifdef MEMOSTAT
            uint32_t gen0 = g_tmemo_gen;
#endif
            int prev = pt_enter(PT_TEX);
            AST(7);
            gs_backend_sync(tbp, GS_FIELD(d->tex0, 14, 6), psm, tw, th);
            AST(8);
            ver0 = gs_ver;
            cver0 = gs_clut_ver;
#ifdef MEMOSTAT
            ms_pre(d);
#endif
            if (!(g_gx_flags & 4096) && g_opt_new)
                for (k = 0; k < 4; k++) {
                    TexMemo *e = &g_tmemo[k];
                    if (e->gen == g_tmemo_gen && e->gs_ver == ver0 && e->clut_ver == cver0 && e->tex0 == d->tex0 &&
                        e->texa == d->texa && e->tex1 == d->tex1 && e->clamp == d->clamp) {
                        mo = e;
                        break;
                    }
                }
            if (mo) {
                /* what tex_lookup and tlut_lookup would come to (and the same GX loads) */
                pt_leave(prev);
                g_tmemo_hits++;
                mo->t->used = g_frame_no;
                if (mo->tl) {
                    mo->tls->used = g_frame_no;
                    tlut_load(mo->tl, mo->n);
                }
                to = mo->to;
                AST(1);
#ifdef MEMOSTAT
                ms_note(d, 1, ver0, cver0, gen0, mo->t, mo->tl, 1);
#endif
                goto tex_ready;
            }
#ifndef NO_TMEMO2
            if (g_tmemo2 && !(g_gx_flags & 4096) && g_opt_new) {
                tm_h = tm2_hash(d);
                tm_pl = gs_placed_for(tbp, &tm_pid);
                if (!tm_pl)
                    tm_pid = 0;
                m2 = tm2_find(d, tm_h, tbp, GS_FIELD(d->tex0, 14, 6), psm, tw, th, ver0, cver0, tm_pl, tm_pid);
                if (m2) {
                    g_tm2_c[TM2_HIT]++;
                    g_tm2_tot[0]++;
                    if (g_tmemo2 == 1) {
                        /* what tex_lookup and tlut_lookup would come to; and the four entries as the lookups would have left them */
                        TexMemo *e = &g_tmemo[g_tmemo_next++ & 3];
                        const int cn = m2->meta & 4 ? 16 : 256;
                        pt_leave(prev);
                        m2->t->used = g_frame_no;
                        if (m2->tl) {
                            m2->tls->used = g_frame_no;
                            tlut_load(m2->tl, (uint32_t)cn);
                        }
                        to = m2->to;
                        e->tex0 = d->tex0;
                        e->texa = d->texa;
                        e->tex1 = d->tex1;
                        e->clamp = d->clamp;
                        e->gs_ver = ver0;
                        e->clut_ver = cver0;
                        e->gen = g_tmemo_gen;
                        e->t = m2->t;
                        e->tl = m2->tl;
                        e->tls = m2->tls;
                        e->n = (uint16_t)(m2->tl ? cn : 0);
                        e->to = to;
                        AST(1);
                        goto tex_ready;
                    }
                }
                tm_up0 = g_lk_upload;
            }
#endif
            g_tmemo_misses++;
            t = tex_lookup(d->tex0, d->texa);
            pt_leave(prev);
            AST(9);
            if (!t)
                return 0;
            if (t->ci) {
                n = t->fmt == GX_TF_CI4 ? 16 : 256;
                tl = tlut_lookup(d->tex0, d->texa, n, 0);
                AST(10);
                if (!tl)
                    return 0;
                if (g_gx_flags & 4096)
                    tlut_flip_note(d->tex0);
                tlut_load(tl, (uint32_t)n);
                GX_InitTexObjCI(&to, t->data, t->w, t->h, t->fmt, wms == 0 || wms == 3 ? GX_REPEAT : GX_CLAMP,
                                wmt == 0 || wmt == 3 ? GX_REPEAT : GX_CLAMP, GX_FALSE, GX_TLUT0);
            } else {
                GX_InitTexObj(&to, t->data, t->w, t->h, t->fmt, wms == 0 || wms == 3 ? GX_REPEAT : GX_CLAMP,
                              wmt == 0 || wmt == 3 ? GX_REPEAT : GX_CLAMP, GX_FALSE);
            }
            AST(1);
            GX_InitTexObjLOD(&to, lin ? GX_LINEAR : GX_NEAR, lin ? GX_LINEAR : GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE,
                             GX_ANISO_1);
#ifdef MEMOSTAT
            ms_note(d, 0, ver0, cver0, gen0, t, tl, gs_ver == ver0 && gs_clut_ver == cver0);
#endif
#ifndef NO_TMEMO2
            if (g_tmemo2 && !(g_gx_flags & 4096) && g_opt_new) {
                if (m2)
                    tm2_compare(m2, t, tl, n, &to, d);
                if (gs_ver == ver0 && gs_clut_ver == cver0 && g_lk_upload == tm_up0)
                    tm2_insert(d, tm_h, GS_FIELD(d->tex0, 14, 6), psm, tw, th, ver0, cver0, tm_pl, tm_pid, t, tl, n, &to);
            }
#endif
            if (gs_ver == ver0 && gs_clut_ver == cver0) {
                /* (the lookups wrote nothing: what they came to holds as long as the memory does) */
                TexMemo *e = &g_tmemo[g_tmemo_next++ & 3];
                e->tex0 = d->tex0;
                e->texa = d->texa;
                e->tex1 = d->tex1;
                e->clamp = d->clamp;
                e->gs_ver = ver0;
                e->clut_ver = cver0;
                e->gen = g_tmemo_gen;
                e->t = t;
                e->tl = tl;
                e->tls = tl ? &g_tluts[((uint8_t *)tl - (uint8_t *)g_texmem) / TLUT_SLOT] : NULL;
                e->n = (uint16_t)n;
                e->to = to;
            }
            goto tex_ready;
        }
        AST(1);
        GX_InitTexObjLOD(&to, lin ? GX_LINEAR : GX_NEAR, lin ? GX_LINEAR : GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE,
                         GX_ANISO_1);
    tex_ready:
        if (!g_gxc.tex_valid || memcmp(&to, &g_gxc.tex, sizeof to)) {
            g_gxc.tex = to;
            g_gxc.tex_valid = 1;
            GX_LoadTexObj(&to, GX_TEXMAP0);
        }
        /* (only the scale changes: the rest is the identity) */
        if (!g_gxc.tm_valid || g_gxc.tm[0] != tm[0][0] || g_gxc.tm[1] != tm[1][1] || (!g_opt_new && g_gxc.tex_valid == 1)) {
            g_gxc.tm[0] = tm[0][0];
            g_gxc.tm[1] = tm[1][1];
            g_gxc.tm_valid = 1;
            GX_LoadTexMtxImm(tm, GX_TEXMTX0, GX_MTX3x4);
        }
        g_cur.uscale = k_rcp16_pow2[__builtin_ctz(tw)];     /* tw, th: 1..1024, powers of two */
        g_cur.vscale = k_rcp16_pow2[__builtin_ctz(th)];
    }

    AST(2);
    /* TEV: colour */
    {
        uint32_t k0 = (uint32_t)d->fogcol & 0xFFFFFF, k1 = a2(fix), k2 = a2(GS_FIELD(d->texa, 0, 8));
        if (k0 != g_gxc.kc[0]) {
            g_gxc.kc[0] = k0;
            GX_SetTevKColor(GX_KCOLOR0, (GXColor){(u8)k0, (u8)(k0 >> 8), (u8)(k0 >> 16), 255});
        }
        if (k1 != g_gxc.kc[1]) {
            g_gxc.kc[1] = k1;
            GX_SetTevKColor(GX_KCOLOR1, (GXColor){255, 255, 255, (u8)k1});
        }
        if (k2 != g_gxc.kc[2]) {
            g_gxc.kc[2] = k2;
            GX_SetTevKColor(GX_KCOLOR2, (GXColor){255, 255, 255, (u8)k2});
        }
    }
    tev_key = 0x80000000u | tme | tfx << 1 | tcc << 3 | (uint32_t)(GS_FIELD(d->tex0, 20, 6) == PSMCT24) << 4 |
              fge << 5 | (uint32_t)fix_alpha << 6 | (uint32_t)sub_premul << 7 | (uint32_t)(C == 2) << 8;
    tev_same = tev_key == g_gxc.tev;
    g_gxc.tev = tev_key;
    TEVC(GX_SetTevOrder(GX_TEVSTAGE0, tme ? GX_TEXCOORD0 : GX_TEXCOORDNULL, tme ? GX_TEXMAP0 : GX_TEXMAP_NULL, GX_COLOR0A0));
    TEVC(GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_1));
    TEVC(GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_1));
    if (!tme) {
        TEVC(GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC));
        TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA));
        TEVC(GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
    } else {
        int ct24 = GS_FIELD(d->tex0, 20, 6) == PSMCT24;   /* alpha comes from TEXA */
        u8 ta = ct24 ? GX_CA_KONST : GX_CA_TEXA;
        if (ct24)
            TEVC(GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K2_A));
        if (tfx == 1) {                 /* DECAL */
            TEVC(GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC));
            TEVC(GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
            if (tcc)
                TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, ta));
            else
                TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA));
        } else {                        /* MODULATE, HIGHLIGHT, HIGHLIGHT2 */
            TEVC(GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO));
            TEVC(GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV));
            if (!tcc)
                TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA));
            else if (tfx == 0)
                TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, ta, GX_CA_RASA, GX_CA_ZERO));
            else if (tfx == 2)
                TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_RASA, GX_CA_KONST, ta));   /* At + Af */
            else
                TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, ta));
        }
    }
    TEVC(GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
    stage = 1;
    if (tme && tfx >= 2) {              /* HIGHLIGHT: colour + vertex alpha */
        TEVC(GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0));
        TEVC(GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_RASA, GX_CC_HALF, GX_CC_CPREV));
        TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV));
        TEVC(GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
        TEVC(GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
        stage++;
    }
    if (fge) {                          /* fog: lerp(FOGCOL, colour, F) with F in CLR1 alpha */
        TEVC(GX_SetTevOrder(GX_TEVSTAGE0 + stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR1A1));
        TEVC(GX_SetTevKColorSel(GX_TEVSTAGE0 + stage, GX_TEV_KCSEL_K0));
        TEVC(GX_SetTevColorIn(GX_TEVSTAGE0 + stage, GX_CC_KONST, GX_CC_CPREV, GX_CC_RASA, GX_CC_ZERO));
        TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0 + stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV));
        TEVC(GX_SetTevColorOp(GX_TEVSTAGE0 + stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
        TEVC(GX_SetTevAlphaOp(GX_TEVSTAGE0 + stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
        stage++;
    }
    g_cur.fix2 = 0;
    if (fix_alpha && alpha_upd && !sub_premul) {
        /* the colour pass leaves alpha alone; fix2_set(1) writes it */
        g_cur.fix2 = 1;
        g_cur.fix_stage = stage;
        g_cur.zte2 = (int)zte;
        g_cur.zfunc1 = k_ztst[ztst];
        g_cur.zfunc2 = ztst == 3 ? GX_GEQUAL : k_ztst[ztst];
        g_cur.zupd1 = z_upd;
        g_cur.color_upd = color_upd;
        g_cur.bm = bm;
        g_cur.bs = bs;
        g_cur.bd = bd;
        alpha_upd = 0;
    }
    if (fix_alpha || sub_premul) {
        /* FIX factor as the output alpha; for subtraction multiply it into the colour */
        TEVC(GX_SetTevOrder(GX_TEVSTAGE0 + stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL));
        TEVC(GX_SetTevKAlphaSel(GX_TEVSTAGE0 + stage, GX_TEV_KASEL_K1_A));
        TEVC(GX_SetTevKColorSel(GX_TEVSTAGE0 + stage, GX_TEV_KCSEL_1));
        if (sub_premul)
            TEVC(GX_SetTevColorIn(GX_TEVSTAGE0 + stage, GX_CC_ZERO, GX_CC_CPREV, C == 2 ? GX_CC_KONST : GX_CC_APREV,
                             GX_CC_ZERO));
        else
            TEVC(GX_SetTevColorIn(GX_TEVSTAGE0 + stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV));
        TEVC(GX_SetTevAlphaIn(GX_TEVSTAGE0 + stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                         fix_alpha ? GX_CA_KONST : GX_CA_APREV));
        TEVC(GX_SetTevColorOp(GX_TEVSTAGE0 + stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
        TEVC(GX_SetTevAlphaOp(GX_TEVSTAGE0 + stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV));
        stage++;
    }
    TEVC(GX_SetNumTevStages((u8)stage));
    AST(3);

    {
        uint32_t p0 = 0x80000000u | bm | bs << 8 | bd << 16, p1 = a_test ? 0x100u | k_atst[atst] | (aref >= 0x80 ? 255u : aref * 2) << 8 : 0;
        uint32_t p2 = zte ? 0x100u | k_ztst[ztst] | (uint32_t)(z_upd != 0) << 4 : 0, p3 = (uint32_t)(color_upd != 0) | (uint32_t)(alpha_upd != 0) << 1;
        if (p0 != g_gxc.pix[0]) {
            g_gxc.pix[0] = p0;
            GX_SetBlendMode(bm, bs, bd, GX_LO_CLEAR);
        }
        if (p1 != g_gxc.pix[1] || !(g_gxc.pix[3] & 0x80000000u)) {
            g_gxc.pix[1] = p1;
            if (a_test)
                GX_SetAlphaCompare(k_atst[atst], (u8)(aref >= 0x80 ? 255 : aref * 2), GX_AOP_AND, GX_ALWAYS, 0);
            else
                GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        }
        if (p2 != g_gxc.pix[2] || !(g_gxc.pix[3] & 0x80000000u)) {
            g_gxc.pix[2] = p2;
            if (!zte)
                GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
            else
                GX_SetZMode(GX_TRUE, k_ztst[ztst], z_upd ? GX_TRUE : GX_FALSE);
        }
        if ((p3 | 0x80000000u) != g_gxc.pix[3]) {
            g_gxc.pix[3] = p3 | 0x80000000u;
            GX_SetColorUpdate(color_upd ? GX_TRUE : GX_FALSE);
            GX_SetAlphaUpdate(alpha_upd ? GX_TRUE : GX_FALSE);
        }
    }

    AST(4);
    /* target placement: scale and offset, scissor */
    if (!g_gxc.place_valid || g_gxc.place[0] != s || g_gxc.place[1] != (float)rt->ex || g_gxc.place[2] != (float)rt->ey) {
        g_gxc.place[0] = s;
        g_gxc.place[1] = (float)rt->ex;
        g_gxc.place[2] = (float)rt->ey;
        g_gxc.place_valid = 1;
        guMtxIdentity(m);
        m[0][0] = s;
        m[1][1] = s;
        m[0][3] = (float)rt->ex;
        m[1][3] = (float)rt->ey;
        GX_LoadPosMtxImm(m, GX_PNMTX0);
    }
    {
        int x0 = (int)GS_FIELD(d->scissor, 0, 11), x1 = (int)GS_FIELD(d->scissor, 16, 11);
        int y0 = (int)GS_FIELD(d->scissor, 32, 11), y1 = (int)GS_FIELD(d->scissor, 48, 11);
        uint32_t sc[4];
        if (x1 >= rt->w) x1 = rt->w - 1;
        if (y1 >= rt->h) y1 = rt->h - 1;
        if (x1 < x0 || y1 < y0)
            return 0;
        sc[0] = (u32)(rt->ex + (x0 >> rt->sh));
        sc[1] = (u32)(rt->ey + (y0 >> rt->sh));
        sc[2] = (u32)(((x1 - x0 + 1) >> rt->sh) ? ((x1 - x0 + 1) >> rt->sh) : 1);
        sc[3] = (u32)(((y1 - y0 + 1) >> rt->sh) ? ((y1 - y0 + 1) >> rt->sh) : 1);
        if (!g_gxc.scis_valid || memcmp(sc, g_gxc.scis, sizeof sc)) {
            memcpy(g_gxc.scis, sc, sizeof sc);
            g_gxc.scis_valid = 1;
            GX_SetScissor(sc[0], sc[1], sc[2], sc[3]);
        }
    }

    AST(5);
    g_cur.zscale = zpsm == 0 ? 1.0f / 4294967296.0f : zpsm == 1 ? 1.0f / 16777216.0f : 1.0f / 65536.0f;
    g_cur.fst = (int)fst;
    g_cur.iip = (int)((d->prim >> 3) & 1);
    g_cur.fge = (int)fge;
    return 1;
}

/* A FIX blend that writes alpha: the colour pass leaves alpha alone, then
 * an alpha-only pass writes the source alpha where the colour pass passed
 * the depth test. Draw loops: for (pass = 0; pass < fix2_passes(); pass++)
 * { fix2_set(pass); emit }, then fix2_set(0) restores the colour pass. */
static int fix2_passes(void)
{
    return g_cur.fix2 ? 2 : 1;
}

static void fix2_set(int alpha_pass)
{
    if (!g_cur.fix2)
        return;
    g_gxc.pix[0] = 0;
    g_gxc.pix[3] = 0;
    g_gxc.tev = 0;
    GX_SetTevAlphaIn(GX_TEVSTAGE0 + g_cur.fix_stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                     alpha_pass ? GX_CA_APREV : GX_CA_KONST);
    if (alpha_pass)
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    else
        GX_SetBlendMode(g_cur.bm, g_cur.bs, g_cur.bd, GX_LO_CLEAR);
    GX_SetColorUpdate(alpha_pass ? GX_FALSE : (u8)g_cur.color_upd);
    GX_SetAlphaUpdate(alpha_pass ? GX_TRUE : GX_FALSE);
    if (alpha_pass)
        GX_SetZMode(g_cur.zte2 ? GX_TRUE : GX_FALSE, g_cur.zte2 ? g_cur.zfunc2 : GX_ALWAYS, GX_FALSE);
    else
        GX_SetZMode(g_cur.zte2 ? GX_TRUE : GX_FALSE, g_cur.zte2 ? g_cur.zfunc1 : GX_ALWAYS, (u8)g_cur.zupd1);
}

/* ---- primitives -------------------------------------------------------------------------------- */

/* experiment (gxflags 8388608): GS depth takes half of GX's range, so native
 * geometry up to twice as near as the GS's largest Z is not clipped away */
#define ZDS ((g_gx_flags & 8388608) ? 0.5f : 1.0f)

static inline void put_vertex(BVert *o, const GsVertex *v, const GsVertex *col)
{
    uint32_t z = g_cur.zscale == 1.0f / 16777216.0f ? (v->z & 0xFFFFFF) : g_cur.zscale == 1.0f / 65536.0f ? (v->z & 0xFFFF) : v->z;
    o->x = (float)v->x * (1.0f / 16.0f);
    o->y = (float)v->y * (1.0f / 16.0f);
    o->z = -(float)z * g_cur.zscale * ZDS;
    if (g_cur.fst) {
        o->s = (float)v->u * g_cur.uscale;
        o->t = (float)v->v * g_cur.vscale;
        o->q = 1.0f;
    } else {
        o->s = v->s;
        o->t = v->t;
        o->q = v->q;
    }
    o->rgba = (uint32_t)col->r << 24 | (uint32_t)col->g << 16 | (uint32_t)col->b << 8 | a2(col->a);
    o->fog = 0xFFFFFF00u | (g_cur.fge ? v->fog : 255);
}

static void gs_backend_draw2(const GsDraw *d, int type, const GsVertex *v, int n);
static int post_filter(const GsDraw *d, int type, const GsVertex *v, int n);

static void post_log(const GsDraw *d, int type, const GsVertex *v, int n, int skipped);

/* debugging (gxflags 262144): every draw into the reflection map target
 * 0x2D60 for a few frames, before any filtering */
static void __attribute__((noinline)) target_log(const GsDraw *d, int type, const GsVertex *v, int n)
{
    static uint32_t frame = 0xFFFFFFFFu;
    static int frames, lines;
    int k, x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    if (GS_FIELD(d->frame, 0, 9) * 32 != (uint32_t)g_tlog_fbp || lines >= 200)
        return;
    if (frame != g_frame_no) {
        if (++frames > 4)
            return;
        frame = g_frame_no;
        LOG("target %05X log: frame %u", (unsigned)g_tlog_fbp, g_frame_no);
    }
    lines++;
    for (k = 0; k < n && k < 512; k++) {
        if (v[k].x < x0) x0 = v[k].x;
        if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y;
        if (v[k].y > y1) y1 = v[k].y;
    }
    LOG("  t%d n%d prim %03X tex %X (tex0 %016llX) %ux%u alpha %010llX test %05llX frame %llX zbuf %llX xy %.1f,%.1f-%.1f,%.1f "
        "rgba %02X%02X%02X%02X scissor %llX", type, n, d->prim, (unsigned)GS_FIELD(d->tex0, 0, 14), (unsigned long long)d->tex0,
        1u << GS_FIELD(d->tex0, 26, 4), 1u << GS_FIELD(d->tex0, 30, 4), (unsigned long long)d->alpha,
        (unsigned long long)d->test, (unsigned long long)d->frame, (unsigned long long)d->zbuf, x0 / 16.0, y0 / 16.0,
        x1 / 16.0, y1 / 16.0, v[0].r, v[0].g, v[0].b, v[0].a, (unsigned long long)d->scissor);
}
static void post_bloom(RT *scene);

/* Small targets are also drawn into GS memory by the rasteriser: the game
 * renders palettes and lookup textures there that it reads back as CLUTs
 * or from inside the target, which the EFB copies cannot serve (and that
 * Dolphin keeps on the GPU by default). Pending uploads it reads first. */
static void soft_draw(RT *rt, const GsDraw *d, int type, const GsVertex *v, int n, int x0, int y0, int x1, int y1)
{
    uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, fbw = GS_FIELD(d->frame, 16, 6), fpsm = GS_FIELD(d->frame, 24, 6);
    int prev;
    if (g_gx_flags & 2)
        return;
    /* a target redrawn every frame that nothing reads from memory is the
     * GPU's alone (the sky gradient: rasterising it costs ~10 ms a frame);
     * so is one drawn in many frames that nothing read lately (a 64x32
     * target in races, ~15% of the time). Should it be read after all, the
     * read gets the EFB copy (gs_backend_sync: rt_readback). */
    if (rt->soft_frame != g_frame_no) {
        rt->soft_streak = rt->soft_frame + 1 >= g_frame_no ? rt->soft_streak + 1 : 0;
        rt->soft_frame = g_frame_no;
        rt->soft_frames++;
    }
    if (rt->soft_streak > 30 && g_frame_no - rt->mem_read > 30)
        return;
    if (rt->soft_frames > 120 && g_frame_no - rt->mem_read > 120)
        return;
    prev = pt_enter(PT_PRIM);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= rt->w) x1 = rt->w - 1;
    if (y1 >= rt->h) y1 = rt->h - 1;
    if (x1 >= x0 && y1 >= y0) {
        if (d->prim & 0x10)
            gs_sync_rect(GS_FIELD(d->tex0, 0, 14), GS_FIELD(d->tex0, 14, 6), GS_FIELD(d->tex0, 20, 6), 0, 0,
                         1u << GS_FIELD(d->tex0, 26, 4), 1u << GS_FIELD(d->tex0, 30, 4));
        gs_sync_rect(fbp, fbw, fpsm, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0 + 1), (uint32_t)(y1 - y0 + 1));
        gs_raster_draw(d, type, v, n);
        rt->soft_n++;
        rt->soft_px += (uint32_t)((x1 - x0 + 1) * (y1 - y0 + 1));
        gs_mark_rect(fbp, fbw, fpsm, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0 + 1), (uint32_t)(y1 - y0 + 1));
        rt->mem_draws = rt->draws;
    }
    pt_leave(prev);
}

/* grow a target's drawn area (GS pixels, inclusive) */
static void rt_drawn(RT *r, int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= r->w) x1 = r->w - 1;
    if (y1 >= r->h) y1 = r->h - 1;
    if (x1 < x0 || y1 < y0)
        return;
    if (x0 < r->dx0) r->dx0 = x0;
    if (y0 < r->dy0) r->dy0 = y0;
    if (x1 > r->dx1) r->dx1 = x1;
    if (y1 > r->dy1) r->dy1 = y1;
}

/* The game's last draw of a frame copies the scene target into the display
 * buffer: sprites, 1:1, sampled half a row up (an anti-flicker blend of
 * each row with the one above). Done as draws that moved 1.2 MB through
 * main memory twice a frame (the scene evicted from the EFB, the display
 * buffer later evicted for the next scene). Instead the display buffer
 * takes over the EFB region the scene is in; the blend becomes the display
 * copy's vertical filter; the next frame's scene takes the region back
 * (rt_evict). gxflags 1048576: draws as before. */
static int g_blit_vblend = -1;          /* last aliased blit: rows blended (1), not (0); -1 none */
static uint32_t g_blit_aliased;         /* status line */

static int blit_alias(const GsDraw *d, int type, const GsVertex *v, int n)
{
    uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, tbp = GS_FIELD(d->tex0, 0, 14), tfx = GS_FIELD(d->tex0, 35, 2);
    uint32_t tcc = GS_FIELD(d->tex0, 34, 1), abe = (d->prim >> 6) & 1;
    uint32_t A = GS_FIELD(d->alpha, 0, 2), B = GS_FIELD(d->alpha, 2, 2), C = GS_FIELD(d->alpha, 4, 2);
    uint32_t D = GS_FIELD(d->alpha, 6, 2);
    float du = 0, dv = 0;
    int k, x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30), ok = 1, vblend;
    RT *src, *dst;
    static int logged;
    if (type != 6 || !(d->prim & 0x10) || !(d->prim & 0x100) || (g_gx_flags & 1048576) || n < 2)
        return 0;
    src = rt_for_texture(tbp);
    dst = rt_find(fbp);
    if (src && src != dst && src->big && logged < 3) {
        static int why;
        if (why++ < 12)
            LOG("gx: blit candidate %05X -> %05X (%s): source resident %d, owner %05X, %dx%d -> %dx%d big %d fbw %u, "
                "%d vertices", src->fbp, fbp, dst ? "known" : "new", src->resident, g_big ? g_big->fbp : 0xFFFFFu,
                src->w, src->h, dst ? dst->w : 0, dst ? dst->h : 0, dst ? dst->big : 0, dst ? dst->fbw : 0, n);
    }
    if (!src || !dst || src == dst || !src->big || !dst->big || !src->resident || src != g_big || dst->w != src->w ||
        dst->h != src->h || dst->fbw * 64 < (uint32_t)dst->w)
        return 0;
    if (logged < 3) {
        logged++;
        LOG("gx: blit %05X -> %05X: tex0 %016llX tex1 %llX alpha %010llX test %05llX frame %llX, %d vertices, rgba "
            "%02X%02X%02X%02X", src->fbp, fbp, (unsigned long long)d->tex0, (unsigned long long)d->tex1,
            (unsigned long long)d->alpha, (unsigned long long)d->test, (unsigned long long)d->frame, n, v[0].r, v[0].g,
            v[0].b, v[0].a);
    }
    /* a copy: the colour written whole (the alpha test passing all, or
     * failing all with the colour written anyway: the race's blit is TEST
     * 31001, never and FB_ONLY, and was refused here, which cost two
     * full-screen EFB copies and two restores a frame); no blend, or Cs As +
     * Cd (1 - As) with As the vertex's (texture alpha off) at 1 */
    {
        const uint32_t ate = GS_FIELD(d->test, 0, 1), atst = GS_FIELD(d->test, 1, 3), afail = GS_FIELD(d->test, 12, 2);
        const uint32_t fbmsk = GS_FIELD(d->frame, 32, 32);
        const uint32_t fpsm = GS_FIELD(d->frame, 24, 6);
        /* The frame's write mask: apply_state writes the whole colour and the
         * whole alpha unless the mask covers them, and only a mask that
         * covers the colour (or the alpha) makes the draw different from a
         * copy. The alias takes the source's content, which is what such a
         * draw writes: a mask that only covers part of the colour (the
         * game's final blit has 0x0A0A0000) changes nothing here. */
        if ((fbmsk & 0xFFFFFF) == 0xFFFFFF || (fbmsk >> 24) == 0xFF)
            ok = 0;
        if (fpsm == PSMCT16 || fpsm == PSMCT16S)
            ok = 0;                      /* the 16-bit path has its own mask rule */
        if (ate && atst != 1 && !(atst == 0 && (afail == 1 || afail == 3)))
            ok = 0;
    }
    if (abe && !(A == 0 && B == 1 && C == 0 && D == 1 && !tcc))
        ok = 0;
    for (k = 0; k < n && ok; k++) {
        float u = (float)v[k].u / 16.0f, w = (float)v[k].v / 16.0f, x = (float)v[k].x / 16.0f, y = (float)v[k].y / 16.0f;
        if (abe && v[k].a < 0x80)
            ok = 0;
        if (tfx != 1 && !(tfx == 0 && v[k].r == 0x80 && v[k].g == 0x80 && v[k].b == 0x80))
            ok = 0;
        if (!k) {
            du = u - x;
            dv = w - y;
        } else if (u - x != du || w - y != dv) {
            ok = 0;
        }
        if (v[k].x < x0) x0 = v[k].x;
        if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y;
        if (v[k].y > y1) y1 = v[k].y;
    }
    /* the whole picture, 1:1 (texel offsets of 0 or a half) */
    if (!ok || x0 > 0 || y0 > 0 || x1 < dst->w * 16 || y1 < dst->h * 16 || du < 0 || du > 0.5f || dv < 0 || dv > 0.5f)
        return 0;
    vblend = GS_FIELD(d->tex1, 5, 1) && dv < 0.25f;     /* bilinear, half a row up */
    batch_flush();
    if (dst->resident)
        rt_evict(dst);
    /* the region and its content pass to the display buffer */
    src->resident = 0;
    src->tex_valid = 0;
    src->dirty = 0;
    dst->sh = 0;
    dst->ex = src->ex;
    dst->ey = src->ey;
    dst->tw = src->tw;
    dst->th = src->th;
    dst->resident = 1;
    dst->dirty = 1;
    dst->tex_valid = 0;
    dst->dx0 = 0;
    dst->dy0 = 0;
    dst->dx1 = dst->w - 1;
    dst->dy1 = dst->h - 1;
    dst->draws++;
    dst->ver = gs_ver;
    dst->used = g_frame_no;
    dst->alias = 1;
    g_big = dst;
    g_state_valid = 0;
    g_blit_vblend = vblend;
    g_blit_src = src;
    g_blit_aliased++;
    return 1;
}

void gs_backend_draw(const GsDraw *d, int type, const GsVertex *v, int n)
{
    gs_backend_draw2(d, type, v, n);
}

/* GX state for a draw: the target it goes to, or NULL when it is skipped */
static uint32_t g_ds_calls, g_ds_slow, g_ds_apply;     /* status line */
/* "wstat 7": draw setups by who asked (0 the GS path, 1 world batches, 2 car batches): calls, checked, applied */
static int g_ds_who;
static uint32_t g_ds_by[3][3];

/* status line: where a frame's chain spends its time, by target: each draw's
 * time runs until the next draw of the chain (the game's own work between
 * two chains is nobody's) */
static void target_time(RT *rt)
{
    extern uint32_t g_vif1_chains;
    static RT *cur;
    static uint64_t t0;
    static uint32_t chain;
    uint64_t now = 0;
    int k;
    if (!g_pt_on)
        return;
    /* the backend's time only: not the game's code, not waiting */
    for (k = 0; k < PT_COUNT; k++)
        if (k != PT_EE && k != PT_IDLE)
            now += g_pt[k];
    if (g_pt_cur != PT_EE && g_pt_cur != PT_IDLE)
        now += plat_ticks() - g_pt_last;
    if (chain != g_vif1_chains) {
        chain = g_vif1_chains;
        cur = NULL;
    }
    if (cur && now >= t0)               /* the sums start over with each status line */
        cur->st_ticks += now - t0;
    cur = rt;
    t0 = now;
    rt->st_draws++;
}

/* The game clears memory through other views of it: one sprite into 0x3160
 * as a 64-pixel wide buffer (depth buffer at the same address, test always)
 * clears the colour of all three reflection views below each other, one into
 * 0x2D60 their depth buffers. Drawn as pictures into a target of that shape
 * they cleared nothing: the views kept the last frame's depth where the sky
 * does not reach (the ground), and nearer geometry there shut out the new
 * frame's. The targets whose colour or depth memory the sprite covers are
 * cleared instead, when next drawn into. */
/* noinline: inlined, it crashed gcc 15 (cfgcleanup) */
/* A sprite into the alpha bytes of memory wider than the screen: the sky's
 * clouds. At a track's load the game copies a 1024x256 8-bit picture into
 * the fourth bytes of the memory its 24-bit depth buffer has (FRAME 01AC0,
 * 1024 wide, colour masked) and reads them as a T8H texture for the dome
 * from then on. The EFB cannot hold that (640 wide, alpha above 0x80 is
 * 1.0 there, six bits of it): as a target it gave a sky without clouds. It
 * is GS memory's: rasterised there once, and a texture like any other. */
static int __attribute__((noinline)) mem_plane_draw(const GsDraw *d, const GsVertex *v)
{
    const uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, fbw = GS_FIELD(d->frame, 16, 6), fpsm = GS_FIELD(d->frame, 24, 6);
    const uint32_t fbmsk = GS_FIELD(d->frame, 32, 32);
    int x0 = (v[0].x < v[1].x ? v[0].x : v[1].x) >> 4, y0 = (v[0].y < v[1].y ? v[0].y : v[1].y) >> 4;
    int x1 = ((v[0].x > v[1].x ? v[0].x : v[1].x) - 1) >> 4, y1 = ((v[0].y > v[1].y ? v[0].y : v[1].y) - 1) >> 4;
    int prev;
    if (fbw <= 10 || fbmsk != 0x00FFFFFFu || fpsm != PSMCT32 || (g_gx_flags & 2))
        return 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (int)fbw * 64) x1 = (int)fbw * 64 - 1;
    if (x1 < x0 || y1 < y0 || y1 >= 1024)
        return 1;
    prev = pt_enter(PT_PRIM);
    batch_flush();
    if (d->prim & 0x10)
        gs_sync_rect(GS_FIELD(d->tex0, 0, 14), GS_FIELD(d->tex0, 14, 6), GS_FIELD(d->tex0, 20, 6), 0, 0,
                     1u << GS_FIELD(d->tex0, 26, 4), 1u << GS_FIELD(d->tex0, 30, 4));
    gs_sync_rect(fbp, fbw, fpsm, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0 + 1), (uint32_t)(y1 - y0 + 1));
    gs_raster_draw(d, 6, v, 2);
    gs_mark_rect(fbp, fbw, fpsm, (uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0 + 1), (uint32_t)(y1 - y0 + 1));
    {
        /* the alpha there is this picture's now, not what targets in that memory drew earlier */
        const uint32_t end = fbp + span_blocks(PSMCT32, fbw, (uint32_t)y1 + 1);
        int k;
        for (k = 0; k < g_nrt; k++)
            if (g_rts[k].fbp < end && g_rts[k].fbp + span_blocks(PSMCT32, g_rts[k].fbw, (uint32_t)g_rts[k].h) > fbp)
                g_rts[k].awrite = 0;
    }
    pt_leave(prev);
    LOG("gx: sprite into the alpha of %05X (%u wide), %d,%d-%d,%d, drawn in GS memory (frame %u)", fbp, fbw * 64, x0, y0, x1, y1,
        g_frame_no);
    return 1;
}

static int __attribute__((noinline)) mem_clear(const GsDraw *d, int type, const GsVertex *v, int n)
{
    uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, fbw = GS_FIELD(d->frame, 16, 6), hi, col;
    int x0, y0, x1, y1, k, hit = 0, half;
    uint32_t fix = 0x80;                /* the colours' factor (alpha is written as it is) */
    RT *r;
    if (type != 6 || n != 2 || (d->prim & 0x10) || GS_FIELD(d->frame, 32, 32) || (g_gx_flags & 33554432))
        return 0;                       /* an untextured sprite writing all bits */
    r = rt_find(fbp);
    if (!r || r->big || r->fbw == fbw || !fbw)
        return 0;
    if (d->prim & 0x40) {
        /* blended: only what does not take the old content */
        uint32_t A = GS_FIELD(d->alpha, 0, 2), B = GS_FIELD(d->alpha, 2, 2), C = GS_FIELD(d->alpha, 4, 2);
        uint32_t D = GS_FIELD(d->alpha, 6, 2);
        if (A == B && D == 2)
            fix = 0;
        else if (A == 0 && B == 2 && C == 2 && D == 2)
            fix = GS_FIELD(d->alpha, 32, 8);        /* Cs x FIX */
        else if (!(A == B && D == 0)) {
            log_once("memory view sprite blends", d->alpha);
            return 0;
        }
    }
    x0 = (v[0].x < v[1].x ? v[0].x : v[1].x) >> 4;
    y0 = (v[0].y < v[1].y ? v[0].y : v[1].y) >> 4;
    x1 = (v[0].x > v[1].x ? v[0].x : v[1].x) >> 4;
    y1 = (v[0].y > v[1].y ? v[0].y : v[1].y) >> 4;
    /* the buffer's whole width from the top -- or half of it with the depth
     * buffer at the same address, 32 bits both and depth written: the two
     * formats order a page's blocks differently, and the left half of each
     * fills the whole page between them (the PS2's quick clear) */
    half = GS_FIELD(d->zbuf, 0, 9) * 32 == fbp && !GS_FIELD(d->zbuf, 32, 1) && GS_FIELD(d->test, 16, 1) &&
           GS_FIELD(d->test, 17, 2) == 1 && GS_FIELD(d->frame, 24, 6) == 0 && GS_FIELD(d->zbuf, 24, 4) == 0;
    if (x0 > 0 || y0 > 0 || x1 < (int)fbw * (half ? 32 : 64) || y1 < 32) {
        static int logged;
        if (logged++ < 4 || (x0 < 0 && logged < 12))
            LOG("gx: memory view sprite at %05X (%u wide) covers %d,%d..%d,%d: drawn; offset %u,%u scissor %u..%u x %u..%u "
                "prim %03X", fbp, fbw * 64, x0, y0, x1, y1, (unsigned)GS_FIELD(d->xyoffset, 0, 16) >> 4,
                (unsigned)GS_FIELD(d->xyoffset, 32, 16) >> 4, (unsigned)GS_FIELD(d->scissor, 0, 11),
                (unsigned)GS_FIELD(d->scissor, 16, 11), (unsigned)GS_FIELD(d->scissor, 32, 11),
                (unsigned)GS_FIELD(d->scissor, 48, 11), d->prim);
        return 0;
    }
    hi = fbp + (uint32_t)(y1 / 32) * fbw * 32;      /* whole pages */
    col = (v[1].r * fix >> 7 > 255 ? 255 : v[1].r * fix >> 7) | (v[1].g * fix >> 7 > 255 ? 255 : v[1].g * fix >> 7) << 8 |
          (v[1].b * fix >> 7 > 255 ? 255 : v[1].b * fix >> 7) << 16 | (uint32_t)v[1].a << 24;
    for (k = 0; k < g_nrt; k++) {
        RT *t = &g_rts[k];
        if (t->big || t->hold)
            continue;
        if (t->fbp >= fbp && t->fbp < hi) {
            t->pend |= 1;
            t->pend_col = col;
            if (!t->resident)
                t->tex_valid = 0;       /* nothing to bring back */
            hit = 1;
        }
        if (t->zbp >= fbp && t->zbp < hi && t->zbp) {
            t->pend |= 2;
            t->pend_z = col;
            hit = 1;
        }
    }
    {
        static int logged;
        if (logged < 6) {
            logged++;
            LOG("gx: memory clear through %05X as %u wide, %d rows (blocks to %05X): colour %08X, z %08X, alpha %010llX, %s",
                fbp, fbw * 64, y1, hi, col, v[1].z, (unsigned long long)d->alpha, hit ? "targets cleared" : "no target there");
        }
    }
    return 1;
}

/* clear a resident target as mem_clear found the game did */
static void __attribute__((noinline)) rt_pending_clear(RT *r)
{
    uint32_t c = r->pend_col, z = r->pend_z, zpsm = GS_FIELD(g_cur.d.zbuf, 24, 4);
    uint32_t rgba = (c & 0xFF) << 24 | (c & 0xFF00) << 8 | (c & 0xFF0000) >> 8 | a2(c >> 24);
    float zs = zpsm == 0 ? 1.0f / 4294967296.0f : zpsm == 1 ? 1.0f / 16777216.0f : 1.0f / 65536.0f;
    float zv = -(float)(zpsm == 0 ? z : zpsm == 1 ? z & 0xFFFFFF : z & 0xFFFF) * zs * ZDS;
    float x0 = (float)r->ex, y0 = (float)r->ey, x1 = x0 + (float)r->tw, y1 = y0 + (float)r->th;
    int pend = r->pend;
    Mtx m;
    r->pend = 0;
    batch_flush();
    g_state_valid = 0;
    gxc_drop();
    guMtxIdentity(m);
    GX_LoadPosMtxImm(m, GX_PNMTX0);
    GX_SetScissor((u32)r->ex, (u32)r->ey, (u32)r->tw, (u32)r->th);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, (GXColor){0, 0, 0, 0});
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode((pend & 2) ? GX_TRUE : GX_FALSE, GX_ALWAYS, (pend & 2) ? GX_TRUE : GX_FALSE);
    GX_SetColorUpdate((pend & 1) ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate((pend & 1) ? GX_TRUE : GX_FALSE);
    ortho_mode();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x0, y0, zv); GX_Normal3f32(0, 0, 1); GX_Color1u32(rgba); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x1, y0, zv); GX_Normal3f32(1, 0, 1); GX_Color1u32(rgba); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x1, y1, zv); GX_Normal3f32(1, 1, 1); GX_Color1u32(rgba); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x0, y1, zv); GX_Normal3f32(0, 1, 1); GX_Color1u32(rgba); GX_Color1u32(0xFFFFFFFF);
    GX_End();
    r->dirty = 1;
}

/* debugging (gxflags 16777216): the targets and depth buffers of a few frames'
 * draws, a line whenever FRAME, ZBUF or the depth test change */
static void __attribute__((noinline)) zstate_log(const GsDraw *d)
{
    extern uint32_t g_vif1_chains;
    static uint64_t frame, zbuf, test;
    static uint32_t chain0, lines, n;
    if (!chain0)
        chain0 = g_vif1_chains + 1;
    if (g_vif1_chains < chain0 || g_vif1_chains > chain0 + 1 || lines > 400)
        return;
    n++;
    if (d->frame == frame && d->zbuf == zbuf && ((d->test ^ test) & 0x70000) == 0)
        return;
    lines++;
    LOG("zst %u: %u draws; then FRAME fbp %05X fbw %u psm %02X mask %08X, ZBUF zbp %05X psm %X zmsk %u, ZTE %u ZTST %u, "
        "scissor %u..%u x %u..%u, prim %03X at draw %u", g_vif1_chains, n, (unsigned)GS_FIELD(d->frame, 0, 9) * 32,
        (unsigned)GS_FIELD(d->frame, 16, 6), (unsigned)GS_FIELD(d->frame, 24, 6), (unsigned)GS_FIELD(d->frame, 32, 32),
        (unsigned)GS_FIELD(d->zbuf, 0, 9) * 32, (unsigned)GS_FIELD(d->zbuf, 24, 4), (unsigned)GS_FIELD(d->zbuf, 32, 1),
        (unsigned)GS_FIELD(d->test, 16, 1), (unsigned)GS_FIELD(d->test, 17, 2), (unsigned)GS_FIELD(d->scissor, 0, 11),
        (unsigned)GS_FIELD(d->scissor, 16, 11), (unsigned)GS_FIELD(d->scissor, 32, 11), (unsigned)GS_FIELD(d->scissor, 48, 11),
        d->prim, g_draw_no);
    n = 0;
    frame = d->frame;
    zbuf = d->zbuf;
    test = d->test;
}

/* Notes from the game side (gs.h): targets that keep their picture for a
 * frame (the reflection views drawn in turn, runtime/hle.c). A held target
 * has a picture (drawn for a whole frame before, and still there); draws
 * into it and the game's clears of it are dropped until the next HOLD_BEGIN,
 * and as a texture it stays what it is. */
static uint32_t g_held_draws, g_held_frames, g_view_swaps;

/* Split-screen: a reflection view target passes to player p (GS_NOTE_VIEW). Both players' views are drawn into the
 * same targets, one player after the other, each in turn one view in three a frame; a view held for a player must
 * show that player's last picture, not the other's (which is what it held: both cars' reflections flickered). The
 * picture now goes to the copy kept for its owner, player p's comes back. */
static void rt_view_owner(RT *r, uint8_t p)
{
    void *t;
    uint32_t c;
    int k;
    if (r->resident && (r->dirty || !r->tex_valid))
        rt_copy(r, 0);
    t = r->tex; r->tex = r->alt_tex; r->alt_tex = t;
    c = r->tex_cap; r->tex_cap = r->alt_cap; r->alt_cap = c;
    k = r->tex_valid; r->tex_valid = r->alt_valid; r->alt_valid = (uint8_t)k;
    k = r->pic; r->pic = r->alt_pic; r->alt_pic = (uint8_t)k;
    k = r->ctw; r->ctw = r->alt_ctw; r->alt_ctw = k;
    k = r->cth; r->cth = r->alt_cth; r->alt_cth = k;
    k = r->csh; r->csh = r->alt_csh; r->alt_csh = k;
    r->owner = p;
    r->a8_ver = 0;
    g_view_swaps++;
    if (r->resident) {
        if (r->tex_valid) {
            rt_restore(r);              /* the region shows player p's picture */
            r->dirty = 0;
        } else {
            r->pic = 0;                 /* none yet: drawn, not held */
            r->dirty = 1;
        }
    }
}

void gs_backend_note(uint32_t note)
{
    int k;
    if ((note & 0xF0000000u) == GS_NOTE_VIEW) {
        RT *r = rt_find(note & 0xFFFFFu);
        if (r && !r->big && r->owner != (uint8_t)(note >> 24 & 15)) {
            if (g_state_valid && g_cur.rt == r) {
                batch_flush();
                g_state_valid = 0;
            }
            rt_view_owner(r, (uint8_t)(note >> 24 & 15));
        }
    } else if ((note & 0xF0000000u) == GS_NOTE_HOLD_BEGIN) {
        for (k = 0; k < g_nrt; k++) {
            RT *r = &g_rts[k];
            if (r->draws != r->hold_draws && !r->hold)
                r->pic = 1;             /* drawn since the last one */
            r->hold_draws = r->draws;
            r->hold = 0;
        }
    } else if ((note & 0xF0000000u) == GS_NOTE_HOLD) {
        RT *r = rt_find(note & 0xFFFFFu);
        if (r && !r->big && r->pic && (r->resident || r->tex_valid)) {
            if (g_state_valid && g_cur.rt == r) {
                batch_flush();
                g_state_valid = 0;
            }
            r->hold = 1;
            g_held_frames++;
        } else {
            g_gs_hold_missed |= 1u << ((note >> 20 & 15) + 8 * (note >> 24 & 3));
        }
    }
}

/* draw_setup when something changed: the target, and the state applied if it differs. NULL: a held target. A
 * function of its own: with rt_get, apply_state and the lookups inlined into draw_setup it saved and restored
 * eighteen registers, and every draw paid for that, also the two in three that change nothing. */
static RT *__attribute__((noinline)) draw_setup_slow(const GsDraw *d)
{
    RT *rt;
    int prev = pt_enter(PT_GX);
    g_ds_slow++;
    g_ds_by[g_ds_who][1]++;
    rt = rt_get(d);
    if (rt->hold) {
        g_held_draws++;
        pt_leave(prev);
        return NULL;
    }
    rt_bind(rt);
    if (!rt->big)
        rt->zbp = GS_FIELD(d->zbuf, 0, 9) * 32;
    if (rt->pend) {
        g_cur.d.zbuf = d->zbuf;         /* the depth format the clear converts with */
        rt_pending_clear(rt);
    }
    if (!g_state_valid || g_cur.rt != rt || !same_state(d, &g_cur.d) || g_cur.gs_ver != gs_ver ||
        g_cur.clut_ver != gs_clut_ver) {
        batch_flush();
        g_cur.d = *d;
        g_cur.rt = rt;
        g_cur.gs_ver = gs_ver;
        g_cur.clut_ver = gs_clut_ver;
        g_ds_apply++;
        g_ds_by[g_ds_who][2]++;
        g_cur.skip = !apply_state(d, rt);
        g_state_valid = 1;
    }
    g_cur.seq = d->seq;
    pt_leave(prev);
    return rt;
}

static RT *draw_setup(const GsDraw *d)
{
    RT *rt;
    g_ds_calls++;
    g_ds_by[g_ds_who][0]++;
    if (g_gx_flags & 16777216)
        zstate_log(d);
    if (g_gxc.tev == COMBO_TEV && !g_combo_call) {
        g_gxc.tev = 0;
        g_state_valid = 0;
    }
    if (g_state_valid && d->seq == g_cur.seq && g_cur.gs_ver == gs_ver && g_cur.clut_ver == gs_clut_ver &&
        g_cur.rt->resident && !g_cur.rt->pend) {
        rt = g_cur.rt;                  /* nothing changed since the last draw */
    } else if (!(rt = draw_setup_slow(d))) {
        return NULL;
    }
    target_time(rt);
    if (g_cur.skip)
        return NULL;
    rt->dirty = 1;
    rt->draws++;
    rt->ver = gs_ver;
    return rt;
}

/* Untextured points also go to GS memory: the game draws palettes with them
 * (the sky's, every frame) and loads them as CLUTs, which come from GS memory */
static int point_to_mem(const GsDraw *d, const GsVertex *v)
{
    uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, fbw = GS_FIELD(d->frame, 16, 6), fpsm = GS_FIELD(d->frame, 24, 6);
    uint32_t fbmsk = GS_FIELD(d->frame, 32, 32), A = GS_FIELD(d->alpha, 0, 2), B = GS_FIELD(d->alpha, 2, 2);
    uint32_t D = GS_FIELD(d->alpha, 6, 2), c;
    int x = (v->x + 8) >> 4, y = (v->y + 8) >> 4;
    uint64_t sc = d->scissor;
    if ((d->prim & 0x40) && !(A == B && D == 0))
        return 0;                       /* blended: not a table */
    if (fpsm != PSMCT32 && fpsm != PSMCT24)
        return 0;
    if (GS_FIELD(d->test, 0, 1) && GS_FIELD(d->test, 1, 3) != 1) {
        /* an alpha test that always fails, writing the frame buffer anyway
         * (FB_ONLY; RGB_ONLY) is how the game draws tables; others are not tables */
        uint32_t afail = GS_FIELD(d->test, 12, 2);
        if (GS_FIELD(d->test, 1, 3) != 0 || (afail != 1 && afail != 3))
            return 0;
        if (afail == 3)
            fbmsk |= 0xFF000000u;
    }
    if (x < (int)(sc & 0x7FF) || x > (int)((sc >> 16) & 0x7FF) || y < (int)((sc >> 32) & 0x7FF) ||
        y > (int)((sc >> 48) & 0x7FF))
        return 1;
    c = (uint32_t)v->r | (uint32_t)v->g << 8 | (uint32_t)v->b << 16 | (uint32_t)v->a << 24;
    if (GS_FIELD(d->fba, 0, 1))
        c |= 0x80000000u;
    if (fpsm == PSMCT24)
        fbmsk |= 0xFF000000u;
    gs_sync_rect(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 1, 1);
    c = (gs_read_pixel(PSMCT32, fbp, fbw, (uint32_t)x, (uint32_t)y) & fbmsk) | (c & ~fbmsk);
    gs_write_pixel(PSMCT32, fbp, fbw, (uint32_t)x, (uint32_t)y, c);
    gs_mark_rect(fbp, fbw, fpsm, (uint32_t)x, (uint32_t)y, 1, 1);  /* memory has it, not the target */
    return 1;
}

static uint32_t g_d2_cnt[10];           /* "wstat 7": draws by what became of them */

/* Split-screen: the game draws player 2's depth haze through a frame buffer that starts inside the scene's (FBP
 * 01180, 640 wide: row 224 of the scene at 008C0), as its own picture of a view's size. A target of its own there
 * took the EFB from the scene (its depth went, the haze read depth that had been cleared) and its draws never
 * reached the picture. Such a draw is the scene's, its rows (and its texture's, when it reads the same place)
 * moved down by the offset: *d and *v then point at the moved copies. Only for an FBP no target has: a whole
 * number of page rows past a screen sized target of the same buffer width. */
#define VA_MAXV 1024
static GsDraw g_va_d;
static GsVertex g_va_v[VA_MAXV];
static uint32_t g_va_draws;             /* status line */

static int __attribute__((noinline)) view_alias(const GsDraw **pd, const GsVertex **pv, int n)
{
    const GsDraw *d = *pd;
    const uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, fbw = GS_FIELD(d->frame, 16, 6);
    const uint32_t tbp = GS_FIELD(d->tex0, 0, 14), fst = (d->prim >> 8) & 1, tme = (d->prim >> 4) & 1;
    uint32_t dy = 0, sy0, sy1;
    RT *p = NULL;
    int k;
    if (!fbw || n > VA_MAXV)
        return 0;
    for (k = 0; k < g_nrt; k++) {
        RT *r = &g_rts[k];
        uint32_t off;
        if (!r->big || r->fbw != fbw || fbp <= r->fbp || r->fbp >= 16384)
            continue;
        off = fbp - r->fbp;
        if (off % (fbw * 32) || off / (fbw * 32) * 32 >= (uint32_t)r->h)
            continue;
        p = r;
        dy = off / (fbw * 32) * 32;
        break;
    }
    if (!p)
        return 0;
    g_va_d = *d;
    g_va_d.frame = (d->frame & ~0x1FFull) | p->fbp / 32;
    sy0 = GS_FIELD(d->scissor, 32, 11) + dy;
    sy1 = GS_FIELD(d->scissor, 48, 11) + dy;
    g_va_d.scissor = (d->scissor & ~(0x7FFull << 32 | 0x7FFull << 48)) | (uint64_t)(sy0 > 2047 ? 2047 : sy0) << 32 |
                     (uint64_t)(sy1 > 2047 ? 2047 : sy1) << 48;
    memcpy(g_va_v, *pv, (size_t)n * sizeof g_va_v[0]);
    for (k = 0; k < n; k++)
        g_va_v[k].y += (int32_t)(dy * 16);
    if (tme && tbp == fbp) {
        const float th = (float)(1u << GS_FIELD(d->tex0, 30, 4));
        g_va_d.tex0 = (d->tex0 & ~0x3FFFull) | p->fbp;
        for (k = 0; k < n; k++) {
            if (fst)
                g_va_v[k].v += dy * 16;
            else
                g_va_v[k].t += (float)dy / th * g_va_v[k].q;
        }
    }
    g_va_draws++;
    *pd = &g_va_d;
    *pv = g_va_v;
    return 1;
}

static void gs_backend_draw2(const GsDraw *d, int type, const GsVertex *v, int n)
{
    RT *rt;
    BVert *o;
    (void)n;
    if (!g_gx_ready)
        gx_init();
    if (g_gx_bisect == 1)
        return;
    if (PS2_UNLIKELY(!rt_find(GS_FIELD(d->frame, 0, 9) * 32)))
        view_alias(&d, &v, type == 0 ? 1 : type <= 2 || type == 6 ? 2 : 3);
    g_draw_no++;
    if (g_wstat == 7)
        g_d2_cnt[type < 6 ? type : 6]++;
    if (g_gx_maxdraw && ++g_frame_draws > (uint32_t)g_gx_maxdraw && GS_FIELD(d->frame, 0, 9))
        return;
    if (g_gx_flags & 262144)
        target_log(d, type, v, type == 0 ? 1 : type <= 2 || type == 6 ? 2 : 3);
    if (type == 0 && !(d->prim & 0x10) && !(g_gx_flags & 8) && point_to_mem(d, v)) {
        g_d2_cnt[7]++;
        return;
    }
    if (post_filter(d, type, v, type == 0 ? 1 : type <= 2 || type == 6 ? 2 : 3)) {
        g_d2_cnt[8]++;
        if (g_gx_flags & 16384)
            post_log(d, type, v, type == 0 ? 1 : type <= 2 || type == 6 ? 2 : 3, 1);
        return;
    }
    if (g_gx_flags & 16384)
        post_log(d, type, v, type == 0 ? 1 : type <= 2 || type == 6 ? 2 : 3, 0);
    if (type == 6 && blit_alias(d, type, v, 2))
        return;
    if (type == 6 && mem_clear(d, type, v, 2))
        return;
    if (type == 6 && mem_plane_draw(d, v))
        return;
    rt = draw_setup(d);
    if (!rt) {
        g_d2_cnt[9]++;
        return;
    }
    if (type == 6 && !(d->prim & 0x10))
        rt->clear = (uint32_t)v[1].r | (uint32_t)v[1].g << 8 | (uint32_t)v[1].b << 16 | 1u << 24;
    {
        int k, cnt = type == 0 ? 1 : type <= 2 || type == 6 ? 2 : 3, e = type ? 1 : 0;
        int x0 = v[0].x >> 4, y0 = v[0].y >> 4, x1 = (v[0].x - e) >> 4, y1 = (v[0].y - e) >> 4;
        for (k = 1; k < cnt; k++) {
            if ((v[k].x >> 4) < x0) x0 = v[k].x >> 4;
            if ((v[k].y >> 4) < y0) y0 = v[k].y >> 4;
            if (((v[k].x - e) >> 4) > x1) x1 = (v[k].x - e) >> 4;
            if (((v[k].y - e) >> 4) > y1) y1 = (v[k].y - e) >> 4;
        }
        rt_drawn(rt, x0, y0, x1, y1);
        if (rt->soft)
            soft_draw(rt, d, type, v, n, x0, y0, x1, y1);
    }
    switch (type) {
    case 3: case 4: case 5: {
        const GsVertex *c2 = g_cur.iip ? NULL : &v[2];
        if (g_nb + 3 > BATCH_MAX)
            batch_flush();
        o = &g_batch[g_nb];
        put_vertex(&o[0], &v[0], c2 ? c2 : &v[0]);
        put_vertex(&o[1], &v[1], c2 ? c2 : &v[1]);
        put_vertex(&o[2], &v[2], c2 ? c2 : &v[2]);
        g_nb += 3;
        break;
    }
    case 6: {                           /* sprite: two corners -> two triangles */
        GsVertex c[4];
        if (g_nb + 6 > BATCH_MAX)
            batch_flush();
        c[0] = v[0];
        c[3] = v[1];
        c[1] = v[1]; c[1].y = v[0].y; c[1].t = v[0].t; c[1].v = v[0].v;
        c[2] = v[0]; c[2].y = v[1].y; c[2].t = v[1].t; c[2].v = v[1].v;
        c[0].z = c[1].z = c[2].z = v[1].z;
        c[0].q = c[1].q = c[2].q = v[1].q;
        o = &g_batch[g_nb];
        put_vertex(&o[0], &c[0], &v[1]);
        put_vertex(&o[1], &c[1], &v[1]);
        put_vertex(&o[2], &c[2], &v[1]);
        put_vertex(&o[3], &c[1], &v[1]);
        put_vertex(&o[4], &c[3], &v[1]);
        put_vertex(&o[5], &c[2], &v[1]);
        g_nb += 6;
        break;
    }
    case 1: case 2: case 0: {           /* lines and points, drawn on their own */
        BVert t[2];
        int k, cnt = type ? 2 : 1;
        batch_flush();
        for (k = 0; k < cnt; k++)
            put_vertex(&t[k], &v[k], g_cur.iip ? &v[k] : &v[cnt - 1]);
        ortho_mode();
        GX_Begin(type ? GX_LINES : GX_POINTS, GX_VTXFMT0, (u16)cnt);
        for (k = 0; k < cnt; k++) {
            GX_Position3f32(t[k].x, t[k].y, t[k].z);
            GX_Normal3f32(t[k].s, t[k].t, t[k].q);
            GX_Color1u32(t[k].rgba);
            GX_Color1u32(t[k].fog);
        }
        GX_End();
        break;
    }
    }
}

void gs_backend_flush(void)
{
}

/* a strip or fan of GS vertices straight to GX (center: a fan's first vertex) */
static void emit_run(u8 prim, const GsVertex *center, const GsVertex *v, int cnt)
{
    static BVert t[1100];
    int k, m = 0, pass;
    if (center)
        put_vertex(&t[m++], center, center);
    for (k = 0; k < cnt && m < 1100; k++)
        put_vertex(&t[m++], &v[k], &v[k]);
    ortho_mode();
    for (pass = 0; pass < fix2_passes(); pass++) {
        fix2_set(pass);
        GX_Begin(prim, GX_VTXFMT0, (u16)m);
        for (k = 0; k < m; k++) {
            GX_Position3f32(t[k].x, t[k].y, t[k].z);
            GX_Normal3f32(t[k].s, t[k].t, t[k].q);
            GX_Color1u32(t[k].rgba);
            GX_Color1u32(t[k].fog);
        }
        GX_End();
    }
    fix2_set(0);
}

/* the triangles or sprite of the primitive completed at vertex i, into the batch */
static void batch_prim(int type, const GsVertex *v, int i)
{
    BVert *o;
    if (type == 6) {
        GsVertex c[4];
        const GsVertex *a = &v[i - 1], *b = &v[i];
        if (g_nb + 6 > BATCH_MAX)
            batch_flush();
        c[0] = *a;
        c[3] = *b;
        c[1] = *b; c[1].y = a->y; c[1].t = a->t; c[1].v = a->v;
        c[2] = *a; c[2].y = b->y; c[2].t = b->t; c[2].v = b->v;
        c[0].z = c[1].z = c[2].z = b->z;
        c[0].q = c[1].q = c[2].q = b->q;
        o = &g_batch[g_nb];
        put_vertex(&o[0], &c[0], b);
        put_vertex(&o[1], &c[1], b);
        put_vertex(&o[2], &c[2], b);
        put_vertex(&o[3], &c[1], b);
        put_vertex(&o[4], &c[3], b);
        put_vertex(&o[5], &c[2], b);
        g_nb += 6;
    } else {
        const GsVertex *p0 = type == 5 ? &v[0] : &v[i - 2], *p1 = &v[i - 1], *p2 = &v[i];
        const GsVertex *c2 = g_cur.iip ? NULL : p2;
        if (g_nb + 3 > BATCH_MAX)
            batch_flush();
        o = &g_batch[g_nb];
        put_vertex(&o[0], p0, c2 ? c2 : p0);
        put_vertex(&o[1], p1, c2 ? c2 : p1);
        put_vertex(&o[2], p2, c2 ? c2 : p2);
        g_nb += 3;
    }
}

/* A run of vertices from a vertex packet (gs.c packet_bulk): the state is set
 * up once; Gouraud strips and fans go to GX as strips and fans, everything
 * else into the triangle batch. */
void gs_backend_prims(const GsDraw *d, int type, const GsVertex *v, int n, const uint8_t *kick)
{
    RT *rt;
    int i, nk = 0, x0, y0, x1, y1, e = 1;
    if (!g_gx_ready)
        gx_init();
    if (g_gx_bisect == 1)
        return;
    if (g_gx_maxdraw) {
        gs_prims_each(d, type, v, n, kick);
        return;
    }
    for (i = 0; i < n; i++)
        nk += kick[i];
    if (!nk)
        return;
    if (PS2_UNLIKELY(!rt_find(GS_FIELD(d->frame, 0, 9) * 32)))
        view_alias(&d, &v, n);
    if (g_gx_flags & 262144)
        target_log(d, type, v, n);
    g_draw_no += (uint32_t)nk;
    if (post_filter(d, type, v, n)) {
        if (g_gx_flags & 16384)
            post_log(d, type, v, n, 1);
        return;
    }
    if (g_gx_flags & 16384)
        post_log(d, type, v, n, 0);
    if (nk == n / 2 && blit_alias(d, type, v, n))
        return;
    if (nk == 1 && n == 2 && mem_clear(d, type, v, n))
        return;
    if (type == 6 && nk == 1 && n == 2 && mem_plane_draw(d, v))
        return;
    rt = draw_setup(d);
    if ((g_gx_flags & 4194304) && GS_FIELD(d->frame, 0, 9) * 32 == 0x3160) {
        /* debugging: every emulated draw into the first reflection view */
        extern uint32_t g_vif1_chains, g_vu1_code;
        static uint32_t lines;
        int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
        uint32_t z0 = 0xFFFFFFFFu, z1 = 0;
        for (i = 0; i < n; i++) {
            if (v[i].x < bx0) bx0 = v[i].x;
            if (v[i].x > bx1) bx1 = v[i].x;
            if (v[i].y < by0) by0 = v[i].y;
            if (v[i].y > by1) by1 = v[i].y;
            if (v[i].z < z0) z0 = v[i].z;
            if (v[i].z > z1) z1 = v[i].z;
        }
        if (0 && lines++ < 8000)
            LOG("p3160 %u %06X t%d n%d k%d x%d..%d y%d..%d z%06X..%06X tex %08X%08X prim %03X %s", g_vif1_chains, g_vu1_code,
                type, n, nk, bx0 >> 4, bx1 >> 4, by0 >> 4, by1 >> 4, z0, z1, (uint32_t)(d->tex0 >> 32), (uint32_t)d->tex0,
                d->prim, rt ? "drawn" : "skipped");
    }
    if (!rt)
        return;
    /* drawn area: every vertex's (some of them start no primitive: a little more) */
    x0 = x1 = v[0].x >> 4;
    y0 = y1 = v[0].y >> 4;
    for (i = 1; i < n; i++) {
        int x = v[i].x >> 4, y = v[i].y >> 4;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    {
        int xm = (v[0].x - e) >> 4, ym = (v[0].y - e) >> 4;
        for (i = 1; i < n; i++) {
            if (((v[i].x - e) >> 4) > xm) xm = (v[i].x - e) >> 4;
            if (((v[i].y - e) >> 4) > ym) ym = (v[i].y - e) >> 4;
        }
        rt_drawn(rt, x0, y0, xm, ym);
    }
    if (type == 6 && !(d->prim & 0x10))
        for (i = n - 1; i >= 0; i--)
            if (kick[i]) {
                rt->clear = (uint32_t)v[i].r | (uint32_t)v[i].g << 8 | (uint32_t)v[i].b << 16 | 1u << 24;
                break;
            }
    if (rt->soft) {
        /* small targets also go to GS memory, a primitive at a time */
        GsVertex t[3];
        for (i = 0; i < n; i++) {
            const GsVertex *pv;
            int cnt = type == 6 ? 2 : 3, k, a0, b0, a1, b1;
            if (!kick[i])
                continue;
            if (type == 5) {
                t[0] = v[0]; t[1] = v[i - 1]; t[2] = v[i];
                pv = t;
            } else {
                pv = &v[i - cnt + 1];
            }
            a0 = a1 = pv[0].x >> 4;
            b0 = b1 = pv[0].y >> 4;
            a1 = (pv[0].x - e) >> 4;
            b1 = (pv[0].y - e) >> 4;
            for (k = 1; k < cnt; k++) {
                if ((pv[k].x >> 4) < a0) a0 = pv[k].x >> 4;
                if ((pv[k].y >> 4) < b0) b0 = pv[k].y >> 4;
                if (((pv[k].x - e) >> 4) > a1) a1 = (pv[k].x - e) >> 4;
                if (((pv[k].y - e) >> 4) > b1) b1 = (pv[k].y - e) >> 4;
            }
            soft_draw(rt, d, type, pv, cnt, a0, b0, a1, b1);
        }
    }
    if ((type == 4 || type == 5) && g_cur.iip) {
        /* runs of drawn triangles */
        for (i = 2; i < n;) {
            int first;
            if (!kick[i]) {
                i++;
                continue;
            }
            first = i;
            while (i < n && kick[i])
                i++;
            if (i - first == 1) {
                batch_prim(type, v, first);     /* one triangle: batched */
                continue;
            }
            batch_flush();
            if (type == 4)
                emit_run(GX_TRIANGLESTRIP, NULL, &v[first - 2], i - first + 2);
            else
                emit_run(GX_TRIANGLEFAN, &v[0], &v[first - 1], i - first + 1);
        }
        return;
    }
    for (i = 0; i < n; i++)
        if (kick[i])
            batch_prim(type, v, i);
}

/* ---- native draws (runtime/hle_render.c) ---------------------------------------------------- */

/* GX state the native draws loaded last (skipped when unchanged); ortho_mode
 * and anything else that loads these registers drops them */
static struct {
    int proj_valid;
    float proj[6];              /* p22, p23, viewport x, y, w, h */
    int cam_valid;              /* PNMTX1 holds the camera of cam[] */
    float cam[12];
    int cull;                   /* GX cull mode, -1 unknown */
    int car_valid;              /* PNMTX1 holds the car matrix car[] (then not the camera) */
    float car[12];
    int pin_valid;              /* native_projection's inputs as pin[] when it last set the projection */
    uint32_t pin[12];
} g_nst = {0, {0}, 0, {0}, -1, 0, {0}, 0, {0}};

/* a car object's matrix into PNMTX1: the batches of an object have the same one (some 80 a car), loaded once */
static void car_mtx_load(Mtx mtx)
{
    const uint32_t *a = (const uint32_t *)mtx, *b = (const uint32_t *)g_nst.car;
    const uint32_t x = (a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) | (a[4] ^ b[4]) | (a[5] ^ b[5]) |
                       (a[6] ^ b[6]) | (a[7] ^ b[7]) | (a[8] ^ b[8]) | (a[9] ^ b[9]) | (a[10] ^ b[10]) | (a[11] ^ b[11]);
    if (!g_nst.car_valid || x || !g_opt_new) {
        memcpy(g_nst.car, mtx, sizeof g_nst.car);
        g_nst.car_valid = 1;
        g_nst.cam_valid = 0;
        GX_LoadPosMtxImm(mtx, GX_PNMTX1);
    }
    set_mtx(GX_PNMTX1);
}

static void set_cull(int mode)
{
    if (g_nst.cull != mode) {
        g_nst.cull = mode;
        GX_SetCullMode((u8)mode);
    }
}

/* back to the state the GS primitive path draws with */
static void ortho_mode(void)
{
    Mtx44 proj;
    if (!g_native_mode)
        return;
    if (g_native_mode == NW_CAR_COMBO || g_native_mode == NW_CAR_COMBO_IMM)
        GX_SetNumTexGens(1);
    g_native_mode = 0;
    g_nst.proj_valid = 0;
    memset(proj, 0, sizeof proj);
    proj[0][0] = 2.0f / EFB_W;
    proj[0][3] = -1.0f;
    proj[1][1] = -2.0f / EFB_H;
    proj[1][3] = 1.0f;
    proj[2][2] = -1.0f;
    proj[2][3] = -1.0f;
    proj[3][3] = 1.0f;
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_SetViewport(0, 0, EFB_W, EFB_H, 0, 1);
    set_mtx(GX_PNMTX0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_NRM, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_NRM, GX_TEXMTX0);
    set_cull(GX_CULL_NONE);
    GX_SetClipMode(GX_CLIP_DISABLE);
}

/* GX state for native draws of a kind: vertices index the game's arrays */
#define NW_LIT_DL 16             /* native_mode: lit world as display lists */
#define NW_INST_LIT 17           /* native_mode: lit instanced props, colours per vertex */
#define NW_CAR_DL 18             /* native_mode: car pass 1 as display lists */
#define NW_CAR_ENV 19            /* native_mode: car pass 2, indexed positions and direct UVs */

static uint32_t *litcol_alloc(uint32_t n);

static void native_mode(int kind)
{
    if (g_native_mode == kind)
        return;
    if (g_native_mode == NW_CAR_COMBO || g_native_mode == NW_CAR_COMBO_IMM)
        GX_SetNumTexGens(1);
    if (!g_native_mode) {
        GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetChanMatColor(GX_COLOR1A1, (GXColor){255, 255, 255, 255});
        GX_SetClipMode(GX_CLIP_ENABLE);
    }
    g_native_mode = kind;
    GX_ClearVtxDesc();
    if (kind == NW_SKY) {
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX3);
        return;
    }
    if (kind == NW_CAR || kind == NW_CAR_COMBO_IMM) {
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        if (kind == NW_CAR_COMBO_IMM) {
            GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
            GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
            GX_SetNumTexGens(2);
        }
        return;
    }
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
    if (kind == NW_CAR_DL || kind == NW_CAR_COMBO) {
        /* the lists carry an index for the sphere map's UVs too: read and,
         * with one texture coordinate generated, not used */
        GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
        GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);
        GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX8);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX2);
        if (kind == NW_CAR_COMBO) {
            GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
            GX_SetNumTexGens(2);
        }
    } else if (kind == NW_CAR_ENV_DL) {
        /* the list's vertices as pass 1 reads them; the colour the pass's constant, the texture coordinate from TEX1
         * (the sphere map's UV bytes, 8 bits of fraction: what car_pass_dl sent as floats) */
        GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
        GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);
        GX_SetVtxDesc(GX_VA_TEX1, GX_INDEX8);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
    } else if (kind == NW_CAR_ENV) {
        /* the colour is the pass's constant (material register) */
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    } else if (kind == NW_INST_LIT) {
        GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX8);
        GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    } else if (kind == NW_INST) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    } else {
        /* NW_LIT_DL: lit world display lists, colours from a per-frame array */
        GX_SetVtxDesc(GX_VA_CLR0, kind == NW_LIT_DL ? GX_INDEX8 : GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX1);
    }
}

/* World render data as GX display lists, by guest address. A cheap check of
 * the data catches memory reused for other geometry (streamed sections). */
#define CL_BIG 24
typedef struct DList {
    /* the first cache line is what a lookup and a call read: the bucket's chain (block, check, idxcol, next) and the list
     * (data, size, the arrays' bases); a chain of lists was two or three lines a list, the key at the front and the link
     * 250 bytes on */
    uint32_t block, check;
    struct DList *next;
    void *data;
    uint32_t size;
    uint32_t pos, uv;           /* array bases (guest, 32-byte aligned) */
    uint8_t idxcol;             /* colours indexed (lit world: an array made per frame) */
    uint8_t uv32;               /* float UVs (VTXFMT2), else 4.12 UVs and colours (VTXFMT1) */
    uint8_t nbig;
    uint8_t cl_has;             /* its last look found triangles to cut, a bit for each picture it is drawn into (clip_ctx) */
    uint32_t flushed;           /* frame its arrays were last written out for GX */
    uint32_t n;                 /* vertices */
    uint32_t uvn;               /* car lists: UV slots (8 bytes each) */
    uint32_t ahash;             /* gxflags2 128: hash of its arrays when first drawn */
    uint32_t vh;                /* the first and the last word of its arrays (the in-place change watch) */
    float bmin[3], bmax[3];     /* bounding box of the positions */
    /* the CL_BIG biggest triangles (by their longest edge, squared), biggest first, and the vertex each one ends at
     * (clip_batch: only a big triangle can reach far outside the screen from within it) */
    float big_e2[CL_BIG];
    uint8_t big_i[CL_BIG];
    /* lit world (lit_draw_dl): room for the colours behind the list (ext bytes), the bounds of the
     * vertices' reflected light for the light direction lit_d, and whether the colours there are those
     * of floor table lit_serial (lit_have: 1 the bounds, 2 the colours; lit_frame: last drawn so) */
    uint32_t ext, lit_d[3], lit_serial, lit_frame;
    float rb[6];
    uint8_t lit_have;
} __attribute__((aligned(32))) DList;
#define DL_MAX 2048
#define DL_HASH 2048
#define DL_KEEP 60              /* frames a list goes undrawn and is still carried over */
static DList g_dls[DL_MAX];
static int g_ndl;
static DList *g_dl_bucket[DL_HASH], *g_dl_free;
static uint32_t g_dl_hw;        /* entries of g_dls ever used */
static uint32_t g_dl_seg, g_dl_adv_frame, g_dl_bytes;
/* status line: times all lists were dropped, segments turned, lists carried over and left behind */
static uint32_t g_dl_resets, g_dl_turns, g_dl_moved, g_dl_aged;

static void dl_drop(DList *d)
{
    DList **pp = &g_dl_bucket[(d->block >> 4) % DL_HASH];
    while (*pp && *pp != d)
        pp = &(*pp)->next;
    if (*pp)
        *pp = d->next;
    g_dl_bytes -= ((d->size + 31) & ~31u) + d->ext;
    d->data = NULL;
    d->next = g_dl_free;
    g_dl_free = d;
    g_ndl--;
}

/* Room for a list of up to cap bytes, and an entry for it (dl_entry).
 *
 * The lists were one arena, emptied when full: driving that was every 12
 * seconds, and the frame after it made every list in view again. Now the
 * arena is a ring of segments. When the one being filled is full the next
 * one, empty, is filled, and the one after that is emptied for the turn
 * after: its lists drawn in the last DL_KEEP frames are copied to the new
 * segment (a list is bytes: no pointers in it), the others are left behind.
 * Nothing is written where the GPU may still read: a segment is written a
 * whole turn after its lists left it (two turns within three frames, as when
 * a race starts: the GPU is waited for). */
static uint8_t *dl_room(uint32_t cap)
{
    uint32_t tries, i;
    if (cap > DL_SEG)
        return NULL;
    for (tries = 0; g_dl_used + cap > (g_dl_seg + 1) * DL_SEG || (!g_dl_free && g_dl_hw == DL_MAX); tries++) {
        const uint32_t next = (g_dl_seg + 1) % DL_SEGS, victim = (next + 1) % DL_SEGS;
        if (tries == DL_SEGS) {
            /* everything is in use: start over */
            gp_drain();
            g_dl_resets++;
            g_ndl = 0;
            g_dl_hw = 0;
            g_dl_free = NULL;
            g_dl_bytes = 0;
            memset(g_dl_bucket, 0, sizeof g_dl_bucket);
            g_dl_seg = 0;
            g_dl_used = 0;
            g_dl_adv_frame = g_frame_no;
            break;
        }
        if (g_frame_no - g_dl_adv_frame < 3)
            gp_drain();
        g_dl_adv_frame = g_frame_no;
        g_mark_frame[5] = g_frame_no;
        g_dl_turns++;
        g_dl_seg = next;
        g_dl_used = next * DL_SEG;
        for (i = 0; i < g_dl_hw; i++) {
            DList *d = &g_dls[i];
            uint32_t size;
            if (!d->data || (uint32_t)((uint8_t *)d->data - g_dl_arena) / DL_SEG != victim)
                continue;
            size = ((d->size + 31) & ~31u) + d->ext;
            if (g_frame_no - d->flushed <= DL_KEEP && g_dl_used + size <= (next + 1) * DL_SEG) {
                uint8_t *to = g_dl_arena + g_dl_used;
                memcpy(to, d->data, size);
                DCFlushRange(to, size);
                d->data = to;
                g_dl_used += size;
                g_dl_moved++;
            } else {
                dl_drop(d);
                g_dl_aged++;
            }
        }
    }
    return g_dl_arena + g_dl_used;
}

/* the entry of a list of size bytes made at dl_room's address */
static DList *dl_entry(uint32_t size)
{
    DList *d = g_dl_free;
    if (d)
        g_dl_free = d->next;
    else
        d = &g_dls[g_dl_hw++];
    memset(d, 0, sizeof *d);
    g_ndl++;
    g_dl_used += (size + 31) & ~31u;
    g_dl_bytes += (size + 31) & ~31u;
    return d;
}


static uint32_t dl_check(const NwBlock *b)
{
    uint32_t m = b->n / 2;
    /* no colours (instanced props): address 4m is the scratchpad, which
     * changes every frame -- the lists were remade every frame */
    return mix(mix(mix(mix(b->n, LW(b->pos)), LW(b->pos + 16 * (b->n - 1) + 8)), LW(b->uv + 4 * m)),
               b->col ? LW(b->col + 4 * m) : 0);
}

/* The lines dl_get will read for a block, touched ahead of it (vif.c: a batch or two before the draw): stage 0 the hash
 * bucket and the words dl_check samples (guest data nothing else touches on the way), stage 1 the bucket's first list
 * (what stage 0 read gave its address; its key and link are read at the draw). Hints. */
void gs_world_prefetch(const NwBlock *b, int stage)
{
    const uint32_t h = (b->pos >> 4) % DL_HASH;
    const DList *d;
    if (b->n > 220)
        return;
    if (stage == 0) {
        const uint32_t m = b->n / 2;
        __builtin_prefetch(&g_dl_bucket[h]);
        __builtin_prefetch(GPTR(b->pos));
        __builtin_prefetch(GPTR(b->pos + 16 * (b->n - 1) + 8));
        __builtin_prefetch(GPTR(b->uv + 4 * m));
        if (b->col)
            __builtin_prefetch(GPTR(b->col + 4 * m));
        return;
    }
    d = g_dl_bucket[h];
    if (d)
        __builtin_prefetch(d);              /* (its key and link are read when the batch is drawn: not here) */
}

/* Vertices index the game's own arrays (positions: stride 16, UVs: stride 4),
 * from array bases rounded down to 32 bytes; colours are converted inline. */
static void dl_vertex(const NwBlock *b, uint32_t k, uint32_t pos_skip, uint32_t uv_skip, int idxcol)
{
    GX_Position1x8((u8)(k + pos_skip));
    if (idxcol) {
        GX_Color1x8((u8)k);
    } else if (b->rgb) {
        /* lit world: the microprogram's lighting only makes framebuffer alpha */
        uint32_t a = b->col + 3 * k;
        GX_Color1u32((uint32_t)LB(a) << 24 | (uint32_t)LB(a + 1) << 16 | (uint32_t)LB(a + 2) << 8);
    } else if (b->col) {
        uint32_t c = LW(b->col + 4 * k);
        GX_Color1u32((c & 0xFF) << 24 | ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | a2(c >> 24));
    }
    GX_TexCoord1x8((u8)(k + uv_skip));
}

/* A list's arrays are the game's own memory, and GX reads them from RAM, not
 * from the CPU's cache: written out when the list is made (dl_get), when it
 * is drawn again after frames without it, and every 16th frame, each list in
 * a frame of its own, should the game ever change vertices in place. Every
 * frame, as it was, 700 lists' arrays were some 66000 cache lines a frame
 * (nothing in Dolphin, time on the console). gxflags2 256: every frame;
 * gxflags2 128: the lists whose arrays did change behind an unchanged check
 * are counted (a hash of all of both arrays, kept from the first drawing). */
static uint32_t g_dl_changed, g_dl_flushes;
static uint32_t g_dl_syncs;              /* lists written out to RAM for the CP to fetch */

static void dl_touch(DList *dl, uint32_t uvbytes)
{
    const uint32_t last = dl->flushed;
    if (last == g_frame_no)
        return;
    dl->flushed = g_frame_no;
    if (g_gx_flags2 & 128) {
        const uint32_t *a = (const uint32_t *)GPTR(dl->pos), *b = (const uint32_t *)GPTR(dl->uv);
        uint32_t h = 0x811C9DC5u, k;
        for (k = 0; k < dl->n * 4; k++)
            h = (h ^ a[k]) * 0x01000193u;
        for (k = 0; k < uvbytes / 4; k++)
            h = (h ^ b[k]) * 0x01000193u;
        h |= 1;
        if (dl->ahash && dl->ahash != h && g_dl_changed++ < 10)
            LOG("gx: list %08X (%u vertices): its arrays changed in place (frame %u)", dl->block, dl->n, g_frame_no);
        dl->ahash = h;
    }
    if (last + 1 != g_frame_no || !((g_frame_no + (uint32_t)(dl - g_dls)) & 15) || (g_gx_flags2 & 256)) {
        DCFlushRange(GPTR(dl->pos), dl->n * 16 + 32);
        DCFlushRange(GPTR(dl->uv), uvbytes + 32);
        /* The GPU's vertex cache holds the vertices it fetched: the game may
         * have rewritten them in place since (the billboards), so the cache
         * has to go when the arrays are written out.  Without this the
         * console draws vertices up to 16 frames old, which Dolphin (with its
         * coherent memory) never shows. */
        GX_InvVtxCache();
        g_dl_flushes++;
    }
    if (g_gx_flags2 & 67108864) {
        /* The game rewrites the vertices in place (the billboards), and the
         * GPU reads them from RAM, not from the CPU's cache: they have to be
         * written out before the draw, not every 16th frame.  The first and
         * the last word of the two arrays are watched - a billboard's
         * vertices all move - and the flush above stays the backstop for a
         * change in between. */
        const uint32_t *p = (const uint32_t *)GPTR(dl->pos), *u = (const uint32_t *)GPTR(dl->uv);
        uint32_t h = p[0] ^ p[(dl->n - 1) * 4] ^ u[0] ^ u[uvbytes / 4 - 1] ^ (dl->n * 4);
        if (h != dl->vh) {
            dl->vh = h;
            DCFlushRange(GPTR(dl->pos), dl->n * 16 + 32);
            DCFlushRange(GPTR(dl->uv), uvbytes + 32);
            GX_InvVtxCache();
            g_dl_flushes++;
        }
    }
}

/* gxflags2 536870912: the calibration pattern - the BP's own interpolation,
 * measured.
 *
 * Two pieces with the same affine colour function over the screen:
 *  - the top half: one triangle whose vertices are far outside the screen, so
 *    the BP interpolates the ramp over a triangle far bigger than the screen.
 *    That is the guard band's case: the PS2's GS clipped every triangle to its
 *    drawing area (the screen) and interpolated over that, the Wii's BP clips
 *    to its guard band (far bigger) and interpolates over that.
 *  - the bottom half: the same ramp over a quad the size of the screen, the
 *    control.
 * The two halves show the same colour at the same x on a GPU that
 * interpolates in floats (Dolphin's HLE) and different ones on a GPU that
 * does it in fixed point (the Software Renderer, and the console).  The
 * measurement is the per-column mean of the two halves: the same slope means
 * the BP's fixed point is not modelled, a different slope means it is.
 */
#define CALIB_C(x, y) (97.27f + 0.094866f * (x) + 0.000244f * (y))
/* The calibration pattern's own dump: the EFB's picture area as RGBA, so the
 * ramps can be measured (the file is next to the image: tools/wii/fatcat.py
 * reads it). */
static void calib_dump(void)
{
    FILE *plat_debug_file(const char *name);
    FILE *f;
    static uint8_t buf[BIG_W * BIG_H * 4];
    static uint8_t *tex;
    static uint32_t cap;
    uint32_t size = GX_GetTexBufferSize(BIG_W, BIG_H, GX_TF_RGBA8, GX_FALSE, 0);
    int x, y;
    if (cap < size) {
        tex = gx_alloc(size);
        cap = size;
    }
    if (!tex || !(f = plat_debug_file("calib.rgba")))
        return;
    batch_flush();
    GX_SetTexCopySrc(0, 0, BIG_W, BIG_H);
    GX_SetTexCopyDst(BIG_W, BIG_H, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(tex, GX_FALSE);
    GX_PixModeSync();
    gp_drain();
    DCInvalidateRange(tex, size);
    for (y = 0; y < BIG_H; y++)
        for (x = 0; x < BIG_W; x++) {
            const uint8_t *p = tex + rgba8_off(x, y, BIG_W);
            buf[(y * BIG_W + x) * 4 + 0] = p[1];
            buf[(y * BIG_W + x) * 4 + 1] = p[32];
            buf[(y * BIG_W + x) * 4 + 2] = p[33];
            buf[(y * BIG_W + x) * 4 + 3] = p[0];
        }
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    LOG("gx: dumped the calibration pattern (%ux%u, first row %u %u %u)", BIG_W, BIG_H, buf[0], buf[1], buf[2]);
}

static void calib_draw(const RT *rt)
{
    Mtx m;
    int i;
    /* the huge triangle: the same colour function as the quad's corners */
    static const float hx[3] = {-1024.0f, 1664.0f, 320.0f};
    static const float hy[3] = {-512.0f, -512.0f, 1536.0f};
    /* the control quad: the bottom half of the screen */
    static const float qx[4] = {0.0f, 640.0f, 640.0f, 0.0f};
    static const float qy[4] = {224.0f, 224.0f, 448.0f, 448.0f};
    if (!rt)
        return;
    /* the state the port's own overlays draw with */
    g_native_mode = -1;                 /* ortho_mode() returns early when it is 0: the descriptors would be the last draw's */
    ortho_mode();
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    guMtxIdentity(m);
    GX_LoadPosMtxImm(m, GX_PNMTX0);
    GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX2x4);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    /* the port's untextured recipe (apply_state): the rasterised colour in the fourth input with ADD.
     * GX_SetTevOp(GX_REPLACE) puts the texture's colour there, and with no texture bound that is black. */
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetScissor(0, 0, BIG_W, BIG_H);
    /* the huge triangle: the vertex format wants all four attributes */
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
    for (i = 0; i < 3; i++) {
        uint32_t c = (uint32_t)(CALIB_C(hx[i], hy[i]) + 0.5f);
        GX_Position3f32(hx[i], hy[i], 0.0f);
        GX_Normal3f32(0.0f, 0.0f, 1.0f);
        GX_Color1u32((c << 16) | (c << 8) | c);
        GX_Color1u32(0xFFFFFFFFu);
    }
    GX_End();
    /* the control quad: the same function at its corners */
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (i = 0; i < 4; i++) {
        uint32_t c = (uint32_t)(CALIB_C(qx[i], qy[i]) + 0.5f);
        GX_Position3f32(qx[i], qy[i], 0.0f);
        GX_Normal3f32(0.0f, 0.0f, 1.0f);
        GX_Color1u32((c << 16) | (c << 8) | c);
        GX_Color1u32(0xFFFFFFFFu);
    }
    GX_End();
    /* the state the port keeps is no longer the GX's, and its draws want one texture coordinate */
    GX_SetNumTexGens(1);
    g_state_valid = 0;
    gxc_drop();
}

static DList *dl_get(const NwBlock *bp, int idxcol)
{
    const NwBlock *b = bp;
    DList *d;
    uint32_t block = b->pos, check, h = (block >> 4) % DL_HASH, cap, i, size, pos_skip, uv_skip, ext;
    uint8_t *buf;
    if (b->n > 220)
        return NULL;
    if (g_trust & 4) {
        /* debugging: the list of this block, without the sampled words (a block with no list yet is made below) */
        for (d = g_dl_bucket[h]; d; d = d->next)
            if (d->block == block && d->n == b->n && d->idxcol == idxcol)
                return d;
    }
    check = dl_check(b);
    for (d = g_dl_bucket[h]; d; d = d->next)
        if (d->block == block && d->check == check && d->idxcol == idxcol)
            return d;

    /* worst case: a strip per triangle, each with a duplicated first vertex; a lit list's colours behind it */
    cap = (b->n * 4 * 6 + 64 + 31) & ~31u;
    ext = idxcol ? (b->n * 4 + 31) & ~31u : 0;
    pos_skip = (b->pos & 31) / 16;
    uv_skip = (b->uv & 31) / (b->uv32 ? 8 : 4);
    /* GX reads the arrays from memory: write the CPU's copy out */
    DCFlushRange(GPTR(b->pos & ~31u), b->n * 16 + 32);
    DCFlushRange(GPTR(b->uv & ~31u), b->n * (b->uv32 ? 8 : 4) + 32);
    GX_InvVtxCache();
    buf = dl_room(cap + ext);
    if (!buf)
        return NULL;
    DCInvalidateRange(buf, cap);
    GX_BeginDispList(buf, cap);
    /* PS2 strips: the triangle ending at a vertex is drawn unless its ADC bit
     * (w bit 15) is set. Runs of drawn triangles become GX strips; a strip
     * that would start at an odd vertex repeats its first vertex, keeping the
     * winding parity the PS2 culling was computed with. */
    for (i = 2; i < b->n;) {
        uint32_t first, last, k, dup;
        if (LW(b->pos + 16 * i + 12) & 0x8000) {
            i++;
            continue;
        }
        first = i - 2;
        while (i < b->n && !(LW(b->pos + 16 * i + 12) & 0x8000))
            i++;
        last = i - 1;
        dup = first & 1;
        GX_Begin(GX_TRIANGLESTRIP, b->uv32 ? GX_VTXFMT2 : GX_VTXFMT1, (u16)(last - first + 1 + dup));
        if (dup)
            dl_vertex(b, first, pos_skip, uv_skip, idxcol);
        for (k = first; k <= last; k++)
            dl_vertex(b, k, pos_skip, uv_skip, idxcol);
        GX_End();
    }
    size = GX_EndDispList();
    if (!size)
        return NULL;
    /* the CP fetches the list from RAM, not from the CPU's cache: the list
     * must be written out before it is called (the console reads it; the
     * emulator's memory is coherent and never sees the difference) */
    DCFlushRange(buf, size);
    g_dl_syncs++;
    d = dl_entry(((size + 31) & ~31u) + ext);
    d->block = block;
    d->check = check;
    d->flushed = g_frame_no;
    d->data = buf;
    d->size = size;
    d->ext = ext;
    d->pos = b->pos & ~31u;
    d->uv = b->uv & ~31u;
    d->n = b->n;
    d->uv32 = b->uv32;
    d->idxcol = (uint8_t)idxcol;
    for (i = 0; i < b->n; i++) {
        uint32_t c;
        for (c = 0; c < 3; c++) {
            float v = LWF(b->pos + 16 * i + 4 * c);
            if (!i || v < d->bmin[c]) d->bmin[c] = v;
            if (!i || v > d->bmax[c]) d->bmax[c] = v;
        }
    }
    for (i = 2; i < b->n; i++) {
        uint32_t e, k;
        float l2max = 0.0f;
        if (LW(b->pos + 16 * i + 12) & 0x8000)
            continue;
        for (e = 0; e < 3; e++) {
            const uint32_t u = i - 2 + e, v = i - 2 + (e + 1) % 3;
            const float dx = LWF(b->pos + 16 * u) - LWF(b->pos + 16 * v), dy = LWF(b->pos + 16 * u + 4) - LWF(b->pos + 16 * v + 4),
                        dz = LWF(b->pos + 16 * u + 8) - LWF(b->pos + 16 * v + 8);
            const float l2 = dx * dx + dy * dy + dz * dz;
            if (l2 > l2max)
                l2max = l2;
        }
        /* into the table of the biggest, which stays in order */
        if (d->nbig == CL_BIG && l2max <= d->big_e2[CL_BIG - 1])
            continue;
        k = d->nbig < CL_BIG ? d->nbig++ : CL_BIG - 1;
        while (k && d->big_e2[k - 1] < l2max) {
            d->big_e2[k] = d->big_e2[k - 1];
            d->big_i[k] = d->big_i[k - 1];
            k--;
        }
        d->big_e2[k] = l2max;
        d->big_i[k] = (uint8_t)i;
    }
    d->next = g_dl_bucket[h];
    g_dl_bucket[h] = d;
    g_dl_built++;
    iop_sd_pump();              /* (a track's first frames make hundreds: sound meanwhile) */
    return d;
}

/* GX transform for VU1 world constants (data memory qwords 1014..1019): the
 * x, y and w rows of a world-to-screen matrix, screen size and centre, Z
 * scale and offset. Screen x = centre + size * (X/W - 0.5), Z = 16 * (offset +
 * scale / W); the projection reproduces what the GS path makes of that Z. */
#define VQ(n, c) ((const float *)(g_vu_mem + 0xC000 / 4))[(n) * 4 + (c)]

/* the camera as a GX position matrix: rows X, Y and -W (loaded only when it
 * changed: the whole frame's world uses one).
 *
 * The microprogram 47E300 is 4745C0 with the world mirrored about the plane
 * y = 1009.y, the water level (at 0x02A8: the camera's y column negated, the
 * translation plus twice 1009.y of it, put back at the end): the scenery's
 * copy for its reflection in the lake, coarser than the scenery and around
 * it. Drawn as it is, unmirrored, that copy lay over the road and the verges
 * where it is higher than they are: the ground over the lower part of the
 * car by the first fence of Silver Lake, at times most of the picture. */
static void native_camera(void)
{
    extern uint32_t g_vu1_code;
    Mtx m;
    int k;
    for (k = 0; k < 4; k++) {
        m[0][k] = VQ(1014 + k, 0);
        m[1][k] = VQ(1014 + k, 1);
        m[2][k] = -VQ(1014 + k, 3);
    }
    if (g_vu1_code == 0x0047E300u && !(g_gx_flags & 134217728)) {
        const float h2 = 2.0f * VQ(1009, 1);
        for (k = 0; k < 3; k++) {
            m[k][3] += h2 * m[k][1];
            m[k][1] = -m[k][1];
        }
    }
    {
        /* twelve words, 500 times a frame: not through memcmp */
        const uint32_t *a = (const uint32_t *)m, *b = (const uint32_t *)g_nst.cam;
        const uint32_t x = (a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) | (a[4] ^ b[4]) | (a[5] ^ b[5]) |
                           (a[6] ^ b[6]) | (a[7] ^ b[7]) | (a[8] ^ b[8]) | (a[9] ^ b[9]) | (a[10] ^ b[10]) | (a[11] ^ b[11]);
        if (!g_nst.cam_valid || x) {
            memcpy(g_nst.cam, m, sizeof g_nst.cam);
            g_nst.cam_valid = 1;
            g_nst.car_valid = 0;
            GX_LoadPosMtxImm(m, GX_PNMTX1);
        }
    }
    set_mtx(GX_PNMTX1);
}

/* Instanced props (471FA0): instance i has columns 934+5i..937+5i (x, y and,
 * in w, z) and colour 938+5i; the microprogram puts the camera in front. */
static void native_instance(uint32_t i)
{
    static const int row[3] = {0, 1, 3};
    Mtx m;
    int j, r;
    for (j = 0; j < 4; j++) {
        uint32_t o = 934 + 5 * i + j;
        for (r = 0; r < 3; r++) {
            int c = row[r];
            float v = VQ(1014, c) * VQ(o, 0) + VQ(1015, c) * VQ(o, 1) + VQ(1016, c) * VQ(o, 3) +
                      (j == 3 ? VQ(1017, c) : 0.0f);
            m[r][j] = r == 2 ? -v : v;
        }
    }
    g_nst.cam_valid = 0;
    g_nst.car_valid = 0;
    GX_LoadPosMtxImm(m, GX_PNMTX1);
    set_mtx(GX_PNMTX1);
}

/* projection and viewport for the VU1 world constants */
/* GS Z = zmul * (1019.z + 1018.z / W); measured against VU1 on the PC host
 * (B3_WORLDCMP, B3_CARCMP): 1 for the world and the cars (16 made the
 * nearest geometry pass the far plane: holes in the road, cars cut open) */
static void native_projection(const RT *rt, const GsDraw *d, float zmul)
{
    const float *q = (const float *)(g_vu_mem + 0xC000 / 4);
    const uint32_t *qw = g_vu_mem + 0xC000 / 4;
    Mtx44 p;
    float s, zscale, sx, sy, ofx, ofy, vx, vy;
    uint32_t zpsm = GS_FIELD(d->zbuf, 24, 4);
    /* what the projection is made of, compared first: the same for most of a frame's thousand calls */
    const uint32_t in[12] = {(uint32_t)rt->sh, (uint32_t)rt->ex, (uint32_t)rt->ey, zpsm, F2BITS(zmul),
                             (uint32_t)(d->xyoffset & 0xFFFF) | (uint32_t)((d->xyoffset >> 16) & 0xFFFF0000u),
                             qw[1018 * 4], qw[1018 * 4 + 1], qw[1018 * 4 + 2], qw[1019 * 4], qw[1019 * 4 + 1],
                             qw[1019 * 4 + 2]};
    if (g_nst.proj_valid && g_nst.pin_valid && g_opt_new &&
        !((in[0] ^ g_nst.pin[0]) | (in[1] ^ g_nst.pin[1]) | (in[2] ^ g_nst.pin[2]) | (in[3] ^ g_nst.pin[3]) |
          (in[4] ^ g_nst.pin[4]) | (in[5] ^ g_nst.pin[5]) | (in[6] ^ g_nst.pin[6]) | (in[7] ^ g_nst.pin[7]) |
          (in[8] ^ g_nst.pin[8]) | (in[9] ^ g_nst.pin[9]) | (in[10] ^ g_nst.pin[10]) | (in[11] ^ g_nst.pin[11])))
        return;
    memcpy(g_nst.pin, in, sizeof in);
    g_nst.pin_valid = 1;
    s = 1.0f / (float)(1 << rt->sh);
#define Q(n, c) q[(n) * 4 + (c)]
    zscale = zpsm == 0 ? 1.0f / 4294967296.0f : zpsm == 1 ? 1.0f / 16777216.0f : 1.0f / 65536.0f;
    {
        float k[6];
        k[0] = 1.0f - ZDS * zmul * zscale * Q(1019, 2);
        k[1] = ZDS * zmul * zscale * Q(1018, 2);
        sx = Q(1018, 0);
        sy = Q(1018, 1);
        ofx = (float)GS_FIELD(d->xyoffset, 0, 16) / 16.0f;
        ofy = (float)GS_FIELD(d->xyoffset, 32, 16) / 16.0f;
        vx = (float)rt->ex + s * (Q(1019, 0) - ofx - 0.5f * sx) - s * sx;
        vy = (float)rt->ey + s * (Q(1019, 1) - ofy - 0.5f * sy) - s * sy;
        k[2] = vx;
        k[3] = vy;
        k[4] = 2.0f * s * sx;
        k[5] = 2.0f * s * sy;
        {
            /* six words, a thousand times a frame: not through memcmp */
            const uint32_t *a = (const uint32_t *)k, *b = (const uint32_t *)g_nst.proj;
            if (g_nst.proj_valid && !((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) | (a[4] ^ b[4]) | (a[5] ^ b[5])))
                return;
        }
        memcpy(g_nst.proj, k, sizeof k);
        g_nst.proj_valid = 1;
        memset(p, 0, sizeof p);
        p[0][0] = 1.0f;
        p[1][1] = -1.0f;
        p[2][2] = k[0];
        p[2][3] = k[1];
        p[3][2] = -1.0f;
        GX_LoadProjectionMtx(p, GX_PERSPECTIVE);
        GX_SetViewport(k[2], k[3], k[4], k[5], 0, 1);
    }
#undef Q
}

static float g_uv_scroll;       /* in TEXMTX1 */

/* scrolling textures (world microprograms): 1005.x set, u += 1005.y */
static void native_scroll(const uint32_t *vm)
{
    float scroll = (vm[1005 * 4] & 0xFFFF) ? BITS2F(vm[1005 * 4 + 1]) : 0.0f;
    if (scroll != g_uv_scroll) {
        Mtx t = {{0, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 1, 0}};
        t[0][3] = g_uv_scroll = scroll;
        GX_LoadTexMtxImm(t, GX_TEXMTX1, GX_MTX2x4);
    }
}

/* debugging (gxflags 67108864, "carwatch"): the camera and screen constants
 * (VU1 1014..1019) the scene's world batches and the player's car batches
 * are drawn with, per frame: how many different sets, the first and the last */
static struct {
    uint32_t n[2], sum[2][2];
    float last[2][8];           /* 1017.xyzw, 1018.z, 1019.z, 1014.w, 1016.w */
    float mat[16];              /* 1014..1017 of the world batches */
} g_cwcam;

/* ... and the drawn ground: the heights of the world triangles of the scene
 * above and below the player's car (within 4 m of its height), highest first */
static struct {
    uint32_t n, code[6], mirrored;
    float y[6], slope[6], h;
} g_cwgnd;

static void cw_ground(const NwBlock *b)
{
    extern uint32_t g_vu1_code;
    const uint32_t mat = LW(0x57BAF0 + 516);
    float px, py, pz;
    uint32_t k, j;
    if (!mat || mat >= PS2_RAM_SIZE - 64 || b->n < 3)
        return;
    px = LWF(mat + 48);
    py = LWF(mat + 52);
    pz = LWF(mat + 56);
    for (k = 2; k < b->n; k++) {
        float v[3][3], d, u, w, y, ny;
        uint32_t c;
        if (LW(b->pos + 16 * k + 12) & 0x8000)
            continue;
        for (j = 0; j < 3; j++)
            for (c = 0; c < 3; c++)
                v[j][c] = LWF(b->pos + 16 * (k - 2 + j) + 4 * c);
        d = (v[1][0] - v[0][0]) * (v[2][2] - v[0][2]) - (v[2][0] - v[0][0]) * (v[1][2] - v[0][2]);
        if (d > -1e-4f && d < 1e-4f)
            continue;
        u = ((px - v[0][0]) * (v[2][2] - v[0][2]) - (v[2][0] - v[0][0]) * (pz - v[0][2])) / d;
        w = ((v[1][0] - v[0][0]) * (pz - v[0][2]) - (px - v[0][0]) * (v[1][2] - v[0][2])) / d;
        if (u < 0.0f || w < 0.0f || u + w > 1.0f)
            continue;
        y = v[0][1] + u * (v[1][1] - v[0][1]) + w * (v[2][1] - v[0][1]);
        if (g_vu1_code == 0x0047E300u && !(g_gx_flags & 134217728)) {
            g_cwgnd.mirrored++;
            g_cwgnd.h = VQ(1009, 1);
            y = 2.0f * g_cwgnd.h - y;
        }
        if (y < py - 4.0f || y > py + 4.0f)
            continue;
        /* how steep: the normal's y over its length */
        {
            const float ax = v[1][0] - v[0][0], ay = v[1][1] - v[0][1], az = v[1][2] - v[0][2];
            const float bx = v[2][0] - v[0][0], by = v[2][1] - v[0][1], bz = v[2][2] - v[0][2];
            const float nx = ay * bz - az * by, nz = ax * by - ay * bx;
            ny = fabsf(d) / sqrtf(nx * nx + d * d + nz * nz);
        }
        for (j = 0; j < g_cwgnd.n && g_cwgnd.y[j] >= y; j++)
            ;
        if (j >= 6)
            continue;
        if (g_cwgnd.n < 6)
            g_cwgnd.n++;
        for (c = g_cwgnd.n - 1; c > j; c--) {
            g_cwgnd.y[c] = g_cwgnd.y[c - 1];
            g_cwgnd.code[c] = g_cwgnd.code[c - 1];
            g_cwgnd.slope[c] = g_cwgnd.slope[c - 1];
        }
        g_cwgnd.y[j] = y;
        g_cwgnd.code[j] = g_vu1_code;
        g_cwgnd.slope[j] = ny;
    }
}

static void cw_cam_note(int who, const uint32_t *vm)
{
    uint32_t h = 0, k;
    const float *q = (const float *)vm;
    for (k = 1014 * 4; k < 1020 * 4; k++)
        h = h * 31 + vm[k];
    if (g_cwcam.n[who] && g_cwcam.sum[who][1] == h)
        return;
    if (!g_cwcam.n[who])
        g_cwcam.sum[who][0] = h;
    g_cwcam.sum[who][1] = h;
    g_cwcam.n[who]++;
    memcpy(g_cwcam.last[who], q + 1017 * 4, 16);
    g_cwcam.last[who][4] = q[1018 * 4 + 2];
    g_cwcam.last[who][5] = q[1019 * 4 + 2];
    g_cwcam.last[who][6] = q[1014 * 4 + 3];
    g_cwcam.last[who][7] = q[1016 * 4 + 3];
    if (!who)
        memcpy(g_cwcam.mat, q + 1014 * 4, 64);
}

/* does a box reach behind the camera plane (W <= 0) */
static int behind_camera(const float *mn, const float *mx)
{
    int k;
    for (k = 0; k < 8; k++) {
        float x = (k & 1) ? mx[0] : mn[0], y = (k & 2) ? mx[1] : mn[1], z = (k & 4) ? mx[2] : mn[2];
        if (VQ(1014, 3) * x + VQ(1015, 3) * y + VQ(1016, 3) * z + VQ(1017, 3) <= 0.0f)
            return 1;
    }
    return 0;
}

/* PS2 culling (world microprograms). Only two of them cull, 46F0D0 (prelit)
 * and 470650 (lit): the triangle ending at a vertex gets its ADC bit from the
 * sign of its screen area, the sense taken from 1013.z (below 0x20 one
 * winding, from 0x20 up the other; there is no value for "none"). Their
 * twins 4745C0, 47E300 (mirrored: native_camera) and 472EB0 have no such
 * test: two-sided geometry (hedges, foliage, fences) and the lake's
 * reflections, which this drew with half its faces missing
 * while it culled by 1013.z whatever the program. Through the native
 * projection (y flipped) the GX windings are the other way round: 0 keeps
 * what GX calls back faces (the reverse showed the backs of signs, and
 * ground culled away). */
static void native_cull(const uint32_t *vm, int behind)
{
    extern uint32_t g_vu1_code;
    uint32_t cull = vm[1013 * 4 + 2] & 0xFFFF;
    (void)behind;
    if ((g_gx_flags & 32) || (g_vu1_code != 0x0046F0D0u && g_vu1_code != 0x00470650u)) {
        set_cull(GX_CULL_NONE);
        return;
    }
    if (g_gx_flags & 16)
        cull = cull < 0x20 ? 0x20 : 0;
    set_cull(cull < 0x20 ? GX_CULL_FRONT : GX_CULL_BACK);
}

/* Lit world: colours from the render data, alpha (a specular term later
 * passes read from the framebuffer) computed per frame (nw_lit_alpha); sent
 * directly, positions and UVs indexed from guest memory. */
/* Lit world colours for GX, made per frame: two halves by frame parity (the
 * GPU may still draw the previous frame while this one is made; the one
 * before that is done, see gs_backend_vsync) */
/* The cars' colours and sphere map UVs are in it too (6 bytes a vertex: the
 * UVs as bytes; as floats, 12 bytes a vertex, the six cars at the start line,
 * 22000 vertices, did not fit and the last of them went the old way).
 * Split-screen draws every car once a view: at the start line up to 270 KB a
 * frame (a single player's: 228), and with 192 KB the rest, 40000 batches in
 * ten seconds, went to nc_load and nc_shade. Allocated at the first use (MEM2). */
#define LITCOL_HALF (320 * 1024)    /* a frame; full (the status line counts the refusals): the rest is sent immediate */
static uint32_t *g_litcol;
static uint32_t g_litcol_used, g_litcol_frame;
/* status line: the most a frame asked for (words), allocations refused */
static uint32_t g_litcol_want, g_litcol_peak, g_litcol_refused;

/* noinline: inlined into gs_backend_world it crashed gcc 15 (cfgcleanup) */
static uint32_t *__attribute__((noinline)) litcol_alloc(uint32_t n)
{
    uint32_t words = (n + 7) & ~7u;     /* 32-byte aligned arrays */
    if (!g_litcol) {
        static int tried;
        if (tried)
            return NULL;
        tried = 1;
        g_litcol = plat_alloc(2 * LITCOL_HALF);
        if (!g_litcol || ((uintptr_t)g_litcol & 31)) {
            g_litcol = NULL;
            LOG("gx: no memory for the colour arrays: lit world and cars sent immediate");
            return NULL;
        }
    }
    if (g_litcol_frame != g_frame_no) {
        g_litcol_frame = g_frame_no;
        g_litcol_used = 0;
        g_litcol_want = 0;
    }
    g_litcol_want += words;
    if (g_litcol_want > g_litcol_peak)
        g_litcol_peak = g_litcol_want;
    if ((g_litcol_used + words) * 4 > LITCOL_HALF) {
        g_mark_frame[3] = g_frame_no;
        g_litcol_refused++;
        return NULL;
    }
    g_litcol_used += words;
    {
        /* every caller writes all of the array's lines (the colours are made for the whole batch, the rest of the last
         * line is rounding nobody reads): the cache is given the lines without fetching them from memory first (a
         * store to a line it does not have reads it: the colour arrays were 3000 line fills a frame) */
        extern int g_dcbz_on;           /* runtime/memfast.c: off under an emulator without a cache model */
        uint32_t *p = g_litcol + (g_frame_no & 1) * (LITCOL_HALF / 4) + g_litcol_used - words;
        uint32_t *q;
        if (g_dcbz_on)
            for (q = p; q < p + words; q += 8)
                __asm__ volatile("dcbz 0,%0" : : "r"(q) : "memory");
        return p;
    }
}

/* status line: lit lists drawn, those with their colours kept from an earlier frame, colours written for
 * keeping, and (the check) kept colours that were not what the vertices' own sums give */
static uint32_t g_lit_lists, g_lit_kept, g_lit_written, g_lit_bad;

/* The colours of a lit list none of whose vertices has a highlight now (most
 * lists, most frames): made once and kept behind the list. NULL: this frame's
 * come from the vertices. gxflags2 65536: never kept; 1024: every kept
 * list's colours checked against the vertices' (else the first 3000). */
static uint32_t *lit_kept(const NwBlock *b, DList *dl, const uint32_t *vm)
{
    const NwLitFloor *f;
    uint32_t *col;
    if (!dl->ext || (g_gx_flags2 & 65536) || !(f = nw_lit_floor(vm)))
        return NULL;
    if (!(dl->lit_have & 1) || memcmp(dl->lit_d, vm + 1007 * 4, 12)) {
        nw_lit_rbounds(vm, b, dl->rb);
        memcpy(dl->lit_d, vm + 1007 * 4, 12);
        dl->lit_have |= 1;
    }
    if (!nw_lit_all_floor(vm, f, dl->rb, dl->bmin, dl->bmax))
        return NULL;
    col = (uint32_t *)((uint8_t *)dl->data + ((dl->size + 31) & ~31u));
    if (!(dl->lit_have & 2) || dl->lit_serial != f->serial) {
        /* (not where the GPU may still read last frame's: another table's colours wait a frame) */
        if ((dl->lit_have & 2) && g_frame_no - dl->lit_frame < 3)
            return NULL;
        nw_lit_floor_rgba(b, f, col);
        DCFlushRange(col, b->n * 4);
        dl->lit_serial = f->serial;
        dl->lit_have |= 2;
        g_lit_written++;
    } else {
        static uint32_t checked;
        g_lit_kept++;
        if (checked < 3000 || (g_gx_flags2 & 1024)) {
            static uint32_t tmp[224];
            checked++;
            nw_lit_rgba(vm, b, tmp);
            if (memcmp(tmp, col, b->n * 4) && g_lit_bad++ < 8)
                LOG("gx: lit list %08X (%u vertices): kept colours differ from the vertices'", dl->block, b->n);
        }
    }
    dl->lit_frame = g_frame_no;
    return col;
}

/* ---- the world's batches cut to the screen (platform/wii/clip.h) ------------------------------
 *
 * The PS2's GS cut every triangle to its drawing area before it drew it.  The Wii's GPU does not: a triangle
 * that reaches far outside the screen, or crosses the near plane (the cut vertex lands at W = near, thousands
 * of screens away), is rasterised and interpolated over its whole extent, and the texture comes out warped,
 * worst close to the camera at the sides, where the road's and the walls' big triangles are.  The game flags
 * the batches that reach the edge of the view (VU1 1013.x: its own microprogram takes a clipping path for
 * them); of those, the ones with a triangle that crosses the near plane, or is outside by more than
 * g_clip_band percent of the screen's size, are drawn here instead of from their list: the triangles that stay
 * as strips as the list has them, the others cut to the screen and drawn as fans from arrays of their own
 * (the ones behind the eye or outside the screen altogether are left out: the GPU would not draw them either).
 * Nothing else changes: the same vertices, the same camera, the same cull, the same state.  A batch with
 * nothing to cut is drawn from its list, as before.
 *
 * Looking at every vertex of every flagged batch costs more than the cut does (the intro's camera: 35000
 * vertices a frame), so the work is cut down:
 *  - the batch's box: wholly behind the eye, nothing; the nearest it comes, with the lengths of its biggest
 *    triangles (the list keeps 24 of them), says which triangles can reach from the screen into the band at that
 *    depth: only a long one can;
 *  - a box that reaches the near plane is looked at by its vertices' depths alone: the triangles with some
 *    vertices behind the eye and some in front cross the plane, those with all behind it are left out, the
 *    rest are the box's business again.
 * Autopilot "clip" (args.txt "--clip", "--clipband"). */
int g_clip = 1;
int g_clip_band = 200;
int g_clip_every = 4;              /* a batch with nothing to cut is looked at every this many frames (each in a phase of its own) */
static uint32_t g_cl_looked, g_cl_tested, g_cl_cut, g_cl_tris, g_cl_dropped, g_cl_verts, g_cl_nospace;
static uint32_t g_cl_few, g_cl_pass_v, g_cl_big_t, g_cl_nearbox, g_cl_wpass_v;
static uint64_t g_cl_ticks;

#define CL_MAXV 256                     /* the lists index with a byte */
#define CL_HALF (192 * 1024)
static uint8_t *g_cl_mem;
static uint32_t g_cl_used, g_cl_frame;

/* arrays for n vertices of cut polygons (positions of 16 bytes, UVs and colours of 4), made in one half by the
 * frame's parity: the GPU may still draw the last frame while this one is made */
static int cl_alloc(uint32_t n, float **pos, uint32_t **uv, uint32_t **col)
{
    const uint32_t pb = (n * 16 + 31) & ~31u, ub = (n * 4 + 31) & ~31u, need = pb + 2 * ub;
    uint8_t *p;
    if (!g_cl_mem) {
        static int tried;
        if (tried)
            return 0;
        tried = 1;
        g_cl_mem = plat_alloc(2 * CL_HALF);
        if (!g_cl_mem || ((uintptr_t)g_cl_mem & 31)) {
            g_cl_mem = NULL;
            LOG("gx: no memory for the cut polygons' arrays: the world is not cut");
            return 0;
        }
    }
    if (g_cl_frame != g_frame_no) {
        g_cl_frame = g_frame_no;
        g_cl_used = 0;
    }
    if (g_cl_used + need > CL_HALF) {
        g_cl_nospace++;
        return 0;
    }
    p = g_cl_mem + (g_frame_no & 1) * CL_HALF + g_cl_used;
    g_cl_used += need;
    *pos = (float *)p;
    *uv = (uint32_t *)(p + pb);
    *col = (uint32_t *)(p + pb + ub);
    return 1;
}

static inline uint32_t cl_byte(float c)
{
    return c <= 0.0f ? 0u : c >= 255.0f ? 255u : (uint32_t)(c + 0.5f);
}

static inline uint32_t cl_half(float v)
{
    const int i = (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
    return (uint32_t)(uint16_t)(i < -32768 ? -32768 : i > 32767 ? 32767 : i);
}

/* a vertex's colour as the list sends it (dl_vertex): the four bytes the GPU is given */
static uint32_t cl_vertex_color(const NwBlock *b, const uint32_t *col, uint32_t k)
{
    if (col)
        return col[k];
    if (b->rgb) {
        const uint32_t a = b->col + 3 * k;
        return (uint32_t)LB(a) << 24 | (uint32_t)LB(a + 1) << 16 | (uint32_t)LB(a + 2) << 8;
    } else {
        const uint32_t c = LW(b->col + 4 * k);
        return (c & 0xFF) << 24 | ((c >> 8) & 0xFF) << 16 | ((c >> 16) & 0xFF) << 8 | a2(c >> 24);
    }
}

/* X, Y and W of vertex v (the camera's three rows, W the third's negative) */
static inline void cl_xyw(const float *cm, const NwBlock *b, uint32_t v, float *X, float *Y, float *W)
{
    const float x = LWF(b->pos + 16 * v), y = LWF(b->pos + 16 * v + 4), z = LWF(b->pos + 16 * v + 8);
    *W = -(cm[8] * x + cm[9] * y + cm[10] * z + cm[11]);
    *X = cm[0] * x + cm[1] * y + cm[2] * z + cm[3];
    *Y = cm[4] * x + cm[5] * y + cm[6] * z + cm[7];
}

/* W alone: the depth scan needs no X or Y (three loads and three multiplies a vertex, not nine) */
static inline void cl_w(const float *cm, const NwBlock *b, uint32_t v, float *W)
{
    const float x = LWF(b->pos + 16 * v), y = LWF(b->pos + 16 * v + 4), z = LWF(b->pos + 16 * v + 8);
    *W = -(cm[8] * x + cm[9] * y + cm[10] * z + cm[11]);
}

/* does triangle i (vertices i-2 to i, all in front of the near plane) have a vertex in the band and not all of them
 * outside one side of the screen: a triangle to cut */
static inline __attribute__((always_inline)) int cl_tri_bad(const float *cm, const NwBlock *b, uint32_t i,
                                                           const ClipRect *r, float bx0, float bx1, float by0, float by1)
{
    uint32_t k, bad = 0, allside = 0x1E;
    for (k = i - 2; k <= i; k++) {
        float xx, yy, w;
        cl_xyw(cm, b, k, &xx, &yy, &w);
        allside &= (xx < r->hx0 * w ? 2u : 0u) | (xx > r->hx1 * w ? 4u : 0u) | (yy < r->hy0 * w ? 8u : 0u) | (yy > r->hy1 * w ? 16u : 0u);
        if (xx < bx0 * w || xx > bx1 * w || yy < by0 * w || yy > by1 * w)
            bad = 1;
    }
    return bad && !allside;
}

/* a triangle of clip_batch looked at once: one drawn as the list has it and far enough outside is cut. nnear: the
 * classes were built (every triangle not drawn as the list has it is out of cls); else the ADC bit says it (from the
 * scan's code, or from memory in a batch the box alone let through: then there was no scan and wpass is 0) */
#define CL_LOOK(i_)                                                                                                        \
    do {                                                                                                                   \
        const uint32_t ii_ = (i_);                                                                                         \
        if (ii_ >= 2 && ii_ < n && !done[ii_] && !cutm[ii_] &&                                                             \
            (nnear ? cls[ii_] == NORMAL                                                                                    \
                   : wpass ? !(code[ii_] & C_ADC) : !(LW(b->pos + 16 * ii_ + 12) & 0x8000))) {                             \
            done[ii_] = 1;                                                                                                 \
            g_cl_big_t++;                                                                                                  \
            if (cl_tri_bad(cm, b, ii_, &r, bx0, bx1, by0, by1)) {                                                          \
                cutm[ii_] = 1;                                                                                             \
                cls[ii_] = 0;      /* (out of the strips; the lazy build below sets it again when !nnear) */               \
                ncut++;                                                                                                    \
            }                                                                                                              \
        }                                                                                                                  \
    } while (0)

/* The picture a batch is drawn into, for what its last look found (DList.cl_has): the big target (the screen) is 0 and each
 * of the small ones (the reflection views) has a number of its own. A batch with triangles to cut in one picture (a view near
 * the road: the road's are huge there) and none in another is the same list: one flag for both was set and cleared by
 * whichever drew it last, and a view whose look was found empty by the screen's draw was only looked at every few frames,
 * the others drawn with their huge triangles uncut. */
static uint32_t g_clip_fbp[7];

static unsigned clip_ctx(const RT *rt)
{
    unsigned i;
    if (rt->big)
        return 0;
    for (i = 0; i < 7 && g_clip_fbp[i]; i++)
        if (g_clip_fbp[i] == rt->fbp + 1)
            return i + 1;
    if (i == 7)
        return 7;
    g_clip_fbp[i] = rt->fbp + 1;
    return i + 1;
}

/* Draws the batch cut to the screen, and returns 1, when a triangle of it needs it; else 0 (the caller draws
 * the list). col: the lit list's colour array, or NULL (the colours inline). Called with the camera, the
 * projection, the cull and the arrays set as for the list. */
static int __attribute__((noinline)) clip_batch(const RT *rt, const NwBlock *b, const DList *dl, const uint32_t *vm, const uint32_t *col)
{
    /* the codes of a vertex: behind the near plane, and not drawn (the strip's ADC bit) */
    enum { C_NEAR = 1, C_ADC = 2 };
    enum { NORMAL = 1, DROP };
    static float W[CL_MAXV];
    static uint8_t code[CL_MAXV], cls[CL_MAXV], done[CL_MAXV], cutm[CL_MAXV];
    static float cam_seen[12], cam_rx, cam_ry, cam_rw;
    const uint32_t n = b->n, pos_skip = (b->pos & 31) / 16, uv_skip = (b->uv & 31) / 4;
    const float *cm = g_nst.cam;
    ClipRect r;
    float bx0, bx1, by0, by1, bw, bh, thr2 = 0.0f, wlim = 0.0f, kreach;
    uint32_t i, ncut = 0, ndrop = 0, nverts = 0, nnear = 0;
    int wpass = 0;
    uint64_t t0;
    const unsigned cbit = 1u << clip_ctx(rt);
    if (!g_clip || !(vm[1013 * 4] & 0xFFFF) || dl->uv32 || n < 3 || n + 2 >= CL_MAXV || !g_nst.cam_valid ||
        !g_nst.proj_valid || !g_gxc.scis_valid || (!col && !b->col))
        return 0;
    /* a batch that had nothing to cut a moment ago is looked at again every g_clip_every frames: what makes a triangle
     * big on the screen builds up over many, and one that is missed for a few is only drawn as it was */
    if (!(dl->cl_has & cbit) && g_clip_every > 1 && (g_frame_no + (uint32_t)(dl - g_dls)) % (uint32_t)g_clip_every)
        return 0;
    t0 = gettime();
    g_cl_looked++;
    {
        /* the screen's rectangle in X/W and Y/W: the GX viewport maps X/W -1..1 to vx..vx + vw (Y/W the same
         * way, down), and the scissor is where the picture is, two pixels more around it */
        const float vx = g_nst.proj[2], vy = g_nst.proj[3], vw = g_nst.proj[4], vh = g_nst.proj[5];
        const float sx0 = (float)g_gxc.scis[0] - 2.0f, sy0 = (float)g_gxc.scis[1] - 2.0f;
        const float sx1 = (float)(g_gxc.scis[0] + g_gxc.scis[2]) + 2.0f, sy1 = (float)(g_gxc.scis[1] + g_gxc.scis[3]) + 2.0f;
        const float k0 = g_nst.proj[0], k1 = g_nst.proj[1], band = (float)g_clip_band * 0.01f;
        if (vw < 1.0f || vh < 1.0f)
            goto none;
        r.hx0 = 2.0f * (sx0 - vx) / vw - 1.0f;
        r.hx1 = 2.0f * (sx1 - vx) / vw - 1.0f;
        r.hy0 = 2.0f * (sy0 - vy) / vh - 1.0f;
        r.hy1 = 2.0f * (sy1 - vy) / vh - 1.0f;
        /* GX's own near plane (where its depth leaves its range) is as near as the cut goes */
        r.eps = k0 > 1e-6f && k1 > 0.0f ? 1.02f * k1 / k0 : 0.0f;
        if (r.eps < 0.01f)
            r.eps = 0.01f;
        bw = (r.hx1 - r.hx0) * band;
        bh = (r.hy1 - r.hy0) * band;
        bx0 = r.hx0 - bw;
        bx1 = r.hx1 + bw;
        by0 = r.hy0 - bh;
        by1 = r.hy1 + bh;
    }
    {
        /* the batch's box: its corners' depths (W is affine: a corner has the nearest and the farthest) */
        float wmax = -1e30f, wbox = 1e30f;
        int c;
        for (c = 0; c < 8; c++) {
            const float x = c & 1 ? dl->bmax[0] : dl->bmin[0], y = c & 2 ? dl->bmax[1] : dl->bmin[1],
                        z = c & 4 ? dl->bmax[2] : dl->bmin[2];
            const float w = -(cm[8] * x + cm[9] * y + cm[10] * z + cm[11]);
            if (w < wbox)
                wbox = w;
            if (w > wmax)
                wmax = w;
        }
        if (wmax < r.eps)
            goto none;                          /* wholly behind the eye: the GPU rejects it */
        if (memcmp(cam_seen, cm, sizeof cam_seen)) {
            memcpy(cam_seen, cm, sizeof cam_seen);
            cam_rx = sqrtf(cm[0] * cm[0] + cm[1] * cm[1] + cm[2] * cm[2]);
            cam_ry = sqrtf(cm[4] * cm[4] + cm[5] * cm[5] + cm[6] * cm[6]);
            cam_rw = sqrtf(cm[8] * cm[8] + cm[9] * cm[9] + cm[10] * cm[10]);
        }
        /* A triangle reaches from the screen into the band only if its longest edge is long against its depth
         * (X/W changes by at most (|dX| + |X/W| |dW|) / W over a step: the rows' lengths and an X/W of at most 2
         * make the bound): needs a depth of at most wlim for the longest edge of the triangles the list's table
         * (its 24 biggest) leaves out, and of at most edge * k / band for any other */
        kreach = ((cam_rx > cam_ry ? cam_rx : cam_ry) + 2.0f * cam_rw) / (bw < bh ? bw : bh);
        if (dl->nbig == CL_BIG) {
            wlim = sqrtf(dl->big_e2[CL_BIG - 1]) * kreach;
            if (wlim > 8.0f)
                wlim = 8.0f;                /* (beyond that only the table's: the near field is looked at in full) */
        }
        if (wbox >= r.eps && wbox >= wlim) {
            /* nothing of the other triangles can: the table's are all there is */
            thr2 = wbox / kreach;
            thr2 *= thr2;
            if (!dl->nbig || dl->big_e2[0] < thr2)
                goto none;                      /* no triangle of it reaches from the screen into the band */
        } else {
            /* the depths of the vertices: which triangles cross the near plane, and which come near enough */
            float wf = 1e30f;
            if (wbox < r.eps)
                g_cl_nearbox++;
            g_cl_wpass_v += n;
            for (i = 0; i < n; i++) {
                float w;
                if (!(i & 1) && i + 8 < n)
                    __builtin_prefetch(GPTR(b->pos + 16 * (i + 8)), 0, 0);   /* four lines on, each line (two vertices) once */
                cl_w(cm, b, i, &w);
                W[i] = w;
                code[i] = LW(b->pos + 16 * i + 12) & 0x8000 ? C_ADC : 0;
                if (w < r.eps) {
                    code[i] |= C_NEAR;
                    nnear++;
                } else if (w < wf) {
                    wf = w;
                }
            }
            if (nnear == n)
                goto none;
            wpass = 1;
            thr2 = wf / kreach;
            thr2 *= thr2;
        }
    }
    g_cl_tested++;
    memset(done, 0, n);
    memset(cutm, 0, n);
    {
        /* The classes, when a vertex is behind the eye: the triangles that stay as the list has them, the ones with
         * a vertex behind the eye and one in front cut, those all behind it left out. Without one (most of the
         * frame) nothing needs them until something is cut. Then the candidates looked at: the table's big ones,
         * and in a batch that comes near, every triangle with a vertex nearer than wlim. */
        uint32_t t;
        if (nnear) {
            for (i = 2; i < n; i++) {
                if (code[i] & C_ADC) {
                    cls[i] = 0;
                } else {
                    const uint32_t m = (uint32_t)((code[i - 2] & C_NEAR) + (code[i - 1] & C_NEAR) + (code[i] & C_NEAR));
                    if (m == 0) {
                        cls[i] = NORMAL;
                    } else if (m == 3) {
                        cls[i] = DROP;
                        ndrop++;
                    } else {
                        cls[i] = 0;
                        cutm[i] = 1;
                        ncut++;
                    }
                }
            }
        }
        g_cl_few += !wpass;
        for (t = 0; t < dl->nbig && dl->big_e2[t] >= thr2; t++)
            CL_LOOK(dl->big_i[t]);
        if (wpass && wlim > 0.0f) {
            const float lim = wlim > r.eps ? wlim : r.eps;
            for (i = 0; i < n; i++)
                if (!(code[i] & C_NEAR) && W[i] < lim) {
                    uint32_t j;
                    for (j = i < 2 ? 2 : i; j <= i + 2 && j < n; j++)
                        CL_LOOK(j);
                }
        }
        if (!ncut)
            goto none;
        if (!nnear) {
            /* no vertex is behind the eye: the classes were not built (nothing needed them until now). The strips
             * need them, and the triangles the look found are cut; the ADC bit says which others are not drawn. */
            for (i = 2; i < n; i++) {
                if (cutm[i])
                    cls[i] = 0;
                else if (wpass ? !(code[i] & C_ADC) : !(LW(b->pos + 16 * i + 12) & 0x8000))
                    cls[i] = NORMAL;
                else
                    cls[i] = 0;
            }
        }
    }
    {
        /* the cut triangles first: their polygons (one that is outside the screen has none), made into arrays of
         * this function's; if none is left there is nothing to draw but what the list draws */
        static float pp[CL_MAXV][4];
        static uint32_t pu[CL_MAXV], pc[CL_MAXV];
        uint32_t used = 0, nfan = 0, fan_n[CL_MAXV / 3 + 1], fan_at[CL_MAXV / 3 + 1], chunks = 0;
        int strips_done = 0;
        for (i = 2; i < n + 1; i++) {
            ClipV tv[3], poly[CLIP_MAX_OUT];
            uint32_t idx[3], k, np = 0, m;
            if (i < n && cutm[i]) {
                /* the winding of the strip: the triangle ending at i is (i-2, i-1, i) from an even i-2 on, else
                 * (i-1, i-2, i) */
                idx[0] = (i - 2) & 1 ? i - 1 : i - 2;
                idx[1] = (i - 2) & 1 ? i - 2 : i - 1;
                idx[2] = i;
                for (k = 0; k < 3; k++) {
                    const uint32_t v = idx[k], cc = cl_vertex_color(b, col, v), uw = b->nouv ? 0 : LW(b->uv + 4 * v);
                    ClipV *o = &tv[k];
                    o->p[0] = LWF(b->pos + 16 * v);
                    o->p[1] = LWF(b->pos + 16 * v + 4);
                    o->p[2] = LWF(b->pos + 16 * v + 8);
                    cl_xyw(cm, b, v, &o->X, &o->Y, &o->W);
                    o->u = (float)(int16_t)(uw >> 16);
                    o->v = (float)(int16_t)(uw & 0xFFFF);
                    o->c[0] = (float)(cc >> 24);
                    o->c[1] = (float)((cc >> 16) & 0xFF);
                    o->c[2] = (float)((cc >> 8) & 0xFF);
                    o->c[3] = (float)(cc & 0xFF);
                }
                np = (uint32_t)clip_poly(&r, tv, poly);
                if (np < 3) {
                    ndrop++;
                    np = 0;
                }
            }
            /* the chunk is full (or the triangles are done): the strips (once, before the first fans), then
             * what the chunk holds */
            if ((i >= n || used + np > CL_MAXV - 2) && used) {
                float *pos;
                uint32_t *uvs, *cols, f;
                if (!strips_done) {
                    /* the strips of the triangles that stay, as the list has them (dl_get) */
                    uint32_t j;
                    strips_done = 1;
                    for (j = 2; j < n;) {
                        uint32_t first, last, dup, q;
                        if (cls[j] != NORMAL) {
                            j++;
                            continue;
                        }
                        first = j - 2;
                        while (j < n && cls[j] == NORMAL)
                            j++;
                        last = j - 1;
                        dup = first & 1;
                        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT1, (u16)(last - first + 1 + dup));
                        if (dup)
                            dl_vertex(b, first, pos_skip, uv_skip, col != NULL);
                        for (q = first; q <= last; q++)
                            dl_vertex(b, q, pos_skip, uv_skip, col != NULL);
                        GX_End();
                    }
                }
                if (cl_alloc(used, &pos, &uvs, &cols)) {
                    memcpy(pos, pp, used * 16);
                    memcpy(uvs, pu, used * 4);
                    memcpy(cols, pc, used * 4);
                    DCFlushRange(pos, used * 16);
                    DCFlushRange(uvs, used * 4);
                    DCFlushRange(cols, used * 4);
                    GX_SetArray(GX_VA_POS, pos, 16);
                    GX_SetArray(GX_VA_TEX0, uvs, 4);
                    if (col)
                        GX_SetArray(GX_VA_CLR0, cols, 4);
                    for (f = 0; f < nfan; f++) {
                        GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT1, (u16)fan_n[f]);
                        for (m = 0; m < fan_n[f]; m++) {
                            const uint32_t q = fan_at[f] + m;
                            GX_Position1x8((u8)q);
                            if (col)
                                GX_Color1x8((u8)q);
                            else
                                GX_Color1u32(pc[q]);
                            GX_TexCoord1x8((u8)q);
                        }
                        GX_End();
                    }
                }
                chunks++;
                used = 0;
                nfan = 0;
            }
            if (!np)
                continue;
            for (m = 0; m < np; m++) {
                const ClipV *q = &poly[m];
                pp[used + m][0] = q->p[0];
                pp[used + m][1] = q->p[1];
                pp[used + m][2] = q->p[2];
                pp[used + m][3] = 0.0f;
                pu[used + m] = cl_half(q->u) << 16 | cl_half(q->v);
                pc[used + m] = cl_byte(q->c[0]) << 24 | cl_byte(q->c[1]) << 16 | cl_byte(q->c[2]) << 8 | cl_byte(q->c[3]);
            }
            fan_n[nfan] = np;
            fan_at[nfan++] = used;
            used += np;
            nverts += np;
        }
        if (!chunks)
            goto none;                      /* every cut triangle was outside: the list draws what is left, as before */
    }
    g_cl_cut++;
    g_cl_tris += ncut;
    g_cl_dropped += ndrop;
    g_cl_verts += nverts;
    g_cl_ticks += gettime() - t0;
    g_native_draws++;
    ((DList *)dl)->cl_has |= cbit;
    return 1;
none:
    g_cl_ticks += gettime() - t0;
    ((DList *)dl)->cl_has &= (uint8_t)~cbit;
    return 0;
}

static int lit_draw_dl(const NwBlock *b, const RT *rt, const GsDraw *d)
{
    const uint32_t *vm = g_vu_mem + 0xC000 / 4;
    uint32_t *col;
    DList *dl;
    int prev;
    if (!b->lit || b->nlit < b->n)
        return 0;
    dl = dl_get(b, 1);
    if (!dl || dl->uv32)
        return 0;
    g_lit_lists++;
    prev = pt_enter(PT_NAT);
    col = lit_kept(b, dl, vm);
    pt_leave(prev);
    if (!col) {
        col = litcol_alloc(b->n);
        if (!col)
            return 0;
        prev = pt_enter(PT_NAT);
        NAT_TIME(4, nw_lit_rgba(vm, b, col));
        pt_leave(prev);
        DCFlushRange(col, b->n * 4);
    }
    native_mode(NW_LIT_DL);
    native_projection(rt, d, 1.0f);
    native_cull(vm, behind_camera(dl->bmin, dl->bmax));
    native_scroll(vm);
    native_camera();
    dl_touch(dl, dl->n * 4);
    GX_SetArray(GX_VA_POS, GPTR(dl->pos), 16);
    GX_SetArray(GX_VA_TEX0, GPTR(dl->uv), 4);
    GX_SetArray(GX_VA_CLR0, col, 4);
    if (clip_batch(rt, b, dl, vm, col))
        return 1;
    GX_CallDispList(dl->data, dl->size);
    g_native_draws++;
    return 1;
}

static void lit_draw(const NwBlock *b, const RT *rt, const GsDraw *d)
{
    const uint32_t *vm = g_vu_mem + 0xC000 / 4;
    static float pos[NC_MAX + 4][3];
    static int8_t lit[NC_MAX + 4][4];
    static uint8_t alpha[NC_MAX + 4];
    uint32_t k, c, i, pos_skip = (b->pos & 31) / 16, uv_skip = (b->uv & 31) / 4, pa = b->pos & ~31u, ua = b->uv & ~31u;
    for (k = 0; k < b->n; k++)
        for (c = 0; c < 3; c++)
            pos[k][c] = LWF(b->pos + 16 * k + 4 * c);
    for (k = 0; k < ((b->n + 3) & ~3u); k++) {
        uint32_t w = LW(b->lit + 4 * k);
        for (c = 0; c < 4; c++)
            lit[k][c] = (int8_t)(w >> (8 * c));
    }
    {
        int prev = pt_enter(PT_NAT);
        nw_lit_alpha(vm, b->n, (const float (*)[3])pos, (const int8_t (*)[4])lit, alpha);
        pt_leave(prev);
    }
    native_mode(NW_LIT);
    native_projection(rt, d, 1.0f);
    {
        float mn[3], mx[3];
        for (c = 0; c < 3; c++) {
            mn[c] = mx[c] = pos[0][c];
            for (k = 1; k < b->n; k++) {
                if (pos[k][c] < mn[c]) mn[c] = pos[k][c];
                if (pos[k][c] > mx[c]) mx[c] = pos[k][c];
            }
        }
        native_cull(vm, behind_camera(mn, mx));
    }
    native_scroll(vm);
    native_camera();
    DCFlushRange(GPTR(pa), b->n * 16 + 32);
    DCFlushRange(GPTR(ua), b->n * 4 + 32);
    GX_SetArray(GX_VA_POS, GPTR(pa), 16);
    GX_SetArray(GX_VA_TEX0, GPTR(ua), 4);
    /* PS2 strips as in dl_get: runs of drawn triangles, parity kept */
    for (i = 2; i < b->n;) {
        uint32_t first, last, dup;
        if (LW(b->pos + 16 * i + 12) & 0x8000) {
            i++;
            continue;
        }
        first = i - 2;
        while (i < b->n && !(LW(b->pos + 16 * i + 12) & 0x8000))
            i++;
        last = i - 1;
        dup = first & 1;
        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT1, (u16)(last - first + 1 + dup));
        for (k = first - dup; k != last + 1; k++) {
            uint32_t v = k + dup > first ? k : first, ca = b->col + 3 * v;    /* the first vertex twice with dup */
            GX_Position1x8((u8)(v + pos_skip));
            GX_Color1u32((uint32_t)LB(ca) << 24 | (uint32_t)LB(ca + 1) << 16 | (uint32_t)LB(ca + 2) << 8 | a2(alpha[v]));
            GX_TexCoord1x8((u8)(v + uv_skip));
        }
        GX_End();
    }
    g_native_draws++;
}

/* payload: render data, clip mode, cull mode (VU1 1013.z), microprogram
 * (from hle_world_batch; world batches now come through the VIF instead) */
void gs_backend_native(const uint32_t *pl, int n)
{
    NwBlock b;
    if (n >= 4 && nw_block_parse(pl[0], &b))
        gs_backend_world(&b, nw_kind(pl[3]));
}

/* Lit instanced props (471FA0 with 932.y set): per instance i (records of
 * five qwords at 934 + 5i: matrix rows in x, y, w; 934.z ambient, 935.z
 * diffuse; 938 the light direction and alpha) each vertex's grey is ambient
 * + diffuse * (n . light), floats that hold the byte in their low mantissa
 * bits (biased by 2^23), as the GS takes the words */
static void inst_lit_rgba(const NwBlock *b, const float *rec, uint32_t *col)
{
    const float *nrm = (const float *)GPTR(b->nrm);
    const float amb = rec[2], dif = rec[4 + 2], lx = rec[16], ly = rec[17], lz = rec[18];
    const uint32_t a = a2(F2BITS(rec[19] + 0.0f) & 0xFF);
    uint32_t k;
    for (k = 0; k < b->n; k++) {
        const float *g = nrm + (k & ~3u) * 4 + (k & 3);
        float ndl = g[0] * lx;
        float c;
        uint32_t grey;
        ndl = ndl + g[4] * ly;
        ndl = ndl + g[8] * lz;
        c = amb + 0.0f;
        c = c + dif * ndl;
        grey = F2BITS(c) & 0xFF;
        col[k] = grey << 24 | grey << 16 | grey << 8 | a;
    }
}

/* An instance's colours are a function of its list's normals (in the render data, with the positions) and six words
 * of its record (ambient, diffuse, the light direction, alpha); the props stand still and so does the light, and the
 * same six words come back frame after frame. KC_N arrays of up to KC_MAXV colours (MEM1, at the first use), each
 * found by its list and the six words, two places for each; one is written again only when the GPU is done with it
 * (three frames undrawn), else the colours are made for this frame alone. The first 3000 taken are checked against
 * the sums (gxflags2 1024: all of them; 65536: none kept, as for the lit world). Autopilot "opt3 0": none kept. */
#define KC_N 128
#define KC_MAXV 224
typedef struct KeptCol {
    uint32_t block, check, nrm, n, frame;
    uint32_t w[6];
} KeptCol;
static KeptCol g_kc[KC_N];
static uint32_t *g_kc_col;
/* status line: instances drawn, those with their colours kept, colours written for keeping, none free to write; and
 * (the check) kept colours that were not what the sums give */
static uint32_t g_kc_inst, g_kc_hits, g_kc_written, g_kc_busy, g_kc_bad;

static uint32_t *inst_kept(const DList *dl, const NwBlock *b, const uint32_t *rw)
{
    uint32_t h, way;
    KeptCol *s, *e;
    if (!g_opt3 || (g_gx_flags2 & 65536) || b->n > KC_MAXV)
        return NULL;
    if (PS2_UNLIKELY(!g_kc_col)) {
        static int tried;
        if (tried)
            return NULL;
        tried = 1;
        g_kc_col = memalign(32, KC_N * KC_MAXV * 4);
        if (!g_kc_col) {
            LOG("gx: no memory for the lit props' kept colours");
            return NULL;
        }
    }
    h = mix(mix(mix(mix(mix(mix(mix(dl->block, b->nrm), rw[2]), rw[6]), rw[16]), rw[17]), rw[18]), rw[19]);
    s = &g_kc[(h % (KC_N / 2)) * 2];
    for (way = 0; way < 2; way++) {
        e = &s[way];
        if (e->block == dl->block && e->check == dl->check && e->nrm == b->nrm && e->n == b->n && e->w[0] == rw[2] &&
            e->w[1] == rw[6] && e->w[2] == rw[16] && e->w[3] == rw[17] && e->w[4] == rw[18] && e->w[5] == rw[19]) {
            uint32_t *col = g_kc_col + (uint32_t)(e - g_kc) * KC_MAXV;
            static uint32_t checked;
            g_kc_hits++;
            if (checked < 3000 || (g_gx_flags2 & 1024)) {
                static uint32_t tmp[KC_MAXV];
                checked++;
                inst_lit_rgba(b, (const float *)rw, tmp);
                if (memcmp(tmp, col, b->n * 4) && g_kc_bad++ < 8)
                    LOG("gx: lit props' list %08X (%u vertices): kept colours differ from the sums", dl->block, b->n);
            }
            e->frame = g_frame_no;
            return col;
        }
    }
    /* the way drawn longer ago, if the GPU is done with it */
    e = g_frame_no - s[0].frame >= g_frame_no - s[1].frame ? &s[0] : &s[1];
    if (e->n && g_frame_no - e->frame < 3) {
        g_kc_busy++;
        return NULL;
    }
    {
        uint32_t *col = g_kc_col + (uint32_t)(e - g_kc) * KC_MAXV;
        e->block = dl->block;
        e->check = dl->check;
        e->nrm = b->nrm;
        e->n = b->n;
        e->w[0] = rw[2];
        e->w[1] = rw[6];
        e->w[2] = rw[16];
        e->w[3] = rw[17];
        e->w[4] = rw[18];
        e->w[5] = rw[19];
        e->frame = g_frame_no;
        inst_lit_rgba(b, (const float *)rw, col);
        DCFlushRange(col, b->n * 4);
        g_kc_written++;
        return col;
    }
}

static int inst_lit_draw(const NwBlock *b, const RT *rt, const GsDraw *d, const uint32_t *vm)
{
    DList *dl = dl_get(b, 1);
    uint32_t i, count;
    if (!dl || !dl->uv32)
        return 0;
    native_mode(NW_INST_LIT);
    native_projection(rt, d, 1.0f);
    set_cull(GX_CULL_NONE);
    dl_touch(dl, dl->n * 8);
    GX_SetArray(GX_VA_POS, GPTR(dl->pos), 16);
    GX_SetArray(GX_VA_TEX0, GPTR(dl->uv), 8);
    count = (vm[932 * 4] & 0xFFFF) + 1;
    if (count > 20)
        count = 20;
    for (i = 0; i < count; i++) {
        const uint32_t *rw = &vm[(934 + 5 * i) * 4];
        uint32_t *col = inst_kept(dl, b, rw);
        g_kc_inst++;
        if (!col) {
            col = litcol_alloc(b->n);
            if (!col)
                return 0;
            inst_lit_rgba(b, (const float *)rw, col);
            DCFlushRange(col, b->n * 4);
        }
        GX_SetArray(GX_VA_CLR0, col, 4);
        native_instance(i);
        GX_CallDispList(dl->data, dl->size);
        g_native_draws++;
    }
    return 1;
}

/* A world batch: render data in guest memory, constants in VU1 memory (the
 * camera at 1014.., PRIM in the GIF tag at 1006, cull mode 1013.z, scrolling
 * UVs 1005, instances at 932..). */
extern int g_wstat;
/* "wstat 3": a hash of what every world batch is drawn with, kept for each frame and logged for 48 frames; the same
 * frames of two builds that draw the world alike have the same lines (the paced 30 a second run, whose frames are
 * the same ones in both) */
static struct { uint32_t frame, hash, n; } g_wh_ring[48];
static uint32_t g_wh_hash, g_wh_n, g_wh_frame, g_wh_count, g_wh_done, g_wh_origin, g_wb_f0;
static uint32_t g_wsk_n[3], g_wsk_v[3];     /* "wskip" 2: checked runs of the three pieces of work, and those that wrote to the FIFO */

static void __attribute__((noinline)) wh_bench(void)
{
        /* "wstat 5": the work of 300 ticks of the game's clock (0x4E273C, a tick an update pass: the race's own progress)
         * from the 600th on, in the game's own interval (pace 0): each frame's time less what the kernel spent
         * waiting, as percentiles over the frames, and the median of 12 stretches of 25 ticks (a wait for the disc
         * spoils a stretch, not the median). From tick 600 the frames are the same ones in every run (the hash of what
         * the world batches are drawn with, "wstat 3"); before it, the intro's camera is not: 220 to 630 batches at
         * tick 407 in three runs of one build, which made the percentiles of 150 to 550 differ by a fifth. */
        extern uint64_t g_kernel_idle_us;
        static uint32_t ft[400], c0, nf;
        static uint64_t t_prev, idle_prev, seg_t[17], seg_idle[17];
        static int started, done;
        const uint32_t clock = LW(0x004E273Cu);
        const uint64_t now = gettime(), idle = g_kernel_idle_us;
        if (!started && clock >= 600 && clock < 700) {
            started = 1;
            c0 = clock;
            seg_t[0] = now;
            seg_idle[0] = idle;
#ifdef MEMOSTAT
            ms_reset();
#endif
#ifndef NO_TMEMO2
            tm2_reset();
#endif
        } else if (started && !done) {
            const uint64_t us = ticks_to_microsecs(now - t_prev), id = idle - idle_prev;
            extern int g_dbg;
            if (nf < 24 || (nf % 50) == 0 || (g_dbg == 77 && nf >= 100 && nf < 160))
                LOG("benchframe %u: clock %u, world draws %u hash %08X, took %u us, repeated work checked %u/%u/%u wrote %u/%u/%u",
                    nf, clock, g_wh_n, g_wh_hash, (uint32_t)(us > id ? us - id : 1), g_wsk_n[0], g_wsk_n[1], g_wsk_n[2],
                    g_wsk_v[0], g_wsk_v[1], g_wsk_v[2]);
            if (nf < 400)
                ft[nf++] = (uint32_t)(us > id ? us - id : 1);
            if (clock >= c0 + 25 * (uint32_t)(nf / 25 + 1) && 0)
                ;
            if (clock >= c0 + 300) {
                uint32_t k, j, st[12], tot = 0;
                uint64_t sum = 0;
                done = 1;
                /* stretches of 25 frames: the frames are a tick each in this interval */
                for (k = 0; k < 12; k++) {
                    uint32_t sacc = 0;
                    for (j = 0; j < 25 && k * 25 + j < nf; j++)
                        sacc += ft[k * 25 + j];
                    st[k] = sacc / 25;
                    tot += st[k];
                }
                for (k = 1; k < 12; k++) {
                    const uint32_t v = st[k];
                    for (j = k; j > 0 && st[j - 1] > v; j--)
                        st[j] = st[j - 1];
                    st[j] = v;
                }
                for (k = 0; k < nf; k++)
                    sum += ft[k];
                for (k = 1; k < nf; k++) {
                    const uint32_t v = ft[k];
                    for (j = k; j > 0 && ft[j - 1] > v; j--)
                        ft[j] = ft[j - 1];
                    ft[j] = v;
                }
                #define PCT(q) ft[(nf - 1) * (q) / 100]
                {
                    /* the mean of the frames below the 98th percentile: a hitch (the disc) does not move it */
                    uint64_t tsum = 0;
                    const uint32_t nt = nf * 98 / 100;
                    for (k = 0; k < nt; k++)
                        tsum += ft[k];
                    LOG("bench: clock %u to %u, %u frames; work a frame (us): p5 %u p25 %u p50 %u p75 %u p90 %u p95 %u p99 %u max %u "
                        "mean %u, mean of the lower 98%% %u; stretches of 25: median %u", c0, clock, nf, PCT(5), PCT(25), PCT(50),
                        PCT(75), PCT(90), PCT(95), PCT(99), ft[nf - 1], (uint32_t)(sum / (nf ? nf : 1)),
                        (uint32_t)(tsum / (nt ? nt : 1)), (st[5] + st[6]) / 2);
                }
                #undef PCT
#ifdef MEMOSTAT
                ms_report(nf);
#endif
#ifndef NO_TMEMO2
                tm2_report(nf);
#endif
                {
                    void plat_bench_done(void);
                    plat_bench_done();          /* autopilot "benchprof": the profile after it */
                }
            }
        }
        t_prev = now;
        idle_prev = idle;
    }

/* the frames are counted from the first with 300 world batches (a race's first): the same frames of a race in any
 * two runs, whatever the host did meanwhile (the game runs a pass a frame in the game's own interval, "pace 0") */
static void __attribute__((noinline)) wh_frame_end(void)
{
    if (!g_wh_origin && g_wh_n >= 300)
        g_wh_origin = g_wh_frame;
    if (g_wh_origin && g_wh_count < 48) {
        g_wh_ring[g_wh_count].frame = g_wh_frame - g_wh_origin;
        g_wh_ring[g_wh_count].hash = g_wh_hash;
        g_wh_ring[g_wh_count++].n = g_wh_n;
    }
    if (g_wstat == 5)
        wh_bench();
    if (g_wstat == 3 && g_wh_count == 48 && !g_wh_done) {
        uint32_t k;
        g_wh_done = 1;
        for (k = 0; k < 48; k++)
            LOG("wh: frame +%u hash %08X draws %u", g_wh_ring[k].frame, g_wh_ring[k].hash, g_wh_ring[k].n);
    }
    g_wh_hash = 0x811C9DC5u;
    g_wh_n = 0;
}

static void __attribute__((noinline)) wh_batch(const NwBlock *b, int kind, uint32_t prim, const RT *rt, const uint32_t *vm)
{
    extern uint32_t g_vu1_code;
    uint32_t h, k;
    if (g_wh_frame != g_frame_no) {
        if (g_wh_frame)
            wh_frame_end();
        g_wh_frame = g_frame_no;
    }
    g_wh_n++;
    if (g_wstat != 3)
        return;                         /* the benchmark ("wstat 5") counts the batches and finds the frames' ends */
    h = g_wh_hash;
#define WH(x) h = (h ^ (uint32_t)(x)) * 0x01000193u
    WH(b->pos); WH(b->n); WH(b->col); WH(b->uv); WH(b->lit); WH(kind); WH(prim); WH(rt->fbp); WH(g_vu1_code);
    for (k = 1005; k < 1024; k++) { WH(vm[k * 4]); WH(vm[k * 4 + 1]); WH(vm[k * 4 + 2]); WH(vm[k * 4 + 3]); }
#undef WH
    g_wh_hash = h;
}

uint64_t g_wst_stage[8];
#define WST(k) do { if (g_wstat == 1) { const uint64_t n_ = gettime(); g_wst_stage[k] += n_ - wst_t; wst_t = n_; } } while (0)

/* gxflags2 8388608: how much clipping the world would need. The PS2's world
 * microprograms clip a triangle that crosses the near plane and flip its
 * winding, which GX does not (GX clips it and keeps the winding, so the
 * visible part is culled the other way round). The guard band is the other
 * case: a triangle that reaches far outside the screen is clipped by the PS2's
 * GS and by GX, but at different places, and the edge's interpolation differs.
 *
 * The probe counts, per frame: the batches with a triangle over the near
 * plane, those triangles, and the triangles that reach far outside the screen
 * (in screen widths from the middle: the screen is -1..1). */
static uint32_t g_clip_bat, g_clip_tri, g_clip_far, g_clip_far2, g_clip_vert, g_clip_skipped, g_clip_inverted;
static int g_clip_crosses;
static float g_clip_w[512], g_clip_nx[512], g_clip_ny[512];

/* the depth of every vertex (the camera's rows 1014..1017: W is the fourth component's row, positive in front of
 * the eye, as the PC host's reference (gs_soft.c worldcmp_batch) has it) and, for the ones in front, where it
 * lands on the screen: in screen half-widths and half-heights from the middle of the GS's scissor rectangle, so
 * the screen is -1..1 whatever the target's size */
static void batch_project(const NwBlock *b, const GsDraw *d)
{
    const float sizex = VQ(1018, 0), sizey = VQ(1018, 1);
    const float ofx = (float)GS_FIELD(d->xyoffset, 0, 16) / 16.0f, ofy = (float)GS_FIELD(d->xyoffset, 32, 16) / 16.0f;
    const float x0 = (float)GS_FIELD(d->scissor, 0, 11), x1 = (float)GS_FIELD(d->scissor, 16, 11) + 1.0f;
    const float y0 = (float)GS_FIELD(d->scissor, 32, 11), y1 = (float)GS_FIELD(d->scissor, 48, 11) + 1.0f;
    const float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1), ihx = 2.0f / (x1 - x0 > 1.0f ? x1 - x0 : 1.0f),
                ihy = 2.0f / (y1 - y0 > 1.0f ? y1 - y0 : 1.0f);
    const float bx = VQ(1019, 0) - ofx - 0.5f * sizex - cx, by = VQ(1019, 1) - ofy - 0.5f * sizey - cy;
    const float a = VQ(1014, 3), c1 = VQ(1015, 3), c2 = VQ(1016, 3), dd = VQ(1017, 3);
    const float ea = VQ(1014, 0), eb = VQ(1015, 0), ec = VQ(1016, 0), ed = VQ(1017, 0);
    const float fa = VQ(1014, 1), fb = VQ(1015, 1), fc = VQ(1016, 1), fd = VQ(1017, 1);
    uint32_t i;
    if (b->n > 512)
        return;
    for (i = 0; i < b->n; i++) {
        const float x = LWF(b->pos + 16 * i), y = LWF(b->pos + 16 * i + 4), z = LWF(b->pos + 16 * i + 8);
        const float W = a * x + c1 * y + c2 * z + dd;
        const float X = ea * x + eb * y + ec * z + ed;
        const float Y = fa * x + fb * y + fc * z + fd;
        g_clip_w[i] = W;
        if (W > 0.0f) {
            const float iw = 1.0f / W;
            g_clip_nx[i] = (bx + sizex * X * iw) * ihx;
            g_clip_ny[i] = (by + sizey * Y * iw) * ihy;
        } else {
            g_clip_nx[i] = 0.0f;
            g_clip_ny[i] = 0.0f;
        }
    }
}

/* does a triangle of the batch cross the near plane (a vertex behind the eye
 * and another in front)? */
static int batch_crosses_near(const NwBlock *b)
{
    uint32_t i;
    if (b->n > 512)
        return 0;
    for (i = 2; i < b->n; i++) {
        const float w0 = g_clip_w[i - 2], w1 = g_clip_w[i - 1], w2 = g_clip_w[i];
        if (LW(b->pos + 16 * i + 12) & 0x8000)
            continue;                       /* the triangle ending at this vertex is not drawn */
        if ((w0 > 0.0f) != (w1 > 0.0f) || (w0 > 0.0f) != (w2 > 0.0f))
            return 1;
    }
    return 0;
}

static void clip_probe(const NwBlock *b, const GsDraw *d)
{
    uint32_t i;
    int cross = 0, far = 0;
    if (b->n > 512)
        return;                             /* batch_project leaves the arrays alone */
    batch_project(b, d);
    for (i = 2; i < b->n; i++) {
        float w0, w1, w2, xmin, xmax, ymin, ymax;
        if (LW(b->pos + 16 * i + 12) & 0x8000)
            continue;                       /* the triangle ending at this vertex is not drawn */
        w0 = g_clip_w[i - 2];
        w1 = g_clip_w[i - 1];
        w2 = g_clip_w[i];
        if ((w0 > 0.0f) != (w1 > 0.0f) || (w0 > 0.0f) != (w2 > 0.0f)) {
            g_clip_tri++;
            cross = 1;
            continue;
        }
        if (w0 <= 0.0f)
            continue;                       /* behind the eye: nothing of it is drawn */
        xmin = g_clip_nx[i - 2] < g_clip_nx[i - 1] ? g_clip_nx[i - 2] : g_clip_nx[i - 1];
        xmax = g_clip_nx[i - 2] > g_clip_nx[i - 1] ? g_clip_nx[i - 2] : g_clip_nx[i - 1];
        ymin = g_clip_ny[i - 2] < g_clip_ny[i - 1] ? g_clip_ny[i - 2] : g_clip_ny[i - 1];
        ymax = g_clip_ny[i - 2] > g_clip_ny[i - 1] ? g_clip_ny[i - 2] : g_clip_ny[i - 1];
        if (g_clip_nx[i] < xmin) xmin = g_clip_nx[i];
        if (g_clip_nx[i] > xmax) xmax = g_clip_nx[i];
        if (g_clip_ny[i] < ymin) ymin = g_clip_ny[i];
        if (g_clip_ny[i] > ymax) ymax = g_clip_ny[i];
        if (xmin < -2.0f || xmax > 2.0f || ymin < -2.0f || ymax > 2.0f) {
            g_clip_far++;
            far = 1;
            if (xmin < -8.0f || xmax > 8.0f || ymin < -8.0f || ymax > 8.0f)
                g_clip_far2++;
        }
    }
    if (cross)
        g_clip_bat++;
    if (far)
        g_clip_vert++;
}

/* "wskip" (gs.h): the state the first world batch of a call was drawn with, for the ones after it */
static const GsDraw *g_rep_d;
static RT *g_rep_rt;
static uint32_t g_rep_prim;
static int g_rep_ok, g_rep_nat;         /* (nat: the GX mode and projection were set by a batch of this call) */

/* "wskip" 2: did a piece of work put anything in the GX FIFO? GX_Flush writes libogc's pending state and then pads the
 * stream with 32 bytes of zeros, and the write pointer (the PI register) moves by whole bursts of the write gather
 * pipe: after a flush every byte written before it is in memory, and only zeros follow. So the bytes between the
 * pointer after one flush and the pointer after the next are zeros if the work between wrote nothing; a command (none
 * begins with a zero byte) or libogc's pending state shows as a word that is not zero. */
static uint32_t wsk_ptr(void)
{
    GX_Flush();
    return 0x80000000u | (*(volatile uint32_t *)0xCC003014u & 0x03FFFFE0u);
}

static void __attribute__((noinline)) wsk_check(uint32_t from, int which)
{
    const uintptr_t lo = (uintptr_t)g_fifo_ring, hi = lo + FIFO_SIZE;
    const uint32_t to = wsk_ptr();
    uintptr_t a = from;
    int wrote = 0;
    while (a != to) {
        const uintptr_t e = a < to ? to : hi;
        const uint32_t *w = (const uint32_t *)a;
        DCInvalidateRange((void *)a, (u32)(e - a));
        for (; (uintptr_t)w < e; w++)
            wrote |= *w != 0;
        a = e == hi ? lo : e;
    }
    g_wsk_n[which]++;
    g_wsk_v[which] += wrote;
}

int gs_backend_world(const NwBlock *b, int kind)
{
    uint32_t *vm = g_vu_mem + 0xC000 / 4;
    uint32_t w1, prim, cull, i, count;
    const GsDraw *d;
    DList *dl;
    RT *rt;
    int prev, rep = g_world_rep, nat = 0, chk = 0;
    uint32_t mark = 0;
    uint64_t wst_t = g_wstat == 1 ? gettime() : 0;
    g_world_rep = 0;
    if (!g_gx_ready || g_gx_bisect == 1 || !kind)
        return 0;
    /* 1013.x set: the batch reaches the edge of the view and the
     * microprogram takes its clipping path; GX clips (gxflags 128: VU1
     * draws those instead) */
    if (kind != NW_INST && (vm[1013 * 4] & 0xFFFF) && (g_gx_flags & 128))
        return 0;
    if (g_gx_maxdraw && ++g_frame_draws > (uint32_t)g_gx_maxdraw)
        return 1;
    /* The world microprograms keep the camera rows in VF registers and reload
     * them only when 1013.w is set (then clear it). Camera uploads consumed
     * here never reached them: make the next emulated run reload. */
    vm[1013 * 4 + 3] = 1;
    prev = pt_enter(PT_GX);
    /* the microprogram's GIF tag (qword 1006) sets PRIM */
    w1 = vm[1006 * 4 + 1];
    prim = (w1 & 0x4000) ? (w1 >> 15) & 0x7FF : (uint32_t)gs_reg(GS_PRIM);
    if (b->nouv && ((prim & 0x10) || (g_gx_flags & 1024))) {
        pt_leave(prev);
        return 0;                       /* textured without UVs: whatever VU memory holds */
    }
    WST(0);
    if (PS2_UNLIKELY(g_wskip & 4)) {
        nat = chk = 1;                      /* the control of 2: every batch is looked at, the ones that change state show it */
        rep = 0;
    } else if (PS2_UNLIKELY(g_wskip & 3) && rep && g_rep_ok && g_rep_prim == prim &&
               !(g_gx_flags2 & (8388608 | 16777216 | 33554432)) && !(g_gx_flags & 4194304)) {
        nat = g_rep_nat;
        chk = !(g_wskip & 1);               /* "wskip" 2: all the work is done, and what it writes to the FIFO counted */
        rep = !chk;
    } else {
        rep = 0;
    }
    if (PS2_UNLIKELY(rep)) {
        d = g_rep_d;
        rt = g_rep_rt;
        g_draw_no++;
        WST(3);
    } else {
        g_rep_ok = g_rep_nat = 0;
        if (PS2_UNLIKELY(chk))
            mark = wsk_ptr();
        d = gs_draw_state(prim);
        if (PS2_UNLIKELY((g_gx_flags2 & (8388608 | 16777216 | 33554432)) && kind != NW_INST)) {
            batch_project(b, d);
            g_clip_crosses = batch_crosses_near(b);
            if (g_gx_flags2 & 8388608)
                clip_probe(b, d);
            if ((g_gx_flags2 & 16777216) && g_clip_crosses) {
                /* the diagnostic: the batches with a triangle over the near plane
                 * are not drawn at all, so the piece of road that the PS2's
                 * microprogram would have clipped is a hole in the picture */
                g_clip_skipped++;
                pt_leave(prev);
                return 1;
            }
        }
        g_draw_no++;
        WST(1);
        g_ds_who = 1;
        rt = draw_setup(d);
        g_ds_who = 0;
        WST(2);
        if (!rt) {
            pt_leave(prev);
            return 1;                       /* the GS would not draw it either */
        }
        if ((g_gx_flags & 4194304) && rt->fbp == 0x3160) {
            /* debugging: every world batch into the first reflection view */
            extern uint32_t g_vif1_chains;
            static uint32_t lines, last;
            if (last != g_vif1_chains && lines++ < 3000) {
                last = g_vif1_chains;
                LOG("w3160 %u cam %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X  %08X %08X %08X %08X z %08X %08X",
                    g_vif1_chains, vm[1014 * 4], vm[1014 * 4 + 1], vm[1014 * 4 + 2], vm[1014 * 4 + 3], vm[1015 * 4],
                    vm[1015 * 4 + 1], vm[1015 * 4 + 2], vm[1015 * 4 + 3], vm[1016 * 4], vm[1016 * 4 + 1], vm[1016 * 4 + 2],
                    vm[1016 * 4 + 3], vm[1017 * 4], vm[1017 * 4 + 1], vm[1017 * 4 + 2], vm[1017 * 4 + 3], vm[1018 * 4 + 2],
                    vm[1019 * 4 + 2]);
            }
        }
        rt_drawn(rt, 0, 0, rt->w - 1, rt->h - 1);
        batch_flush();
        if (PS2_UNLIKELY(chk))
            wsk_check(mark, 0);
        WST(3);
        g_rep_d = d;
        g_rep_rt = rt;
        g_rep_prim = prim;
        g_rep_ok = 1;
    }
    if (PS2_UNLIKELY(g_wstat == 3 || g_wstat == 5))
        wh_batch(b, kind, prim, rt, vm);
    if ((g_gx_flags & 67108864) && rt->fbp == 0x8C0) {
        cw_cam_note(0, vm);
        if (kind != NW_INST)
            cw_ground(b);
        if (!g_cwrt.nworld++)
            g_cwrt.world[0] = g_draw_no;
        g_cwrt.world[1] = g_draw_no;
    }
    if (kind == NW_LIT) {
        g_rep_nat = 0;
        if (!(g_gx_flags & 256) && lit_draw_dl(b, rt, d)) {
            pt_leave(prev);
            return 1;
        }
        lit_draw(b, rt, d);
        pt_leave(prev);
        return 1;
    }
    if (kind == NW_INST && b->nrm && (vm[932 * 4 + 1] & 0xFFFF)) {
        int r;
        g_rep_nat = 0;
        r = inst_lit_draw(b, rt, d, vm);
        pt_leave(prev);
        return r;
    }
    dl = dl_get(b, 0);
    WST(4);
    if (!dl || dl->uv32 != (kind == NW_INST)) {
        pt_leave(prev);
        return 0;
    }
    rep = rep && nat;
    if (!rep) {
        chk = chk && nat;
        if (PS2_UNLIKELY(chk))
            mark = wsk_ptr();
        native_mode(kind);
        native_projection(rt, d, 1.0f);
        if (PS2_UNLIKELY(chk))
            wsk_check(mark, 1);
        g_rep_nat = 1;
    }
    if (kind == NW_INST)
        set_cull(GX_CULL_NONE);         /* the instanced props cull nothing */
    else
        native_cull(vm, behind_camera(dl->bmin, dl->bmax));
    if (PS2_UNLIKELY((g_gx_flags2 & 33554432) && g_clip_crosses)) {
        /* the diagnostic: the batches with a triangle over the near plane are
         * drawn with the other cull sense - what the PS2's microprogram does
         * when it clips such a triangle and reverses its winding */
        set_cull(g_nst.cull == GX_CULL_BACK ? GX_CULL_FRONT : GX_CULL_BACK);
        g_clip_inverted++;
    }
    WST(5);
    /* the game may rewrite vertices in place (billboards): GX reads memory,
     * written out once a frame */
    dl_touch(dl, dl->n * (dl->uv32 ? 8 : 4));
    GX_SetArray(GX_VA_POS, GPTR(dl->pos), 16);
    GX_SetArray(GX_VA_TEX0, GPTR(dl->uv), dl->uv32 ? 8 : 4);
    WST(6);
    if (kind == NW_INST) {
        /* 932.x + 1 instances share the vertices */
        count = (vm[932 * 4] & 0xFFFF) + 1;
        if (count > 20)
            count = 20;
        for (i = 0; i < count; i++) {
            const uint32_t *c = &vm[(938 + 5 * i) * 4];
            native_instance(i);
            /* as vertex colours: RGB as the GS gets them, alpha 0x80 = 1.0 */
            GX_SetChanMatColor(GX_COLOR0A0, (GXColor){(u8)c[0], (u8)c[1], (u8)c[2], (u8)a2(c[3] & 0xFF)});
            GX_CallDispList(dl->data, dl->size);
            g_native_draws++;
        }
    } else {
        if (!rep) {
            if (PS2_UNLIKELY(chk))
                mark = wsk_ptr();
            native_scroll(vm);
            native_camera();
            if (PS2_UNLIKELY(chk))
                wsk_check(mark, 2);
        }
        if (!clip_batch(rt, b, dl, vm, NULL)) {
            GX_CallDispList(dl->data, dl->size);
            g_native_draws++;
        }
        WST(7);
    }
    pt_leave(prev);
    return 1;
}

/* ---- sky (VU1 46A590) ----------------------------------------------------------------------- */

/* ---- the sky, a whole run (VU1 0x46A590) ----------------------------------------------------------
 *
 * The microprogram makes a layer of the dome out of nothing but its header
 * (1003..1006: azimuth step d in turns, Y scale; strips S, Z; height step dh
 * and start h0, pairs N, mode; u0, du, v0, v scale; the colour): S / 4 rings
 * of a quarter turn, each a meridian strip of 2 (N + 1) vertices from the
 * horizon up between azimuths a and a + d, drawn four times a quarter turn
 * apart; r = 1 - h^2, y = 2h - h^2; U along the azimuth and V from y
 * (mode 0) or both from x and z (mode 1, the reflection's clouds); cosines
 * from its own polynomial. 144 strips, 3552 vertices a frame for the five
 * runs, the same every frame but for the view's matrix and the clouds' U
 * offset: each layer is a display list here, made once (positions and UVs as
 * 16-bit, 10 bytes a vertex), drawn with the frame's matrix, the U offset a
 * texture matrix, the colour a register. vu1_sky_strip (the strip subroutine
 * alone) stays for gxflags2 32, and with gxflags2 64 compares the strips the
 * microprogram hands it with these (sky_check). */
typedef struct SkyVert {
    float x, y, z, u, v;
} SkyVert;
#define SKY_HDR(n, k) (h[((n) - 1003) * 4 + (k)])
#define SKY_HDRF(n, k) BITS2F(SKY_HDR(n, k))

static struct {
    uint32_t key[9];
    void *list;
    uint32_t size, strips, per;
} g_sky[8];
static int g_nsky;
static uint32_t g_sky_hdr[16], g_sky_call, g_sky_runs;

static float sky_cos(float t)
{
    const float t2 = t * t, t4 = t2 * t2, t6 = t4 * t2, t8 = t4 * t4, t10 = t6 * t4;
    float a = 1.0f - t2 * BITS2F(0x419DC51Du);
    a += t4 * BITS2F(0x4281C2B8u);
    a -= t6 * BITS2F(0x42AAC215u);
    a += t8 * BITS2F(0x4270C264u);
    return a - t10 * BITS2F(0x41D337BDu);
}

/* ring i of a layer: its strip at 0 degrees, per = 2 (N + 1) vertices (U without u0), and du */
static void sky_ring(const uint32_t *h, uint32_t i, SkyVert *b)
{
    const uint32_t S = SKY_HDR(1003, 2) & 0xFFFF, N = SKY_HDR(1004, 2) & 0xFFFF, rings = S / 4;
    const float d = SKY_HDRF(1003, 0), dh = SKY_HDRF(1004, 0), h0 = SKY_HDRF(1004, 1);
    const float du = SKY_HDRF(1005, 1), v0 = SKY_HDRF(1005, 2), vs = SKY_HDRF(1005, 3);
    float a = 0.0f, u = 0.0f, hh = h0, c0, s0, c1, s1;
    uint32_t k;
    for (k = 0; k < i; k++) {           /* as the program adds them up */
        a += d;
        u += du;
    }
    c0 = !i ? 1.0f : sky_cos(a);
    s0 = !i ? 0.0f : sky_cos(a - 0.25f);
    c1 = i + 1 == rings ? 0.0f : sky_cos(a + d);
    s1 = i + 1 == rings ? 1.0f : sky_cos((a - 0.25f) + d);
    for (k = 0; k <= N; k++, hh += dh) {
        const float r = 1.0f - hh * hh, y = (hh + hh) - hh * hh, v = v0 + y * vs;
        b[2 * k] = (SkyVert){c0 * r, y, s0 * r, u, v};
        b[2 * k + 1] = (SkyVert){c1 * r, y, s1 * r, u + du, v};
    }
}

/* vertex k of the ring's strip turned by quarter q (0, 180, 90, 270 degrees: the program's order) */
static SkyVert sky_turn(const uint32_t *h, const SkyVert *b, uint32_t i, uint32_t q, uint32_t k)
{
    const uint32_t S = SKY_HDR(1003, 2) & 0xFFFF;
    const float du = SKY_HDRF(1005, 1), s = du * (float)S;
    static const float off[4] = {0.0f, 0.5f, 0.25f, 0.75f};
    SkyVert o = b[k];
    if (q == 1) {
        o.x = -b[k].x;
        o.z = -b[k].z;
    } else if (q == 2) {
        o.x = -b[k].z;
        o.z = b[k].x;
    } else if (q == 3) {
        o.x = b[k].z;
        o.z = -b[k].x;
    }
    if (SKY_HDR(1004, 3) & 0xFFFF) {
        o.u = 0.5f + 0.5f * o.x;
        o.v = 0.5f + 0.5f * o.z;
    } else if (q) {
        const float un = (float)(i + 1) * du + off[q] * s;
        o.u = (k & 1) ? un : un - du;
    }
    return o;
}

static inline s16 sky_s16(float x, float scale)
{
    const float v = x * scale;
    return (s16)(v >= 32767.0f ? 32767 : v <= -32767.0f ? -32767 : (int)(v + (v >= 0.0f ? 0.5f : -0.5f)));
}

static int sky_mesh(const uint32_t *h)
{
    const uint32_t S = SKY_HDR(1003, 2) & 0xFFFF, N = SKY_HDR(1004, 2) & 0xFFFF, per = 2 * (N + 1);
    const uint32_t key[9] = {SKY_HDR(1003, 0), S, SKY_HDR(1004, 0), SKY_HDR(1004, 1), N, SKY_HDR(1004, 3) & 0xFFFF,
                             SKY_HDR(1005, 1), SKY_HDR(1005, 2), SKY_HDR(1005, 3)};
    static SkyVert b[52];
    uint32_t cap, i, q, k;
    uint8_t *buf;
    int e;
    for (e = 0; e < g_nsky; e++)
        if (!memcmp(g_sky[e].key, key, sizeof key))
            return e;
    if (g_nsky == 8 || !S || (S & 3) || S > 128 || N + 1 > 26)
        return -1;
    cap = (S * (per * 10 + 3) + 64 + 31) & ~31u;
    buf = plat_alloc(cap);
    if (!buf || ((uintptr_t)buf & 31))
        return -1;
    batch_flush();
    DCInvalidateRange(buf, cap);
    GX_BeginDispList(buf, cap);
    for (i = 0; i < S / 4; i++) {
        sky_ring(h, i, b);
        for (q = 0; q < 4; q++) {
            GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT6, (u16)per);
            for (k = 0; k < per; k++) {
                const SkyVert o = sky_turn(h, b, i, q, k);
                GX_Position3s16(sky_s16(o.x, 16384.0f), sky_s16(o.y, 16384.0f), sky_s16(o.z, 16384.0f));
                GX_TexCoord2s16(sky_s16(o.u, 4096.0f), sky_s16(o.v, 4096.0f));
            }
            GX_End();
        }
    }
    e = g_nsky;
    g_sky[e].size = GX_EndDispList();
    if (!g_sky[e].size)
        return -1;
    DCFlushRange(buf, g_sky[e].size);      /* the CP fetches the list from RAM */
    g_dl_syncs++;
    memcpy(g_sky[e].key, key, sizeof key);
    g_sky[e].list = buf;
    g_sky[e].strips = S;
    g_sky[e].per = per;
    g_nsky++;
    LOG("gx: sky layer %d: %u strips of %u vertices, a list of %u bytes", e, S, per, g_sky[e].size);
    return e;
}

int gs_backend_sky(const uint32_t *m)
{
    const uint32_t *h = &m[1003 * 4];
    const uint32_t tag_y = m[1022 * 4 + 1], ad_y = m[1023 * 4 + 1], nloop = m[1023 * 4] & 0x7FFF;
    const uint32_t prim = (tag_y >> 14) & 1 ? (tag_y >> 15) & 0x7FF : (uint32_t)gs_reg(GS_PRIM);
    const float *q19 = (const float *)&m[1019 * 4];
    const float ys = BITS2F(m[1003 * 4 + 1]), zc = (float)(int32_t)(m[1003 * 4 + 3] & 0xFFFF) / 16.0f;
    float col[4][3], s, sx, sy, ofx, ofy, zscale;
    const GsDraw *d;
    uint32_t i, c, zpsm;
    Mtx mtx, tm;
    Mtx44 p;
    RT *rt;
    int e, fp;
    memcpy(g_sky_hdr, h, sizeof g_sky_hdr);     /* the program changes it as it goes (sky_check) */
    g_sky_call = 0;
    if (!g_gx_ready || g_gx_bisect == 1 || g_gx_maxdraw || !(g_native_render & NATIVE_WORLD) || (g_gx_flags & 512) ||
        (g_gx_flags2 & 32))
        return 0;
    /* the strips' tag (PACKED: ST, RGBAQ, XYZ2; a triangle strip without fog) and the state's (A+D) */
    if (((tag_y >> 26) & 3) != 0 || (tag_y >> 28) != 3 || (prim & 0x27) != 4 || ((ad_y >> 26) & 3) != 0 || (ad_y >> 28) != 1 ||
        (m[1023 * 4 + 2] & 0xF) != 0xE || nloop > 8)
        return 0;
    e = sky_mesh(h);
    if (e < 0)
        return 0;
    for (i = 0; i < nloop; i++) {
        const uint32_t *q = &m[(1007 + i) * 4];
        gs_write_reg(q[2] & 0xFF, (uint64_t)q[1] << 32 | q[0]);
    }
    g_sky_runs++;
    d = gs_draw_state(prim);
    g_draw_no += g_sky[e].strips * (g_sky[e].per - 2);
    rt = draw_setup(d);
    if (!rt)
        return 1;
    rt_drawn(rt, 0, 0, rt->w - 1, rt->h - 1);
    batch_flush();
    native_mode(NW_SKY);
    set_cull(GX_CULL_NONE);
    /* the view's rows X, Y, W as the program prepares them (0x0000): columns for x, y, z and the translation */
    sx = q19[0] > 1.0f ? q19[0] : 640.0f;
    sy = q19[1] > 1.0f ? q19[1] : 448.0f;
    for (c = 0; c < 4; c++) {
        const float *r = (const float *)&m[(1015 + c) * 4];
        col[c][0] = (r[0] - 0.5f * r[3]) * q19[0];
        col[c][1] = (r[1] - 0.5f * r[3]) * q19[1];
        col[c][2] = r[3];
    }
    for (c = 0; c < 3; c++)
        col[1][c] *= ys;
    for (c = 0; c < 4; c++) {
        mtx[0][c] = col[c][0];
        mtx[1][c] = col[c][1];
        mtx[2][c] = -col[c][2];
    }
    GX_LoadPosMtxImm(mtx, GX_PNMTX2);
    set_mtx(GX_PNMTX2);
    g_nst.cam_valid = 0;
    /* screen = (1019.z, 1019.w) + (X, Y) / W, GS Z = 1003.w / 16 (vu1_sky_strip) */
    s = 1.0f / (float)(1 << rt->sh);
    ofx = (float)GS_FIELD(d->xyoffset, 0, 16) / 16.0f;
    ofy = (float)GS_FIELD(d->xyoffset, 32, 16) / 16.0f;
    zpsm = GS_FIELD(d->zbuf, 24, 4);
    zscale = zpsm == 0 ? 1.0f / 4294967296.0f : zpsm == 1 ? 1.0f / 16777216.0f : 1.0f / 65536.0f;
    memset(p, 0, sizeof p);
    p[0][0] = 1.0f / sx;
    p[1][1] = -1.0f / sy;
    p[2][2] = 1.0f - ZDS * zscale * zc;
    p[3][2] = -1.0f;
    GX_LoadProjectionMtx(p, GX_PERSPECTIVE);
    GX_SetViewport((float)rt->ex + s * (q19[2] - ofx) - s * sx, (float)rt->ey + s * (q19[3] - ofy) - s * sy, 2.0f * s * sx,
                   2.0f * s * sy, 0, 1);
    g_nst.proj_valid = 0;
    GX_SetChanMatColor(GX_COLOR0A0, (GXColor){(u8)m[1006 * 4], (u8)m[1006 * 4 + 1], (u8)m[1006 * 4 + 2],
                                              (u8)a2(m[1006 * 4 + 3] & 0xFF)});
    /* the clouds' U offset (1005.x, moving): not in the list */
    memset(tm, 0, sizeof tm);
    tm[0][0] = tm[1][1] = tm[2][2] = 1.0f;
    tm[0][3] = (m[1004 * 4 + 3] & 0xFFFF) ? 0.0f : BITS2F(m[1005 * 4]);
    GX_LoadTexMtxImm(tm, GX_TEXMTX3, GX_MTX2x4);
    for (fp = 0; fp < fix2_passes(); fp++) {
        fix2_set(fp);
        GX_CallDispList(g_sky[e].list, g_sky[e].size);
    }
    fix2_set(0);
    g_native_draws++;
    return 1;
}

/* gxflags2 64 (with 32): a strip as the microprogram hands it to its subroutine against sky_ring and sky_turn */
static void __attribute__((noinline)) sky_check(const uint32_t *m, uint32_t n)
{
    static SkyVert b[52];
    static uint32_t checked, bad;
    const uint32_t *h = g_sky_hdr, i = g_sky_call / 4, q = g_sky_call % 4, per = 2 * ((SKY_HDR(1004, 2) & 0xFFFF) + 1);
    const float u0 = (SKY_HDR(1004, 3) & 0xFFFF) ? 0.0f : SKY_HDRF(1005, 0);
    float dp = 0.0f, du = 0.0f;
    uint32_t k, c;
    g_sky_call++;
    if (checked >= 600 || per > 52)
        return;
    sky_ring(h, i, b);
    for (k = 0; k < n && k < per; k++) {
        const float *pos = (const float *)&m[(3 * k) * 4], *uv = (const float *)&m[(3 * k + 1) * 4];
        const SkyVert o = sky_turn(h, b, i, q, k);
        for (c = 0; c < 3; c++)         /* in memory the strip is not turned: the matrix is */
            if (fabsf(pos[c] - (&b[k].x)[c]) > dp)
                dp = fabsf(pos[c] - (&b[k].x)[c]);
        if (fabsf(uv[0] - (o.u + u0)) > du) du = fabsf(uv[0] - (o.u + u0));
        if (fabsf(uv[1] - o.v) > du) du = fabsf(uv[1] - o.v);
    }
    if (n != per || dp > 0.0002f || du > 0.0002f) {
        if (bad++ < 12)
            LOG("sky check: ring %u quarter %u: %u vertices (%u expected), positions %g, UVs %g apart", i, q, n, per, (double)dp,
                (double)du);
    }
    if (++checked == 600)
        LOG("sky check: 600 strips compared with the lists' vertices, %u differ", bad);
}


/* The sky microprogram builds its dome strip by strip and calls 0x0888 for
 * each: transform, clip against the view (a full polygon clipper), divide
 * and kick. vu1_sky_strip does that with GX: the strip in VU1 memory
 * (vertex k: position at 3k with the ADC flag in w, UV floats at 3k+1,
 * colour integers at 3k+2), vi8 = (vi9 + 1) & ~1 vertices, through
 *   X, Y, W = vf24 + vf28 x + vf29 y + vf30 z   (vf24: vf31 x, y, w)
 * to screen vf2.x + X/W, vf2.y + Y/W, Z = vf2.z + vf31.z / W, fog min(vf2.w,
 * 255), STQ from the UVs; GX clips. The GS state goes first, once a run: the
 * A+D packet at 208..216 (the GIF tag of the strips at 1022 sets PRIM).
 * Returns 0 to let the microcode do it (no native rendering, or a layout
 * this does not know). */
int vu1_sky_strip(VuCtx *ctx)
{
    const uint32_t *m = ctx->mem;
    float (*vf)[4] = ctx->vf;
    uint32_t *vi = ctx->vi;
    uint32_t n, k, prim, tag_y, i, fog;
    const GsDraw *d;
    RT *rt;
    if (!g_gx_ready || !(g_native_render & NATIVE_WORLD) || (g_gx_flags & 512))
        return 0;
    /* the strips' GIF tag: PACKED STQ, RGBAQ, XYZF2 (in some order), PRIM a triangle strip */
    tag_y = m[1022 * 4 + 1];
    prim = (tag_y >> 14) & 1 ? (tag_y >> 15) & 0x7FF : (uint32_t)gs_reg(GS_PRIM);
    if (((tag_y >> 26) & 3) != 0 || (tag_y >> 28) != 3 || (prim & 7) != 4)
        return 0;
    if ((vi[13] & 0xFFFF) == 208) {
        /* the run's first strip: the GS state packet (GIF tag at 208, A+D) */
        uint32_t nloop = m[208 * 4] & 0x7FFF, ty = m[208 * 4 + 1];
        if (((ty >> 26) & 3) != 0 || (ty >> 28) != 1 || (m[208 * 4 + 2] & 0xF) != 0xE || nloop > 16)
            return 0;
        for (i = 0; i < nloop; i++) {
            const uint32_t *q = &m[(209 + i) * 4];
            gs_write_reg(q[2] & 0xFF, (uint64_t)q[1] << 32 | q[0]);
        }
        vi[13] = 217;                   /* sent: the microcode would kick from 217/610 next */
    }
    n = ((vi[9] & 0xFFFF) + 1) & 0x7FFE;
    if (g_gx_flags2 & 64)
        sky_check(m, n);
    if (n >= 3 && n <= 300) {
        float s, sx, sy, ofx, ofy, zscale, k0, k1;
        const float *q19 = (const float *)&m[1019 * 4];
        uint32_t zpsm;
        Mtx mtx;
        Mtx44 p;
        d = gs_draw_state(prim);
        g_draw_no++;
        rt = draw_setup(d);
        if (rt) {
            rt_drawn(rt, 0, 0, rt->w - 1, rt->h - 1);
            batch_flush();
            native_mode(NW_CAR);        /* direct positions, colours and float UVs (VTXFMT3) */
            set_cull(GX_CULL_NONE);
            /* object to (X, Y, -W) */
            for (k = 0; k < 3; k++) {
                mtx[0][k] = vf[28 + k][0];
                mtx[1][k] = vf[28 + k][1];
                mtx[2][k] = -vf[28 + k][3];
            }
            mtx[0][3] = vf[31][0];
            mtx[1][3] = vf[31][1];
            mtx[2][3] = -vf[31][3];
            GX_LoadPosMtxImm(mtx, GX_PNMTX2);
            set_mtx(GX_PNMTX2);
            g_nst.cam_valid = 0;
            /* screen = vf2 + (X, Y)/W, GS Z = vf2.z + vf31.z/W; the clip
             * volume twice the screen (1019.xy) around its centre */
            s = 1.0f / (float)(1 << rt->sh);
            sx = q19[0] > 1.0f ? q19[0] : 640.0f;
            sy = q19[1] > 1.0f ? q19[1] : 448.0f;
            ofx = (float)GS_FIELD(d->xyoffset, 0, 16) / 16.0f;
            ofy = (float)GS_FIELD(d->xyoffset, 32, 16) / 16.0f;
            zpsm = GS_FIELD(d->zbuf, 24, 4);
            zscale = zpsm == 0 ? 1.0f / 4294967296.0f : zpsm == 1 ? 1.0f / 16777216.0f : 1.0f / 65536.0f;
            k0 = 1.0f - ZDS * zscale * vf[2][2];
            k1 = ZDS * zscale * vf[31][2];
            memset(p, 0, sizeof p);
            p[0][0] = 1.0f / sx;
            p[1][1] = -1.0f / sy;
            p[2][2] = k0;
            p[2][3] = k1;
            p[3][2] = -1.0f;
            GX_LoadProjectionMtx(p, GX_PERSPECTIVE);
            GX_SetViewport((float)rt->ex + s * (vf[2][0] - ofx) - s * sx, (float)rt->ey + s * (vf[2][1] - ofy) - s * sy,
                           2.0f * s * sx, 2.0f * s * sy, 0, 1);
            g_nst.proj_valid = 0;
            fog = vf[2][3] < 255.0f ? (vf[2][3] > 0.0f ? (uint32_t)vf[2][3] : 0) : 255;
            if (prim & 0x20)
                GX_SetChanMatColor(GX_COLOR1A1, (GXColor){255, 255, 255, (u8)fog});
            {
                int fp;
                for (fp = 0; fp < fix2_passes(); fp++) {
                    fix2_set(fp);
                    /* runs of triangles: a vertex with ADC (position w bit 15) ends none */
                    for (i = 2; i < n;) {
                        uint32_t first, last;
                        if (m[(3 * i) * 4 + 3] & 0x8000) {
                            i++;
                            continue;
                        }
                        first = i - 2;
                        while (i < n && !(m[(3 * i) * 4 + 3] & 0x8000))
                            i++;
                        last = i - 1;
                        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT3, (u16)(last - first + 1));
                        for (k = first; k <= last; k++) {
                            const float *pos = (const float *)&m[(3 * k) * 4];
                            const float *uv = (const float *)&m[(3 * k + 1) * 4];
                            const uint32_t *c = &m[(3 * k + 2) * 4];
                            GX_Position3f32(pos[0], pos[1], pos[2]);
                            GX_Color1u32((c[0] & 0xFF) << 24 | (c[1] & 0xFF) << 16 | (c[2] & 0xFF) << 8 |
                                         a2(c[3] & 0xFF));
                            GX_TexCoord2f32(uv[0], uv[1]);
                        }
                        GX_End();
                    }
                }
                fix2_set(0);
            }
            if (prim & 0x20)
                GX_SetChanMatColor(GX_COLOR1A1, (GXColor){255, 255, 255, 255});
            g_native_draws++;
        }
    }
    /* what the subroutine leaves that the program reads again */
    for (k = 0; k < 3; k++)
        vf[28 + k][2] = 0.0f;
    vf[1][1] = BITS2F(m[1022 * 4 + 1]);
    vf[1][2] = BITS2F(m[1022 * 4 + 2]);
    vf[1][3] = BITS2F(m[1022 * 4 + 3]);
    vf[23][2] = 0.0f;
    vf[23][3] = 1.0f;
    return 1;
}

/* ---- cars (runtime/nw_car.c) -------------------------------------------------------------------- */

static NcIn g_nc_in;
static NcOut g_nc_out;

/* one pass of a car batch: PS2 strips, a vertex with ADC set ends no triangle */
static void car_pass(const NcOut *o, int pass)
{
    uint32_t i, k, c1 = (uint32_t)o->col1[0] << 24 | (uint32_t)o->col1[1] << 16 | (uint32_t)o->col1[2] << 8 |
                        a2(o->col1[3]);
    int fp;
    for (fp = 0; fp < fix2_passes(); fp++) {
    fix2_set(fp);
    for (i = 2; i < o->n;) {
        uint32_t first, last;
        if (o->adc[i]) {
            i++;
            continue;
        }
        first = i - 2;
        while (i < o->n && !o->adc[i])
            i++;
        last = i - 1;
        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT3, (u16)(last - first + 1));
        for (k = first; k <= last; k++) {
            GX_Position3f32(o->pos[k][0], o->pos[k][1], o->pos[k][2]);
            if (pass)
                GX_Color1u32(c1);
            else
                GX_Color1u32((uint32_t)o->col[k][0] << 24 | (uint32_t)o->col[k][1] << 16 | (uint32_t)o->col[k][2] << 8 |
                             a2(o->col[k][3]));
            if (pass)
                GX_TexCoord2f32(o->uv1[k][0], o->uv1[k][1]);
            else
                GX_TexCoord2f32(o->uv0[k][0], o->uv0[k][1]);
        }
        GX_End();
    }
    }
    fix2_set(0);
}

/* Car batches without crash deformation as display lists over the render
 * data (DList, idxcol 2): pass 1 indexes the positions (stride 16), a colour
 * array made per frame (nc_shade_direct) and the UV pairs, which nc_shade
 * takes for vertex 4g+r from slot 1 of vertex 4g (r 0, 2) or 4g+2 (r 1, 3),
 * first or second pair (r < 2 or not). Pass 2 sends its sphere-map UVs
 * directly, positions indexed. */
static float g_car_uv1[NC_MAX][2];

static uint32_t car_uv_pair(const NcBlock *b, uint32_t k)
{
    uint32_t j = (k & ~3u) + ((k & 1) ? 2 : 0);
    return 2 * (b->uvstep == 2 ? j >> 1 : j) + ((k >> 1) & 1);
}

static DList *car_dl_get(const NcBlock *b)
{
    uint32_t block = b->pos, h = (block >> 4) % DL_HASH, check, cap, i, size, pos_skip, uv_skip, ext;
    DList *d;
    uint8_t *buf;
    if (b->n < 3 || b->n > 200 || !b->uvn || b->uvn > 120)
        return NULL;
    /* (and two words of the normals: the list keeps a copy of them) */
    check = mix(mix(mix(mix(mix(mix(b->n, LW(b->pos)), LW(b->pos + 16 * (b->n - 1) + 8)), LW(b->uv + 8 * (b->uvn / 2))),
                        b->uvstep | b->uvn << 8 | b->nrm << 16), LW(b->nrm & ~3u)), LW((b->nrm + 3 * b->n - 1) & ~3u));
    for (d = g_dl_bucket[h]; d; d = d->next)
        if (d->block == block && d->check == check && d->idxcol == 2)
            return d;
    /* every vertex's UV pair in the data (else nc_shade makes it zero) */
    for (i = 0; i < b->n; i++)
        if (car_uv_pair(b, i) >= 2 * b->uvn)
            return NULL;
    cap = (b->n * 16 + 64 + 31) & ~31u;
    pos_skip = (b->pos & 31) / 16;
    uv_skip = (b->uv & 31) / 4;
    DCFlushRange(GPTR(b->pos & ~31u), b->n * 16 + 32);
    DCFlushRange(GPTR(b->uv & ~31u), b->uvn * 8 + 32);
    GX_InvVtxCache();
    ext = (b->n * 4 + 31) & ~31u;           /* the normals, host order: nc_shade_direct8 */
    buf = dl_room(cap + ext);
    if (!buf)
        return NULL;
    DCInvalidateRange(buf, cap);
    GX_BeginDispList(buf, cap);
    /* PS2 strips: a vertex with ADC (w bit 15) ends no triangle */
    for (i = 2; i < b->n;) {
        uint32_t first, last, k;
        if (LW(b->pos + 16 * i + 12) & 0x8000) {
            i++;
            continue;
        }
        first = i - 2;
        while (i < b->n && !(LW(b->pos + 16 * i + 12) & 0x8000))
            i++;
        last = i - 1;
        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT4, (u16)(last - first + 1));
        for (k = first; k <= last; k++) {
            GX_Position1x8((u8)(k + pos_skip));
            GX_Color1x8((u8)k);
            GX_TexCoord1x8((u8)(car_uv_pair(b, k) + uv_skip));
            GX_TexCoord1x8((u8)k);
        }
        GX_End();
    }
    size = GX_EndDispList();
    if (!size)
        return NULL;
    d = dl_entry(((size + 31) & ~31u) + ext);
    d->block = block;
    d->check = check;
    d->flushed = g_frame_no;
    d->data = buf;
    d->size = size;
    d->ext = ext;
    d->pos = b->pos & ~31u;
    d->uv = b->uv & ~31u;
    d->n = b->n;
    d->uvn = b->uvn;
    d->idxcol = 2;
    {
        /* the normals' bytes in host order, xor 0x80 (as signed bytes: k - 128) */
        int8_t *nr = (int8_t *)((uint8_t *)buf + ((size + 31) & ~31u));
        for (i = 0; i < b->n; i++) {
            const uint32_t a = b->nrm + 3 * i;
            nr[4 * i] = (int8_t)(LB(a) ^ 0x80);
            nr[4 * i + 1] = (int8_t)(LB(a + 1) ^ 0x80);
            nr[4 * i + 2] = (int8_t)(LB(a + 2) ^ 0x80);
            nr[4 * i + 3] = 0;
        }
    }
    /* the list and the normals behind it: both are read from RAM (the list by
     * the CP, the normals by the GPU when they are an indexed array) */
    DCFlushRange(buf, ((size + 31) & ~31u) + ext);
    g_dl_syncs++;
    d->next = g_dl_bucket[h];
    g_dl_bucket[h] = d;
    g_dl_built++;
    iop_sd_pump();              /* (a track's first frames make hundreds: sound meanwhile) */
    return d;
}

/* Both passes of a car batch as one GX draw (car_combo).
 *
 * The microprogram kicks every batch twice. Pass 1 (GS context 1): the paint
 * texture times the lighting, opaque (ALPHA (Cs - Cd) FIX + Cd with FIX
 * 0x80), Z tested and written; its alpha, texture times the vertex's (the
 * reflectivity nc_shade makes), goes to the frame buffer. Pass 2 (context
 * 2): the sphere map times the colour 1012, laid over by that alpha, (Cs -
 * Cd) Ad + Cd, with an alpha test that never passes and writes the frame
 * buffer only: no Z, and the frame's alpha becomes the pass's own.
 *
 * As two draws that is every vertex twice and, the two states alternating,
 * all of GX's TEV, blend and texture state set twice a batch, about 360
 * times a frame for the player's car alone. One draw with two textures does
 * the same: stage 0 the paint as pass 1 has it, stage 1 the sphere map
 * times 1012 into a register, stage 2 mixes the two colours by stage 0's
 * alpha and puts out stage 1's alpha; opaque, Z as pass 1. The mix is done
 * with 8 bits of alpha where the frame buffer's would have had 6. Only
 * where the two states are exactly these; everything else (blended paint:
 * glass, fading cars) stays two draws. gxflags 268435456: always two. */
static struct {
    GXTexObj tex;
    uint32_t c1;
    int valid;
} g_combo;

/* are the two contexts' states the pair car_combo draws at once? d1: pass
 * 1's. Loads the sphere map and returns 1, or 0 for two draws. */
/* What the GS registers decided the last time (a write to any register the check reads gives the draw state a
 * new seq), the target it found and what the texture object was made of */
static struct {
    uint32_t seq, tbp, valid, ok;
    RT *src;
    void *tex;
    int ctw, cth, lin;
} g_cok;

/* status line: car batches whose sphere map was a target's picture, was a target's memory that changed since
 * (decoded from GS memory instead), or a target with no picture */
static uint32_t g_env_rt, g_env_mem, g_env_nopic;

static int car_combo_ok(const GsDraw *d1)
{
    const uint64_t a2r = gs_reg(GS_ALPHA_2), t2 = gs_reg(GS_TEST_2), x2 = gs_reg(GS_TEX0_2);
    uint32_t tbp, psm;
    RT *src;
    if (g_gx_flags & 268435456)
        return 0;
    if (g_cok.valid && g_cok.seq == d1->seq && g_opt_new) {
        if (!g_cok.ok)
            return 0;
        src = g_cok.src;
        /* rt_for_texture's answer for the same TBP, without the search */
        if (src->fbp == g_cok.tbp && (src->resident || src->tex_valid) &&
            (src->hold || gs_page_ver[(src->fbp / 32) & 511] <= src->ver)) {
            if (src->resident && (src->dirty || !src->tex_valid))
                rt_copy(src, 0);
            if (!src->tex_valid) {
                g_env_nopic++;
                return 0;
            }
            if (src->tex == g_cok.tex && src->ctw == g_cok.ctw && src->cth == g_cok.cth &&
                (int)GS_FIELD(gs_reg(GS_TEX1_2), 5, 1) == g_cok.lin) {
                g_env_rt++;
                return 1;               /* the texture object made then (g_combo.tex) stands */
            }
        }
    }
    g_cok.valid = 0;
    /* pass 1: opaque, textured (modulate, the texture's alpha), no fog, alpha test off or always, Z GEQUAL written */
    if ((d1->alpha & 0xFF) != 0x64 || GS_FIELD(d1->alpha, 32, 8) < 0x80 || (d1->prim & 0x70) != 0x50)
        goto no;
    if (GS_FIELD(d1->tex0, 35, 2) != 0 || !GS_FIELD(d1->tex0, 34, 1) || GS_FIELD(d1->tex0, 20, 6) == PSMCT24)
        goto no;
    if ((GS_FIELD(d1->test, 0, 1) && GS_FIELD(d1->test, 1, 3) != 1) || GS_FIELD(d1->test, 16, 3) != 5 ||
        GS_FIELD(d1->zbuf, 32, 1) || GS_FIELD(d1->frame, 32, 32) || GS_FIELD(d1->fba, 0, 1))
        goto no;
    /* pass 2: the same target, over by the frame's alpha, alpha test never with FB only, Z GEQUAL */
    if ((a2r & 0xFF) != 0x54 || (t2 & 0x7300F) != 0x51001 || gs_reg(GS_FRAME_2) != d1->frame ||
        gs_reg(GS_ZBUF_2) != d1->zbuf || gs_reg(GS_SCISSOR_2) != d1->scissor || (gs_reg(GS_FBA_2) & 1))
        goto no;
    psm = GS_FIELD(x2, 20, 6);
    if (GS_FIELD(x2, 35, 2) != 0 || !GS_FIELD(x2, 34, 1) || psm != PSMCT32)
        goto no;
    /* the sphere map: a target's picture, covering the texture as pass 2 takes it (UVs 0..1 over the copy) */
    tbp = GS_FIELD(x2, 0, 14);
    src = rt_for_texture(tbp);
    if (!src) {
        g_env_mem++;
        return 0;
    }
    if (src->resident && (src->dirty || !src->tex_valid))
        rt_copy(src, 0);
    if (!src->tex_valid) {
        g_env_nopic++;
        return 0;
    }
    g_env_rt++;
    {
        const int lin = (int)GS_FIELD(gs_reg(GS_TEX1_2), 5, 1);
        GXTexObj to;
        GX_InitTexObj(&to, src->tex, (u16)src->ctw, (u16)src->cth, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjLOD(&to, lin ? GX_LINEAR : GX_NEAR, lin ? GX_LINEAR : GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        if (!g_combo.valid || memcmp(&to, &g_combo.tex, sizeof to)) {
            g_combo.tex = to;
            g_combo.valid = 0;          /* loaded by car_combo_tev */
        }
        g_cok.valid = 1;
        g_cok.ok = 1;
        g_cok.seq = d1->seq;
        g_cok.tbp = tbp;
        g_cok.src = src;
        g_cok.tex = src->tex;
        g_cok.ctw = src->ctw;
        g_cok.cth = src->cth;
        g_cok.lin = lin;
    }
    return 1;
no:
    g_cok.valid = 1;
    g_cok.ok = 0;
    g_cok.seq = d1->seq;
    return 0;
}

/* after draw_setup (pass 1's state is GX's): stages 1 and 2 on top of it */
static void car_combo_tev(uint32_t c1)
{
    if (g_gxc.tev != COMBO_TEV) {
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLORNULL);
        GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K3);
        GX_SetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_K3_A);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVREG0);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_ZERO);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
        GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_CPREV, GX_CC_C0, GX_CC_APREV, GX_CC_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
        GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetNumTevStages(3);
        g_gxc.tev = COMBO_TEV;          /* any other state loads its own stages (apply_state) */
        g_combo.valid = 0;
    }
    if (!g_combo.valid || g_combo.c1 != c1) {
        if (!g_combo.valid)
            GX_LoadTexObj(&g_combo.tex, GX_TEXMAP1);
        g_combo.valid = 1;
        g_combo.c1 = c1;
        GX_SetTevKColor(GX_KCOLOR3, (GXColor){(u8)(c1 >> 24), (u8)(c1 >> 16), (u8)(c1 >> 8), (u8)c1});
    }
}

/* Both passes of a batch that has no display list (a crashed car's: its
 * vertices are deformed every frame) in one draw, as car_combo does for the
 * others: positions, colours and both textures' coordinates sent. As two
 * draws the crash scenes were the slowest frames there are. */
static uint32_t g_car_both;             /* status line */

static void car_pass_both(const NcOut *o)
{
    uint32_t i, k;
    for (i = 2; i < o->n;) {
        uint32_t first, last;
        if (o->adc[i]) {
            i++;
            continue;
        }
        first = i - 2;
        while (i < o->n && !o->adc[i])
            i++;
        last = i - 1;
        GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT3, (u16)(last - first + 1));
        for (k = first; k <= last; k++) {
            GX_Position3f32(o->pos[k][0], o->pos[k][1], o->pos[k][2]);
            GX_Color1u32((uint32_t)o->col[k][0] << 24 | (uint32_t)o->col[k][1] << 16 | (uint32_t)o->col[k][2] << 8 |
                         a2(o->col[k][3]));
            GX_TexCoord2f32(o->uv0[k][0], o->uv0[k][1]);
            GX_TexCoord2f32(o->uv1[k][0], o->uv1[k][1]);
        }
        GX_End();
    }
}

/* debugging (gxflags2 4): nc_shade_skin against nc_load and nc_shade for the first batches */
static void __attribute__((noinline)) car_skin_check(const uint32_t *vm, const NcBlock *b)
{
    static NcOut ref;
    static int checked, bad;
    uint32_t k, c;
    float dp = 0.0f, du = 0.0f;
    int dc = 0;
    if (checked >= 8000)
        return;
    nc_load(b, &g_nc_in);
    if (!nc_shade(vm, &g_nc_in, &ref))
        return;
    for (k = 0; k < b->n; k++) {
        for (c = 0; c < 3; c++)
            if (fabsf(ref.pos[k][c] - g_nc_out.pos[k][c]) > dp)
                dp = fabsf(ref.pos[k][c] - g_nc_out.pos[k][c]);
        for (c = 0; c < 4; c++) {
            int e = (int)ref.col[k][c] - (int)g_nc_out.col[k][c];
            if (e < 0) e = -e;
            if (e > dc) dc = e;
        }
        for (c = 0; c < 2; c++) {
            if (fabsf(ref.uv0[k][c] - g_nc_out.uv0[k][c]) > du) du = fabsf(ref.uv0[k][c] - g_nc_out.uv0[k][c]);
            if (ref.env && fabsf(ref.uv1[k][c] - g_nc_out.uv1[k][c]) > du) du = fabsf(ref.uv1[k][c] - g_nc_out.uv1[k][c]);
        }
        if (ref.adc[k] != g_nc_out.adc[k])
            dc = 999;
    }
    if (ref.n != g_nc_out.n || ref.env != g_nc_out.env || dp > 0.0005f || dc > 1 || du > 0.0005f) {
        if (bad++ < 12)
            LOG("car skin check: batch %08X (%u vertices): positions %g, colours %d, UVs %g apart", b->pos, b->n, (double)dp, dc,
                (double)du);
    }
    if (++checked == 400 || checked == 8000)
        LOG("car skin check: %d deformed batches compared with nc_shade, %d differ", checked, bad);
}

/* debugging: nc_shade_direct against nc_shade for the first batches */
static void __attribute__((noinline)) car_check(const uint32_t *vm, const NcBlock *b, const uint32_t *col, int env)
{
    static int checked, bad;
    uint32_t k;
    int diff = 0;
    if (checked >= 300)
        return;
    nc_load(b, &g_nc_in);
    if (!nc_shade(vm, &g_nc_in, &g_nc_out))
        return;
    for (k = 0; k < b->n; k++) {
        const uint8_t *c = g_nc_out.col[k];
        uint32_t want = (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | a2(c[3]);
        int apart = 0, sh;
        for (sh = 0; sh < 32; sh += 8) {
            /* a byte may be 1 off: the loop in paired singles adds in another order, and converts twice the alpha */
            const int e = (int)((want >> sh) & 0xFF) - (int)((col[k] >> sh) & 0xFF);
            apart |= e > 1 || e < -1;
        }
        if (apart || (env && memcmp(g_nc_out.uv1[k], g_car_uv1[k], 8)) ||
            g_nc_out.uv0[k][0] != (float)(LW(b->uv + 4 * car_uv_pair(b, k)) & 0xFFFF) / 4096.0f ||
            g_nc_out.uv0[k][1] != (float)(LW(b->uv + 4 * car_uv_pair(b, k)) >> 16) / 4096.0f) {
            if (bad < 8)
                LOG("car batch %08X vertex %u: direct %08X uv %g %g, nc_shade %08X uv %g %g", b->pos, k, col[k],
                    (double)g_car_uv1[k][0], (double)g_car_uv1[k][1], want, (double)g_nc_out.uv1[k][0],
                    (double)g_nc_out.uv1[k][1]);
            diff = 1;
        }
    }
    bad += diff;
    if (++checked == 300)
        LOG("gx: car display lists: 300 batches checked against nc_shade (%d differ)", bad);
}

/* pass 1 or 2 of a car batch through its display list */
static void car_pass_dl(const NcBlock *b, DList *dl, const uint32_t *col, int pass, uint32_t c1, uint8_t (*uv8)[2])
{
    uint32_t i, k, pos_skip = (b->pos - dl->pos) / 16;
    int fp;
    if (pass && uv8) {
        /* pass 2 through the list as well: the same strips, the UVs from the bytes the shading made */
        dl_touch(dl, dl->uvn * 8);
        GX_SetArray(GX_VA_POS, GPTR(dl->pos), 16);
        native_mode(NW_CAR_ENV_DL);
        GX_SetArray(GX_VA_CLR0, (void *)col, 4);
        GX_SetArray(GX_VA_TEX0, GPTR(dl->uv), 4);
        GX_SetArray(GX_VA_TEX1, (void *)uv8, 2);
        GX_SetChanMatColor(GX_COLOR0A0, (GXColor){(u8)(c1 >> 24), (u8)(c1 >> 16), (u8)(c1 >> 8), (u8)c1});
        for (fp = 0; fp < fix2_passes(); fp++) {
            fix2_set(fp);
            GX_CallDispList(dl->data, dl->size);
        }
        fix2_set(0);
        return;
    }
    dl_touch(dl, dl->uvn * 8);
    GX_SetArray(GX_VA_POS, GPTR(dl->pos), 16);
    if (!pass) {
        native_mode(NW_CAR_DL);
        GX_SetArray(GX_VA_CLR0, (void *)col, 4);
        GX_SetArray(GX_VA_TEX0, GPTR(dl->uv), 4);
        GX_SetArray(GX_VA_TEX1, GPTR(dl->uv), 4);
        for (fp = 0; fp < fix2_passes(); fp++) {
            fix2_set(fp);
            GX_CallDispList(dl->data, dl->size);
        }
        fix2_set(0);
        return;
    }
    native_mode(NW_CAR_ENV);
    GX_SetChanMatColor(GX_COLOR0A0, (GXColor){(u8)(c1 >> 24), (u8)(c1 >> 16), (u8)(c1 >> 8), (u8)c1});
    for (fp = 0; fp < fix2_passes(); fp++) {
        fix2_set(fp);
        for (i = 2; i < b->n;) {
            uint32_t first, last;
            if (LW(b->pos + 16 * i + 12) & 0x8000) {
                i++;
                continue;
            }
            first = i - 2;
            while (i < b->n && !(LW(b->pos + 16 * i + 12) & 0x8000))
                i++;
            last = i - 1;
            GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT5, (u16)(last - first + 1));
            for (k = first; k <= last; k++) {
                GX_Position1x8((u8)(k + pos_skip));
                if (g_gx_flags & 524288)      /* experiment: sphere map UVs pulled in */
                    GX_TexCoord2f32(0.5f + (g_car_uv1[k][0] - 0.5f) * 0.94f, 0.5f + (g_car_uv1[k][1] - 0.5f) * 0.94f);
                else
                    GX_TexCoord2f32(g_car_uv1[k][0], g_car_uv1[k][1]);
            }
            GX_End();
        }
    }
    fix2_set(0);
}

/* debugging (gxflags 8192): car batches whose draw (path, passes drawn,
 * their GS state, colours, reflection UVs) differs from the frame before but
 * equals the one two frames ago (A B A: flickering parts), logged by field */
typedef struct CarSig {
    uint32_t key, frame, c1, colsum, uvsum, n;
    uint32_t vk[52];                    /* hash of each VU1 constant qword 962..1013 */
    uint8_t env, path, drawn[2];
    uint64_t tex0[2], alpha[2], test[2], zbuf[2], frame_reg[2];
} CarSig;
typedef struct { CarSig cur, prev, prev2; } CarSigSlot;
static CarSigSlot *g_carsig;            /* 512, allocated when first used (MEM1 is tight) */
static CarSig g_cs;                     /* the batch being drawn */

/* TEX0 without TBP and CBP: textures move between VRAM slots every frame */
#define TEX0_NOADDR 0xFFF8001FFFFFC000ull

static int carsig_eq(const CarSig *a, const CarSig *b)
{
    return a->c1 == b->c1 && a->colsum == b->colsum && a->uvsum == b->uvsum && a->n == b->n && a->env == b->env &&
           a->path == b->path && !memcmp(a->drawn, b->drawn, 2) && ((a->tex0[0] ^ b->tex0[0]) & TEX0_NOADDR) == 0 &&
           ((a->tex0[1] ^ b->tex0[1]) & TEX0_NOADDR) == 0 && !memcmp(a->vk, b->vk, sizeof a->vk) &&
           !memcmp(a->alpha, b->alpha, sizeof a->alpha) && !memcmp(a->test, b->test, sizeof a->test) &&
           !memcmp(a->zbuf, b->zbuf, sizeof a->zbuf) && !memcmp(a->frame_reg, b->frame_reg, sizeof a->frame_reg);
}

static void __attribute__((noinline)) carsig_note(void)
{
    static int logged;
    uint32_t slot = (g_cs.key * 0x9E3779B1u) >> 23;
    CarSigSlot *e;
    if (!g_carsig && !(g_carsig = calloc(512, sizeof *g_carsig)))
        return;
    e = &g_carsig[slot];
    g_cs.frame = g_frame_no;
    if (e->cur.key != g_cs.key || e->cur.frame == g_frame_no) {
        if (e->cur.key != g_cs.key)
            memset(e, 0, sizeof *e);
        e->cur = g_cs;                  /* new, or drawn twice a frame: the last counts */
        return;
    }
    e->prev2 = e->cur.frame == e->prev.frame + 1 ? e->prev : (CarSig){0};
    e->prev = e->cur;
    e->cur = g_cs;
    if (e->prev.frame + 1 == g_frame_no && e->prev2.key && !carsig_eq(&e->cur, &e->prev) &&
        carsig_eq(&e->cur, &e->prev2) && logged < 60) {
        const CarSig *a = &e->prev, *b = &e->cur;
        int p;
        logged++;
        LOG("car flicker: batch %08X (%u verts) frame %u: path %u->%u env %u->%u drawn %u%u->%u%u c1 %08X->%08X "
            "colsum %08X->%08X uvsum %08X->%08X", b->key, b->n, g_frame_no, a->path, b->path, a->env, b->env,
            a->drawn[0], a->drawn[1], b->drawn[0], b->drawn[1], a->c1, b->c1, a->colsum, b->colsum, a->uvsum, b->uvsum);
        {
            char line[200];
            size_t m = 0;
            for (p = 0; p < 52 && m < 180; p++)
                if (a->vk[p] != b->vk[p])
                    m += (size_t)snprintf(line + m, sizeof line - m, " %d", 962 + p);
            if (m)
                LOG("  VU1 constants that differ:%s", line);
        }
        for (p = 0; p < 2; p++)
            if (((a->tex0[p] ^ b->tex0[p]) & TEX0_NOADDR) || a->alpha[p] != b->alpha[p] || a->test[p] != b->test[p] ||
                a->zbuf[p] != b->zbuf[p] || a->frame_reg[p] != b->frame_reg[p])
                LOG("  pass %d: tex0 %016llX->%016llX alpha %010llX->%010llX test %05llX->%05llX zbuf %llX->%llX "
                    "frame %llX->%llX", p + 1, (unsigned long long)a->tex0[p], (unsigned long long)b->tex0[p],
                    (unsigned long long)a->alpha[p], (unsigned long long)b->alpha[p], (unsigned long long)a->test[p],
                    (unsigned long long)b->test[p], (unsigned long long)a->zbuf[p], (unsigned long long)b->zbuf[p],
                    (unsigned long long)a->frame_reg[p], (unsigned long long)b->frame_reg[p]);
    }
}

/* debugging (gxflags 67108864): frames in which the nearest car part drawn to
 * the scene is not where it was the frame before (twice as far, half as far,
 * or none in front of the camera), with the frames around them: what the
 * draws were, and where the game has the player's car (0x57BAF0) */
typedef struct CarWatch {
    uint32_t frame, all, mine, front, deformed, draws;
    float z, x, y, pos[3], speed, up[3], vy, disp, disp_dy;
    uint32_t flags, disp_w;
    struct { uint32_t n[2], sum[2][2]; float last[2][8]; float mat[16]; } cam;
    struct { uint32_t n, code[6], mirrored; float y[6], slope[6], h; } gnd;
    uint32_t nz, nort;
    uint64_t zbuf, test;
} CarWatch;
static CarWatch g_cw, g_cw_ring[8];
static uint32_t g_car_combos;          /* status line: car batches drawn as one draw */
static uint32_t g_car_vtx[2], g_car_bat[2];    /* status line: car vertices shaded and batches, for the scene (0x8C0) and for other targets */
static uint32_t g_cw_at, g_cw_after, g_cw_events;
static int g_cw_mine;

static void cw_line(const CarWatch *w, const char *mark)
{
    LOG("carwatch %s depth state of the player's batches: %u kinds, last ZBUF %llX TEST %llX, %u without a target", mark,
        w->nz, (unsigned long long)w->zbuf, (unsigned long long)w->test, w->nort);
    LOG("carwatch %s cams: world %u sets (%08X..%08X) 1017 %.3f %.3f %.3f %.3f z %.4g %.4g w %.4f %.4f; car %u sets "
        "(%08X..%08X) 1017 %.3f %.3f %.3f %.3f z %.4g %.4g w %.4f %.4f", mark, w->cam.n[0], w->cam.sum[0][0],
        w->cam.sum[0][1], (double)w->cam.last[0][0], (double)w->cam.last[0][1], (double)w->cam.last[0][2],
        (double)w->cam.last[0][3], (double)w->cam.last[0][4], (double)w->cam.last[0][5], (double)w->cam.last[0][6],
        (double)w->cam.last[0][7], w->cam.n[1], w->cam.sum[1][0], w->cam.sum[1][1], (double)w->cam.last[1][0],
        (double)w->cam.last[1][1], (double)w->cam.last[1][2], (double)w->cam.last[1][3], (double)w->cam.last[1][4],
        (double)w->cam.last[1][5], (double)w->cam.last[1][6], (double)w->cam.last[1][7]);
    {
        char line[200];
        size_t m = 0;
        uint32_t k;
        line[0] = 0;
        for (k = 0; k < w->gnd.n; k++)
            m += (size_t)snprintf(line + m, sizeof line - m, " %+.3f (%05X, %.2f)", (double)(w->gnd.y[k] - w->pos[1]),
                                  w->gnd.code[k] & 0xFFFFF, (double)w->gnd.slope[k]);
        LOG("carwatch %s drawn ground from the car's height:%s; %u mirrored triangles over it (about y = %.2f)", mark, line,
            w->gnd.mirrored, (double)w->gnd.h);
    }
    {
        /* where the camera is: the point the matrix sends to x = y = w = 0 */
        const float *m = w->cam.mat;
        const float a[3][3] = { { m[0], m[4], m[8] }, { m[1], m[5], m[9] }, { m[3], m[7], m[11] } };
        const float r[3] = { -m[12], -m[13], -m[15] };
        const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                          a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        float c[3] = { 0.0f, 0.0f, 0.0f };
        const float fl = sqrtf(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
        if (det != 0.0f) {
            c[0] = (r[0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (r[1] * a[2][2] - a[1][2] * r[2]) +
                    a[0][2] * (r[1] * a[2][1] - a[1][1] * r[2])) / det;
            c[1] = (a[0][0] * (r[1] * a[2][2] - a[1][2] * r[2]) - r[0] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                    a[0][2] * (a[1][0] * r[2] - r[1] * a[2][0])) / det;
            c[2] = (a[0][0] * (a[1][1] * r[2] - r[1] * a[2][1]) - a[0][1] * (a[1][0] * r[2] - r[1] * a[2][0]) +
                    r[0] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;
        }
        LOG("carwatch %s camera at %.2f %.3f %.2f (from the car %.2f %.3f %.2f), looking %.3f %.3f %.3f", mark,
            (double)c[0], (double)c[1], (double)c[2], (double)(c[0] - w->pos[0]), (double)(c[1] - w->pos[1]),
            (double)(c[2] - w->pos[2]), (double)(fl > 0.0f ? m[3] / fl : 0.0f), (double)(fl > 0.0f ? m[7] / fl : 0.0f),
            (double)(fl > 0.0f ? m[11] / fl : 0.0f));
    }
    LOG("carwatch %s frame %u: %u car batches to the scene, %u the player's (%u deformed), %u in front, mean depth "
        "%.2f screen %.2f %.2f; player at %.2f %.3f %.2f speed %.2f vy %.2f up %.2f %.2f %.2f flags %08X %X; moved %.2f (dy %.2f, weights %08X); %u draws",
        mark, w->frame, w->all, w->mine, w->deformed, w->front, (double)w->z, (double)w->x, (double)w->y,
        (double)w->pos[0], (double)w->pos[1], (double)w->pos[2], (double)w->speed, (double)w->vy, (double)w->up[0],
        (double)w->up[1], (double)w->up[2], w->flags, (unsigned)LW(0x57BAF0 + 5320), (double)sqrtf(w->disp),
        (double)w->disp_dy, w->disp_w, w->draws);
}

static void __attribute__((noinline)) car_watch_frame(void)
{
    const CarWatch *p = &g_cw_ring[(g_cw_at + 7) & 7];
    const uint32_t mat = LW(0x57BAF0 + 516);
    int j, odd;
    g_cw.frame = g_frame_no;
    g_cw.draws = g_draw_no;
    {
        /* the frame's order, when it is not the usual one or around an event */
        static uint32_t usual, lines;
        uint32_t sig = g_cwrt.n, k;
        char line[200];
        size_t m = 0;
        for (k = 0; k < g_cwrt.n; k++) {
            uint32_t ph = g_cwrt.at[k] < g_cwrt.world[0] ? 0 : g_cwrt.at[k] <= g_cwrt.world[1] ? 1 :
                          g_cwrt.at[k] < g_cwrt.car[0] ? 2 : g_cwrt.at[k] <= g_cwrt.car[1] ? 3 : 4;
            sig = sig * 131 + g_cwrt.what[k] * 5 + ph;
            m += (size_t)snprintf(line + m, sizeof line - m, " %s%05X@%u", (g_cwrt.what[k] >> 20) == 1 ? "out " :
                                  (g_cwrt.what[k] >> 20) == 2 ? "restore " : "in ", g_cwrt.what[k] & 0xFFFFF, g_cwrt.at[k]);
            if (m > 170)
                break;
        }
        if (g_cw_after && lines < 200) {
            lines++;
            LOG("carwatch order frame %u%s: world %u..%u (%u), car %u..%u (%u);%s", g_frame_no, sig != usual ? " NEW" : "",
                g_cwrt.world[0], g_cwrt.world[1], g_cwrt.nworld, g_cwrt.car[0], g_cwrt.car[1], g_cwrt.ncar, line);
        }
        if (g_cwrt.ncar)
            usual = sig;
        memset(&g_cwrt, 0, sizeof g_cwrt);
    }
    memcpy(&g_cw.cam, &g_cwcam, sizeof g_cw.cam);
    memset(&g_cwcam, 0, sizeof g_cwcam);
    memcpy(&g_cw.gnd, &g_cwgnd, sizeof g_cw.gnd);
    memset(&g_cwgnd, 0, sizeof g_cwgnd);
    g_cw.speed = LWF(0x57BAF0 + 188);
    for (j = 0; j < 3; j++) {
        g_cw.pos[j] = mat && mat < PS2_RAM_SIZE - 64 ? LWF(mat + 48 + 4 * j) : 0.0f;
        g_cw.up[j] = mat && mat < PS2_RAM_SIZE - 64 ? LWF(mat + 16 + 4 * j) : 0.0f;
    }
    g_cw.vy = LWF(0x57BAF0 + 180);
    g_cw.flags = (uint32_t)LB(0x57BAF0 + 528) << 16 | (uint32_t)LB(0x57BAF0 + 529) << 8 | LB(0x57BAF0 + 533);
    if (g_cw.front) {
        g_cw.z /= (float)g_cw.front;
        g_cw.x /= (float)g_cw.front;
        g_cw.y /= (float)g_cw.front;
    }
    /* the player's batches: under half of the last frame's, or not before the camera as they were */
    odd = p->frame + 1 == g_frame_no && p->front >= 10 &&
          (g_cw.front * 2 < p->front || g_cw.z > 2.0f * p->z + 2.0f || g_cw.z < 0.4f * p->z ||
           fabsf(g_cw.x - p->x) > 0.2f || fabsf(g_cw.y - p->y) > 0.2f || p->speed - g_cw.speed > 1.0f ||
           g_cw.x < 0.3f || g_cw.x > 0.7f);
    if (odd && !g_cw_after && g_cw_events < 20) {
        g_cw_events++;
        for (j = 0; j < 8; j++)
            if (g_cw_ring[(g_cw_at + j) & 7].frame)
                cw_line(&g_cw_ring[(g_cw_at + j) & 7], "before");
        g_cw_after = 16;
    }
    if (g_cw_after) {
        cw_line(&g_cw, g_cw_after == 16 ? "ODD   " : "after ");
        g_cw_after--;
    }
    else if (g_frame_no % 600 == 0)
        cw_line(&g_cw, "sample");
    g_cw_ring[g_cw_at] = g_cw;
    g_cw_at = (g_cw_at + 1) & 7;
    memset(&g_cw, 0, sizeof g_cw);
}

/* A car batch (VU1 microprogram 46BB80) from its render data and the VU1
 * constants: pass 1 (GS context 1) lit, pass 2 (context 2) the reflection. */
static int car_skin_timed(const uint32_t *vm, const NcBlock *b)
{
    int r;
    NAT_TIME(1, r = nc_shade_skin(vm, b, &g_nc_out));
    return r;
}

/* "wstat 8": the cycles of a car batch's steps per frame (g_cst), and the batches by path (g_cst_n: combo with a
 * list, combo sent whole, one or two draws) */
uint64_t g_cst[12];
uint32_t g_cst_n[8];                    /* [4] GS packets */
#define CST(k) do { if (PS2_UNLIKELY(g_wstat == 8)) { const uint64_t n_ = gettime(); g_cst[k] += n_ - cst_t; cst_t = n_; } } while (0)

static int gs_backend_car_in(const NcBlock *b);

int gs_backend_car(const NcBlock *b)
{
    int r;
    g_ds_who = 2;
    r = gs_backend_car_in(b);
    g_ds_who = 0;
    return r;
}

static int gs_backend_car_in(const NcBlock *b)
{
    uint64_t cst_t = g_wstat == 8 ? gettime() : 0;
    int env_dl = 0;
    uint32_t *vm = g_vu_mem + 0xC000 / 4;
    float m[3][4];
    Mtx mtx;
    const GsDraw *d;
    RT *rt;
    int prev, pass, j, env;
    DList *dl = NULL;
    uint32_t *col = NULL, c1;
    uint8_t (*uv8)[2] = NULL;
    int combo = 0;
    if (!g_gx_ready || g_gx_bisect == 1) {
        g_fm_car_fail++;
        return 0;
    }
    if (g_gx_maxdraw && ++g_frame_draws > (uint32_t)g_gx_maxdraw)
        return 1;
    /* undeformed: a display list and a colour array (gxflags 2048: never) */
    if (!(vm[1013 * 4 + 2] & 0xFFFF) && !(g_gx_flags & 2048) && (dl = car_dl_get(b)) != NULL)
        col = litcol_alloc(b->n);
    if (col && !(vm[1013 * 4 + 1] & 0xFFFF) && !(g_gx_flags & (268435456 | 131072 | 524288))) {
        /* with the sphere map: its UVs as an array too, for one draw of both passes */
        uv8 = (uint8_t (*)[2])litcol_alloc((b->n + 1) / 2);
        combo = uv8 != NULL;
    }
    CST(0);
    {
        const int other = GS_FIELD(gs_reg(GS_FRAME_1), 0, 9) * 32 != 0x8C0;
        g_car_vtx[other] += b->n;
        g_car_bat[other]++;
    }
    prev = pt_enter(PT_NAT);
    if (col) {
        NAT_TIME(0, env = nc_shade_direct8(vm, b, col, g_car_uv1, uv8,
                                           (const int8_t *)((uint8_t *)dl->data + ((dl->size + 31) & ~31u))) == 2);
        if (!combo)
            car_check(vm, b, col, env);
        /* GX reads the arrays from memory, not from the CPU's cache */
        DCFlushRange(col, b->n * 4);
        if (combo)
            DCFlushRange(uv8, b->n * 2);
    } else if ((g_gx_flags2 & 8) || !car_skin_timed(vm, b)) {
        nc_load(b, &g_nc_in);
        if (!nc_shade(vm, &g_nc_in, &g_nc_out)) {
            pt_leave(prev);
            g_fm_car_fail++;
            return 0;
        }
        env = g_nc_out.env;
    } else {
        if (g_gx_flags2 & 4)
            car_skin_check(vm, b);
        env = g_nc_out.env;
    }
    c1 = (uint32_t)(u8)vm[1012 * 4] << 24 | (uint32_t)(u8)vm[1012 * 4 + 1] << 16 | (uint32_t)(u8)vm[1012 * 4 + 2] << 8 |
         a2((u8)vm[1012 * 4 + 3]);
    pt_leave(prev);
    CST(1);
    prev = pt_enter(PT_GX);
    if (vm[1006 * 4] & 0xFFFF) {
        /* the object's GS state (a GIF packet at 1006) goes first, once */
        uint32_t pk[16];
        memcpy(pk, &vm[1006 * 4], sizeof pk);
        pk[0] |= 0x8000;
        gif_xgkick(pk, 4, 0);
        vm[1006 * 4] = 0;
        g_cst_n[4]++;
    }
    CST(2);
    nc_matrix(vm, m);       /* (kept by its inputs it was no faster: an object's batches are few, the compare as dear) */
    for (j = 0; j < 4; j++) {
        mtx[0][j] = m[0][j];
        mtx[1][j] = m[1][j];
        mtx[2][j] = -m[2][j];
    }
    CST(3);
    g_cw_mine = 0;
    if (g_gx_flags & 67108864) {
        /* debugging (the car gone for a frame): the nearest car part drawn to
         * the scene, see car_watch_frame */
        if (GS_FIELD(gs_reg(GS_FRAME_1), 0, 9) * 32 == 0x8C0) {
            const uint32_t mat = LW(0x57BAF0 + 516);
            float d2 = 1e9f;
            g_cw.all++;
            if (mat && mat < PS2_RAM_SIZE - 64) {
                const float dx = BITS2F(vm[965 * 4]) - LWF(mat + 48), dy = BITS2F(vm[965 * 4 + 1]) - LWF(mat + 52),
                            dz = BITS2F(vm[965 * 4 + 2]) - LWF(mat + 56);
                d2 = dx * dx + dy * dy + dz * dz;
            }
            if (d2 < 16.0f) {
                g_cw_mine = 1;
                cw_cam_note(1, vm);
                if (!g_cwrt.ncar++)
                    g_cwrt.car[0] = g_draw_no;
                g_cwrt.car[1] = g_draw_no;
                g_cw.mine++;
                g_cw.deformed += !col;
                if (!col) {
                    /* how far the deformation moves the vertices of this batch */
                    uint32_t k, c;
                    for (k = 0; k < b->n; k++) {
                        float d = 0.0f;
                        for (c = 0; c < 3; c++) {
                            float e = g_nc_out.pos[k][c] - LWF(b->pos + 16 * k + 4 * c);
                            d += e * e;
                        }
                        if (d > g_cw.disp) {
                            static uint32_t lines;
                            const uint32_t wa = g_nc_in.q1[(k & ~3u) + 1][k & 3], wb = g_nc_in.q1[(k & ~3u) + 3][k & 3];
                            g_cw.disp = d;
                            g_cw.disp_dy = g_nc_out.pos[k][1] - LWF(b->pos + 16 * k + 4);
                            g_cw.disp_w = wa << 16 | wb;
                            if (d > 0.09f && lines < 40) {
                                const float *ma = (const float *)&vm[((974 + wa) & 1023) * 4];
                                lines++;
                                LOG("carwatch: batch %08X n %u vertex %u moved %.2f: weights %04X %04X (uvn %u step %u), "
                                    "matrix at %u: %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f",
                                    b->pos, b->n, k, (double)sqrtf(d), wa, wb, b->uvn, b->uvstep, (974 + wa) & 1023,
                                    (double)ma[0], (double)ma[1], (double)ma[2], (double)ma[4], (double)ma[5],
                                    (double)ma[6], (double)ma[8], (double)ma[9], (double)ma[10], (double)ma[12],
                                    (double)ma[13], (double)ma[14]);
                            }
                        }
                    }
                }
                if (m[2][3] > 0.0f) {
                    g_cw.front++;
                    g_cw.z += m[2][3];
                    g_cw.x += m[0][3] / m[2][3];
                    g_cw.y += m[1][3] / m[2][3];
                }
            }
        }
    }
    if (g_gx_flags & 8192) {
        uint32_t k, cs = 0, us = 0;
        memset(&g_cs, 0, sizeof g_cs);
        g_cs.key = b->pos;
        g_cs.n = b->n;
        g_cs.env = (uint8_t)env;
        g_cs.path = col != NULL;
        g_cs.c1 = c1;
        for (k = 0; k < b->n; k++) {
            cs = cs * 31 + (col ? col[k] : (uint32_t)g_nc_out.col[k][0] << 24 | (uint32_t)g_nc_out.col[k][1] << 16 |
                                               (uint32_t)g_nc_out.col[k][2] << 8 | g_nc_out.col[k][3]);
            if (env)
                us = us * 31 + F2BITS(col ? g_car_uv1[k][0] : g_nc_out.uv1[k][0]) +
                     3 * F2BITS(col ? g_car_uv1[k][1] : g_nc_out.uv1[k][1]);
        }
        g_cs.colsum = cs;
        g_cs.uvsum = us;
        for (k = 0; k < 52; k++) {
            const uint32_t *q = &vm[(962 + k) * 4];
            g_cs.vk[k] = ((q[0] * 31 + q[1]) * 31 + q[2]) * 31 + q[3];
        }
        g_cs.vk[1006 - 962] = 0;        /* the GS state packet: cleared once used */
    }
    if (combo && env) {
        d = gs_draw_state(0x05C);
        combo = car_combo_ok(d);
    } else {
        combo = 0;
    }
    CST(4);
    if (combo) {
        g_draw_no++;
        g_combo_call = 1;
        rt = draw_setup(d);
        g_combo_call = 0;
        CST(5);
        g_cst_n[0]++;
        if (rt) {
            g_fm_car++;
            g_car_combos++;
            car_combo_tev(c1);
            rt_drawn(rt, 0, 0, rt->w - 1, rt->h - 1);
            batch_flush();
            native_mode(NW_CAR_COMBO);
            native_projection(rt, d, 1.0f);
            CST(6);
            car_mtx_load(mtx);
            set_cull(GX_CULL_NONE);
            dl_touch(dl, dl->uvn * 8);
            GX_SetArray(GX_VA_POS, GPTR(dl->pos), 16);
            GX_SetArray(GX_VA_CLR0, (void *)col, 4);
            GX_SetArray(GX_VA_TEX0, GPTR(dl->uv), 4);
            GX_SetArray(GX_VA_TEX1, (void *)uv8, 2);
            GX_CallDispList(dl->data, dl->size);
            g_native_draws++;
            CST(7);
        } else {
            g_fm_car_skip++;
        }
        pt_leave(prev);
        return 1;
    }
    if (!col && env && !(g_gx_flags & (268435456 | 131072 | 524288))) {
        d = gs_draw_state(0x05C);
        if (car_combo_ok(d)) {
            g_draw_no++;
            g_combo_call = 1;
            rt = draw_setup(d);
            g_combo_call = 0;
            if (rt) {
                g_fm_car++;
                g_car_combos++;
                g_car_both++;
                car_combo_tev(c1);
                rt_drawn(rt, 0, 0, rt->w - 1, rt->h - 1);
                batch_flush();
                native_mode(NW_CAR_COMBO_IMM);
                native_projection(rt, d, 1.0f);
                car_mtx_load(mtx);
                set_cull(GX_CULL_NONE);
                car_pass_both(&g_nc_out);
                g_native_draws++;
            } else {
                g_fm_car_skip++;
            }
            pt_leave(prev);
            return 1;
        }
    }
    /* two draws after all: pass 2 goes through the list with the UV bytes (car_pass_dl), or sends them as floats */
    if (uv8 && env && g_opt_new && !(g_gx_flags & (524288 | 8192)))
        env_dl = 1;
    if (uv8 && env && !env_dl) {
        uint32_t k;
        for (k = 0; k < b->n; k++) {
            g_car_uv1[k][0] = (float)uv8[k][0] * (1.0f / 256.0f);
            g_car_uv1[k][1] = (float)uv8[k][1] * (1.0f / 256.0f);
        }
    }
    g_cst_n[col ? 2 + (env && !(g_gx_flags & 131072)) : 1]++;
    CST(9);
    for (pass = 0; pass < (env && !(g_gx_flags & 131072) ? 2 : 1); pass++) {
        d = gs_draw_state(pass ? 0x25C : 0x05C);
        g_draw_no++;
        rt = draw_setup(d);
        CST(10);
        if (!pass) {
            if (rt)
                g_fm_car++;
            else
                g_fm_car_skip++;
            if (g_cw_mine) {
                /* carwatch: the depth state the player's batches are drawn with */
                if (!g_cw.nz || g_cw.zbuf != d->zbuf || g_cw.test != d->test)
                    g_cw.nz++;
                g_cw.zbuf = d->zbuf;
                g_cw.test = d->test;
                g_cw.nort += !rt;
            }
        }
        if (g_gx_flags & 67108864) {
            /* carwatch: the different GS states the passes are drawn with */
            static struct { uint64_t alpha, test, zbuf; uint32_t tex, n; } seen[2][12];
            static uint32_t nseen[2];
            const uint32_t tex = (uint32_t)(GS_FIELD(d->tex0, 20, 6) | GS_FIELD(d->tex0, 34, 3) << 8 | GS_FIELD(d->tex0, 35, 2) << 12);
            uint32_t q;
            for (q = 0; q < nseen[pass]; q++)
                if (seen[pass][q].alpha == d->alpha && seen[pass][q].test == d->test && seen[pass][q].zbuf == d->zbuf &&
                    seen[pass][q].tex == tex)
                    break;
            if (q == nseen[pass] && q < 12) {
                nseen[pass]++;
                seen[pass][q].alpha = d->alpha;
                seen[pass][q].test = d->test;
                seen[pass][q].zbuf = d->zbuf;
                seen[pass][q].tex = tex;
                LOG("carwatch: car pass %d state %u: ALPHA %010llX TEST %05llX ZBUF %09llX tex psm %02X tcc %u tfx %u FBA %u "
                    "PABE %u COLCLAMP %u frame %08llX fbmsk %08X, vertex alpha c1 %02X", pass + 1, q, (unsigned long long)d->alpha,
                    (unsigned long long)d->test, (unsigned long long)d->zbuf, tex & 0x3F, (tex >> 8) & 1, tex >> 12,
                    (unsigned)(gs_reg(pass ? GS_FBA_2 : GS_FBA_1) & 1), (unsigned)(gs_reg(GS_PABE) & 1),
                    (unsigned)(gs_reg(GS_COLCLAMP) & 1), (unsigned long long)(d->frame & 0xFFFFFFFFu),
                    (unsigned)(d->frame >> 32), (unsigned)(c1 & 0xFF));
            }
        }
        if ((g_gx_flags & 8192) && pass) {
            /* the reflection's texture, once a frame: a render target? */
            static uint32_t logged_frame, lines;
            uint32_t tbp = GS_FIELD(d->tex0, 0, 14);
            RT *src = rt_for_texture(tbp);
            if (logged_frame != g_frame_no && lines < 90) {
                logged_frame = g_frame_no;
                lines++;
                if (src)
                    LOG("car env frame %u: tex %X %ux%u psm %02X from target %05X %dx%d (drawn %d..%d x %d..%d), valid %d "
                        "dirty %d resident %d, %u draws, pass %s", g_frame_no, tbp, 1u << GS_FIELD(d->tex0, 26, 4),
                        1u << GS_FIELD(d->tex0, 30, 4), (unsigned)GS_FIELD(d->tex0, 20, 6), src->fbp, src->w, src->h,
                        src->dx0, src->dx1, src->dy0, src->dy1, src->tex_valid, src->dirty, src->resident, src->draws,
                        rt ? "drawn" : "skipped");
                else
                    LOG("car env frame %u: tex %X %ux%u psm %02X (no target), pass %s", g_frame_no, tbp,
                        1u << GS_FIELD(d->tex0, 26, 4), 1u << GS_FIELD(d->tex0, 30, 4), (unsigned)GS_FIELD(d->tex0, 20, 6),
                        rt ? "drawn" : "skipped");
            }
        }
        if (g_gx_flags & 8192) {
            g_cs.drawn[pass] = rt != NULL;
            g_cs.tex0[pass] = d->tex0;
            g_cs.alpha[pass] = d->alpha;
            g_cs.test[pass] = d->test;
            g_cs.zbuf[pass] = d->zbuf;
            g_cs.frame_reg[pass] = d->frame;
        }
        if (!rt)
            continue;
        rt_drawn(rt, 0, 0, rt->w - 1, rt->h - 1);
        batch_flush();
        if (!col || !g_opt_new)
            native_mode(NW_CAR);        /* (car_pass_dl sets its own: two modes a pass were set before) */
        native_projection(rt, d, 1.0f);
        car_mtx_load(mtx);
        set_cull(GX_CULL_NONE);
        if (col)
            car_pass_dl(b, dl, col, pass, c1, env_dl ? uv8 : NULL);
        else
            car_pass(&g_nc_out, pass);
        g_native_draws++;
        CST(11);
    }
    if (g_gx_flags & 8192)
        carsig_note();
    pt_leave(prev);
    return 1;
}

/* ---- native post effects ------------------------------------------------------------------------ */

/* The game's bloom (after the haze, before the HUD) is a chain of PS2
 * tricks: a bright pass (Cs - Cd) x 2 over a buffer cleared to the
 * threshold, the scene in 64-pixel columns through buffers whose pages
 * interleave, downsampling to 160x112, ~20 self-blending blur passes,
 * accumulation with last frame's (Cd x FIX + Cs) and an additive composite;
 * then a radial speed blur. None of that maps onto GX, so its draws are
 * skipped (post_filter) and GX does the bloom directly at the end of the
 * chain (post_bloom): a box-filtered half-size copy of the scene, the
 * bright pass and blur in a small strip target, the accumulation with last
 * frame's, one additive full-screen quad. Then the speed blur (post_speed_blur).
 * Split-screen runs the chain once for each player's view, right after that
 * view (the scene's top half, then its bottom half: the bright pass reads
 * 640x224, the blur buffer is 160x56, the accumulation keeps one 160x56 strip
 * a view, the composite covers the view), and draws the next view's
 * reflections and scene in between. A chain is a view's: it is the rectangle
 * its bright pass reads, keeps an accumulation of its own, and ends at the
 * first draw that is not the chain's (post_filter). */
#define POST_FBP 0xFFFFF000u            /* the strip target the bloom works in (no GS address) */
#define POST_SETS 8                     /* chain buffers remembered (post_filter) */

static struct {
    int active;                         /* inside the game's chain: its draws are skipped */
    RT *scene;
    uint32_t threshold, decay;          /* bright pass level (0..255); accumulation (x/128) */
    void *half, *tmp, *acc[2][2];       /* acc: by view, then this frame's / the last */
    int acc_cur[2], acc_frames[2];
    int acc_key[2];                     /* the view each accumulation is of (post_bloom) */
    uint32_t chain_no;                  /* chains so far (the speed blur notes belong to one) */
    int vx0, vy0, vx1, vy1;             /* the scene rectangle the bright pass reads (GS pixels) */
    uint32_t set[POST_SETS], nset;      /* targets the chain drew into */
    uint32_t chains;                    /* status line */
    uint32_t blurs_alone;               /* status line: speed blurs with no bloom */
} g_post = {0, NULL, 0xBF, 0x3B, NULL, NULL, {{NULL, NULL}, {NULL, NULL}}, {0, 0}, {0, 0}, {-1, -1}, 0, 0, 0, 0, 0, {0}, 0, 0};

/* a post pass's strip target (fbp: one of no GS address), w x h GS pixels, at half that in the EFB at most: the bloom's
 * (a quarter of the view), the speed blur's tiles */
static RT *post_target(uint32_t fbp, int w, int h)
{
    RT *r = rt_find(fbp);
    if (r && (r->w != w || r->h != h)) {
        r->resident = 0;                /* (scratch: nothing to keep) placed anew at the new size */
        r->w = w;
        r->h = h;
    }
    if (!r) {
        int k, lru = -1;
        if (g_nrt < RT_MAX) {
            r = &g_rts[g_nrt++];
        } else {
            for (k = 0; k < g_nrt; k++)
                if (&g_rts[k] != g_big && (lru < 0 || g_rts[k].used < g_rts[lru].used))
                    lru = k;
            r = &g_rts[lru];
            if (r->resident) {
                rt_copy(r, 1);
                r->resident = 0;
            }
        }
        {
            void *t = r->tex, *a = r->tex_a8, *t2 = r->alt_tex;
            uint32_t tc = r->tex_cap, ac = r->a8_cap, tc2 = r->alt_cap, old = r->fbp;
            memset(r, 0, sizeof *r);
            r->tex = t; r->tex_a8 = a; r->tex_cap = tc; r->a8_cap = ac;
            r->alt_tex = t2; r->alt_cap = tc2;
            rt_fbp_set(r, old, fbp);
        }
        r->fbw = 3;
        r->w = w;
        r->h = h;
        r->minsh = 1;
        g_rt_pages_stale = 1;
        r->dx0 = r->dy0 = 1 << 30;
        r->dx1 = r->dy1 = -1;
    }
    r->used = g_frame_no;
    r->tex_valid = 0;                   /* scratch: nothing to draw back when it is placed again */
    rt_bind(r);
    r->dirty = 0;                       /* scratch: never copied out */
    return r;
}

static void *post_buf(void **p, int w, int h, u8 fmt)
{
    if (!*p)
        *p = gx_alloc(GX_GetTexBufferSize((u16)w, (u16)h, fmt, GX_FALSE, 0));
    return *p;
}

/* a textured quad over EFB pixels x0,y0-x1,y1 with the texture's u0,v0-u1,v1 */
static void post_quad(void *tex, int tw, int th, u8 fmt, float x0, float y0, float x1, float y1, float u0, float v0,
                      float u1, float v1)
{
    GXTexObj to;
    GX_InitTexObj(&to, tex, (u16)tw, (u16)th, fmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&to, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&to, GX_TEXMAP0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x0, y0, 0); GX_Normal3f32(u0, v0, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x1, y0, 0); GX_Normal3f32(u1, v0, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x1, y1, 0); GX_Normal3f32(u1, v1, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_Position3f32(x0, y1, 0); GX_Normal3f32(u0, v1, 1); GX_Color1u32(0xFFFFFFFF); GX_Color1u32(0xFFFFFFFF);
    GX_End();
}

/* TEV for the post passes: texture minus a level, times 2 (bright), or the
 * texture; then times a weight (k: 0..255); added to the target or replacing it */
static void post_tev(int bright, uint32_t level, uint32_t k, int add)
{
    GX_SetNumTevStages(2);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevKColor(GX_KCOLOR0, (GXColor){(u8)level, (u8)level, (u8)level, 255});
    GX_SetTevKColor(GX_KCOLOR1, (GXColor){(u8)k, (u8)k, (u8)k, 255});
    GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
    if (bright) {
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_KONST, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
    } else {
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);
    GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    if (add)
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_CLEAR);
    else
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
}

/* copy a target's EFB region (tw x th) to a texture */
static void post_copy(const RT *r, void *dst, u8 fmt)
{
    GX_SetTexCopySrc((u16)r->ex, (u16)r->ey, (u16)r->tw, (u16)r->th);
    GX_SetTexCopyDst((u16)r->tw, (u16)r->th, fmt, GX_FALSE);
    GX_CopyTex(dst, GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();
}

/* The game's radial speed blur, the last part of its chain (at speed): the
 * scene at half size into a buffer (320x224), a mask into the buffer's
 * alpha (sprites of a 32x32 radial ramp, mirrored about a centre, reaching
 * further in the faster the car), three passes blending the buffer onto
 * itself 50% at a slight zoom (each its own rectangle of UVs), then the
 * buffer over the scene through that alpha. Three 50% passes are eight
 * equally weighted zoomed copies: GX sums them in one pass (eight texture
 * coordinates, TEV sums of 1/8) at half size, as the PS2 draws its passes,
 * into a texture (speed_blur_sum), and draws that over a grid whose vertex
 * alpha is the mask; cells where the mask is zero are not drawn. */
#define SB_GX 16                        /* grid cells across */
#define SB_GY 14
static struct {
    uint32_t frame, half_fbp;           /* the chain it was seen in (g_post.chain_no), its buffer */
    float w, h;                         /* the part of the buffer used */
    int nzoom, nmask, comp;
    uint8_t comp_a;                     /* the composite sprite's alpha (0x80 = 1) */
    int cx0, cy0, cx1, cy1;             /* the scene rectangle the composite covers (a view) */
    float zoom[3][4];                   /* each pass: u0 v0 u1 v1 for 0..w, 0..h */
    GsVertex mv[64];                    /* the mask sprites */
    uint64_t mtex0, mtexa;
    uint32_t drawn;                     /* status line */
} g_sb;

/* a draw of the chain that belongs to the speed blur: noted */
static void speed_blur_note(const GsDraw *d, const GsVertex *v, int n)
{
    uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, tbp = GS_FIELD(d->tex0, 0, 14), tme = (d->prim >> 4) & 1;
    int k, x1 = 0, y1 = 0;
    if (!tme || !g_post.scene)
        return;
    for (k = 0; k < n; k++) {
        if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y > y1) y1 = v[k].y;
    }
    if (tbp == g_post.scene->fbp && fbp != tbp && x1 >= 300 * 16 && y1 >= 100 * 16 && x1 <= 330 * 16) {
        /* the scene into the buffer at half size: a new blur (a split-screen view's: 320x112) */
        g_sb.frame = g_post.chain_no;
        g_sb.half_fbp = fbp;
        g_sb.w = (float)x1 / 16.0f;
        g_sb.h = (float)y1 / 16.0f;
        g_sb.nzoom = g_sb.nmask = g_sb.comp = 0;
        return;
    }
    if (g_sb.frame != g_post.chain_no)
        return;
    if (fbp == g_sb.half_fbp && tbp == fbp && ((d->prim >> 8) & 1) && g_sb.nzoom < 3) {
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (k = 0; k < n; k++) {
            float u = (float)v[k].u / 16.0f, w = (float)v[k].v / 16.0f;
            if (u < u0) u0 = u;
            if (u > u1) u1 = u;
            if (w < v0) v0 = w;
            if (w > v1) v1 = w;
        }
        g_sb.zoom[g_sb.nzoom][0] = u0;
        g_sb.zoom[g_sb.nzoom][1] = v0;
        g_sb.zoom[g_sb.nzoom][2] = u1;
        g_sb.zoom[g_sb.nzoom][3] = v1;
        g_sb.nzoom++;
    } else if (fbp == g_sb.half_fbp && tbp != fbp && ((d->prim >> 8) & 1) && (1u << GS_FIELD(d->tex0, 26, 4)) <= 64) {
        g_sb.mtex0 = d->tex0;
        g_sb.mtexa = d->texa;
        for (k = 0; k < n && g_sb.nmask < 64; k++)
            g_sb.mv[g_sb.nmask++] = v[k];
    } else if (fbp == g_post.scene->fbp && tbp == g_sb.half_fbp) {
        /* the buffer over the scene: (Cs - Cd) As + Cd, As the mask times
         * this sprite's own alpha, which is how the game fades the blur with
         * speed (0 at a standstill: nothing to draw); over the view */
        int x0 = 1 << 30, y0 = 1 << 30;
        for (k = 0; k < n; k++) {
            if (v[k].x < x0) x0 = v[k].x;
            if (v[k].y < y0) y0 = v[k].y;
        }
        g_sb.comp = 1;
        g_sb.comp_a = n ? v[n - 1].a : 0;
        g_sb.cx0 = x0 >> 4;
        g_sb.cy0 = y0 >> 4;
        g_sb.cx1 = x1 >> 4;
        g_sb.cy1 = y1 >> 4;
    }
}

/* the mask at buffer point (x, y): the sprite over it, its UVs there, the
 * ramp's alpha (bilinear; GS 0x80 = 1) */
static float speed_blur_mask(const uint8_t *ramp, int rw, int rh, float x, float y)
{
    int k;
    for (k = 0; k + 1 < g_sb.nmask; k += 2) {
        const GsVertex *a = &g_sb.mv[k], *b = &g_sb.mv[k + 1];
        float ax = a->x / 16.0f, ay = a->y / 16.0f, bx = b->x / 16.0f, by = b->y / 16.0f;
        float x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax, y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
        if (x >= x0 && x <= x1 && y >= y0 && y <= y1 && x1 > x0 && y1 > y0) {
            float fx = (x - ax) / (bx - ax), fy = (y - ay) / (by - ay);
            float u = ((float)a->u + fx * ((float)b->u - (float)a->u)) / 16.0f - 0.5f;
            float t = ((float)a->v + fy * ((float)b->v - (float)a->v)) / 16.0f - 0.5f;
            int iu, it, iu1, it1;
            float du, dt, r;
            if (u < 0) u = 0;
            if (t < 0) t = 0;
            if (u > rw - 1) u = (float)(rw - 1);
            if (t > rh - 1) t = (float)(rh - 1);
            iu = (int)u;
            it = (int)t;
            du = u - (float)iu;
            dt = t - (float)it;
            iu1 = iu + 1 < rw ? iu + 1 : iu;
            it1 = it + 1 < rh ? it + 1 : it;
            r = (ramp[it * rw + iu] * (1 - du) + ramp[it * rw + iu1] * du) * (1 - dt) +
                (ramp[it1 * rw + iu] * (1 - du) + ramp[it1 * rw + iu1] * du) * dt;
            r /= 128.0f;
            return r > 1.0f ? 1.0f : r;
        }
    }
    return 0.0f;
}

/* the mask at every grid point, as speed_blur_mask has it (times the fade): the first sprite over a point decides
 * it, so each sprite in turn fills the points under it that none before it covered, with speed_blur_mask's
 * arithmetic on that one sprite (a point's sprites are tested in the same order; the grid's coordinates are the
 * same expressions). The first 200 grids are checked against speed_blur_mask point by point ("speed blur
 * grids checked" in the log). */
static uint32_t g_sbg_checked, g_sbg_bad;

static void speed_blur_grid(const uint8_t *ramp, int rw, int rh, float fade, float alpha[SB_GY + 1][SB_GX + 1])
{
    float gx[SB_GX + 1], gy[SB_GY + 1];
    uint8_t done[SB_GY + 1][SB_GX + 1];
    const GsVertex *mv = g_sb.mv;
    const int nmask = g_sb.nmask;
    int i, j, k;
    for (i = 0; i <= SB_GX; i++)
        gx[i] = g_sb.w * (float)i / SB_GX;
    for (j = 0; j <= SB_GY; j++)
        gy[j] = g_sb.h * (float)j / SB_GY;
    memset(done, 0, sizeof done);
    for (k = 0; k + 1 < nmask; k += 2) {
        const GsVertex *a = &mv[k], *b = &mv[k + 1];
        float ax = a->x / 16.0f, ay = a->y / 16.0f, bx = b->x / 16.0f, by = b->y / 16.0f;
        float x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax, y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
        int i0 = SB_GX + 1, i1 = -1, j0 = SB_GY + 1, j1 = -1;
        if (!(x1 > x0 && y1 > y0))
            continue;
        for (i = 0; i <= SB_GX; i++)
            if (gx[i] >= x0 && gx[i] <= x1) {
                if (i < i0) i0 = i;
                i1 = i;
            }
        for (j = 0; j <= SB_GY; j++)
            if (gy[j] >= y0 && gy[j] <= y1) {
                if (j < j0) j0 = j;
                j1 = j;
            }
        for (j = j0; j <= j1; j++)
            for (i = i0; i <= i1; i++) {
                float x = gx[i], y = gy[j];
                if (done[j][i] || !(x >= x0 && x <= x1 && y >= y0 && y <= y1))
                    continue;
                done[j][i] = 1;
                {
                    float fx = (x - ax) / (bx - ax), fy = (y - ay) / (by - ay);
                    float u = ((float)a->u + fx * ((float)b->u - (float)a->u)) / 16.0f - 0.5f;
                    float t = ((float)a->v + fy * ((float)b->v - (float)a->v)) / 16.0f - 0.5f;
                    int iu, it, iu1, it1;
                    float du, dt, r;
                    if (u < 0) u = 0;
                    if (t < 0) t = 0;
                    if (u > rw - 1) u = (float)(rw - 1);
                    if (t > rh - 1) t = (float)(rh - 1);
                    iu = (int)u;
                    it = (int)t;
                    du = u - (float)iu;
                    dt = t - (float)it;
                    iu1 = iu + 1 < rw ? iu + 1 : iu;
                    it1 = it + 1 < rh ? it + 1 : it;
                    r = (ramp[it * rw + iu] * (1 - du) + ramp[it * rw + iu1] * du) * (1 - dt) +
                        (ramp[it1 * rw + iu] * (1 - du) + ramp[it1 * rw + iu1] * du) * dt;
                    r /= 128.0f;
                    alpha[j][i] = fade * (r > 1.0f ? 1.0f : r);
                }
            }
    }
    for (j = 0; j <= SB_GY; j++)
        for (i = 0; i <= SB_GX; i++)
            if (!done[j][i])
                alpha[j][i] = fade * 0.0f;
    if (g_sbg_checked < 200) {
        uint32_t bad = 0;
        g_sbg_checked++;
        for (j = 0; j <= SB_GY; j++)
            for (i = 0; i <= SB_GX; i++) {
                const float want = fade * speed_blur_mask(ramp, rw, rh, g_sb.w * (float)i / SB_GX, g_sb.h * (float)j / SB_GY);
                bad += F2BITS(want) != F2BITS(alpha[j][i]);
            }
        if (bad && g_sbg_bad++ < 8)
            LOG("gx: speed blur grid %u: %u of %u points differ from the point by point mask", g_sbg_checked, bad,
                (SB_GX + 1) * (SB_GY + 1));
        if (g_sbg_checked == 200)
            LOG("gx: speed blur grids checked: 200, %u differed", g_sbg_bad);
    }
}

/* The ramp's GS memory as last read: the blocks under it (up to 64 of 64 words: 64x64 at 32 bits) and the CLUT
 * entries an indexed one looks up, so the ramp is read again only when one of them changed (exact: compared
 * whole). The mask sprites, the buffer's size and the fade the grid was made from, likewise. */
static struct {
    uint64_t tex0, texa;
    uint32_t nblk, blk[64], words[64 * 64], clut[256], nclut;     /* blk: less TBP */
    int valid;
    /* the grid's inputs */
    int gvalid, nmask, any;
    float w, h, fade;
    GsVertex mv[64];
    uint32_t ramp_reads, grids;         /* status line */
} g_sbc;

/* 1: the ramp under tex0 is what it was when last read. The game's texture cache puts it in the other half of GS
 * memory every frame: what the texels are made of is the shape (width, height, format, buffer width, how the
 * CLUT is read) and what the blocks hold in the same order (a texel's block, less TBP, does not depend on TBP),
 * not where they are. */
#define SB_TEX0_SHAPE (0xFFFFFull << 14 | 0x3FFull << 51)     /* TBW PSM TW TH, CPSM CSM CSA */
static int speed_blur_ramp_same(uint32_t rw, uint32_t rh)
{
    const uint64_t tex0 = g_sb.mtex0;
    const uint32_t tbp = GS_FIELD(tex0, 0, 14), tbw = GS_FIELD(tex0, 14, 6), psm = GS_FIELD(tex0, 20, 6);
    uint32_t blk[64], rel[64], nblk = 0, x, y, k, nclut = 0, cbase = 0;
    int same = g_sbc.valid && !((g_sbc.tex0 ^ tex0) & SB_TEX0_SHAPE) && g_sbc.texa == g_sb.mtexa;
    /* every block holds at least 8x8 texels: one sample per 8x8 finds them all, in a fixed order */
    for (y = 0; y < rh; y += 8)
        for (x = 0; x < rw; x += 8) {
            const uint32_t b = gs_block_of(psm, tbp, tbw, x, y);
            for (k = 0; k < nblk && blk[k] != b; k++)
                ;
            if (k == nblk) {
                if (nblk == 64) {
                    g_sbc.valid = 0;
                    return 0;
                }
                blk[nblk++] = b;
            }
        }
    if (psm == PSMT8 || psm == PSMT8H)
        nclut = 256;
    else if (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH) {
        nclut = 16;
        cbase = GS_FIELD(tex0, 51, 4) == 0 ? (GS_FIELD(tex0, 56, 5) & 15) * 16 : GS_FIELD(tex0, 56, 5) * 16;
    }
    for (k = 0; k < nblk; k++)
        rel[k] = (blk[k] - tbp) & 16383u;
    if (same && (nblk != g_sbc.nblk || memcmp(rel, g_sbc.blk, nblk * 4) || nclut != g_sbc.nclut))
        same = 0;
    for (k = 0; k < nblk; k++) {
        const uint32_t *src = &gs_mem[blk[k] * 64];
        if (same && memcmp(src, &g_sbc.words[k * 64], 256))
            same = 0;
        if (!same)
            memcpy(&g_sbc.words[k * 64], src, 256);
    }
    if (nclut)
        gs_clut_sync();
    for (k = 0; k < nclut; k++) {
        const uint32_t e = gs_clut[(cbase + k) & 511];
        if (same && e != g_sbc.clut[k])
            same = 0;
        g_sbc.clut[k] = e;
    }
    g_sbc.tex0 = tex0;
    g_sbc.texa = g_sb.mtexa;
    g_sbc.nblk = nblk;
    memcpy(g_sbc.blk, rel, nblk * 4);
    g_sbc.nclut = nclut;
    g_sbc.valid = 1;
    return same;
}

/* The eight copies' sum at half size, where the PS2 draws its three passes (its 320x224 buffer), for the composite to
 * take with one read a pixel: eight reads a pixel of a quarter of the view's area instead of eight of all of it. The
 * strip has room for a 160x80 target (SB_FBP; a 320-wide one would push the others out every frame), so the sum is
 * drawn there a tile at a time and each tile copied to its place in one texture (the copy's line stride is the
 * texture's width: a tile's rows land between the others'). RGBA8: the EFB has 6 bits a channel and keeps them that
 * way (RGB565 would take red and blue down to 5). The cells summed are the composite's and their neighbours (its
 * bilinear reach goes half a texel past a cell's edge). 0 when there is no room for the sum: the full-size pass then.
 * gxflags2 268435456: the full-size pass always. */
#define SB_FBP 0xFFFFE000u              /* the speed blur's tile target (no GS address) */
#define SB_TW 160                       /* its size in the EFB */
#define SB_TH 80
static void *g_sb_sum;                  /* RGBA8, up to BIG_W / 2 x BIG_H / 2, in MEM1 */
static uint32_t g_sb_tiles;             /* status line: tiles summed */

/* MEM1 from the arena, for good (32-byte aligned); NULL when there is none */
static void *mem1_take(uint32_t size)
{
    uint32_t level;
    uint8_t *lo;
    _CPU_ISR_Disable(level);
    lo = (uint8_t *)(((uintptr_t)SYS_GetArena1Lo() + 31) & ~(uintptr_t)31);
    if (lo + size > (uint8_t *)SYS_GetArena1Hi())
        lo = NULL;
    else
        SYS_SetArena1Lo(lo + size);
    _CPU_ISR_Restore(level);
    return lo;
}

static int speed_blur_sum(int sw, int sh, const float m[8][2], const float c[8][2], float alpha[SB_GY + 1][SB_GX + 1])
{
    const int hw = sw / 2, hh = sh / 2;
    const float rx = g_sb.w / (float)hw, ry = g_sb.h / (float)hh;
    uint8_t on[SB_GY][SB_GX], need[SB_GY][SB_GX];
    int i, j, k, tx, ty, a, b;
    RT *s;
    Mtx tm;
    GXTexObj to;
    if (!g_sb_sum) {
        static int tried;
        if (tried)
            return 0;
        tried = 1;
        g_sb_sum = mem1_take(GX_GetTexBufferSize(BIG_W / 2, BIG_H / 2, GX_TF_RGBA8, GX_FALSE, 0));
        if (!g_sb_sum) {
            LOG("speed blur: no room in MEM1 for the half-size sum: the eight copies are read at full size");
            return 0;
        }
    }
    for (j = 0; j < SB_GY; j++)
        for (i = 0; i < SB_GX; i++)
            on[j][i] = alpha[j][i] > 0.0f || alpha[j][i + 1] > 0.0f || alpha[j + 1][i] > 0.0f || alpha[j + 1][i + 1] > 0.0f;
    for (j = 0; j < SB_GY; j++)
        for (i = 0; i < SB_GX; i++) {
            int n = 0;
            for (b = j - 1; b <= j + 1; b++)
                for (a = i - 1; a <= i + 1; a++)
                    if (b >= 0 && b < SB_GY && a >= 0 && a < SB_GX)
                        n |= on[b][a];
            need[j][i] = (uint8_t)n;
        }
    /* (placed before any state is set: placing it may move other targets out) */
    s = post_target(SB_FBP, 2 * SB_TW, 2 * SB_TH);
    if (s->tw < SB_TW || s->th < SB_TH)
        return 0;
    ortho_mode();
    GX_SetNumTexGens(8);
    for (k = 0; k < 8; k++) {
        u32 mid = k ? GX_TEXMTX3 + 3 * (u32)(k - 1) : GX_TEXMTX0;
        GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + k), GX_TG_MTX2x4, GX_TG_POS, mid);
        GX_SetTevOrder((u8)(GX_TEVSTAGE0 + k), (u8)(GX_TEXCOORD0 + k), GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevKColorSel((u8)(GX_TEVSTAGE0 + k), GX_TEV_KCSEL_1_8);
        GX_SetTevColorIn((u8)(GX_TEVSTAGE0 + k), GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, k ? GX_CC_CPREV : GX_CC_ZERO);
        GX_SetTevColorOp((u8)(GX_TEVSTAGE0 + k), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn((u8)(GX_TEVSTAGE0 + k), GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GX_SetTevAlphaOp((u8)(GX_TEVSTAGE0 + k), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    GX_SetNumTevStages(8);
    GX_InitTexObj(&to, g_post.half, (u16)hw, (u16)hh, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&to, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&to, GX_TEXMAP0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    for (ty = 0; ty < hh; ty += SB_TH)
        for (tx = 0; tx < hw; tx += SB_TW) {
            const int w = hw - tx < SB_TW ? hw - tx : SB_TW, h = hh - ty < SB_TH ? hh - ty : SB_TH;
            int drawn = 0;
            for (j = 0; j < SB_GY; j++) {
                /* a cell in half-size texels, and where that is in the tile target */
                const float y0 = (float)hh * (float)j / SB_GY, y1 = (float)hh * (float)(j + 1) / SB_GY;
                if (y1 <= (float)ty || y0 >= (float)(ty + h))
                    continue;
                for (i = 0; i < SB_GX; i++) {
                    const float x0 = (float)hw * (float)i / SB_GX, x1 = (float)hw * (float)(i + 1) / SB_GX;
                    const float ox = (float)(s->ex - tx), oy = (float)(s->ey - ty);
                    if (!need[j][i] || x1 <= (float)tx || x0 >= (float)(tx + w))
                        continue;
                    if (!drawn) {
                        /* the tile's eight matrices: target pixel X is half-size texel X - ex + tx, the buffer's point
                         * that times rx (ry for Y), u = (c + m x) / hw */
                        for (k = 0; k < 8; k++) {
                            memset(tm, 0, sizeof tm);
                            tm[0][0] = m[k][0] * rx / (float)hw;
                            tm[0][3] = (c[k][0] - m[k][0] * rx * ox) / (float)hw;
                            tm[1][1] = m[k][1] * ry / (float)hh;
                            tm[1][3] = (c[k][1] - m[k][1] * ry * oy) / (float)hh;
                            GX_LoadTexMtxImm(tm, k ? GX_TEXMTX3 + 3 * (u32)(k - 1) : GX_TEXMTX0, GX_MTX2x4);
                        }
                        GX_SetScissor((u32)s->ex, (u32)s->ey, (u32)w, (u32)h);
                        drawn = 1;
                    }
                    GX_Begin(GX_QUADS, GX_VTXFMT7, 4);
                    GX_Position2f32(x0 + ox, y0 + oy);
                    GX_Color1u32(0xFFFFFFFFu);
                    GX_Position2f32(x1 + ox, y0 + oy);
                    GX_Color1u32(0xFFFFFFFFu);
                    GX_Position2f32(x1 + ox, y1 + oy);
                    GX_Color1u32(0xFFFFFFFFu);
                    GX_Position2f32(x0 + ox, y1 + oy);
                    GX_Color1u32(0xFFFFFFFFu);
                    GX_End();
                }
            }
            if (!drawn)
                continue;
            /* RGBA8 tiles are 4x4 texels of 64 bytes, a row of them hw * 16 bytes */
            GX_SetTexCopySrc((u16)s->ex, (u16)s->ey, (u16)w, (u16)h);
            GX_SetTexCopyDst((u16)hw, (u16)h, GX_TF_RGBA8, GX_FALSE);
            GX_CopyTex((uint8_t *)g_sb_sum + (uint32_t)ty * (uint32_t)hw * 4u + (uint32_t)tx * 16u, GX_FALSE);
            g_sb_tiles++;
        }
    GX_PixModeSync();
    GX_InvalidateTexAll();
    return 1;
}

static void post_speed_blur(RT *scene)
{
    static uint8_t ramp[64 * 64];
    static float alpha[SB_GY + 1][SB_GX + 1];
    uint32_t rw = 1u << GS_FIELD(g_sb.mtex0, 26, 4), rh = 1u << GS_FIELD(g_sb.mtex0, 30, 4), x, y;
    float m[8][2], c[8][2];
    int k, j, i, any = 0, ramp_same, half;
    Mtx tm;
    const float fade = g_sb.comp_a >= 0x80 ? 1.0f : (float)g_sb.comp_a / 128.0f;
    /* the view the composite covers (the whole screen but in split-screen), its half-size copy hw x hh */
    int sx0 = g_sb.cx0, sy0 = g_sb.cy0, sw = g_sb.cx1 - g_sb.cx0, sh = g_sb.cy1 - g_sb.cy0, hw, hh;
    if (sx0 < 0 || sy0 < 0 || sw < 64 || sh < 64 || sx0 + sw > BIG_W || sy0 + sh > BIG_H) {
        sx0 = sy0 = 0;
        sw = BIG_W;
        sh = BIG_H;
    }
    sw &= ~7;
    sh &= ~7;
    hw = sw / 2;
    hh = sh / 2;
    if (rw > 64 || rh > 64 || g_sb.nzoom != 3 || g_sb.nmask < 2 || g_sb.w < 8 || g_sb.h < 8 || !g_sb.comp_a)
        return;
    /* the ramp's alpha (read again only when its memory changed) */
    gs_sync_rect(GS_FIELD(g_sb.mtex0, 0, 14), GS_FIELD(g_sb.mtex0, 14, 6), GS_FIELD(g_sb.mtex0, 20, 6), 0, 0, rw, rh);
    ramp_same = speed_blur_ramp_same(rw, rh) && g_opt_new;
    g_sbc.ramp_reads += !ramp_same;
    if (!ramp_same)
        for (y = 0; y < rh; y++)
            for (x = 0; x < rw; x++)
                ramp[y * rw + x] = (uint8_t)(gs_texel_rgba(g_sb.mtex0, g_sb.mtexa, (int)x, (int)y) >> 24);
    /* the mask on the grid (buffer coordinates): the last frame's when nothing it is made from changed */
    if (ramp_same && g_sbc.gvalid && g_sbc.nmask == g_sb.nmask && g_sbc.w == g_sb.w && g_sbc.h == g_sb.h &&
        g_sbc.fade == fade && !memcmp(g_sbc.mv, g_sb.mv, (size_t)g_sb.nmask * sizeof g_sb.mv[0])) {
        any = g_sbc.any;
    } else if (!g_opt_new) {
        for (j = 0; j <= SB_GY; j++)
            for (i = 0; i <= SB_GX; i++) {
                alpha[j][i] = fade * speed_blur_mask(ramp, (int)rw, (int)rh, g_sb.w * (float)i / SB_GX,
                                                     g_sb.h * (float)j / SB_GY);
                any |= alpha[j][i] > 0.0f;
            }
        g_sbc.gvalid = 0;
    } else {
        speed_blur_grid(ramp, (int)rw, (int)rh, fade, alpha);
        g_sbc.grids++;
        for (j = 0; j <= SB_GY; j++)
            for (i = 0; i <= SB_GX; i++)
                any |= alpha[j][i] > 0.0f;
        g_sbc.gvalid = 1;
        g_sbc.nmask = g_sb.nmask;
        g_sbc.w = g_sb.w;
        g_sbc.h = g_sb.h;
        g_sbc.fade = fade;
        g_sbc.any = any;
        memcpy(g_sbc.mv, g_sb.mv, (size_t)g_sb.nmask * sizeof g_sb.mv[0]);
    }
    if (!any)
        return;
    /* the view as it is now (after the bloom) at half size */
    batch_flush();
    post_buf(&g_post.half, 320, 224, GX_TF_RGB565);
    GX_SetTexCopySrc((u16)(scene->ex + sx0), (u16)(scene->ey + sy0), (u16)sw, (u16)sh);
    GX_SetTexCopyDst((u16)hw, (u16)hh, GX_TF_RGB565, GX_TRUE);
    GX_CopyTex(g_post.half, GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();
    /* the eight copies: subsets of the three passes, the last pass applied
     * first (pass p maps buffer x to zoom[p].u0 + x (u1 - u0) / w) */
    for (k = 0; k < 8; k++) {
        float mx = 1.0f, cx = 0.0f, my = 1.0f, cy = 0.0f;
        int p;
        for (p = 2; p >= 0; p--)
            if (k & (1 << p)) {
                float sx = (g_sb.zoom[p][2] - g_sb.zoom[p][0]) / g_sb.w, sy = (g_sb.zoom[p][3] - g_sb.zoom[p][1]) / g_sb.h;
                cx = g_sb.zoom[p][0] + sx * cx;
                mx = sx * mx;
                cy = g_sb.zoom[p][1] + sy * cy;
                my = sy * my;
            }
        m[k][0] = mx;
        c[k][0] = cx;
        m[k][1] = my;
        c[k][1] = cy;
    }
    half = !(g_gx_flags2 & 268435456) && speed_blur_sum(sw, sh, m, c, alpha);
    ortho_mode();
    GX_SetNumTexGens(half ? 1 : 8);
    for (k = 0; k < (half ? 1 : 8); k++) {
        float bx = g_sb.w / (float)sw, by = g_sb.h / (float)sh;
        u32 mid = k ? GX_TEXMTX3 + 3 * (u32)(k - 1) : GX_TEXMTX0;
        memset(tm, 0, sizeof tm);
        if (half) {
            /* the sum: EFB pixel X -> half-size texel (X - ex - sx0) hw / sw -> u = (X - ex - sx0) / sw */
            tm[0][0] = 1.0f / (float)sw;
            tm[0][3] = -(float)(scene->ex + sx0) / (float)sw;
            tm[1][1] = 1.0f / (float)sh;
            tm[1][3] = -(float)(scene->ey + sy0) / (float)sh;
        } else {
            /* EFB pixel X -> buffer x = (X - ex - sx0) w / sw -> u = (c + m x) / hw */
            tm[0][0] = m[k][0] * bx / (float)hw;
            tm[0][3] = (c[k][0] - m[k][0] * bx * (float)(scene->ex + sx0)) / (float)hw;
            tm[1][1] = m[k][1] * by / (float)hh;
            tm[1][3] = (c[k][1] - m[k][1] * by * (float)(scene->ey + sy0)) / (float)hh;
        }
        GX_LoadTexMtxImm(tm, mid, GX_MTX2x4);
        GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + k), GX_TG_MTX2x4, GX_TG_POS, mid);
        GX_SetTevOrder((u8)(GX_TEVSTAGE0 + k), (u8)(GX_TEXCOORD0 + k), GX_TEXMAP0, k ? GX_COLORNULL : GX_COLOR0A0);
        GX_SetTevKColorSel((u8)(GX_TEVSTAGE0 + k), GX_TEV_KCSEL_1_8);
        if (half)
            GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        else
            GX_SetTevColorIn((u8)(GX_TEVSTAGE0 + k), GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, k ? GX_CC_CPREV : GX_CC_ZERO);
        GX_SetTevColorOp((u8)(GX_TEVSTAGE0 + k), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn((u8)(GX_TEVSTAGE0 + k), GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, k ? GX_CA_APREV : GX_CA_RASA);
        GX_SetTevAlphaOp((u8)(GX_TEVSTAGE0 + k), GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    GX_SetNumTevStages(half ? 1 : 8);
    {
        GXTexObj to;
        if (half)
            GX_InitTexObj(&to, g_sb_sum, (u16)hw, (u16)hh, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        else
            GX_InitTexObj(&to, g_post.half, (u16)hw, (u16)hh, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjLOD(&to, GX_LINEAR, GX_LINEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
        GX_LoadTexObj(&to, GX_TEXMAP0);
    }
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetScissor((u32)(scene->ex + sx0), (u32)(scene->ey + sy0), (u32)sw, (u32)sh);
    for (j = 0; j < SB_GY; j++) {
        float y0 = (float)(scene->ey + sy0) + (float)sh * (float)j / SB_GY;
        float y1 = (float)(scene->ey + sy0) + (float)sh * (float)(j + 1) / SB_GY;
        for (i = 0; i < SB_GX; i++) {
            float x0 = (float)(scene->ex + sx0) + (float)sw * (float)i / SB_GX;
            float x1 = (float)(scene->ex + sx0) + (float)sw * (float)(i + 1) / SB_GX;
            float a[4];
            a[0] = alpha[j][i];
            a[1] = alpha[j][i + 1];
            a[2] = alpha[j + 1][i + 1];
            a[3] = alpha[j + 1][i];
            if (a[0] <= 0.0f && a[1] <= 0.0f && a[2] <= 0.0f && a[3] <= 0.0f)
                continue;
            GX_Begin(GX_QUADS, GX_VTXFMT7, 4);
            GX_Position2f32(x0, y0);
            GX_Color1u32(0xFFFFFF00u | (u32)(a[0] * 255.0f + 0.5f));
            GX_Position2f32(x1, y0);
            GX_Color1u32(0xFFFFFF00u | (u32)(a[1] * 255.0f + 0.5f));
            GX_Position2f32(x1, y1);
            GX_Color1u32(0xFFFFFF00u | (u32)(a[2] * 255.0f + 0.5f));
            GX_Position2f32(x0, y1);
            GX_Color1u32(0xFFFFFF00u | (u32)(a[3] * 255.0f + 0.5f));
            GX_End();
        }
    }
    g_sb.drawn++;
    g_mark_frame[0] = g_frame_no;
    /* back to the state the other draws expect */
    GX_SetNumTexGens(1);
    for (k = 1; k < 8; k++)
        GX_SetTexCoordGen((u16)(GX_TEXCOORD0 + k), GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    g_native_mode = 1;
    ortho_mode();
}

static void post_bloom(RT *scene)
{
    static const float tap[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    RT *b;
    Mtx m;
    float x0, y0, x1, y1, du, dv;
    int k, it, w, h, vx0 = g_post.vx0, vy0 = g_post.vy0, vw = g_post.vx1 - g_post.vx0, vh = g_post.vy1 - g_post.vy0;
    int hw, hh, slot, key;
    static uint32_t last[2], logged;
    batch_flush();
    if (!scene || !scene->resident || !scene->big) {
        if (logged < 60) {
            logged++;
            LOG("bloom: none in frame %u: the scene is %s", g_frame_no, !scene ? "unknown" : !scene->big ? "small" : "not in the EFB");
        }
        return;
    }
    /* the view: the rectangle the bright pass read (the whole screen but in split-screen) */
    if (vx0 < 0 || vy0 < 0 || vw < 64 || vh < 64 || vx0 + vw > BIG_W || vy0 + vh > BIG_H) {
        vx0 = vy0 = 0;
        vw = BIG_W;
        vh = BIG_H;
    }
    vw &= ~7;
    vh &= ~7;
    hw = vw / 2;
    hh = vh / 2;
    slot = vy0 > 0;                     /* the lower view keeps the other accumulation */
    key = vx0 | vy0 << 10 | vw << 20;
    key = key * 31 + vh;
    if (g_post.acc_key[slot] != key) {
        g_post.acc_key[slot] = key;
        g_post.acc_frames[slot] = 0;
    }
    if (last[slot] && g_frame_no - last[slot] != 1 && logged < 60) {
        logged++;
        LOG("bloom: frame %u, the one before in frame %u (view at %d,%d %dx%d)", g_frame_no, last[slot], vx0, vy0, vw, vh);
    }
    last[slot] = g_frame_no;
    g_mark_frame[1] = g_frame_no;
    g_post.chains++;
    /* the view at half size, box filtered by the copy */
    post_buf(&g_post.half, 320, 224, GX_TF_RGB565);
    GX_SetTexCopySrc((u16)(scene->ex + vx0), (u16)(scene->ey + vy0), (u16)vw, (u16)vh);
    GX_SetTexCopyDst((u16)hw, (u16)hh, GX_TF_RGB565, GX_TRUE);
    GX_CopyTex(g_post.half, GX_FALSE);
    GX_PixModeSync();
    GX_InvalidateTexAll();

    b = post_target(POST_FBP, vw / 4, vh / 4);
    w = b->tw;
    h = b->th;
    /* (the buffers for the largest: a view of the whole screen, 80x56) */
    post_buf(&g_post.tmp, BIG_W / 8, BIG_H / 8, GX_TF_RGB565);
    post_buf(&g_post.acc[slot][0], BIG_W / 8, BIG_H / 8, GX_TF_RGB565);
    post_buf(&g_post.acc[slot][1], BIG_W / 8, BIG_H / 8, GX_TF_RGB565);
    ortho_mode();
    guMtxIdentity(m);
    GX_LoadPosMtxImm(m, GX_PNMTX0);
    GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX3x4);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_SetScissor((u32)b->ex, (u32)b->ey, (u32)w, (u32)h);
    x0 = (float)b->ex; y0 = (float)b->ey; x1 = x0 + (float)w; y1 = y0 + (float)h;

    /* bright pass, four bilinear taps (a 4x4 box of the half-size scene each) */
    du = 1.0f / (float)hw;
    dv = 1.0f / (float)hh;
    for (k = 0; k < 4; k++) {
        post_tev(1, g_post.threshold, 64, k > 0);
        post_quad(g_post.half, hw, hh, GX_TF_RGB565, x0, y0, x1, y1, tap[k][0] * du, tap[k][1] * dv,
                  1.0f + tap[k][0] * du, 1.0f + tap[k][1] * dv);
    }
    /* blur: twice four diagonal taps, one texel then two out */
    du = 1.0f / (float)w;
    dv = 1.0f / (float)h;
    for (it = 0; it < 2; it++) {
        post_copy(b, g_post.tmp, GX_TF_RGB565);
        for (k = 0; k < 4; k++) {
            post_tev(0, 0, 64, k > 0);
            post_quad(g_post.tmp, w, h, GX_TF_RGB565, x0, y0, x1, y1, tap[k][0] * du * (float)(it + 1),
                      tap[k][1] * dv * (float)(it + 1), 1.0f + tap[k][0] * du * (float)(it + 1),
                      1.0f + tap[k][1] * dv * (float)(it + 1));
        }
    }
    /* accumulation: plus the view's last frame's, decayed */
    if (g_post.acc_frames[slot]) {
        post_tev(0, 0, g_post.decay >= 0x80 ? 255 : g_post.decay * 2, 1);
        post_quad(g_post.acc[slot][g_post.acc_cur[slot]], w, h, GX_TF_RGB565, x0, y0, x1, y1, 0, 0, 1, 1);
    }
    g_post.acc_cur[slot] ^= 1;
    post_copy(b, g_post.acc[slot][g_post.acc_cur[slot]], GX_TF_RGB565);
    g_post.acc_frames[slot]++;

    /* composite: added to the view */
    GX_SetScissor((u32)(scene->ex + vx0), (u32)(scene->ey + vy0), (u32)vw, (u32)vh);
    post_tev(0, 0, 255, 1);
    post_quad(g_post.acc[slot][g_post.acc_cur[slot]], w, h, GX_TF_RGB565, (float)(scene->ex + vx0),
              (float)(scene->ey + vy0), (float)(scene->ex + vx0 + vw), (float)(scene->ey + vy0 + vh), 0, 0, 1, 1);
    if (g_sb.frame == g_post.chain_no && g_sb.comp && !(g_gx_flags & 65536))
        post_speed_blur(scene);
    scene->dirty = 1;
    scene->draws++;
    g_state_valid = 0;
    gxc_drop();
}

/* a chain's end: its bloom (and speed blur), or a speed blur that had no bloom before it */
static void post_end(void)
{
    RT *scene = g_post.scene;
    const int kind = g_post.active;
    g_post.active = 0;
    if (kind == 1) {
        post_bloom(scene);
        return;
    }
    batch_flush();
    if (!scene || !scene->resident || !scene->big || g_sb.frame != g_post.chain_no || !g_sb.comp ||
        (g_gx_flags & 65536))
        return;
    post_speed_blur(scene);
    scene->dirty = 1;
    scene->draws++;
    g_state_valid = 0;
    gxc_drop();
}

/* debugging (gxflags 16384): the draws of one frame's post chain (while it
 * is active, or textured from a render target), with what post_filter did */
static void __attribute__((noinline)) post_log(const GsDraw *d, int type, const GsVertex *v, int n, int skipped)
{
    static uint32_t frame = 0xFFFFFFFFu;
    static int lines, frames;
    uint32_t tbp = GS_FIELD(d->tex0, 0, 14), tme = (d->prim >> 4) & 1;
    int k, x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    float s0 = 1e9f, t0 = 1e9f, s1 = -1e9f, t1 = -1e9f;
    if (!g_post.active && !(tme && rt_find(tbp)) && !skipped)
        return;
    if (g_gx_flags & 32768) {
        /* only the speed blur: small textures in the chain (its mask), a
         * target drawn onto itself, and what follows a mask in that frame */
        static uint32_t mask_frame = 0xFFFFFFFFu;
        static int masks;
        int fbp = (int)(GS_FIELD(d->frame, 0, 9) * 32);
        int is_mask = tme && g_post.active && (1u << GS_FIELD(d->tex0, 26, 4)) <= 64;
        if (is_mask && mask_frame != g_frame_no) {
            if (masks >= 2)
                return;
            masks++;
            mask_frame = g_frame_no;
            LOG("post log: speed blur in frame %u", g_frame_no);
        }
        if (mask_frame != g_frame_no || (!is_mask && fbp != (int)tbp && !(tme && rt_find(tbp))))
            return;
        frame = g_frame_no;
        frames = 0;
        lines = 0;
    } else if (frame != g_frame_no) {
        if (frames >= 2)
            return;                     /* two frames' worth */
        if (lines)
            frames++;
        frame = g_frame_no;
        if (frames >= 2)
            return;
        LOG("post log: frame %u", g_frame_no);
    }
    if (lines >= 240)
        return;
    lines++;
    for (k = 0; k < n && k < 64; k++) {
        float q = v[k].q ? v[k].q : 1.0f;
        if (v[k].x < x0) x0 = v[k].x;
        if (v[k].x > x1) x1 = v[k].x;
        if (v[k].y < y0) y0 = v[k].y;
        if (v[k].y > y1) y1 = v[k].y;
        if ((d->prim >> 8) & 1) {
            if ((float)v[k].u < s0) s0 = (float)v[k].u / 16.0f;
            if ((float)v[k].u / 16.0f > s1) s1 = (float)v[k].u / 16.0f;
            if ((float)v[k].v / 16.0f < t0) t0 = (float)v[k].v / 16.0f;
            if ((float)v[k].v / 16.0f > t1) t1 = (float)v[k].v / 16.0f;
        } else {
            if (v[k].s / q < s0) s0 = v[k].s / q;
            if (v[k].s / q > s1) s1 = v[k].s / q;
            if (v[k].t / q < t0) t0 = v[k].t / q;
            if (v[k].t / q > t1) t1 = v[k].t / q;
        }
    }
    LOG("  %s t%d n%d prim %03X fbp %05X tbp %05X %ux%u alpha %010llX test %05llX xy %.1f,%.1f-%.1f,%.1f "
        "st %.3f,%.3f-%.3f,%.3f rgba %02X%02X%02X%02X%s", skipped ? "skip" : "draw", type, n, d->prim,
        (unsigned)(GS_FIELD(d->frame, 0, 9) * 32), tbp, 1u << GS_FIELD(d->tex0, 26, 4), 1u << GS_FIELD(d->tex0, 30, 4),
        (unsigned long long)d->alpha, (unsigned long long)d->test, x0 / 16.0, y0 / 16.0, x1 / 16.0, y1 / 16.0,
        (double)s0, (double)t0, (double)s1, (double)t1, v[0].r, v[0].g, v[0].b, v[0].a, g_post.active ? " (chain)" : "");
    if (GS_FIELD(d->frame, 32, 32))
        LOG("    fbmsk %08X tex0 %016llX tex1 %llX clamp %llX", (unsigned)GS_FIELD(d->frame, 32, 32),
            (unsigned long long)d->tex0, (unsigned long long)d->tex1, (unsigned long long)d->clamp);
    if (tme && (1u << GS_FIELD(d->tex0, 26, 4)) <= 64 && g_post.active) {
        /* small textures in the chain (masks): every vertex */
        char line[240];
        size_t m = 0;
        LOG("    tex0 %016llX tex1 %llX clamp %llX texa %llX", (unsigned long long)d->tex0, (unsigned long long)d->tex1,
            (unsigned long long)d->clamp, (unsigned long long)d->texa);
        {
            /* the texture, every 4th texel of every 4th row: alpha, then the colour */
            uint32_t tw = 1u << GS_FIELD(d->tex0, 26, 4), th = 1u << GS_FIELD(d->tex0, 30, 4), x, y;
            gs_sync_rect(tbp, GS_FIELD(d->tex0, 14, 6), GS_FIELD(d->tex0, 20, 6), 0, 0, tw, th);
            for (y = 0; y < th; y += th / 8) {
                m = 0;
                for (x = 0; x < tw; x += tw / 8)
                    m += (size_t)snprintf(line + m, sizeof line - m, " %08X",
                                          gs_texel_rgba(d->tex0, d->texa, (int)x, (int)y));
                LOG("    tex row %2u:%s", y, line);
            }
            m = 0;
        }
        for (k = 0; k < n && k < 64; k++) {
            m += (size_t)snprintf(line + m, sizeof line - m, " %.1f,%.1f/%.2f,%.2f", v[k].x / 16.0, v[k].y / 16.0,
                                  v[k].u / 16.0, v[k].v / 16.0);
            if (m > 180 || k == n - 1 || k == 63) {
                LOG("    v:%s", line);
                m = 0;
            }
        }
    }
}

/* The game's static strips as display lists (gs.h). The cars' sphere map is
 * made of the three reflection views by three strips of 446 vertices that
 * the game keeps ready in memory and sends every frame: a vertex at a time
 * that was 1300 triangles a frame through the triangle batch. A list is the
 * strip as emit_run would send it, made with the state the strip is drawn
 * with (put_vertex takes the Z format from it), and kept by address, content
 * and the registers its vertices depend on. gxflags 536870912: none. */
static struct {
    const uint32_t *q;
    uint32_t n, hash;
    uint32_t size;              /* of its list; 0: none yet (seen once, or changed) */
    uint32_t seen;              /* frame the strip was last sent in */
    uint32_t drawn;             /* frame the slot's buffer was last drawn from */
    uint64_t rgbaq, ofs;
    float zscale;
    int box[4];
} g_sdl[SDL_SLOTS];
static uint32_t g_sdl_draws, g_sdl_built, g_sdl_direct;

/* the strip's vertices as emit_run would send them; box: the pixels covered */
static void strip_emit(const uint32_t *q, uint32_t n, int *box)
{
    uint32_t k;
    box[0] = box[1] = 1 << 30;
    box[2] = box[3] = -(1 << 30);
    GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)n);
    for (k = 0; k < n; k++) {
        GsVertex v;
        BVert t;
        gs_strip_vertex(q, k, &v);
        put_vertex(&t, &v, &v);
        GX_Position3f32(t.x, t.y, t.z);
        GX_Normal3f32(t.s, t.t, t.q);
        GX_Color1u32(t.rgba);
        GX_Color1u32(t.fog);
        if ((v.x >> 4) < box[0]) box[0] = v.x >> 4;
        if ((v.y >> 4) < box[1]) box[1] = v.y >> 4;
        if (((v.x - 1) >> 4) > box[2]) box[2] = (v.x - 1) >> 4;
        if (((v.y - 1) >> 4) > box[3]) box[3] = (v.y - 1) >> 4;
    }
    GX_End();
}

int gs_backend_strip(const uint32_t *q, uint32_t n)
{
    const uint32_t prim = (uint32_t)gs_reg(GS_PRIM);
    const uint64_t rgbaq = gs_reg(GS_RGBAQ), ofs = gs_reg(GS_XYOFFSET_1);
    const GsDraw *d;
    uint32_t h = n, k, e;
    RT *rt;
    int fp, list;
    if (!g_gx_ready || g_gx_bisect == 1 || g_gx_maxdraw || g_post.active || !g_dl_arena || n > SDL_MAXV ||
        (g_gx_flags & (536870912 | 262144 | 16384 | 4194304)))
        return 0;
    for (k = 0; k < 4 * n; k++)
        h = (h << 5 | h >> 27) ^ q[k];
    d = gs_draw_state(prim);
    rt = draw_setup(d);
    if (!rt) {
        g_draw_no += n - 2;
        return 1;                       /* not drawn a vertex at a time either */
    }
    if (rt->soft || g_cur.fst || g_cur.fge)
        return 0;
    /* A slot a strip (address and length). A strip seen for the first time,
     * or changed, is sent as it is: the menus' change colour every frame and
     * the speed blur's is at another address every frame. One that comes
     * again unchanged in a later frame gets a list, once the GPU has left
     * what the slot's buffer held. */
    for (e = 0; e < SDL_SLOTS; e++)
        if (g_sdl[e].q == q && g_sdl[e].n == n)
            break;
    if (e < SDL_SLOTS && g_sdl[e].hash == h && g_sdl[e].rgbaq == rgbaq && g_sdl[e].ofs == ofs &&
        g_sdl[e].zscale == g_cur.zscale) {
        list = g_sdl[e].size != 0;
        if (!list && g_sdl[e].seen != g_frame_no && (!g_sdl[e].drawn || g_frame_no - g_sdl[e].drawn >= 3)) {
            uint8_t *buf = g_dl_arena + DL_SEGS * DL_SEG + e * SDL_CAP;
            g_sdl_built++;
            batch_flush();
            DCInvalidateRange(buf, SDL_CAP);
            GX_BeginDispList(buf, SDL_CAP);
            strip_emit(q, n, g_sdl[e].box);
            g_sdl[e].size = GX_EndDispList();
            DCFlushRange(buf, g_sdl[e].size);   /* the CP fetches the list from RAM */
            g_dl_syncs++;
            list = g_sdl[e].size != 0;
        }
    } else {
        if (e == SDL_SLOTS)
            for (e = 0, k = 1; k < SDL_SLOTS; k++)
                if (g_sdl[k].seen < g_sdl[e].seen)
                    e = k;
        g_sdl[e].q = q;
        g_sdl[e].n = n;
        g_sdl[e].hash = h;
        g_sdl[e].rgbaq = rgbaq;
        g_sdl[e].ofs = ofs;
        g_sdl[e].zscale = g_cur.zscale;
        g_sdl[e].size = 0;
        list = 0;
    }
    g_sdl[e].seen = g_frame_no;
    g_draw_no += n - 2;
    batch_flush();
    ortho_mode();
    if (list) {
        g_sdl_draws++;
        g_sdl[e].drawn = g_frame_no;
        rt_drawn(rt, g_sdl[e].box[0], g_sdl[e].box[1], g_sdl[e].box[2], g_sdl[e].box[3]);
        for (fp = 0; fp < fix2_passes(); fp++) {
            fix2_set(fp);
            GX_CallDispList(g_dl_arena + DL_SEGS * DL_SEG + e * SDL_CAP, g_sdl[e].size);
        }
    } else {
        int box[4];
        g_sdl_direct++;
        for (fp = 0; fp < fix2_passes(); fp++) {
            fix2_set(fp);
            strip_emit(q, n, box);
        }
        rt_drawn(rt, box[0], box[1], box[2], box[3]);
    }
    fix2_set(0);
    return 1;
}

/* The particles (VU1 0x46D090, gs.h): each trail segment's quads straight to
 * GX with the state the game set for them (additive, textured, the colour
 * the RGBAQ register has). Through the microprogram a long scrape was 7000
 * GS primitives a frame and took the frame rate from 30 to 20. */
static struct {
    uint32_t n[2], seg[2];              /* gxflags2 2: the last two segments with quads, the newest first */
    NsVert v[2][NS_MAX_QUADS * 4];
    uint32_t runs, bad, why[5], zmax;
} g_spk;
static uint32_t g_spark_quads;          /* status line */

/* a run's quads as they come from nw_spark_run: the state once, at the
 * first ones. Window x and y and the Z go to GX as they are (the GS would
 * cut them to 1/16 pixel and to whole Z steps first). */
typedef struct SparkDraw {
    uint32_t prim, rgba;
    RT *rt;
    int skip;
    float ofx, ofy, zk, zmax;
} SparkDraw;

/* the run's state, at its first primitive: 0 when there is nothing to draw into */
static int spark_setup(SparkDraw *sd)
{
    const GsDraw *d = gs_draw_state(sd->prim);
    const uint32_t zpsm = GS_FIELD(d->zbuf, 24, 4);
    sd->rt = draw_setup(d);
    if (!sd->rt || sd->rt->soft || g_cur.fst) {
        sd->skip = 1;
        return 0;
    }
    rt_drawn(sd->rt, 0, 0, sd->rt->w - 1, sd->rt->h - 1);
    batch_flush();
    ortho_mode();
    sd->ofx = (float)GS_FIELD(d->xyoffset, 0, 16) * (1.0f / 16.0f);
    sd->ofy = (float)GS_FIELD(d->xyoffset, 32, 16) * (1.0f / 16.0f);
    sd->zk = -g_cur.zscale * ZDS;
    sd->zmax = zpsm == 0 ? 4294967040.0f : zpsm == 1 ? 16777215.0f : 65535.0f;
    return 1;
}

static void spark_flush(const NsVert *q, uint32_t n, void *ctx)
{
    static const uint8_t order[4] = {0, 1, 3, 2};       /* the strip's vertices around the quad */
    SparkDraw *sd = ctx;
    uint32_t k, i;
    int fp;
    if (sd->skip)
        return;
    g_draw_no += 2 * n;
    if (!sd->rt && !spark_setup(sd))
        return;
    for (fp = 0; fp < fix2_passes(); fp++) {
        fix2_set(fp);
        GX_Begin(GX_QUADS, GX_VTXFMT0, (u16)(4 * n));
        for (k = 0; k < n; k++)
            for (i = 0; i < 4; i++) {
                const NsVert *v = &q[4 * k + order[i]];
                const float z = v->z < 0.0f ? 0.0f : v->z > sd->zmax ? sd->zmax : v->z;
                GX_Position3f32(v->x - sd->ofx, v->y - sd->ofy, z * sd->zk);
                GX_Normal3f32(v->s, v->t, 1.0f);
                GX_Color1u32(sd->rgba);
                GX_Color1u32(0xFFFFFFFFu);
            }
        GX_End();
    }
}

/* The same run as ribbons. A particle's trail is up to 17 quads end to end,
 * each pushed apart along its own segment's normal; as one strip with two
 * vertices a trail point (the normal there the sum of its two segments')
 * it is half the vertices and none of them twice, at 20 bytes each (no
 * colour: it is one register for the run). At a crash the game has 7000 to
 * 8000 of these quads a frame, 16 ms as quads. The picture differs where
 * two quads met at an angle (they overlapped there and left a notch on the
 * other side). gxflags2 16: as quads. -1: not drawn (the state needs the
 * quads' way). */
#define SPQ(n) ((const float *)(mem + (((n) & 1023u) << 2)))

static inline float spark_recip(float x)
{
    double e, d = x;
    float y;
    if (x == 0.0f)
        return 0.0f;
    __asm__("ps_res %0,%1" : "=f"(e) : "f"(d));
    y = (float)e;
    return y * (2.0f - x * y);
}

static inline void spark_strip_vertex(const SparkDraw *sd, const float *o, const float *sc, float qx, float qy, float qz,
                                      float u, float v)
{
    const float r = spark_recip(qz);
    float z = o[2] + o[3] * r;
    z = z < 0.0f ? 0.0f : z > sd->zmax ? sd->zmax : z;
    GX_Position3f32(o[0] + (qx * sc[2]) * r - sd->ofx, o[1] + (qy * sc[3]) * r - sd->ofy, z * sd->zk);
    GX_TexCoord2f32(u, v);
}

static uint32_t g_spark_seg_frame, g_spark_seg_now, g_spark_seg_prev;   /* trail segments drawn, this frame and the last */
static uint32_t g_spark_live_now, g_spark_live_prev;                    /* particles alive, the same */
static uint32_t g_spark_shed;           /* status line: particles alive and not drawn */
static uint32_t g_spark_shed_lvl = 1;   /* every how many of the particles is drawn */
static uint32_t g_spark_alive;          /* status line: trail segments alive (g_spark_quads: drawn) */
static uint32_t g_spark_cnt[4];         /* status line: emitter runs, particles in them, particles alive, points worked out */
#define SPARK_TOL 0.75f                 /* pixels a trail point may be from the drawn trail and be left out */
#define SPARK_SEG 40.0f                 /* the longest stretch drawn as one segment, in pixels */
/* gxflags2 262144: how far the points left out are from the trail as drawn, in pixels */
static uint32_t g_spark_dev_n;
static float g_spark_dev_sum, g_spark_dev_max;

/* a trail point: the particle t after its birth through the history's matrix me; 0 when it is in the view, else
 * the edges of the view it is beyond (a bit each) */
static inline uint32_t spark_point(const float *me, const float *p, const float *v, float h, float t, float *x, float *y,
                                   float *w)
{
    /* (a variable a lane: as arrays with loops over them the lanes went through memory, and the
     * comparisons of magnitudes through the integer registers) */
    const float t2 = t * t, g = h * t2;
    const float px = p[0] + v[0] * t, pz = p[2] + v[2] * t, pw = (p[3] + v[3] * t) + g;
    const float py0 = (p[1] + v[1] * t) + g, py = py0 < pw ? pw : py0;
    const float c0 = ((me[12] + me[0] * px) + me[4] * py) + me[8] * pz;
    const float c1 = ((me[13] + me[1] * px) + me[5] * py) + me[9] * pz;
    const float c2 = ((me[14] + me[2] * px) + me[6] * py) + me[10] * pz;
    const float c3 = ((me[15] + me[3] * px) + me[7] * py) + me[11] * pz;
    const float aw = __builtin_fabsf(c3);
    *x = c0 * me[16];
    *y = c1 * me[17];
    *w = c3;
    if (!(__builtin_fabsf(c0) > aw || __builtin_fabsf(c1) > aw || __builtin_fabsf(c2) > aw))
        return 0;
    return (c0 > aw ? 1u : 0u) | (c0 < -aw ? 2u : 0u) | (c1 > aw ? 4u : 0u) | (c1 < -aw ? 8u : 0u) |
           (c2 > aw ? 16u : 0u) | (c2 < -aw ? 32u : 0u) | 64u;
}

static int spark_strips(const uint32_t *mem, uint32_t top, uint32_t itop, SparkDraw *sd)
{
    const float *k = SPQ(913), *o = SPQ(914), *uv = SPQ(912), *m[18];
    const float life = (float)(int32_t)(itop & 0x3FF) * k[1], width = k[2], h = 0.5f * k[3];
    float tm[18], u[18];
    uint32_t nseg = mem[913 * 4] & 0xFFFF, ngrp = itop & 15, g, j, s, e, step = 1, shed;
    float tol, tol2, seg2;
    int total = 0, falling = 1;
    if (nseg > 17)
        nseg = 17;
    if (ngrp > 11)
        ngrp = 11;
    for (s = 0; s <= nseg; s++) {
        m[s] = SPQ(916 + 6 * s);
        tm[s] = m[s][20];
        u[s] = uv[0] + (SPQ(921)[0] - tm[s]) * uv[3];
        if (s && !(tm[s - 1] >= tm[s]))
            falling = 0;
    }
    g_spark_cnt[0]++;
    g_spark_cnt[1] += 4 * ngrp;
    /* Thousands of trail segments a frame (a heavy crash: 6500 alive on
     * average over ten seconds, 9000 in the worst, 5 ms of a frame even as
     * ribbons). They were thinned by count (every second, fourth or sixth
     * point when the frame before had so many alive), which left points up
     * to 76 pixels off the trail at a camera cut and 2 to 3 on average. Now
     * each trail is drawn to a tolerance (below), a sixth of the segments
     * at 0.75 pixels; when the frame before still drew thousands, the
     * tolerance is doubled, and again.
     *
     * A crash's heaviest frames have 1500 to 2000 particles alive and their
     * trails were still 3 to 4 ms of a frame that had 33 for everything and
     * took 38: with more than 900 alive in the frame before, every second
     * particle is drawn (by its place in its emitter, which it keeps for its
     * life), with more than 1800 every third. gxflags2 4194304: all of them. */
    if (g_spark_seg_frame != g_frame_no) {
        const int next = g_spark_seg_frame + 1 == g_frame_no;
        g_spark_seg_prev = next ? g_spark_seg_now : 0;
        g_spark_live_prev = next ? g_spark_live_now : 0;
        g_spark_seg_now = g_spark_live_now = 0;
        g_spark_seg_frame = g_frame_no;
        /* (and back to all of them at a count well below, or half the sparks would blink at the edge) */
        if (!next || (g_spark_shed_lvl == 2 && g_spark_live_prev < 700))
            g_spark_shed_lvl = 1;
        if (g_spark_shed_lvl == 3 && g_spark_live_prev < 1400)
            g_spark_shed_lvl = 2;
        if (g_spark_shed_lvl < 2 && g_spark_live_prev > 900)
            g_spark_shed_lvl = 2;
        if (g_spark_shed_lvl < 3 && g_spark_live_prev > 1800)
            g_spark_shed_lvl = 3;
    }
    shed = (g_gx_flags2 & 4194304) ? 1 : g_spark_shed_lvl;
    tol = g_spark_seg_prev > 2400 ? 8.0f * SPARK_TOL : g_spark_seg_prev > 1600 ? 4.0f * SPARK_TOL :
          g_spark_seg_prev > 800 ? 2.0f * SPARK_TOL : SPARK_TOL;
    tol2 = tol * tol;
    seg2 = (SPARK_SEG / SPARK_TOL) * (SPARK_SEG / SPARK_TOL) * tol2;
    for (g = 0; g < ngrp; g++) {
        const float *grp = SPQ(top + 9 * g);
        for (j = 0; j < 4; j++) {
            const float *p = grp + 4 + 8 * j, *v = p + 4;
            const float b = grp[j];
            float x[19], y[19], w[19], nx[19], ny[19], nz[19];
            uint8_t idx[19], in[19];
            uint32_t sa = nseg, sb = 0, np = 0, i;
            /* the segments it lives in: one stretch (the history's times fall). The first test is true from
             * some segment on and the second up to some segment, so a dead particle (most of an emitter's
             * are) is known by two compares, and the stretch's ends are found from either side. */
            if (falling && nseg) {
                if (!(tm[1] - b > 0.0f) || !(b - (tm[nseg - 1] - life) > 0.0f))
                    continue;
                for (sa = 0; !(b - (tm[sa] - life) > 0.0f); sa++)
                    ;
                for (s = nseg - 1; !(tm[s + 1] - b > 0.0f); s--)
                    ;
                if (sa > s)
                    continue;
                sb = s + 1;
            } else {
                for (s = 0; s < nseg; s++)
                    if (b - (tm[s] - life) > 0.0f && tm[s + 1] - b > 0.0f) {
                        if (sa == nseg)
                            sa = s;
                        sb = s + 1;
                    }
                if (sa == nseg)
                    continue;
            }
            g_spark_cnt[2]++;
            g_spark_live_now++;
            if (shed > 1 && (4 * g + j) % shed) {
                g_spark_shed++;
                continue;
            }
            g_spark_alive += sb - sa;
            {
                /* Most trails of a crash are far away and a few pixels long, and were up to 17 segments each
                 * all the same. The ends first, then the point in the middle of a stretch: when it is on the
                 * line between the stretch's ends (within SPARK_TOL pixels) and they are no further apart
                 * than SPARK_SEG, the stretch is one segment; else its halves in turn. A stretch with a
                 * point outside the view is halved too, unless its ends and middle are beyond the same
                 * edge (half the trails of a crash are off the screen). gxflags2 131072: every point. */
                float X[19], Y[19], W[19], SX[19], SY[19];
                uint8_t IN[19], OC[19], st[20][2];
                uint32_t have = 0, keep = 1u << sa | 1u << sb, sp = 0;
#define SP_PT(s_) do {                                                                              \
        if (!(have >> (s_) & 1)) {                                                                  \
            have |= 1u << (s_);                                                                     \
            g_spark_cnt[3]++;                                                                       \
            OC[s_] = (uint8_t)spark_point(m[s_], p, v, h, tm[s_] - b, &X[s_], &Y[s_], &W[s_]);      \
            IN[s_] = !OC[s_];                                                                       \
            if (IN[s_]) {                                                                           \
                const float r_ = spark_recip(W[s_]);                                                \
                SX[s_] = X[s_] * m[s_][18] * r_;                                                    \
                SY[s_] = Y[s_] * m[s_][19] * r_;                                                    \
            }                                                                                       \
        }                                                                                           \
    } while (0)
                SP_PT(sa);
                SP_PT(sb);
                if (g_gx_flags2 & 131072) {
                    for (s = sa; s < sb; s += step) {
                        SP_PT(s);
                        keep |= 1u << s;
                    }
                } else {
                    st[sp][0] = (uint8_t)sa;
                    st[sp++][1] = (uint8_t)sb;
                }
                while (sp) {
                    const uint32_t i0 = st[--sp][0], j0 = st[sp][1], mid = (i0 + j0) >> 1;
                    int split = 1;
                    if (j0 - i0 <= step)
                        continue;
                    if (IN[i0] && IN[j0]) {
                        /* a trail (or a stretch) of a couple of pixels: one segment, whatever its middle does */
                        const float ex = SX[j0] - SX[i0], ey = SY[j0] - SY[i0];
                        if (ex * ex + ey * ey <= 4.0f * tol2)
                            continue;
                    } else if (!IN[i0] && !IN[j0] && (OC[i0] & OC[j0] & 63)) {
                        continue;       /* both ends beyond the same edge of the view */
                    }
                    SP_PT(mid);
                    if (IN[i0] && IN[j0] && IN[mid]) {
                        const float ex = SX[j0] - SX[i0], ey = SY[j0] - SY[i0], qx = SX[mid] - SX[i0], qy = SY[mid] - SY[i0];
                        const float l2 = ex * ex + ey * ey, dt = qx * ex + qy * ey, cr = qx * ey - qy * ex;
                        if (l2 > seg2)
                            split = 1;
                        else if (dt <= 0.0f)
                            split = qx * qx + qy * qy > tol2;
                        else if (dt >= l2)
                            split = (qx - ex) * (qx - ex) + (qy - ey) * (qy - ey) > tol2;
                        else
                            split = cr * cr > tol2 * l2;
                    } else if (OC[i0] & OC[j0] & OC[mid] & 63) {
                        split = 0;      /* all three beyond the same edge of the view: so is the stretch */
                    }
                    if (split && sp + 2 <= 20) {
                        keep |= 1u << mid;
                        st[sp][0] = (uint8_t)i0;
                        st[sp++][1] = (uint8_t)mid;
                        st[sp][0] = (uint8_t)mid;
                        st[sp++][1] = (uint8_t)j0;
                    }
                }
#undef SP_PT
                for (s = sa; s <= sb; s++)
                    if (keep >> s & 1) {
                        idx[np] = (uint8_t)s;
                        x[np] = X[s];
                        y[np] = Y[s];
                        w[np] = W[s];
                        in[np] = IN[s];
                        np++;
                    }
                if (PS2_UNLIKELY(g_gx_flags2 & 262144) && np < sb - sa + 1)
                    for (i = 0; i + 1 < np; i++) {
                        const float *m0 = m[idx[i]], *m1 = m[idx[i + 1]];
                        const float ax = x[i] * m0[18] / w[i], ay = y[i] * m0[19] / w[i];
                        const float ex = x[i + 1] * m1[18] / w[i + 1] - ax, ey = y[i + 1] * m1[19] / w[i + 1] - ay;
                        const float l2 = ex * ex + ey * ey;
                        if (!in[i] || !in[i + 1])
                            continue;
                        for (s = idx[i] + 1u; s < idx[i + 1]; s++) {
                            float qx, qy, qw, t, d;
                            if (spark_point(m[s], p, v, h, tm[s] - b, &qx, &qy, &qw))
                                continue;
                            qx = qx * m[s][18] / qw - ax;
                            qy = qy * m[s][19] / qw - ay;
                            t = l2 > 0.0f ? (qx * ex + qy * ey) / l2 : 0.0f;
                            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                            d = sqrtf((qx - t * ex) * (qx - t * ex) + (qy - t * ey) * (qy - t * ey));
                            g_spark_dev_n++;
                            g_spark_dev_sum += d;
                            if (d > g_spark_dev_max)
                                g_spark_dev_max = d;
                        }
                    }
            }
            for (i = 0; i + 1 < np;) {
                uint32_t a0, z0;
                if (!in[i] || !in[i + 1]) {
                    i++;
                    continue;
                }
                if (!sd->rt) {
                    if (!spark_setup(sd))
                        return total;
                    if (fix2_passes() != 1)
                        return -1;
                    g_native_mode = NW_SPARK;   /* ortho_mode puts its own back */
                    GX_ClearVtxDesc();
                    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
                    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
                    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
                    GX_SetChanMatColor(GX_COLOR0A0, (GXColor){(u8)(sd->rgba >> 24), (u8)(sd->rgba >> 16),
                                                              (u8)(sd->rgba >> 8), (u8)sd->rgba});
                    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_TEX0, GX_TEXMTX0);
                }
                a0 = i;
                while (i + 1 < np && in[i] && in[i + 1]) {
                    const float dx = x[i + 1] - x[i], dy = y[i + 1] - y[i], dz = w[i + 1] - w[i];
                    nx[i] = dy * w[i + 1] - y[i + 1] * dz;
                    ny[i] = dz * x[i + 1] - w[i + 1] * dx;
                    nz[i] = dx * y[i + 1] - x[i + 1] * dy;
                    i++;
                }
                z0 = i;                         /* points a0..z0, segments a0..z0-1 */
                total += (int)(z0 - a0);
                g_spark_seg_now += z0 - a0;
                GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT3, (u16)(2 * (z0 - a0 + 1)));
                for (e = a0; e <= z0; e++) {
                    const uint32_t hi = idx[e];
                    float fx = 0.0f, fy = 0.0f, fz = 0.0f, q;
                    if (e > a0) {
                        fx += nx[e - 1];
                        fy += ny[e - 1];
                        fz += nz[e - 1];
                    }
                    if (e < z0) {
                        fx += nx[e];
                        fy += ny[e];
                        fz += nz[e];
                    }
                    q = width * rt_rsqrtf((fz * fz + fx * fx) + fy * fy);
                    fx *= q;
                    fy *= q;
                    fz *= q;
                    spark_strip_vertex(sd, o, m[hi] + 16, x[e] + fx, y[e] + fy, w[e] + fz, u[hi], uv[1]);
                    spark_strip_vertex(sd, o, m[hi] + 16, x[e] - fx, y[e] - fy, w[e] - fz, u[hi], uv[2]);
                }
                GX_End();
            }
        }
    }
    g_draw_no += 2 * (uint32_t)total;
    return total;
}

int gs_backend_sparks(const uint32_t *mem, uint32_t top, uint32_t itop, int first)
{
    static int native;
    static NsVert q[NS_MAX_QUADS * 4];
    const uint32_t tag_y = mem[915 * 4 + 1], prim = (tag_y >> 15) & 0x7FF;
    /* the tag: PRIM in it (a textured strip, no fog), PACKED, ST and XYZF2 four times */
    const int known = ((tag_y >> 14) & 1) && (prim & 0x37) == 0x14 && ((tag_y >> 26) & 3) == 0 && (tag_y >> 28) == 8 &&
                      mem[915 * 4 + 2] == 0x42424242u;
    uint32_t nseg = mem[913 * 4] & 0xFFFF, s, n;
    uint64_t rgbaq;
    if (nseg > 17)
        nseg = 17;
    if (g_gx_flags2 & 2) {
        /* checking: what the native path would send, kept for gs_backend_sparks_done */
        g_spk.n[0] = g_spk.n[1] = 0;
        for (s = 0; s < nseg; s++) {
            n = nw_spark_segment(mem, top, itop, s, q);
            if (!n)
                continue;
            g_spk.n[1] = g_spk.n[0];
            g_spk.seg[1] = g_spk.seg[0];
            memcpy(g_spk.v[1], g_spk.v[0], sizeof g_spk.v[0]);
            g_spk.n[0] = n;
            g_spk.seg[0] = s;
            memcpy(g_spk.v[0], q, sizeof g_spk.v[0]);
        }
    }
    if (first)
        native = g_gx_ready && g_gx_bisect != 1 && !g_gx_maxdraw && (g_native_render & NATIVE_WORLD) && !(g_gx_flags2 & 3) &&
                 known;
    if (!native)
        return 0;
    if (!known)
        return 1;                       /* in the middle of an emitter: nothing the microprogram could take over */
    gs_write_reg(GS_PRIM, prim);        /* as the tag does */
    {
        SparkDraw sd;
        int prev = pt_enter(PT_NAT);
        memset(&sd, 0, sizeof sd);
        sd.prim = prim;
        rgbaq = gs_reg(GS_RGBAQ);
        sd.rgba = (uint32_t)(uint8_t)rgbaq << 24 | (uint32_t)(uint8_t)(rgbaq >> 8) << 16 |
                  (uint32_t)(uint8_t)(rgbaq >> 16) << 8 | a2((uint8_t)(rgbaq >> 24));
        {
            int n2 = -1;
            if (!(g_gx_flags2 & 16))
                NAT_TIME(2, n2 = spark_strips(mem, top, itop, &sd));
            if (n2 < 0) {
                sd.skip = 0;
                n2 = (int)nw_spark_run(mem, top, itop, q, NS_MAX_QUADS, spark_flush, &sd);
                if (sd.rt)
                    fix2_set(0);
            }
            g_spark_quads += (uint32_t)n2;
            if (g_native_mode == NW_SPARK)
                ortho_mode();           /* the next draw of any kind finds the 2D state it expects */
        }
        pt_leave(prev);
    }
    return 1;
}

/* gxflags2 2: the microprogram's last two kicks are still in its buffers
 * (198 and 551 in turn; vi2: the next one's) */
void gs_backend_sparks_done(const uint32_t *mem, uint32_t vi2)
{
    uint32_t e;
    if (!(g_gx_flags2 & 2) || g_spk.runs >= 400)
        return;
    for (e = 0; e < 2 && g_spk.n[e]; e++) {
        const uint32_t base = e ? vi2 : 749 - vi2, n = mem[base * 4] & 0x7FFF;
        int32_t dmax[3] = {0, 0, 0};
        float smax = 0.0f;
        uint32_t k, i, c, adc = 0;
        if (base != 198 && base != 551)
            return;
        for (k = 0; k < n && k < g_spk.n[e]; k++)
            for (i = 0; i < 4; i++) {
                const NsVert *q = &g_spk.v[e][4 * k + i];
                const uint32_t *st = &mem[(base + 1 + 8 * k + 2 * i) * 4], *xyz = st + 4;
                const int32_t want[3] = {(int32_t)(q->x * 16.0f), (int32_t)(q->y * 16.0f), (int32_t)(q->z * 16.0f)};
                for (c = 0; c < 3; c++) {
                    int32_t dd = (int32_t)xyz[c] - want[c];
                    if (dd < 0) dd = -dd;
                    if (dd > dmax[c]) dmax[c] = dd;
                }
                if (fabsf(BITS2F(st[0]) - q->s) > smax) smax = fabsf(BITS2F(st[0]) - q->s);
                if (fabsf(BITS2F(st[1]) - q->t) > smax) smax = fabsf(BITS2F(st[1]) - q->t);
                adc += ((xyz[3] >> 15) & 1) != (i < 2);
            }
        if (n != g_spk.n[e] || dmax[0] > 16 || dmax[1] > 16 || dmax[2] > 4096 || smax > 0.001f || adc)
            g_spk.bad++;
        g_spk.why[0] += n != g_spk.n[e];
        g_spk.why[1] += dmax[0] > 16 || dmax[1] > 16;
        g_spk.why[2] += dmax[2] > 4096;
        g_spk.why[3] += smax > 0.001f;
        g_spk.why[4] += adc != 0;
        if (dmax[2] > (int32_t)g_spk.zmax)
            g_spk.zmax = (uint32_t)dmax[2];
        if (g_spk.runs < 24 || (g_spk.bad && g_spk.bad < 24 && (n != g_spk.n[e] || dmax[0] > 16 || dmax[1] > 16)))
            LOG("sparks check: segment %u at %u: %u quads native, %u from the microprogram; most apart x %d y %d (1/16 "
                "pixel) z %d (1/16), st %g, %u ADC bits differ", g_spk.seg[e], base, g_spk.n[e], n, (int)dmax[0],
                (int)dmax[1], (int)dmax[2], (double)smax, adc);
    }
    if (g_spk.n[0] && ++g_spk.runs == 400)
        LOG("sparks check: 400 runs with quads compared, %u kicks differ (count %u, x or y over a pixel %u, z over 256 %u, st "
            "%u, ADC %u; z at most %u/16 apart)", g_spk.bad, g_spk.why[0], g_spk.why[1], g_spk.why[2], g_spk.why[3],
            g_spk.why[4], g_spk.zmax);
}

/* The particle systems (VU1 0x46D750, gs.h): a batch's quads straight to GX
 * with the state the pool's header carries, which the microprogram sends
 * with a pool's first packet (a pool none of whose particles is drawn sends
 * nothing). Smoke is 5 to 9 runs of it a frame, a crash 18 and more. */
static uint32_t g_puff_quads, g_puff_vu;        /* status line: quads drawn here, batches the microprogram ran */

static void puff_draw(uint32_t prim, const NpVert *q, uint32_t n)
{
    static const uint8_t order[4] = {0, 1, 3, 2};       /* the strip's vertices around the quad */
    const GsDraw *d = gs_draw_state(prim);
    const uint32_t zpsm = GS_FIELD(d->zbuf, 24, 4);
    RT *rt = draw_setup(d);
    float ofx, ofy, zk, zmax;
    uint32_t k, i;
    int fp;
    g_draw_no += 2 * n;
    if (!rt || rt->soft || g_cur.fst)
        return;
    rt_drawn(rt, 0, 0, rt->w - 1, rt->h - 1);
    batch_flush();
    ortho_mode();
    ofx = (float)GS_FIELD(d->xyoffset, 0, 16) * (1.0f / 16.0f);
    ofy = (float)GS_FIELD(d->xyoffset, 32, 16) * (1.0f / 16.0f);
    zk = -g_cur.zscale * ZDS;
    zmax = zpsm == 0 || zpsm == 1 ? 16777215.0f : 65535.0f;     /* XYZF2: 24 bits of it */
    for (fp = 0; fp < fix2_passes(); fp++) {
        fix2_set(fp);
        GX_Begin(GX_QUADS, GX_VTXFMT0, (u16)(4 * n));
        for (k = 0; k < n; k++)
            for (i = 0; i < 4; i++) {
                const NpVert *v = &q[4 * k + order[i]];
                const float z = v->z < 0.0f ? 0.0f : v->z > zmax ? zmax : v->z;
                GX_Position3f32(v->x - ofx, v->y - ofy, z * zk);
                GX_Normal3f32(v->s, v->t, 1.0f);
                GX_Color1u32((v->rgba & 0xFFFFFF00u) | a2(v->rgba & 0xFF));
                GX_Color1u32(0xFFFFFFFFu);
            }
        GX_End();
    }
    fix2_set(0);
}

/* gxflags2 512: what the native path would draw, in a ring until the
 * microprogram kicks its packets (they span batches) */
#define PFC_RING 128
static struct {
    NpVert v[PFC_RING * 4];
    NpPool pool;
    uint32_t head, n, skip, type;
    uint32_t pools, kicks, quads, bad, why[6], wraps, detail, dxy, dz, dcol, kind[6];
    float dst;
} g_pfc;

static void puff_check_batch(const uint32_t *mem, uint32_t top, uint32_t n)
{
    static NpVert q[NP_BATCH * 4];
    uint32_t nq, k;
    if (g_pfc.skip)
        return;
    nq = nw_puff_batch(mem, top, n > NP_BATCH ? NP_BATCH : n, &g_pfc.pool, q);
    for (k = 0; k < nq; k++) {
        if (g_pfc.n == PFC_RING) {
            g_pfc.why[0]++;
            break;
        }
        memcpy(&g_pfc.v[4 * ((g_pfc.head + g_pfc.n++) % PFC_RING)], &q[4 * k], 4 * sizeof q[0]);
    }
}

static void puff_check_line(void)
{
    LOG("puffs check: %u pools, %u packets, %u quads compared, %u differ (count %u, x or y over 1/4 pixel %u, z over 256 %u, "
        "st %u, colour over 2 %u, ADC %u); most apart x or y %u/16, z %u/16, st %g, colour %u; %u colours over 255 from "
        "the microprogram; of kind 0 %u, 2 %u, 4 %u, 5 %u", g_pfc.pools, g_pfc.kicks, g_pfc.quads, g_pfc.bad, g_pfc.why[0],
        g_pfc.why[1], g_pfc.why[2], g_pfc.why[3], g_pfc.why[4], g_pfc.why[5], g_pfc.dxy, g_pfc.dz, (double)g_pfc.dst,
        g_pfc.dcol, g_pfc.wraps, g_pfc.kind[0], g_pfc.kind[2], g_pfc.kind[4], g_pfc.kind[5]);
}

/* The embers (VU1 0x47C6F0, gs.h): a batch's sprites with the state the game
 * set for them. A crash has some 40 runs of 80 particles a frame.
 * gxflags2 2048: the microprogram runs and its packet (one a run: XYZF2, ST,
 * RGBAQ, XYZF2 a sprite) is compared with the native path's. */
static uint32_t g_ember_quads, g_ember_vu;      /* status line */
static struct {
    NpVert v[NE_GROUPS * 4 * 4];
    uint32_t n, pending, skip;
    uint32_t runs, kicks, quads, bad, why[4], detail, dxy, dz, dcol;
} g_emc;

static void ember_check_kick(const uint32_t *mem, uint32_t qaddr)
{
    const uint32_t *q = mem + 4 * (qaddr & 1023), cnt = q[0] & 0x7FFF;
    uint32_t k, j, c;
    if (!(g_gx_flags2 & 2048) || g_emc.skip)
        return;
    g_emc.kicks++;
    if (!g_emc.pending || cnt != g_emc.n || (q[1] >> 28) != 4) {
        g_emc.why[0]++;
        g_emc.bad += cnt;
        if (g_emc.detail < 12) {
            g_emc.detail++;
            LOG("embers check: a packet of %u sprites, %u from the native path%s", cnt, g_emc.n, g_emc.pending ? "" : " (taken)");
        }
        g_emc.pending = 0;
        return;
    }
    g_emc.pending = 0;
    for (k = 0; k < cnt; k++) {
        const uint32_t *it = mem + 4 * ((qaddr + 1 + 4 * k) & 1023);
        const NpVert *w = &g_emc.v[4 * k];
        uint32_t dxy = 0, dz = 0, dcol = 0;
        for (j = 0; j < 2; j++) {
            const uint32_t *xyz = it + 12 * j;
            const NpVert *o = &w[3 * j];
            const int32_t want[3] = {(int32_t)(o->x * 16.0f), (int32_t)(o->y * 16.0f), (int32_t)(o->z * 16.0f)};
            for (c = 0; c < 3; c++) {
                int32_t dd = (int32_t)xyz[c] - want[c];
                if (dd < 0)
                    dd = -dd;
                if (c < 2 && (uint32_t)dd > dxy)
                    dxy = (uint32_t)dd;
                if (c == 2 && j && (uint32_t)dd > dz)
                    dz = (uint32_t)dd;
            }
        }
        for (c = 0; c < 4; c++) {
            const uint32_t raw = it[8 + c], have = (w->rgba >> (24 - 8 * c)) & 0xFF;
            int32_t dd = (int32_t)raw - (int32_t)have;
            if (raw > 255)
                continue;
            if (dd < 0)
                dd = -dd;
            if ((uint32_t)dd > dcol)
                dcol = (uint32_t)dd;
        }
        g_emc.quads++;
        g_emc.why[1] += dxy > 4;
        g_emc.why[2] += dz > 4096;
        g_emc.why[3] += dcol > 2;
        if (dxy > g_emc.dxy) g_emc.dxy = dxy;
        if (dz > g_emc.dz) g_emc.dz = dz;
        if (dcol > g_emc.dcol) g_emc.dcol = dcol;
        if (dxy > 4 || dz > 4096 || dcol > 2) {
            g_emc.bad++;
            if (g_emc.detail < 12) {
                g_emc.detail++;
                LOG("embers check: sprite %u of %u: x or y %u/16, z %u/16, colour %u apart: native %.2f %.2f to %.2f %.2f z %.1f "
                    "rgba %08X; the microprogram's %.2f %.2f to %.2f %.2f z %.1f rgba %X %X %X %X", k, cnt, dxy, dz, dcol,
                    (double)w[0].x, (double)w[0].y, (double)w[3].x, (double)w[3].y, (double)w[3].z, w->rgba,
                    (double)(int32_t)it[0] / 16.0, (double)(int32_t)it[1] / 16.0, (double)(int32_t)it[12] / 16.0,
                    (double)(int32_t)it[13] / 16.0, (double)(int32_t)it[14] / 16.0, it[8], it[9], it[10], it[11]);
            }
        }
    }
    if (g_emc.kicks <= 3 || !(g_emc.kicks & 255))
        LOG("embers check: %u runs, %u packets, %u sprites compared, %u differ (count %u, x or y over 1/4 pixel %u, z over 256 "
            "%u, colour over 2 %u); most apart x or y %u/16, z %u/16, colour %u", g_emc.runs, g_emc.kicks, g_emc.quads,
            g_emc.bad, g_emc.why[0], g_emc.why[1], g_emc.why[2], g_emc.why[3], g_emc.dxy, g_emc.dz, g_emc.dcol);
}

void gs_backend_embers_done(void)
{
    if (!(g_gx_flags2 & 2048) || g_emc.skip)
        return;
    if (g_emc.pending && g_emc.n) {
        g_emc.why[0]++;
        g_emc.bad += g_emc.n;
        if (g_emc.detail < 12) {
            g_emc.detail++;
            LOG("embers check: %u sprites of the native path the microprogram did not send", g_emc.n);
        }
    }
    g_emc.pending = 0;
}

int gs_backend_embers(const uint32_t *mem, uint32_t top, uint32_t itop, int first)
{
    static int native;
    static uint32_t prim;
    static NeState st;
    static NpVert q[NE_GROUPS * 4 * 4];
    uint32_t n;
    g_kick_watch = (g_gx_flags2 & (512 | 2048)) != 0;
    if (first) {
        const uint32_t ty = mem[1021 * 4 + 1];
        /* the tag: PRIM in it (a textured sprite, STQ, no fog), PACKED, XYZF2, ST, RGBAQ, XYZF2 */
        const int known = ((ty >> 14) & 1) && ((ty >> 26) & 3) == 0 && (ty >> 28) == 4 && mem[1021 * 4 + 2] == 0x4124u &&
                          (((ty >> 15) & 0x7FF) & 0x337) == 0x016;
        prim = (ty >> 15) & 0x7FF;
        if (!known) {
            static uint32_t logged;
            if (logged++ < 4)
                LOG("gx: embers with a tag the native path does not know: %08X %08X %08X", ty, mem[1021 * 4 + 2],
                    mem[1021 * 4 + 3]);
        }
        native = known && g_gx_ready && g_gx_bisect != 1 && !g_gx_maxdraw && (g_native_render & NATIVE_WORLD) &&
                 !(g_gx_flags2 & (2048 | 4096)) && !(mem[1023 * 4 + 1] & 0xFFFF) && nw_ember_setup(mem, &st);
        if (g_gx_flags2 & 2048) {
            g_emc.skip = !known || !nw_ember_setup(mem, &st);
        }
    }
    if ((g_gx_flags2 & 2048) && !g_emc.skip) {
        /* what the native path would send, with the position the microprogram has for the batch's second
         * particle: its register 5 as the run before left it */
        extern const float *vu_vf(int unit, int k);
        float stale[4];
        memcpy(stale, vu_vf(1, 5), sizeof stale);
        g_emc.n = (mem[1023 * 4 + 1] & 0xFFFF) ? 0 : nw_ember_run(mem, top, itop, &st, stale, g_emc.v);
        g_emc.pending = !(mem[1023 * 4 + 1] & 0xFFFF);
        g_emc.runs++;
    }
    if (!native) {
        g_ember_vu++;
        return 0;
    }
    NAT_TIME(3, n = nw_ember_run(mem, top, itop, &st, NULL, q));
    if (n) {
        int prev = pt_enter(PT_NAT);
        gs_write_reg(GS_PRIM, prim);    /* as the tag does */
        puff_draw(prim, q, n);
        g_ember_quads += n;
        pt_leave(prev);
    }
    return 1;
}

void gs_backend_puffs_kick(const uint32_t *mem, uint32_t qaddr)
{
    /* where each of a particle's vertices has its ST, RGBAQ and XYZF2, by kind (0: its two corners) */
    static const uint8_t lay0[2][3] = {{0, 1, 2}, {3, 1, 4}};
    static const uint8_t lay2[4][3] = {{0, 1, 2}, {3, 1, 4}, {5, 1, 6}, {7, 1, 8}};
    static const uint8_t lay4[4][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {9, 10, 11}};
    const uint32_t *q = mem + 4 * (qaddr & 1023);
    const uint32_t type = g_pfc.type, per = type == 0 ? 5 : type == 4 ? 12 : 9, nv = type == 0 ? 2 : 4;
    const uint8_t (*lay)[3] = type == 0 ? lay0 : type == 4 ? lay4 : lay2;
    uint32_t cnt, k, j, c;
    {
        extern uint32_t g_vu1_code;
        if (g_vu1_code == 0x0047C6F0u) {
            ember_check_kick(mem, qaddr);
            return;
        }
    }
    if (!(g_gx_flags2 & 512) || g_pfc.skip)
        return;
    if ((q[0] & 0x7FFF) == 6 && (q[1] >> 28) == 1 && (q[2] & 15) == 0xE) {
        qaddr += 7;                     /* the pool's registers first */
        q = mem + 4 * (qaddr & 1023);
    }
    cnt = q[0] & 0x7FFF;
    g_pfc.kicks++;
    if ((q[1] >> 28) != per) {
        g_pfc.why[0]++;
        g_pfc.bad += cnt;
        return;
    }
    for (k = 0; k < cnt; k++) {
        const uint32_t *it = mem + 4 * ((qaddr + 1 + per * k) & 1023);
        const NpVert *nq = &g_pfc.v[4 * g_pfc.head];
        uint32_t dxy = 0, dz = 0, dcol = 0, adc = 0;
        float dst = 0.0f;
        int bad;
        if (!g_pfc.n) {
            g_pfc.why[0]++;
            g_pfc.bad += cnt - k;
            if (g_pfc.detail < 12) {
                g_pfc.detail++;
                LOG("puffs check: kind %u, packet of %u: the native path has none for its particle %u", type, cnt, k);
            }
            break;
        }
        for (j = 0; j < nv; j++) {
            const NpVert *w = &nq[type == 0 ? 3 * j : j];
            const uint32_t *st = it + 4 * lay[j][0], *rg = it + 4 * lay[j][1], *xyz = it + 4 * lay[j][2];
            const float iq = BITS2F(st[2]) != 0.0f ? 1.0f / BITS2F(st[2]) : 0.0f;
            const int32_t want[3] = {(int32_t)(w->x * 16.0f), (int32_t)(w->y * 16.0f), (int32_t)(w->z * 16.0f)};
            for (c = 0; c < 3; c++) {
                int32_t dd = (int32_t)xyz[c] - want[c];
                if (dd < 0)
                    dd = -dd;
                if (c < 2 && (uint32_t)dd > dxy)
                    dxy = (uint32_t)dd;
                if (c == 2 && (type || j) && (uint32_t)dd > dz)
                    dz = (uint32_t)dd;          /* a sprite's Z is its second corner's */
            }
            if (fabsf(BITS2F(st[0]) * iq - w->s) > dst)
                dst = fabsf(BITS2F(st[0]) * iq - w->s);
            if (fabsf(BITS2F(st[1]) * iq - w->t) > dst)
                dst = fabsf(BITS2F(st[1]) * iq - w->t);
            for (c = 0; c < 4; c++) {
                /* the integer the GS takes the low byte of: as it is, or in a float of 2^23 and more */
                const uint32_t raw = type == 0 || type == 2 ? rg[c] : (rg[c] >> 23) == 0x96 ? rg[c] & 0x7FFFFF : 0xFFFFFFFFu;
                const uint32_t have = (w->rgba >> (24 - 8 * c)) & 0xFF;
                int32_t dd;
                if (raw > 255) {
                    g_pfc.wraps++;
                    continue;
                }
                dd = (int32_t)raw - (int32_t)have;
                if (dd < 0)
                    dd = -dd;
                if ((uint32_t)dd > dcol)
                    dcol = (uint32_t)dd;
            }
            if (type)
                adc += ((xyz[3] >> 15) & 1) != (j < 2);
        }
        bad = dxy > 4 || dz > 4096 || dst > 0.002f || dcol > 2 || adc;
        g_pfc.why[1] += dxy > 4;
        g_pfc.why[2] += dz > 4096;
        g_pfc.why[3] += dst > 0.002f;
        g_pfc.why[4] += dcol > 2;
        g_pfc.why[5] += adc != 0;
        if (dxy > g_pfc.dxy) g_pfc.dxy = dxy;
        if (dz > g_pfc.dz) g_pfc.dz = dz;
        if (dst > g_pfc.dst) g_pfc.dst = dst;
        if (dcol > g_pfc.dcol) g_pfc.dcol = dcol;
        g_pfc.quads++;
        g_pfc.kind[type < 6 ? type : 1]++;
        if (bad) {
            g_pfc.bad++;
            if (g_pfc.detail < 12 && (dxy > 4 || dst > 0.002f || dcol > 2 || adc || dz > 65536)) {
                g_pfc.detail++;
                LOG("puffs check: kind %u particle %u of %u: x or y %u/16, z %u/16, st %g, colour %u, ADC %u apart", type, k,
                    cnt, dxy, dz, (double)dst, dcol, adc);
                for (j = 0; j < nv; j++) {
                    const NpVert *w = &nq[type == 0 ? 3 * j : j];
                    const uint32_t *st = it + 4 * lay[j][0], *rg = it + 4 * lay[j][1], *xyz = it + 4 * lay[j][2];
                    LOG("  vertex %u: native %.2f %.2f %.1f st %g %g rgba %08X; the microprogram's %.2f %.2f %.1f stq %g %g "
                        "%g rgba %08X %08X %08X %08X adc %u", j, (double)w->x, (double)w->y, (double)w->z, (double)w->s,
                        (double)w->t, w->rgba, (double)(int32_t)xyz[0] / 16.0, (double)(int32_t)xyz[1] / 16.0,
                        (double)(int32_t)xyz[2] / 16.0, (double)BITS2F(st[0]), (double)BITS2F(st[1]), (double)BITS2F(st[2]),
                        rg[0], rg[1], rg[2], rg[3], (xyz[3] >> 15) & 1);
                }
            }
        }
        g_pfc.head = (g_pfc.head + 1) % PFC_RING;
        g_pfc.n--;
    }
    if (g_pfc.kicks <= 3 || !(g_pfc.kicks & 255))
        puff_check_line();
}

void gs_backend_puffs_done(int last)
{
    if (!(g_gx_flags2 & 512) || !last)
        return;
    if (g_pfc.n && !g_pfc.skip) {
        g_pfc.why[0]++;
        g_pfc.bad += g_pfc.n;
        if (g_pfc.detail < 12) {
            g_pfc.detail++;
            LOG("puffs check: kind %u: %u quads of the native path the microprogram did not send (smallest size %g, alpha "
                "over %g)", g_pfc.type, g_pfc.n, (double)g_pfc.pool.smin, (double)g_pfc.pool.athr);
        }
    }
    g_pfc.n = g_pfc.head = 0;
}

int gs_backend_puffs(const uint32_t *mem, uint32_t top, uint32_t itop, int first)
{
    static int native, sent;
    static uint32_t prim;
    static NpPool pool;
    static NpVert q[NP_BATCH * 4];
    const uint32_t n = itop & 255;
    uint32_t nq, k;
    g_kick_watch = (g_gx_flags2 & (512 | 2048)) != 0;
    if (first) {
        const uint32_t type = mem[1007 * 4] & 0xFFFF, ty = mem[1010 * 4 + 1], nreg = ty >> 28, *at = mem + 1023 * 4;
        const uint64_t regs = mem[1010 * 4 + 2] | (uint64_t)mem[1010 * 4 + 3] << 32;
        int known;
        prim = (ty >> 15) & 0x7FF;
        /* the header's tags: the six registers; PRIM in the vertices' (textured, STQ, no fog), PACKED, and a
         * particle's registers as its kind sends them */
        known = ((ty >> 14) & 1) && ((ty >> 26) & 3) == 0 && (prim & 0x330) == 0x010 && (at[0] & 0x7FFF) == 6 &&
                ((at[1] >> 26) & 3) == 0 && (at[1] >> 28) == 1 && (at[2] & 15) == 0xE &&
                (type == 0 ? (prim & 7) == 6 && nreg == 5 && regs == 0x42412ull
                 : type == 4 ? (prim & 15) == 0xC && nreg == 12 && regs == 0x412412412412ull
                 : (type == 2 || type == 5) && (prim & 7) == 4 && nreg == 9 && regs == 0x424242412ull);
        if (!known && type != 3) {
            static uint32_t logged;
            if (logged < 8) {
                logged++;
                LOG("gx: particle pool of kind %u with tags the native path does not know: %08X %08X %08X; %08X %08X %08X",
                    type, ty, mem[1010 * 4 + 2], mem[1010 * 4 + 3], at[0], at[1], at[2]);
            }
        }
        /* kind 0 (an explosion's flash) has not come up in a compared run yet: the microprogram's until one has */
        native = known && type != 0 && n <= NP_BATCH && g_gx_ready && g_gx_bisect != 1 && !g_gx_maxdraw &&
                 (g_native_render & NATIVE_WORLD) && !(g_gx_flags2 & (512 | 1024)) && nw_puff_setup(mem, &pool);
        sent = 0;
        if (g_gx_flags2 & 512) {
            g_pfc.type = type;
            g_pfc.n = g_pfc.head = 0;
            g_pfc.skip = !known || !nw_puff_setup(mem, &g_pfc.pool);
            g_pfc.pools += !g_pfc.skip;
        }
    }
    if (g_gx_flags2 & 512)
        puff_check_batch(mem, top, n);
    if (!native) {
        g_puff_vu++;
        return 0;
    }
    NAT_TIME(3, nq = nw_puff_batch(mem, top, n > NP_BATCH ? NP_BATCH : n, &pool, q));
    if (nq) {
        int prev = pt_enter(PT_NAT);
        if (!sent) {
            for (k = 0; k < 6; k++) {
                const uint32_t *r = mem + (1011 + k) * 4;
                gs_write_reg(r[2] & 0xFF, r[0] | (uint64_t)r[1] << 32);
            }
            sent = 1;
        }
        gs_write_reg(GS_PRIM, prim);    /* as each packet's tag does */
        puff_draw(prim, q, nq);
        g_puff_quads += nq;
        pt_leave(prev);
    }
    return 1;
}

/* the chain's buffers: the targets its draws went to */
static int post_in_set(uint32_t bp)
{
    uint32_t k;
    for (k = 0; k < g_post.nset; k++)
        if (g_post.set[k] == bp)
            return 1;
    return 0;
}

/* a bright pass draw: the scene rectangle it reads (texel coordinates; floor: the game samples at texel centres) */
static void post_view_note(const GsDraw *d, const GsVertex *v, int n)
{
    int k;
    if (!((d->prim >> 8) & 1))
        return;
    for (k = 0; k < n; k++) {
        const int u = (int)(v[k].u >> 4), t = (int)(v[k].v >> 4);
        if (u < g_post.vx0) g_post.vx0 = u;
        if (u > g_post.vx1) g_post.vx1 = u;
        if (t < g_post.vy0) g_post.vy0 = t;
        if (t > g_post.vy1) g_post.vy1 = t;
    }
}

/* 1: a draw of the game's bloom chain, skipped (post_bloom does it) */
static int post_filter(const GsDraw *d, int type, const GsVertex *v, int n)
{
    uint32_t fbp = GS_FIELD(d->frame, 0, 9) * 32, tbp = GS_FIELD(d->tex0, 0, 14), tme = (d->prim >> 4) & 1;
    uint32_t A = GS_FIELD(d->alpha, 0, 2), B = GS_FIELD(d->alpha, 2, 2), C = GS_FIELD(d->alpha, 4, 2);
    uint32_t D = GS_FIELD(d->alpha, 6, 2), abe = (d->prim >> 6) & 1;
    RT *dst;
    int chain;
    if (g_gx_flags & 64)
        return 0;
    if (!g_post.active) {
        /* the start: a bright pass, (Cs - Cd) scaled, from the scene into another target */
        if (tme && abe && type == 6 && A == 0 && B == 1 && C == 2 && D == 2 && g_big && tbp == g_big->fbp &&
            fbp != g_big->fbp) {
            dst = rt_find(fbp);
            g_post.active = 1;
            g_post.scene = g_big;
            g_post.chain_no++;
            g_post.set[0] = fbp;
            g_post.nset = 1;
            g_post.vx0 = g_post.vy0 = 1 << 30;
            g_post.vx1 = g_post.vy1 = -1;
            post_view_note(d, v, n);
            {
                static uint32_t logged;
                const uint32_t was = g_post.threshold;
                if (dst && dst->clear)
                    g_post.threshold = dst->clear & 0xFF;
                if ((g_post.threshold != was || !dst || !dst->clear) && logged < 60) {
                    logged++;
                    LOG("bloom: frame %u threshold %02X (was %02X), bright pass into %05X, %s cleared to %08X", g_frame_no,
                        g_post.threshold, was, fbp, dst ? "known," : "unknown,", dst ? dst->clear : 0);
                }
            }
            return 1;
        }
        /* or a speed blur with no bloom before it (the second or so after a crash's replay, the race going on):
         * its start, the view at half size into the blur buffer (a copy: 320 wide, half the source's width
         * and height). Drawn as the game's, its 32-pixel mask sprites and self-blending passes went through the
         * buffer at a fraction of its size. */
        if (tme && type == 6 && ((d->prim >> 8) & 1) && A == 2 && B == 2 && D == 0 && g_big && tbp == g_big->fbp &&
            fbp != g_big->fbp && !((dst = rt_find(fbp)) && dst->big)) {
            int k, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, u0 = 1 << 30, u1 = -1, t0 = 1 << 30, t1 = -1;
            for (k = 0; k < n; k++) {
                if (v[k].x < x0) x0 = v[k].x;
                if (v[k].x > x1) x1 = v[k].x;
                if (v[k].y < y0) y0 = v[k].y;
                if (v[k].y > y1) y1 = v[k].y;
                if ((int)v[k].u < u0) u0 = (int)v[k].u;
                if ((int)v[k].u > u1) u1 = (int)v[k].u;
                if ((int)v[k].v < t0) t0 = (int)v[k].v;
                if ((int)v[k].v > t1) t1 = (int)v[k].v;
            }
            if (x0 == 0 && y0 == 0 && x1 >= 300 * 16 && x1 <= 330 * 16 && y1 >= 100 * 16 &&
                abs((u1 - u0) - 2 * x1) <= 32 && abs((t1 - t0) - 2 * y1) <= 32) {
                g_post.active = 2;
                g_post.scene = g_big;
                g_post.chain_no++;
                g_post.set[0] = fbp;
                g_post.nset = 1;
                g_post.vx0 = g_post.vy0 = 1 << 30;
                g_post.vx1 = g_post.vy1 = -1;
                g_post.blurs_alone++;
                speed_blur_note(d, v, n);
                return 1;
            }
        }
        {
            /* anomalies: a bright pass not taken as a chain's start, or a draw of a chain outside one (drawn
             * as the game's: bright vertical strips, the bloom's 64-pixel columns) */
            static uint32_t logged;
            const RT *t = rt_find(fbp);
            if (tme && logged < 40 && !(t && t->big) &&
                ((abe && type == 6 && A == 0 && B == 1 && C == 2 && D == 2) ||
                 (g_post.scene && fbp != g_post.scene->fbp && (tbp == g_post.scene->fbp || post_in_set(tbp))))) {
                logged++;
                LOG("bloom: frame %u draw %u outside a chain: prim %03X fbp %05X tbp %05X alpha %010llX, scene %05X "
                    "(the last chain's), big %05X%s", g_frame_no, g_draw_no, d->prim, fbp, tbp,
                    (unsigned long long)d->alpha, g_post.scene ? g_post.scene->fbp : 0xFFFFFu,
                    g_big ? g_big->fbp : 0xFFFFFu, g_big ? (g_big->resident ? "" : " (not resident)") : "");
            }
        }
        return 0;
    }
    dst = rt_find(fbp);
    /* the chain's draws: from the scene or its buffers into its buffers (the speed blur's mask: a small texture
     * into the blur buffer), and from its buffers into the scene */
    if (fbp == g_post.scene->fbp)
        chain = tme && post_in_set(tbp);
    else
        chain = tme && (tbp == g_post.scene->fbp || post_in_set(tbp) ||
                        (post_in_set(fbp) && GS_FIELD(d->tex0, 26, 4) <= 6 && GS_FIELD(d->tex0, 30, 4) <= 6 && !rt_find(tbp)));
    if ((fbp == g_post.scene->fbp && !(tme && rt_find(tbp))) || (dst && dst->big && dst != g_post.scene) || !chain) {
        /* the end: the scene drawn from something else (the HUD), another screen, or anything else (split-screen:
         * the next view's reflections; its first draw clears them) */
        if (!(fbp == g_post.scene->fbp && !(tme && rt_find(tbp))) && !(dst && dst->big && dst != g_post.scene)) {
            /* (each kind of draw that ends a chain this way once: the game's own order, to check) */
            static uint32_t seen[24], nseen;
            const uint32_t key = fbp | (tme ? tbp : 0x3FFFu) << 14 | (uint32_t)(d->prim & 0x7) << 28;
            uint32_t k;
            for (k = 0; k < nseen && seen[k] != key; k++)
                ;
            if (k == nseen && nseen < 24) {
                seen[nseen++] = key;
                LOG("bloom: frame %u chain %u ended by draw %u: prim %03X fbp %05X tbp %05X %ux%u alpha %010llX xy %d,%d "
                    "rgba %02X%02X%02X%02X", g_frame_no, g_post.chain_no, g_draw_no, d->prim, fbp, tme ? tbp : 0u,
                    1u << GS_FIELD(d->tex0, 26, 4), 1u << GS_FIELD(d->tex0, 30, 4), (unsigned long long)d->alpha,
                    v[0].x >> 4, v[0].y >> 4, v[0].r, v[0].g, v[0].b, v[0].a);
            }
        }
        post_end();
        return 0;
    }
    if (!post_in_set(fbp) && fbp != g_post.scene->fbp && g_post.nset < POST_SETS)
        g_post.set[g_post.nset++] = fbp;
    if (tme && abe && A == 0 && B == 1 && C == 2 && D == 2 && tbp == g_post.scene->fbp)
        post_view_note(d, v, n);
    if (abe && A == 1 && B == 2 && C == 2 && D == 0 && fbp != g_post.scene->fbp) {
        static uint32_t logged;
        if (g_post.decay != GS_FIELD(d->alpha, 32, 8) && logged < 60) {
            logged++;
            LOG("bloom: frame %u decay %02X (was %02X)", g_frame_no, (unsigned)GS_FIELD(d->alpha, 32, 8), g_post.decay);
        }
        g_post.decay = GS_FIELD(d->alpha, 32, 8);     /* accumulation: Cd x FIX + Cs */
    }
    speed_blur_note(d, v, n);
    return 1;
}

/* ---- display ------------------------------------------------------------------------------------ */

/* status line: VU1 runs by microprogram, entry and target */
static struct { uint32_t code, entry, fbp, runs; } g_vrun[24];

static void vrun_hook(uint32_t addr)
{
    extern uint32_t g_vu1_code, g_dma_where;
    uint32_t fbp = (GS_FIELD(gs_reg(GS_FRAME_1), 0, 9) * 32) << 12 | g_dma_where;
    int k;
    if ((g_gx_flags & 4194304) && (fbp >> 12) == 0x3160) {
        extern uint32_t g_vif1_chains;
        static uint32_t lines;
        const uint32_t *vm = g_vu_mem + 0xC000 / 4;
        if (0 && lines++ < 4000)
            LOG("v3160 %u %06X@%04X itop %u c%04X %04X %04X", g_vif1_chains, g_vu1_code, addr, vif_itop(1),
                vm[1013 * 4] & 0xFFFF, vm[1013 * 4 + 1] & 0xFFFF, vm[1013 * 4 + 2] & 0xFFFF);
    }
    for (k = 0; k < 24; k++)
        if (!g_vrun[k].runs || (g_vrun[k].code == g_vu1_code && g_vrun[k].entry == addr && g_vrun[k].fbp == fbp)) {
            g_vrun[k].code = g_vu1_code;
            g_vrun[k].entry = addr;
            g_vrun[k].fbp = fbp;
            g_vrun[k].runs++;
            return;
        }
}

static uint32_t g_prims_period, g_last_chains;
static uint64_t g_last_us;

/* the target the CRTC shows */
static RT *display_rt(void)
{
    uint64_t pmode = gs_priv_reg(GS_PMODE);
    int circ = (pmode & 2) ? 2 : (pmode & 1) ? 1 : 0;
    if (!circ)
        return NULL;
    return rt_find(GS_FIELD(gs_priv_reg(circ == 2 ? GS_DISPFB2 : GS_DISPFB1), 0, 9) * 32);
}

void gs_backend_vsync(void)
{
    RT *r;
    if (g_gx_meter | g_frame_hist)
        g_pt_on = 1;                    /* both read the time split */
    if (++g_vsyncs % 600 == 0) {
        uint64_t now = plat_time_us();
        uint32_t frames = g_vif1_chains - g_last_chains, f = frames ? frames : 1;
        char pt[160];
        pt_report(pt, sizeof pt);
        LOG("gs: %.1f frames/s, %u draws, %u uploads (%u KB) per frame, %d textures (%u+%u decoded), %u KB; %s",
            frames * 1e6 / (double)(now - g_last_us), g_draw_no / f, g_gs_uploads / f,
            g_gs_upload_bytes / f / 1024, g_ntex, g_tex_from_upload, g_tex_from_mem, g_texmem_used / 1024, pt);
        pmc_report(f);
        if (g_tex_evicted || g_tex_dropped)
            LOG("gx: texture cache: %u textures dropped for room, the whole cache %u times", g_tex_evicted, g_tex_dropped);
        {
            void plat_io_report(char *out, size_t size);
            void iop_sd_report(char *line, size_t size);
            void plat_audio_report(uint32_t *blocks, uint32_t *silent);
            char io[440];
            uint32_t blocks, silent;
            plat_io_report(io, sizeof io);
            LOG("disc: %s", io);
            iop_sd_report(io, sizeof io);
            plat_audio_report(&blocks, &silent);
            LOG("sound: %s; %u blocks played, %u of them silence for want of samples", io, blocks, silent);
            {
                /* (in the spike frames' form: work/spike.py names the addresses) */
                uint32_t plat_audio_dry(uint32_t out[12][8]);
                static uint32_t dry[12][8];
                uint32_t nd = plat_audio_dry(dry), i;
                if (nd)
                    LOG("spike frame 0: the sound output ran dry; where its interrupt found the processor, %u times:", nd);
                for (i = 0; i < nd; i++)
                    LOG("  s %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)dry[i][0], (unsigned)dry[i][1],
                        (unsigned)dry[i][2], (unsigned)dry[i][3], (unsigned)dry[i][4], (unsigned)dry[i][5],
                        (unsigned)dry[i][6], (unsigned)dry[i][7]);
            }
        }
        if (g_lit_lists)
            LOG("gx: lit world per frame: %u lists, %u with their colours kept, %u written for keeping (per 100 frames); "
                "%u kept ones found wrong so far", g_lit_lists / f, g_lit_kept / f, g_lit_written * 100 / f, g_lit_bad);
        g_lit_lists = g_lit_kept = g_lit_written = 0;
        {
            extern uint32_t g_lit_ps_lists, g_lit_ps_checked, g_lit_ps_bad, g_lit_ps_slow;     /* nw_world.c */
            if (g_lit_ps_lists)
                LOG("gx: lit world's colour sums in paired singles: %u lists a frame, %u vertices above their floor; %u "
                    "lists checked against the scalar loop so far, %u differed", g_lit_ps_lists / f, g_lit_ps_slow / f,
                    g_lit_ps_checked, g_lit_ps_bad);
            g_lit_ps_lists = g_lit_ps_slow = 0;
        }
        if (g_kc_inst)
            LOG("gx: lit props per frame: %u instances, %u with their colours kept, %u written for keeping and %u with no "
                "place free (per 100 frames); %u kept ones found wrong so far", g_kc_inst / f, g_kc_hits / f,
                g_kc_written * 100 / f, g_kc_busy * 100 / f, g_kc_bad);
        g_kc_inst = g_kc_hits = g_kc_written = g_kc_busy = 0;
        if (g_tex_direct)
            LOG("gx: %u placed textures decoded from their transfers (%u differed from GS memory's so far)", g_tex_direct,
                g_tex_direct_bad);
        g_tex_direct = 0;
        g_tex_evicted = g_tex_dropped = 0;
        LOG("gx: lookups per frame: %u by upload, %u known, %u from blocks, %u hashed (%u Ktexels)", g_lk_upload / f,
            g_lk_known / f, g_lk_cover / f,
            g_lk_hash / f, g_lk_hash_kb / f);
#ifndef NO_TMEMO2
        tm2_status();
#endif
        {
            extern uint32_t g_gs_ext_uploads, g_gs_ext_aged, g_clut_hits, g_clut_misses, g_clut_same, g_gs_stage_why[4];
            LOG("gx: per frame %u draw setups, %u checked, %u applied", g_ds_calls / f, g_ds_slow / f, g_ds_apply / f);
            if (g_wstat == 7)
                LOG("gx: draw setups per frame (calls/checked/applied): GS path %u/%u/%u, world %u/%u/%u, cars %u/%u/%u",
                    g_ds_by[0][0] / f, g_ds_by[0][1] / f, g_ds_by[0][2] / f, g_ds_by[1][0] / f, g_ds_by[1][1] / f,
                    g_ds_by[1][2] / f, g_ds_by[2][0] / f, g_ds_by[2][1] / f, g_ds_by[2][2] / f);
            memset(g_ds_by, 0, sizeof g_ds_by);
            {
                void pc_report(uint32_t out[8]);
                extern uint32_t g_tm_reads[2];
                uint32_t pc[8];
                LOG("timers: per frame %u reads of a count, %u in all with its event due", g_tm_reads[0] / f, g_tm_reads[1]);
                g_tm_reads[0] = g_tm_reads[1] = 0;
                pc_report(pc);
                LOG("gx: per frame %u world packets to parse, %u of them known from before, and %u of the cars', %u known; "
                    "%u and %u not kept, the memory emptied %u times",
                    pc[0] / f, pc[1] / f, pc[4] / f, pc[5] / f, pc[2], pc[6], pc[3] + pc[7]);
            }
            g_ds_calls = g_ds_slow = g_ds_apply = 0;
            {
                extern uint32_t g_clut_repeats, g_clut_lazy_n, g_clut_sync_n;
                LOG("gs: per frame %u uploads in place, %u aged out, staged %u split %u not RAM (%u path 2, %u path 3); "
                    "CLUT loads %u repeated, %u cached (%u of what the buffer held, %u copied later, %u of those read), %u read; "
                    "target checks %u, %u past the page test", g_gs_ext_uploads / f, g_gs_ext_aged / f, g_gs_stage_why[0] / f,
                    g_gs_stage_why[1] / f, g_gs_stage_why[2] / f, g_gs_stage_why[3] / f, g_clut_repeats / f, g_clut_hits / f,
                    g_clut_same / f, g_clut_lazy_n / f, g_clut_sync_n / f, g_clut_misses / f,
                    (g_sync_quick + g_sync_slow) / f, g_sync_slow / f);
                g_clut_repeats = g_clut_lazy_n = g_clut_sync_n = 0;
            }
            g_gs_ext_uploads = g_gs_ext_aged = g_clut_hits = g_clut_misses = g_clut_same = 0;
            g_sync_quick = g_sync_slow = 0;
            {
                extern uint32_t g_lit_floor_made;
                if (g_lit_floor_made > f / 4)
                    LOG("gx: world lighting: %u floor tables made in %u frames", g_lit_floor_made, f);
                g_lit_floor_made = 0;
            }
            {
                extern uint32_t g_gs_placed, g_gs_placed_loads, g_tex_sent, g_tex_mips;
                LOG("gs: per frame %u textures placed by the game without an upload (%u lookups, %u written to decode "
                    "per 100 frames), %u script entries sent, %u mip levels left out", g_gs_placed / f, g_lk_placed / f,
                    g_gs_placed_loads * 100 / f, g_tex_sent / f, g_tex_mips / f);
                g_gs_placed = g_gs_placed_loads = g_lk_placed = g_tex_sent = g_tex_mips = 0;
            }
            memset(g_gs_stage_why, 0, 4 * sizeof g_gs_stage_why[0]);
            {
                extern uint32_t g_gs_ext_reads[3], g_gs_ext_read_words, g_gs_ext_turn_changed;
                LOG("gs: uploads in place read in full, per 100 frames: %u new or back, %u with other samples, %u in turn "
                    "(%u found changed so far); %u KB a frame", g_gs_ext_reads[0] * 100 / f, g_gs_ext_reads[1] * 100 / f,
                    g_gs_ext_reads[2] * 100 / f, g_gs_ext_turn_changed, g_gs_ext_read_words / 256 / f);
                memset(g_gs_ext_reads, 0, 3 * sizeof g_gs_ext_reads[0]);
                g_gs_ext_read_words = 0;
            }
        }
        vu1_prof_report(pt, sizeof pt);
        LOG("vu1: %s", pt);
        {
            char line[400];
            size_t m = 0;
            int j, k;
            line[0] = 0;
            for (j = 0; j < 8; j++) {
                int best = -1;
                for (k = 0; k < 24; k++)
                    if (g_vrun[k].runs && (best < 0 || g_vrun[k].runs > g_vrun[best].runs))
                        best = k;
                if (best < 0)
                    break;
                m += (size_t)snprintf(line + m, sizeof line - m, " %06X@%04X fbp %05X dma %03X %u", g_vrun[best].code,
                                      g_vrun[best].entry, g_vrun[best].fbp >> 12, g_vrun[best].fbp & 0xFFF,
                                      g_vrun[best].runs / f);
                g_vrun[best].runs = 0;
            }
            memset(g_vrun, 0, sizeof g_vrun);
            LOG("vu1 runs per frame:%s", line);
        }
        {
            extern uint32_t g_vif_words[4], g_vif_unpack_fmt[16][2];
            char ul[200];
            size_t um = 0;
            int uk;
            LOG("vif: per frame %u UNPACK, %u DIRECT, %u MPG, %u other words", g_vif_words[0] / f, g_vif_words[1] / f,
                g_vif_words[2] / f, g_vif_words[3] / f);
            memset(g_vif_words, 0, sizeof g_vif_words);
            ul[0] = 0;
            for (uk = 0; uk < 16 && um < sizeof ul - 32; uk++)
                if (g_vif_unpack_fmt[uk][0])
                    um += (size_t)snprintf(ul + um, sizeof ul - um, " %X: %u of %u words", (unsigned)uk,
                                           g_vif_unpack_fmt[uk][0] / f, g_vif_unpack_fmt[uk][1] / f);
            LOG("vif: UNPACKs per frame by format:%s", ul);
            memset(g_vif_unpack_fmt, 0, sizeof g_vif_unpack_fmt);
        }
        {
            extern uint32_t g_vif_deferred[4];
            LOG("gx: per frame %u target copies, %u restores, %u readbacks (the strip full %u times), %u blooms (speed blur in %u frames: its ramp read %u "
                "times, its grid made %u times, %u tiles summed at half size), %u native draws, %u display lists made (%d kept, %u KB); car batches %u native, %u emulated; world "
                "batches %u native, %u emulated", g_rt_copies / f, g_rt_restores / f, g_rt_readbacks / f, g_strip_fulls, g_post.chains / f,
                g_sb.drawn, g_sbc.ramp_reads, g_sbc.grids, g_sb_tiles,
                g_native_draws / f, g_dl_built / f, g_ndl, g_dl_bytes / 1024, g_vif_deferred[0] / f,
                g_vif_deferred[1] / f, g_vif_deferred[2] / f, g_vif_deferred[3] / f);
            LOG("gx: per frame %u blits passed on without a copy, %u copies of big targets, %u sky layers from lists",
                g_blit_aliased / f, g_big_copies / f, g_sky_runs / f);
            LOG("gx: cache per frame: %u lists written out to RAM, %u textures invalidated", g_dl_syncs / f, g_tex_inv / f);
            g_dl_syncs = 0;
            g_tex_inv = 0;
            if (g_disp_tex_read)
                LOG("gx: the display buffer's picture was read as a texture %u times a frame", g_disp_tex_read / f);
            g_disp_tex_read = 0;
            if (g_gx_flags2 & 8388608) {
                LOG("gx: clip work per frame: %u batches with a triangle over the near plane, %u such triangles; "
                    "%u triangles far outside the screen (%u of them very far), %u batches with those",
                    g_clip_bat / f, g_clip_tri / f, g_clip_far / f, g_clip_far2 / f, g_clip_vert / f);
                g_clip_bat = g_clip_tri = g_clip_far = g_clip_far2 = g_clip_vert = 0;
            }
            if (g_cl_looked)
                LOG("gx: cut to the screen per frame: %u batches looked at, %u past the box and the depth scan, %u drawn cut (%u triangles cut, "
                    "%u left out, %u vertices of polygons), %u us%s; %u by the box and the table of big triangles alone, %u triangles looked at, "
                    "%u vertices' depths for the %u boxes that reach the near plane or come near",
                    g_cl_looked / f, g_cl_tested / f, g_cl_cut / f, g_cl_tris / f, g_cl_dropped / f, g_cl_verts / f,
                    (unsigned)(ticks_to_microsecs(g_cl_ticks) / f), g_cl_nospace ? "; out of room at times" : "", g_cl_few / f,
                    g_cl_big_t / f, g_cl_wpass_v / f, g_cl_nearbox / f);
            g_cl_looked = g_cl_tested = g_cl_cut = g_cl_tris = g_cl_dropped = g_cl_verts = g_cl_nospace = 0;
            g_cl_few = g_cl_pass_v = g_cl_big_t = g_cl_nearbox = g_cl_wpass_v = 0;
            g_cl_ticks = 0;
            if (g_clip_skipped)
                LOG("gx: %u batches with a triangle over the near plane were not drawn (the near plane diagnostic)",
                    g_clip_skipped / f);
            if (g_clip_inverted)
                LOG("gx: %u batches with a triangle over the near plane were drawn with the other cull sense",
                    g_clip_inverted / f);
            g_clip_skipped = g_clip_inverted = 0;
            if (g_post.blurs_alone)
                LOG("gx: %u speed blurs with no bloom (after a crash's replay)", g_post.blurs_alone);
            g_post.blurs_alone = 0;
            g_sky_runs = 0;
            g_blit_aliased = g_big_copies = 0;
            LOG("gx: display lists made %u, segments turned %u, lists carried over %u, left behind %u; all dropped %u times "
                "so far; static strip lists made %u, strips sent without one %u", g_dl_built, g_dl_turns, g_dl_moved,
                g_dl_aged, g_dl_resets, g_sdl_built, g_sdl_direct);
            g_dl_turns = g_dl_moved = g_dl_aged = g_sdl_built = g_sdl_direct = 0;
            LOG("gx: per frame %u lists' arrays written out; %u lists found changed in place so far", g_dl_flushes / f, g_dl_changed);
            g_dl_flushes = 0;
            {
                extern int g_nc_psq_check;
                extern uint32_t g_nc_psq_n, g_nc_psq_bad, g_nc_psq_max[3];
                g_nc_psq_check = (g_gx_flags2 & 8192) != 0;
                if (g_nc_psq_n)
                    LOG("gx: car bytes check: %u vertices, %u more than 1 apart; most apart RGB %u, alpha %u, UV %u",
                        g_nc_psq_n, g_nc_psq_bad, g_nc_psq_max[0], g_nc_psq_max[1], g_nc_psq_max[2]);
            }
            {
                extern int g_nc_ps, g_nc_ps_check;
                extern uint32_t g_nc_ps_n, g_nc_ps_bad, g_nc_ps_max[3];
                g_nc_ps = !(g_gx_flags2 & 32768);
                {
                    extern int g_nc_sk;
                    extern uint32_t g_nc_sk_hits, g_nc_sk_made, g_nc_sk_resets, g_nc_sk_verts, g_nc_sk_blends, g_nc_sk_part;
                    g_nc_sk = (g_gx_flags2 & 65536) ? 0 : (g_gx_flags2 & 1048576) ? 2 : 1;
                    {
                        extern int g_nc_sk_dbg;
                        extern uint32_t g_nc_sk_why[4];
                        g_nc_sk_dbg = (g_gx_flags2 & 524288) != 0;
                        if (g_nc_sk_why[0] | g_nc_sk_why[1] | g_nc_sk_why[2] | g_nc_sk_why[3])
                            LOG("gx: batches skinned again: %u for their bones, %u for the rest, %u for both, %u not "
                                "followed", g_nc_sk_why[0], g_nc_sk_why[1], g_nc_sk_why[2], g_nc_sk_why[3]);
                        memset(g_nc_sk_why, 0, sizeof g_nc_sk_why);
                    }
                    if (g_nc_sk_hits | g_nc_sk_made)
                        LOG("gx: per frame %u crashed car batches skinned, %u of them only where their bones changed (%u "
                            "vertices on bones in all), %u as the frame before left them; the pools started over %u times "
                            "so far", g_nc_sk_made / f, g_nc_sk_part / f, g_nc_sk_verts / f, g_nc_sk_hits / f,
                            g_nc_sk_resets);
                    g_nc_sk_hits = g_nc_sk_made = g_nc_sk_verts = g_nc_sk_blends = g_nc_sk_part = 0;
                    {
                        extern int g_nc_sk_time;
                        extern uint64_t g_nc_sk_t[5];
                        extern uint32_t g_nc_rows_made;
                        g_nc_sk_time = (g_gx_flags2 & 2097152) != 0;
                        if (g_nc_sk_t[0] | g_nc_sk_t[1] | g_nc_sk_t[2] | g_nc_sk_t[3])
                            LOG("gx: crashed car batches, microseconds a frame: %u sum and entry of the ones skinned, %u "
                                "the kept ones (sum, entry, copies), %u skinning, %u lighting, %u constants; the normals' "
                                "table made %u times a frame", (unsigned)(ticks_to_microsecs(g_nc_sk_t[0]) / f),
                                (unsigned)(ticks_to_microsecs(g_nc_sk_t[1]) / f), (unsigned)(ticks_to_microsecs(g_nc_sk_t[2]) / f),
                                (unsigned)(ticks_to_microsecs(g_nc_sk_t[3]) / f), (unsigned)(ticks_to_microsecs(g_nc_sk_t[4]) / f),
                                g_nc_rows_made / f);
                        memset(g_nc_sk_t, 0, sizeof g_nc_sk_t);
                        g_nc_rows_made = 0;
                    }
                }
                g_nc_ps_check = (g_gx_flags2 & 16384) != 0;
                if (g_nc_ps_n)
                    LOG("gx: car loop check: %u vertices, %u more than 1 apart from the C loop; most apart RGB %u, alpha %u, "
                        "UV %u", g_nc_ps_n, g_nc_ps_bad, g_nc_ps_max[0], g_nc_ps_max[1], g_nc_ps_max[2]);
            }
            {
                extern uint32_t g_mpg_noted, g_mpg_flushed;
                LOG("gx: per frame %u microprogram words noted where they are, %u of them written out after all",
                    g_mpg_noted / f, g_mpg_flushed / f);
                g_mpg_noted = g_mpg_flushed = 0;
            }
            LOG("gx: per frame %u spark trail segments drawn of %u alive; %u smoke and debris quads, %u batches of those "
                "(per 100 frames) through the microprogram; %u embers, %u batches of those through theirs",
                g_spark_quads / f, g_spark_alive / f, g_puff_quads / f, g_puff_vu * 100 / f, g_ember_quads / f,
                g_ember_vu * 100 / f);
            if (g_spark_cnt[0])
                LOG("gx: per frame %u spark emitter runs of %u particles, %u alive (%u of them not drawn: every second or "
                    "third when there are so many), %u trail points worked out", g_spark_cnt[0] / f, g_spark_cnt[1] / f,
                    g_spark_cnt[2] / f, g_spark_shed / f, g_spark_cnt[3] / f);
            memset(g_spark_cnt, 0, sizeof g_spark_cnt);
            g_spark_shed = 0;
            g_spark_quads = g_spark_alive = g_puff_quads = g_puff_vu = g_ember_quads = g_ember_vu = 0;
            if (g_spark_dev_n)
                LOG("gx: spark trails check: %u points left out, %.2f pixels from the drawn trail on average, %.1f at most "
                    "(so far)", g_spark_dev_n, (double)(g_spark_dev_sum / (float)g_spark_dev_n), (double)g_spark_dev_max);
            g_spark_dev_n = 0;
            g_spark_dev_sum = 0.0f;
            memset(g_vif_deferred, 0, 4 * sizeof g_vif_deferred[0]);
            g_sb.drawn = 0;
            g_sbc.ramp_reads = g_sbc.grids = g_sb_tiles = g_strip_fulls = 0;
            LOG("gx: per frame %u car batches with both passes in one draw; colour arrays at most %u KB of %u a frame, %u "
                "refused; %u static strips from display lists", g_car_combos / f, g_litcol_peak / 256, LITCOL_HALF / 1024,
                g_litcol_refused, g_sdl_draws / f);
            LOG("gx: car sphere maps per frame: %u a target's picture, %u from GS memory (the target's pages written "
                "since), %u a target with no picture", g_env_rt / f, g_env_mem / f, g_env_nopic / f);
            g_env_rt = g_env_mem = g_env_nopic = 0;
            g_litcol_peak = g_litcol_refused = 0;
            g_car_combos = 0;
            g_sdl_draws = 0;
            {
                extern int g_nc_s8_used;
                LOG("gx: per frame %u car vertices shaded in %u batches for the scene, %u in %u for other targets (%u batches' "
                    "normals from their lists' copies); %u batches without a list in one draw", g_car_vtx[0] / f,
                    g_car_bat[0] / f, g_car_vtx[1] / f, g_car_bat[1] / f, g_nc_s8_used / f, g_car_both / f);
                g_nc_s8_used = 0;
            }
            {
                extern uint32_t g_carest_batches, g_carest_uv, g_carest_uvdiff, g_carest_uvmax, g_carest_coldiff,
                    g_carest_hist[4], g_carpair_batches, g_carpair_redo, g_carpair_checked, g_carpair_coldiff,
                    g_carpair_uvdiff;
                if (g_carpair_checked)
                    LOG("gx: car shading two vertices a register: %u pairs with a light term out of range so far; %u "
                        "batches compared with the loop vertex by vertex, %u colours and %u UV pairs different",
                        g_carpair_redo, g_carpair_checked, g_carpair_coldiff, g_carpair_uvdiff);
                if (g_carest_batches)
                    LOG("gx: car UVs from the square roots' estimate: %u batches compared with the full steps so far, %u of %u "
                        "UV bytes different (%u.%02u%%; by 1: %u, by 2: %u, by more: %u), at most by %u; %u colours different",
                        g_carest_batches, g_carest_uvdiff, g_carest_uv, g_carest_uv ? g_carest_uvdiff * 100 / g_carest_uv : 0,
                        g_carest_uv ? (uint32_t)((uint64_t)g_carest_uvdiff * 10000 / g_carest_uv % 100) : 0,
                        g_carest_hist[1], g_carest_hist[2], g_carest_hist[3], g_carest_uvmax, g_carest_coldiff);
            }
            g_car_both = 0;
            memset(g_car_vtx, 0, sizeof g_car_vtx);
            memset(g_car_bat, 0, sizeof g_car_bat);
        }
        {
            char line[400];
            size_t m = 0;
            int k;
            line[0] = 0;
            for (k = 0; k < g_nrt && m < sizeof line - 48; k++) {
                RT *r = &g_rts[k];
                if (r->st_draws)
                    m += (size_t)snprintf(line + m, sizeof line - m, " %05X %dx%d: %u draws %.2f ms;", r->fbp, r->w, r->h,
                                          r->st_draws / f, (double)ticks_to_microsecs(r->st_ticks) / 1000.0 / f);
                r->st_draws = 0;
                r->st_ticks = 0;
            }
            LOG("gx: targets per frame:%s", line);
        }
        {
            extern void vu0_calls_report(uint32_t frames);
            extern void vif_wstat_report(uint32_t frames);
            vu0_calls_report(f);
            vif_wstat_report(f);
            if (g_wstat == 9) {
                extern void vif_cmd_report(uint32_t frames);
                vif_cmd_report(f);
            }
            if (g_wstat == 7) {
                LOG("pt_enter calls per frame: %u", g_pt_n / f);
                g_pt_n = 0;
                LOG("draws per frame by type (points, lines, line strips, triangles, strips, fans, sprites): %u %u %u %u %u %u %u; "
                    "points into GS memory %u, dropped by the post filter %u, no target %u", g_d2_cnt[0] / f, g_d2_cnt[1] / f,
                    g_d2_cnt[2] / f, g_d2_cnt[3] / f, g_d2_cnt[4] / f, g_d2_cnt[5] / f, g_d2_cnt[6] / f, g_d2_cnt[7] / f,
                    g_d2_cnt[8] / f, g_d2_cnt[9] / f);
                memset(g_d2_cnt, 0, sizeof g_d2_cnt);
            }
            if (g_wstat == 7 && g_as_n) {
                LOG("apply_state's texture memo per frame: %u hits, %u misses", g_tmemo_hits / f, g_tmemo_misses / f);
                g_tmemo_hits = g_tmemo_misses = 0;
                LOG("apply_state per frame (%u calls), cycles: decode+blend %u, texture lookups %u, tex object+load %u, TEV %u, "
                    "pixel state %u, place+scissor %u; texture lookups in parts: rt_for_texture %u, pt_enter %u, sync %u, tex_lookup %u, "
                    "tlut_lookup %u", (uint32_t)(g_as_n / f), (uint32_t)(g_as_t[0] * 12 / f), (uint32_t)(g_as_t[1] * 12 / f),
                    (uint32_t)(g_as_t[2] * 12 / f), (uint32_t)(g_as_t[3] * 12 / f), (uint32_t)(g_as_t[4] * 12 / f),
                    (uint32_t)(g_as_t[5] * 12 / f), (uint32_t)(g_as_t[6] * 12 / f), (uint32_t)(g_as_t[7] * 12 / f),
                    (uint32_t)(g_as_t[8] * 12 / f), (uint32_t)(g_as_t[9] * 12 / f), (uint32_t)(g_as_t[10] * 12 / f));
                memset(g_as_t, 0, sizeof g_as_t);
                g_as_n = 0;
            }
            if (g_wstat == 8) {
                LOG("car batches per frame: %u GS packets", g_cst_n[4] / f);
                LOG("car batch steps per frame, cycles (%u combo, %u combo whole, %u one draw, %u two draws): list+arrays %u, "
                    "shading %u, GS packet %u, matrix %u, combo check %u, combo draw_setup %u, combo TEV/mode/projection %u, "
                    "combo call %u, other paths' leftovers %u, passes' draw_setup %u, passes' draws %u",
                    g_cst_n[0] / f, g_cst_n[1] / f, g_cst_n[2] / f, g_cst_n[3] / f,
                    (uint32_t)(g_cst[0] * 12 / f), (uint32_t)(g_cst[1] * 12 / f), (uint32_t)(g_cst[2] * 12 / f),
                    (uint32_t)(g_cst[3] * 12 / f), (uint32_t)(g_cst[4] * 12 / f), (uint32_t)(g_cst[5] * 12 / f),
                    (uint32_t)(g_cst[6] * 12 / f), (uint32_t)(g_cst[7] * 12 / f), (uint32_t)(g_cst[9] * 12 / f),
                    (uint32_t)(g_cst[10] * 12 / f), (uint32_t)(g_cst[11] * 12 / f));
                memset(g_cst, 0, sizeof g_cst);
                memset(g_cst_n, 0, sizeof g_cst_n);
            }
            if (g_wstat == 9)
                WT_REPORT(f);
            if (g_wstat == 1) {
                LOG("world draw stages per frame, cycles: prologue %u, draw state %u, draw_setup %u, drawn+flush %u, "
                    "list %u, mode/projection/cull %u, arrays %u, scroll/camera/call %u",
                    (uint32_t)(g_wst_stage[0] * 12 / f), (uint32_t)(g_wst_stage[1] * 12 / f),
                    (uint32_t)(g_wst_stage[2] * 12 / f), (uint32_t)(g_wst_stage[3] * 12 / f),
                    (uint32_t)(g_wst_stage[4] * 12 / f), (uint32_t)(g_wst_stage[5] * 12 / f),
                    (uint32_t)(g_wst_stage[6] * 12 / f), (uint32_t)(g_wst_stage[7] * 12 / f));
                memset(g_wst_stage, 0, sizeof g_wst_stage);
            }
        }
        {
            char line[256];
            size_t m = 0;
            int k;
            line[0] = 0;
            for (k = 0; k < g_nrt && m < sizeof line - 64; k++)
                if (g_rts[k].soft_n) {
                    m += (size_t)snprintf(line + m, sizeof line - m, " %05X %dx%d: %u (%u px), read %u ago;", g_rts[k].fbp,
                                          g_rts[k].w, g_rts[k].h, g_rts[k].soft_n / f, g_rts[k].soft_px / f,
                                          g_frame_no - g_rts[k].mem_read);
                    g_rts[k].soft_n = g_rts[k].soft_px = 0;
                }
            if (m)
                LOG("gx: rasterised per frame:%s", line);
        }
        {
            extern void plat_mem_report(char *out, size_t size);
            char m[160];
            plat_mem_report(m, sizeof m);
            LOG("mem: %s", m);
        }
        {
            /* the game's frame rate mode (001D3EE0): vblanks a frame (the
             * vblank handler 001F1CE8 flips after that many), mode 0 NTSC
             * 60, 2 NTSC 30, and the step its simulation takes a frame */
            union { uint32_t u; float f; } dt = {LW(0x4E2740)};
            extern int g_pace_interval;
            extern uint32_t g_pace_steps[5];
            if (g_held_frames)
                LOG("gx: %u targets held per frame, %u draws into them dropped", g_held_frames / f, g_held_draws / f);
            if (g_view_swaps || g_va_draws)
                LOG("gx: split-screen: %u reflection views passed between the players per frame, %u draws into the scene "
                    "through another view of it", g_view_swaps / f, g_va_draws / f);
            g_held_frames = g_held_draws = 0;
            g_view_swaps = g_va_draws = 0;
            LOG("gx: frames shown for 0/1/2/3/4+ retraces: %u %u %u %u %u; game vblanks a frame 0/1/2/3/4+: %u %u %u %u "
                "%u; game frame interval %u, mode %u, step %.4f s; pacer: interval %d, race frames of 1/2/3/4+ vblanks: "
                "%u %u %u %u",
                g_pace_shown[0], g_pace_shown[1], g_pace_shown[2], g_pace_shown[3], g_pace_shown[4], g_pace_game[0],
                g_pace_game[1], g_pace_game[2], g_pace_game[3], g_pace_game[4], LB(0x4E17DC), LW(0x4E2CFC),
                (double)dt.f, g_pace_interval, g_pace_steps[1], g_pace_steps[2], g_pace_steps[3], g_pace_steps[4]);
            {
                /* the game's speed: ticks of its clock (one an update pass, 1/60 s of game time each in a race)
                 * against the vblanks of the same ten seconds */
                static uint32_t tick0, vb0;
                const uint32_t tick = LW(0x004E273Cu), vbn = kernel_vblank_count();
                if (vb0 && vbn != vb0 && tick < tick0)      /* a race's start sets it back: no speed to tell */
                    LOG("gx: game clock: set back from %u to %u in %u vblanks", tick0, tick, vbn - vb0);
                else if (vb0 && vbn != vb0)
                    LOG("gx: game clock: %u ticks in %u vblanks (%u%% of real time in a race)", tick - tick0, vbn - vb0,
                        (tick - tick0) * 100 / (vbn - vb0));
                tick0 = tick;
                vb0 = vbn;
            }
            memset(g_pace_steps, 0, sizeof g_pace_steps);
        }
        memset(g_pace_shown, 0, sizeof g_pace_shown);
        memset(g_pace_game, 0, sizeof g_pace_game);
        g_rt_copies = g_rt_restores = g_rt_readbacks = 0;
        g_post.chains = 0;
        g_native_draws = g_dl_built = 0;
        g_tex_from_upload = g_tex_from_mem = 0;
        g_lk_upload = g_lk_known = g_lk_cover = g_lk_hash = g_lk_hash_kb = 0;
        g_draws_before += g_draw_no;
        g_draw_no = 0;
        g_gs_uploads = g_gs_upload_bytes = 0;
        g_last_chains = g_vif1_chains;
        g_last_us = now;
    }
    (void)g_prims_period;
    if (!g_gx_ready || g_gx_bisect == 2)
        return;
    GX_InvVtxCache();                   /* native draws index guest memory, which changes */
    r = display_rt();
    if (!r || !r->resident || r->draws == r->presented)
        return;
    present(r);
}

/* debugging (g_gx_rtdump): every target's content as linear RGBA, and GS
 * memory, to files next to the image (tools/wii/fatcat.py reads them) */
static void rt_dump_all(void)
{
    FILE *plat_debug_file(const char *name);
    char name[48];
    FILE *f;
    int k;
    uint32_t x, y;
    for (k = 0; k < g_nrt; k++) {
        RT *r = &g_rts[k];
        const uint8_t *t;
        if (r->resident && (r->dirty || !r->tex_valid))
            rt_copy(r, 0);
        if (!r->tex_valid)
            continue;
        gp_drain();
        DCInvalidateRange(r->tex, (uint32_t)(r->ctw * r->cth * 4));
        t = r->tex;
        snprintf(name, sizeof name, "rt_%05X_%ux%d_%dx%d_s%d.rgba", r->fbp, r->fbw * 64, r->h, r->ctw, r->cth, r->csh);
        if (!(f = plat_debug_file(name)))
            continue;
        for (y = 0; y < (uint32_t)r->cth; y++)
            for (x = 0; x < (uint32_t)r->ctw; x++) {
                const uint8_t *p = t + rgba8_off(x, y, (uint32_t)r->ctw);
                uint8_t px[4] = {p[1], p[32], p[33], p[0]};
                fwrite(px, 1, 4, f);
            }
        fclose(f);
        LOG("gx: dumped %s (drawn %d,%d-%d,%d)", name, r->dx0, r->dy0, r->dx1, r->dy1);
    }
    if ((f = plat_debug_file("gsmem.bin"))) {
        fwrite(gs_mem, 1, 4u << 20, f);
        fclose(f);
    }
    LOG("gx: dumped %d targets and GS memory", g_nrt);
}

/* copy a target's EFB region to the next XFB and show it */
static void present(RT *r)
{
    if (g_post.active)
        post_end();
    batch_flush();
    g_frame_draws = 0;
    if (g_gx_rtdump) {
        g_gx_rtdump = 0;
        rt_dump_all();
    }
    if ((g_gx_flags & 2097152) && r->big && r->resident) {
        /* debugging: the cars' reflection map and the three views it is made of, over the picture */
        static const uint32_t fbps[4] = {0x2D60, 0x3160, 0x3260, 0x3360};
        Mtx m;
        int k;
        ortho_mode();
        guMtxIdentity(m);
        GX_LoadPosMtxImm(m, GX_PNMTX0);
        GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX3x4);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_FALSE);
        for (k = 0; k < 4; k++) {
            RT *t = rt_find(fbps[k]);
            float x0 = (float)(r->ex + 8 + 136 * k), y0 = (float)(r->ey + 70);
            if (!t)
                continue;
            if (t->resident && (t->dirty || !t->tex_valid))
                rt_copy(t, 0);
            if (!t->tex_valid)
                continue;
            GX_SetScissor((u32)r->ex, (u32)r->ey, BIG_W, BIG_H);
            post_tev(0, 0, 255, 0);
            post_quad(t->tex, t->ctw, t->cth, GX_TF_RGBA8, x0, y0, x0 + 128, y0 + 128, 0, 0, 1, 1);
        }
        {
            /* the frame (chain) number, 16 bits, most significant first: white 1, black 0 */
            extern uint32_t g_vif1_chains;
            static uint8_t px[2][32] __attribute__((aligned(32)));
            if (!px[1][0]) {
                memset(px[1], 0xFF, 32);
                DCFlushRange(px, sizeof px);
            }
            GX_SetScissor((u32)r->ex, (u32)r->ey, BIG_W, BIG_H);
            post_tev(0, 0, 255, 0);
            for (k = 0; k < 16; k++) {
                float x0 = (float)(r->ex + 160 + 20 * k), y0 = (float)(r->ey + 40);
                post_quad(px[(g_vif1_chains >> (15 - k)) & 1], 4, 4, GX_TF_I8, x0, y0, x0 + 16, y0 + 16, 0, 0, 1, 1);
            }
        }
        g_state_valid = 0;
        gxc_drop();
    }
    if (g_gx_meter && r->big && r->resident) {
        /* debugging: how long the last frame took, as white squares in the
         * top left corner: one a vblank (1: 60 fps, 2: 30, 3: 20, 4: 15 or
         * less), on four black ones. The game keeps its speed when frames
         * are slow (it updates several times a frame), so a slow frame does
         * not show as slow motion; it shows here. */
        static uint8_t px[2][32] __attribute__((aligned(32)));
        Mtx m;
        int k;
        if (!px[1][0]) {
            memset(px[1], 0xFF, 32);
            DCFlushRange(px, sizeof px);
        }
        ortho_mode();
        guMtxIdentity(m);
        GX_LoadPosMtxImm(m, GX_PNMTX0);
        GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX3x4);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_FALSE);
        GX_SetScissor((u32)r->ex, (u32)r->ey, BIG_W, BIG_H);
        post_tev(0, 0, 255, 0);
        for (k = 0; k < 4; k++) {
            float x0 = (float)(r->ex + 24 + 10 * k), y0 = (float)(r->ey + 20);
            post_quad(px[0], 4, 4, GX_TF_I8, x0 - 1, y0 - 1, x0 + 9, y0 + 9, 0, 0, 1, 1);
            if (k < (int)g_meter_vblanks)
                post_quad(px[1], 4, 4, GX_TF_I8, x0 + 1, y0 + 1, x0 + 7, y0 + 7, 0, 0, 1, 1);
        }
        if (g_gx_flags & 1073741824) {
            extern uint32_t g_vu1_particle_runs;
            if (g_vu1_particle_runs)
                g_mark_frame[2] = g_frame_no;
            g_vu1_particle_runs = 0;
            if (g_post.decay > 0x3B)
                g_mark_frame[4] = g_frame_no;
            if (g_frame_no & 1)
                g_mark_frame[6] = g_frame_no;
            for (k = 0; k < 8; k++) {
                float x0 = (float)(r->ex + 24 + 12 * k), y0 = (float)(r->ey + 36);
                post_quad(px[0], 4, 4, GX_TF_I8, x0 - 1, y0 - 1, x0 + 11, y0 + 11, 0, 0, 1, 1);
                if (g_mark_frame[k] == g_frame_no)
                    post_quad(px[1], 4, 4, GX_TF_I8, x0 + 1, y0 + 1, x0 + 9, y0 + 9, 0, 0, 1, 1);
            }
        }
        g_state_valid = 0;
        gxc_drop();
    }
    r->presented = r->draws;
    if (!r->big) {
        log_once("display buffer is a small target", r->fbp);
        return;
    }
    {
        /* The picture is 640x448 in a 480-line frame, as on the PS2: the
         * lines are not stretched, so the HUD at the bottom of the picture is
         * not in the television's overscan. Stretching the 448 lines to 480
         * put the HUD's last row in the frame's last line, where the
         * overscan cuts it. The lines below the picture stay black (the frame
         * buffers are cleared when they are taken). "tvfit 0": the old way,
         * stretched to the frame's height. */
        u32 lines;
        static int vf_set = -2;
        int vf = r->alias ? g_blit_vblend : -1;
        if (vf != vf_set) {
            /* an aliased blit's half-row blend (each row with the one above),
             * else the video mode's filter */
            static u8 blend[7] = {0, 0, 32, 32, 0, 0, 0}, none[7] = {0, 0, 0, 64, 0, 0, 0};
            vf_set = vf;
            GX_SetCopyFilter(g_mode->aa, g_mode->sample_pattern, GX_TRUE,
                             vf < 0 ? g_mode->vfilter : vf ? blend : none);
        }
        GX_SetDispCopySrc(0, 0, BIG_W, BIG_H);
        if (g_tv_fit) {
            GX_SetDispCopyYScale(1.0f);
            lines = BIG_H;
        } else {
            lines = GX_SetDispCopyYScale(GX_GetYScaleFactor(BIG_H, g_mode->xfbHeight));
        }
        if (lines > g_mode->xfbHeight)
            lines = g_mode->xfbHeight;
        GX_SetDispCopyDst(BIG_W, (u16)lines);
    }
    {
        /* Show the previous frame once the GPU is done with it (long since,
         * normally), copy this one and let the GPU finish it while the game
         * goes on: CPU and GPU overlap, the picture is a frame later. Three
         * buffers: the flip happens at the next retrace, and with two the
         * copy went into the buffer still on screen -- a fast copy showed
         * the new frame, then the retrace the older one (time rubberbanded).
         * Now the copy goes to the buffer shown two frames ago, which the
         * screen has left once a retrace passed since the last flip. */
        static int pending, last;
        int prev = pt_enter(PT_SYNC);
        if (pending) {
            uint32_t rc, vb;
            GX_WaitDrawDone();
            if (g_retraces == g_pace_rc)
                VIDEO_WaitVSync();      /* faster than the screen: the copy target may still be shown */
            VIDEO_SetNextFramebuffer(g_xfb[last]);
            VIDEO_Flush();
            rc = g_retraces;
            vb = kernel_vblank_count();
            g_pace_shown[rc - g_pace_rc < 4 ? rc - g_pace_rc : 4]++;
            g_pace_game[vb - g_pace_vb < 4 ? vb - g_pace_vb : 4]++;
            g_meter_vblanks = vb - g_pace_vb < 4 ? vb - g_pace_vb : 4;
            {
                extern uint32_t g_flip_count, g_flip_vblanks, g_flip_vb_total;  /* hle.c: the pacer's measure */
                g_flip_vblanks = vb - g_pace_vb;
                g_flip_vb_total += vb - g_pace_vb;
                g_flip_count++;
            }
            {
                /* debugging: where a race frame went that took a vblank more than the pacer's interval */
                extern int g_pace_interval;
                static const char *const names[PT_COUNT] = {"EE", "VU0", "VU1", "VIF", "GIF", "upload", "sync", "prim",
                                                            "flush", "tex", "GX", "idle", "kick", "native"};
                static uint64_t snap[PT_COUNT], nsnap[5];
                static uint32_t logged, lists, quads;
                char line[340];
                size_t m = 0;
                int k, ok = 1;
                uint16_t part[PT_COUNT];        /* the frame's time by part, 0.1 ms */
                line[0] = 0;
                for (k = 0; k < PT_COUNT; k++) {
                    part[k] = 0;
                    if (g_pt[k] < snap[k])
                        ok = 0;         /* the sums started over with a status line */
                    else {
                        uint64_t us = ticks_to_microsecs(g_pt[k] - snap[k]);
                        part[k] = us > 6553500 ? 65535 : (uint16_t)(us / 100);
                        if (g_pt[k] - snap[k] >= secs_to_ticks(1) / 2000)
                            m += (size_t)snprintf(line + m, sizeof line - m, " %s %.1f", names[k], (double)us / 1000.0);
                    }
                    snap[k] = g_pt[k];
                }
                if (g_frame_hist && g_pace_interval >= 1) {
                    /* debugging ("fhist 0": off): the frames around the first sparks after two quiet
                     * seconds (a collision begins), 96 before and 32 after, a line each: the vblanks it
                     * took (the game's count, the screen's), the pacer's interval, the update passes
                     * the game ran for it (ticks of its clock, 0x4E273C), VU0 calls, particle quads,
                     * display lists made, then the time by part */
                    extern uint32_t g_vu0_runs;
                    extern struct LongWait { uint32_t us, sid, fno; uint16_t id; uint8_t type, thread; } g_long_wait;
                    static struct FrameRec {
                        uint32_t no;
                        struct LongWait wait;
                        uint16_t part[PT_COUNT], quads, lists, vu0;
                        uint8_t vb, rc, pacer, passes, ok, owed;
                    } fh[128];
                    static uint32_t fh_n, fh_quiet, fh_at, fh_dumps, fh_tick, fh_vu0, fh_mark;
                    struct FrameRec *e = &fh[fh_n & 127];
                    uint32_t dq = g_spark_quads >= quads ? g_spark_quads - quads : g_spark_quads;
                    uint32_t tick = LW(0x004E273Cu);
                    e->no = g_frame_no;
                    memcpy(e->part, part, sizeof part);
                    e->quads = dq > 65535 ? 65535 : (uint16_t)dq;
                    e->lists = (uint16_t)(g_dl_built - lists);
                    e->vu0 = (uint16_t)(g_vu0_runs - fh_vu0);
                    e->vb = (uint8_t)(vb - g_pace_vb);
                    e->rc = (uint8_t)(rc - g_pace_rc);
                    e->pacer = (uint8_t)g_pace_interval;
                    e->passes = (uint8_t)(tick - fh_tick);
                    e->owed = (uint8_t)LW(0x0051BAC4u);     /* update passes the race loop was last told to run */
                    e->ok = (uint8_t)ok;
                    e->wait = g_long_wait;
                    memset(&g_long_wait, 0, sizeof g_long_wait);
                    {
                        /* a frame of a race with half as much game code again as the frames before it
                         * (15 ms at least, 6 over): where the 1 ms ticks found the processor in it */
                        extern uint32_t plat_tick_take(const uint32_t **samples);
                        static uint32_t ee16, spikes, spike_no;
                        const uint32_t *smp;
                        uint32_t ns = plat_tick_take(&smp), usual = ee16 / 16, ee = part[PT_EE], i;
                        if (ok && e->passes && e->passes <= e->vb && ee >= 150 && ee * 2 >= usual * 3 && ee >= usual + 60 &&
                            usual >= 30 && spikes < 40 && g_frame_no - spike_no >= 20) {
                            spikes++;
                            spike_no = g_frame_no;
                            LOG("spike frame %u: game code %u.%u ms (usual %u.%u), %u update passes, %u VU0 calls, %u lists made, "
                                "%u samples (pc, lr, return addresses):", g_frame_no, ee / 10, ee % 10, usual / 10, usual % 10,
                                e->passes, e->vu0, e->lists, ns);
                            for (i = 0; i < ns; i++)
                                LOG("  s %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)smp[i * 8], (unsigned)smp[i * 8 + 1],
                                    (unsigned)smp[i * 8 + 2], (unsigned)smp[i * 8 + 3], (unsigned)smp[i * 8 + 4],
                                    (unsigned)smp[i * 8 + 5], (unsigned)smp[i * 8 + 6], (unsigned)smp[i * 8 + 7]);
                        } else if (ok && e->passes && e->passes <= e->vb) {
                            ee16 += ee - usual;
                        }
                    }
                    {
                        /* what decides whether the race loop makes up for slow frames (0x132D40: update
                         * passes by the clock) or runs one pass a frame whatever it took: 0x51BA94 must be
                         * -1, 0x51BAD4 and 0x4EB1E0 set; the timer's own flag is 0x51BA84 (1: one pass) */
                        static uint32_t was = ~0u, changes;
                        uint32_t now = (LW(0x0051BA94u) == 0xFFFFFFFFu) | (uint32_t)(LB(0x0051BAD4u) != 0) << 1 |
                                       (uint32_t)(LB(0x004EB1E0u) != 0) << 2 | (uint32_t)(LB(0x0051BA84u) != 0) << 3 |
                                       (LW(0x0051BA50u) & 3) << 4 | (uint32_t)(LB(0x0051BAA6u) != 0) << 6 |
                                       (LW(0x0051BA70u) / 100000u) << 8;
                        if (now != was && changes < 200) {
                            changes++;
                            LOG("frame %u: update passes %s (0x51BA94 %08X, 0x51BAD4 %u, 0x4EB1E0 %u; timer flag %u, mode %u, "
                                "step %u, threshold %u, passes made %d, last %u, extra %d; 30 fps flag %u), %u owed",
                                g_frame_no, (now & 8) ? "one a frame" : "by the clock", (unsigned)LW(0x0051BA94u),
                                (unsigned)LB(0x0051BAD4u), (unsigned)LB(0x004EB1E0u), (unsigned)LB(0x0051BA84u),
                                (unsigned)LW(0x0051BA50u), (unsigned)LW(0x0051BA70u), (unsigned)LW(0x0051BA68u),
                                (int)LW(0x0051BA7Cu), (unsigned)LW(0x0051BA78u), (int)LW(0x0051BA80u),
                                (unsigned)LB(0x0051BAA6u), (unsigned)LW(0x0051BAC4u));
                        }
                        was = now;
                    }
                    {
                        /* a wait of half a vblank that is not the flip's: the game's loop falls a frame behind */
                        static uint32_t stalls;
                        if (e->wait.us >= 8000 && stalls < 80) {
                            stalls++;
                            LOG("frame %u: thread %u waited %u.%u ms (kind %u id %u; RPC %08X fn %u); %u lists made, textures "
                                "%u.%u ms, uploads %u.%u ms", g_frame_no, e->wait.thread, e->wait.us / 1000,
                                e->wait.us / 100 % 10, e->wait.type, e->wait.id, e->wait.sid, e->wait.fno, e->lists,
                                part[PT_TEX] / 10, part[PT_TEX] % 10, part[PT_XFER] / 10, part[PT_XFER] % 10);
                        }
                    }
                    fh_tick = tick;
                    fh_vu0 = g_vu0_runs;
                    if (dq >= 16) {
                        if (fh_quiet >= 120 && !fh_at && fh_dumps < 12) {
                            fh_at = fh_n + 32;
                            fh_mark = g_frame_no;
                        }
                        fh_quiet = 0;
                    } else {
                        fh_quiet++;
                    }
                    if (fh_at && fh_n == fh_at) {
                        uint32_t i;
                        fh_at = 0;
                        fh_dumps++;
                        LOG("frames around the first sparks (frame %u): frame, vblanks game/screen, pacer, update passes "
                            "owed/clock ticks, VU0 calls, particle quads, lists made, ms by part", fh_mark);
                        for (i = fh_n - 127; i != fh_n + 1; i++) {
                            const struct FrameRec *r2 = &fh[i & 127];
                            char fl[260];
                            size_t fm = 0;
                            if (i > fh_n)       /* fewer than 128 frames so far */
                                continue;
                            for (k = 0; k < PT_COUNT; k++)
                                if (r2->part[k] >= 5 && fm < sizeof fl - 24)
                                    fm += (size_t)snprintf(fl + fm, sizeof fl - fm, " %s %u.%u", names[k], r2->part[k] / 10,
                                                           r2->part[k] % 10);
                            fl[fm] = 0;
                            if (r2->wait.us >= 3000 && fm < sizeof fl - 80)
                                snprintf(fl + fm, sizeof fl - fm, "; thread %u waited %u.%u ms (kind %u id %u; RPC %08X fn %u)",
                                         r2->wait.thread, r2->wait.us / 1000, r2->wait.us / 100 % 10, r2->wait.type,
                                         r2->wait.id, r2->wait.sid, r2->wait.fno);
                            LOG("  %u%c v%u/%u p%u u%u/%u vu0 %u q %u l %u:%s%s", r2->no, r2->no == fh_mark ? '*' : ' ', r2->vb,
                                r2->rc, r2->pacer, r2->owed, r2->passes, r2->vu0, r2->quads, r2->lists, fl,
                                r2->ok ? "" : " (sums restarted)");
                        }
                    }
                    fh_n++;
                }
                if (g_gx_flags2 & 2097152) {
                    static const char *const parts[5] = {"car lighting", "crashed cars", "sparks", "smoke and embers", "world lighting"};
                    m += (size_t)snprintf(line + m, sizeof line - m, " (");
                    for (k = 0; k < 5; k++) {
                        m += (size_t)snprintf(line + m, sizeof line - m, "%s%s %.1f", k ? ", " : "", parts[k],
                                              (double)ticks_to_microsecs(g_nat_t[k] - nsnap[k]) / 1000.0);
                        nsnap[k] = g_nat_t[k];
                    }
                    m += (size_t)snprintf(line + m, sizeof line - m, ")");
                }
                if (ok && g_pace_interval >= 1 && vb - g_pace_vb > (uint32_t)g_pace_interval && logged < 160) {
                    logged++;
                    /* the game's own step (0x4E2740: 1/60 or 1/30 s) and the interval its vblank handler waits (0x4E17DC) */
                    LOG("slow frame %u: %u vblanks (pacer %d; the game's step %.4f s, interval %u), ms:%s; %u draws, %u lists made, "
                        "%u particle quads", g_frame_no, vb - g_pace_vb, g_pace_interval, (double)LWF(0x004E2740u),
                        (unsigned)LB(0x004E17DCu), line, g_draw_no, g_dl_built - lists, g_spark_quads - quads);
                    {
                        /* what happened when (hle.c, tl_add): the last 44 events, times in 0.1 ms from the first */
                        extern struct TlEvent { uint32_t t; uint8_t what; uint16_t arg; } g_tl[64];
                        extern uint32_t g_tl_n;
                        static int dumps;
                        if (dumps < 40 && g_frame_no > 7000 && g_tl_n >= 44 && (dumps < 6 || g_frame_no > 9000)) {
                            char tl[520];
                            size_t tn = 0;
                            uint32_t i, t0 = g_tl[(g_tl_n - 44) & 63].t;
                            dumps++;
                            for (i = g_tl_n - 44; i != g_tl_n && tn < sizeof tl - 16; i++) {
                                const struct TlEvent *e = &g_tl[i & 63];
                                if (e->what == 'V')
                                    tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " V%X@%u", e->arg, (e->t - t0) / 100);
                                else if (e->what == 'K')
                                    tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " K%u@%u", e->arg, (e->t - t0) / 100);
                                else if (e->what == 'I')
                                    tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " I%u@%u", e->arg, (e->t - t0) / 100);
                                else
                                    tn += (size_t)snprintf(tl + tn, sizeof tl - tn, " %c@%u", e->what, (e->t - t0) / 100);
                            }
                            LOG("  events:%s", tl);
                        }
                    }
                }
                lists = g_dl_built;
                quads = g_spark_quads;
            }
            g_pace_rc = rc;
            g_pace_vb = vb;
        }
        pt_leave(prev);
        if (g_gx_flags2 & 536870912) {
            static int dumped;
            calib_draw(r);        /* the pattern, over the picture, in the EFB */
            if (!dumped) {
                dumped = 1;
                calib_dump();
            }
        }
        GX_CopyDisp(g_xfb[g_xfb_cur], GX_FALSE);
        last = g_xfb_cur;
        GX_SetDrawDone();
        GX_Flush();
        pending = 1;
        /* (a race: the game's 60 fps mode, 0x4E2CFC 0; its clock counts from the race's start) */
        if (PS2_UNLIKELY(g_gx_freeze) && LW(0x004E2CFCu) == 0 && LW(0x004E273Cu) >= (uint32_t)g_gx_freeze &&
            LW(0x004E273Cu) < (uint32_t)g_gx_freeze + 600) {
            extern FILE *plat_debug_file(const char *name);
            const uint32_t size = VIDEO_GetFrameBufferSize(g_mode);
            FILE *fz;
            GX_WaitDrawDone();
            VIDEO_SetNextFramebuffer(g_xfb[last]);
            VIDEO_Flush();
            VIDEO_WaitVSync();
            /* the picture as the display buffer has it (YUV 4:2:2), for a comparison byte by byte */
            fz = plat_debug_file("frz.bin");
            if (fz) {
                fwrite(g_xfb[last], 1, size, fz);
                fclose(fz);
            }
            LOG("gx: frozen at game tick %u (frame %u, %u world draws), the display buffer (%u bytes, %ux%u) in frz.bin",
                LW(0x004E273Cu), g_frame_no, g_wh_n, size, g_mode->fbWidth, g_mode->xfbHeight);
            for (;;)
                VIDEO_WaitVSync();
        }
    }
    g_xfb_cur = (g_xfb_cur + 1) % 3;
    {
        /* a frame whose car batches fell to under a quarter of the last one's */
        extern uint32_t g_vif_deferred[4];
        static uint32_t last_car, last_emu, logged;
        uint32_t emu = g_vif_deferred[1] >= last_emu ? g_vif_deferred[1] - last_emu : g_vif_deferred[1];
        if (last_car >= 20 && g_fm_car * 4 < last_car && logged < 30) {
            logged++;
            LOG("frame monitor: frame %u has %u car batches drawn (last frame %u): %u first passes skipped, %u failed, "
                "%u sent to VU1, %u big targets evicted", g_frame_no, g_fm_car, last_car, g_fm_car_skip, g_fm_car_fail,
                emu, g_fm_big_out);
        }
        if (g_gx_flags & 67108864)
            car_watch_frame();
        last_car = g_fm_car;
        last_emu = g_vif_deferred[1];
        g_fm_car = g_fm_car_skip = g_fm_car_fail = g_fm_big_out = 0;
    }
    g_frame_no++;
    {
        /* debugging (autopilot "statdraws N"): gsstat over the next two
         * fields, once, after the first frame of more than N draws */
        extern int g_gs_stat;
        static uint32_t last, fired;
        uint32_t total = g_draws_before + g_draw_no;
        if (g_gs_statdraws && !fired && !g_gs_stat && total - last > (uint32_t)g_gs_statdraws) {
            fired = 1;
            g_gs_stat = 2;
            LOG("gsstat: a frame of %u draws", total - last);
        }
        last = total;
    }
}
