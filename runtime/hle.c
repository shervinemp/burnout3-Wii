/* Native replacements for specific guest functions (see config OVERRIDES). */
#include "rt.h"
#include "gs.h"
#include "wtime.h"

#include <stdlib.h>
#include <string.h>

static void set_v0(R5900 *c, int32_t v)
{
    c->r[2][0] = (uint32_t)v;
    c->r[2][1] = (uint32_t)(v >> 31);
}

/* 0x1D3EE0(a0, a1): the game's frame rate mode, a1 = 1 for 30 fps. It sets
 * the vblanks per frame the vblank handler 0x1F1CE8 waits for (0x1F4290)
 * and the step of the game clock (0x1D3D30: 1/fps a frame; the clock
 * counts frames). Menus ask for 30 fps, races for 60: a race drawn at 30 fps
 * ran at half speed. g_fps: 0 = the game's choice, 30 or 60 = forced
 * (autopilot "fps"). hle_game_tick below keeps the game's speed right at
 * whatever rate the frames really come. */
int g_fps;
void f_001D3EE0(R5900 *c);


/* 0x228040(waiting, free): the callback of the DMA queue's frame kick
 * (0x1F2408), called in a loop while the last frame has not been flipped yet
 * (waiting = 1), once more when it has. The loop only spins: wait for the
 * next interrupt here instead, so the time shows as idle (what is left of a
 * frame) and not as game code. */
void tl_add(int what, uint32_t arg);

static uint32_t g_frame_kicks;          /* frame kicks so far: a frame is built between two (hle_env_clear_entry) */

void hle_flip_wait_entry(R5900 *c)
{
    static int waiting;
    extern void *g_long_wait_who;
    g_long_wait_who = c->thread;
    if (c->r[4][0]) {
        if (!waiting)
            tl_add('W', 0);             /* the frame kick starts waiting for the last frame's queue */
        waiting = 1;
        kernel_spin_wait();
    } else {
        tl_add('K', (uint32_t)waiting); /* the frame kick goes ahead (1: after a wait) */
        waiting = 0;
        g_frame_kicks++;
    }
}

/* ---- frame pacing ---------------------------------------------------------------- */

/* The game's speed. The race loop (0x132600, state 5) runs N passes of its
 * update, each 1/60 s of game time and a tick of the game clock (0x1D3D20,
 * counted at 0x4E273C), before it draws once; N for the next frame comes
 * from its timer (0x376570, the object at 0x51BA50) and is kept at 0x51BAC4.
 * The timer knows how many passes real time asks for (COP0 Count; its word
 * +40) but hands that out only in its mode 1. What the game runs with:
 *   - ordinary racing: mode 0, one pass a frame whatever the frame took. On
 *     the PS2 that is real time at 60 frames a second and slow motion below.
 *     Here, at 30, it was half speed for the whole race but the crashes;
 *   - a crash: the game's own 30 fps flag (0x51BAA6: flip interval 2, two
 *     passes a frame at least). Right at 30, double speed when the pacer
 *     below showed those frames for one vblank, two thirds at 20;
 *   - for some seconds after a crash: mode 1, passes by the clock, sampled
 *     wherever the loop happens to be: 1, 2 and 3 passes in no order for
 *     frames that were each on screen for two vblanks.
 * So the speed changed at every crash and with every step of the pacer.
 * Now the passes are counted here (pace_passes): a frame gets as many as
 * the vblanks the frame before it is on screen, which is the pacer's
 * interval, put right afterwards by what the screen really did. The same
 * in all three cases; it is what the PS2 does when it holds its rate.
 * (Earlier states of this port forced the game's 30 fps mode, and tried to
 * stretch the clock by ticks and by the step: both fought the game's own
 * updates. This leaves ticks and step alone and only says how many passes.)
 *
 * What is left to do here is the flip interval in the 60 fps mode: the
 * game's vblank handler (0x1F1CE8) flips as soon as a frame is ready and
 * PACE_INTERVAL vblanks have passed, which with frames of about 33 ms of
 * work shows them for 2 and 3 vblanks in no order. The pacer picks the
 * interval, 1, 2 or 3: up when half the frames miss it, down when the work
 * of a frame would fit the shorter one with a tenth to spare. Called once a
 * frame of a race (hle_env_clear_entry). g_pace 0 (autopilot "pace 0"):
 * the game's own interval. */
#ifdef GEKKO
int g_pace = 1;
#else
int g_pace;
#endif
int g_pace_interval;            /* vblanks a frame now (for the status line) */
uint32_t g_pace_steps[5];       /* frames that took 1, 2, 3, 4 or more vblanks ([0] unused) */
/* the backend's count of frames shown and the vblanks the last one took (the
 * Wii's flip): what the screen did. The pacer's own count, between two
 * calls in the middle of a frame's build, says 1 and 3 for two frames of 2
 * whenever the call falls near a vblank. */
uint32_t g_flip_count, g_flip_vblanks, g_flip_vb_total;
int g_pace_passes = 1;          /* the update passes of a frame counted here (autopilot "passes 0": the game's timer) */
extern int g_dbg;

#define PACE_STEP 0x004E2740u       /* seconds a tick of the game clock */
#define PACE_INTERVAL 0x004E17DCu   /* vblanks the vblank handler waits before a flip */
#define PACE_OWED 0x0051BAC4u       /* update passes the race loop runs before its next frame */
#define VBLANK_USEC 16683u

/* debugging: what happened when in the last frames (a slow frame's line in
 * the status log is followed by these): V a vblank, C and c the frame's VIF1
 * chain starting and done, R the frame's render build starting, M the frame
 * rate mode asked for (once a frame), I an idle wait of a millisecond or
 * more (its length in 0.1 ms) */
struct TlEvent {
    uint32_t t;
    uint8_t what;
    uint16_t arg;
} g_tl[64];
uint32_t g_tl_n;
uint64_t plat_time_us(void);

void tl_add(int what, uint32_t arg)
{
    struct TlEvent *e = &g_tl[g_tl_n++ & 63];
    e->t = (uint32_t)plat_time_us();
    e->what = (uint8_t)what;
    e->arg = (uint16_t)arg;
}

void hle_frame_rate(R5900 *c)
{
    tl_add('M', 0);
    if (g_fps == 30 || g_fps == 60) {
        c->r[5][0] = g_fps == 30;
        c->r[5][1] = 0;
    }
    f_001D3EE0(c);
}

/* half of the last 16 frames missed their interval (the pacer steps up then) */
static int misses_soon(uint32_t hist)
{
    uint32_t k, n = 0;
    for (k = 0; k < 16; k++)
        n += hist >> k & 1;
    return n >= 8;
}

/* The passes for the next frame: the interval it is planned for, and what
 * the frames shown since the last call took more or less than planned
 * (nflip of them, dvb vblanks together). bal is vblanks shown or planned
 * less passes made; a frame that was a vblank late is made up for by the
 * next one built. reset: pacing starts over (loading, another mode). */
static void pace_passes(uint32_t interval, uint32_t nflip, uint32_t dvb, int reset)
{
    static uint8_t plan[8];
    static uint32_t head, tail;
    static int32_t bal;
    int32_t n;
    uint32_t k;
    if (reset) {
        head = tail = 0;
        bal = 0;
        return;
    }
    for (k = 0; k < nflip; k++)
        bal -= head != tail ? (int32_t)plan[tail++ & 7] : (int32_t)interval;
    bal += (int32_t)dvb;
    bal = bal < -1 ? -1 : bal > 4 ? 4 : bal;
    n = (int32_t)interval + bal;
    n = n < 1 ? 1 : n > 4 ? 4 : n;
    bal += (int32_t)interval - n;
    plan[head++ & 7] = (uint8_t)interval;
    if (head - tail > 8)
        tail = head - 8;
    if (g_pace_passes)
        SW(PACE_OWED, (uint32_t)n);
}

/* Frames whose work would fit the shorter interval before it is tried. It
 * was 45, and 240 after a try that failed: a crash's heaviest two seconds
 * put the interval up, and the game then stayed at 20 frames a second for
 * four more (twelve after a failed try) with the work long back to normal.
 * Now 16, three times as many after each failed try. */
#define PACE_NEED 16

static void pace_frame(void)
{
    static uint32_t last_vb, fast, hist, interval, need = PACE_NEED, since_down = 1000, roomy;
    /* a frame at the next shorter interval has an update pass less: its work
     * is `gain` / 256 of this one's, learned from each step down (the average
     * work before it against the average after) */
    static uint32_t avg, before, gain = 225, last_flip, last_total, leak;
    static uint64_t last_idle;
    uint32_t vb = kernel_vblank_count(), el = vb - last_vb, work, k, misses = 0, shown;
    const uint32_t nflip = g_flip_count - last_flip, dvb = g_flip_vb_total - last_total;
    uint64_t idle = g_kernel_idle_us - last_idle;
    last_vb = vb;
    last_idle = g_kernel_idle_us;
    g_pace_interval = 0;
    shown = g_flip_count != last_flip ? g_flip_vblanks : el;
    last_flip = g_flip_count;
    last_total = g_flip_vb_total;
    if (!g_pace || el > 30 || LWF(PACE_STEP) > 0.025f) {
        fast = hist = 0;                /* off, loading, or the 30 fps mode */
        pace_passes(0, 0, 0, 1);
        return;
    }
    if (interval < 1 || interval > 3)
        interval = 2;
    if (el == 0) {
        /* two frames' builds within one vblank (frames of a vblank each, the
         * calls a little apart): no work to measure, the interval stays */
        if (nflip)
            hist = hist << 1 | (shown > interval);
        g_pace_interval = (int)interval;
        SB(PACE_INTERVAL, interval);
        pace_passes(interval, nflip < 8 ? nflip : 0, nflip < 8 ? dvb : 0, 0);
        return;
    }
    g_pace_steps[el < 4 ? el : 4]++;
    work = el * VBLANK_USEC > idle ? (uint32_t)(el * VBLANK_USEC - idle) : 0;
    hist = hist << 1 | (shown > interval);
    /* misses over a longer time: each counts 64, a 64th of the sum goes every frame */
    leak = leak * 63 / 64 + (shown > interval ? 64u : 0u);
    avg = avg ? (avg * 7 + work) / 8 : work;
    if (before && (since_down == 24 || (misses_soon(hist) && since_down < 24))) {
        uint32_t g = avg * 256 / before;
        g = g < 180 ? 180 : g > 256 ? 256 : g;
        gain = (gain * 3 + g) / 4;
        before = 0;
    }
    /* The shorter interval when this frame's work, less the pass, fits it with
     * a tenth to spare. (With a twentieth, 15.5 ms of work went to one vblank
     * a frame and every eighth frame missed it: neither 60 nor 30.) */
    if (interval > 1 && work * gain / 256 < (interval - 1) * (VBLANK_USEC * 9 / 10))
        fast++;
    else
        fast = fast > 8 ? fast - 8 : 0; /* one long frame does not start the count over */
    /* With a fifth of the shorter interval to spare it is tried after 20
     * frames, whatever the last try did: after a crash the game stayed at 20
     * frames a second for ten seconds and more, half of each frame idle,
     * because a step down that failed inside the crash made the next one
     * wait 240 frames. */
    if (interval > 1 && work * gain / 256 < (interval - 1) * (VBLANK_USEC * 4 / 5))
        roomy++;
    else
        roomy = 0;
    if (roomy >= 10 && fast < need)
        fast = need;
    for (k = 0; k < 16; k++)
        misses += hist >> k & 1;
    if (since_down < 1000)
        since_down++;
    if (since_down == 300)
        need = PACE_NEED;               /* the shorter interval held */
    /* Up when half the frames miss the interval; from 1 already at three of 16:
     * between 17 and 25 ms of work fewer than half the frames missed, and the
     * picture stayed a mix of frames of one and two vblanks for as long as
     * the work stayed there (the start grid: two seconds of it). And from 1
     * when a frame in 35 or more misses for a while (`leak`): with 17.0 ms of
     * work each frame is ready a little later against the vblank than the
     * last until one misses it, about twice a second, and three in 16 never
     * come together. */
    if ((misses >= (interval == 1 ? 3u : 8u) || (interval == 1 && leak >= 150)) && interval < 3) {
        interval++;
        hist = fast = 0;
        roomy = 0;
        leak = 0;
        if (since_down < 300)
            need = need * 3 > 240 ? 240 : need * 3;     /* it did not: wait longer before the next try */
    } else if (fast >= need) {
        interval--;
        hist = fast = 0;
        roomy = 0;
        leak = 0;
        since_down = 0;
        before = avg;
    }
    SB(PACE_INTERVAL, interval);
    g_pace_interval = (int)interval;
    pace_passes(interval, nflip < 8 ? nflip : 0, nflip < 8 ? dvb : 0, 0);
}

/* ---- loading: the step taken again within the frame ----------------------------- */

/* The main loop (0x132600) loads a race in its state 4 by calling 0x19A950 on
 * the world (0x522660) once a frame: each call takes the load (its phase, 0 to
 * 24, at world + 0x143844) as far as the parts it waits for are ready, then
 * returns 0 (again next frame) or 1 (the race may start). States 7 and 9 do
 * the same with 0x1322B0 (the front end's screen loads, the long one at
 * boot). A phase goes one step a frame, so most of a load is waiting for the
 * next frame, not for the disc. Here a step that returned 0 is taken again
 * within the frame: before each, the main thread naps a little (kernel_nap: a
 * native wait of its own), so that the IOP's threads, the disc reader and the
 * game's other threads run and their replies are delivered, as they are
 * between two frames. That while the frame has had less than g_load_step ms
 * since the first step and the next vblank is more than LOAD_MARGIN_US away.
 * Vblanks come at the console's pace: the loading screen and anything else
 * counted in frames are as before. (The idea is the burnout3-pc port's, its
 * FINDINGS.md "Loading a race".) g_load_step 0 (autopilot "loadstep 0"): one
 * step a frame, as the game (and the PC host's default: the reference).
 * Not the game's own sleep (0x237050: SetAlarm and SleepThread) between the
 * steps: that took a wakeup the main thread waits for elsewhere, and the game
 * called address 0 right after the race's load.
 * Measured in Dolphin (the scripted race, 2026-10-19): 6.31-6.35 s with one
 * step a frame (the step itself 0.27 s of it, the processor idle 65-71%), 3.35
 * to 3.45 s with 8 ms (4 ms: 3.74, 12 ms: 3.26); every loading frame still on
 * screen for one vblank, and the race's first 48 frames draw the world the same
 * ("wstat 3"). What is left is paced by something else: phase 4 (0x289EE0 on
 * 0x1E77540) alone takes 1.4 s. The game clock (0x4E273C) counts the loading
 * frames, so the race starts at a smaller tick: work/fb.sh keeps "loadstep 0".
 * The front end's load at boot gains little (2.98 to 2.51 s), so g_load_fe is
 * off. */
#ifdef GEKKO
int g_load_step;                /* 0: one step a frame, the game's; autopilot "loadstep 8" takes them again */
#else
int g_load_step;
#endif
int g_load_sleep = 500;         /* us each nap between two steps (autopilot "loadnap") */
int g_load_fe;                  /* the front end's loads too (autopilot "loadfe 1"): fe_screen_step */

#define LOAD_MARGIN_US 2000u
#define LOAD_PHASE_OFF 0x143844u

void f_0019A950(R5900 *c);
void f_001322B0(R5900 *c);
void kernel_nap(uint64_t until);            /* kernel.c */
void kernel_log_threads(const char *why);

/* what a load did, logged when it ends: [0] the race's, [1] the front end's */
static struct LoadStat {
    int on;
    uint32_t calls, steps, naps, vb0, last_vb, flips0, seen, max_call, max_step, state, nheavy, early;
    uint32_t gap[5];            /* calls 0, 1, 2, 3, 4 or more vblanks after the one before (a frame a call) */
    uint64_t t0, idle0, busy, slept;
    uint16_t phase_ms[25];      /* when the load first stood at each phase (ms from its start) */
    struct { uint8_t from, to; uint16_t at, us100; } heavy[12];    /* steps of 3 ms or more: phases, start in the frame */
} g_load[2];

/* time left for another step: under g_load_step ms since the frame's first, no vblank since, the next not near */
static int load_time_left(uint64_t entry, uint32_t vb)
{
    const uint64_t now = plat_time_us();
    return now - entry < (uint64_t)g_load_step * 1000u && kernel_vblank_count() == vb &&
           kernel_next_vblank_us() > now + LOAD_MARGIN_US;
}

static void load_step(R5900 *c, void (*step)(R5900 *), void (*again)(R5900 *), int kind)
{
    struct LoadStat *s = &g_load[kind];
    uint32_t save[32][4], k, ph, n;
    const uint64_t entry = plat_time_us();
    const uint32_t vb = kernel_vblank_count();
    uint64_t now, t;
    memcpy(save, c->r, sizeof save);
    if (!s->on) {
        memset(s, 0, sizeof *s);
        s->on = 1;
        s->t0 = entry;
        s->idle0 = g_kernel_idle_us;
        s->vb0 = s->last_vb = vb;
        s->flips0 = g_flip_count;
        s->state = LW(0x0051BAD0u);     /* the main loop's state */
        if (kind == 0)
            kernel_log_threads("at a race's load");
    } else {
        s->gap[vb - s->last_vb < 4 ? vb - s->last_vb : 4]++;
        s->last_vb = vb;
    }
    s->calls++;
    if ((s->calls % 60) == 1)
        LOG("load %s: call %u, state %u, phase %u, %u steps, longest %u.%u ms",
            kind == 0 ? "race" : "front end", s->calls, s->state,
            kind == 0 && save[4][0] ? LW(save[4][0] + LOAD_PHASE_OFF) : 0,
            s->steps, s->max_step / 1000, s->max_step / 100 % 10);
    for (n = 0;; n++) {
        ph = kind == 0 ? LW(save[4][0] + LOAD_PHASE_OFF) : 0;
        if (ph < 25 && !(s->seen >> ph & 1)) {
            s->seen |= 1u << ph;
            s->phase_ms[ph] = (uint16_t)((plat_time_us() - s->t0) / 1000);
        }
        t = plat_time_us();
        (n ? again : step)(c);
        now = plat_time_us();
        s->busy += now - t;
        s->max_step = now - t > s->max_step ? (uint32_t)(now - t) : s->max_step;
        s->steps++;
        if (now - t >= 3000 && s->nheavy < 12) {
            s->heavy[s->nheavy].from = (uint8_t)ph;
            s->heavy[s->nheavy].to = (uint8_t)(kind == 0 ? LW(save[4][0] + LOAD_PHASE_OFF) : 0);
            s->heavy[s->nheavy].at = (uint16_t)((t - entry) / 100);
            s->heavy[s->nheavy++].us100 = (uint16_t)((now - t) / 100);
        }
        if (n && kind == 1 && c->r[2][0]) {
            /* the front end's screen is loaded: 0x1322B0 finishes next frame (its sound update stays once a frame) */
            c->r[2][0] = c->r[2][1] = 0;
            s->early++;
            break;
        }
        if (c->r[2][0] || !g_load_step || (kind && !g_load_fe) || !kern_can_wait() || !load_time_left(entry, vb))
            break;
        /* the others' turn: a nap, not past the frame's time for steps */
        {
            uint64_t until = now + (uint64_t)g_load_sleep, end = entry + (uint64_t)g_load_step * 1000u;
            const uint64_t vbt = kernel_next_vblank_us() - LOAD_MARGIN_US;
            until = until < end ? until : end;
            until = until < vbt ? until : vbt;
            kernel_nap(until);
        }
        s->slept += plat_time_us() - now;
        s->naps++;
        if (!load_time_left(entry, vb))
            break;                      /* the step is taken again next frame (v0 stays the last step's 0) */
        memcpy(c->r, save, sizeof save);
    }
    now = plat_time_us();
    s->max_call = now - entry > s->max_call ? (uint32_t)(now - entry) : s->max_call;
    if (c->r[2][0]) {
        /* done: what the load took */
        static const char *const names[2] = {"race (0x19A950)", "front end (0x1322B0)"};
        const uint64_t dt = now - s->t0;
        char line[200];
        size_t m = 0;
        for (k = 0; k < 25 && m < sizeof line - 12; k++)
            if (s->seen >> k & 1)
                m += (size_t)snprintf(line + m, sizeof line - m, " %u:%u", k, s->phase_ms[k]);
        line[m] = 0;
        LOG("load %s, state %u: %u.%02u s, %u vblanks, %u calls, %u steps (longest %u.%u ms, a call %u.%u ms); in the step %u ms, "
            "%u naps between steps %u ms, idle %u ms; frames shown %u, calls 1/2/3/4+ vblanks apart %u/%u/%u/%u; loadstep %d%s",
            names[kind], s->state, (unsigned)(dt / 1000000), (unsigned)(dt / 10000 % 100), kernel_vblank_count() - s->vb0, s->calls,
            s->steps, s->max_step / 1000, s->max_step / 100 % 10, s->max_call / 1000, s->max_call / 100 % 10,
            (unsigned)(s->busy / 1000), s->naps, (unsigned)(s->slept / 1000), (unsigned)((g_kernel_idle_us - s->idle0) / 1000),
            g_flip_count - s->flips0, s->gap[1], s->gap[2], s->gap[3], s->gap[4], g_load_step,
            kind == 0 ? "" : g_load_fe ? (s->early ? " loadfe 1 (the screen ready within a frame)" : " loadfe 1") : " loadfe 0");
        if (kind == 0)
            LOG("  phases (ms from the start):%s", line);
        for (m = 0, k = 0; k < s->nheavy && m < sizeof line - 32; k++)
            m += (size_t)snprintf(line + m, sizeof line - m, " %u>%u %u.%u ms at %u.%u", s->heavy[k].from, s->heavy[k].to,
                                  s->heavy[k].us100 / 10, s->heavy[k].us100 % 10, s->heavy[k].at / 10, s->heavy[k].at % 10);
        line[m] = 0;
        if (m)
            LOG("  steps of 3 ms and more (phases, ms, start in the frame's call):%s", line);
        s->on = 0;
    }
}

void hle_race_load(R5900 *c)
{
    load_step(c, f_0019A950, f_0019A950, 0);
}

/* The front end's screen's own load step, the one 0x1322B0(main) waits for: the screen [main + 0x30000 - 9656],
 * the table at its +432, entry 3, called on the screen. Taken again without the rest of 0x1322B0, whose sound
 * update (0x28B160: every sound object's update) stays once a frame. */
static void fe_screen_step(R5900 *c)
{
    const uint32_t screen = LW(c->r[4][0] + 0x30000u - 9656u);
    const uint32_t fn = screen ? LW(LW(screen + 432u) + 12u) : 0;
    const PS2Func f = fn ? rt_lookup(fn) : NULL;
    if (!f) {
        c->r[2][0] = c->r[2][1] = 0;
        return;
    }
    c->r[4][0] = screen;
    c->r[4][1] = 0;
    f(c);
}

void hle_fe_load(R5900 *c)
{
    load_step(c, f_001322B0, fe_screen_step, 1);
}

/* experiments (autopilot "dbg N") */
int g_dbg;

/* ---- textures: placed, not sent ------------------------------------------------ */

/* 0x221E80(state, texture, base): the game sends a texture it has just given
 * a place in its GS memory cache: the entries of the texture's upload script
 * ([texture + 36]: a count, then 64 bytes an entry from +16) copied into the
 * chain, each a DMA tag with VIF DIRECT, a GIF tag, BITBLTBUF (plus base) and
 * a DMA ref to the transfer's packet in the asset. In a race nearly every
 * texture is placed again every frame (the cache is smaller than a frame's
 * textures): about 220 rasters, 2.6 MB, through the GS's upload machinery.
 *
 * Here the copy of a raster's entry becomes two notes for the GS (gs.h: the
 * entry, the TBP), in the same four qwords: nothing is transferred, the
 * backend takes the texture from the asset when a draw asks for that TBP
 * (gs.c, "textures the game placed"). The rest of the function (the tail of
 * the chain, the texture's counts) runs as it is. An 8 bit texture's packet
 * goes on with its palette, a second transfer into the same buffer: there
 * the entry stays, its DMA ref moved past the raster, and one note goes
 * where the function ends each texture with a write to the GS's NOP
 * register. Other entries stay as they
 * are (palettes of their own). g_tex_place 0 (autopilot "texplace 0"):
 * everything sent.
 *
 * Mip levels (g_tex_place 2, the default; 1: sent as before). Of a race
 * frame's 500 transfers about 355 were mip levels, which nothing here reads
 * (GX textures have one level, the rasteriser reads level 0): each was
 * copied, hashed, marked and often written into GS memory all the same. A
 * texture's record (texture + [gp-24088]) has its palette's block at +16 and
 * MIPTBP1/2 at +24..+36, relative to its place while it is being sent, and a
 * transfer's block is the entry's DBP plus that of TRXPOS in the buffer
 * (checked on a race's textures with "texplace 3": an 8-bit texture's packet
 * is raster, mips and palette into one buffer, or raster alone with a second
 * entry of mips ending in the palette; a 4-bit one's is raster and mips with
 * the palette, 8x3, as an entry of its own). tex_rest sorts a packet's
 * transfers by that; an entry of mips alone becomes two NOP writes, one of
 * mips and the palette refers to the palette alone, anything else stays. */
uint32_t g_tex_mips;            /* mip transfers left out (status line) */

/* the transfers of an entry's packet from p, left qwords: 1 when each is a mip
 * level or the palette (*pal, *palq: where and how long, 0 none), 0 to leave
 * the entry as it is */
static int tex_rest(uint32_t rec, uint32_t se, uint32_t p, uint32_t left, uint32_t *pal, uint32_t *palq, uint32_t *nmips)
{
    const uint32_t bbhi = LW(se + 36), dbw = (bbhi >> 16) & 0x3F, dpsm = (bbhi >> 24) & 0x3F, off = bbhi & 0x3FFF;
    const uint32_t clut = LW(rec + 16) & 0x3FFF, m1 = LW(rec + 24), m1h = LW(rec + 28), m2 = LW(rec + 32), m2h = LW(rec + 36);
    const uint32_t tbp[6] = {m1 & 0x3FFF, (m1 >> 20 | m1h << 12) & 0x3FFF, (m1h >> 8) & 0x3FFF,
                             m2 & 0x3FFF, (m2 >> 20 | m2h << 12) & 0x3FFF, (m2h >> 8) & 0x3FFF};
    *pal = *palq = *nmips = 0;
    if (dpsm != 0 && dpsm != 2)         /* CT32 or CT16, as the rasters go */
        return 0;
    while (left) {
        uint32_t need, blk, k;
        if (left < 6 || p < 0x4000 || p > PS2_RAM_SIZE - 96 || (p & 15) || LW(p) != 3 || LW(p + 4) != 0x10000000u || LW(p + 8) != 0xE ||
            LW(p + 24) != 0x51 || LW(p + 40) != 0x52 || LW(p + 48) || LW(p + 56) != 0x53 || ((LW(p + 68) >> 26) & 3) != 2)
            return 0;
        need = LW(p + 64) & 0x7FFF;
        if (!need || 5 + need > left || (LW(p + 64) & 0x8000) || p + (5 + need) * 16 > PS2_RAM_SIZE)
            return 0;
        blk = off + gs_block_of(dpsm, 0, dbw ? dbw : 1, LW(p + 20) & 0x7FF, (LW(p + 20) >> 16) & 0x7FF);
        for (k = 0; k < 6 && !(tbp[k] && tbp[k] == blk); k++)
            ;
        if (clut && blk == clut && !*pal) {
            *pal = p;
            *palq = 5 + need;
        } else if (k < 6) {
            (*nmips)++;
        } else {
            return 0;
        }
        left -= 5 + need;
        p += (5 + need) * 16;
    }
    return 1;
}

#ifdef GEKKO
int g_tex_place = 2;
#else
int g_tex_place;                /* the PC reference draws from GS memory */
#endif
uint32_t g_tex_sent;            /* script entries left as transfers (status line) */

void f_00221E80(R5900 *c);

void hle_tex_send(R5900 *c)
{
    extern int g_native_render;
    const uint32_t tex = c->r[5][0], base = c->r[6][0], gp = c->r[28][0];
    const uint32_t at = LW(gp - 24236u), script = LW(tex + 36), n = LW(script), roff = LW(gp - 24088u);
    uint32_t qw = 0, end;
    int kind, mips, sent = 0;
    {
        WT_MARK(wm);
        f_00221E80(c);
        WT_DONE(texsend, wm);
    }
    if (n < 1 || n > 16)
        return;
    if (g_tex_place == 3) {
        /* looking: a texture's record (CLUT block, MIPTBP) against where its transfers go */
        static int logged;
        if (logged < 40) {
            const uint32_t rec = tex + LW(gp - 24088u);
            uint32_t k;
            logged++;
            LOG("texlook %08X: %u entries, record +%u: clut %08X miptbp %08X %08X %08X %08X", tex, n, LW(gp - 24088u), LW(rec + 16),
                LW(rec + 24), LW(rec + 28), LW(rec + 32), LW(rec + 36));
            for (k = 0; k < n; k++) {
                const uint32_t se = script + 16 + 64 * k, bbhi = LW(se + 36);
                uint32_t pk = LW(se + 52) & 0x7FFFFFFFu, left = LW(se + 48) & 0xFFFF, t = 0;
                while (left >= 6 && t < 10 && pk >= 0x4000 && pk < PS2_RAM_SIZE - 96 && LW(pk) == 3 && LW(pk + 24) == 0x51) {
                    const uint32_t need = LW(pk + 64) & 0x7FFF, dbw = (bbhi >> 16) & 0x3F, dpsm = (bbhi >> 24) & 0x3F;
                    const uint32_t dx = LW(pk + 20) & 0x7FF, dy = (LW(pk + 20) >> 16) & 0x7FF;
                    LOG("  entry %u: dbp +%X bw %u psm %02X: %ux%u at %u,%u, %u qwords%s, block +%X", k, bbhi & 0x3FFF, dbw, dpsm,
                        LW(pk + 32) & 0xFFF, LW(pk + 36) & 0xFFF, dx, dy, need, (LW(pk + 64) >> 15) & 1 ? " EOP" : "",
                        (bbhi & 0x3FFF) + gs_block_of(dpsm, 0, dbw ? dbw : 1, dx, dy));
                    if (5 + need > left)
                        break;
                    left -= 5 + need;
                    pk += (5 + need) * 16;
                    t++;
                }
                if (left)
                    LOG("  entry %u: %u qwords left unparsed", k, left);
            }
        }
    }
    kind = g_tex_place && g_native_render && !(tex & 3) && LW(at) == 0x10000002u && LW(at + 12) == 0x50000002u
               ? gs_placed_entry_ok(script + 16, &qw) : 0;
    end = LW(gp - 24236u);
    mips = g_tex_place == 2 && kind && end - at == n * 64 + 48 && roff >= 16 && roff < 256 && !(roff & 3);
    if (mips) {
        /* the entries after the first: mips left out, the palette kept */
        uint32_t k;
        for (k = 1; k < n; k++) {
            const uint32_t ce = at + 64 * k, qwc = LW(ce + 48) & 0xFFFF;
            uint32_t pal, palq, nm;
            if (LW(ce) != 0x10000002u || LW(ce + 12) != 0x50000002u || ((LW(ce + 48) >> 28) & 7) != 3 ||
                LW(ce + 60) != (0x50000000u | qwc) ||
                !tex_rest(tex + roff, script + 16 + 64 * k, LW(ce + 52) & 0x7FFFFFFFu, qwc, &pal, &palq, &nm) || !nm)
                continue;
            g_tex_mips += nm;
            if (pal) {
                SW(ce + 48, (LW(ce + 48) & 0xFFFF0000u) | palq);
                SW(ce + 52, (LW(ce + 52) & 0x80000000u) | pal);
                SW(ce + 60, 0x50000000u | palq);
            } else {
                SW(ce, 0x10000003u);            /* DMA cnt, three qwords: the GIF tag and two NOP writes */
                SW(ce + 12, 0x50000003u);
                SW(ce + 16, 2);
                SW(ce + 32, 0);
                SW(ce + 36, 0);
                SW(ce + 40, 0x7F);
                SW(ce + 44, 0);
                SW(ce + 48, 0);
                SW(ce + 52, 0);
                SW(ce + 56, 0x7F);
                SW(ce + 60, 0);
                g_tex_sent--;                   /* counted below with the rest */
            }
        }
    }
    if (kind == 2 && end - at == n * 64 + 48 && LW(end - 8) == 0x7F && !LW(end - 16) && !LW(end - 12) &&
        (LW(at + 48) & 0xFFFF) > qw && LW(at + 60) == (0x50000000u | (LW(at + 48) & 0xFFFF))) {
        uint32_t rest = (LW(at + 48) & 0xFFFF) - qw, from = LW(at + 52) + qw * 16, pal, palq, nm;
        if (mips && tex_rest(tex + roff, script + 16, from & 0x7FFFFFFFu, rest, &pal, &palq, &nm) && nm) {
            g_tex_mips += nm;
            if (pal) {
                from = (from & 0x80000000u) | pal;      /* the palette alone */
                rest = palq;
            } else {
                kind = 1;                               /* mips alone after the raster: nothing is sent */
            }
        }
        if (kind == 2) {
            SW(at + 48, (LW(at + 48) & 0xFFFF0000u) | rest);    /* the DMA ref: what follows the raster */
            SW(at + 52, from);
            SW(at + 60, 0x50000000u | rest);                    /* VIF DIRECT */
            SW(end - 16, GS_NOTE_TEX_HERE | tex >> 2);
            SW(end - 12, (GS_NOTE_MAGIC & 0xFFFFC000u) | ((base >> 6) & 0x3FFF));
            g_tex_sent += n;
            sent = 1;
        }
    }
    if (sent) {
        ;
    } else if (kind == 1) {
        SW(at, 0x10000003u);                    /* DMA cnt, three qwords */
        SW(at + 12, 0x50000003u);               /* VIF DIRECT, three */
        SW(at + 16, 2);                         /* GIF tag: two registers' writes */
        SW(at + 32, GS_NOTE_TEX_ENTRY | tex >> 2);
        SW(at + 36, GS_NOTE_MAGIC);
        SW(at + 40, 0x7F);
        SW(at + 44, 0);
        SW(at + 48, GS_NOTE_TEX_AT | ((base >> 6) & 0x3FFF));
        SW(at + 52, GS_NOTE_MAGIC);
        SW(at + 56, 0x7F);
        SW(at + 60, 0);
        g_tex_sent += n - 1;
    } else {
        g_tex_sent += n;
    }
}

/* ---- a stopped car stays put --------------------------------------------------- */

/* Below 0.2 m/s the game scales a car's velocity and angular velocity by 0.9
 * an update (the end of 0x1C38F0). The velocity gets to zero that way; the
 * rotation about the car's up axis does not: the tyre friction of the same
 * function (four steps an update, full friction against any slip however
 * small) and the suspension's small constant torque on a cambered road push
 * it again each update, harder the nearer it is to zero. A stopped car turned
 * to and fro by a tenth of a degree, and the chase camera 5 m behind it swung
 * the picture with it.
 *
 * Here a stopped car gets no torque about its up axis: after the last of the
 * car's own forces (the suspension, 0x1CC900(car, dt)) and before the body's
 * integrator (0x1AC470), that part is taken out of the torque (+256) and
 * angular impulse (+288) sums, the angular momentum (+224) and velocity
 * (+208). The inertia tensor has the up axis as a principal axis, so the
 * rest of the rotation is as it was. Stopped: slower than REST_SPEED (a tenth
 * of the game's own limit), turning slower than REST_TURN, and what the
 * game's own rule asks (+529, +5320 clear). g_rest_hold 0 (autopilot
 * "resthold 0"): as the game. */
int g_rest_hold = 1;
uint32_t g_rest_held;           /* updates a car was held (for the status line) */

#define REST_SPEED 0.02f        /* m/s */
#define REST_TURN 0.02f         /* rad/s */

/* The car shadow's strength (f13 of f_00303A40, the strip's alpha being f13 x 90 x f12: min(1, 3.33 x the first term of
 * the road's baked light at the car), see f_00303170) falls to nothing where the baked light is flat: in shade (under
 * the trees and bridges of Silver Lake, the approach to its tunnel), where the game draws no shadow at all. A floor under
 * it keeps a faint contact shadow there, as the ambient occlusion a car has in shade too; in the sun the strength is 1
 * and the floor changes nothing. Percent of the full strength (autopilot "shadowfloor N", args.txt "--shadowfloor N");
 * 0 draws the game's own. The PC host is the reference of the game's own picture: 0 there. */
#ifdef GEKKO
int g_shadow_floor = 35;
#else
int g_shadow_floor = 0;
#endif

void f_00303A40(R5900 *c);

void hle_shadow_alpha(R5900 *c)
{
    const float lo = (float)g_shadow_floor * 0.01f;
    if (g_shadow_floor > 0 && c->f[13] < lo)
        c->f[13] = lo;
    f_00303A40(c);
}

void f_001CC900(R5900 *c);

/* debugging ("carlog 1"): the player's car after each update while it is in
 * the box of the first fence of Silver Lake (the same line on the PC host
 * and on the Wii: where do the two part) */
#ifdef GEKKO
int g_carlog;
#else
int g_carlog = 1;
#endif

void hle_car_forces(R5900 *c)
{
    static const uint16_t k_vec[4] = {208, 224, 256, 288};
    uint32_t car = c->r[4][0], m;
    float up[3], d;
    int k, j;
    f_001CC900(c);
    if (g_carlog && car == 0x0057BAF0u) {
        static uint32_t lines;
        const uint32_t mt = LW(car + 516);
        const float x = LWF(mt + 48), z = LWF(mt + 56);
        if (x > 4490.0f && x < 4545.0f && z > 1465.0f && z < 1500.0f && lines < 400) {
            lines++;
            LOG("carlog %u: pos %.3f %.3f %.3f v %.3f %.3f %.3f (%.3f) up %.3f %.3f %.3f fwd %.3f %.3f %.3f w %.3f %.3f %.3f",
                lines, (double)x, (double)LWF(mt + 52), (double)z, (double)LWF(car + 176), (double)LWF(car + 180),
                (double)LWF(car + 184), (double)LWF(car + 188), (double)LWF(mt + 16), (double)LWF(mt + 20),
                (double)LWF(mt + 24), (double)LWF(mt + 32), (double)LWF(mt + 36), (double)LWF(mt + 40),
                (double)LWF(car + 208), (double)LWF(car + 212), (double)LWF(car + 216));
        }
    }
    if (!g_rest_hold || LWF(car + 188) >= REST_SPEED || LB(car + 529) || LW(car + 5320))
        return;
    m = LW(car + 516);          /* the body's matrix: row 1 is its up axis */
    for (j = 0; j < 3; j++)
        up[j] = LWF(m + 16 + 4 * (uint32_t)j);
    d = LWF(car + 208) * up[0] + LWF(car + 212) * up[1] + LWF(car + 216) * up[2];
    if (d >= REST_TURN || d <= -REST_TURN)
        return;
    for (k = 0; k < 4; k++) {
        uint32_t a = car + k_vec[k];
        d = LWF(a) * up[0] + LWF(a + 4) * up[1] + LWF(a + 8) * up[2];
        for (j = 0; j < 3; j++)
            SWF(a + 4 * (uint32_t)j, LWF(a + 4 * (uint32_t)j) - up[j] * d);
    }
    g_rest_held++;
}

/* ---- reflection views in turn ------------------------------------------------------ */

/* The cars' reflections come from three (or five) small views of the world
 * around the player's car, drawn anew every frame and then folded into one
 * sphere map: about half of a race frame's draws for a 64-pixel picture.
 * Here one view in three is drawn a frame; the others keep their picture
 * (the backend holds those targets: gs_backend_hold) and the game's render
 * functions leave them out:
 *   0x26AF50  world batch (hle_world_batch): the view is the batch group
 *             state's byte 153;
 *   0x26A710  adds a view to an instanced object's list (hle_inst_view);
 *   0x2BC140, 0x2BC070  the sky and the horizon of the view at 0x667050.
 * 0x222F70(renderer, kind) is the clear of the views; the in-game frame
 * render (0x276300) calls it with kind 3 right after their cameras are set
 * up (0x223130; that also sets the view count at 0x667054): the frame's
 * choice is made at its entry, and told to the backend by notes in the
 * chain there (gs.h), since the chain is drawn later, possibly while the
 * next frame is being built. A view the backend could not hold (no picture
 * yet) is drawn in full the frame after. Views are 560 bytes each from
 * 0x666330; +448 is their FRAME register. g_env_rot 0 (autopilot "envrot
 * 0"): all, every frame.
 * Split-screen calls the clear once for each player, a frame's second call
 * (since the frame kick) being player 2's, and both players' views are drawn
 * into the same three targets: each player's views go in turn on their own,
 * and VIEW notes tell the backend whose they are now (it keeps each player's
 * pictures), from the first frame that had a second call on. */
#ifdef GEKKO
int g_env_rot = 1;
/* One reflection view in three. Four was measured to change the race's draw
 * count by under 1% (the held views already cost nothing), so it is an option
 * ("--reflect 4/6") rather than a default. */
int g_env_period = 3;
#else
int g_env_rot;
int g_env_period = 3;
#endif
uint32_t g_env_skip;            /* bit v: view v is left out of the frame being built */

#define ENV_COUNT 0x00667054u
#define ENV_CUR 0x00667050u
#define ENV_VIEW0 0x00666330u

void f_001F3080(R5900 *c);

/* notes for the backend into the chain: a GIF packet (0x1F3080(0x80000000,
 * qwords) makes room at the write pointer, gp - 24240, as the game's own
 * code does for its register writes) of one A+D tag and the notes as writes
 * of the NOP register */
static void chain_notes(R5900 *c, const uint32_t *notes, uint32_t n)
{
    uint32_t save[32][4], p, k, gp = c->r[28][0];
    memcpy(save, c->r, sizeof save);
    c->r[4][0] = 0x80000000u;
    c->r[4][1] = 0xFFFFFFFFu;
    c->r[5][0] = n + 1;
    c->r[5][1] = 0;
    f_001F3080(c);
    memcpy(c->r, save, sizeof save);
    p = LW(gp - 24240u);
    SW(p + 0, 0x8000u | n);             /* NLOOP n, EOP */
    SW(p + 4, 0x10000000u);             /* packed, one register: */
    SW(p + 8, 0xE);                     /* A+D */
    SW(p + 12, 0);
    for (k = 0; k < n; k++) {
        SW(p + 16 + 16 * k, notes[k]);
        SW(p + 20 + 16 * k, GS_NOTE_MAGIC);
        SW(p + 24 + 16 * k, 0x7F);
        SW(p + 28 + 16 * k, 0);
    }
    SW(gp - 24240u, p + 16 * (n + 1));
}

void hle_env_clear_entry(R5900 *c)
{
    extern int g_native_render;
    static uint32_t frame[2], kicks, calls, split;
    uint32_t count = LW(ENV_COUNT), v, n = 0, notes[16], skip = 0, missed, p;
    if (g_dbg & 1)
        *(uint32_t *)GPTR(ENV_COUNT) = 1;
    if (g_dbg & 2)
        *(uint8_t *)GPTR(0x00665F20u ^ PS2_BXOR) = 0;   /* world sections in the reflection views */
    if (c->r[5][0] != 3)
        return;
    /* the player: the calls since the frame's kick */
    calls = kicks == g_frame_kicks ? calls + 1 : 0;
    kicks = g_frame_kicks;
    p = calls ? 1 : 0;
    if (p)
        split = 1;
    tl_add('R', 0);
    pace_frame();
    if (!g_native_render)
        return;
    frame[p]++;
    missed = g_gs_hold_missed >> (8 * p) & 0xFF;
    g_gs_hold_missed &= ~(0xFFu << (8 * p));
    notes[n++] = GS_NOTE_HOLD_BEGIN | p << 24;
    if (g_env_rot && count >= 4 && count <= 6)
        for (v = 1; v < count; v++) {
            const uint32_t fbp = (LW(ENV_VIEW0 + 560 * v + 448) & 0x1FF) * 32;
            if (split)
                notes[n++] = GS_NOTE_VIEW | p << 24 | fbp;
            if ((v + frame[p]) % (uint32_t)(g_env_period > 0 ? g_env_period : 3) && !(missed >> v & 1)) {
                notes[n++] = GS_NOTE_HOLD | p << 24 | v << 20 | fbp;
                skip |= 1u << v;
            }
        }
    g_env_skip = skip;
    chain_notes(c, notes, n);
}

void f_0026A710(R5900 *c);
void f_002BC140(R5900 *c);
void f_002BC070(R5900 *c);

/* 0x26A710(state, view): one more view for the instanced object being set up */
void hle_inst_view(R5900 *c)
{
    if (g_env_skip >> (c->r[5][0] & 31) & 1) {
        set_v0(c, 1);
        return;
    }
    {
        WT_MARK(wm);
        f_0026A710(c);
        WT_DONE(instview, wm);
    }
}

void hle_env_sky(R5900 *c)
{
    if (g_env_skip >> (LW(ENV_CUR) & 31) & 1)
        return;
    f_002BC140(c);
}

void hle_env_horizon(R5900 *c)
{
    if (g_env_skip >> (LW(ENV_CUR) & 31) & 1)
        return;
    f_002BC070(c);
}

/* int write(int fd, const void *buf, int size) for the debug TTY */
void hle_tty_write(R5900 *c)
{
    static char line[512];
    static int len;
    uint32_t buf = c->r[5][0], size = c->r[6][0], k;
    for (k = 0; k < size; k++) {
        char ch = (char)LB(buf + k);
        if (ch == '\n' || len == (int)sizeof line - 1) {
            line[len] = 0;
            LOG("tty: %s", line);
            len = 0;
            if (ch == '\n')
                continue;
        }
        if (ch != '\r')
            line[len++] = ch;
    }
    set_v0(c, (int32_t)size);
}

/* DEBUG_WRAP: log calls to selected functions (arguments may be stale when the
 * callee does not read them, since callers sync only what callees use) */
static uint32_t g_dbg_sp[256];
static int g_dbg_depth;

void ps2_dbg_enter(R5900 *c, uint32_t addr)
{
    if (g_dbg_depth < 256)
        g_dbg_sp[g_dbg_depth] = c->r[29][0];
    g_dbg_depth++;
    LOG("-> %08X(%08X:%08X %08X:%08X %08X %08X | %08X %08X %08X %08X | f12 %g f13 %g) ra %08X sp %08X", addr,
        c->r[4][1], c->r[4][0], c->r[5][1], c->r[5][0], c->r[6][0], c->r[7][0], c->r[8][0], c->r[9][0],
        c->r[10][0], c->r[11][0], (double)c->f[12], (double)c->f[13], c->r[31][0], c->r[29][0]);
}

void ps2_dbg_leave(R5900 *c, uint32_t addr)
{
    g_dbg_depth--;
    LOG("<- %08X = %08X:%08X f0 %g", addr, c->r[2][1], c->r[2][0], (double)c->f[0]);
    if (g_dbg_depth < 256 && c->r[29][0] != g_dbg_sp[g_dbg_depth])
        LOG("   !! sp %08X at return, %08X at entry", c->r[29][0], g_dbg_sp[g_dbg_depth]);
}

/* libmc2 write worker (0x246788): clear the file position slot it reads before
 * initializing (see ENTRY_HOOKS in config/burnout3.py). */
void hle_mc2_write_entry(R5900 *c)
{
    g_w32(c->r[29][0] - 768 + 680, 0);
}

uint64_t g_pt[PT_COUNT];
uint32_t g_pt_n;
int g_pt_on;                    /* autopilot "ptstat" */
int g_pt_cur;
uint64_t g_pt_last;

void pt_report(char *out, size_t size)
{
    static const char *const names[PT_COUNT] = {"EE", "VU0", "VU1", "VIF", "GIF", "upload", "sync", "prim", "flush", "tex",
                                                "gx", "idle", "kick", "native"};
    uint64_t total = 0;
    size_t n = 0;
    int k;
    if (!g_pt_on) {
        snprintf(out, size, "(time split off: autopilot \"ptstat 1\") ");
        return;
    }
    pt_leave(pt_enter(g_pt_cur));       /* bring the current category up to date */
    for (k = 0; k < PT_COUNT; k++)
        total += g_pt[k];
    out[0] = 0;
    for (k = 0; k < PT_COUNT && n < size; k++) {
        n += (size_t)snprintf(out + n, size - n, "%s %.0f%% ", names[k], 100.0 * (double)g_pt[k] / (double)(total ? total : 1));
        g_pt[k] = 0;
    }
}

#if defined(PS2_FPU_CHECK)
#include <math.h>
/* Float results that are Inf/NaN on the host: reported once per guest pc. With
 * "fclamp on" they are also saturated the way the EE FPU and VUs saturate. */
int g_fclamp;
static struct { uint32_t pc, n; } g_fbad[1024];
static int g_fbad_n;

/* the value watchpoint also reports where a watched value is computed
 * (returns 1 the first time for a pc) */
static int fwatch_note(uint32_t bits, uint32_t pc, const char *what)
{
    extern uint32_t g_watch_val, g_watch_span;
    static uint32_t seen[256];
    static int n;
    int k;
    if (bits - g_watch_val >= g_watch_span)
        return 0;
    for (k = 0; k < n && seen[k] != pc; k++)
        ;
    if (k < n || n == 256)
        return 0;
    seen[n++] = pc;
    LOG("watch: %s computes %08X (%g) at %08X", what, bits, (double)BITS2F(bits), pc);
    return 1;
}

int ps2_vu_watch(int unit, uint32_t pc, float v)
{
    return fwatch_note(F2BITS(v), 0xF0000000u | (uint32_t)unit << 16 | pc, unit ? "VU1" : "VU0");
}

float ps2_fchk(float f, uint32_t pc)
{
    fwatch_note(F2BITS(f), pc, "EE");
    if (PS2_LIKELY(isfinite(f)))
        return f;
    int k = 0;
    while (k < g_fbad_n && g_fbad[k].pc != pc)
        k++;
    if (k == g_fbad_n && k < 1024) {
        g_fbad[g_fbad_n++].pc = pc;
        if (g_fbad_n <= 200)
            LOG("FPU: %s at %08X (vblank %u)", isnan(f) ? "NaN" : "Inf", pc, kernel_vblank_count());
    }
    if (k < 1024)
        g_fbad[k].n++;
    if (g_fclamp) {
        uint32_t b = F2BITS(f);
        return BITS2F((b & 0x80000000u) | 0x7F7FFFFFu);
    }
    return f;
}
#endif

#if defined(PS2_PROFILE)
#include <stdio.h>
#include <stdlib.h>
/* "prof": guest instructions executed per function since the last report,
 * written to work/prof.txt, then the counters restart. */
typedef struct { uint64_t n; uint32_t idx; } ProfRow;

static int prof_cmp(const void *a, const void *b)
{
    uint64_t x = ((const ProfRow *)a)->n, y = ((const ProfRow *)b)->n;
    return x < y ? 1 : x > y ? -1 : 0;
}

void ps2_prof_dump(const char *path)
{
    static uint32_t last_vblank;
    void vu_prof_dump(FILE *f);
    uint32_t vb = kernel_vblank_count(), frames = vb - last_vblank;
    ProfRow *rows = malloc(sizeof(ProfRow) * ps2_func_count);
    uint64_t total = 0;
    unsigned n = 0;
    for (unsigned k = 0; k < ps2_func_count; k++) {
        if (g_prof[k]) {
            rows[n].n = g_prof[k];
            rows[n++].idx = k;
            total += g_prof[k];
        }
    }
    qsort(rows, n, sizeof(ProfRow), prof_cmp);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "vblanks %u  EE guest instructions %llu  per vblank %llu  functions %u\n", frames,
                (unsigned long long)total, (unsigned long long)(frames ? total / frames : 0), n);
        uint64_t cum = 0;
        for (unsigned k = 0; k < n && k < 400; k++) {
            cum += rows[k].n;
            fprintf(f, "%08X %12llu %10llu/vb %5.2f%% cum %5.1f%%\n", ps2_func_table[rows[k].idx].addr,
                    (unsigned long long)rows[k].n, (unsigned long long)(frames ? rows[k].n / frames : 0),
                    100.0 * rows[k].n / (total ? total : 1), 100.0 * cum / (total ? total : 1));
        }
        fprintf(f, "\n");
        vu_prof_dump(f);
        fclose(f);
    }
    free(rows);
    memset(g_prof, 0, sizeof(uint64_t) * ps2_func_count);
    last_vblank = vb;
    LOG("profile: %u vblanks, %llu EE instructions -> %s", frames, (unsigned long long)total, path);
}
#endif

/* ---- qsort (0x125338) ------------------------------------------------------------------------ */

/* The game's C library qsort(base, n, size, compare), the BSD one: insertion
 * sort under 7 elements, median of three (of nine over 40), a partition that
 * falls back to insertion sort when it swapped nothing. Every comparison is
 * an indirect guest call; the per-frame sort of the update (0x1B8FFC: 8-byte
 * records by their float key, comparator 0x1B9820: -1 when a < b, else 1)
 * was a millisecond of a race frame. Those sorts run here, the same
 * algorithm on the same memory, so equal keys end up in the same order; the
 * first ones also run the game's code and compare. */
void f_00125338(R5900 *c);

typedef struct { float k; uint32_t v; } QsRec;

#define QS_CMP(a, b) ((a)->k < (b)->k ? -1 : 1)
#define QS_SWAP(a, b) do { QsRec t_ = *(a); *(a) = *(b); *(b) = t_; } while (0)

static inline QsRec *qs_med3(QsRec *a, QsRec *b, QsRec *c)
{
    return QS_CMP(a, b) < 0 ? (QS_CMP(b, c) < 0 ? b : (QS_CMP(a, c) < 0 ? c : a))
                            : (QS_CMP(b, c) > 0 ? b : (QS_CMP(a, c) < 0 ? a : c));
}

static void qs_insertion(QsRec *a, uint32_t n)
{
    QsRec *pm, *pl;
    for (pm = a + 1; pm < a + n; pm++)
        for (pl = pm; pl > a && QS_CMP(pl - 1, pl) > 0; pl--)
            QS_SWAP(pl, pl - 1);
}

static void qs_float(QsRec *a, uint32_t n)
{
    for (;;) {
        QsRec *pa, *pb, *pc, *pd, *pl, *pm, *pn;
        int swapped = 0;
        int32_t r, k;
        if (n < 7) {
            qs_insertion(a, n);
            return;
        }
        pm = a + n / 2;
        if (n > 7) {
            pl = a;
            pn = a + (n - 1);
            if (n > 40) {
                uint32_t d = n / 8;
                pl = qs_med3(pl, pl + d, pl + 2 * d);
                pm = qs_med3(pm - d, pm, pm + d);
                pn = qs_med3(pn - 2 * d, pn - d, pn);
            }
            pm = qs_med3(pl, pm, pn);
        }
        QS_SWAP(a, pm);
        pa = pb = a + 1;
        pc = pd = a + (n - 1);
        /* the comparator never says "equal": pa and pd stay where they are */
        for (;;) {
            while (pb <= pc && QS_CMP(pb, a) <= 0)
                pb++;
            while (pb <= pc && QS_CMP(pc, a) >= 0)
                pc--;
            if (pb > pc)
                break;
            QS_SWAP(pb, pc);
            swapped = 1;
            pb++;
            pc--;
        }
        if (!swapped) {
            qs_insertion(a, n);
            return;
        }
        pn = a + n;
        r = (int32_t)(pa - a) < (int32_t)(pb - pa) ? (int32_t)(pa - a) : (int32_t)(pb - pa);
        for (k = 0; k < r; k++)
            QS_SWAP(a + k, pb - r + k);
        r = (int32_t)(pd - pc) < (int32_t)(pn - pd - 1) ? (int32_t)(pd - pc) : (int32_t)(pn - pd - 1);
        for (k = 0; k < r; k++)
            QS_SWAP(pb + k, pn - r + k);
        if ((r = (int32_t)(pb - pa)) > 1)
            qs_float(a, (uint32_t)r);
        if ((r = (int32_t)(pd - pc)) <= 1)
            return;
        a = pn - r;
        n = (uint32_t)r;
    }
}

#define QS_CHECK_CALLS 40
#define QS_CHECK_MAX 4096

void hle_qsort(R5900 *c)
{
    uint32_t base = c->r[4][0], n = c->r[5][0], size = c->r[6][0], cmp = c->r[7][0];
    static int checked, bad, inside;
    static QsRec *copy;
    /* inside: the game's code sorting a partition of a sort being checked */
    if (inside || bad || cmp != 0x001B9820u || size != 8 || (base & 7) || n > 0x100000 ||
        (base & PS2_RAM_MASK) + n * 8 > PS2_RAM_MASK + 1) {
        f_00125338(c);
        return;
    }
    if (checked < QS_CHECK_CALLS && n >= 2 && n <= QS_CHECK_MAX && (copy || (copy = malloc(QS_CHECK_MAX * sizeof *copy)))) {
        memcpy(copy, GPTR(base), n * 8);
        inside = 1;
        f_00125338(c);
        inside = 0;
        qs_float(copy, n);
        checked++;
        if (memcmp(copy, GPTR(base), n * 8))
            bad++;
        if (bad || checked == QS_CHECK_CALLS) {
            LOG("qsort: %d sorts by float key checked against the game's (%u records in the last), %d differ", checked, n,
                bad);
            if (checked == QS_CHECK_CALLS) {
                free(copy);
                copy = NULL;
            }
        }
        return;                         /* a difference: the game's code from now on (bad) */
    }
    qs_float((QsRec *)GPTR(base), n);
}
