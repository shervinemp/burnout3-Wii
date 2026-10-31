/*
 * GS emulation core: GIF paths, register file, vertex queue, local memory and
 * transfers. Drawing is delegated to the backend (gs_backend_draw).
 *
 * The block/column swizzle tables below are derived from PCSX2's
 * GSTables.cpp (Copyright (C) 2002-2026 PCSX2 Dev Team, GPL-3.0-or-later).
 * See THIRD_PARTY_NOTICES.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gs.h"
#include "hw.h"
#include "wtime.h"

#include <stdio.h>
#include <string.h>

#define GS_MEM_WORDS (4u * 1024 * 1024 / 4)
#define block_of gs_block_of

uint32_t *gs_mem;
uint32_t gs_clut[512];
static uint64_t g_regs[0x100];
static uint64_t g_cbp0, g_cbp1;
#if !defined(GEKKO)
static void rtdep_clut(uint32_t cbp, uint32_t cbw, uint32_t psm, uint32_t w, uint32_t h);
#define RTDEP(x) x
#else
#define RTDEP(x)
#endif

/* ---- swizzle tables (as in PCSX2's GSTables.cpp) -------------------------------- */

static const uint8_t block32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
static const uint8_t block16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
static const uint8_t block16s[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
static const uint8_t column32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
static const uint8_t column16[8][16] = {
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
    {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
    {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59},
    {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
    {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91},
    {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
    {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123},
    {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
static const uint8_t column8[16][16] = {
    {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
    {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
    {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
    {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
    {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
    {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
    {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
    {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
    {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
    {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
    {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
    {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
    {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
    {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
    {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255}};
static const uint16_t column4[16][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
    {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
    {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
    {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
    {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
    {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
    {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
    {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
    {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
    {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
    {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
    {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
    {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511}};

/* word index for 32-bit formats */
uint32_t gs_addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t page = (y >> 5) * bw + (x >> 6);
    uint32_t block = block32[(y >> 3) & 3][(x >> 3) & 7];
    return (bp * 64 + page * 2048 + block * 64 + column32[y & 7][x & 7]) & (GS_MEM_WORDS - 1);
}

uint32_t gs_addr32z(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t page = (y >> 5) * bw + (x >> 6);
    uint32_t block = block32[(y >> 3) & 3][(x >> 3) & 7] ^ 24u;
    return (bp * 64 + page * 2048 + block * 64 + column32[y & 7][x & 7]) & (GS_MEM_WORDS - 1);
}

/* halfword index for 16-bit formats */
static uint32_t addr16_tab(const uint8_t (*bt)[4], uint32_t bxor, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t page = (y >> 6) * bw + (x >> 6);
    uint32_t block = bt[(y >> 3) & 7][(x >> 4) & 3] ^ bxor;
    return (bp * 128 + page * 4096 + block * 128 + column16[y & 7][x & 15]) & (GS_MEM_WORDS * 2 - 1);
}

uint32_t gs_addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return addr16_tab(block16, 0, bp, bw, x, y); }
uint32_t gs_addr16s(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return addr16_tab(block16s, 0, bp, bw, x, y); }
uint32_t gs_addr16z(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return addr16_tab(block16, 24, bp, bw, x, y); }
uint32_t gs_addr16sz(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) { return addr16_tab(block16s, 24, bp, bw, x, y); }

/* byte index for 8-bit textures; bw in units of 64 pixels, pages are 128 wide */
uint32_t gs_addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t page = (y >> 6) * ((bw + 1) >> 1) + (x >> 7);
    uint32_t block = block32[(y >> 4) & 3][(x >> 4) & 7];
    return (bp * 256 + page * 8192 + block * 256 + column8[y & 15][x & 15]) & (GS_MEM_WORDS * 4 - 1);
}

/* nibble index for 4-bit textures; pages are 128x128 */
uint32_t gs_addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t page = (y >> 7) * ((bw + 1) >> 1) + (x >> 7);
    uint32_t block = block16[(y >> 4) & 7][(x >> 5) & 3];
    return (bp * 512 + page * 16384 + block * 512 + column4[y & 15][x & 31]) & (GS_MEM_WORDS * 8 - 1);
}

/* local memory is little-endian like the GS: halfword/byte views pick lanes by value */
static inline uint32_t mem_r16(uint32_t hidx) { return (gs_mem[hidx >> 1] >> ((hidx & 1) * 16)) & 0xFFFF; }
static inline void mem_w16(uint32_t hidx, uint32_t v)
{
    uint32_t *w = &gs_mem[hidx >> 1];
    uint32_t sh = (hidx & 1) * 16;
    *w = (*w & ~(0xFFFFu << sh)) | ((v & 0xFFFF) << sh);
}
static inline uint32_t mem_r8(uint32_t bidx) { return (gs_mem[bidx >> 2] >> ((bidx & 3) * 8)) & 0xFF; }
static inline void mem_w8(uint32_t bidx, uint32_t v)
{
    uint32_t *w = &gs_mem[bidx >> 2];
    uint32_t sh = (bidx & 3) * 8;
    *w = (*w & ~(0xFFu << sh)) | ((v & 0xFF) << sh);
}
static inline uint32_t mem_r4(uint32_t nidx) { return (gs_mem[nidx >> 3] >> ((nidx & 7) * 4)) & 0xF; }
static inline void mem_w4(uint32_t nidx, uint32_t v)
{
    uint32_t *w = &gs_mem[nidx >> 3];
    uint32_t sh = (nidx & 7) * 4;
    *w = (*w & ~(0xFu << sh)) | ((v & 0xF) << sh);
}

uint32_t gs_read_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    switch (psm) {
    case PSMCT32: return gs_mem[gs_addr32(bp, bw, x, y)];
    case PSMCT24: return gs_mem[gs_addr32(bp, bw, x, y)] & 0xFFFFFF;
    case PSMCT16: return mem_r16(gs_addr16(bp, bw, x, y));
    case PSMCT16S: return mem_r16(gs_addr16s(bp, bw, x, y));
    case PSMT8: return mem_r8(gs_addr8(bp, bw, x, y));
    case PSMT4: return mem_r4(gs_addr4(bp, bw, x, y));
    case PSMT8H: return gs_mem[gs_addr32(bp, bw, x, y)] >> 24;
    case PSMT4HL: return (gs_mem[gs_addr32(bp, bw, x, y)] >> 24) & 0xF;
    case PSMT4HH: return gs_mem[gs_addr32(bp, bw, x, y)] >> 28;
    case PSMZ32: return gs_mem[gs_addr32z(bp, bw, x, y)];
    case PSMZ24: return gs_mem[gs_addr32z(bp, bw, x, y)] & 0xFFFFFF;
    case PSMZ16: return mem_r16(gs_addr16z(bp, bw, x, y));
    case PSMZ16S: return mem_r16(gs_addr16sz(bp, bw, x, y));
    default: return 0;
    }
}

void gs_write_pixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v)
{
    uint32_t *w;
    switch (psm) {
    case PSMCT32: gs_mem[gs_addr32(bp, bw, x, y)] = v; break;
    case PSMCT24: w = &gs_mem[gs_addr32(bp, bw, x, y)]; *w = (*w & 0xFF000000u) | (v & 0xFFFFFF); break;
    case PSMCT16: mem_w16(gs_addr16(bp, bw, x, y), v); break;
    case PSMCT16S: mem_w16(gs_addr16s(bp, bw, x, y), v); break;
    case PSMT8: mem_w8(gs_addr8(bp, bw, x, y), v); break;
    case PSMT4: mem_w4(gs_addr4(bp, bw, x, y), v); break;
    case PSMT8H: w = &gs_mem[gs_addr32(bp, bw, x, y)]; *w = (*w & 0x00FFFFFFu) | (v << 24); break;
    case PSMT4HL: w = &gs_mem[gs_addr32(bp, bw, x, y)]; *w = (*w & 0xF0FFFFFFu) | ((v & 0xF) << 24); break;
    case PSMT4HH: w = &gs_mem[gs_addr32(bp, bw, x, y)]; *w = (*w & 0x0FFFFFFFu) | ((v & 0xF) << 28); break;
    case PSMZ32: gs_mem[gs_addr32z(bp, bw, x, y)] = v; break;
    case PSMZ24: w = &gs_mem[gs_addr32z(bp, bw, x, y)]; *w = (*w & 0xFF000000u) | (v & 0xFFFFFF); break;
    case PSMZ16: mem_w16(gs_addr16z(bp, bw, x, y), v); break;
    case PSMZ16S: mem_w16(gs_addr16sz(bp, bw, x, y), v); break;
    default: break;
    }
}

/* ---- textures and CLUT -------------------------------------------------------------- */

static uint32_t expand16(uint32_t c, uint64_t texa, int aem)
{
    uint32_t r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
    uint32_t a;
    if (aem && !(c & 0x7FFF))
        a = 0;
    else
        a = (c & 0x8000) ? GS_FIELD(texa, 32, 8) : GS_FIELD(texa, 0, 8);
    return r | g << 8 | b << 16 | a << 24;
}

/* CLUT loads, cached: a game loads the same palettes again and again (at
 * every texture bind), from a few blocks of GS memory each. Key: what those
 * blocks hold (gs_block_ident) and how the load reads them; value: the
 * entries. gs_clut_ver changes only when the CLUT buffer's content does. */
typedef struct ClutEnt {
    uint32_t key, key2;
    uint16_t n, off;
    uint32_t ser, hash;                 /* see g_clut_slot */
    uint32_t e[256];
} ClutEnt;
#define CLUT_CACHE 128
static ClutEnt g_clut_cache[CLUT_CACHE];
/* A race frame loads some 110 different palettes, 150 loads: one entry a
 * key in 128 had two loads in five miss (read from memory again, compared,
 * copied). The 256-entry ones are two to a set now (the one used last
 * stays), the 16-entry ones, two thirds of them, have a table of their own,
 * and the key is what the blocks hold and how they are read, not where they
 * are: the game moves a texture and its palette about its cache. */
typedef struct ClutSmall {
    uint32_t key, key2;
    uint16_t n, off;
    uint32_t ser, hash;
    uint32_t e[16];
} ClutSmall;
#define CLUT_SMALL 512
static ClutSmall g_clut_small[CLUT_SMALL];
static uint8_t g_clut_mru[CLUT_CACHE / 2];
uint32_t g_clut_hits, g_clut_misses;    /* status line */
uint32_t gs_block_ident(uint32_t block);

static int clut_from_upload(uint32_t cbp, uint32_t rpsm, uint32_t w, uint32_t h, int entries, uint32_t *vals);
static void clut_chk_owner(uint32_t cbp, uint32_t rpsm);
static void clut_blk_state(uint32_t cbp, uint32_t rpsm, uint32_t key);
static void clut_hit_log(uint32_t cbp, uint32_t rpsm, int entries, const uint32_t *hit, const uint32_t *mem);
static void clut_chk_log(uint32_t cbp, uint32_t rpsm, int entries, const uint32_t *up, const uint32_t *mem);
int g_gs_clut_up = 1;                   /* debugging (autopilot "clutup 0"): CLUTs only from GS memory */
int g_gs_ext_on = 1;                    /* debugging (autopilot "extup 0"): uploads always staged and hashed */
int g_gs_clut_cache = 1;                /* debugging (autopilot "clutcache 0"): every CLUT load reads the entries */
int g_gs_clut_chk;                      /* debugging (autopilot "clutchk 1"): CLUTs from uploads checked against GS
                                         * memory; 4: from uploads, the blocks written to GS memory after */
static uint32_t g_clut_up_vals[256];

/* What the CLUT buffer holds, by 16-entry slot: the cached load it was copied
 * from (that load's serial, new each time its entries are read; 0: not known),
 * and at a load's first slot its size and the hash of its entries. The game
 * binds a texture's palette again for every batch that uses it: a load of
 * what is there already was a compare of all of it (1% of a race frame), and
 * the backend hashed the 256 entries again at every change. */
static uint32_t g_clut_ser, g_clut_slot[32];
static struct { uint32_t ser, hash; uint16_t n; } g_clut_at[32];
uint32_t g_clut_same;                   /* status line: loads of what the buffer held */

static uint32_t clut_fold(const uint32_t *e, int n)
{
    uint32_t h = 0x5BD1E995u;
    int i;
    for (i = 0; i < n; i++) {
        h = (h ^ e[i]) * 0x9E3779B1u;
        h ^= h >> 15;
    }
    return h ? h : 1;
}

static int clut_holds(uint32_t off, int entries, uint32_t ser)
{
    uint32_t s = off >> 4, k, n = (uint32_t)entries >> 4;
    if (!ser || (off & 15))
        return 0;
    for (k = 0; k < n; k++)
        if (g_clut_slot[(s + k) & 31] != ser)
            return 0;
    return 1;
}

static void clut_note(uint32_t off, int entries, uint32_t ser, uint32_t hash)
{
    uint32_t s = off >> 4, k, n = (uint32_t)entries >> 4;
    if (off & 15)
        ser = 0;
    for (k = 0; k < n; k++)
        g_clut_slot[(s + k) & 31] = ser;
    g_clut_at[s & 31].ser = ser;
    g_clut_at[s & 31].hash = hash;
    g_clut_at[s & 31].n = (uint16_t)entries;
}

/* A cached load that changes the buffer is not copied until something reads the buffer: what the game does with a
 * palette is mostly a TLUT the backend has (gs_clut_hash knows the hash of a whole load, so the lookup does not read
 * the entries), and the copy was 1 KB of cold lines for each of some 50 such loads a race frame (1700 L2 misses).
 * g_clut_pend is the one copy still to make (src: a cache entry, which is not written before it is made). */
static struct { const uint32_t *src; uint32_t off, n; } g_clut_pend;
int g_gs_clut_lazy = 1;                 /* debugging (autopilot "clutlazy 0"): every load copied at once; 2: lazy, and each
                                         * decision and the buffer at every read checked against an eager copy */
static uint32_t g_clut_shadow[512];
static int g_clut_shadow_ok;
uint32_t g_clut_lazy_n, g_clut_sync_n;  /* status line: loads whose copy waited, copies made when read */

void gs_clut_sync(void)
{
    if (g_clut_pend.src) {
        memcpy(&gs_clut[g_clut_pend.off], g_clut_pend.src, (size_t)g_clut_pend.n * 4);
        g_clut_pend.src = NULL;
        g_clut_sync_n++;
    }
    if (g_gs_clut_lazy == 2 && g_clut_shadow_ok && memcmp(gs_clut, g_clut_shadow, sizeof gs_clut)) {
        static int logged;
        unsigned k = 0;
        while (gs_clut[k] == g_clut_shadow[k])
            k++;
        if (logged++ < 8)
            LOG("clutlazy: the buffer differs from the eager copy at entry %u: %08X, not %08X", k, gs_clut[k], g_clut_shadow[k]);
        memcpy(g_clut_shadow, gs_clut, sizeof gs_clut);
    }
}

/* an eager write of the buffer (anything waiting is made first) */
static void clut_put(uint32_t off, const uint32_t *src, int n)
{
    gs_clut_sync();
    memcpy(&gs_clut[off], src, (size_t)n * 4);
    if (g_gs_clut_lazy == 2 && g_clut_shadow_ok)
        memcpy(&g_clut_shadow[off], src, (size_t)n * 4);
}

/* A load of a cached entry the buffer may not hold: whether it does is known by the hashes of whole loads (clut_holds: the
 * same hash is the same entries) or else compared, where the first word that differs ends it. A change (gs_clut_ver) has
 * its copy wait. 0: lazy copies are off, the caller does it. */
static int clut_defer(uint32_t off, int entries, const uint32_t *ee, uint32_t hash)
{
    const uint32_t s = off >> 4;
    int differs;
    if (!g_gs_clut_lazy)
        return 0;
    if (!(off & 15) && g_clut_at[s].n == entries && clut_holds(off, entries, g_clut_at[s].ser)) {
        differs = g_clut_at[s].hash != hash;
    } else {
        const uint32_t *cur = &gs_clut[off];
        if (g_clut_pend.src && g_clut_pend.off == off && g_clut_pend.n == (uint32_t)entries)
            cur = g_clut_pend.src;      /* what the buffer will hold there */
        else
            gs_clut_sync();
        differs = memcmp(cur, ee, (size_t)entries * 4) != 0;
    }
    if (g_gs_clut_lazy == 2) {
        int would;
        if (!g_clut_shadow_ok) {
            gs_clut_sync();
            memcpy(g_clut_shadow, gs_clut, sizeof gs_clut);
            g_clut_shadow_ok = 1;
        }
        would = memcmp(&g_clut_shadow[off], ee, (size_t)entries * 4) != 0;
        if (would != differs) {
            static int logged;
            if (logged++ < 8)
                LOG("clutlazy: a load at %u (%d entries) %s by hash, %s by content", off, entries, differs ? "differs" : "is the same",
                    would ? "differs" : "is the same");
        }
        memcpy(&g_clut_shadow[off], ee, (size_t)entries * 4);
    }
    if (differs) {
        if (g_clut_pend.src && !(off <= g_clut_pend.off && g_clut_pend.off + g_clut_pend.n <= off + (uint32_t)entries))
            gs_clut_sync();             /* (one covering it: it is never read) */
        g_clut_pend.src = ee;
        g_clut_pend.off = off;
        g_clut_pend.n = (uint32_t)entries;
        gs_clut_ver++;
        g_clut_lazy_n++;
    }
    return 1;
}

/* a hash of entries [base, base + n) of the CLUT buffer: the load's own when
 * one load put exactly those there */
uint32_t gs_clut_hash(uint32_t base, int n)
{
    uint32_t t[256];
    int i;
    if (!(base & 15) && base < 512 && g_clut_at[base >> 4].n == n && clut_holds(base, n, g_clut_at[base >> 4].ser))
        return g_clut_at[base >> 4].hash;
    gs_clut_sync();
    if (n > 256)
        n = 256;
    if (base + (uint32_t)n <= 512)
        return clut_fold(&gs_clut[base], n);
    for (i = 0; i < n; i++)
        t[i] = gs_clut[(base + (uint32_t)i) & 511];
    return clut_fold(t, n);
}

#ifndef NO_TMEMO2
/* gs_clut_hash(base, n) when one load put exactly those entries there (1, *h set), else 0 and nothing read or copied: the
 * validated texture memo (autopilot "tmemo", gs_gx.c) gives up on a palette whose hash would have to be folded */
int gs_clut_hash_cheap_h(uint32_t base, int n, uint32_t *h)
{
    if (!(base & 15) && base < 512 && g_clut_at[base >> 4].n == n && clut_holds(base, n, g_clut_at[base >> 4].ser)) {
        *h = g_clut_at[base >> 4].hash;
        return 1;
    }
    return 0;
}
#endif

#ifdef MEMOSTAT
/* -DMEMOSTAT (debug, plan-60fps.md): whether gs_clut_hash(base, n) would answer from one load's hash, without folding the entries
 * or making the pending copy */
int gs_clut_hash_cheap(uint32_t base, int n)
{
    return !(base & 15) && base < 512 && g_clut_at[base >> 4].n == n && clut_holds(base, n, g_clut_at[base >> 4].ser);
}
#endif

/* The last loads that left the CLUT buffer holding what they read: the same load again (the same CLUT fields of
 * TEX0, the same TEXCLUT, GS memory and the buffer unchanged since: gs_ver, gs_clut_ver) puts there what is there.
 * A car object's GS packet binds its passes' textures at every one of its batches' groups, the world's group setup
 * the same texture again and again: some 330 loads a frame in a race, most of them of what the buffer holds. */
#define CLUT_FIELDS (0x3Full << 20 | 0xFFFFFFull << 37)        /* PSM; CBP CPSM CSM CSA */
/* Autopilot "newopt 0": the exact speedups of 2026-10-15 (memos of GS and GX state, the CLUT repeats, the draw state
 * captured only when it changed, the cars' normals from their lists' copies, the speed blur's ramp kept) left out:
 * the old ways, for comparisons in one build (the same frames from where it is switched). */
int g_opt_new = 1;
static struct { uint64_t tex0, texclut; uint32_t ver, cver; } g_clut_last[2];
static uint32_t g_clut_last_n;
uint32_t g_clut_repeats;                /* status line */

#ifdef WTIME
static void gs_load_clut_inner(uint64_t tex0);
void gs_load_clut(uint64_t tex0)
{
    WT_MARK_IN(wm);
    gs_load_clut_inner(tex0);
    WT_DONE_IN(clut, wm);
}
static void gs_load_clut_inner(uint64_t tex0)
#else
void gs_load_clut(uint64_t tex0)
#endif
{
    uint32_t psm = GS_FIELD(tex0, 20, 6), cbp = GS_FIELD(tex0, 37, 14), cpsm = GS_FIELD(tex0, 51, 4);
    uint32_t csm = GS_FIELD(tex0, 55, 1), csa = GS_FIELD(tex0, 56, 5), cld = GS_FIELD(tex0, 61, 3);
    uint32_t key, key2, off, rpsm, cbw, w, h, x, y;
    uint32_t vals[256];
    uint32_t *ek = NULL, *ek2 = NULL, *ee = NULL, *es = NULL, *eh = NULL;
    uint16_t *en = NULL, *eo = NULL;
    int entries, i, hit_chk = 0;
    if (psm != PSMT8 && psm != PSMT8H && psm != PSMT4 && psm != PSMT4HL && psm != PSMT4HH)
        return;
    switch (cld) {
    case 0: return;
    case 1: break;
    case 2: g_cbp0 = cbp; break;
    case 3: g_cbp1 = cbp; break;
    case 4: if (cbp == g_cbp0) return; g_cbp0 = cbp; break;
    case 5: if (cbp == g_cbp1) return; g_cbp1 = cbp; break;
    default: return;
    }
    entries = (psm == PSMT8 || psm == PSMT8H) ? 256 : 16;
    RTDEP(rtdep_clut(cbp, csm ? GS_FIELD(g_regs[GS_TEXCLUT], 0, 6) : 1, cpsm == 0 ? PSMCT32 : PSMCT16,
                     csm ? 1024 : entries == 256 ? 16 : 8, csm ? 1 : entries == 256 ? 16 : 2));
    gs_backend_sync(cbp, csm ? GS_FIELD(g_regs[GS_TEXCLUT], 0, 6) : 1, cpsm == 0 ? PSMCT32 : PSMCT16,
                    csm ? 1024 : entries == 256 ? 16 : 8, csm ? 1 : entries == 256 ? 16 : 2);
    if (!g_gs_clut_chk && g_opt_new)
        for (i = 0; i < 2; i++)
            if (!((g_clut_last[i].tex0 ^ tex0) & CLUT_FIELDS) && g_clut_last[i].ver == gs_ver &&
                g_clut_last[i].cver == gs_clut_ver && (!csm || g_clut_last[i].texclut == g_regs[GS_TEXCLUT]) &&
                g_clut_last[i].tex0) {
                g_clut_repeats++;
                return;
            }
    /* where the entries go in the CLUT buffer, and what they are read from */
    if (csm) {
        off = cpsm == 0 ? 0 : csa * 16;
        rpsm = cpsm == 0 ? PSMCT32 : PSMCT16;
        cbw = GS_FIELD(g_regs[GS_TEXCLUT], 0, 6);
        w = (uint32_t)entries;
        h = 1;
    } else {
        off = entries == 256 ? 0 : cpsm == 0 ? (csa & 15) * 16 : csa * 16;
        rpsm = cpsm == 0 ? PSMCT32 : cpsm == 2 ? PSMCT16 : PSMCT16S;
        cbw = 1;
        w = entries == 256 ? 16 : 8;
        h = entries == 256 ? 16 : 2;
    }
    key = ((csm ? cbp : 0) | rpsm << 14 | (uint32_t)entries << 20 | csm << 29) * 0x9E3779B1u;
    key2 = off | (csm ? (uint32_t)(g_regs[GS_TEXCLUT] & 0x3FFFFF) << 9 : 0);
    if (g_gs_lazy && g_gs_clut_cache) {
        uint32_t dw = rpsm == PSMCT32 ? 8 : 16, dh = 8, xs = csm ? GS_FIELD(g_regs[GS_TEXCLUT], 6, 6) * 16 : 0;
        uint32_t ys = csm ? GS_FIELD(g_regs[GS_TEXCLUT], 12, 10) : 0;
        for (y = ys - ys % dh; y < ys + h; y += dh)
            for (x = xs - xs % dw; x < xs + w; x += dw)
                key = (key ^ gs_block_ident(block_of(rpsm, cbp, cbw, x, y))) * 0x85EBCA6Bu;
        key ^= key >> 15;
        if (entries == 16) {
            ClutSmall *cs = &g_clut_small[(key ^ key2) % CLUT_SMALL];
            ek = &cs->key;
            ek2 = &cs->key2;
            en = &cs->n;
            eo = &cs->off;
            es = &cs->ser;
            eh = &cs->hash;
            ee = cs->e;
        } else {
            const uint32_t set = (key ^ key2) % (CLUT_CACHE / 2);
            ClutEnt *a = &g_clut_cache[2 * set], *b = a + 1, *ce;
            ce = a->n == entries && a->key == key && a->key2 == key2 ? a :
                 b->n == entries && b->key == key && b->key2 == key2 ? b : g_clut_mru[set] ? a : b;
            g_clut_mru[set] = (uint8_t)(ce - a);
            ek = &ce->key;
            ek2 = &ce->key2;
            en = &ce->n;
            eo = &ce->off;
            es = &ce->ser;
            eh = &ce->hash;
            ee = ce->e;
        }
        if (*en == entries && *ek == key && *ek2 == key2 && g_gs_clut_chk == 8) {
            /* debugging: the hit read again from GS memory and compared */
            memcpy(g_clut_up_vals, ee, (size_t)entries * 4);
            clut_blk_state(cbp, rpsm, key);
            hit_chk = 1;
        } else if (*en == entries && *ek == key && *ek2 == key2) {
            g_clut_hits++;
            if (clut_holds(off, entries, *es)) {
                g_clut_same++;          /* the buffer has it from this very load */
                goto done;
            }
            if (!clut_defer(off, entries, ee, *eh)) {
                gs_clut_sync();
                if (memcmp(&gs_clut[off], ee, (size_t)entries * 4)) {
                    clut_put(off, ee, entries);
                    gs_clut_ver++;
                }
            }
            clut_note(off, entries, *es, *eh);
            goto done;
        }
        g_clut_misses++;
    }
    if (!hit_chk && !csm && g_gs_lazy && g_gs_clut_up && clut_from_upload(cbp, rpsm, w, h, entries, vals)) {
        if (g_gs_clut_chk == 4) {
            gs_sync_rect(cbp, 1, rpsm, 0, 0, w, h);     /* debugging: the blocks released as the old path did */
            goto have_vals;
        }
        if (!g_gs_clut_chk)
            goto have_vals;
        memcpy(g_clut_up_vals, vals, (size_t)entries * 4);
        clut_chk_owner(cbp, rpsm);
    } else if (g_gs_clut_chk) {
        g_gs_clut_chk |= 2;             /* not from an upload: nothing to compare */
    }
    if (csm)
        gs_sync_rect(cbp, cbw, rpsm, 0, 0, 1024, 1);
    else
        gs_sync_rect(cbp, 1, rpsm, 0, 0, w, h);
    if (csm) {
        uint64_t tc = g_regs[GS_TEXCLUT];
        uint32_t cou = GS_FIELD(tc, 6, 6) * 16, cov = GS_FIELD(tc, 12, 10);
        for (i = 0; i < entries; i++)
            vals[i] = gs_read_pixel(rpsm, cbp, cbw, cou + (uint32_t)i, cov);
    } else if (rpsm == PSMCT32) {
        /* 16x16 or 8x2 in 8x8 blocks of 64 words: each block's base once */
        uint32_t base[4];
        base[0] = gs_addr32(cbp, 1, 0, 0);
        base[1] = gs_addr32(cbp, 1, 8, 0);
        base[2] = gs_addr32(cbp, 1, 0, 8);
        base[3] = gs_addr32(cbp, 1, 8, 8);
        for (i = 0; i < entries; i++) {
            if (entries == 256) {
                uint32_t e = ((uint32_t)i & ~0x18u) | (((uint32_t)i & 0x08u) << 1) | (((uint32_t)i & 0x10u) >> 1);
                x = e & 15;
                y = e >> 4;
            } else {
                x = (uint32_t)i & 7;
                y = (uint32_t)i >> 3;
            }
            vals[i] = gs_mem[(base[(y >> 3) * 2 + (x >> 3)] + column32[y & 7][x & 7]) & (GS_MEM_WORDS - 1)];
        }
    } else {
        for (i = 0; i < entries; i++) {
            if (entries == 256) {
                uint32_t e = ((uint32_t)i & ~0x18u) | (((uint32_t)i & 0x08u) << 1) | (((uint32_t)i & 0x10u) >> 1);
                x = e & 15;
                y = e >> 4;
            } else {
                x = (uint32_t)i & 7;
                y = (uint32_t)i >> 3;
            }
            vals[i] = gs_read_pixel(rpsm, cbp, 1, x, y);
        }
    }
    if (g_gs_clut_chk == 1 && memcmp(g_clut_up_vals, vals, (size_t)entries * 4))
        clut_chk_log(cbp, rpsm, entries, g_clut_up_vals, vals);
    if (hit_chk && memcmp(g_clut_up_vals, vals, (size_t)entries * 4))
        clut_hit_log(cbp, rpsm, entries, g_clut_up_vals, vals);
    g_gs_clut_chk &= 1;
have_vals:
    gs_clut_sync();                     /* (before a cache entry is written: one may be what is waiting) */
    if (memcmp(&gs_clut[off], vals, (size_t)entries * 4)) {
        clut_put(off, vals, entries);
        gs_clut_ver++;
    }
    if (ek) {
        *ek = key;
        *ek2 = key2;
        *en = (uint16_t)entries;
        *eo = (uint16_t)off;
        memcpy(ee, vals, (size_t)entries * 4);
        if (!++g_clut_ser)
            g_clut_ser = 1;
        *es = g_clut_ser;
        *eh = clut_fold(vals, entries);
        clut_note(off, entries, *es, *eh);
    } else {
        clut_note(off, entries, 0, 0);
    }
done:
    if (g_gs_clut_chk)
        return;
    {
        /* (another load's entry of the two goes: the older one) */
        const uint32_t e = g_clut_last_n++ & 1;
        g_clut_last[e].tex0 = tex0 | 1ull << 61;        /* (CLD, not compared, keeps it non-zero) */
        g_clut_last[e].texclut = g_regs[GS_TEXCLUT];
        g_clut_last[e].ver = gs_ver;
        g_clut_last[e].cver = gs_clut_ver;
    }
}

uint32_t gs_texel_rgba(uint64_t tex0, uint64_t texa, int x, int y)
{
    uint32_t tbp = GS_FIELD(tex0, 0, 14), tbw = GS_FIELD(tex0, 14, 6), psm = GS_FIELD(tex0, 20, 6);
    uint32_t cpsm = GS_FIELD(tex0, 51, 4), csa = GS_FIELD(tex0, 56, 5);
    int aem = (int)GS_FIELD(texa, 15, 1);
    uint32_t idx, c;
    switch (psm) {
    case PSMCT32:
        return gs_mem[gs_addr32(tbp, tbw, (uint32_t)x, (uint32_t)y)];
    case PSMCT24: {
        uint32_t v = gs_mem[gs_addr32(tbp, tbw, (uint32_t)x, (uint32_t)y)] & 0xFFFFFF;
        uint32_t a = (aem && !v) ? 0 : GS_FIELD(texa, 0, 8);
        return v | a << 24;
    }
    case PSMCT16:
    case PSMCT16S:
        return expand16(gs_read_pixel(psm, tbp, tbw, (uint32_t)x, (uint32_t)y), texa, aem);
    case PSMZ32: case PSMZ24: case PSMZ16: case PSMZ16S:
        c = gs_read_pixel(psm, tbp, tbw, (uint32_t)x, (uint32_t)y);
        return psm == PSMZ16 || psm == PSMZ16S ? expand16(c, texa, aem) : c;
    case PSMT8: case PSMT8H:
        idx = gs_read_pixel(psm, tbp, tbw, (uint32_t)x, (uint32_t)y);
        break;
    case PSMT4: case PSMT4HL: case PSMT4HH:
        idx = gs_read_pixel(psm, tbp, tbw, (uint32_t)x, (uint32_t)y) + (cpsm == 0 ? (csa & 15) * 16 : csa * 16);
        break;
    default:
        return 0;
    }
    gs_clut_sync();
    if (cpsm == 0)
        return gs_clut[idx & 255];
    return expand16(gs_clut[idx & 511], texa, aem);
}

/* ---- transfers ------------------------------------------------------------------------- */

static struct {
    int active;
    uint32_t dbp, dbw, dpsm, x0, y0, w, h, x, y;
    uint32_t pending;        /* leftover bits for 24-bit formats */
    uint32_t pending_bytes;
} g_xfer;

static void xfer_put(uint32_t v)
{
    gs_write_pixel(g_xfer.dpsm, g_xfer.dbp, g_xfer.dbw, g_xfer.x0 + g_xfer.x, g_xfer.y0 + g_xfer.y, v);
    if (++g_xfer.x >= g_xfer.w) {
        g_xfer.x = 0;
        if (++g_xfer.y >= g_xfer.h)
            g_xfer.active = 0;
    }
}

/* Fast uploads: pixels of one row within one block share the block's base
 * address, so the swizzle is a column-table lookup per pixel. Local memory is
 * little-endian words; on a big-endian host bytes and halfwords sit at XORed
 * addresses within a word. */
#define GS_BX (PS2_BXOR)
#define GS_HX (PS2_HXOR >> 1)

/* write n pixels of row y from x on (n stays within the row) */
static void put_row(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, const uint32_t *v, uint32_t n)
{
    uint8_t *mem8 = (uint8_t *)gs_mem;
    uint16_t *mem16 = (uint16_t *)gs_mem;
    uint32_t k, span, b;
    while (n) {
        switch (psm) {
        case PSMCT32: {
            const uint8_t *col = column32[y & 7];
            span = 8 - (x & 7);
            b = bp * 64 + ((y >> 5) * bw + (x >> 6)) * 2048 + block32[(y >> 3) & 3][(x >> 3) & 7] * 64;
            if (span > n) span = n;
            for (k = 0; k < span; k++)
                gs_mem[(b + col[(x + k) & 7]) & (GS_MEM_WORDS - 1)] = v[k];
            break;
        }
        case PSMCT16: case PSMCT16S: {
            const uint8_t *col = column16[y & 7];
            const uint8_t (*bt)[4] = psm == PSMCT16 ? block16 : block16s;
            span = 16 - (x & 15);
            b = bp * 128 + ((y >> 6) * bw + (x >> 6)) * 4096 + bt[(y >> 3) & 7][(x >> 4) & 3] * 128;
            if (span > n) span = n;
            for (k = 0; k < span; k++)
                mem16[((b + col[(x + k) & 15]) & (GS_MEM_WORDS * 2 - 1)) ^ GS_HX] = (uint16_t)v[k];
            break;
        }
        case PSMT8: {
            const uint8_t *col = column8[y & 15];
            span = 16 - (x & 15);
            b = bp * 256 + ((y >> 6) * ((bw + 1) >> 1) + (x >> 7)) * 8192 + block32[(y >> 4) & 3][(x >> 4) & 7] * 256;
            if (span > n) span = n;
            for (k = 0; k < span; k++)
                mem8[((b + col[(x + k) & 15]) & (GS_MEM_WORDS * 4 - 1)) ^ GS_BX] = (uint8_t)v[k];
            break;
        }
        case PSMT4: {
            const uint16_t *col = column4[y & 15];
            span = 32 - (x & 31);
            b = bp * 512 + ((y >> 7) * ((bw + 1) >> 1) + (x >> 7)) * 16384 + block16[(y >> 4) & 7][(x >> 5) & 3] * 512;
            if (span > n) span = n;
            for (k = 0; k < span; k++) {
                uint32_t ni = (b + col[(x + k) & 31]) & (GS_MEM_WORDS * 8 - 1);
                uint8_t *q = &mem8[(ni >> 1) ^ GS_BX];
                *q = ni & 1 ? (uint8_t)((*q & 0x0F) | (v[k] << 4)) : (uint8_t)((*q & 0xF0) | (v[k] & 0xF));
            }
            break;
        }
        default:
            for (k = 0; k < n; k++)
                gs_write_pixel(psm, bp, bw, x + k, y, v[k]);
            return;
        }
        v += span;
        n -= span;
        x += span;
    }
}

#ifdef WTIME
static void xfer_run_inner(const uint32_t *v, uint32_t n);
static void xfer_run(const uint32_t *v, uint32_t n)
{
    WT_MARK_IN(wm);
    xfer_run_inner(v, n);
    WT_DONE_IN(xfer, wm);
}
static void xfer_run_inner(const uint32_t *v, uint32_t n)
#else
static void xfer_run(const uint32_t *v, uint32_t n)
#endif
{
    while (n && g_xfer.active) {
        uint32_t left = g_xfer.w - g_xfer.x, span = n < left ? n : left;
        put_row(g_xfer.dpsm, g_xfer.dbp, g_xfer.dbw, g_xfer.x0 + g_xfer.x, g_xfer.y0 + g_xfer.y, v, span);
        v += span;
        n -= span;
        g_xfer.x += span;
        if (g_xfer.x >= g_xfer.w) {
            g_xfer.x = 0;
            if (++g_xfer.y >= g_xfer.h)
                g_xfer.active = 0;
        }
    }
}

/* ---- lazy uploads ------------------------------------------------------------------------ */

static uint32_t psm_bits(uint32_t psm);

/* With g_gs_lazy (the GX renderer) a block-aligned upload is kept as the
 * linear data it arrived as, plus a hash, and GS memory is written only when
 * something reads it (gs_sync_rect). The renderer takes textures straight
 * from the upload (gs_upload_for). Every 256-byte block records the pending
 * upload that owns it; an upload writes back only the blocks it still owns,
 * so overlapping uploads need no ordering.
 *
 * An upload whose data arrives in one piece from guest RAM (DMA from a
 * texture's raster) is not copied: it points at that memory (ext), and its
 * blocks are identified by where their rows come from plus a few of their
 * words, not by hashing all of it. Such uploads still live after two frames
 * are written back to GS memory then, before the game reuses the memory. */
int g_gs_lazy;

typedef struct GsUpload {
    uint32_t dbp, dbw, dpsm, x0, y0, w, h, bits;
    uint32_t hash, ident, words, live;  /* ident: hash of data and shape, not of where it went */
    uint32_t *data;                     /* NULL until the data starts arriving */
    uint32_t frame;                     /* g_vif1_chains (frames) when it was made */
    uint32_t ver;                       /* gs_ver of the transfer: its blocks get it when the data is in */
    uint8_t ext;                        /* data is guest RAM, not the stage */
} GsUpload;

#define UR_MAX 2048
/* The stage holds the data of uploads that did not come from guest RAM until
 * something reads GS memory there; full, the oldest are written out early.
 * A race stages none (they are read in place), and it had 0.5 MB of MEM2
 * left with 1 MB here: the profiler's buffers on top made the blur's
 * buffers fail at the start of one. */
#ifdef GEKKO
#define STAGE_MAX_WORDS (512u * 1024 / 4)
#else
#define STAGE_MAX_WORDS (1024u * 1024 / 4)
#endif
static GsUpload g_ur[UR_MAX];
static uint32_t g_ur_head;              /* next slot (ring) */
static uint16_t g_ext_list[UR_MAX];     /* the slots of the ext uploads that may still own blocks (ext_age) */
static uint8_t g_ext_in[UR_MAX];
static uint32_t g_ext_n;
static uint16_t g_blk_owner[GS_MEM_WORDS / 64];
static uint16_t g_blk_idx[GS_MEM_WORDS / 64];      /* which block of its owner's rectangle */
/* content hash of each 256-byte block, valid while g_blk_hver == gs_blk_ver + 1 */
uint32_t gs_blk_hash[GS_MEM_WORDS / 64], gs_blk_hver[GS_MEM_WORDS / 64];
#define g_blk_hash gs_blk_hash
#define g_blk_hver gs_blk_hver
static uint32_t *g_stage;
static uint32_t g_stage_used, STAGE_WORDS = STAGE_MAX_WORDS;    /* as much as memory allows */
/* uploads with data in the stage, oldest first: (slot, data) */
#define SQ_MAX UR_MAX
static struct { uint16_t idx; uint32_t *data; } g_sq[SQ_MAX];
static uint32_t g_sq_head, g_sq_tail;
extern uint32_t g_vif1_chains;          /* frames: VIF1 chains kicked (hw.c) */
static void lazy_finish(void);
uint32_t g_gs_ext_uploads, g_gs_ext_aged;   /* status line */
uint32_t g_gs_stage_why[4];             /* status line: staged uploads: split data, not guest RAM; path 2, path 3 */
static int g_lz_path;

static void psm_blockdim(uint32_t psm, uint32_t *bw, uint32_t *bh)
{
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *bw = 16; *bh = 8; break;
    case PSMT8: *bw = 16; *bh = 16; break;
    case PSMT4: *bw = 32; *bh = 16; break;
    default: *bw = 8; *bh = 8; break;
    }
}

static inline uint32_t ur_pixel(const GsUpload *u, uint32_t i)
{
    switch (u->bits) {
    case 32: return u->data[i];
    case 16: return (u->data[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
    case 8: return (u->data[i >> 2] >> ((i & 3) * 8)) & 0xFF;
    default: return (u->data[i >> 3] >> ((i & 7) * 4)) & 0xF;
    }
}

/* write back the blocks an upload still owns */
static void ur_materialize(uint32_t idx)
{
    GsUpload *u = &g_ur[idx];
    uint32_t bw, bh, bx, by, x, y, row[32];
    int prev;
    if (!u->live)
        return;
    prev = pt_enter(PT_SYNC);
    psm_blockdim(u->dpsm, &bw, &bh);
    for (by = u->y0; by < u->y0 + u->h; by += bh)
        for (bx = u->x0; bx < u->x0 + u->w; bx += bw) {
            uint32_t b = block_of(u->dpsm, u->dbp, u->dbw, bx, by);
            if (g_blk_owner[b] != idx + 1)
                continue;
            g_blk_owner[b] = 0;
            for (y = by; y < by + bh; y++) {
                uint32_t base = (y - u->y0) * u->w + (bx - u->x0);
                for (x = 0; x < bw; x++)
                    row[x] = ur_pixel(u, base + x);
                put_row(u->dpsm, u->dbp, u->dbw, bx, y, row, bw);
            }
            u->live--;
        }
    u->live = 0;
    pt_leave(prev);
}

/* The stage is a ring: before stage words [lo, hi) (and, when the ring
 * wraps, [lo2, hi2)) are reused, write back what the oldest uploads there
 * still own. Usually nothing: a game re-uploads its textures every frame,
 * so old uploads are superseded block by block long before. */
static void stage_reclaim(uint32_t lo, uint32_t hi, uint32_t lo2, uint32_t hi2)
{
    while (g_sq_tail != g_sq_head) {
        GsUpload *u = &g_ur[g_sq[g_sq_tail].idx];
        uint32_t s, e;
        if (u->data != g_sq[g_sq_tail].data || u->ext) {   /* the slot moved on since */
            g_sq_tail = (g_sq_tail + 1) % SQ_MAX;
            continue;
        }
        s = (uint32_t)(u->data - g_stage);
        e = s + u->words;
        if (!(s < hi && e > lo) && !(s < hi2 && e > lo2))
            break;
        if (u->live)
            ur_materialize(g_sq[g_sq_tail].idx);
        g_sq_tail = (g_sq_tail + 1) % SQ_MAX;
    }
}

/* stage space for an upload's data */
static uint32_t *stage_alloc(uint32_t idx, uint32_t words)
{
    uint32_t *d;
    if (g_stage_used + words > STAGE_WORDS) {
        stage_reclaim(g_stage_used, STAGE_WORDS, 0, words);      /* wrap around */
        g_stage_used = 0;
    } else {
        stage_reclaim(g_stage_used, g_stage_used + words, 0, 0);
    }
    if ((g_sq_head + 1) % SQ_MAX == g_sq_tail) {
        GsUpload *o = &g_ur[g_sq[g_sq_tail].idx];
        if (o->data == g_sq[g_sq_tail].data && !o->ext && o->live)
            ur_materialize(g_sq[g_sq_tail].idx);
        g_sq_tail = (g_sq_tail + 1) % SQ_MAX;
    }
    d = g_stage + g_stage_used;
    g_stage_used += words;
    g_sq[g_sq_head].idx = (uint16_t)idx;
    g_sq[g_sq_head].data = d;
    g_sq_head = (g_sq_head + 1) % SQ_MAX;
    return d;
}

/* Uploads that point at guest RAM and still own blocks two frames later are
 * written back now: the game may reuse that memory. */
static void ext_age(void)
{
    /* walks the slots of the ext uploads made, not all 2048 of them (128 KB of upload records read a frame): a slot
     * that is not ext and live any more is left out (live only falls, until the slot is made again) */
    uint32_t i, keep = 0;
    for (i = 0; i < g_ext_n; i++) {
        uint32_t k = g_ext_list[i];
        GsUpload *u = &g_ur[k];
        if (u->ext && u->live) {
            if (g_vif1_chains - u->frame < 2) {
                g_ext_list[keep++] = (uint16_t)k;
                continue;
            }
            ur_materialize(k);
            g_gs_ext_aged++;
        }
        g_ext_in[k] = 0;
    }
    g_ext_n = keep;
}

static void ext_note(uint32_t k)
{
    if (!g_ext_in[k]) {
        g_ext_in[k] = 1;
        g_ext_list[g_ext_n++] = (uint16_t)k;
    }
}

/* write back one block an upload still owns */
static void ur_materialize_block(uint32_t b)
{
    uint32_t o = g_blk_owner[b], bw, bh, bx, by, x, y, row[32], k;
    GsUpload *u;
    int prev = pt_enter(PT_SYNC);
    u = &g_ur[o - 1];
    psm_blockdim(u->dpsm, &bw, &bh);
    /* the block's place in the rectangle, from its index (row-major blocks) */
    k = g_blk_idx[b];
    bx = u->x0 + (k % (u->w / bw)) * bw;
    by = u->y0 + (k / (u->w / bw)) * bh;
    g_blk_owner[b] = 0;
    for (y = by; y < by + bh; y++) {
        uint32_t base = (y - u->y0) * u->w + (bx - u->x0);
        for (x = 0; x < bw; x++)
            row[x] = ur_pixel(u, base + x);
        put_row(u->dpsm, u->dbp, u->dbw, bx, y, row, bw);
    }
    u->live--;
    pt_leave(prev);
}

static void placed_sync(uint32_t bp);

/* make GS memory current for a rectangle: only its blocks are written back */
void gs_sync_rect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t dw, dh, bx, by;
    if (!x && !y)
        placed_sync(bp);                /* a texture the game placed there without sending it */
    if (!g_gs_lazy || !w || !h)
        return;
    psm_blockdim(psm, &dw, &dh);
    {
        GsBlkWalk wk;
        gs_bw_init(&wk, psm, bp, bw);
        for (by = y & ~(dh - 1); by < y + h; by += dh) {
            GsBlkRow row = gs_bw_row(&wk, by);
            for (bx = x & ~(dw - 1); bx < x + w; bx += dw) {
                uint32_t b = gs_bw_blk(&wk, row, bx);
                if (g_blk_owner[b])
                    ur_materialize_block(b);
            }
        }
    }
}

/* the pending upload that holds exactly this texture, if any */
const GsUpload *gs_upload_for(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t *hash)
{
    uint32_t o, dw, dh;
    const GsUpload *u;
    if (!g_gs_lazy)
        return NULL;
    o = g_blk_owner[block_of(psm, tbp, tbw, 0, 0)];
    if (!o)
        return NULL;
    u = &g_ur[o - 1];
    if (u->dbp != tbp || u->dbw != tbw || u->dpsm != psm || u->x0 || u->y0 || u->w != tw || u->h != th)
        return NULL;
    psm_blockdim(psm, &dw, &dh);
    if (u->live != (tw / dw) * (th / dh))
        return NULL;                    /* partly overwritten since */
    *hash = u->hash;
    return u;
}

uint32_t gs_upload_pixel(const GsUpload *u, uint32_t x, uint32_t y) { return ur_pixel(u, y * u->w + x); }

void gs_upload_row8(const GsUpload *u, uint32_t y, uint32_t n, uint8_t *out)
{
    const uint32_t *d = u->data;
    uint32_t i = y * u->w, k;
    switch (u->bits) {
    case 8:
        for (k = 0; k < n; k++, i++)
            out[k] = (uint8_t)(d[i >> 2] >> ((i & 3) * 8));
        break;
    case 4:
        for (k = 0; k < n; k++, i++)
            out[k] = (d[i >> 3] >> ((i & 7) * 4)) & 0xF;
        break;
    default:
        for (k = 0; k < n; k++)
            out[k] = (uint8_t)ur_pixel(u, i + k);
        break;
    }
}

void gs_read_row8(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t y, uint32_t n, uint8_t *out)
{
    uint32_t x = 0, k, m;
    if (psm == PSMT8) {
        const uint32_t pages = (y >> 6) * ((bw + 1) >> 1);
        while (x < n) {
            /* the rest of a 16-pixel block */
            uint32_t base = bp * 256 + (pages + (x >> 7)) * 8192 + block32[(y >> 4) & 3][(x >> 4) & 7] * 256u;
            m = 16 - (x & 15);
            if (m > n - x)
                m = n - x;
            for (k = 0; k < m; k++)
                out[x + k] = (uint8_t)mem_r8((base + column8[y & 15][(x + k) & 15]) & (GS_MEM_WORDS * 4 - 1));
            x += m;
        }
    } else if (psm == PSMT4) {
        const uint32_t pages = (y >> 7) * ((bw + 1) >> 1);
        while (x < n) {
            uint32_t base = bp * 512 + (pages + (x >> 7)) * 16384 + block16[(y >> 4) & 7][(x >> 5) & 3] * 512u;
            m = 32 - (x & 31);
            if (m > n - x)
                m = n - x;
            for (k = 0; k < m; k++)
                out[x + k] = (uint8_t)mem_r4((base + column4[y & 15][(x + k) & 31]) & (GS_MEM_WORDS * 8 - 1));
            x += m;
        }
    } else {
        for (; x < n; x++)
            out[x] = (uint8_t)gs_read_pixel(psm, bp, bw, x, y);
    }
}
uint32_t gs_upload_ident(const GsUpload *u) { return u->ident; }

/* What a 256-byte block holds, as a hash: the identity of the pending upload
 * that owns it (its data and geometry decide the block), or a hash of the
 * block in memory. A texture's identity is these, block by block. */
uint32_t gs_block_ident(uint32_t b)
{
    uint32_t o, h = 0x811C9DC5u, k;
    const uint32_t *w;
    b &= GS_MEM_WORDS / 64 - 1;
    if (g_blk_hver[b] == gs_blk_ver[b] + 1)
        return g_blk_hash[b];           /* hashed when uploaded, or earlier and unchanged since */
    o = g_blk_owner[b];
    if (o) {
        h = (g_ur[o - 1].ident ^ g_blk_idx[b]) * 0x9E3779B1u;
        return (h ^ (h >> 15)) ^ 0xA5A5A5A5u;
    }
    w = gs_mem + b * 64;
    for (k = 0; k < 64; k += 4) {
        h ^= w[k] + w[k + 1] * 3 + w[k + 2] * 5 + w[k + 3] * 7;
        h *= 0x9E3779B1u;
        h ^= h >> 15;
    }
    g_blk_hash[b] = h;
    g_blk_hver[b] = gs_blk_ver[b] + 1;
    return h;
}

/* The pending upload whose data covers every block of a texture, in any
 * format (games send 4-bit textures pre-swizzled as 16- or 32-bit images).
 * The texture is then a function of that upload's data and geometry: *hash
 * identifies it without reading GS memory. */
const GsUpload *gs_upload_covering(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t *hash)
{
    uint32_t o, dw, dh, bx, by, h;
    const GsUpload *u;
    if (!g_gs_lazy)
        return NULL;
    o = g_blk_owner[block_of(psm, tbp, tbw, 0, 0)];
    if (!o)
        return NULL;
    psm_blockdim(psm, &dw, &dh);
    for (by = 0; by < th; by += dh)
        for (bx = 0; bx < tw; bx += dw)
            if (g_blk_owner[block_of(psm, tbp, tbw, bx, by)] != o)
                return NULL;
    u = &g_ur[o - 1];
    /* the upload's identity and where in it the texture starts */
    h = (u->ident ^ g_blk_idx[block_of(psm, tbp, tbw, 0, 0)]) * 0x9E3779B1u;
    *hash = h ^ (h >> 15);
    return u;
}

/* A CLUT miss: the entries straight from the pending upload that holds
 * them (in the CLUT's format, anywhere in it: each block's index in the
 * upload gives where), without writing its blocks to GS memory first. 0 when
 * there is none. */
static int clut_from_upload(uint32_t cbp, uint32_t rpsm, uint32_t w, uint32_t h, int entries, uint32_t *vals)
{
    uint32_t uh, x, y, dw, dh, bpr;
    int i;
    const GsUpload *u = gs_upload_covering(cbp, 1, rpsm, w, h, &uh);
    if (!u || u->dpsm != rpsm || !u->data)
        return 0;
    psm_blockdim(rpsm, &dw, &dh);
    bpr = u->w / dw;
    if (!bpr)
        return 0;
    if (rpsm == PSMCT32) {
        /* 32-bit: each 8x8 block's first row in the upload's words, once */
        const uint32_t *row[4];
        uint32_t j;
        for (j = 0; j < 4; j++) {
            uint32_t k = g_blk_idx[block_of(PSMCT32, cbp, 1, (j & 1) * 8, (j >> 1) * 8)];
            row[j] = u->data + (k / bpr) * 8 * u->w + (k % bpr) * 8;
        }
        for (i = 0; i < entries; i++) {
            if (entries == 256) {
                uint32_t e = ((uint32_t)i & ~0x18u) | (((uint32_t)i & 0x08u) << 1) | (((uint32_t)i & 0x10u) >> 1);
                x = e & 15;
                y = e >> 4;
            } else {
                x = (uint32_t)i & 7;
                y = (uint32_t)i >> 3;
            }
            vals[i] = row[(y >> 3) * 2 + (x >> 3)][(y & 7) * u->w + (x & 7)];
        }
        return 1;
    }
    for (i = 0; i < entries; i++) {
        uint32_t k;
        if (entries == 256) {
            uint32_t e = ((uint32_t)i & ~0x18u) | (((uint32_t)i & 0x08u) << 1) | (((uint32_t)i & 0x10u) >> 1);
            x = e & 15;
            y = e >> 4;
        } else {
            x = (uint32_t)i & 7;
            y = (uint32_t)i >> 3;
        }
        k = g_blk_idx[block_of(rpsm, cbp, 1, x, y)];
        vals[i] = gs_upload_pixel(u, (k % bpr) * dw + x % dw, (k / bpr) * dh + y % dh);
    }
    return 1;
}

/* debugging (clutchk 8): a CLUT cache hit that GS memory disagrees with,
 * and the state of the CLUT's blocks at the lookup */
static uint32_t g_hit_key, g_hit_st[4][6];

static void clut_blk_state(uint32_t cbp, uint32_t rpsm, uint32_t key)
{
    uint32_t j, dw = rpsm == PSMCT32 ? 8 : 16;
    g_hit_key = key;
    for (j = 0; j < 4; j++) {
        uint32_t b = block_of(rpsm, cbp, 1, (j & 1) * dw, (j >> 1) * 8), o = g_blk_owner[b];
        g_hit_st[j][0] = b;
        g_hit_st[j][1] = o;
        g_hit_st[j][2] = g_blk_hver[b];
        g_hit_st[j][3] = gs_blk_ver[b];
        g_hit_st[j][4] = g_blk_hash[b];
        g_hit_st[j][5] = o ? g_ur[o - 1].ext | g_ur[o - 1].words << 1 : 0;
    }
}

static void clut_hit_log(uint32_t cbp, uint32_t rpsm, int entries, const uint32_t *hit, const uint32_t *mem)
{
    static int logged;
    int i, first = -1, nd = 0, j;
    if (logged >= 30)
        return;
    logged++;
    for (i = 0; i < entries; i++)
        if (hit[i] != mem[i]) {
            if (first < 0)
                first = i;
            nd++;
        }
    LOG("CLUT cache hit %X/%02X %d entries key %08X: %d differ from GS memory (first %d: %08X, memory %08X), gs_ver %u",
        cbp, rpsm, entries, g_hit_key, nd, first, hit[first], mem[first], gs_ver);
    for (j = 0; j < (entries == 256 ? 4 : 1); j++)
        LOG("  block %X: owner %u hver %u ver %u hash %08X upload ext|words<<1 %u", g_hit_st[j][0], g_hit_st[j][1],
            g_hit_st[j][2], g_hit_st[j][3], g_hit_st[j][4], g_hit_st[j][5]);
}

/* debugging (clutchk): the upload a CLUT was read from, before the sync */
static GsUpload g_clut_chk_u;
static uint32_t g_clut_chk_o, g_clut_chk_idx[4];

static void clut_chk_owner(uint32_t cbp, uint32_t rpsm)
{
    uint32_t j;
    g_clut_chk_o = g_blk_owner[block_of(rpsm, cbp, 1, 0, 0)];
    if (g_clut_chk_o)
        g_clut_chk_u = g_ur[g_clut_chk_o - 1];
    for (j = 0; j < 4; j++)
        g_clut_chk_idx[j] = g_blk_idx[block_of(rpsm, cbp, 1, (j & 1) * 8, (j >> 1) * 8)];
}

static void clut_chk_log(uint32_t cbp, uint32_t rpsm, int entries, const uint32_t *up, const uint32_t *mem)
{
    static int logged;
    const GsUpload *u = &g_clut_chk_u;
    int i, first = -1, nd = 0;
    if (logged >= 40)
        return;
    logged++;
    for (i = 0; i < entries; i++)
        if (up[i] != mem[i]) {
            if (first < 0)
                first = i;
            nd++;
        }
    LOG("CLUT %X/%02X %d entries: %d differ from the upload (first %d: %08X, GS memory %08X); owner %u: upload "
        "%X/%u/%02X at %u,%u %ux%u live %u ext %u data %p frame %u (now %u), blocks %u %u %u %u", cbp, rpsm, entries, nd,
        first, up[first], mem[first], g_clut_chk_o, u->dbp, u->dbw, u->dpsm, u->x0, u->y0, u->w, u->h, u->live, u->ext,
        (void *)u->data, u->frame, g_vif1_chains, g_clut_chk_idx[0], g_clut_chk_idx[1], g_clut_chk_idx[2],
        g_clut_chk_idx[3]);
}

/* debugging: why a texture is not taken from an upload */
void gs_upload_explain(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th)
{
    static int n;
    uint32_t o = g_blk_owner[block_of(psm, tbp, tbw, 0, 0)];
    const GsUpload *u = o ? &g_ur[o - 1] : NULL;
    if (n >= 40)
        return;
    n++;
    if (!u)
        LOG("tex %X/%u/%02X %ux%u: no pending upload at its first block", tbp, tbw, psm, tw, th);
    else
        LOG("tex %X/%u/%02X %ux%u: upload %X/%u/%02X at %u,%u %ux%u live %u", tbp, tbw, psm, tw, th, u->dbp, u->dbw,
            u->dpsm, u->x0, u->y0, u->w, u->h, u->live);
}

/* the transfer being received into the stage */
static struct {
    int on;
    uint32_t need, got, idx;
} g_lz;

static int lazy_begin(void)
{
    uint32_t bits = psm_bits(g_xfer.dpsm), dw, dh, words;
    if (!g_gs_lazy || (bits != 32 && bits != 16 && bits != 8 && bits != 4) || g_xfer.dpsm == PSMCT24 ||
        g_xfer.dpsm == PSMT8H || g_xfer.dpsm == PSMT4HL || g_xfer.dpsm == PSMT4HH || g_xfer.dpsm >= PSMZ32)
        return 0;
    psm_blockdim(g_xfer.dpsm, &dw, &dh);
    if (g_xfer.x0 % dw || g_xfer.y0 % dh || g_xfer.w % dw || g_xfer.h % dh)
        return 0;
    words = (g_xfer.w * g_xfer.h * bits + 31) / 32;
    if (!g_stage) {
        while (!(g_stage = plat_alloc(STAGE_WORDS * 4)) && STAGE_WORDS > 512 * 1024 / 4)
            STAGE_WORDS /= 2;
        LOG("GS upload stage: %u KB%s", STAGE_WORDS / 256, g_stage ? "" : " (none, uploads go to GS memory)");
        if (!g_stage) {
            g_gs_lazy = 0;
            return 0;
        }
    }
    if (words > STAGE_WORDS / 2)
        return 0;
    /* the slot's last upload must not own blocks any more */
    if (g_ur[g_ur_head].live)
        ur_materialize(g_ur_head);
    g_lz.on = 1;
    g_lz.need = words;
    g_lz.got = 0;
    g_lz.idx = g_ur_head;
    {
        GsUpload *u = &g_ur[g_lz.idx];
        u->dbp = g_xfer.dbp; u->dbw = g_xfer.dbw; u->dpsm = g_xfer.dpsm;
        u->x0 = g_xfer.x0; u->y0 = g_xfer.y0; u->w = g_xfer.w; u->h = g_xfer.h;
        u->bits = bits;
        u->words = words;
        u->live = 0;
        u->data = NULL;
        u->ext = 0;
        u->frame = g_vif1_chains;
        u->ver = gs_ver;
    }
    g_ur_head = (g_ur_head + 1) % UR_MAX;
    return 1;
}

/* data for the upload being received: n words at src (a host pointer) */
static void lazy_data(const uint32_t *src, uint32_t n)
{
    GsUpload *u = &g_ur[g_lz.idx];
    uint32_t words = g_lz.need - g_lz.got;
    if (n > words)
        n = words;
    if (!u->data) {
        const uint8_t *p = (const uint8_t *)src;
        if (n == g_lz.need && n > 256 && g_gs_ext_on && p >= ps2_ram + 0x4000 &&
            p + n * 4 <= ps2_ram + PS2_RAM_SIZE) {
            /* all of it, from guest RAM: keep pointing there. Not palettes
             * (1 KB and less): games build those in one buffer, rewritten
             * for the next car within the frame, and a CLUT is loaded from
             * its upload again and again (clut_from_upload); copied, as the
             * DMA did */
            u->data = (uint32_t *)src;
            u->ext = 1;
            ext_note(g_lz.idx);
            g_gs_ext_uploads++;
            g_lz.got = n;
            lazy_finish();
            return;
        }
        g_gs_stage_why[n < g_lz.need ? 0 : 1]++;
        g_gs_stage_why[g_lz_path == 2 ? 2 : 3]++;
        u->data = stage_alloc(g_lz.idx, g_lz.need);
    }
    memcpy(u->data + g_lz.got, src, n * 4);
    g_lz.got += n;
    if (g_lz.got == g_lz.need)
        lazy_finish();
}

/* content hash of the block of upload u at (bx, by): all of its pixels
 * (staged uploads are small: palettes, mostly, and car palettes differ in a
 * few entries: a CLUT cache hit on another car's made parts flash) */
static uint32_t upload_block_hash(const GsUpload *u, uint32_t bx, uint32_t by, uint32_t dw, uint32_t dh)
{
    uint32_t wpr = dw * u->bits / 32, stride = u->w * u->bits / 32, r, k, step = 1;
    const uint32_t *p = u->data + (by - u->y0) * stride + (bx - u->x0) * u->bits / 32;
    uint32_t a = 0x811C9DC5u ^ u->dpsm << 24, b = 0x9E3779B9u;
    /* rows are 4, 6 or 8 words: two lanes of add/xor-rotate, mixed per row */
    for (r = 0; r < dh; r += step, p += step * stride) {
        for (k = 0; k + 1 < wpr; k += 2) {
            a = ((a ^ p[k]) << 5 | (a ^ p[k]) >> 27) + p[k + 1];
            b = ((b + p[k + 1]) << 11 | (b + p[k + 1]) >> 21) ^ p[k];
        }
        a = a * 0x9E3779B1u + b;
        b ^= a >> 16;
    }
    a ^= b * 0x85EBCA6Bu;
    return a ^ (a >> 15);
}

/* debugging (autopilot "upchk 1"): an in-place upload's identity is
 * sampled (lazy_finish); check that one identity always comes with the same
 * data, by a hash of all of it, and log when not (a texture changed in
 * guest RAM where the samples did not) */
int g_gs_upchk;

static void upload_ident_check(const GsUpload *u, uint32_t uh)
{
    static struct { uint32_t uh, full; } *seen;     /* 4096, allocated when first used */
    static int logged;
    uint32_t k, full = 0x811C9DC5u, slot = (uh * 0x9E3779B1u) >> 20;
    if (!seen && !(seen = calloc(4096, sizeof *seen)))
        return;
    for (k = 0; k < u->words; k++)
        full = (full ^ u->data[k]) * 0x01000193u;
    if (seen[slot].uh == uh && seen[slot].full != full && logged < 40) {
        logged++;
        LOG("upchk: upload %X/%u/%02X %ux%u from %08X: same identity %08X, other data", u->dbp, u->dbw, u->dpsm, u->w,
            u->h, (unsigned)((const uint8_t *)u->data - ps2_ram), uh);
    }
    seen[slot].uh = uh;
    seen[slot].full = full;
}

/* In-place uploads (data in guest RAM) by what they hold. Reading all of
 * every upload every frame is too much (~3 MB in a race), and a few sampled
 * words do not tell textures apart for good: the game streams the track's
 * textures into memory it used before, builds some anew each frame in its
 * two frame buffers in turn, and a texture that shares the samples of the one
 * that was there kept the old picture (parts of the car flickering between
 * two looks, wrong textures the further a race went). So each source
 * (address, size, shape) keeps a hash of all of its data, read again when
 * the source turns up after a frame without it, when its samples change, and
 * every few frames in turn anyway; the identity is that hash and the shape,
 * not the address. */
typedef struct ExtSrc {
    const uint32_t *d;
    uint32_t shape;                     /* words, row length, format, height */
    uint32_t samp, full;                /* hash of the samples; of all of it when last read */
    uint32_t seen, read;                /* frames (g_vif1_chains) */
} ExtSrc;
#define EXT_SRC_N 2048
/* A source back after up to this many frames with its samples the same is
 * taken as it was: the reflection views are drawn one a frame in turn, and
 * each one's textures came back "after a frame without them" every third
 * frame, which was most of the reading (8 sources a frame of 13). The
 * samples are 16 words now, not 5, and the reading in turn stays. */
#define EXT_BACK_FRAMES 4
/* An unchanged source is read again every g_ext_reread frames (30 until 2026-10-22; 12 until the samples were 16: the reading in
 * turn found nothing changed in 430 s of menus and race) ... */
#define EXT_REREAD_WORDS (48u * 1024)   /* ... as far as this many words a frame go */
static ExtSrc g_ext_src[EXT_SRC_N];
/* An unchanged source is read again after this many frames (args.txt "--reread N", autopilot "reread"): 30 until 2026-10-22, 120
 * since. The reading is an insurance against a source that changes with its sampled words the same; it has found nothing in
 * 430 s of menus and races, and it was 90% of the 87 KB of sources read a frame (2,700 cache misses, 0.5 ms on the console). */
int g_ext_reread = 120;
uint32_t g_gs_ext_reads[3];             /* status line: sources read (new or back, samples changed, in turn) */
uint32_t g_gs_ext_turn_changed;         /* ... and of those read in turn, found changed (ever) */
uint32_t g_gs_ext_read_words;

static uint32_t hash_words(const uint32_t *p, uint32_t n)
{
    uint32_t a = 0x811C9DC5u, b = 0x9E3779B9u, k;
    for (k = 0; k + 3 < n; k += 4) {
#ifdef GEKKO
        if (k + 16 < n)
            __builtin_prefetch(p + k + 16, 0, 0);   /* two lines on: the full re-reads' misses overlap */
#endif
        a = (a ^ p[k]) * 0x01000193u + p[k + 1];
        b = (b + p[k + 2]) * 0x85EBCA6Bu ^ p[k + 3];
    }
    for (; k < n; k++)
        a = (a ^ p[k]) * 0x01000193u;
    a ^= (b << 13 | b >> 19) * 0xC2B2AE35u;
    return a ^ (a >> 15);
}

/* the identity of an in-place upload: n words at d, its shape, the hash of its samples */
static uint32_t ext_ident(const uint32_t *d, uint32_t n, uint32_t shape, uint32_t samp)
{
    static uint32_t budget_frame, budget;
    uint32_t frame = g_vif1_chains, slot = (((uint32_t)(uintptr_t)d >> 4) ^ shape * 0x9E3779B1u) & (EXT_SRC_N - 1), k, h;
    ExtSrc *e = NULL, *old = NULL;
    int why = -1;
    for (k = 0; k < 8; k++) {
        ExtSrc *c = &g_ext_src[(slot + k) & (EXT_SRC_N - 1)];
        if (c->d == d && c->shape == shape) {
            e = c;
            break;
        }
        /* else a free one, else the one unseen the longest */
        if (!old || (old->d && (!c->d || c->seen < old->seen)))
            old = c;
    }
    if (budget_frame != frame) {
        budget_frame = frame;
        budget = EXT_REREAD_WORDS;
    }
    if (!e) {
        e = old;
        e->d = d;
        e->shape = shape;
        why = 0;
    } else if (frame - e->seen > EXT_BACK_FRAMES && e->seen != frame && !(g_trust & 2)) {
        why = 0;                        /* back after frames without it */
    } else if (e->samp != samp) {
        why = 1;
    } else if (frame - e->read >= (uint32_t)g_ext_reread && budget >= n && !(g_trust & 2)) {
        budget -= n;
        why = 2;
    }
    if (why >= 0) {
        const uint32_t was = e->full;
        e->full = hash_words(d, n);
        e->read = frame;
        g_gs_ext_reads[why]++;
        g_gs_ext_read_words += n;
        /* does the reading in turn ever find a source changed that its samples did not give away? */
        if (why == 2 && e->full != was) {
            static int logged;
            g_gs_ext_turn_changed++;
            if (logged < 20) {
                logged++;
                LOG("gs: in-place upload at %p (%u words, shape %08X) changed with its samples the same (chain %u)", (const void *)d, n,
                    shape, frame);
            }
        }
    }
    e->samp = samp;
    e->seen = frame;
    h = (e->full ^ shape) * 0x9E3779B1u;
    return h ^ (h >> 15);
}

/* the identity of n words of a transfer at d (guest RAM), rw words a row */
static uint32_t ext_source_ident(const uint32_t *d, uint32_t n, uint32_t rw, uint32_t dpsm, uint32_t h)
{
    uint32_t uh = ((uint32_t)(uintptr_t)d ^ rw << 20 ^ dpsm << 26 ^ h) * 0x9E3779B1u, k;
    const uint32_t step = n / 15;
    if (g_trust & 2) {
        /* debugging: a source seen before is taken for the same, by the samples it had */
        const uint32_t shape = n << 12 ^ rw << 20 ^ dpsm << 26 ^ h;
        const uint32_t slot = (((uint32_t)(uintptr_t)d >> 4) ^ shape * 0x9E3779B1u) & (EXT_SRC_N - 1);
        for (k = 0; k < 8; k++) {
            const ExtSrc *c = &g_ext_src[(slot + k) & (EXT_SRC_N - 1)];
            if (c->d == d && c->shape == shape)
                return ext_ident(d, n, shape, c->samp);
        }
    }
#ifdef GEKKO
    /* The 16 samples are a line each, spread over the texture: touch them all first, so their misses overlap
     * instead of following one another (the console's whole cost of this check is those 16 misses). */
    for (k = 0; k < 15; k++)
        __builtin_prefetch(d + k * step, 0, 0);
    __builtin_prefetch(d + n - 1, 0, 0);
#endif
    for (k = 0; k < 15; k++)            /* 16 words, the first and the last among them */
        uh = (uh ^ d[k * step]) * 0x85EBCA6Bu + (uh >> 13);
    uh = (uh ^ d[n - 1]) * 0xC2B2AE35u;
    uh ^= uh >> 15;
    return ext_ident(d, n, n << 12 ^ rw << 20 ^ dpsm << 26 ^ h, uh);
}

/* a block now belongs to upload own (index k of its rectangle), hash bh */
#define LZ_OWN(b, bh)                                                                                                  \
    do {                                                                                                               \
        uint32_t o_ = g_blk_owner[b];                                                                                  \
        if (o_ && o_ != own)                                                                                           \
            g_ur[o_ - 1].live--;        /* fully replaced: that block is never written back */                         \
        g_blk_owner[b] = (uint16_t)own;                                                                                \
        g_blk_idx[b] = (uint16_t)k++;                                                                                  \
        g_blk_hash[b] = bh;                                                                                            \
        gs_blk_ver[b] = ver;            /* mark_rect left the blocks to us */                                          \
        g_blk_hver[b] = ver + 1;                                                                                       \
        h = (h ^ bh) * 0x9E3779B1u;                                                                                    \
    } while (0)

#ifdef WTIME
static void lazy_finish_inner(void);
static void lazy_finish(void)
{
    WT_MARK_IN(wm);
    lazy_finish_inner();
    WT_DONE_IN(lazyfin, wm);
}
static void lazy_finish_inner(void)
#else
static void lazy_finish(void)
#endif
{
    GsUpload *u = &g_ur[g_lz.idx];
    /* the upload's fields in locals: the stores below may alias it */
    const uint32_t own = g_lz.idx + 1, ver = u->ver, x0 = u->x0, y0 = u->y0, x1 = u->x0 + u->w, y1 = u->y0 + u->h;
    uint32_t dw, dh, bx, by, k = 0, h = 0x811C9DC5u;
    int prev = pt_enter(PT_XFER);
    GsBlkWalk wk;
    /* Each block's identity is a hash of its pixels: games stream textures
     * to a different place each frame, batched with whatever else is needed
     * then, so neither the address nor the upload identifies a texture. */
    psm_blockdim(u->dpsm, &dw, &dh);
    gs_bw_init(&wk, u->dpsm, u->dbp, u->dbw);
    if (u->ext) {
        /* Data in guest RAM (a texture's raster): the upload's identity
         * comes from its source (ext_ident: all of its data hashed, though
         * not every frame); a block's is that and its index in the
         * rectangle. No reads per block: on the Wii each is a cache miss,
         * and three a block were ~40000 a race frame (~640 uploads, 2.9 MB). */
        const uint32_t *d = u->data, n = u->words;
        const uint32_t cols = wk.cols, xr = wk.xr, col0 = (x0 >> wk.bws) & wk.cmask, page0 = (x0 >> wk.pws) * 32;
        uint32_t uh = ext_source_ident(d, n, u->w * u->bits / 32, u->dpsm, u->h);
        if (g_gs_upchk)
            upload_ident_check(u, uh);
        /* gs_bw_blk stepped: x advances a block, so its column in the page
         * counts up and wraps to the next page (cols blocks a page row) */
        for (by = y0; by < y1; by += dh) {
            GsBlkRow row = gs_bw_row(&wk, by);
            const uint8_t *t = row.t;
            uint32_t base = row.base + page0, col = col0;
            for (bx = x0; bx < x1; bx += dw) {
                uint32_t b = (base + (t[col] ^ xr)) & 16383u, bh = (uh ^ k) * 0x9E3779B1u;
                if (++col == cols) {
                    col = 0;
                    base += 32;
                }
                bh ^= bh >> 15;
                LZ_OWN(b, bh);
            }
        }
    } else {
        for (by = y0; by < y1; by += dh) {
            GsBlkRow row = gs_bw_row(&wk, by);
            for (bx = x0; bx < x1; bx += dw) {
                uint32_t b = gs_bw_blk(&wk, row, bx), bh = upload_block_hash(u, bx, by, dw, dh);
                LZ_OWN(b, bh);
            }
        }
    }
    u->live += k;
    u->hash = h ^ (h >> 15);
    h = (u->hash ^ (u->dbw | u->dpsm << 8)) * 0x85EBCA6Bu;
    h = (h ^ (u->w | u->h << 16)) * 0x9E3779B1u;
    u->ident = h ^ (h >> 15);
    pt_leave(prev);
    g_lz.on = 0;
    g_xfer.active = 0;
}

static void xfer_word2(uint32_t w);

/* Image data of a transfer that is not lazy (small palettes, not block
 * aligned) in bulk: pixels unpacked a run at a time and written a row at a
 * time, not a word at a time. */
static void xfer_words(const uint32_t *w, uint32_t n)
{
    uint32_t px[64], k, m;
    int prev;
    if (!g_xfer.active)
        return;
    prev = pt_enter(PT_XFER);
    switch (g_xfer.dpsm) {
    case PSMCT32:
        xfer_run(w, n);
        break;
    case PSMCT16: case PSMCT16S:
        for (; n && g_xfer.active; w += m, n -= m) {
            m = n < 32 ? n : 32;
            for (k = 0; k < m; k++) {
                px[2 * k] = w[k] & 0xFFFF;
                px[2 * k + 1] = w[k] >> 16;
            }
            xfer_run(px, 2 * m);
        }
        break;
    case PSMT8:
        for (; n && g_xfer.active; w += m, n -= m) {
            m = n < 16 ? n : 16;
            for (k = 0; k < 4 * m; k++)
                px[k] = (w[k >> 2] >> (8 * (k & 3))) & 0xFF;
            xfer_run(px, 4 * m);
        }
        break;
    case PSMT4:
        for (; n && g_xfer.active; w += m, n -= m) {
            m = n < 8 ? n : 8;
            for (k = 0; k < 8 * m; k++)
                px[k] = (w[k >> 3] >> (4 * (k & 7))) & 0xF;
            xfer_run(px, 8 * m);
        }
        break;
    default:
        for (k = 0; k < n && g_xfer.active; k++)
            xfer_word2(w[k]);
        break;
    }
    pt_leave(prev);
}

static void xfer_word(uint32_t w)
{
    int prev;
    if (!g_xfer.active)
        return;
    prev = pt_enter(PT_XFER);
    xfer_word2(w);
    pt_leave(prev);
}

static void xfer_word2(uint32_t w)
{
    int k;
    uint32_t px[8];
    switch (g_xfer.dpsm) {
    case PSMCT32:
        xfer_run(&w, 1);
        return;
    case PSMCT16: case PSMCT16S:
        px[0] = w & 0xFFFF;
        px[1] = w >> 16;
        xfer_run(px, 2);
        return;
    case PSMT8:
        for (k = 0; k < 4; k++)
            px[k] = (w >> (8 * k)) & 0xFF;
        xfer_run(px, 4);
        return;
    case PSMT4:
        for (k = 0; k < 8; k++)
            px[k] = (w >> (4 * k)) & 0xF;
        xfer_run(px, 8);
        return;
    default:
        break;
    }
    switch (g_xfer.dpsm) {
    case PSMCT32: case PSMZ32: case PSMT8H: case PSMT4HL: case PSMT4HH:
        if (g_xfer.dpsm == PSMT8H || g_xfer.dpsm == PSMT4HL || g_xfer.dpsm == PSMT4HH) {
            /* these take 8 or 4 bits per pixel from the stream */
            int bits = g_xfer.dpsm == PSMT8H ? 8 : 4;
            for (k = 0; k < 32 && g_xfer.active; k += bits)
                xfer_put((w >> k) & ((1u << bits) - 1));
        } else {
            xfer_put(w);
        }
        break;
    case PSMCT24: case PSMZ24:
        /* 3 bytes per pixel, packed */
        for (k = 0; k < 4 && g_xfer.active; k++) {
            g_xfer.pending |= ((w >> (8 * k)) & 0xFF) << (8 * g_xfer.pending_bytes);
            if (++g_xfer.pending_bytes == 3) {
                xfer_put(g_xfer.pending);
                g_xfer.pending = 0;
                g_xfer.pending_bytes = 0;
            }
        }
        break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        xfer_put(w & 0xFFFF);
        if (g_xfer.active)
            xfer_put(w >> 16);
        break;
    case PSMT8:
        for (k = 0; k < 32 && g_xfer.active; k += 8)
            xfer_put((w >> k) & 0xFF);
        break;
    case PSMT4:
        for (k = 0; k < 32 && g_xfer.active; k += 4)
            xfer_put((w >> k) & 0xF);
        break;
    default:
        break;
    }
}

uint32_t g_gs_upload_bytes, g_gs_uploads;
uint32_t gs_page_ver[512], gs_ver, gs_clut_ver;
uint32_t gs_blk_ver[GS_MEM_WORDS / 64];

static void psm_blockdim(uint32_t psm, uint32_t *bw, uint32_t *bh);

/* page size in pixels by format: 64x32 (32-bit), 64x64 (16-bit), 128x64 (8-bit), 128x128 (4-bit) */
static void psm_page(uint32_t psm, uint32_t *pw, uint32_t *ph)
{
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: *pw = 64; *ph = 64; break;
    case PSMT8: *pw = 128; *ph = 64; break;
    case PSMT4: *pw = 128; *ph = 128; break;
    default: *pw = 64; *ph = 32; break;
    }
}

#if !defined(GEKKO)
/* Debugging (B3_RTDEP=1 in the environment): report reads of GS memory that
 * rendering wrote last -- textures, CLUTs and local transfers fed by render
 * targets, which a renderer drawing outside GS memory (gx) has to handle.
 * Logged once per source and reader: "rtdep: tex 2D40 ... <- FRAME ...". */
static int g_rtdep = -1;
static uint32_t g_drawn[GS_MEM_WORDS / 64];     /* per block: FRAME (or ZBUF | 1 << 30) | 1 << 31 */
static uint32_t g_drawn_gen, g_rtdep_field;

static int rtdep_on(void)
{
    if (g_rtdep < 0) {
        const char *e = getenv("B3_RTDEP");
        g_rtdep = e && *e && *e != '0';
    }
    return g_rtdep;
}

static void rtdep_mark(uint32_t psm, uint32_t bp, uint32_t bw, int x0, int y0, int x1, int y1, uint32_t mark)
{
    uint32_t dw, dh;
    int x, y;
    psm_blockdim(psm, &dw, &dh);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 2047) x1 = 2047;
    if (y1 > 2047) y1 = 2047;
    for (y = y0 - y0 % (int)dh; y <= y1; y += (int)dh)
        for (x = x0 - x0 % (int)dw; x <= x1; x += (int)dw) {
            uint32_t b = block_of(psm, bp, bw, (uint32_t)x, (uint32_t)y) & (GS_MEM_WORDS / 64 - 1);
            if (g_drawn[b] != mark) {
                g_drawn[b] = mark;
                g_drawn_gen++;
            }
        }
}

/* which drawn source a rectangle reads: 0 if none; *nb / *nd: blocks, drawn ones */
static uint32_t rtdep_scan(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h, uint32_t *nb, uint32_t *nd)
{
    uint32_t dw, dh, x, y, found = 0;
    psm_blockdim(psm, &dw, &dh);
    *nb = *nd = 0;
    if (w > 1024) w = 1024;
    if (h > 1024) h = 1024;
    for (y = 0; y < h; y += dh)
        for (x = 0; x < w; x += dw) {
            uint32_t m = g_drawn[block_of(psm, bp, bw ? bw : 1, x, y) & (GS_MEM_WORDS / 64 - 1)];
            ++*nb;
            if (m) {
                ++*nd;
                if (!found)
                    found = m;
            }
        }
    return found;
}

static void rtdep_report(const char *what, uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h, uint32_t extra)
{
    static struct { const char *what; uint32_t bp, psm, src; } seen[512];
    static int nseen;
    uint32_t nb, nd, src = rtdep_scan(bp, bw, psm, w, h, &nb, &nd);
    int k;
    if (!src)
        return;
    for (k = 0; k < nseen; k++)
        if (seen[k].what == what && seen[k].bp == bp && seen[k].psm == psm && seen[k].src == src)
            return;
    if (nseen < 512) {
        seen[nseen].what = what;
        seen[nseen].bp = bp;
        seen[nseen].psm = psm;
        seen[nseen].src = src;
        nseen++;
    }
    LOG("rtdep: field %u %s %04X bw %u psm %02X %ux%u (%08X) <- %s %08X: %u of %u blocks drawn", g_rtdep_field, what,
        bp, bw, psm, w, h, extra, (src >> 30 & 1) ? "ZBUF" : "FRAME", src & 0x3FFFFFFFu, nd, nb);
}

static void rtdep_draw(const GsDraw *d, const GsVertex *v, int n)
{
    static uint64_t last_tex0;
    static uint32_t last_gen;
    int k, x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    uint64_t sc = d->scissor;
    uint32_t fbmsk = GS_FIELD(d->frame, 32, 32), fpsm = GS_FIELD(d->frame, 24, 6);
    if (!rtdep_on())
        return;
    if (d->prim & 0x10) {
        uint64_t t = d->tex0 & 0x1FFFFFFFFFull;     /* TBP, TBW, PSM, TW, TH */
        if (t != last_tex0 || g_drawn_gen != last_gen) {
            uint32_t psm = GS_FIELD(d->tex0, 20, 6);
            last_tex0 = t;
            last_gen = g_drawn_gen;
            rtdep_report("tex", GS_FIELD(d->tex0, 0, 14), GS_FIELD(d->tex0, 14, 6), psm,
                         1u << GS_FIELD(d->tex0, 26, 4), 1u << GS_FIELD(d->tex0, 30, 4), (uint32_t)d->frame);
        }
    }
    for (k = 0; k < n; k++) {
        int x = v[k].x >> 4, y = v[k].y >> 4;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (n > 1) {                        /* the far edges are not drawn */
        x1 = (int)((v[0].x > v[n - 1].x ? v[0].x : v[n - 1].x) - 1) >> 4;
        for (k = 0; k < n; k++)
            if (((v[k].x - 1) >> 4) > x1) x1 = (v[k].x - 1) >> 4;
        y1 = -(1 << 30);
        for (k = 0; k < n; k++)
            if (((v[k].y - 1) >> 4) > y1) y1 = (v[k].y - 1) >> 4;
    }
    if (x0 < (int)(sc & 0x7FF)) x0 = (int)(sc & 0x7FF);
    if (x1 > (int)((sc >> 16) & 0x7FF)) x1 = (int)((sc >> 16) & 0x7FF);
    if (y0 < (int)((sc >> 32) & 0x7FF)) y0 = (int)((sc >> 32) & 0x7FF);
    if (y1 > (int)((sc >> 48) & 0x7FF)) y1 = (int)((sc >> 48) & 0x7FF);
    if (x1 < x0 || y1 < y0)
        return;
    if (fbmsk != 0xFFFFFFFFu && !(fpsm == PSMCT16 && (fbmsk & 0xFFFF) == 0xFFFF))
        rtdep_mark(fpsm, GS_FIELD(d->frame, 0, 9) * 32, GS_FIELD(d->frame, 16, 6), x0, y0, x1, y1,
                   ((uint32_t)d->frame & 0x3FFFFFFFu) | 0x80000000u);
    if (GS_FIELD(d->test, 16, 1) && !GS_FIELD(d->zbuf, 32, 1))
        rtdep_mark(GS_FIELD(d->zbuf, 24, 4) | 0x30, GS_FIELD(d->zbuf, 0, 9) * 32, GS_FIELD(d->frame, 16, 6), x0, y0,
                   x1, y1, ((uint32_t)d->zbuf & 0x3FFFFFFFu) | 0xC0000000u);
}

/* Debugging (B3_HAZEDBG=1): at the first draws of a frame buffer's alpha as
 * a T8H texture (the depth haze), what the alpha holds against Z there, and
 * the CLUT it goes through; and the vertices (with UVs) of the 16-bit masked
 * draws before it. A few frames' worth. */
static void hazedbg_draw(const GsDraw *d, const GsVertex *v, int n)
{
    static int left = -1;
    const char *e;
    uint32_t psm = GS_FIELD(d->tex0, 20, 6), tbp = GS_FIELD(d->tex0, 0, 14), fbp = GS_FIELD(d->frame, 0, 9) * 32;
    int k;
    if (left < 0)
        left = (e = getenv("B3_HAZEDBG")) && *e && *e != '0' ? 3 : 0;
    if (!left)
        return;
    if (GS_FIELD(d->frame, 24, 6) == PSMCT16 && GS_FIELD(d->frame, 32, 32) == 0x3FFF) {
        static int shown;
        if (shown++ < 6)
            for (k = 0; k < n; k++)
                LOG("hazedbg: masked draw v%d xy %d,%d uv %u,%u rgba %02X%02X%02X%02X texa %llX tex0 %llX", k,
                    v[k].x >> 4, v[k].y >> 4, v[k].u >> 4, v[k].v >> 4, v[k].r, v[k].g, v[k].b, v[k].a,
                    (unsigned long long)d->texa, (unsigned long long)d->tex0);
        return;
    }
    if ((d->prim & 0x10) && psm == PSMT8H && tbp == fbp && GS_FIELD(d->tex0, 37, 14) != 0) {
        uint32_t zbp = GS_FIELD(d->zbuf, 0, 9) * 32, fbw = GS_FIELD(d->frame, 16, 6), x, y, i;
        char line[512];
        size_t m;
        left--;
        LOG("hazedbg: haze draw tex0 %llX test %llX alpha %llX frame %llX zbuf %llX", (unsigned long long)d->tex0,
            (unsigned long long)d->test, (unsigned long long)d->alpha, (unsigned long long)d->frame,
            (unsigned long long)d->zbuf);
        for (y = 8; y < 448; y += 40) {
            m = 0;
            for (x = 8; x < 640; x += 56) {
                uint32_t z = gs_mem[gs_addr32z(zbp, fbw, x, y)] & 0xFFFFFF, a = gs_mem[gs_addr32(fbp, fbw, x, y)] >> 24;
                m += (size_t)snprintf(line + m, sizeof line - m, " %06X:%02X", z, a);
            }
            LOG("hazedbg: y %3u z:alpha%s", y, line);
        }
        gs_load_clut(d->tex0);
        gs_clut_sync();
        m = 0;
        for (i = 0; i < 256; i += 8)
            m += (size_t)snprintf(line + m, sizeof line - m, " %02X", gs_clut[i] >> 24);
        LOG("hazedbg: clut alpha every 8th:%s; rgb %06X", line, gs_clut[255] & 0xFFFFFF);
    }
}

static void rtdep_clut(uint32_t cbp, uint32_t cbw, uint32_t psm, uint32_t w, uint32_t h)
{
    if (rtdep_on())
        rtdep_report("clut", cbp, cbw, psm, w, h, 0);
}

/* an upload or transfer replaced a rectangle: no longer drawn */
static void rtdep_clear(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t dw, dh, bx, by;
    if (!rtdep_on())
        return;
    psm_blockdim(psm, &dw, &dh);
    for (by = y - y % dh; by < y + h; by += dh)
        for (bx = x - x % dw; bx < x + w; bx += dw) {
            uint32_t b = block_of(psm, bp, bw, bx, by) & (GS_MEM_WORDS / 64 - 1);
            if (g_drawn[b]) {
                g_drawn[b] = 0;
                g_drawn_gen++;
            }
        }
}
#endif

/* blocks: 0 leaves the blocks' versions to the caller (lazy uploads set
 * them as their data comes in) */
static void mark_rect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int blocks)
{
    GsBlkWalk wk;
    uint32_t px, py, ppr;
    RTDEP(rtdep_clear(bp, bw, psm, x, y, w, h));
    gs_bw_init(&wk, psm, bp, bw);
    ppr = wk.ppr ? wk.ppr : 1;
    gs_ver++;
    if (blocks) {
        uint32_t dw = 1u << wk.bws, dh = 1u << wk.bhs, bx, by;
        for (by = y & ~(dh - 1); by < y + h; by += dh) {
            GsBlkRow row = gs_bw_row(&wk, by);
            for (bx = x & ~(dw - 1); bx < x + w; bx += dw)
                gs_blk_ver[gs_bw_blk(&wk, row, bx)] = gs_ver;
        }
    }
    for (py = y >> wk.phs; py <= (y + h - 1) >> wk.phs; py++)
        for (px = x >> wk.pws; px <= (x + w - 1) >> wk.pws && px < ppr; px++) {
            uint32_t pg = bp / 32 + py * ppr + px;
            gs_page_ver[pg & 511] = gs_ver;
            if (bp & 31)                /* a base inside a page spills into the next one */
                gs_page_ver[(pg + 1) & 511] = gs_ver;
        }
}

void gs_mark_rect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    mark_rect(bp, bw, psm, x, y, w, h, 1);
}
int g_gs_dump;                  /* debugging ("gsdump"): fields left to write to work/gsdump.txt */
static int g_gs_path;           /* GIF path feeding the GS right now */
static FILE *g_gs_dump_f;

static uint32_t psm_bits(uint32_t psm)
{
    switch (psm) {
    case PSMCT32: case PSMZ32: return 32;
    case PSMCT24: case PSMZ24: return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H: return 8;
    default: return 4;
    }
}

static void start_transfer(uint32_t dir)
{
    uint64_t blt = g_regs[GS_BITBLTBUF], pos = g_regs[GS_TRXPOS], reg = g_regs[GS_TRXREG];
    uint32_t w = GS_FIELD(reg, 0, 12), h = GS_FIELD(reg, 32, 12);
    if (dir == 0) {
        g_xfer.active = w && h;
        g_xfer.dbp = GS_FIELD(blt, 32, 14);
        g_xfer.dbw = GS_FIELD(blt, 48, 6);
        g_xfer.dpsm = GS_FIELD(blt, 56, 6);
        g_xfer.x0 = GS_FIELD(pos, 32, 11);
        g_xfer.y0 = GS_FIELD(pos, 48, 11);
        g_xfer.w = w;
        g_xfer.h = h;
        g_xfer.x = g_xfer.y = 0;
        g_xfer.pending = g_xfer.pending_bytes = 0;
        g_gs_upload_bytes += w * h * psm_bits(g_xfer.dpsm) / 8;
        if (w && h)
            mark_rect(g_xfer.dbp, g_xfer.dbw, g_xfer.dpsm, g_xfer.x0, g_xfer.y0, w, h, 0);
        if (g_gs_dump && g_gs_dump_f)
            fprintf(g_gs_dump_f, "upload p%d bp %X bw %u psm %02X at %u,%u size %ux%u\n", g_gs_path, g_xfer.dbp,
                    g_xfer.dbw, g_xfer.dpsm, g_xfer.x0, g_xfer.y0, w, h);
        g_gs_uploads++;
        if (g_trace > 1)
            LOG("GS upload %ux%u psm %02X to bp %X bw %u at %u,%u", w, h, g_xfer.dpsm, g_xfer.dbp, g_xfer.dbw,
                g_xfer.x0, g_xfer.y0);
        g_lz.on = 0;
        if (g_xfer.active && !lazy_begin()) {
            static int logged;
            mark_rect(g_xfer.dbp, g_xfer.dbw, g_xfer.dpsm, g_xfer.x0, g_xfer.y0, w, h, 1);
            if (g_gs_lazy && logged < 40 && kernel_vblank_count() > 12000) {
                logged++;
                LOG("GS upload not lazy: %ux%u psm %02X to bp %X bw %u at %u,%u", w, h, g_xfer.dpsm, g_xfer.dbp,
                    g_xfer.dbw, g_xfer.x0, g_xfer.y0);
            }
            gs_sync_rect(g_xfer.dbp, g_xfer.dbw, g_xfer.dpsm, g_xfer.x0, g_xfer.y0, w, h);
        }
    } else if (dir == 2) {
        uint32_t sbp = GS_FIELD(blt, 0, 14), sbw = GS_FIELD(blt, 16, 6), spsm = GS_FIELD(blt, 24, 6);
        uint32_t dbp = GS_FIELD(blt, 32, 14), dbw = GS_FIELD(blt, 48, 6), dpsm = GS_FIELD(blt, 56, 6);
        uint32_t sx = GS_FIELD(pos, 0, 11), sy = GS_FIELD(pos, 16, 11);
        uint32_t dx = GS_FIELD(pos, 32, 11), dy = GS_FIELD(pos, 48, 11);
        uint32_t x, y;
        RTDEP(if (rtdep_on()) rtdep_report("xfer", sbp, sbw, spsm, sx + w, sy + h, dbp));
        if (w && h)
            gs_mark_rect(dbp, dbw, dpsm, dx, dy, w, h);
        gs_sync_rect(sbp, sbw, spsm, sx, sy, w, h);
        gs_sync_rect(dbp, dbw, dpsm, dx, dy, w, h);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                gs_write_pixel(dpsm, dbp, dbw, dx + x, dy + y, gs_read_pixel(spsm, sbp, sbw, sx + x, sy + y));
    } else if (dir == 1) {
        LOG("GS local->host transfer requested (not emulated)");
    }
}

/* ---- vertex queue ---------------------------------------------------------------------- */

static GsVertex g_vq[3];
static int g_vcount;
static float g_q = 1.0f;

/* Debugging ("gsstat" command): what the GS drew over the next fields, grouped
 * by source: GIF path, and for PATH1 the VU1 program that kicked it. */
int g_gs_stat;                  /* fields left to summarize */
uint32_t g_gs_skip = 0xFFFFFFFFu;   /* debugging ("gsskip"): VU1 program whose PATH1 draws are dropped */
uint32_t g_vu1_prog;            /* start address of the running VU1 program */

static void gs_dump_prim(const GsDraw *d, int type, const GsVertex *v, int n)
{
    int k;
    if (!g_gs_dump_f && !(g_gs_dump_f = fopen("work/gsdump.txt", "w")))
        return;
    fprintf(g_gs_dump_f, "p%d %04X t%d prim %03X frame %08llX zbuf %08llX test %05llX alpha %010llX tex0 %016llX",
            g_gs_path, g_gs_path == 1 ? g_vu1_prog : 0, type, d->prim, (unsigned long long)d->frame,
            (unsigned long long)d->zbuf, (unsigned long long)d->test, (unsigned long long)d->alpha,
            (unsigned long long)d->tex0);
    for (k = 0; k < n; k++)
        fprintf(g_gs_dump_f, " | %.1f %.1f %06X %02X%02X%02X%02X st %g %g q %g", v[k].x / 16.0, v[k].y / 16.0,
                v[k].z, v[k].r, v[k].g, v[k].b, v[k].a, (double)v[k].s, (double)v[k].t, (double)v[k].q);
    fputc('\n', g_gs_dump_f);
}
extern uint32_t g_vu1_code;     /* microcode chain last uploaded to VU1 (vif.c) */
typedef struct GsStat {
    uint32_t key, code;
    unsigned prims, offscreen, flat;
    int x0, y0, x1, y1;
    uint32_t z0, z1;
    uint64_t tex0, frame, alpha, test;
    uint32_t prim, amin, amax;
} GsStat;
static GsStat g_gstat[96];
static int g_gstat_n;

static void gs_stat_prim(const GsDraw *d, const GsVertex *v, int n)
{
    /* PATH2 and PATH3: by texture (TBP0, or 0x3FFF untextured) and target */
    uint32_t key = (uint32_t)g_gs_path << 16 |
                   (g_gs_path == 1 ? g_vu1_prog : (d->prim & 0x10) ? (uint32_t)(d->tex0 & 0x3FFF) : 0x3FFFu);
    uint64_t sc = d->scissor;
    int sx0 = (int)(sc & 0x7FF), sx1 = (int)((sc >> 16) & 0x7FF);
    int sy0 = (int)((sc >> 32) & 0x7FF), sy1 = (int)((sc >> 48) & 0x7FF);
    int k, out = 1;
    GsStat *s = NULL;
    uint32_t code = g_gs_path == 1 ? g_vu1_code : (uint32_t)(d->frame & 0x1FF) << 5;
    for (k = 0; k < g_gstat_n; k++)
        if (g_gstat[k].key == key && g_gstat[k].code == code)
            s = &g_gstat[k];
    if (!s) {
        if (g_gstat_n == 96)
            return;
        s = &g_gstat[g_gstat_n++];
        memset(s, 0, sizeof *s);
        s->key = key;
        s->code = code;
        s->x0 = s->y0 = 1 << 30;
        s->x1 = s->y1 = -(1 << 30);
        s->z0 = 0xFFFFFFFFu;
        s->amin = 255;
    }
    s->prims++;
    s->tex0 = d->tex0;
    s->frame = d->frame;
    s->alpha = d->alpha;
    s->test = d->test;
    s->prim = d->prim;
    for (k = 0; k < n; k++) {
        int x = v[k].x >> 4, y = v[k].y >> 4;
        if (x < s->x0) s->x0 = x;
        if (x > s->x1) s->x1 = x;
        if (y < s->y0) s->y0 = y;
        if (y > s->y1) s->y1 = y;
        if (v[k].z < s->z0) s->z0 = v[k].z;
        if (v[k].z > s->z1) s->z1 = v[k].z;
        if (v[k].a < s->amin) s->amin = v[k].a;
        if (v[k].a > s->amax) s->amax = v[k].a;
        if (x >= sx0 && x <= sx1 && y >= sy0 && y <= sy1)
            out = 0;
    }
    s->offscreen += out;
    if (n == 3 && (int64_t)(v[1].x - v[0].x) * (v[2].y - v[0].y) - (int64_t)(v[2].x - v[0].x) * (v[1].y - v[0].y) == 0)
        s->flat++;                          /* zero-area triangle */
}

static void gs_stat_dump(void)
{
    int k;
    LOG("gsstat: %d sources", g_gstat_n);
    for (k = 0; k < g_gstat_n; k++) {
        GsStat *s = &g_gstat[k];
        LOG("  path %u code %06X prog %04X: %6u prims (%u outside scissor, %u zero area) x %d..%d y %d..%d z %08X..%08X "
            "tex0 %016llX frame %08llX prim %03X alpha %010llX test %05llX a %02X..%02X", s->key >> 16, s->code,
            s->key & 0xFFFF, s->prims, s->offscreen, s->flat, s->x0, s->x1, s->y0, s->y1, s->z0, s->z1,
            (unsigned long long)s->tex0, (unsigned long long)s->frame, s->prim, (unsigned long long)s->alpha,
            (unsigned long long)s->test, s->amin, s->amax);
    }
    {
        extern uint32_t g_vif_deferred[4];
        LOG("gsstat: native pipelines since the last report: car batches %u native, %u emulated; world batches %u "
            "native, %u emulated", g_vif_deferred[0], g_vif_deferred[1], g_vif_deferred[2], g_vif_deferred[3]);
        memset(g_vif_deferred, 0, sizeof g_vif_deferred);
    }
    g_gstat_n = 0;
}

static void capture_draw(GsDraw *d)
{
    uint64_t prim = g_regs[GS_PRIM];
    uint32_t attrs;
    int ctx;
    if (g_regs[GS_PRMODECONT] & 1)
        attrs = (uint32_t)prim;
    else
        attrs = ((uint32_t)prim & 7) | ((uint32_t)g_regs[GS_PRMODE] & ~7u);
    ctx = (attrs >> 9) & 1;
    d->prim = attrs;
    d->frame = g_regs[GS_FRAME_1 + ctx];
    d->zbuf = g_regs[GS_ZBUF_1 + ctx];
    d->test = g_regs[GS_TEST_1 + ctx];
    d->alpha = g_regs[GS_ALPHA_1 + ctx];
    d->tex0 = g_regs[GS_TEX0_1 + ctx];
    d->tex1 = g_regs[GS_TEX1_1 + ctx];
    d->clamp = g_regs[GS_CLAMP_1 + ctx];
    d->scissor = g_regs[GS_SCISSOR_1 + ctx];
    d->xyoffset = g_regs[GS_XYOFFSET_1 + ctx];
    d->fba = g_regs[GS_FBA_1 + ctx];
    d->texa = g_regs[GS_TEXA];
    d->fogcol = g_regs[GS_FOGCOL];
    d->dthe = g_regs[GS_DTHE];
    d->colclamp = g_regs[GS_COLCLAMP];
    d->pabe = g_regs[GS_PABE];
    d->dimx = g_regs[GS_DIMX];
}

/* the draw state, captured again only after a register it depends on was written */
static GsDraw g_draw;
static int g_state_dirty = 1;

/* native draws: the state for a primitive whose GIF tag sets PRIM (PRE), in
 * place (valid until the next GS register write) */
const GsDraw *gs_draw_state(uint32_t prim)
{
    if (g_regs[GS_PRIM] != prim) {
        g_regs[GS_PRIM] = prim;
        g_state_dirty = 1;
    }
    if (g_state_dirty) {
        uint32_t seq = g_draw.seq + 1;
        capture_draw(&g_draw);
        g_draw.seq = seq;
        g_state_dirty = 0;
    }
    return &g_draw;
}

void gs_capture_draw(GsDraw *d, uint32_t prim)
{
    if (g_regs[GS_PRIM] != prim) {
        g_regs[GS_PRIM] = prim;
        g_state_dirty = 1;
    }
    if (g_state_dirty) {
        uint32_t seq = g_draw.seq + 1;
        capture_draw(&g_draw);
        g_draw.seq = seq;
        g_state_dirty = 0;
    }
    *d = g_draw;
}

static void vertex_kick(uint64_t xyz, int with_fog, int draw)
{
    GsVertex *v;
    uint64_t rgbaq = g_regs[GS_RGBAQ], st = g_regs[GS_ST], uv = g_regs[GS_UV];
    int type = (int)(g_regs[GS_PRIM] & 7);
    int ctx = (int)((g_regs[GS_PRMODECONT] & 1 ? g_regs[GS_PRIM] : g_regs[GS_PRMODE]) >> 9) & 1;
    uint64_t ofs = g_regs[GS_XYOFFSET_1 + ctx];
    int needed;
    if (g_vcount >= 3) {
        /* strips/fans keep sliding windows; others restart */
        g_vcount = 2;
    }
    v = &g_vq[g_vcount];
    v->x = (int32_t)(xyz & 0xFFFF) - (int32_t)GS_FIELD(ofs, 0, 16);
    v->y = (int32_t)((xyz >> 16) & 0xFFFF) - (int32_t)GS_FIELD(ofs, 32, 16);
    v->z = with_fog ? (uint32_t)((xyz >> 32) & 0xFFFFFF) : (uint32_t)(xyz >> 32);
    v->fog = with_fog ? (uint8_t)(xyz >> 56) : (uint8_t)(g_regs[GS_FOG] >> 56);
    v->r = (uint8_t)rgbaq;
    v->g = (uint8_t)(rgbaq >> 8);
    v->b = (uint8_t)(rgbaq >> 16);
    v->a = (uint8_t)(rgbaq >> 24);
    v->q = BITS2F((uint32_t)(rgbaq >> 32));
    v->s = BITS2F((uint32_t)st);
    v->t = BITS2F((uint32_t)(st >> 32));
    v->u = (uint32_t)(uv & 0x3FFF);
    v->v = (uint32_t)((uv >> 16) & 0x3FFF);
    g_vcount++;
    switch (type) {
    case 0: needed = 1; break;
    case 1: case 2: case 6: needed = 2; break;
    case 3: case 4: case 5: needed = 3; break;
    default: g_vcount = 0; return;
    }
    if (g_vcount < needed)
        return;
    if (draw) {
        if (g_state_dirty) {
            uint32_t seq = g_draw.seq + 1;
            capture_draw(&g_draw);
            g_draw.seq = seq;
            g_state_dirty = 0;
        }
        if (g_gs_stat)
            gs_stat_prim(&g_draw, g_vq, needed);
        if (g_gs_dump)
            gs_dump_prim(&g_draw, type, g_vq, needed);
        RTDEP(rtdep_draw(&g_draw, g_vq, needed));
        RTDEP(hazedbg_draw(&g_draw, g_vq, needed));
        if (!(g_gs_path == 1 && (g_vu1_prog == g_gs_skip || g_vu1_code == g_gs_skip))) {
            int prev = pt_enter(PT_PRIM);
            gs_backend_draw(&g_draw, type, g_vq, needed);
            pt_leave(prev);
        }
    }
    switch (type) {
    case 0: case 1: case 3: case 6:
        g_vcount = 0;
        break;
    case 2:
        g_vq[0] = g_vq[1];
        g_vcount = 1;
        break;
    case 4:
        g_vq[0] = g_vq[1];
        g_vq[1] = g_vq[2];
        g_vcount = 2;
        break;
    case 5:
        g_vq[1] = g_vq[2];
        g_vcount = 2;
        break;
    }
}

/* ---- register writes ----------------------------------------------------------------------- */

uint64_t gs_reg(uint32_t reg) { return g_regs[reg & 0xFF]; }

uint32_t g_gs_hold_missed;

/* ---- textures the game placed ------------------------------------------------- */

/* The game keeps its own cache of textures in GS memory and sends a texture
 * again whenever it places it (most of them, every frame of a race). Its
 * send function is replaced (runtime/hle.c, hle_tex_send): for a raster it
 * leaves two notes in the chain where the transfer was, and here that is a
 * record by TBP: which entry of the texture's upload script is there now.
 * No data moves and no block changes hands. A draw with that TBP gets the
 * texture by the identity of the entry's data in guest RAM (ext_ident, as
 * uploads read in place); the backend decodes it, when it has not got it,
 * from GS memory after gs_placed_load has written it there.
 *
 * A record holds while nothing wrote its first block since: whatever is
 * sent the old way wins (palettes still are). The record takes a version of
 * its own (++gs_ver): a transfer after it gives the block that version or a
 * later one (an upload read in place takes gs_ver as it is), one before it
 * an earlier one. */
typedef struct GsPlaced {
    uint32_t entry, ident, ver;         /* entry | 1: written to GS memory since (gs_placed_load) */
} GsPlaced;
static int g_placed_writing;
static GsPlaced *g_placed;              /* by TBP; allocated when first used */
static uint32_t g_placed_bits[16384 / 32];  /* the records with an entry: a placement clears a texture's whole allocation (up
                                         * to 4096 blocks) and only these need it */
static uint32_t g_place_entry;          /* from the first of the two notes */
uint32_t g_gs_placed, g_gs_placed_loads;    /* status line */

/* the transfer of a script entry (guest address): shape, data, words; 0: not one to place */
static int placed_shape(uint32_t e, uint32_t *dbw, uint32_t *dpsm, uint32_t *w, uint32_t *h, const uint32_t **data,
                        uint32_t *words, uint32_t *qwords)
{
    uint32_t bbhi, tag, pkt, img, bits, need;
    if (e < 0x4000 || e > PS2_RAM_SIZE - 64 || (e & 15))
        return 0;
    bbhi = LW(e + 36);
    tag = LW(e + 48);
    pkt = LW(e + 52) & 0x7FFFFFFFu;
    if ((bbhi & 0x3FFF) || ((tag >> 28) & 7) != 3 || pkt < 0x4000 || pkt > PS2_RAM_SIZE - 96 || (pkt & 15))
        return 0;
    *dbw = (bbhi >> 16) & 0x3F;
    *dpsm = (bbhi >> 24) & 0x3F;
    if (*dpsm != PSMCT32 && *dpsm != PSMCT16)
        return 0;
    bits = *dpsm == PSMCT32 ? 32 : 16;
    /* the packet: TRXPOS 0, TRXREG, TRXDIR 0, an IMAGE tag and the data (then
     * nothing, or another transfer into the same buffer: the palette) */
    if (LW(pkt) != 3 || LW(pkt + 4) != 0x10000000u || LW(pkt + 8) != 0xE || LW(pkt + 16) || LW(pkt + 20) ||
        LW(pkt + 24) != 0x51 || LW(pkt + 40) != 0x52 || LW(pkt + 48) || LW(pkt + 56) != 0x53)
        return 0;
    *w = LW(pkt + 32) & 0xFFF;
    *h = LW(pkt + 36) & 0xFFF;
    img = LW(pkt + 64);
    if (!*w || !*h || *w > 1024 || *h > 1024 || ((LW(pkt + 68) >> 26) & 3) != 2)
        return 0;
    need = (*w * *h * bits + 127) / 128;
    if ((img & 0x7FFF) != need || (tag & 0xFFFF) < 5 + need || pkt + 80 + need * 16 > PS2_RAM_SIZE)
        return 0;
    *words = (*w * *h * bits + 31) / 32;
    if (*words < 8)
        return 0;
    *data = (const uint32_t *)GPTR(pkt + 80);
    *qwords = 5 + need;
    if ((tag & 0xFFFF) == 5 + need)
        return 1;
    /* more: it must start as a transfer does */
    return (tag & 0xFFFF) >= 5 + need + 6 && LW(pkt + 80 + need * 16) == 3 && LW(pkt + 80 + need * 16 + 24) == 0x51 ? 2 : 0;
}

int gs_placed_entry_ok(uint32_t entry, uint32_t *qwords)
{
    uint32_t dbw, dpsm, w, h, words;
    const uint32_t *d;
    return placed_shape(entry, &dbw, &dpsm, &w, &h, &d, &words, qwords);
}

static void placed_write(uint32_t tbp, uint32_t e)
{
    uint32_t dbw, dpsm, w, h, words, qw, x, y, row[1024];
    const uint32_t *d;
    int prev;
    if (!placed_shape(e, &dbw, &dpsm, &w, &h, &d, &words, &qw))
        return;
    prev = pt_enter(PT_XFER);
    if (!dbw)
        dbw = 1;
    g_placed_writing++;
    gs_sync_rect(tbp, dbw, dpsm, 0, 0, w, h);
    g_placed_writing--;
    mark_rect(tbp, dbw, dpsm, 0, 0, w, h, 1);
    for (y = 0; y < h; y++) {
        if (dpsm == PSMCT32) {
            put_row(dpsm, tbp, dbw, 0, y, d + y * w, w);
        } else {
            const uint32_t i0 = y * w;
            for (x = 0; x < w; x++)
                row[x] = (d[(i0 + x) >> 1] >> (((i0 + x) & 1) * 16)) & 0xFFFF;
            put_row(dpsm, tbp, dbw, 0, y, row, w);
        }
    }
    g_gs_placed_loads++;
    pt_leave(prev);
}

static void gs_place(uint32_t tbp)
{
    /* the note holds the game's texture: its upload script at +36, the size
     * of its place (words of GS memory, the palette and the smaller levels
     * with it) at +100 */
    const uint32_t tex = g_place_entry;
    uint32_t e, dbw, dpsm, w, h, words, qw, nblk, k;
    const uint32_t *d;
    GsPlaced *p;
    g_place_entry = 0;
    if (tex < 0x4000 || tex > PS2_RAM_SIZE - 128)
        return;
    e = LW(tex + 36) + 16;
    nblk = (LW(tex + 100) + 63) / 64;
    if (!placed_shape(e, &dbw, &dpsm, &w, &h, &d, &words, &qw))
        return;
    if (!g_placed) {
        g_placed = plat_alloc(16384 * sizeof *g_placed);
        if (!g_placed) {
            placed_write(tbp, e);       /* no memory for the records: sent after all */
            return;
        }
        memset(g_placed, 0, 16384 * sizeof *g_placed);
    }
    /* the records of whatever was placed in this texture's blocks before
     * are over (their first blocks get no new version from a note) */
    if (nblk > 4096)
        nblk = 4096;
    for (k = 0; k < nblk;) {
        const uint32_t i = (tbp + k) & 16383, w = i >> 5, b = i & 31;
        uint32_t span = 32 - b, live, m;
        if (span > nblk - k)
            span = nblk - k;
        m = span == 32 ? 0xFFFFFFFFu : ((1u << span) - 1u) << b;
        live = g_placed_bits[w] & m;
        if (live) {
            g_placed_bits[w] &= ~m;
            do {
                g_placed[(w << 5) + (uint32_t)__builtin_ctz(live)].entry = 0;
                live &= live - 1;
            } while (live);
        }
        k += span;
    }
    p = &g_placed[tbp & 16383];
    g_placed_bits[(tbp & 16383) >> 5] |= 1u << (tbp & 31);
    p->entry = e;
    p->ident = ext_source_ident(d, words, w * (dpsm == PSMCT32 ? 32u : 16u) / 32, dpsm, h) ^ dbw * 0x9E3779B1u;
    p->ver = ++gs_ver;
    g_gs_placed++;
}

int gs_placed_for(uint32_t tbp, uint32_t *ident)
{
    const GsPlaced *p;
    if (!g_placed)
        return 0;
    p = &g_placed[tbp & 16383];
    if (!p->entry || (int32_t)(gs_blk_ver[tbp & 16383] - p->ver) >= 0)
        return 0;
    *ident = p->ident;
    return 1;
}

#ifdef MEMOSTAT
/* -DMEMOSTAT (debug, plan-60fps.md): the version of the record that holds at tbp, 0 when none does */
uint32_t gs_placed_ver(uint32_t tbp)
{
    const GsPlaced *p;
    if (!g_placed)
        return 0;
    p = &g_placed[tbp & 16383];
    if (!p->entry || (int32_t)(gs_blk_ver[tbp & 16383] - p->ver) >= 0)
        return 0;
    return p->ver;
}
#endif

/* The transfer of what the game placed at tbp, for a backend that decodes
 * it from there (the image's buffer width and format, its size in its own
 * pixels, the data in guest RAM); 0: nothing placed there now. */
int gs_placed_source(uint32_t tbp, uint32_t *dbw, uint32_t *dpsm, uint32_t *w, uint32_t *h, const uint32_t **data)
{
    const GsPlaced *p;
    uint32_t words, qw;
    if (!g_placed)
        return 0;
    p = &g_placed[tbp & 16383];
    if (!p->entry || (int32_t)(gs_blk_ver[tbp & 16383] - p->ver) >= 0)
        return 0;
    if (!placed_shape(p->entry & ~1u, dbw, dpsm, w, h, data, &words, &qw))
        return 0;
    if (!*dbw)
        *dbw = 1;
    return 1;
}

void gs_placed_load(uint32_t tbp)
{
    GsPlaced *p;
    if (!g_placed)
        return;
    p = &g_placed[tbp & 16383];
    if (!p->entry || (p->entry & 1))
        return;
    placed_write(tbp, p->entry);
    p->entry |= 1;
    p->ver = ++gs_ver;
}

/* before GS memory at bp is read: what the game placed there, if it still holds */
static void placed_sync(uint32_t bp)
{
    const GsPlaced *p;
    if (!g_placed || g_placed_writing)
        return;
    p = &g_placed[bp & 16383];
    if (p->entry && !(p->entry & 1) && (int32_t)(gs_blk_ver[bp & 16383] - p->ver) < 0)
        gs_placed_load(bp);
}

void gs_write_reg(uint32_t reg, uint64_t v)
{
    reg &= 0xFF;
    switch (reg) {
    case GS_PRIM:
        g_regs[reg] = v;
        g_vcount = 0;
        g_state_dirty = 1;
        return;
    case GS_XYZ2:
        vertex_kick(v, 0, 1);
        return;
    case GS_XYZF2:
        vertex_kick(v, 1, 1);
        return;
    case GS_XYZ3:
        vertex_kick(v, 0, 0);
        return;
    case GS_XYZF3:
        vertex_kick(v, 1, 0);
        return;
    case GS_ST:
        g_regs[reg] = v;
        return;
    case GS_TEX0_1:
    case GS_TEX0_2:
        /* the draw state is captured again only when it changed (a GS packet sends the same TEX0 at every batch) */
        if (g_regs[reg] != v || !g_opt_new) {
            g_regs[reg] = v;
            g_state_dirty = 1;
        }
        gs_load_clut(v);
        return;
    case GS_TEX2_1:
    case GS_TEX2_2: {
        /* TEX2 updates only the CLUT-related fields of TEX0 */
        uint64_t mask = 0xFFFFFFE003F00000ull;   /* PSM + CBP..CLD */
        uint64_t *t0 = &g_regs[reg == GS_TEX2_1 ? GS_TEX0_1 : GS_TEX0_2];
        const uint64_t n0 = (*t0 & ~mask) | (v & mask);
        if (*t0 != n0 || g_regs[reg] != v || !g_opt_new) {
            *t0 = n0;
            g_regs[reg] = v;
            g_state_dirty = 1;
        }
        gs_load_clut(*t0);
        return;
    }
    case GS_TRXDIR:
        g_regs[reg] = v;
        start_transfer((uint32_t)v & 3);
        return;
    case GS_HWREG:
        xfer_word((uint32_t)v);
        xfer_word((uint32_t)(v >> 32));
        return;
    case GS_SIGNAL: {
        uint64_t id = gs_priv_reg(GS_SIGLBLID);
        uint32_t mask = (uint32_t)(v >> 32);
        id = (id & ~(uint64_t)mask) | ((uint32_t)v & mask);
        (void)id;
        gs_set_csr_bits(0x1);
        if (!(gs_priv_reg(GS_IMR) & 0x100))
            kernel_raise_intc(INTC_GS);
        return;
    }
    case GS_FINISH:
        gs_backend_flush();
        gs_set_csr_bits(0x2);
        if (!(gs_priv_reg(GS_IMR) & 0x200))
            kernel_raise_intc(INTC_GS);
        return;
    case GS_LABEL:
        return;
    case GS_RGBAQ: case GS_UV: case GS_FOG:
        g_regs[reg] = v;
        return;
    case 0x7F:                      /* NOP: or a note for the backend (gs.h) */
        if (((uint32_t)(v >> 32) & 0xFFFFC000u) == (GS_NOTE_MAGIC & 0xFFFFC000u) &&
            ((uint32_t)v & 0xF0000000u) == GS_NOTE_TEX_HERE) {
            /* texture and TBP in one note: the TBP in the magic's low bits */
            g_place_entry = ((uint32_t)v & 0x0FFFFFFFu) << 2;
            gs_place((uint32_t)(v >> 32) & 0x3FFF);
        } else if ((uint32_t)(v >> 32) == GS_NOTE_MAGIC) {
            uint32_t note = (uint32_t)v;
            if ((note & 0xF0000000u) == GS_NOTE_TEX_ENTRY)
                g_place_entry = (note & 0x0FFFFFFFu) << 2;
            else if ((note & 0xF0000000u) == GS_NOTE_TEX_AT)
                gs_place(note & 0x3FFF);
            else
                gs_backend_note(note);
        }
        return;
    default:
        if (g_regs[reg] != v || !g_opt_new) {
            g_regs[reg] = v;
            g_state_dirty = 1;      /* a register the draw state holds (the same value again: nothing to capture) */
        }
        return;
    }
}

/* ---- vertex packets in bulk ------------------------------------------------------------------- */

/* A PACKED packet whose registers are all vertex data (RGBAQ, ST, UV, XYZF2,
 * XYZ2 and their no-kick forms, NOP) for a triangle or sprite PRIM is built
 * into vertices in one loop and handed to the backend in one call
 * (gs_backend_prims), not vertex by vertex and primitive by primitive. The
 * vertex queue carries over: its vertices start the array, and the array's
 * last ones become the queue. */
#define PK_MAX 1024
static GsVertex g_pk[PK_MAX + 4];
static uint8_t g_pk_kick[PK_MAX + 4];

/* primitive by primitive through gs_backend_draw: backends without a bulk path */
void gs_prims_each(const GsDraw *d, int type, const GsVertex *v, int n, const uint8_t *kick)
{
    GsVertex t[3];
    int i;
    for (i = 0; i < n; i++) {
        if (!kick[i])
            continue;
        switch (type) {
        case 3: case 4: gs_backend_draw(d, type, &v[i - 2], 3); break;
        case 5: t[0] = v[0]; t[1] = v[i - 1]; t[2] = v[i]; gs_backend_draw(d, type, t, 3); break;
        case 6: gs_backend_draw(d, type, &v[i - 1], 2); break;
        }
    }
}

/* the vertex queue after the array's vertices (those from base on: a PRIM
 * write restarted the queue there) */
static void pk_queue(int type, int n, int base)
{
    int r, k, m = n - base;
    switch (type) {
    case 3: case 6:
        r = m % (type == 3 ? 3 : 2);
        for (k = 0; k < r; k++)
            g_vq[k] = g_pk[n - r + k];
        g_vcount = r;
        break;
    case 4:
        if (m >= 2) {
            g_vq[0] = g_pk[n - 2];
            g_vq[1] = g_pk[n - 1];
            g_vcount = 2;
        } else {
            if (m)
                g_vq[0] = g_pk[n - 1];
            g_vcount = m;
        }
        break;
    default:            /* fan: its first vertex stays */
        if (m)
            g_vq[0] = g_pk[base];
        if (m >= 2)
            g_vq[1] = g_pk[n - 1];
        g_vcount = m >= 2 ? 2 : m;
        break;
    }
}

static void pk_draw(int type, int n, int kicks)
{
    if (g_state_dirty) {
        uint32_t seq = g_draw.seq + 1;
        capture_draw(&g_draw);
        g_draw.seq = seq;
        g_state_dirty = 0;
    }
    if (kicks && !(g_gs_path == 1 && (g_vu1_prog == g_gs_skip || g_vu1_code == g_gs_skip))) {
        int prev = pt_enter(PT_PRIM);
        gs_backend_prims(&g_draw, type, g_pk, n, g_pk_kick);
        pt_leave(prev);
    }
}

/* registers the bulk path takes: vertex data, PRIM, FOG, NOP (PACKED: not A+D) */
static int bulk_reg_ok(uint32_t r, int reglist)
{
    return r <= 5 || r == 0xA || r == 0xC || r == 0xD || r == 0xF || (reglist && r == 0xE);
}

/* register k of loop i: the qword (PACKED) or its 64-bit half (REGLIST) */
#define PK_REG(i, k) (reglist ? mem + 4 * ((a + (((i) * nreg + (k)) >> 1)) & qmask) + 2 * ((((i) * nreg + (k))) & 1) \
                              : mem + 4 * ((a + (i) * nreg + (k)) & qmask))

/* A PACKED or REGLIST packet of vertex data. mem: the qwords (qaddr & qmask
 * indexes them), a: the packet's first data qword. Returns 0 when it needs
 * the general path (nothing done then). */
static int packet_bulk(const uint32_t *mem, uint32_t qmask, uint32_t a, uint32_t nloop, uint32_t nreg, uint64_t regs,
                       int reglist)
{
    int type = (int)(g_regs[GS_PRIM] & 7), ctx, n, kicks = 0, base = 0, has_prim = 0;
    uint64_t rgbaq = g_regs[GS_RGBAQ], st = g_regs[GS_ST], uv = g_regs[GS_UV];
    uint32_t i, k, r8, g8, b8, a8, fog = (uint32_t)(g_regs[GS_FOG] >> 56), ofx, ofy;
    float q = BITS2F((uint32_t)(rgbaq >> 32)), s = BITS2F((uint32_t)st), t = BITS2F((uint32_t)(st >> 32));
    uint32_t u = (uint32_t)(uv & 0x3FFF), v = (uint32_t)((uv >> 16) & 0x3FFF);
    if (g_gs_dump || g_gs_stat)
        return 0;
#if !defined(GEKKO)
    {
        /* off for the debugging reports that look at every primitive, and with B3_NOBULK=1 */
        static int off = -1;
        const char *e;
        if (off < 0)
            off = ((e = getenv("B3_HAZEDBG")) && *e && *e != '0') || ((e = getenv("B3_NOBULK")) && *e && *e != '0');
        if (off || rtdep_on())
            return 0;
    }
#endif
    for (k = 0; k < nreg; k++) {
        uint32_t r = (uint32_t)(regs >> (4 * k)) & 15;
        if (!bulk_reg_ok(r, reglist))
            return 0;
        has_prim |= r == 0;
    }
    if (has_prim) {
        /* PRIM written inside: every value must be a triangle or sprite type */
        for (i = 0; i < nloop; i++)
            for (k = 0; k < nreg; k++)
                if (((regs >> (4 * k)) & 15) == 0) {
                    uint32_t pt = PK_REG(i, k)[0] & 7;
                    if (pt < 3 || pt > 6)
                        return 0;
                }
    }
    if ((type < 3 || type > 6) && (regs & 15) != 0)
        return 0;                       /* vertices before a PRIM write would need the current type */
    ctx = (int)((g_regs[GS_PRMODECONT] & 1 ? g_regs[GS_PRIM] : g_regs[GS_PRMODE]) >> 9) & 1;
    ofx = GS_FIELD(g_regs[GS_XYOFFSET_1 + ctx], 0, 16);
    ofy = GS_FIELD(g_regs[GS_XYOFFSET_1 + ctx], 32, 16);
    r8 = (uint32_t)rgbaq & 0xFF;
    g8 = (uint32_t)(rgbaq >> 8) & 0xFF;
    b8 = (uint32_t)(rgbaq >> 16) & 0xFF;
    a8 = (uint32_t)(rgbaq >> 24) & 0xFF;
    if (g_vcount >= 3)
        g_vcount = 2;
    n = 0;
    if (type >= 3 && type <= 6)
        for (; n < g_vcount; n++) {
            g_pk[n] = g_vq[n];
            g_pk_kick[n] = 0;
        }
    for (i = 0; i < nloop; i++)
        for (k = 0; k < nreg; k++) {
            const uint32_t *d = PK_REG(i, k);
            uint32_t r = (uint32_t)(regs >> (4 * k)) & 15, kick;
            GsVertex *o;
            int done;
            switch (r) {
            case 0x0: {
                uint32_t np = d[0] & 0x7FF;
                if (np != (uint32_t)g_regs[GS_PRIM] || (np & 7) == 5) {
                    /* another primitive: what is built so far goes first */
                    pk_draw(type, n, kicks);
                    n = base = kicks = 0;
                    if (np != (uint32_t)g_regs[GS_PRIM]) {
                        g_regs[GS_PRIM] = np;
                        g_state_dirty = 1;
                        /* its context may be the other one: that one's offset
                         * from here on (the game's clears of its reflection
                         * views, sent after a draw in context 2, went 1728
                         * pixels off their buffers with the old one) */
                        ctx = (int)((g_regs[GS_PRMODECONT] & 1 ? g_regs[GS_PRIM] : g_regs[GS_PRMODE]) >> 9) & 1;
                        ofx = GS_FIELD(g_regs[GS_XYOFFSET_1 + ctx], 0, 16);
                        ofy = GS_FIELD(g_regs[GS_XYOFFSET_1 + ctx], 32, 16);
                    }
                    type = (int)(np & 7);
                } else {
                    base = n;           /* the same: the vertex queue restarts */
                }
                g_vcount = 0;
                continue;
            }
            case 0x1:
                r8 = d[0] & 0xFF;
                if (reglist) {
                    g8 = (d[0] >> 8) & 0xFF; b8 = (d[0] >> 16) & 0xFF; a8 = d[0] >> 24;
                    q = BITS2F(d[1]);
                } else {
                    g8 = d[1] & 0xFF; b8 = d[2] & 0xFF; a8 = d[3] & 0xFF;
                    q = g_q;
                }
                continue;
            case 0x2:
                s = BITS2F(d[0]);
                t = BITS2F(d[1]);
                if (!reglist)
                    g_q = BITS2F(d[2]);
                continue;
            case 0x3:
                u = d[0] & 0x3FFF;
                v = reglist ? (d[0] >> 16) & 0x3FFF : d[1] & 0x3FFF;
                continue;
            case 0xA:
                fog = reglist ? d[1] >> 24 : (d[3] >> 4) & 0xFF;
                continue;
            case 0x4: case 0x5: case 0xC: case 0xD:
                break;
            default:
                continue;
            }
            if (n == PK_MAX) {
                pk_draw(type, n, kicks);
                pk_queue(type, n, base);
                for (n = 0; n < g_vcount; n++) {
                    g_pk[n] = g_vq[n];
                    g_pk_kick[n] = 0;
                }
                base = kicks = 0;
            }
            o = &g_pk[n];
            if (reglist) {
                o->x = (int32_t)(d[0] & 0xFFFF) - (int32_t)ofx;
                o->y = (int32_t)(d[0] >> 16) - (int32_t)ofy;
                if (r == 0x4 || r == 0xC) {
                    o->z = d[1] & 0xFFFFFF;
                    o->fog = (uint8_t)(d[1] >> 24);
                } else {
                    o->z = d[1];
                    o->fog = (uint8_t)fog;
                }
                kick = r == 0x4 || r == 0x5;
            } else {
                o->x = (int32_t)(d[0] & 0xFFFF) - (int32_t)ofx;
                o->y = (int32_t)(d[1] & 0xFFFF) - (int32_t)ofy;
                if (r == 0x4 || r == 0xC) {
                    o->z = (d[2] >> 4) & 0xFFFFFF;
                    o->fog = (uint8_t)(d[3] >> 4);
                } else {
                    o->z = d[2];
                    o->fog = (uint8_t)fog;
                }
                kick = (r == 0x4 || r == 0x5) && !((d[3] >> 15) & 1);
            }
            o->r = (uint8_t)r8; o->g = (uint8_t)g8; o->b = (uint8_t)b8; o->a = (uint8_t)a8;
            o->q = q;
            o->s = s;
            o->t = t;
            o->u = u;
            o->v = v;
            switch (type) {
            case 3: done = (n - base) % 3 == 2; break;
            case 6: done = (n - base) & 1; break;
            default: done = n - base >= 2; break;
            }
            g_pk_kick[n] = (uint8_t)(done && kick);
            kicks |= g_pk_kick[n];
            n++;
        }
    /* registers as the per-vertex path leaves them */
    g_regs[GS_RGBAQ] = r8 | g8 << 8 | b8 << 16 | (uint64_t)a8 << 24 | (uint64_t)F2BITS(q) << 32;
    g_regs[GS_ST] = F2BITS(s) | (uint64_t)F2BITS(t) << 32;
    g_regs[GS_UV] = u | (uint64_t)v << 16;
    g_regs[GS_FOG] = (uint64_t)fog << 56;
    pk_draw(type, n, kicks);
    pk_queue(type, n, base);
    return 1;
}
#undef PK_REG


/* ---- GIF ---------------------------------------------------------------------------------- */

typedef struct GifPath {
    int in_packet;
    uint32_t nloop, nreg, flg, eop, reg_idx;
    uint64_t regs;
} GifPath;

static GifPath g_path[4];

static void packed_reg(uint32_t r, const uint32_t *q)
{
    switch (r) {
    case 0x0: gs_write_reg(GS_PRIM, q[0] & 0x7FF); break;
    case 0x1: {
        uint64_t rgba = (q[0] & 0xFF) | (q[1] & 0xFF) << 8 | (q[2] & 0xFF) << 16 | (uint64_t)(q[3] & 0xFF) << 24;
        g_regs[GS_RGBAQ] = rgba | (uint64_t)F2BITS(g_q) << 32;
        break;
    }
    case 0x2:
        g_regs[GS_ST] = q[0] | (uint64_t)q[1] << 32;
        g_q = BITS2F(q[2]);
        break;
    case 0x3: g_regs[GS_UV] = (q[0] & 0x3FFF) | (uint64_t)(q[1] & 0x3FFF) << 16; break;
    case 0x4: {
        uint64_t v = (q[0] & 0xFFFF) | (uint64_t)(q[1] & 0xFFFF) << 16 |
                     (uint64_t)((q[2] >> 4) & 0xFFFFFF) << 32 | (uint64_t)((q[3] >> 4) & 0xFF) << 56;
        gs_write_reg((q[3] >> 15) & 1 ? GS_XYZF3 : GS_XYZF2, v);
        break;
    }
    case 0x5: {
        uint64_t v = (q[0] & 0xFFFF) | (uint64_t)(q[1] & 0xFFFF) << 16 | (uint64_t)q[2] << 32;
        gs_write_reg((q[3] >> 15) & 1 ? GS_XYZ3 : GS_XYZ2, v);
        break;
    }
    case 0x6: case 0x7: case 0x8: case 0x9:
        gs_write_reg(r, q[0] | (uint64_t)q[1] << 32);
        break;
    case 0xA: g_regs[GS_FOG] = (uint64_t)((q[3] >> 4) & 0xFF) << 56; break;
    case 0xC: {
        uint64_t v = (q[0] & 0xFFFF) | (uint64_t)(q[1] & 0xFFFF) << 16 |
                     (uint64_t)((q[2] >> 4) & 0xFFFFFF) << 32 | (uint64_t)((q[3] >> 4) & 0xFF) << 56;
        gs_write_reg(GS_XYZF3, v);
        break;
    }
    case 0xD: gs_write_reg(GS_XYZ3, (q[0] & 0xFFFF) | (uint64_t)(q[1] & 0xFFFF) << 16 | (uint64_t)q[2] << 32); break;
    case 0xE: gs_write_reg(q[2] & 0xFF, q[0] | (uint64_t)q[1] << 32); break;
    default: break;
    }
}

void gif_path_qword(int path, const uint32_t *qw)
{
    GifPath *p = &g_path[path];
    g_gs_path = path;
    if (!p->in_packet) {
        uint64_t lo = qw[0] | (uint64_t)qw[1] << 32;
        p->nloop = (uint32_t)(lo & 0x7FFF);
        p->eop = (uint32_t)(lo >> 15) & 1;
        p->flg = (uint32_t)(lo >> 58) & 3;
        p->nreg = (uint32_t)(lo >> 60) & 15;
        if (!p->nreg)
            p->nreg = 16;
        p->regs = qw[2] | (uint64_t)qw[3] << 32;
        p->reg_idx = 0;
        if ((lo >> 46) & 1)
            gs_write_reg(GS_PRIM, (lo >> 47) & 0x7FF);
        g_q = 1.0f;
        p->in_packet = p->nloop != 0;
        return;
    }
    switch (p->flg) {
    case 0: /* PACKED */
        packed_reg((uint32_t)(p->regs >> (4 * p->reg_idx)) & 15, qw);
        if (++p->reg_idx == p->nreg) {
            p->reg_idx = 0;
            if (--p->nloop == 0)
                p->in_packet = 0;
        }
        break;
    case 1: { /* REGLIST: two 64-bit registers per qword */
        int half;
        for (half = 0; half < 2 && p->in_packet; half++) {
            uint32_t r = (uint32_t)(p->regs >> (4 * p->reg_idx)) & 15;
            uint64_t v = qw[2 * half] | (uint64_t)qw[2 * half + 1] << 32;
            if (r == 0xE || r == 0xF) {
                /* A+D and NOP have no meaning in REGLIST */
            } else if (r <= 0xD) {
                static const uint8_t map[14] = {GS_PRIM, GS_RGBAQ, GS_ST, GS_UV, GS_XYZF2, GS_XYZ2, GS_TEX0_1,
                                                GS_TEX0_2, GS_CLAMP_1, GS_CLAMP_2, GS_FOG, 0, GS_XYZF3, GS_XYZ3};
                if (r != 0xB)
                    gs_write_reg(map[r], v);
            }
            if (++p->reg_idx == p->nreg) {
                p->reg_idx = 0;
                if (--p->nloop == 0)
                    p->in_packet = 0;
            }
        }
        break;
    }
    default: /* IMAGE */
        if (g_lz.on) {
            g_lz_path = path;
            lazy_data(qw, 4);
        } else {
            xfer_word(qw[0]);
            xfer_word(qw[1]);
            xfer_word(qw[2]);
            xfer_word(qw[3]);
        }
        if (--p->nloop == 0)
            p->in_packet = 0;
        break;
    }
}

/* Image data in bulk: inside an IMAGE packet, take up to n qwords at once
 * from host-order words. Returns the qwords taken (0 when not in image data). */
uint32_t gif_path_image_host(int path, const uint32_t *src, uint32_t n)
{
    GifPath *p = &g_path[path];
    uint32_t k;
    if (!p->in_packet || p->flg < 2)
        return 0;
    if (n > p->nloop)
        n = p->nloop;
    if (!n)
        return 0;
    g_gs_path = path;
    if (g_lz.on) {
        g_lz_path = path;
        lazy_data(src, n * 4);
    } else {
        xfer_words(src, n * 4);
    }
    p->nloop -= n;
    if (!p->nloop)
        p->in_packet = 0;
    return n;
}

/* the same from guest RAM */
uint32_t gif_path_image(int path, uint32_t gaddr, uint32_t n)
{
    if ((gaddr & PS2_RAM_MASK) + n * 16 > PS2_RAM_MASK + 1)
        n = (PS2_RAM_MASK + 1 - (gaddr & PS2_RAM_MASK)) / 16;
    return gif_path_image_host(path, (const uint32_t *)GPTR(gaddr), n);
}

/* PATH2/PATH3 data in bulk: n qwords of host-order words. Whole vertex
 * packets go through packet_bulk, image data in one piece; anything else a
 * qword at a time. Returns the qwords taken (at least 1). */
uint32_t gif_path_bulk(int path, const uint32_t *q, uint32_t n)
{
    GifPath *p = &g_path[path];
    uint32_t t;
    if (p->in_packet && p->flg >= 2) {
        t = gif_path_image_host(path, q, n);
        if (t)
            return t;
    }
    if (!p->in_packet && n > 1) {
        uint32_t nloop = q[0] & 0x7FFF, flg = (q[1] >> 26) & 3, nreg = q[1] >> 28, need;
        uint64_t regs = q[2] | (uint64_t)q[3] << 32;
        if (!nreg)
            nreg = 16;
        need = flg == 0 ? nloop * nreg : (nloop * nreg + 1) / 2;
        if (flg == 0 && nloop && 1 + (uint64_t)need <= n &&
            ((regs ^ 0xEEEEEEEEEEEEEEEEull) & (0xFFFFFFFFFFFFFFFFull >> (64 - 4 * nreg))) == 0) {
            /* A+D only (texture upload headers, GS state): each qword a
             * register write, as packed_reg does it, without the per-qword
             * state machine */
            const uint32_t *d = q + 4, *e = q + 4 + 4 * need;
            g_gs_path = path;
            if ((q[1] >> 14) & 1)
                gs_write_reg(GS_PRIM, (q[1] >> 15) & 0x7FF);
            g_q = 1.0f;
            for (; d < e; d += 4)
                gs_write_reg(d[2] & 0xFF, d[0] | (uint64_t)d[1] << 32);
            p->eop = (q[0] >> 15) & 1;
            p->in_packet = 0;
            return 1 + need;
        }
        if (flg < 2 && nloop && 1 + (uint64_t)need <= n) {
            uint32_t prim0 = (uint32_t)g_regs[GS_PRIM], vc = (uint32_t)g_vcount;
            int dirty0 = g_state_dirty;
            /* The game's static strips (the three views folded into the cars'
             * sphere map: 446 vertices each, the same every frame, from the
             * same memory): the backend may have them as display lists */
            if (path == 2 && flg == 1 && nreg == 2 && ((uint32_t)regs & 0xFF) == 0x52 && nloop >= 64 && !((q[1] >> 14) & 1) &&
                (g_regs[GS_PRIM] & 0x327) == 0x004 && (g_regs[GS_PRMODECONT] & 1) && !g_vcount && !g_gs_dump && !g_gs_stat &&
                1 + (uint64_t)need <= n && gs_backend_strip(q + 4, nloop)) {
                g_gs_path = path;
                g_q = BITS2F((uint32_t)(g_regs[GS_RGBAQ] >> 32));
                gs_strip_vertex(q + 4, nloop - 2, &g_vq[0]);
                gs_strip_vertex(q + 4, nloop - 1, &g_vq[1]);
                g_vcount = 2;
                g_regs[GS_ST] = q[4 * nloop] | (uint64_t)q[4 * nloop + 1] << 32;
                g_regs[GS_XYZ2] = q[4 * nloop + 2] | (uint64_t)q[4 * nloop + 3] << 32;
                p->eop = (q[0] >> 15) & 1;
                p->in_packet = 0;
                return 1 + need;
            }
            if (g_gs_stat && path == 2 && (g_regs[GS_FRAME_1] & 0x1FF) == 0x16B) {
                /* debugging (gsstat): the packets drawn into the cars' sphere map: where they are and whether they change */
                static uint32_t lines;
                if (lines < 48) {
                    uint32_t h = 0, k;
                    for (k = 0; k < 4 + 4 * need; k++)
                        h = h * 31 + q[k];
                    lines++;
                    LOG("gsstat: sphere map packet at %08X: %u qwords, nloop %u nreg %u regs %08X%08X flg %u prim %03X tex0 %05X, "
                        "hash %08X, first %08X %08X %08X %08X | %08X %08X %08X %08X", (unsigned)((const uint8_t *)q - ps2_ram),
                        1 + need, nloop, nreg, (unsigned)(regs >> 32), (unsigned)regs, flg, (q[1] >> 15) & 0x7FF,
                        (unsigned)(g_regs[GS_TEX0_1] & 0x3FFF) << 6, h, q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11]);
                }
            }
            g_gs_path = path;
            if ((q[1] >> 14) & 1)
                gs_write_reg(GS_PRIM, (q[1] >> 15) & 0x7FF);
            g_q = 1.0f;
            if (packet_bulk(q, 0xFFFFFFFFu, 1, nloop, nreg, regs, flg == 1)) {
                p->eop = (q[0] >> 15) & 1;
                p->in_packet = 0;
                return 1 + need;
            }
            /* not for the bulk path: undo the tag's effects, take it the usual way */
            if ((q[1] >> 14) & 1) {
                g_regs[GS_PRIM] = prim0;
                g_vcount = (int)vc;
                g_state_dirty = dirty0;
            }
        }
    }
    gif_path_qword(path, q);
    return 1;
}

void gs_strip_vertex(const uint32_t *q, uint32_t k, GsVertex *o)
{
    const uint32_t *d = q + 4 * k;
    const uint64_t rgbaq = g_regs[GS_RGBAQ];
    const int ctx = (int)((g_regs[GS_PRMODECONT] & 1 ? g_regs[GS_PRIM] : g_regs[GS_PRMODE]) >> 9) & 1;
    const uint64_t ofs = g_regs[GS_XYOFFSET_1 + ctx];
    memset(o, 0, sizeof *o);
    o->x = (int32_t)(d[2] & 0xFFFF) - (int32_t)GS_FIELD(ofs, 0, 16);
    o->y = (int32_t)(d[2] >> 16) - (int32_t)GS_FIELD(ofs, 32, 16);
    o->z = d[3];
    o->s = BITS2F(d[0]);
    o->t = BITS2F(d[1]);
    o->q = BITS2F((uint32_t)(rgbaq >> 32));
    o->r = (uint8_t)rgbaq;
    o->g = (uint8_t)(rgbaq >> 8);
    o->b = (uint8_t)(rgbaq >> 16);
    o->a = (uint8_t)(rgbaq >> 24);
    o->fog = (uint8_t)(g_regs[GS_FOG] >> 56);
}

/* A path is between packets and the last one had EOP: PATH3 masking stops here. */
int gif_path_at_eop(int path)
{
    return !g_path[path].in_packet && g_path[path].eop;
}

/* XGKICK: send the GIF packet at qword qaddr of VU1 data memory down PATH1,
 * tag by tag until one with EOP is done */
uint32_t g_kick_dump_code;      /* debugging ("kickdump CODE N"): log PATH1 packets of this VU1 code */
int g_kick_dump_n;
int g_kick_watch;               /* debugging: the particle systems' packets shown to the backend (gs_backend_puffs_kick) */

void gif_xgkick(const uint32_t *mem, uint32_t mem_qwords, uint32_t qaddr)
{
    GifPath *p = &g_path[1];
    uint32_t n;
    int prev = pt_enter(PT_KICK);
    if (g_kick_dump_n && g_vu1_code == g_kick_dump_code) {
        uint32_t k;
        g_kick_dump_n--;
        LOG("XGKICK code %06X prog %04X at qw %03X:", g_vu1_code, g_vu1_prog, qaddr);
        for (k = 0; k < 12; k++) {
            const uint32_t *q = mem + 4 * ((qaddr + k) & (mem_qwords - 1));
            LOG("  %03X: %08X %08X %08X %08X  (%g %g %g %g)", (qaddr + k) & (mem_qwords - 1), q[0], q[1], q[2], q[3],
                (double)BITS2F(q[0]), (double)BITS2F(q[1]), (double)BITS2F(q[2]), (double)BITS2F(q[3]));
        }
    }
    if (g_kick_watch && (g_vu1_code == 0x0046D750u || g_vu1_code == 0x0047C6F0u))
        gs_backend_puffs_kick(mem, qaddr);
    p->in_packet = 0;
    g_gs_path = 1;
    for (n = 0; n < mem_qwords; n++) {
        const uint32_t *q = mem + 4 * ((qaddr + n) & (mem_qwords - 1));
        if (!p->in_packet && !g_gs_dump && !g_gs_stat) {
            /* a PACKED tag whose registers are all vertex data: the whole
             * packet in one loop, without the per-qword dispatch */
            uint32_t nloop = q[0] & 0x7FFF, flg = (q[1] >> 26) & 3, nreg = q[1] >> 28, k;
            uint64_t regs = q[2] | (uint64_t)q[3] << 32;
            int ok = flg == 0 && nloop && n + 1 + (uint64_t)nloop * (nreg ? nreg : 16) <= mem_qwords;
            if (!nreg)
                nreg = 16;
            for (k = 0; ok && k < nreg; k++) {
                uint32_t r = (uint32_t)(regs >> (4 * k)) & 15;
                ok = (r >= 1 && r <= 5) || r == 0xF;
            }
            if (ok) {
                uint32_t i, a = qaddr + n + 1;
                if ((q[1] >> 14) & 1)
                    gs_write_reg(GS_PRIM, (q[1] >> 15) & 0x7FF);
                g_q = 1.0f;
                if (!packet_bulk(mem, mem_qwords - 1, a, nloop, nreg, regs, 0)) {
                    for (i = 0; i < nloop; i++)
                        for (k = 0; k < nreg; k++, a++) {
                            const uint32_t *d = mem + 4 * (a & (mem_qwords - 1));
                            switch ((uint32_t)(regs >> (4 * k)) & 15) {
                            case 0x1:
                                g_regs[GS_RGBAQ] = ((d[0] & 0xFF) | (d[1] & 0xFF) << 8 | (d[2] & 0xFF) << 16 |
                                                    (d[3] & 0xFF) << 24) | (uint64_t)F2BITS(g_q) << 32;
                                break;
                            case 0x2:
                                g_regs[GS_ST] = d[0] | (uint64_t)d[1] << 32;
                                g_q = BITS2F(d[2]);
                                break;
                            case 0x3:
                                g_regs[GS_UV] = (d[0] & 0x3FFF) | (uint64_t)(d[1] & 0x3FFF) << 16;
                                break;
                            case 0x4:
                                vertex_kick((d[0] & 0xFFFF) | (uint64_t)(d[1] & 0xFFFF) << 16 |
                                            (uint64_t)((d[2] >> 4) & 0xFFFFFF) << 32 | (uint64_t)((d[3] >> 4) & 0xFF) << 56,
                                            1, !((d[3] >> 15) & 1));
                                break;
                            case 0x5:
                                vertex_kick((d[0] & 0xFFFF) | (uint64_t)(d[1] & 0xFFFF) << 16 | (uint64_t)d[2] << 32,
                                            0, !((d[3] >> 15) & 1));
                                break;
                            default:
                                break;
                            }
                        }
                } else {
                    a += nloop * nreg;
                }
                n = a - qaddr - 1;
                p->eop = (q[0] >> 15) & 1;
                if (p->eop) {
                    pt_leave(prev);
                    return;
                }
                continue;
            }
        }
        gif_path_qword(1, q);
        if (!p->in_packet && p->eop) {
            pt_leave(prev);
            return;
        }
    }
    LOG("XGKICK at %04X: no end of packet", qaddr);
    pt_leave(prev);
}

/* ---- lifecycle ------------------------------------------------------------------------- */

void gs_init(void)
{
    if (!gs_mem)
        gs_mem = plat_alloc(GS_MEM_WORDS * 4);
    memset(gs_mem, 0, GS_MEM_WORDS * 4);
    memset(g_regs, 0, sizeof g_regs);
    memset(g_path, 0, sizeof g_path);
    g_regs[GS_PRMODECONT] = 1;
}

void gs_reset(void)
{
    memset(g_regs, 0, sizeof g_regs);
    memset(g_path, 0, sizeof g_path);
    g_regs[GS_PRMODECONT] = 1;
    g_vcount = 0;
    g_state_dirty = 1;
}

void gs_vsync(void)
{
    static int stat_on;
    if (g_gs_stat && !stat_on) {
        extern uint32_t g_vif_deferred[4];
        memset(g_vif_deferred, 0, sizeof g_vif_deferred);
    }
    stat_on = g_gs_stat != 0;
    RTDEP(g_rtdep_field++);
    if (g_gs_lazy)
        ext_age();
    if (g_gs_stat && --g_gs_stat == 0)
        gs_stat_dump();
    if (g_gs_dump && --g_gs_dump == 0 && g_gs_dump_f) {
        fclose(g_gs_dump_f);
        g_gs_dump_f = NULL;
        LOG("gsdump written");
    }
    gs_backend_vsync();
}
