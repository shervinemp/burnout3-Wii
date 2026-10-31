/*
 * Native versions of the game's EE audio DSP loops (config OVERRIDES). They
 * work on 16-bit samples in blocks of 64 (8 quadwords) with MMI code that the
 * recompiler turns into 128-bit register emulation; here the same fixed-point
 * arithmetic runs on plain 32-bit integers. Sample counts that are not a
 * multiple of 64 (never seen) go to the recompiled code.
 */
#include "rt.h"

#include <string.h>

void f_002FBE20(R5900 *c);
void f_002FC820(R5900 *c);
void f_002FC070(R5900 *c);

/* The engine's sound is made where the sound chip is mixed (below): the game's own loop for it then
 * has nothing to do. */
int g_eng_native = 1;           /* autopilot "engnative 0": the game's own loop through the chip's rings */
static uint32_t g_eng_state;    /* the game's state for it (the left channel's; the right one's is 272 on) */
#define ENG_ON (g_eng_native && g_eng_state)

/* low 32 bits of the product, >> 12 (R5900 mult + sra) */
static inline int32_t mul12(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a * (uint32_t)b) >> 12;
}

static inline int32_t clamp16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* 0x2FBE20(state, src, dst): state-variable filter, wave folder, drive and
 * dry/wet mix. state: +0 drive, +12 f, +16 q, +20 band, +24 low (kept),
 * +52 fold gain, +56 level, +60 wet, +64 dry, +68 sample count. */
static void audio_filter(uint32_t st, uint32_t src, uint32_t dst)
{
    uint32_t n = LW(st + 68), k;
    int32_t band, low, f, q, g, wet, dry, drive, level;
    band = (int32_t)LW(st + 20);
    low = (int32_t)LW(st + 24);
    f = (int32_t)LW(st + 12) >> 4;
    q = (int32_t)LW(st + 16) >> 4;
    g = (int32_t)LW(st + 52) >> 4;
    level = (int32_t)LW(st + 56);
    wet = (int32_t)(((int64_t)(int32_t)LW(st + 60) * level) >> 16) >> 4;
    dry = (int32_t)(((int64_t)(int32_t)LW(st + 64) * level) >> 16) >> 4;
    drive = (int32_t)((LW(st) + 0x10000u) << 1) >> 4;
    for (; n; n -= 64, src += 128, dst += 128) {
        const uint32_t *in = (const uint32_t *)GPTR(src);
        uint32_t buf[32], *out;
        for (k = 0; k < 64; k++) {
            uint32_t w = in[k >> 1];
            int32_t x = (int16_t)(k & 1 ? w >> 16 : w), a, m, y;
            low = (int32_t)((uint32_t)low + (uint32_t)mul12(f, band));
            a = (int32_t)((uint32_t)x - (uint32_t)low - (uint32_t)mul12(q, band));
            band = (int32_t)((uint32_t)band + (uint32_t)mul12(a, f));
            a = mul12(low, drive);
            m = (int32_t)(((uint32_t)a + 0x10000u) << 14) >> 31;
            a ^= m;
            a ^= 0x20000 & m;
            a = (int32_t)((uint32_t)a << 14) >> 14;
            a = clamp16(mul12(a, g));
            a = mul12(wet, a);
            y = clamp16((int32_t)((uint32_t)mul12(x, dry) + (uint32_t)a));
            if (k & 1)
                buf[k >> 1] |= (uint32_t)y << 16;
            else
                buf[k >> 1] = (uint16_t)y;
        }
        out = (uint32_t *)GPTR(dst);
        for (k = 0; k < 32; k++)
            out[k] = buf[k];
    }
    SW(st + 20, (uint32_t)band);
    SW(st + 24, (uint32_t)low);
}

/* 0x2FC820(count, left, right, ., gain_l * 2, gain_r * 2): both channels in
 * place become their average, scaled by 32767/32768, times a gain each. */
static void audio_mix(uint32_t n, uint32_t a, uint32_t b, int32_t ga, int32_t gb)
{
    uint32_t k;
    ga = ga / 2;                /* sra after rounding towards zero */
    gb = gb / 2;
    ga = (int16_t)(ga > 32767 ? 32767 : ga);
    gb = (int16_t)(gb > 32767 ? 32767 : gb);
    for (; n; n -= 64, a += 128, b += 128) {
        uint32_t *pa = (uint32_t *)GPTR(a), *pb = (uint32_t *)GPTR(b);
        for (k = 0; k < 32; k++) {
            uint32_t wa = pa[k], wb = pb[k];
            int32_t m0 = ((int32_t)(int16_t)wa + (int16_t)wb) >> 1;
            int32_t m1 = ((int32_t)(int16_t)(wa >> 16) + (int16_t)(wb >> 16)) >> 1;
            m0 = (m0 * 32767) >> 15;
            m1 = (m1 * 32767) >> 15;
            pa[k] = (uint16_t)((m0 * ga) >> 15) | (uint32_t)((m1 * ga) >> 15) << 16;
            pb[k] = (uint16_t)((m0 * gb) >> 15) | (uint32_t)((m1 * gb) >> 15) << 16;
        }
    }
}

/* The first calls also run the recompiled code on the same input and compare
 * (the log reports the first difference). */
#define CHECK_CALLS 24
#define CHECK_MAX 4096                  /* samples */
static uint16_t g_chk_in[2][CHECK_MAX], g_chk_ref[2][CHECK_MAX];

static void save16(uint16_t *d, uint32_t a, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++)
        d[k] = LH(a + 2 * k);
}

static void load16(uint32_t a, const uint16_t *s, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++)
        SH(a + 2 * k, s[k]);
}

static void compare16(const char *what, uint32_t a, const uint16_t *ref, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++)
        if (LH(a + 2 * k) != ref[k]) {
            LOG("audio %s: sample %u of %u is %04X, recompiled code gives %04X", what, k, n, LH(a + 2 * k), ref[k]);
            return;
        }
}

void hle_audio_filter(R5900 *c)
{
    static int checked;
    uint32_t st = c->r[4][0], src = c->r[5][0], dst = c->r[6][0], n = LW(st + 68);
    if (ENG_ON)
        return;
    if (!n || (n & 63)) {
        f_002FBE20(c);
        return;
    }
    if (checked < CHECK_CALLS && n <= CHECK_MAX) {
        uint32_t band = LW(st + 20), low = LW(st + 24), rband, rlow;
        checked++;
        save16(g_chk_in[0], src, n);
        f_002FBE20(c);
        save16(g_chk_ref[0], dst, n);
        rband = LW(st + 20);
        rlow = LW(st + 24);
        load16(src, g_chk_in[0], n);
        SW(st + 20, band);
        SW(st + 24, low);
        audio_filter(st, src, dst);
        compare16("filter", dst, g_chk_ref[0], n);
        if (LW(st + 20) != rband || LW(st + 24) != rlow)
            LOG("audio filter: state %08X %08X, recompiled code gives %08X %08X", LW(st + 20), LW(st + 24), rband, rlow);
        if (checked == CHECK_CALLS)
            LOG("audio filter: %d calls checked", checked);
        return;
    }
    audio_filter(st, src, dst);
}

void hle_audio_mix(R5900 *c)
{
    static int checked;
    uint32_t n = c->r[4][0], a = c->r[5][0], b = c->r[6][0];
    int32_t ga = (int32_t)c->r[8][0], gb = (int32_t)c->r[9][0];
    if (ENG_ON)
        return;
    if (!n || (n & 63)) {
        f_002FC820(c);
        return;
    }
    if (checked < CHECK_CALLS && n <= CHECK_MAX) {
        checked++;
        save16(g_chk_in[0], a, n);
        save16(g_chk_in[1], b, n);
        f_002FC820(c);
        save16(g_chk_ref[0], a, n);
        save16(g_chk_ref[1], b, n);
        load16(a, g_chk_in[0], n);
        load16(b, g_chk_in[1], n);
        audio_mix(n, a, b, ga, gb);
        compare16("mix left", a, g_chk_ref[0], n);
        compare16("mix right", b, g_chk_ref[1], n);
        if (checked == CHECK_CALLS)
            LOG("audio mix: %d calls checked (gains %d %d)", checked, ga, gb);
        return;
    }
    audio_mix(n, a, b, ga, gb);
}

/* ---- the engine's sound ---------------------------------------------------------------------------
 * The game does not let the chip play the engine's voices (core 0's first eight). They are mixed to
 * that core's effect side alone; the sound driver reads that sum back out of the chip's capture
 * rings half a ring (256 samples, 5.3 ms) at a time and sends it here; 0x2FC530 runs a filter over
 * each channel (0x2FC070: by the mode at state + 112 the filter with the wave folder above, or the
 * two-pole filter below), 0x2FC820 makes one sound of the two and pans it, and the driver puts the
 * result into core 1's sound data input, which the chip plays. That loop wants its answer within a
 * few milliseconds, 187 times a second; here a frame's drawing alone keeps everything else waiting
 * for ten. So the mixer (runtime/iop_sd.c) hands core 0's effect sum to hle_engine_dsp and adds what
 * comes back where the input would have played: the same arithmetic on the same numbers, from the
 * game's own state (its settings are read every time, the filters' memories kept where the game
 * keeps them), without the trip and without its 5 to 10 ms of delay. The game's loop still runs,
 * with nothing to work out: the channel routine and the mix return at once. */
static uint32_t g_eng_idle = ~0u;       /* samples since the input was last heard */
static uint32_t g_eng_peak, g_eng_modes, g_eng_run;

int hle_engine_ready(void)
{
    return ENG_ON;
}

static inline int32_t mul16(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 16);
}

/* one channel in place: every other sample of s */
static void engine_channel(uint32_t st, int16_t *s, uint32_t n)
{
    const uint32_t mode = LW(st + 112);
    uint32_t k;
    g_eng_modes |= 1u << (mode < 7 ? mode : 7);
    if (mode == 1) {
        /* as audio_filter */
        int32_t band = (int32_t)LW(st + 20), low = (int32_t)LW(st + 24);
        const int32_t f = (int32_t)LW(st + 12) >> 4, q = (int32_t)LW(st + 16) >> 4, g = (int32_t)LW(st + 52) >> 4;
        const int32_t level = (int32_t)LW(st + 56);
        const int32_t wet = (int32_t)(((int64_t)(int32_t)LW(st + 60) * level) >> 16) >> 4;
        const int32_t dry = (int32_t)(((int64_t)(int32_t)LW(st + 64) * level) >> 16) >> 4;
        const int32_t drive = (int32_t)((LW(st) + 0x10000u) << 1) >> 4;
        for (k = 0; k < n; k++) {
            const int32_t x = s[2 * k];
            int32_t a, m;
            low = (int32_t)((uint32_t)low + (uint32_t)mul12(f, band));
            a = (int32_t)((uint32_t)x - (uint32_t)low - (uint32_t)mul12(q, band));
            band = (int32_t)((uint32_t)band + (uint32_t)mul12(a, f));
            a = mul12(low, drive);
            m = (int32_t)(((uint32_t)a + 0x10000u) << 14) >> 31;
            a ^= m;
            a ^= 0x20000 & m;
            a = (int32_t)((uint32_t)a << 14) >> 14;
            a = clamp16(mul12(a, g));
            a = mul12(wet, a);
            s[2 * k] = (int16_t)clamp16((int32_t)((uint32_t)mul12(x, dry) + (uint32_t)a));
        }
        SW(st + 20, (uint32_t)band);
        SW(st + 24, (uint32_t)low);
    } else if (mode == 2) {
        /* 0x2FB890: y = b0 x + b1 x' + b2 x'' - (a1 y' + a2 y''), the factors with 16 bits of fraction
         * at state + 76.. and + 92.., the two samples before in 16 bits each at + 110, 108 (in) and
         * + 106, 104 (out). (The game's stops its first two samples of a block at -32767 and 32768,
         * which it then stores as -32768; this one stops them all at the 16 bits.) */
        const int32_t b0 = (int32_t)LW(st + 76), b1 = (int32_t)LW(st + 80), b2 = (int32_t)LW(st + 84);
        const int32_t a1 = (int32_t)LW(st + 92), a2 = (int32_t)LW(st + 96);
        int32_t x1 = (int16_t)LH(st + 110), x2 = (int16_t)LH(st + 108);
        int32_t y1 = (int16_t)LH(st + 106), y2 = (int16_t)LH(st + 104);
        for (k = 0; k < n; k++) {
            const int32_t x = s[2 * k];
            const uint32_t in = (uint32_t)mul16(b0, x) + (uint32_t)mul16(b1, x1) + (uint32_t)mul16(b2, x2);
            const uint32_t back = (uint32_t)mul16(a1, y1) + (uint32_t)mul16(a2, y2);
            const int32_t y = clamp16((int32_t)(in - back));
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            s[2 * k] = (int16_t)y;
        }
        SH(st + 110, x1);
        SH(st + 108, x2);
        SH(st + 106, y1);
        SH(st + 104, y2);
    } else {
        /* (0 and 3: the game's routine writes nothing) */
        for (k = 0; k < n; k++)
            s[2 * k] = 0;
    }
}

static void engine_rest(uint32_t st)
{
    SW(st + 20, 0);
    SW(st + 24, 0);
    SW(st + 104, 0);
    SW(st + 108, 0);
}

/* n samples of core 0's effect sum (lr: left and right in turn; `input` 0: all of them zero, not even
 * written) become the engine's sound in place. Returns 0 when there is none to add. */
int hle_engine_dsp(int16_t *lr, uint32_t n, int input)
{
    const uint32_t st = g_eng_state;
    int32_t ga, gb;
    uint32_t k, peak = g_eng_peak;
    if (!ENG_ON)
        return 0;
    if (!input) {
        /* the filters ring out for half a second, then rest */
        if (g_eng_idle >= 24000u)
            return 0;
        g_eng_idle += n;
        if (g_eng_idle >= 24000u) {
            engine_rest(st);
            engine_rest(st + 272);
            return 0;
        }
        memset(lr, 0, n * 4);
    } else {
        g_eng_idle = 0;
    }
    engine_channel(st, lr, n);
    engine_channel(st + 272, lr + 1, n);
    /* as audio_mix */
    ga = (int32_t)LW(st + 6208) / 2;
    gb = (int32_t)LW(st + 6212) / 2;
    ga = (int16_t)(ga > 32767 ? 32767 : ga);
    gb = (int16_t)(gb > 32767 ? 32767 : gb);
    for (k = 0; k < n; k++) {
        const int32_t m = (((int32_t)lr[2 * k] + lr[2 * k + 1]) >> 1) * 32767 >> 15;
        const int32_t a = (int16_t)((m * ga) >> 15), b = (int16_t)((m * gb) >> 15);
        const uint32_t top = (uint32_t)(a < 0 ? -a : a) | (uint32_t)(b < 0 ? -b : b);
        if (top > peak)
            peak = top;
        lr[2 * k] = (int16_t)a;
        lr[2 * k + 1] = (int16_t)b;
    }
    g_eng_peak = peak;
    g_eng_run += n;
    return 1;
}

/* 0x2FC070(state, source, destination): one channel of a block */
void hle_audio_channel(R5900 *c)
{
    const uint32_t st = c->r[4][0];
    if ((c->r[5][0] & 0x0FFFFFFFu) - st == 576u && g_eng_state != st) {
        g_eng_state = st;
        LOG("engine sound: the game's state for it is at %08X; %s", st,
            g_eng_native ? "made where the chip is mixed" : "left to the game's loop through the chip");
    }
    if (ENG_ON)
        return;
    f_002FC070(c);
}

/* status line (once every ten seconds) */
void hle_engine_report(char *line, size_t size)
{
    const uint32_t st = g_eng_state;
    if (!st) {
        snprintf(line, size, "no engine sound state yet");
        return;
    }
    snprintf(line, size, "engine sound %s: %u samples filtered, peak %u, filter modes met %X (now %u %u), gains %d %d, "
             "left f %d q %d drive %d fold %d level %d wet %d dry %d",
             g_eng_native ? "by the mixer" : "by the game's loop", (unsigned)g_eng_run, (unsigned)g_eng_peak,
             (unsigned)g_eng_modes, (unsigned)LW(st + 112), (unsigned)LW(st + 272 + 112), (int)LW(st + 6208),
             (int)LW(st + 6212), (int)LW(st + 12), (int)LW(st + 16), (int)LW(st), (int)LW(st + 52), (int)LW(st + 56),
             (int)LW(st + 60), (int)LW(st + 64));
    g_eng_peak = g_eng_modes = g_eng_run = 0;
}
