/*
 * Patches of the game's data, as PCSX2 applies its .pnach files: writes into guest RAM after the executable is
 * loaded and again at every vblank (pnach "patch=1"). Only data can be patched this way: the game's code is
 * recompiled to C, so a write into its .text changes nothing; the parts of a patch that change instructions need the
 * recompiler (listed with each set below).
 *
 * Widescreen 16:9 (args.txt "--widescreen N", autopilot "widescreen N"; PC: B3_WIDESCREEN=N; 0 by default): the
 * data part of PCSX2's patch for this disc (pcsx2_patches, SLUS-21050_BEBF8793.pnach, "Widescreen 16:9" by Aero_:
 * "Corrects the aspect ratio of the FOV, HUD, and FMVs for 16:9"). The game's own widescreen switch (0x665ECD, next
 * to its bloom and motion blur switches), the FOV and aspect values for one and two players, the HUD's width, the
 * HUD's X place for each player and its corners, the crash cam's borders, the car select's overlay, the globe and
 * its glow. The picture stays 640 wide, squeezed (anamorphic) as a PS2 game's 16:9 is: the TV (or Dolphin's
 * aspect ratio setting, or the Wii's own 16:9 setting) stretches it.
 *
 * The patch's other half changes instructions: the HUD's render width and X place per player (0x1A176C..0x1A17D8),
 * "Get Ready! GO" (0x134F2C, 0x134F74), the menus' width (0x30D7E4, 0x30D834), the top and bottom borders
 * (0x31B180.., 0x38AE00..), the checkerboards (0x31D6E4..), the loading background (0x31DA20), the replay bars
 * (0x3A6984..), the crash cam list (0x3D7238..). A write into the game's code changes nothing here, so the
 * recompiler compiles both forms of those instructions in (the config's CODE_PATCHES, tools/recomp/patches.py) and
 * bit 0 of g_code_patch picks the patched one: set with the data half. Code generated before that (gen/ from
 * an older recompiler) has no sites, and those parts stay as wide as for a 4:3 screen.
 */
#include "rt.h"

typedef struct GamePatch {
    uint32_t addr, value;
    uint8_t size;                       /* 1 or 4 bytes */
} GamePatch;

static const GamePatch k_widescreen[] = {
    {0x00665ECDu, 0x01u, 1},            /* the game's widescreen FOV values on */
    {0x004E0A38u, 0x3FAAAAABu, 4},      /* FOV, one player: 1.33333 */
    {0x004E0C70u, 0x3FE38E39u, 4},      /* aspect, one player: 1.77778 */
    {0x004E0C7Cu, 0x3FE38E39u, 4},      /* FOV, two players: 1.77778 */
    {0x004E0C80u, 0x40638E39u, 4},      /* aspect, two players: 3.55556 */
    {0x004E105Cu, 0x3A99999Au, 4},      /* HUD width: 0.00117188 */
    {0x006682B0u, 0x42A00000u, 4},      /* HUD X place, player 1: +80 */
    {0x00669B30u, 0x42A00000u, 4},      /* HUD X place, player 2: +80 */
    {0x004B7688u, 0x3F9947AEu, 4},      /* HUD top right X: +0.1975 */
    {0x004B7678u, 0x3F9947AEu, 4},      /* HUD bottom right X */
    {0x004B7658u, 0xBE4A3D71u, 4},      /* HUD top left X: -0.1975 */
    {0x004B7668u, 0xBE4A3D71u, 4},      /* HUD bottom left X */
    {0x004CA660u, 0x44555555u, 4},      /* crash cam border width */
    {0x004CA640u, 0x44555555u, 4},
    {0x004CA650u, 0x44555555u, 4},
    {0x004CA638u, 0xC2D70000u, 4},      /* crash cam border X: -107.5 */
    {0x004CA658u, 0xC2D70000u, 4},
    {0x004D1570u, 0x44555555u, 4},      /* car select overlay width: 853.3 */
    {0x004D1568u, 0xC2D70000u, 4},      /* car select overlay X: -107.5 */
    {0x004E0C3Cu, 0x3FE38E39u, 4},      /* globe aspect: 1.77778 */
    {0x004E0A94u, 0x3FB8E38Fu, 4},      /* globe glow aspect: 1.44444 */
};
#define WS_N (sizeof k_widescreen / sizeof k_widescreen[0])

int g_widescreen;                       /* 0 off, 1 on (the platform turns its 2, "as the Wii's settings", into 0 or 1) */
uint32_t g_code_patch;                  /* sets of code patches on (ps2_recomp.h): bit 0 widescreen */
static uint32_t g_ws_saved[WS_N];       /* what was there before the first write, for turning it off again */
static int g_ws_applied;

static uint32_t patch_read(const GamePatch *p)
{
    return p->size == 1 ? g_r8(p->addr) : g_r32(p->addr);
}

static void patch_write(const GamePatch *p, uint32_t v)
{
    if (p->size == 1)
        g_w8(p->addr, (uint8_t)v);
    else
        g_w32(p->addr, v);
}

/* After the executable is loaded (main.c) and at every vblank (kernel.c): the set written while it is on, what was
 * there written back once when it is turned off (the places the game rewrites itself, the HUD's X places, take the
 * game's values again at its next write) */
void game_patches_apply(void)
{
    size_t k;
    g_code_patch = g_widescreen ? 1u : 0u;
    if (g_widescreen) {
        if (!g_ws_applied) {
            for (k = 0; k < WS_N; k++)
                g_ws_saved[k] = patch_read(&k_widescreen[k]);
            g_ws_applied = 1;
            LOG("widescreen 16:9: PCSX2's patch on (%u data writes a vblank; its code half where gen/ has the "
                "sites: runtime/patches.c)", (unsigned)WS_N);
        }
        for (k = 0; k < WS_N; k++)
            patch_write(&k_widescreen[k], k_widescreen[k].value);
    } else if (g_ws_applied) {
        for (k = 0; k < WS_N; k++)
            patch_write(&k_widescreen[k], g_ws_saved[k]);
        g_ws_applied = 0;
        LOG("widescreen 16:9: off, the values before it written back");
    }
}
