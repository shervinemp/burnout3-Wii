/*
 * Wii platform layer (libogc).
 *
 * Memory: the 32 MB of PS2 RAM sits at a fixed address in MEM2
 * (PS2_RAM_FIXED_BASE) so recompiled code addresses it with an immediate
 * base. Other large buffers (IOP RAM, sound RAM, GS memory, thread stacks)
 * come from MEM2 too; MEM1 is left to the recompiled code and libogc.
 *
 * Files: the game disc is an ISO image on SD or USB (FAT32 holds it: the
 * Burnout 3 image is 2.9 GB). Saves go next to it.
 */
#include "plat.h"
#include "rt.h"
#include "iop.h"
#include "dsp_mix.h"

#include <gccore.h>
#include <fat.h>
#include <ntfs.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <wiiuse/wpad.h>

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef PS2_RAM_FIXED_BASE
#error "the Wii build places PS2 RAM at PS2_RAM_FIXED_BASE"
#endif

/* Host threads are cooperative contexts on the one LWP thread (see below). */
typedef struct Green {
    void *sp;                       /* saved by green_switch */
    uint32_t *canary;               /* bottom of the stack: must keep the fill pattern */
    int stack_id;                   /* index in g_stacks */
    void (*fn)(void *);
    void *arg;
    struct Green *next;             /* ready queue or semaphore wait queue */
} Green;
struct PlatSem { int count; Green *head, *tail; };
struct PlatThread { Green g; };
/* Several handles on one file: the game streams music while it loads data,
 * and FatFs seeks backwards by walking the cluster chain from the start of
 * the file (a 4 GB image: ~100k clusters); each read goes to the handle that
 * is at or a little before it, so seeks are short hops forward or none. */
#define PF_HANDLES 4
struct PlatFile { FILE *f[PF_HANDLES]; int64_t pos[PF_HANDLES]; uint32_t used[PF_HANDLES]; int n; int64_t size; int cached; };

static const char *const k_roots[] = {"sd:/burnout3", "usb:/burnout3", "sd:/apps/burnout3", "usb:/apps/burnout3"};
static char g_ntfs_roots[8][64];       /* "<mount>:/burnout3" and "<mount>:/apps/burnout3" */
static int g_ntfs_nroots;
static ntfs_md *g_ntfs_mounts;
static int g_ntfs_count;
static char g_root[64];
static char g_save_dir[96];
static void plat_quit(int code);
void plat_pad_rumble(int port, unsigned strength);
static FILE *g_log;
static mutex_t g_log_lock;
static int g_sd_mounted;        /* the card's volume is on the SD interface with the lock (sd_remount) */
static int sd_remount(void);
static void raw_setup(PlatFile *f, const char *path);
static uint64_t g_t0;
static void *g_xfb;
static GXRModeObj *g_rmode;
static int g_console_lines;
static int g_video_taken;

/* ---- MEM2 ---------------------------------------------------------------------------------- */

static uint8_t *mem2_alloc(size_t size)
{
    uint8_t *lo = (uint8_t *)(((uintptr_t)SYS_GetArena2Lo() + 63) & ~(uintptr_t)63);
    if (lo + size > (uint8_t *)SYS_GetArena2Hi())
        return NULL;
    SYS_SetArena2Lo(lo + size);
    return lo;
}

static void reserve_guest_ram(void)
{
    uint8_t *lo = SYS_GetArena2Lo(), *hi = SYS_GetArena2Hi();
    uint8_t *base = (uint8_t *)PS2_RAM_FIXED_BASE;
    if (base < lo || base + PS2_RAM_SIZE > hi)
        plat_fatal("PS2 RAM at %p does not fit MEM2 arena %p-%p", base, lo, hi);
    /* MEM2 above the guest RAM stays available to mem2_alloc */
    SYS_SetArena2Lo(base + PS2_RAM_SIZE);
    memset(base, 0, PS2_RAM_SIZE);
    DCFlushRange(base, PS2_RAM_SIZE);
}

/* ---- console and logging ------------------------------------------------------------------ */

static void video_init(void)
{
    VIDEO_Init();
    g_rmode = VIDEO_GetPreferredMode(NULL);
    plat_log("video: frame buffer %dx%d, the display %d samples from %d, as the library has it", g_rmode->fbWidth, g_rmode->xfbHeight, g_rmode->viWidth, g_rmode->viXOrigin);
    /* libogc's 640-sample picture leaves the line's 704 active samples unfilled (black margins, and a 16:9 picture
     * 9% too narrow); the video interface scales the 640 columns to the whole line */
    g_rmode->viWidth = 704;
    g_rmode->viXOrigin = (VI_MAX_WIDTH_NTSC - 704) / 2;
    g_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(g_rmode));
    console_init(g_xfb, 20, 20, g_rmode->fbWidth, g_rmode->xfbHeight, g_rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(g_rmode);
    VIDEO_SetNextFramebuffer(g_xfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (g_rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
}

/* The GX renderer takes over the display: three frame buffers of its own
 * (one shown, one queued for the next retrace, one being copied to). The
 * console's frame buffer stays for fatal errors. */
void plat_video_takeover(GXRModeObj **mode, void **xfb0, void **xfb1, void **xfb2)
{
    /* with spare lines: a scaled display copy can write one more than asked */
    u32 size = VIDEO_GetFrameBufferSize(g_rmode) + 16 * 640 * 2;
    void *a = memalign(32, size), *b = memalign(32, size), *c = memalign(32, size);
    if (!a || !b || !c)
        plat_fatal("no memory for the frame buffers");
    /* The lines below the picture (640x448 in a 480-line frame, as on the
     * PS2) are never written by the display copy: they keep what is here.
     * The display buffer is YUV 4:2:2, and a zero there is not black but
     * green (Y 0, U 0, V 0), so the black of the format goes in: Y 0, U 128,
     * V 128, a pair of pixels in four bytes. */
    {
        void *p[3];
        u32 k, j;
        p[0] = a;
        p[1] = b;
        p[2] = c;
        for (k = 0; k < 3; k++)
            for (j = 0; j + 3 < size; j += 4) {
                ((u8 *)p[k])[j] = 0;
                ((u8 *)p[k])[j + 1] = 128;
                ((u8 *)p[k])[j + 2] = 0;
                ((u8 *)p[k])[j + 3] = 128;
            }
    }
    DCFlushRange(a, size);
    DCFlushRange(b, size);
    DCFlushRange(c, size);
    DCInvalidateRange(a, size);
    DCInvalidateRange(b, size);
    DCInvalidateRange(c, size);
    *mode = g_rmode;
    *xfb0 = MEM_K0_TO_K1(a);
    *xfb1 = MEM_K0_TO_K1(b);
    *xfb2 = MEM_K0_TO_K1(c);
    g_console_lines = 400;              /* no more log lines on screen */
    g_video_taken = 1;
}

/* Log lines go into a queue and a thread below the game's writes them out (log_main): the UART
 * takes its time for every character and the file an SD write now and then, and a status block of
 * thirty lines written in place cost the frame it fell in a vblank. log_drain writes what is queued
 * at once (a fatal error, the end); the watchdog shows the queue on the UART (log_show_queue). */
#define LOGQ_SIZE (64u * 1024)
static char g_logq[LOGQ_SIZE];
static volatile uint32_t g_logq_head, g_logq_tail;      /* characters in, characters out */
static lwp_t g_log_thread = LWP_THREAD_NULL;
static uint8_t g_log_stack[16 * 1024] __attribute__((aligned(32)));

static void log_drain(void)
{
    static uint64_t last_flush, last_sync;
    char line[520];
    uint64_t us;
    LWP_MutexLock(g_log_lock);
    for (;;) {
        uint32_t t = g_logq_tail, h = g_logq_head, n = 0;
        if (t == h)
            break;
        while (t + n != h && n < sizeof line - 1) {
            char ch = g_logq[(t + n) % LOGQ_SIZE];
            line[n++] = ch;
            if (ch == '\n')
                break;
        }
        line[n] = 0;
        g_logq_tail = t + n;
#if defined(WII_UART_LOG)
        /* also to Dolphin's OSReport UART (Logs/dolphin.log): survives hangs */
        SYS_Report("%s", line);
#endif
        if (g_log)
            fputs(line, g_log);
    }
    us = ticks_to_microsecs(gettime() - g_t0);
    if (g_log && us - last_flush > 1000000) {       /* keep the log readable after a hang */
        last_flush = us;
        fflush(g_log);
        if (us - last_sync > 10000000) {
            /* the file's length in its directory entry too: the FAT library writes it at a close or a sync
             * only, and a console switched off after a hang would otherwise leave an empty log */
            last_sync = us;
            fsync(fileno(g_log));
        }
    }
    LWP_MutexUnlock(g_log_lock);
}

/* everything logged so far onto the card, length included (the benchmark's result, the profile's end) */
void plat_log_sync(void)
{
    log_drain();
    LWP_MutexLock(g_log_lock);
    if (g_log) {
        fflush(g_log);
        fsync(fileno(g_log));
    }
    LWP_MutexUnlock(g_log_lock);
}

static void *log_main(void *arg)
{
    (void)arg;
    for (;;) {
        if (g_logq_head != g_logq_tail)
            log_drain();
        usleep(10000);
    }
    return NULL;
}

/* from the watchdog's interrupt: what the hang kept from being written */
static void log_show_queue(void)
{
#if defined(WII_UART_LOG)
    char line[200];
    uint32_t t = g_logq_tail, h = g_logq_head, n = 0;
    for (; t != h; t++) {
        line[n++] = g_logq[t % LOGQ_SIZE];
        if (line[n - 1] == '\n' || n == sizeof line - 1) {
            line[n] = 0;
            SYS_Report("%s", line);
            n = 0;
        }
    }
#endif
}

int plat_log_pending(void)
{
    return g_logq_head != g_logq_tail;
}

void plat_vlog(const char *fmt, va_list ap)
{
    char line[512];
    int n, len;
    uint32_t level, k;
    uint64_t us = ticks_to_microsecs(gettime() - g_t0);
    n = snprintf(line, sizeof line, "[%9.3f] ", (double)us / 1e6);
    len = n + vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
    if (len > (int)sizeof line - 2)
        len = (int)sizeof line - 2;
    /* the on-screen console is for early boot and fatal errors only */
    if (g_console_lines < 400) {
        printf("%s\n", line + n);
        g_console_lines++;
    }
    line[len++] = '\n';
    for (;;) {
        int fits;
        level = IRQ_Disable();
        fits = LOGQ_SIZE - (g_logq_head - g_logq_tail) >= (uint32_t)len;
        if (fits) {
            uint32_t h = g_logq_head;
            for (k = 0; k < (uint32_t)len; k++)
                g_logq[(h + k) % LOGQ_SIZE] = line[k];
            g_logq_head = h + (uint32_t)len;
        }
        IRQ_Restore(level);
        if (fits)
            break;
        log_drain();
    }
    if (g_log_thread == LWP_THREAD_NULL)
        log_drain();
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    plat_vlog(fmt, ap);
    va_end(ap);
}

void plat_fatal(const char *fmt, ...)
{
    va_list ap;
    if (g_video_taken) {                /* back to the console */
        VIDEO_SetNextFramebuffer(g_xfb);
        VIDEO_Flush();
    }
    va_start(ap, fmt);
    printf("\n*** FATAL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    va_start(ap, fmt);
    plat_vlog(fmt, ap);
    va_end(ap);
    rt_dump_trace();
    log_drain();
    LWP_MutexLock(g_log_lock);
    if (g_log)
        fclose(g_log);
    g_log = NULL;
    LWP_MutexUnlock(g_log_lock);
    printf("Press HOME or RESET to return to the loader.\n");
    for (;;) {
        WPAD_ScanPads();
        if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME || SYS_ResetButtonDown())
            exit(1);
        VIDEO_WaitVSync();
    }
}

/* ---- init ---------------------------------------------------------------------------------- */

/* ---- sound ---------------------------------------------------------------------------------------
 * What the sound chip's model mixes (runtime/iop_sd.c) waits in a queue; the audio interface plays
 * blocks of 512 samples (10.7 ms at 48 kHz) from two buffers in turn, and its interrupt at the start
 * of each fills the other one from the queue (silence when the game has not made enough). A sample
 * of the interface is the right channel's word, then the left's. */
#define AQ_FRAMES 8192u
#define AQ_BLOCK 512u
static int16_t g_aq[AQ_FRAMES][2];
static volatile uint32_t g_aq_in, g_aq_out;
static int16_t g_aq_dma[2][AQ_BLOCK][2] __attribute__((aligned(32)));
static int g_aq_cur, g_aq_on;
int g_aq_swap;                  /* autopilot "aswap 1": left first (to see which is which) */
int g_aq_tone;                  /* autopilot "atone 1": a 220 Hz square on the left channel alone */
static uint32_t g_aq_under, g_aq_blocks, g_aq_blocks_all;
static void aq_dry_note(void);

static void aq_next(void)
{
    int16_t (*d)[2] = g_aq_dma[g_aq_cur ^= 1];
    const uint32_t have = g_aq_in - g_aq_out, at = g_aq_out;
    const int first = g_aq_swap ? 0 : 1;
    uint32_t k;
    if (have >= AQ_BLOCK) {
        for (k = 0; k < AQ_BLOCK; k++) {
            const int16_t *s = g_aq[(at + k) % AQ_FRAMES];
            d[k][0] = s[first];
            d[k][1] = s[first ^ 1];
        }
        g_aq_out = at + AQ_BLOCK;
    } else {
        memset(d, 0, sizeof g_aq_dma[0]);
        g_aq_under++;
        aq_dry_note();
    }
    g_aq_blocks++;
    g_aq_blocks_all++;
    DCFlushRange(d, sizeof g_aq_dma[0]);
    AUDIO_InitDMA((u32)d, sizeof g_aq_dma[0]);
}

static void audio_start(void)
{
    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AUDIO_RegisterDMACallback(aq_next);
    memset(g_aq_dma, 0, sizeof g_aq_dma);
    DCFlushRange(g_aq_dma, sizeof g_aq_dma);
    AUDIO_InitDMA((u32)g_aq_dma[0], sizeof g_aq_dma[0]);
    AUDIO_StartDMA();
    g_aq_cur = 1;               /* the block after the first */
    AUDIO_InitDMA((u32)g_aq_dma[1], sizeof g_aq_dma[1]);
    g_aq_on = 1;
}

int plat_audio_level(void)
{
    return g_aq_on ? (int)(g_aq_in - g_aq_out) : -1;
}

uint32_t plat_audio_write(const int16_t *lr, uint32_t frames)
{
    const uint32_t at = g_aq_in, room = AQ_FRAMES - (at - g_aq_out);
    uint32_t k;
    if (!g_aq_on)
        return 0;
    if (frames > room)
        frames = room;
    for (k = 0; k < frames; k++) {
        g_aq[(at + k) % AQ_FRAMES][0] = lr[2 * k];
        g_aq[(at + k) % AQ_FRAMES][1] = lr[2 * k + 1];
    }
    if (g_aq_tone)
        for (k = 0; k < frames; k++)
            g_aq[(at + k) % AQ_FRAMES][0] = (at + k) % 218 < 109 ? 6000 : -6000;
    g_aq_in = at + frames;
    return frames;
}

/* status: blocks played and of them silent for want of samples, since the last call */
void plat_audio_report(uint32_t *blocks, uint32_t *silent)
{
    *blocks = g_aq_blocks;
    *silent = g_aq_under;
    g_aq_blocks = g_aq_under = 0;
}

/* NTFS volumes, if any, join the roots after the FAT ones. libntfs mounts them
 * as ntfs0, ntfs1, ...; its devoptab makes the usual fopen/mkdir/stat work. */
static void ntfs_mount_all(void)
{
    int n, i;
    g_ntfs_count = n = ntfsMountAll(&g_ntfs_mounts, NTFS_DEFAULT | NTFS_RECOVER);
    if (n <= 0) {
        g_ntfs_count = 0;
        g_ntfs_mounts = NULL;
        return;
    }
    for (i = 0; i < n && g_ntfs_nroots + 2 <= (int)(sizeof g_ntfs_roots / sizeof g_ntfs_roots[0]); i++) {
        const char *vol = ntfsGetVolumeName(g_ntfs_mounts[i].name);
        snprintf(g_ntfs_roots[g_ntfs_nroots++], 64, "%s:/burnout3", g_ntfs_mounts[i].name);
        snprintf(g_ntfs_roots[g_ntfs_nroots++], 64, "%s:/apps/burnout3", g_ntfs_mounts[i].name);
        plat_log("storage: NTFS %s (%s)", g_ntfs_mounts[i].name, vol ? vol : "no label");
    }
}

/* The data root must hold the image: the app's own folder exists even when
 * the data is on another device, and must not shadow it. */
static int root_has_image(const char *root)
{
    char path[160];
    struct stat st;
    snprintf(path, sizeof path, "%s/burnout3.iso", root);
    return !stat(path, &st) && S_ISREG(st.st_mode);
}

static int ntfs_on_usb(void)
{
    int i;
    for (i = 0; i < g_ntfs_count; i++)
        if (g_ntfs_mounts[i].interface && (g_ntfs_mounts[i].interface->features & FEATURE_WII_USB))
            return 1;
    return 0;
}

static int choose_root(const char *root)
{
    struct stat st;
    plat_log("storage: looking in %s", root);
    if (!stat(root, &st) && S_ISDIR(st.st_mode) && root_has_image(root)) {
        snprintf(g_root, sizeof g_root, "%s", root);
        plat_log("storage: data folder %s", g_root);
        return 1;
    }
    return 0;
}

static void find_root(void)
{
    size_t i;
    const int usb_taken = ntfs_on_usb();
    /* the SD data folder, then NTFS: checking usb:/ before an NTFS mount can
     * touch libfat's USB device while libntfs owns it */
    if (choose_root(k_roots[0]))
        return;
    for (i = 0; i < (size_t)g_ntfs_nroots; i++)
        if (choose_root(g_ntfs_roots[i]))
            return;
    for (i = 1; i < sizeof k_roots / sizeof k_roots[0]; i++) {
        if (usb_taken && !strncmp(k_roots[i], "usb:", 4))
            continue;           /* libfat's usb: is the same NTFS device */
        if (choose_root(k_roots[i]))
            return;
    }
    plat_fatal("no burnout3.iso on SD or USB (put it in burnout3/ there)");
}

static void fpu_ps2_mode(void);

/* ---- sampling profiler ----------------------------------------------------------------------
 * Autopilot "<t> prof <seconds>": every 100 us an alarm (decrementer
 * interrupt) takes the PC it interrupted from the context libogc saved
 * (SPRG2), into a histogram of 64-byte buckets over MEM1; afterwards the
 * counts go to prof.bin next to the image (work/wprof.py maps them to
 * functions with the ELF's symbols). Code runs from MEM1 only. */
#define PROF_BUCKETS (17u * 1024 * 1024 / 64)    /* the code: text ends below 0x81100000 */
/* call stacks of every g_prof_every'th sample (spread over the whole run):
 * pc, lr, then the return addresses up the back chain of stack frames;
 * work/wprof.py -i turns them into time spent inside each function */
#define PROF_STACK 16
#define PROF_NSTACKS 4096
static uint16_t *g_prof_hist;
static uint32_t *g_prof_stacks, g_prof_nstacks, g_prof_every;
static uint32_t g_prof_other, g_prof_total;
static int g_prof_secs;                 /* set by the autopilot: start a profile of this many seconds */
static int g_bench_prof;                /* autopilot "benchprof S": a profile of S seconds when the benchmark ends */
static volatile int g_prof_on;
static uint64_t g_prof_end;
static syswd_t g_prof_alarm;

static void prof_alloc(void)
{
    if (!g_prof_hist) {
        g_prof_hist = (uint16_t *)mem2_alloc(PROF_BUCKETS * 2);
        g_prof_stacks = (uint32_t *)mem2_alloc(PROF_NSTACKS * PROF_STACK * 4);
    }
}

static void prof_sample(syswd_t alarm, void *arg)
{
    const uint32_t *ctx;
    uint32_t pc, b;
    (void)alarm;
    (void)arg;
    if (!g_prof_on)
        return;
    __asm__ volatile("mfsprg %0, 2" : "=r"(ctx));
    pc = ctx[0];
    if (g_prof_stacks && g_prof_nstacks < PROF_NSTACKS && !(g_prof_total % g_prof_every)) {
        /* r1 (gpr[1] at word 7) is the interrupted function's frame, if it
         * made one: the word there links to the caller's frame, whose word
         * 1 holds the return address the function saved */
        uint32_t *st = g_prof_stacks + PROF_STACK * g_prof_nstacks++, k = 2, f = ctx[7], up;
        st[0] = pc;
        st[1] = ctx[3];
        while (k < PROF_STACK && !(f & 7) && ((f >= 0x80000000u && f < 0x81800000u) || (f >= 0x90000000u && f < 0x94000000u))) {
            up = *(const uint32_t *)f;
            if (up <= f || up - f > 0x100000u || (up >= 0x81800000u && up < 0x90000000u) || up >= 0x94000000u)
                break;
            st[k++] = *(const uint32_t *)(up + 4);
            f = up;
        }
        while (k < PROF_STACK)
            st[k++] = 0;
    }
    g_prof_total++;
    b = (pc - 0x80000000u) / 64;
    if (b < PROF_BUCKETS && g_prof_hist[b] != 0xFFFF)
        g_prof_hist[b]++;
    else
        g_prof_other++;
}

FILE *plat_debug_file(const char *name);

/* the benchmark (gs_gx.c wh_bench) has its frames: "benchprof" starts at the next vblank */
void plat_bench_done(void)
{
    plat_log_sync();
    if (g_bench_prof && g_prof_hist) {
        g_prof_secs = g_bench_prof;
        g_bench_prof = 0;
    }
}

/* ---- watchdog -------------------------------------------------------------------------------------
 * A hang (no emulated vblank for 3 s) is reported once on the UART log
 * (Dolphin's OSReport; no file I/O, no locks): the context the alarm
 * interrupted and, when that is not the main thread (it sleeps: a lock, IOS,
 * the GPU), the main thread's saved context; each with return addresses up
 * its stack (powerpc-eabi-addr2line -f -e build-wii/burnout3.elf ADDR...),
 * and the GPU's status. */
#include <tuxedo/thread.h>
static syswd_t g_wd_alarm;
static KThread *g_wd_main;
static volatile uint32_t g_wd_beat;
static uint32_t g_wd_seen, g_wd_still, g_wd_done;

static void wd_stack(const char *what, const uint32_t *ctx)
{
    uint32_t ra[14], n = 0, f = ctx[7], up;
    while (n < 14 && !(f & 7) && ((f >= 0x80000000u && f < 0x81800000u) || (f >= 0x90000000u && f < 0x94000000u))) {
        up = *(const uint32_t *)f;
        if (up <= f || up - f > 0x100000u || (up >= 0x81800000u && up < 0x90000000u) || up >= 0x94000000u)
            break;
        ra[n++] = *(const uint32_t *)(up + 4);
        f = up;
    }
    SYS_Report("watchdog: %s pc %08X lr %08X sp %08X, return addresses:", what, ctx[0], ctx[3], ctx[7]);
    for (f = 0; f < n; f++)
        SYS_Report(" %08X", ra[f]);
    SYS_Report("\n");
}

static void wd_check(syswd_t alarm, void *arg)
{
    const uint32_t *ctx;
    (void)alarm;
    (void)arg;
    if (g_wd_beat != g_wd_seen) {
        g_wd_seen = g_wd_beat;
        g_wd_still = 0;
        return;
    }
    if (g_wd_done || ++g_wd_still < 6)
        return;
    g_wd_done = 1;
    log_show_queue();
    __asm__ volatile("mfsprg %0, 2" : "=r"(ctx));
    {
        u8 overhi, underlo, ridle, cidle, brk;
        GX_GetGPStatus(&overhi, &underlo, &ridle, &cidle, &brk);
        SYS_Report("watchdog: no emulated vblank for 3 s (vblank %u); GPU fifo over %u under %u read idle %u command "
                   "idle %u\n", g_wd_beat, overhi, underlo, ridle, cidle);
    }
    wd_stack("interrupted:", ctx);
    if (g_wd_main && (const void *)ctx != (const void *)&g_wd_main->ctx) {
        SYS_Report("watchdog: main thread state %u\n", g_wd_main->state);
        wd_stack("main thread:", (const uint32_t *)&g_wd_main->ctx);
    }
}

static void wd_start(void)
{
    struct timespec ts = {0, 500 * 1000 * 1000};
    g_wd_main = KThreadGetSelf();
    SYS_CreateAlarm(&g_wd_alarm);
    SYS_SetPeriodicAlarm(g_wd_alarm, &ts, &ts, wd_check, NULL);
}

/* from plat_vblank: start and finish */
static void prof_poll(void)
{
    if (g_prof_secs && g_prof_hist && !g_prof_on) {
        struct timespec ts = {0, 100 * 1000};
        memset(g_prof_hist, 0, PROF_BUCKETS * 2);
        g_prof_other = g_prof_total = g_prof_nstacks = 0;
        g_prof_every = (uint32_t)g_prof_secs * 10000u / PROF_NSTACKS + 1;
        g_prof_end = gettime() + secs_to_ticks(g_prof_secs);
        g_prof_secs = 0;
        if (!g_prof_alarm)
            SYS_CreateAlarm(&g_prof_alarm);
        g_prof_on = 1;
        SYS_SetPeriodicAlarm(g_prof_alarm, &ts, &ts, prof_sample, NULL);
        plat_log("profile: started");
    } else if (g_prof_on && gettime() >= g_prof_end) {
        FILE *f;
        uint32_t k;
        g_prof_on = 0;
        SYS_CancelAlarm(g_prof_alarm);
        f = plat_debug_file("prof.bin");
        if (f) {
            /* header: magic, samples, samples outside MEM1; then (bucket, count) pairs */
            uint32_t h[3] = {0x50524F46u, g_prof_total, g_prof_other};
            fwrite(h, 4, 3, f);
            for (k = 0; k < PROF_BUCKETS; k++)
                if (g_prof_hist[k]) {
                    uint32_t e[2] = {k, g_prof_hist[k]};
                    fwrite(e, 4, 2, f);
                }
            fclose(f);
        }
        f = plat_debug_file("profstack.bin");
        if (f && g_prof_stacks) {
            fwrite(g_prof_stacks, PROF_STACK * 4, g_prof_nstacks, f);
            fclose(f);
        }
        plat_log("profile: %u samples (%u outside MEM1) -> prof.bin", g_prof_total, g_prof_other);
        plat_log_sync();
    }
}

/* Scripted input for testing without a controller: auto.txt next to the
 * image, lines of "<seconds> <button> <fields>" (button names as below), or
 * "<seconds> native <0|1>" to switch native rendering (A/B comparisons). */
static struct { uint32_t at_us, until_us, bits; int port; } g_auto[64];
static int g_nauto;
/* args.txt "--pad 0": the PS2 face-button positions (GameCube B=square,
 * X=circle; Classic B=cross, A=circle). "--pad 1" (the default) keeps the
 * Nintendo positions for A and B, so A is select/accelerate and B is
 * back/boost, as the console's controllers are labelled. */
static int g_pad_profile = 1;
int g_rumble = 1;               /* args.txt "--rumble 0", autopilot "rumble 0": the game's vibration is not sent to the controllers */
static uint32_t g_pad_mask;     /* the GameCube pads PAD_ScanPads found plugged in, a bit each */
static int g_auto_nopad;        /* autopilot "nopad 1": the controllers are ignored */
static int g_auto_p2;           /* the script presses the second player's buttons ("p2cross"): port 1 is plugged in */
static uint32_t g_auto_b2;      /* those held now (worked out with port 0's) */
static struct { uint32_t at_us; int on, done; int *var; const char *name; } g_auto_native[16];
static int g_nauto_native;
/* "<t> waitfps <n>": from t on, the script's clock stops until the game draws
 * n frames a second again (loading screens draw ~11): load times vary by
 * tens of seconds between runs */
static struct { uint32_t at_us; int fps, card, draws, done; } g_auto_wait[8];
static int g_nauto_wait;
static uint32_t g_auto_shift;

/* Test positions ("<t> gotox <dm>", "<t> gotoz <dm>", "<t> goto 1"): the
 * player's car (0x57BAF0: velocity at +176, speed at +188, its matrix through
 * +516, rows right, up, forward, position) driven to a point of the world
 * with the pad and stopped there, so that a view can be had again in the
 * next run. Which of left and right turns the car which way is found out
 * (the heading turning the wrong way while a button is held: the other
 * one). Past the point it backs up; against an obstacle it backs off for a
 * second with the wheel the other way. */
static int g_goto_x, g_goto_z, g_goto_on;
static int g_goto_r;        /* "gotor <dm>": only once the car is within this of the point (it gets there by the script) */
static int g_goto_active;   /* the pad is the controller's alone */

static uint32_t goto_buttons(uint32_t now_us)
{
    enum { CROSS = 1 << 6, SQUARE = 1 << 7, LEFT = 1 << 15, RIGHT = 1 << 13 };
    static uint32_t state, since, stuck_since, turn_at, back_until, log_at, held;
    static int sign = 1;
    static float hx, hz;
    const uint32_t car = 0x57BAF0, mat = LW(car + 516);
    float px, pz, fx, fz, tx, tz, dist, ahead, turn, speed, along;
    uint32_t b = 0;
    if (g_goto_on != 1 || !mat || mat >= PS2_RAM_SIZE - 64)
        return 0;
    px = LWF(mat + 48); pz = LWF(mat + 56);
    fx = LWF(mat + 32); fz = LWF(mat + 40);
    speed = LWF(car + 188);
    along = LWF(car + 176) * fx + LWF(car + 184) * fz;      /* forward speed, signed */
    tx = (float)g_goto_x * 0.1f - px;
    tz = (float)g_goto_z * 0.1f - pz;
    dist = sqrtf(tx * tx + tz * tz);
    ahead = tx * fx + tz * fz;
    turn = (fz * tx - fx * tz) / (dist + 0.01f);            /* which way the heading must turn (about up) */
    if (!state && g_goto_r && dist > (float)g_goto_r * 0.1f)
        return 0;
    g_goto_active = 1;
    if (!state) {
        state = 1;
        stuck_since = turn_at = now_us;
        hx = fx;
        hz = fz;
    }
    if (state == 3 || dist < 0.6f) {
        /* there: stop */
        if (state != 3) {
            state = 3;
            since = now_us;
        }
        if (fabsf(along) < 0.08f || now_us - since > 4000000) {
            g_goto_on = 2;
            g_goto_active = 0;
            state = 0;
            plat_log("autopilot: at %.2f %.2f (%.2f from the point), forward %.3f %.3f", (double)px, (double)pz,
                     (double)dist, (double)fx, (double)fz);
            return 0;
        }
        return along > 0.25f ? SQUARE : along < -0.25f ? CROSS : 0;
    }
    if (now_us < back_until)
        b = SQUARE | (sign * turn > 0.0f ? RIGHT : LEFT);
    else if (speed < 0.3f && now_us - stuck_since > 1500000) {
        back_until = now_us + 1200000;      /* against something */
        stuck_since = back_until;
        b = SQUARE;
    } else if (state == 2 || (ahead < -0.5f && dist < 12.0f)) {
        /* past it: straight back until it is well ahead again */
        state = ahead > 3.0f ? 1 : 2;
        b = along > 0.5f || along > -2.0f ? SQUARE : 0;
    } else {
        if (fabsf(turn) > 0.04f || ahead < 0.0f)
            b |= sign * turn > 0.0f ? LEFT : RIGHT;
        if (speed < (dist > 6.0f ? 5.0f : dist > 2.0f ? 2.5f : 1.0f))
            b |= CROSS;
        else if (speed > (dist > 6.0f ? 8.0f : dist > 2.0f ? 4.0f : 1.6f))
            b |= SQUARE;
    }
    if (speed >= 0.3f)
        stuck_since = now_us < back_until ? back_until : now_us;
    /* the heading over the last 0.3 s against the button held all that time (moving forward) */
    if ((b & (LEFT | RIGHT)) != held || along < 1.0f) {
        held = b & (LEFT | RIGHT);
        turn_at = now_us;
        hx = fx;
        hz = fz;
    } else if (held && now_us - turn_at > 300000) {
        float turned = hz * fx - hx * fz;
        if (fabsf(turned) > 0.004f && (turned > 0.0f) != (held == (sign > 0 ? LEFT : RIGHT)))
            sign = -sign;
        turn_at = now_us;
        hx = fx;
        hz = fz;
    }
    if (now_us - log_at > 1000000) {
        log_at = now_us;
        plat_log("goto: at %.1f %.1f dist %.1f ahead %.1f turn %.2f speed %.1f (%.1f) state %u sign %d buttons %s%s%s%s",
                 (double)px, (double)pz, (double)dist, (double)ahead, (double)turn, (double)speed, (double)along, state,
                 sign, b & CROSS ? "X" : "", b & SQUARE ? "S" : "", b & LEFT ? "L" : "", b & RIGHT ? "R" : "");
    }
    return b;
}

static void autopilot_load(void)
{
    static const struct { const char *name; uint32_t bit; } names[] = {
        {"start", 1 << 11}, {"select", 1 << 8}, {"cross", 1 << 6}, {"circle", 1 << 5}, {"square", 1 << 7},
        {"triangle", 1 << 4}, {"up", 1 << 12}, {"down", 1 << 14}, {"left", 1 << 15}, {"right", 1 << 13},
        {"l1", 1 << 2}, {"r1", 1 << 3}, {"l2", 1 << 0}, {"r2", 1 << 1},
    };
    char path[128], name[32];
    double t;
    int fields;
    FILE *f;
    snprintf(path, sizeof path, "%s/auto.txt", g_root);
    f = fopen(path, "r");
    if (!f)
        return;
    while (g_nauto < 64 && fscanf(f, "%lf %31s %d", &t, name, &fields) == 3) {
        size_t k;
        if (!strcmp(name, "waitfps") && g_nauto_wait < 8) {
            g_auto_wait[g_nauto_wait].at_us = (uint32_t)(t * 1e6);
            g_auto_wait[g_nauto_wait++].fps = fields;
            continue;
        }
        /* "<t> waitdraws <n>": until the frames have n draws each (+-3%): a
         * screen known by its draw count (Burnout 3's "load successful"
         * prompt: ~1140) */
        if (!strcmp(name, "waitdraws") && g_nauto_wait < 8) {
            g_auto_wait[g_nauto_wait].at_us = (uint32_t)(t * 1e6);
            g_auto_wait[g_nauto_wait++].draws = fields;
            continue;
        }
        /* "<t> waitcard <tenths>": the clock stops until the memory card was
         * used in the 5 s before t or since, and then not for that long */
        if (!strcmp(name, "waitcard") && g_nauto_wait < 8) {
            g_auto_wait[g_nauto_wait].at_us = (uint32_t)(t * 1e6);
            g_auto_wait[g_nauto_wait++].card = fields;
            continue;
        }
        /* "<t> prof <seconds>": the sampling profiler (prof_start) */
        if (!strcmp(name, "prof") && g_nauto_native < 16) {
            g_auto_native[g_nauto_native].at_us = (uint32_t)(t * 1e6);
            g_auto_native[g_nauto_native].var = &g_prof_secs;
            g_auto_native[g_nauto_native].name = "profile";
            g_auto_native[g_nauto_native++].on = fields;
            prof_alloc();
            continue;
        }
        /* "<t> benchprof <seconds>": the profiler right after the benchmark ("wstat 5") has its frames, whenever the
         * race gets there (a console's loading times are not Dolphin's: no timed presses needed) */
        if (!strcmp(name, "benchprof")) {
            g_bench_prof = fields;
            prof_alloc();
            continue;
        }
        /* "<t> native <mask>", and debugging switches of the GX backend and the GS */
        if (g_nauto_native < 16) {
            extern int g_native_render, g_gx_maxdraw, g_gx_rtdump, g_gx_flags, g_gx_flags2, g_tlog_fbp, g_gs_stat, g_vu0_stat, g_native_log, g_gs_clut_up, g_gs_ext_on, g_gs_clut_cache, g_gs_clut_chk, g_gs_clut_lazy,
                g_timescale, g_fps, g_gs_upchk, g_gs_statdraws, g_vu0_chk_all, g_vu0_poison, g_vu0_lean, g_spu_mute, g_spu_dump, g_spu_irqdl, g_idle_spin_ms, g_wstat, g_pt_on, g_eng_native, g_nwc_on, g_aq_tone, g_dbg, g_pace, g_pace_passes, g_env_rot, g_gx_meter, g_frame_hist, g_rest_hold, g_tex_place, g_carlog, g_sif_wait, g_mpg_lazy, g_gx_freeze, g_opt_new, g_vu0_time, g_vu0_coll, g_coll_c90, g_vu0_inl, g_coll_bf0, g_coll_400, g_coll_chk, g_coll_time, g_coll_mut, g_opt3, g_carest, g_carpair, g_wbn, g_cpy, g_vpk, g_widescreen, g_load_step, g_load_sleep, g_load_fe, g_clip, g_clip_band, g_clip_every, g_dcbz_on, g_ext_reread, g_gain_heard, g_rev_new, g_trust, g_wskip, g_tmemo2, g_tmemo2_n;
            extern int g_shadow_floor;
            static const struct { const char *name, *what; int *var; } sw[] = {
                {"rumble", "the game's vibration sent to the GameCube pad and the Wii Remote (1; 0 off)", &g_rumble},
                {"shadowfloor","percent of its full strength the car shadow keeps in shade (35; 0 = the game's own)", &g_shadow_floor},
                {"dcbz", "memcpy, memset and the colour arrays fill lines with dcbz (off under Dolphin)", &g_dcbz_on},
                {"reread", "an unchanged texture source is read in full again every N frames (120)", &g_ext_reread},
                {"trust", "debug: guest memory taken for unchanged (1 parse cache, 2 upload sources, 4 display lists; sum)", &g_trust},
                {"wskip", "debug: world batches after a call's first take its draw state unlooked (1), or do it all and count the runs that write to the GX FIFO (2; 4 for every batch: the control)", &g_wskip},
#ifndef NO_TMEMO2
                {"tmemo", "texture memo validated by the texture's own memory and palette (gs_gx.c): 0 off, 1 on (default), 2 on and every use checked against the full lookup", &g_tmemo2},
                {"tmemon", "entries of that memo (a power of two, 16 to 1024; 1024)", &g_tmemo2_n},
#endif
                {"heard", "voices with a gain below N (of 32768) are not mixed (32)", &g_gain_heard},
                {"dsp", "voices heard dry only stretched and summed by the DSP (1), here from their streams (2), both ways compared (3)", &g_dsp_on},
                {"rev", "the reverb a chunk at a time (1), a sample at a time as first written (0), both ways compared (2)", &g_rev_new},
                {"native", "native rendering", &g_native_render}, {"maxdraw", "maxdraw", &g_gx_maxdraw},
                {"rtdump", "rtdump", &g_gx_rtdump}, {"gxflags", "gxflags", &g_gx_flags}, {"gxflags2", "gxflags2", &g_gx_flags2}, {"tlog", "target log", &g_tlog_fbp},
                {"gsstat", "gsstat (fields)", &g_gs_stat}, {"vu0stat", "VU0 call counts", &g_vu0_stat},
                {"nlog", "native pipeline logs", &g_native_log}, {"clutup", "CLUTs from uploads", &g_gs_clut_up},
                {"extup", "uploads in place", &g_gs_ext_on}, {"clutcache", "CLUT cache", &g_gs_clut_cache}, {"clutlazy", "CLUT copies made when read", &g_gs_clut_lazy},
                {"clutchk", "CLUT check", &g_gs_clut_chk}, {"timescale", "time scale", &g_timescale},
                {"fps", "frame rate mode", &g_fps}, {"upchk", "upload identity check", &g_gs_upchk},
                {"statdraws", "gsstat after a frame of more draws than", &g_gs_statdraws},
                {"vu0chk", "every native VU0 call checked", &g_vu0_chk_all},
                {"vu0poison", "registers a native VU0 routine does not leave behind overwritten", &g_vu0_poison},
                {"nopad", "controllers ignored", &g_auto_nopad}, {"dbg", "experiments", &g_dbg}, {"pace", "frame pacing", &g_pace}, {"passes", "update passes by the frames shown", &g_pace_passes}, {"sifwait", "RPC replies at the next timer event", &g_sif_wait}, {"mpglazy", "microprograms noted, not copied", &g_mpg_lazy}, {"envrot", "reflection views in turn", &g_env_rot}, {"vu0lean", "VU0 routines that leave only what is read", &g_vu0_lean}, {"mute", "no sound mixed", &g_spu_mute}, {"parsecache", "world packets' parses remembered", &g_nwc_on}, {"engnative", "engine sound filtered by the mixer", &g_eng_native}, {"spudump", "the playing voices logged", &g_spu_dump}, {"spudl", "the sound chip's interrupt wakes the idle wait", &g_spu_irqdl}, {"idlespin", "idle waits up to N ms on the clock", &g_idle_spin_ms}, {"wstat", "cycles of the world batches' path", &g_wstat}, {"ptstat", "the time split in the status line", &g_pt_on}, {"aswap", "sound channels the other way round", &g_aq_swap}, {"atone", "a test tone on the left channel", &g_aq_tone}, {"meter", "frame time meter", &g_gx_meter}, {"fhist", "frames around a collision logged", &g_frame_hist}, {"resthold", "stopped cars held", &g_rest_hold}, {"texplace", "textures placed without uploads", &g_tex_place}, {"carlog", "car log at the fence", &g_carlog}, {"freeze", "the picture held at game tick", &g_gx_freeze}, {"newopt", "the speedups of 2026-10-15", &g_opt_new}, {"vu0time", "recompiled VU0 calls timed", &g_vu0_time}, {"vu0coll", "collision tests decided natively", &g_vu0_coll}, {"collc90", "0x274C90 natively; 2: every call checked", &g_coll_c90}, {"vu0inl", "VU0 collision entries inline in the generated code", &g_vu0_inl}, {"collbf0", "0x273BF0 as its inline variant; 2: every call checked", &g_coll_bf0}, {"coll400", "0x273400 as its inline variant; 2: every call checked", &g_coll_400}, {"collchk", "calls of each inline variant that run checked", &g_coll_chk}, {"colltime", "clock reads around the two collision functions", &g_coll_time}, {"collmut", "debug: the segment of the next N checked 0x273400 calls stretched", &g_coll_mut}, {"opt3", "the speedups of 2026-10-16 (A/B)", &g_opt3}, {"carest", "car sphere-map UVs from the square roots' estimate", &g_carest}, {"carpair", "car shading two vertices a register", &g_carpair}, {"wbn", "world batches (0x26AF50) natively; 2: every call checked", &g_wbn}, {"cpy", "the staged batches copied natively (0x26B1C0); 2: checked", &g_cpy}, {"vpk", "the view packet natively (0x26BAA0); 2: every call checked", &g_vpk},
                {"widescreen", "widescreen 16:9 (the data part of PCSX2's patch)", &g_widescreen},
                {"loadstep", "a load's step taken again within the frame for up to N ms", &g_load_step}, {"loadnap", "us of each nap between two steps of a load", &g_load_sleep}, {"loadfe", "the front end's loads stepped again too", &g_load_fe}, {"clip", "the world's batches cut to the screen (platform/wii/clip.h)", &g_clip}, {"clipband", "percent of the screen a triangle may reach outside it before it is cut", &g_clip_band}, {"clipevery", "a batch with nothing to cut is looked at every N frames", &g_clip_every},
                {"gotox", "drive to x (dm)", &g_goto_x}, {"gotoz", "drive to z (dm)", &g_goto_z},
                {"goto", "drive to the point", &g_goto_on}, {"gotor", "drive to the point from within (dm)", &g_goto_r},
            };
            size_t j;
            for (j = 0; j < sizeof sw / sizeof sw[0] && strcmp(name, sw[j].name); j++)
                ;
            if (j < sizeof sw / sizeof sw[0]) {
                g_auto_native[g_nauto_native].at_us = (uint32_t)(t * 1e6);
                g_auto_native[g_nauto_native].var = sw[j].var;
                g_auto_native[g_nauto_native].name = sw[j].what;
                g_auto_native[g_nauto_native++].on = fields;
                continue;
            }
        }
        for (k = 0; k < sizeof names / sizeof names[0]; k++) {
            int p2 = !strncmp(name, "p2", 2);
            if (!strcmp(name + (p2 ? 2 : 0), names[k].name)) {
                g_auto[g_nauto].at_us = (uint32_t)(t * 1e6);
                g_auto[g_nauto].until_us = (uint32_t)(t * 1e6 + fields * 16683.0);
                g_auto[g_nauto].bits = names[k].bit;
                g_auto[g_nauto].port = p2;
                g_auto_p2 |= p2;
                g_nauto++;
            }
        }
    }
    fclose(f);
    plat_log("autopilot: %d presses", g_nauto);
}

static uint32_t autopilot_buttons(void)
{
    extern uint32_t g_vif1_chains;
    extern uint32_t gs_backend_draw_count(void);
    static uint32_t last, t0, c0, d0;
    static float fps, dpf;
    uint32_t raw = (uint32_t)plat_time_us(), now, b = 0;
    int k;
    /* frames a second, measured each half second */
    if (raw - t0 >= 500000) {
        uint32_t d = gs_backend_draw_count();
        fps = (float)(g_vif1_chains - c0) * 1e6f / (float)(raw - t0);
        dpf = g_vif1_chains != c0 ? (float)(d - d0) / (float)(g_vif1_chains - c0) : 0.0f;
        t0 = raw;
        c0 = g_vif1_chains;
        d0 = d;
    }
    if (!last)
        last = raw;
    now = raw - g_auto_shift;
    for (k = 0; k < g_nauto_wait; k++)
        if (now >= g_auto_wait[k].at_us && !g_auto_wait[k].done) {
            static uint32_t since, ok;
            if (!since) {
                since = raw;
                ok = 0;
            }
            if (g_auto_wait[k].draws) {
                /* this screen, or one the following waits look for: the game
                 * may be faster than the script (then the presses between are
                 * for screens already passed, and the clock jumps there) */
                static int cand = -1;
                int j, hit = -1;
                for (j = k; j < g_nauto_wait && g_auto_wait[j].draws && hit < 0; j++) {
                    float want = (float)g_auto_wait[j].draws;
                    if (dpf > want * 0.97f && dpf < want * 1.03f && t0 > since + 400000)
                        hit = j;
                }
                if (hit >= 0 && hit == cand) {
                    if (++ok >= 60) {
                        for (j = k; j <= hit; j++)
                            g_auto_wait[j].done = 1;
                        plat_log("autopilot: waited %.1f s for %d draws a frame%s", (raw - since) / 1e6,
                                 g_auto_wait[hit].draws, hit > k ? " (a later screen: script skipped ahead)" : "");
                        if (hit > k)
                            g_auto_shift = raw - g_auto_wait[hit].at_us;
                        since = 0;
                        cand = -1;
                        last = raw;
                        break;
                    }
                } else {
                    cand = hit;
                    ok = 0;
                }
            } else if (g_auto_wait[k].card) {
                extern uint64_t g_card_last_us;
                uint32_t last_card = (uint32_t)g_card_last_us;
                if (last_card + 5000000u > since && raw - last_card >= (uint32_t)g_auto_wait[k].card * 100000u) {
                    g_auto_wait[k].done = 1;
                    plat_log("autopilot: waited %.1f s for the memory card", (raw - since) / 1e6);
                    since = 0;
                }
            /* two half-second measurements in a row at the rate, both after the wait began */
            } else if (fps >= (float)g_auto_wait[k].fps && t0 > since + 400000) {
                if (++ok >= 2 * 30) {   /* counted per field: 2 halves need ~60 fields */
                    g_auto_wait[k].done = 1;
                    plat_log("autopilot: waited %.1f s for %d frames/s", (raw - since) / 1e6, g_auto_wait[k].fps);
                    since = 0;
                }
            } else {
                ok = 0;
            }
            if (!g_auto_wait[k].done)
                g_auto_shift += raw - last;
            break;
        }
    last = raw;
    now = raw - g_auto_shift;
    g_auto_b2 = 0;
    for (k = 0; k < g_nauto; k++)
        if (now >= g_auto[k].at_us && now < g_auto[k].until_us) {
            if (g_auto[k].port)
                g_auto_b2 |= g_auto[k].bits;
            else
                b |= g_auto[k].bits;
        }
    for (k = 0; k < g_nauto_native; k++)
        if (now >= g_auto_native[k].at_us && !g_auto_native[k].done) {
            g_auto_native[k].done = 1;
            *g_auto_native[k].var = g_auto_native[k].on;
            plat_log("autopilot: %s %d", g_auto_native[k].name, g_auto_native[k].on);
        }
    {
        uint32_t g = goto_buttons(raw);
        if (g_goto_active || g_goto_on == 2)
            b = g;                      /* the script's own presses (the accelerator held) end there */
    }
    return b;
}

/* debugging output next to the image (GX target dumps) */
FILE *plat_debug_file(const char *name)
{
    char path[160];
    snprintf(path, sizeof path, "%s/%s", g_root, name);
    return fopen(path, "wb");
}

static int memory_card_ok(const char *path)
{
    static const long card_bytes = 0x4000L * 528;      /* 8 MB, 512+16 bytes a page */
    FILE *f = fopen(path, "rb");
    char magic[20];
    struct stat st;
    int ok = 0;
    if (!f)
        return 0;
    if (fread(magic, 1, sizeof magic, f) == sizeof magic)
        ok = !memcmp(magic, "Sony PS2 Memory Card", sizeof magic);
    fclose(f);
    return ok && stat(path, &st) == 0 && st.st_size == card_bytes;
}

static int copy_file(const char *src, const char *dst)
{
    static char buf[64 * 1024];
    FILE *in = fopen(src, "rb"), *out;
    size_t n;
    int ok = 0;
    if (!in)
        return 0;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            goto done;
    ok = !fflush(out);
    if (ok)
        fsync(fileno(out));
done:
    fclose(out);
    fclose(in);
    if (!ok)
        remove(dst);
    return ok;
}

/* A save is small and SD is always writable (the NTFS volume may be dirty,
 * hibernated or mounted read-only). Keep the card on SD and bring an existing
 * one over from the data folder the first time; fall back to the data folder
 * when there is no SD card. */
static void choose_save_dir(void)
{
    static const char *const sd_dir = "sd:/burnout3/saves";
    char card[96], src[96];
    FILE *test;
    struct stat st;
    if (!mkdir("sd:/burnout3", 0777))
        plat_log("storage: made sd:/burnout3");
    mkdir(sd_dir, 0777);
    snprintf(card, sizeof card, "%s/Mcd001.ps2", sd_dir);
    test = fopen("sd:/burnout3/saves/.write-test", "wb");
    if (!test) {
        snprintf(g_save_dir, sizeof g_save_dir, "%s/saves", g_root);
        mkdir(g_save_dir, 0777);
        plat_log("storage: saves in %s (no writable SD card)", g_save_dir);
        return;
    }
    fclose(test);
    remove("sd:/burnout3/saves/.write-test");
    snprintf(src, sizeof src, "%s/saves/Mcd001.ps2", g_root);
    if (!memory_card_ok(card)) {
        if (memory_card_ok(src)) {
            if (copy_file(src, card))
                plat_log("storage: copied the memory card from %s to %s", src, card);
            else
                plat_log("storage: could not copy %s to %s", src, card);
        } else if (stat(src, &st) == 0) {
            plat_log("storage: %s is not a formatted PS2 card", src);
        }
    }
    if (!memory_card_ok(card) && stat(src, &st) == 0) {
        /* The SD card has no usable card (or the copy failed): keep using the
         * data folder's card instead of creating an empty SD one. */
        snprintf(g_save_dir, sizeof g_save_dir, "%s/saves", g_root);
        plat_log("storage: saves in %s (the SD card has no valid card)", g_save_dir);
        return;
    }
    snprintf(g_save_dir, sizeof g_save_dir, "%s", sd_dir);
    plat_log("storage: saves in %s", g_save_dir);
}

void plat_init(int argc, char **argv)
{
    char path[128];
    (void)argc;
    (void)argv;
    fpu_ps2_mode();
    g_t0 = gettime();
    LWP_MutexInit(&g_log_lock, false);
    video_init();
    printf("\n\n Burnout 3 for Wii (recompiled)\n\n");
    WPAD_Init();
    PAD_Init();
    if (!fatInitDefault())
        plat_fatal("cannot mount SD or USB storage");
    g_sd_mounted = sd_remount();
    ntfs_mount_all();
    find_root();
    snprintf(path, sizeof path, "%s/run.log", g_root);
    /* libntfs does not update a truncated file's directory timestamp; remove
     * the old log first so a new run is visibly new in an explorer too. */
    remove(path);
    g_log = fopen(path, "w");
    if (!g_log) {
        /* The data folder may be an NTFS mount that is read-only (a dirty or
         * hibernated volume): keep the log on the SD card instead. */
        static const char *const alt[] = {"sd:/apps/burnout3/run.log", "sd:/burnout3/run.log"};
        size_t k;
        for (k = 0; k < sizeof alt / sizeof alt[0] && !g_log; k++) {
            if (k == 1)
                mkdir("sd:/burnout3", 0777);
            remove(alt[k]);
            g_log = fopen(alt[k], "w");
            if (g_log)
                snprintf(path, sizeof path, "%s", alt[k]);
        }
    }
    if (g_log) {
        setvbuf(g_log, NULL, _IOFBF, 64 * 1024);
        plat_log("log: %s", path);
    } else {
        plat_log("log: cannot open a run.log on %s or SD", g_root);
    }
    choose_save_dir();
    reserve_guest_ram();
    /* debug options in args.txt next to the image, e.g. "--trace 2" */
    snprintf(path, sizeof path, "%s/args.txt", g_root);
    {
        FILE *a = fopen(path, "r");
        char opt[32];
        int v;
        g_widescreen = 2;               /* 16:9 when the Wii's own screen setting says so, unless args.txt says otherwise */
        while (a && fscanf(a, "%31s %d", opt, &v) == 2)
            if (!strcmp(opt, "--trace"))
                g_trace = v;
            else if (!strcmp(opt, "--widescreen"))
                g_widescreen = v;       /* runtime/patches.c: 0 off, 1 on, 2 (the default) as the Wii's settings say */
            else if (!strcmp(opt, "--pad"))
                g_pad_profile = v ? 1 : 0;
            else if (!strcmp(opt, "--rumble"))
                g_rumble = v ? 1 : 0;   /* the game's vibration to the GameCube pad and the Wii Remote (plat_pad_rumble) */
            else if (!strcmp(opt, "--fps")) {
                extern int g_fps;
                g_fps = v == 30 ? 30 : v == 60 ? 60 : 0;
            } else if (!strcmp(opt, "--reflect")) {
                extern int g_env_period;
                if (v >= 1 && v <= 12)
                    g_env_period = v;       /* 1: every view every frame; out of range: the default (3) stays */
            } else if (!strcmp(opt, "--tvfit")) {
                extern int g_tv_fit;
                g_tv_fit = v ? 1 : 0;       /* 1: the picture in the frame as on the PS2 */
            } else if (!strcmp(opt, "--clip")) {
                extern int g_clip;
                g_clip = v ? 1 : 0;         /* the world's batches with big triangles cut to the screen (gs_gx.c clip_batch) */
            } else if (!strcmp(opt, "--stbench")) {
                extern int g_stbench;
                g_stbench = v ? 1 : 0;      /* platform/wii/pmc.c: what a store to a cold line costs and what dcbz takes off, measured once */
            } else if (!strcmp(opt, "--shadowfloor")) {
                extern int g_shadow_floor;
                if (v >= 0 && v <= 100)
                    g_shadow_floor = v;     /* percent of its full strength the car shadow keeps in shade (runtime/hle.c) */
            } else if (!strcmp(opt, "--clipevery")) {
                extern int g_clip_every;
                if (v >= 1 && v <= 60)
                    g_clip_every = v;       /* a batch with nothing to cut is looked at every v-th frame */
            } else if (!strcmp(opt, "--clipband")) {
                extern int g_clip_band;
                if (v >= 0 && v <= 400)
                    g_clip_band = v;        /* percent of the screen's size a triangle may reach outside it before it is cut */
            } else if (!strcmp(opt, "--reread")) {
                extern int g_ext_reread;
                if (v >= 1 && v <= 10000)
                    g_ext_reread = v;       /* runtime/gs.c: frames between two full reads of an unchanged texture source */
            } else if (!strcmp(opt, "--heard")) {
                extern int g_gain_heard;
                if (v >= 0 && v <= 4096)
                    g_gain_heard = v;       /* runtime/iop_sd.c: voices with a gain below this are not mixed */
            } else if (!strcmp(opt, "--dsp")) {
                if (v >= 0 && v <= 3)
                    g_dsp_on = v;           /* runtime/iop_sd.c: voices heard dry only stretched and summed by the DSP (1), here
                                             * from the streams made for it (2), both ways compared (3) */
            } else if (!strcmp(opt, "--lcbench")) {
                extern int g_lcbench;
                g_lcbench = v ? 1 : 0;      /* platform/wii/pmc.c: the locked cache's DMA measured once at the start */
            } else if (!strcmp(opt, "--pfbench")) {
                extern int g_pfbench;
                g_pfbench = v ? 1 : 0;      /* platform/wii/pmc.c: what dcbt overlaps and what a code line costs, measured once */
#ifndef NO_TMEMO2
            } else if (!strcmp(opt, "--tmemo")) {
                extern int g_tmemo2;
                if (v >= 0 && v <= 2)
                    g_tmemo2 = v;           /* gs_gx.c: the texture memo validated by the texture's own memory and palette: 0 off, 1 on
                                             * (the default), 2 on and every use also run through the full lookup and compared */
#endif
            } else if (!strcmp(opt, "--efbfill")) {
                /* debugging: the EFB filled with a colour at the start, like the console's (gs_gx.c efb_fill_test).
                 * It has to be set here: the autopilot's "0 gxflags2" is applied after gx_init has run. */
                extern int g_gx_flags2;
                if (v)
                    g_gx_flags2 |= 134217728;
            }
        if (a)
            fclose(a);
        plat_log("build: %s %s, trace %d", __DATE__, __TIME__, g_trace);
        plat_log("input: pad profile %d (%s)", g_pad_profile,
                 g_pad_profile ? "A cross, B circle, X square, Y triangle" : "PS2 face positions");
        plat_log("input: rumble %s", g_rumble ? "on" : "off (--rumble 0)");
        {
            extern int g_fps;
            plat_log("fps: %s", g_fps == 30 ? "30 (forced)" : g_fps == 60 ? "60 (forced)" : "automatic");
        }
        {
            extern int g_env_period;
            plat_log("reflections: one view in %d", g_env_period);
        }
        {
            extern int g_clip, g_clip_band, g_clip_every;
            plat_log("clip: %s (band %d%%, a batch with nothing to cut looked at every %d frames)",
                     g_clip ? "the world's big triangles are cut to the screen" : "off", g_clip_band, g_clip_every);
        }
        if (g_widescreen == 2) {
            CONF_Init();                /* harmless when the library has done it already */
            /* the console's own setting (Wii Settings > Screen > Widescreen; Dolphin: its SYSCONF, and its "Auto"
             * aspect ratio follows the same setting) */
            g_widescreen = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
            plat_log("widescreen: the Wii is set to %s", g_widescreen ? "16:9" : "4:3");
        }
    }
    {
        /* Dolphin has an IOS device of its own; a console does not. Under Dolphin on Windows a sleep of a few
         * milliseconds ends late by about one, which the idle waits (the sound chip's interrupt wakes them every
         * 5 ms) turn into a fifth of the emulation's speed: they wait on the clock there. A console sleeps. */
        extern int g_idle_spin_ms;
        s32 fd = IOS_Open("/dev/dolphin", 0);
        if (fd >= 0) {
            extern int g_dcbz_on;
            extern int g_dsp_on;
            IOS_Close(fd);
            g_idle_spin_ms = 20;
            g_dcbz_on = 0;              /* no cache to fill lines in, and the JIT pays for each dcbz (runtime/memfast.c) */
            /* The mixer runs on the chip by default (platform/wii/dsp_mix.c). Dolphin's DSP is either HLE, which
             * does not know that microcode, or LLE, which is slower than the CPU it would relieve; args.txt
             * "--dsp 1" turns it on here for a test. */
            g_dsp_on = 0;
        }
    }
    autopilot_load();
    audio_start();
    if (LWP_CreateThread(&g_log_thread, log_main, NULL, g_log_stack, sizeof g_log_stack, 20) < 0)
        g_log_thread = LWP_THREAD_NULL;     /* lines written where they are made */
    plat_log("data folder %s, MEM2 free %u KB", g_root,
             (unsigned)(((uint8_t *)SYS_GetArena2Hi() - (uint8_t *)SYS_GetArena2Lo()) / 1024));
    {
        extern void pmc_init(void);
        extern void pmc_lcbench_if(void);
        extern void pmc_pfbench_if(void);
        extern void pmc_stbench_if(void);
        pmc_init();                     /* the performance counters (platform/wii/pmc.c): off under an emulator */
        pmc_lcbench_if();               /* args.txt "--lcbench 1": the locked cache's DMA measured, then off again */
        pmc_pfbench_if();               /* args.txt "--pfbench 1": what a dcbt overlaps, what a code line costs */
        pmc_stbench_if();               /* args.txt "--stbench 1": what a store to a cold line costs, what a dcbz takes off */
    }
}

void plat_shutdown(void)
{
    int i;
    iop_sio2_flush();
    log_drain();
    for (i = 0; i < g_ntfs_count; i++)
        ntfsUnmount(g_ntfs_mounts[i].name, false);
    free(g_ntfs_mounts);
    g_ntfs_mounts = NULL;
    g_ntfs_count = 0;
    LWP_MutexLock(g_log_lock);
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
    LWP_MutexUnlock(g_log_lock);
}

/* HOME, RESET and the console's power button: flush the memory card and the
 * log, then leave through exit() (libogc's exit path returns to the loader, or
 * resets or powers off after RESET and the power button). NTFS stays mounted:
 * see below. */
static void plat_quit(int code)
{
    plat_log("exit: returning to the loader");
    plat_log_sync();
    iop_sio2_flush();
    /* Do not unmount NTFS or free its mounts here: the disc reader and the
     * log thread can still be in the middle of a read.  The loader resets
     * the USB device; NTFS_RECOVER handles a volume that was not unmounted. */
    AUDIO_StopDMA();
    g_rumble = 0;                       /* a motor left on keeps shaking after the game has gone */
    plat_pad_rumble(0, 0);
    plat_pad_rumble(1, 0);
    WPAD_Shutdown();
    exit(code);
}

/* ---- time ------------------------------------------------------------------------------------ */

/* ticks_to_microsecs (ticks * 4 / 243) without a 64-bit division (a library call here) or state: the high half of
 * ticks * 4 times ceil(2^64 / 243), which is exact while ticks * 4 < 2^57 (the error 110 * n stays below 2^64; checked
 * against the division on 4 * 10^8 random and 8 * 10^7 edge values; the time base would take 18 years to get there).
 * Nothing is kept between calls, so no interrupt is masked for it (the old one was incremental under IRQ_Disable). */
static inline uint64_t ticks_us(uint64_t t)
{
    const uint32_t m1 = 0x010DB20Au, m0 = 0x88F4695Au;
    uint64_t n = t << 2, p00, p01, p10, mid;
    uint32_t n1 = (uint32_t)(n >> 32), n0 = (uint32_t)n;
    p00 = (uint64_t)n0 * m0;
    p01 = (uint64_t)n0 * m1;
    p10 = (uint64_t)n1 * m0;
    mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    return (uint64_t)n1 * m1 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

int g_timescale = 1;            /* debugging (autopilot "timescale N"): emulated time N times slower */
static uint64_t g_ts_real, g_ts_out;
static int g_ts_cur = 1;

static uint64_t __attribute__((noinline)) plat_time_scaled(uint64_t us)
{
    uint32_t level = IRQ_Disable();
    if (g_timescale != g_ts_cur && g_timescale >= 1) {
        g_ts_out += (us - g_ts_real) / (uint64_t)g_ts_cur;
        g_ts_real = us;
        g_ts_cur = g_timescale;
    }
    us = g_ts_out + (us - g_ts_real) / (uint64_t)g_ts_cur;
    IRQ_Restore(level);
    return us;
}

uint64_t plat_time_us(void)
{
    uint64_t us = ticks_us(gettime() - g_t0);
    if (__builtin_expect(g_timescale == 1 && g_ts_cur == 1, 1))
        return g_ts_out + (us - g_ts_real);
    return plat_time_scaled(us);
}

/* (the phase timer reads the time base at every change of phase: the counters' share of the time since the last is the
 * phase that was running's: pmc.c) */
extern int g_pmc_phase;
void pmc_phase_account(int cat);
uint64_t plat_ticks(void)
{
    if (__builtin_expect(g_pmc_phase, 0) && g_pt_on)
        pmc_phase_account(g_pt_cur);
    return gettime();
}
const uint64_t plat_tick_hz = TB_TIMER_CLOCK * 1000ull;

void plat_sleep_us(uint32_t us)
{
    usleep(us);
}

/* ---- threads --------------------------------------------------------------------------------- */

/* The runtime lets one host thread at a time run guest code and hands over
 * with semaphores, so host threads are coroutines: they all run on the main
 * LWP thread and switch with green_switch (green.S). This is not bound by
 * libogc's small semaphore and thread pools (64 semaphores: the IOP alone
 * starts some 30 threads), and a switch costs a few dozen instructions.
 * Waiting for time to pass (plat_sleep_us) sleeps the whole LWP thread. */
void green_switch(void **save_sp, void *new_sp);
void green_start(void);
void green_main(Green *g);

static Green g_main_green;
static Green *g_green_cur = &g_main_green;
static Green *g_ready_head, *g_ready_tail;

static void ready_push(Green *g)
{
    g->next = NULL;
    if (g_ready_tail)
        g_ready_tail->next = g;
    else
        g_ready_head = g;
    g_ready_tail = g;
}

/* the current context is queued on a semaphore or finished: run another */
static void green_block(void)
{
    Green *prev = g_green_cur, *next = g_ready_head;
    if (prev->canary && (prev->canary[0] != 0xA5A5A5A5u || prev->canary[63] != 0xA5A5A5A5u))
        plat_fatal("host thread stack overflow (thread %d)", prev->stack_id);
    if (!next)
        plat_fatal("every host thread is waiting");
    g_ready_head = next->next;
    if (!g_ready_head)
        g_ready_tail = NULL;
    g_green_cur = next;
    green_switch(&prev->sp, next->sp);
}

void green_main(Green *g)
{
    g->fn(g->arg);
    plat_thread_exit();
}

PlatSem *plat_sem_create(int initial)
{
    PlatSem *s = calloc(1, sizeof *s);
    s->count = initial;
    return s;
}

void plat_sem_post(PlatSem *s)
{
    Green *g = s->head;
    if (!g) {
        s->count++;
        return;
    }
    s->head = g->next;
    if (!s->head)
        s->tail = NULL;
    ready_push(g);
}

void plat_sem_wait(PlatSem *s)
{
    Green *me = g_green_cur;
    if (s->count > 0) {
        s->count--;
        return;
    }
    me->next = NULL;
    if (s->tail)
        s->tail->next = me;
    else
        s->head = me;
    s->tail = me;
    green_block();
}

/* The EE FPU and the VUs round toward zero and treat denormals as zero: set
 * FPSCR RN = 01 (toward zero) and NI (non-IEEE mode, denormals flush to 0).
 * FPSCR belongs to the LWP thread, which every host thread shares. */
static void fpu_ps2_mode(void)
{
    union { double d; unsigned long long u; } f;
    __asm__ volatile("mffs %0" : "=f"(f.d));
    f.u = (f.u & ~3ull) | 1u | 4u;
    __asm__ volatile("mtfsf 0xFF, %0" : : "f"(f.d));
}

/* Thread stacks are filled with a pattern; plat_vblank reports how deep each
 * has been used, since an overflow on the Wii corrupts memory silently. */
#define STACK_FILL 0xA5
static struct { uint8_t *base; uint32_t size, reported; char name[24]; } g_stacks[64];
static int g_nstacks;

static void stack_report(void)
{
    int k, deepest = -1;
    uint32_t deepest_used = 0;
    for (k = 0; k < g_nstacks; k++) {
        uint32_t free = 0;
        while (free < g_stacks[k].size && g_stacks[k].base[free] == STACK_FILL)
            free++;
        if (g_stacks[k].size - free > deepest_used) {
            deepest_used = g_stacks[k].size - free;
            deepest = k;
        }
    }
    if (deepest >= 0)
        plat_log("stacks: %d host threads, deepest %s uses %u of %u KB", g_nstacks, g_stacks[deepest].name,
                 deepest_used / 1024, g_stacks[deepest].size / 1024);
    for (k = 0; k < g_nstacks; k++) {
        uint32_t free = 0, used;
        while (free < g_stacks[k].size && g_stacks[k].base[free] == STACK_FILL)
            free++;
        used = g_stacks[k].size - free;
        if (used > g_stacks[k].reported + 8 * 1024) {
            g_stacks[k].reported = used;
            plat_log("stack %s: %u of %u KB used", g_stacks[k].name, used / 1024, g_stacks[k].size / 1024);
            if (free < g_stacks[k].size / 8)
                plat_log("stack %s is nearly full", g_stacks[k].name);
        }
    }
}

void plat_vblank(uint32_t count)
{
    static int wd;
    if (!wd) {
        wd = 1;
        wd_start();
    }
    g_wd_beat = count;
    prof_poll();
    if (count % 600)
        return;
    stack_report();
}

PlatThread *plat_thread_create(void (*fn)(void *), void *arg, size_t stack_size, const char *name)
{
    PlatThread *t = malloc(sizeof *t);
    void *stack = mem2_alloc(stack_size);
    if (!stack)
        plat_fatal("out of MEM2 for a %u KB thread stack", (unsigned)(stack_size / 1024));
    memset(stack, STACK_FILL, stack_size);
    if (g_nstacks < 64) {
        g_stacks[g_nstacks].base = stack;
        g_stacks[g_nstacks].size = (uint32_t)stack_size;
        snprintf(g_stacks[g_nstacks].name, sizeof g_stacks[0].name, "%s", name);
        g_nstacks++;
    }
    /* a first frame that green_switch "returns" from into green_start, with
     * the context in r31 (frame layout in green.S) */
    {
        uint8_t *top = (uint8_t *)(((uintptr_t)stack + stack_size) & ~(uintptr_t)15) - 16;
        uint8_t *sp = top - 240;
        memset(sp, 0, 240 + 16);
        *(uint32_t *)(sp + 240 + 4) = (uint32_t)(uintptr_t)green_start;
        uint32_t sda;
        __asm__("mr %0, 13" : "=r"(sda));   /* r13: small data base, the same everywhere */
        *(uint32_t *)(sp + 12) = sda;
        *(uint32_t *)(sp + 12 + (31 - 13) * 4) = (uint32_t)(uintptr_t)&t->g;
        t->g.sp = sp;
        t->g.canary = (uint32_t *)stack;
        t->g.stack_id = g_nstacks - 1;
    }
    t->g.fn = fn;
    t->g.arg = arg;
    ready_push(&t->g);
    return t;
}

/* a periodic alarm runs from the decrementer interrupt, so it reaches guest
 * code that never yields */
static volatile int *g_tick_flag;
static syswd_t g_tick_alarm;

static volatile uint32_t g_tick_count;  /* status line */
static uint32_t g_tick_pc[16][2];
/* debugging: where the ticks since the last picture found the processor: pc, lr and six return
 * addresses up the stack (plat_tick_take; the renderer logs them for a frame with far more game
 * code than usual, work/spike.py names them) */
#define TICK_SAMPLES 64
static uint32_t g_tick_smp[2][TICK_SAMPLES][8];
static volatile uint32_t g_tick_nsmp;
static int g_tick_buf;

static void tick(syswd_t alarm, void *arg)
{
    (void)alarm;
    (void)arg;
    *g_tick_flag = 1;
    {
        /* debugging: where the tick found the processor (pc and lr of the last 16 ticks) */
        const uint32_t *ctx;
        __asm__ volatile("mfsprg %0, 2" : "=r"(ctx));
        g_tick_pc[g_tick_count & 15][0] = ctx[0];
        g_tick_pc[g_tick_count & 15][1] = ctx[3];
        if (g_tick_nsmp < TICK_SAMPLES) {
            uint32_t *st = g_tick_smp[g_tick_buf][g_tick_nsmp++], k = 2, f = ctx[7], up;
            st[0] = ctx[0];
            st[1] = ctx[3];
            while (k < 8 && !(f & 7) && ((f >= 0x80000000u && f < 0x81800000u) || (f >= 0x90000000u && f < 0x94000000u))) {
                up = *(const uint32_t *)f;
                if (up <= f || up - f > 0x100000u || (up >= 0x81800000u && up < 0x90000000u) || up >= 0x94000000u)
                    break;
                st[k++] = *(const uint32_t *)(up + 4);
                f = up;
            }
            while (k < 8)
                st[k++] = 0;
        }
    }
    g_tick_count++;
}

/* debugging: where the sound output's interrupt found the processor when it had nothing to play
     * (pc, lr and six return addresses, as the ticks' samples; not the first 20 seconds, when there is
     * no sound yet) */
static uint32_t g_aq_dry[12][8], g_aq_dry_n, g_aq_total;

static void aq_dry_note(void)
{
    const uint32_t *ctx;
    uint32_t *st, k = 2, f, up;
    if (++g_aq_total < 8 || g_aq_blocks_all < 2000u || g_aq_dry_n >= 12)
        return;
    __asm__ volatile("mfsprg %0, 2" : "=r"(ctx));
    st = g_aq_dry[g_aq_dry_n++];
    st[0] = ctx[0];
    st[1] = ctx[3];
    f = ctx[7];
    while (k < 8 && !(f & 7) && ((f >= 0x80000000u && f < 0x81800000u) || (f >= 0x90000000u && f < 0x94000000u))) {
        up = *(const uint32_t *)f;
        if (up <= f || up - f > 0x100000u || (up >= 0x81800000u && up < 0x90000000u) || up >= 0x94000000u)
            break;
        st[k++] = *(const uint32_t *)(up + 4);
        f = up;
    }
    while (k < 8)
        st[k++] = 0;
}

/* those samples (eight words each) since the last call */
uint32_t plat_audio_dry(uint32_t out[12][8])
{
    uint32_t level = IRQ_Disable(), n = g_aq_dry_n;
    memcpy(out, g_aq_dry, sizeof g_aq_dry);
    g_aq_dry_n = 0;
    IRQ_Restore(level);
    return n;
}

/* the samples since the last call (eight words each), good until the next call */
uint32_t plat_tick_take(const uint32_t **samples)
{
    uint32_t level = IRQ_Disable(), n = g_tick_nsmp;
    *samples = g_tick_smp[g_tick_buf][0];
    g_tick_buf ^= 1;
    g_tick_nsmp = 0;
    IRQ_Restore(level);
    return n;
}

void plat_tick_trace(char *out, size_t size)
{
    uint32_t k, n = g_tick_count;
    size_t m = 0;
    out[0] = 0;
    for (k = n - 12; k != n && m + 20 < size; k++)
        m += (size_t)snprintf(out + m, size - m, " %08X/%08X", (unsigned)g_tick_pc[k & 15][0], (unsigned)g_tick_pc[k & 15][1]);
}

void plat_start_ticker(volatile int *flag, uint32_t period_us)
{
    struct timespec ts;
    g_tick_flag = flag;
    ts.tv_sec = 0;
    ts.tv_nsec = (long)period_us * 1000;
    SYS_CreateAlarm(&g_tick_alarm);
    SYS_SetPeriodicAlarm(g_tick_alarm, &ts, &ts, tick, NULL);
}

void plat_thread_exit(void)
{
    green_block();                  /* never resumed: nothing queues it again */
    plat_fatal("finished host thread resumed");
}

/* ---- memory ------------------------------------------------------------------------------------ */

void *plat_alloc_guest_ram(size_t size)
{
    (void)size;
    return (void *)PS2_RAM_FIXED_BASE;
}

/* status line: free memory (the heap's free blocks, and what the MEM1 and
 * MEM2 arenas still have for it and mem2_alloc to grow into) */
void plat_mem_report(char *out, size_t size)
{
    struct mallinfo mi = mallinfo();
    snprintf(out, size, "heap %u KB in use, %u KB free in it; arenas MEM1 %u KB, MEM2 %u KB", (unsigned)(mi.uordblks / 1024),
             (unsigned)(mi.fordblks / 1024),
             (unsigned)(((uint8_t *)SYS_GetArena1Hi() - (uint8_t *)SYS_GetArena1Lo()) / 1024),
             (unsigned)(((uint8_t *)SYS_GetArena2Hi() - (uint8_t *)SYS_GetArena2Lo()) / 1024));
}

uint32_t plat_mem2_free(void)
{
    return (uint32_t)((uint8_t *)SYS_GetArena2Hi() - (uint8_t *)SYS_GetArena2Lo());
}

void *plat_alloc(size_t size)
{
    /* big buffers (IOP RAM, sound RAM, GS memory) go to MEM2 */
    if (size >= 256 * 1024) {
        void *p = mem2_alloc(size);
        if (p)
            return p;
    }
    return malloc(size);
}

void plat_free(void *p)
{
    uintptr_t a = (uintptr_t)p;
    if (a >= 0x90000000u)
        return;                    /* MEM2 buffers live for the whole run */
    free(p);
}

/* ---- files --------------------------------------------------------------------------------------- */

static PlatFile *open_path(const char *full, int handles)
{
    FILE *f = fopen(full, "rb");
    PlatFile *pf;
    int k;
    if (!f)
        return NULL;
    pf = calloc(1, sizeof *pf);
    pf->f[0] = f;
    fseek(f, 0, SEEK_END);
    pf->size = ftello(f);
    pf->pos[0] = pf->size;
    pf->n = 1;
    /* the disc image is read a whole block at a time, straight into the block cache */
    setvbuf(f, NULL, handles > 1 ? _IONBF : _IOFBF, handles > 1 ? 0 : 32 * 1024);
    for (k = 1; k < handles && k < PF_HANDLES; k++) {
        if (!(pf->f[k] = fopen(full, "rb")))
            break;
        setvbuf(pf->f[k], NULL, _IONBF, 0);
        pf->pos[k] = 0;
        pf->n++;
    }
    return pf;
}

PlatFile *plat_open(const char *path)
{
    char full[256];
    snprintf(full, sizeof full, "%s/disc/%s", g_root, path);
    return open_path(full, 1);
}

PlatFile *plat_open_disc_image(void)
{
    char full[128];
    PlatFile *f;
    snprintf(full, sizeof full, "%s/burnout3.iso", g_root);
    f = open_path(full, PF_HANDLES);
    if (f)
        f->cached = 1;
    if (!f)
        plat_fatal("cannot open %s", full);
    raw_setup(f, full);
    return f;
}

void plat_close(PlatFile *f)
{
    int k;
    if (f) {
        for (k = 0; k < f->n; k++)
            fclose(f->f[k]);
        free(f);
    }
}

int64_t plat_size(PlatFile *f) { return f->size; }

#define DC_BLOCK 32768

/* ---- the disc image by its sectors ---------------------------------------------------------------
 * A seek in a file walks its cluster chain: forward from where the handle is, back from the start of
 * the file. A sound starting somewhere else in the 1.7 GB image cost the reader thread 4 to 11 ms of
 * processor each time (some 50000 table entries), in the frame it fell in: the frames with twice the
 * usual "game code" shortly before a collision. So the image's place on the card is read once, a
 * list of runs of clusters, and its blocks are read by sector, without the file system: any block
 * costs the same.
 *
 * The file system and these reads share the card: the SD interface the volume is mounted on is one
 * of our own (sd_remount) that takes g_sd_lock around every transfer, as raw_sectors does. The
 * layout is worked out here for FAT16 and FAT32 with 512 byte sectors and names of the 8.3 kind
 * (burnout3/burnout3.iso is one), and three blocks read both ways have to agree; anything else, a
 * USB drive too, and the image is read through the file system as before (file_read). */
#include <sdcard/wiisd_io.h>

static mutex_t g_sd_lock;
static DISC_INTERFACE g_sd_io;          /* the SD interface behind the lock */
static uint32_t g_sd_wrapped;           /* transfers the file system made through it */

static bool sdw_read(sec_t s, sec_t n, void *b)
{
    bool r;
    LWP_MutexLock(g_sd_lock);
    g_sd_wrapped++;
    r = __io_wiisd.readSectors(s, n, b);
    LWP_MutexUnlock(g_sd_lock);
    return r;
}

static bool sdw_write(sec_t s, sec_t n, const void *b)
{
    bool r;
    LWP_MutexLock(g_sd_lock);
    g_sd_wrapped++;
    r = __io_wiisd.writeSectors(s, n, b);
    LWP_MutexUnlock(g_sd_lock);
    return r;
}

/* after fatInitDefault: the card's volume mounted again on the interface with the lock; 0: it is as
 * it was (no card, or it would not), and the image is not read by sector */
static int sd_remount(void)
{
    struct stat st;
    if (LWP_MutexInit(&g_sd_lock, false) < 0 || stat("sd:/", &st))
        return 0;
    g_sd_io = __io_wiisd;
    g_sd_io.readSectors = sdw_read;
    g_sd_io.writeSectors = sdw_write;
    fatUnmount("sd");
    if (fatMountSimple("sd", &g_sd_io) && !stat("sd:/", &st))
        return 1;
    /* as it was: on the card's own interface, or by the library's whole start again */
    fatUnmount("sd");
    if (!fatMountSimple("sd", &__io_wiisd) || stat("sd:/", &st))
        fatInitDefault();
    return 0;
}

typedef struct RawExt { uint32_t first, start, count; } RawExt;     /* clusters: of the file, on the card, how many */
#define RAW_EXTS 4096
static struct {
    int on;
    uint32_t data;              /* the card's sector where cluster 2 begins */
    uint32_t spc;               /* sectors a cluster */
    uint32_t fat, fat_secs;     /* the table: its first sector on the card, its length */
    uint32_t root, root_secs;   /* FAT16: the root directory's sectors; FAT32: its first cluster, 0 */
    int fat32;
    uint32_t next;
    RawExt *ext;
    int64_t size;
    uint8_t *buf;               /* 64 sectors, while the layout is read */
    uint32_t win;               /* the table's sectors in it (first; ~0: none) */
} g_raw;
static uint32_t g_raw_reads;            /* status: blocks read by sector */

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static int raw_sectors(uint32_t sector, uint32_t n, void *buf)
{
    bool r;
    LWP_MutexLock(g_sd_lock);
    r = __io_wiisd.readSectors(sector, n, buf);
    LWP_MutexUnlock(g_sd_lock);
    return r ? 1 : 0;
}

static int raw_is_vbr(const uint8_t *s)
{
    return s[510] == 0x55 && s[511] == 0xAA && (s[0] == 0xEB || s[0] == 0xE9) && le16(s + 11) == 512 && s[13] &&
           !(s[13] & (s[13] - 1)) && (s[16] == 1 || s[16] == 2) && memcmp(s + 3, "EXFAT", 5) != 0;
}

/* the volume's layout from its boot sector (the card's first, or the first partition's) */
static int raw_volume(void)
{
    uint8_t *s = g_raw.buf;
    uint32_t part = 0, rsvd, nfat, rootent, total, fatsz, first, k;
    if (!raw_sectors(0, 1, s))
        return 0;
    if (!raw_is_vbr(s)) {
        for (k = 0; k < 4; k++) {
            part = le32(s + 446 + 16 * k + 8);
            if (s[446 + 16 * k + 4] && part)
                break;
        }
        if (k == 4 || !raw_sectors(part, 1, s) || !raw_is_vbr(s))
            return 0;
    }
    g_raw.spc = s[13];
    rsvd = le16(s + 14);
    nfat = s[16];
    rootent = le16(s + 17);
    total = le16(s + 19) ? le16(s + 19) : le32(s + 32);
    fatsz = le16(s + 22) ? le16(s + 22) : le32(s + 36);
    g_raw.root_secs = (rootent * 32 + 511) / 512;
    first = rsvd + nfat * fatsz + g_raw.root_secs;
    if (!fatsz || total <= first)
        return 0;
    k = (total - first) / g_raw.spc;            /* clusters: what kind of table */
    if (k < 4085)
        return 0;
    g_raw.fat32 = k >= 65525;
    g_raw.fat = part + rsvd;
    if (g_raw.fat32 && (s[40] & 0x80))
        g_raw.fat += (s[40] & 15) * fatsz;      /* one table in use, not the first */
    g_raw.fat_secs = fatsz;
    g_raw.data = part + first;
    g_raw.root = g_raw.fat32 ? le32(s + 44) : part + rsvd + nfat * fatsz;
    g_raw.win = ~0u;
    return 1;
}

/* the cluster after this one; 0 at the end of the chain or where the table makes no sense */
static uint32_t raw_next(uint32_t clus)
{
    const uint32_t off = clus * (g_raw.fat32 ? 4u : 2u), sec = off / 512;
    uint32_t v;
    if (sec >= g_raw.fat_secs)
        return 0;
    if (g_raw.win == ~0u || sec < g_raw.win || sec >= g_raw.win + 64) {
        const uint32_t w = sec & ~63u, n = g_raw.fat_secs - w < 64 ? g_raw.fat_secs - w : 64;
        g_raw.win = ~0u;
        if (!raw_sectors(g_raw.fat + w, n, g_raw.buf))
            return 0;
        g_raw.win = w;
    }
    v = g_raw.fat32 ? le32(g_raw.buf + (off - g_raw.win * 512)) & 0x0FFFFFFFu : le16(g_raw.buf + (off - g_raw.win * 512));
    return v < 2 || v >= (g_raw.fat32 ? 0x0FFFFFF0u : 0xFFF0u) ? 0 : v;
}

/* a name as a directory keeps it when it needs no long one: 8 and 3 capitals, padded */
static int raw_name83(const char *name, size_t len, char out[11])
{
    size_t k, dot = len, n = 0;
    for (k = 0; k < len; k++)
        if (name[k] == '.')
            dot = k;
    if (!dot || dot > 8 || (dot < len && (len - dot - 1 > 3 || len - dot - 1 == 0)))
        return 0;
    memset(out, ' ', 11);
    for (k = 0; k < len; k++) {
        char ch = name[k];
        if (k == dot) {
            n = 8;
            continue;
        }
        if (ch == '.' || ch == ' ' || ch == '+' || ch == ',' || ch == ';' || ch == '=' || ch == '[' || ch == ']' || ch < 0x21)
            return 0;
        out[n++] = ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
    }
    return 1;
}

/* a directory's entry by its 8.3 name: the first cluster, the size, whether it is a directory.
 * dir 0: the root (a FAT16 volume's lies in sectors of its own) */
static int raw_find(uint32_t dir, const char name[11], uint32_t *clus, uint32_t *size, int *isdir)
{
    uint8_t *e = memalign(32, 512 * 64);
    uint32_t sec, left, k, n, found = 0, guard = 0;
    int fixed = !dir && !g_raw.fat32;
    if (!e)
        return 0;
    if (!dir && g_raw.fat32)
        dir = g_raw.root;
    sec = fixed ? g_raw.root : g_raw.data + (dir - 2) * g_raw.spc;
    left = fixed ? g_raw.root_secs : g_raw.spc;
    while (!found && guard++ < 65536) {
        n = left < 64 ? left : 64;
        if (!n || !raw_sectors(sec, n, e))
            break;
        for (k = 0; k < n * 512; k += 32) {
            const uint8_t *d = e + k;
            if (!d[0]) {
                guard = 65536;          /* the directory ends here */
                break;
            }
            if (d[0] == 0xE5 || (d[11] & 0x0F) == 0x0F || (d[11] & 0x08))
                continue;
            if (!memcmp(d, name, 11)) {
                *clus = le16(d + 20) << 16 | le16(d + 26);
                *size = le32(d + 28);
                *isdir = (d[11] & 0x10) != 0;
                found = 1;
                break;
            }
        }
        sec += n;
        left -= n;
        if (!left && !fixed && !found && guard < 65536) {
            dir = raw_next(dir);
            if (!dir)
                break;
            sec = g_raw.data + (dir - 2) * g_raw.spc;
            left = g_raw.spc;
        }
    }
    free(e);
    return (int)found;
}

static int64_t raw_read(void *buf, int64_t size, int64_t offset)
{
    const int64_t cb = (int64_t)g_raw.spc * 512;
    int64_t done = 0;
    if (offset >= g_raw.size)
        return 0;
    if (size > g_raw.size - offset)
        size = g_raw.size - offset;
    if (size & 511)
        return -1;
    while (done < size) {
        const int64_t at = offset + done;
        const uint32_t fc = (uint32_t)(at / cb), in = (uint32_t)(at % cb) / 512;
        uint32_t lo = 0, hi = g_raw.next, n, room;
        const RawExt *e;
        while (hi - lo > 1) {
            const uint32_t mid = (lo + hi) / 2;
            if (g_raw.ext[mid].first <= fc)
                lo = mid;
            else
                hi = mid;
        }
        e = &g_raw.ext[lo];
        if (fc < e->first || fc >= e->first + e->count)
            return -1;
        room = (e->first + e->count - fc) * g_raw.spc - in;
        n = (uint32_t)((size - done) / 512);
        if (n > room)
            n = room;
        if (!raw_sectors(g_raw.data + (e->start + (fc - e->first) - 2) * g_raw.spc + in, n, (uint8_t *)buf + done))
            return -1;
        done += (int64_t)n * 512;
    }
    g_raw_reads++;
    return done;
}

static int64_t file_read(PlatFile *f, void *buf, int64_t size, int64_t offset);

/* the image (open, `path` its name from the device on) is looked up on the card and its runs listed */
static void raw_setup(PlatFile *f, const char *path)
{
    static uint8_t a[DC_BLOCK] __attribute__((aligned(32))), b[DC_BLOCK] __attribute__((aligned(32)));
    uint32_t dir = 0, clus = 0, size = 0, nclus, k, wrapped = g_sd_wrapped;
    int isdir = 1;
    const char *p = path;
    const char *why = "the card's layout";
    if (!g_sd_mounted || strncmp(path, "sd:/", 4))
        return;
    g_raw.buf = memalign(32, 512 * 64);
    g_raw.ext = malloc(RAW_EXTS * sizeof(RawExt));
    if (!g_raw.buf || !g_raw.ext || !raw_volume())
        goto out;
    why = "the image's directory entry";
    for (p = path + 4; *p; ) {
        const char *q = strchr(p, '/');
        const size_t len = q ? (size_t)(q - p) : strlen(p);
        char name[11];
        if (!isdir || !raw_name83(p, len, name) || !raw_find(dir, name, &clus, &size, &isdir))
            goto out;
        dir = clus;
        p += len + (q ? 1 : 0);
    }
    if (isdir || (int64_t)size != f->size || clus < 2)
        goto out;
    why = "the image's cluster chain";
    nclus = (uint32_t)(((int64_t)size + (int64_t)g_raw.spc * 512 - 1) / ((int64_t)g_raw.spc * 512));
    for (k = 0; k < nclus; k++) {
        RawExt *e = g_raw.next ? &g_raw.ext[g_raw.next - 1] : NULL;
        if (!clus)
            goto out;
        if (e && e->start + e->count == clus) {
            e->count++;
        } else {
            if (g_raw.next == RAW_EXTS)
                goto out;               /* in too many pieces */
            e = &g_raw.ext[g_raw.next++];
            e->first = k;
            e->start = clus;
            e->count = 1;
        }
        clus = k + 1 < nclus ? raw_next(clus) : 0;
    }
    g_raw.size = f->size;
    why = "a block read both ways";
    for (k = 0; k < 3; k++) {
        const int64_t blk = k == 0 ? 0 : k == 1 ? f->size / DC_BLOCK / 2 : (f->size - 1) / DC_BLOCK;
        const int64_t ga = file_read(f, a, DC_BLOCK, blk * DC_BLOCK), gb = raw_read(b, DC_BLOCK, blk * DC_BLOCK);
        if (ga <= 0 || ga != gb || memcmp(a, b, (size_t)ga))
            goto out;
    }
    if (g_sd_wrapped == wrapped)
        goto out;                       /* the file system is not behind the lock after all */
    {
        RawExt *e = realloc(g_raw.ext, g_raw.next * sizeof(RawExt));
        if (e)
            g_raw.ext = e;
    }
    g_raw.on = 1;
    plat_log("disc image read by sector: %u runs of clusters (%u KB each) on a FAT%d volume", (unsigned)g_raw.next,
             (unsigned)(g_raw.spc / 2), g_raw.fat32 ? 32 : 16);
out:
    free(g_raw.buf);
    g_raw.buf = NULL;
    if (!g_raw.on) {
        free(g_raw.ext);
        g_raw.ext = NULL;
        plat_log("disc image read through the file system (%s did not work out)", why);
    }
}


/* The disc image's recently read 32 KB blocks: the game loops audio from
 * the disc (192 KB over and over at the menus), and every jump back was a
 * seek walking the cluster chain from the start of the image.
 *
 * A reader thread (an LWP thread of its own; the game's threads are
 * coroutines on the main one) fetches a block while the game goes on:
 * plat_read_guest. One block at a time; the cache's books are kept by the
 * main thread alone (the reader writes the block's bytes and says how
 * many), and a read that does not wait first lets the reader finish.
 * The reader runs above the main thread (below it, a game thread that
 * polls for its data without sleeping kept it from ever running: a hang
 * at a loading screen), so what it costs comes out of the frame it falls
 * in: little by sector (raw_read), but through the file system a seek's
 * walk along the cluster chain, up to 11 ms. */
#define IO_PRIO 80                      /* the main thread: 64 */
#define DC_N 12
static struct { int64_t blk; uint32_t stamp, len; uint8_t *data; int ahead; } g_dc[DC_N];
/* Blocks read ahead that nothing then read: the block after the end of a sound the game loops (the
 * menus' 192 KB) was fetched again every three seconds, each time its slot had gone to the music.
 * The last eight such are not read ahead again. */
static int64_t g_dc_unused[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static uint32_t g_dc_nunused;
static uint8_t g_dc_mem[DC_N][DC_BLOCK] __attribute__((aligned(32)));   /* MEM1: MEM2 is full */
static uint32_t g_dc_clock;

static lwp_t g_io_thread = LWP_THREAD_NULL;
static sem_t g_io_go;
static volatile int g_io_state;         /* 0: idle, 1: the reader is at it, 2: read, not in the books yet */
static struct { PlatFile *f; int64_t blk, got; int slot; } g_io;
static uint8_t g_io_stack[32 * 1024] __attribute__((aligned(32)));
static int64_t g_io_failed = -1;        /* a block the reader could not get: read the plain way, with its error */
static uint32_t g_io_reads, g_io_ahead, g_io_waits;    /* status: blocks read by the thread, of them ahead; waits */
static uint32_t g_io_seek_kb;           /* status: how far the seeks walked the cluster chain, KB of file */
static struct { uint32_t blk; uint16_t mb; uint8_t handle; } g_io_tr[16];   /* debugging: the last reads */
static uint32_t g_io_ntr;
extern volatile int g_host_event, g_poll_request;

static void *io_main(void *arg)
{
    (void)arg;
    for (;;) {
        LWP_SemWait(g_io_go);
        g_io.got = file_read(g_io.f, g_dc[g_io.slot].data, DC_BLOCK, g_io.blk * DC_BLOCK);
        g_io_state = 2;
        g_host_event = 1;
        g_poll_request = 1;
    }
    return NULL;
}

/* what the reader fetched, into the books */
static void io_settle(void)
{
    if (g_io_state != 2)
        return;
    if (g_io.got > 0) {
        g_dc[g_io.slot].blk = g_io.blk;
        g_dc[g_io.slot].len = (uint32_t)g_io.got;
    } else {
        g_dc[g_io.slot].blk = -1;
        g_io_failed = g_io.blk;
    }
    g_io_state = 0;
}

/* the reader is let finish (a read that cannot wait as a guest thread) */
static void io_finish(void)
{
    while (g_io_state == 1)
        usleep(200);
    io_settle();
}

static int dc_find(int64_t blk)
{
    int k;
    for (k = 0; k < DC_N; k++)
        if (g_dc[k].data && g_dc[k].blk == blk)
            return k;
    return -1;
}

static int dc_victim(void)
{
    int k, victim = 0;
    for (k = 0; k < DC_N; k++) {
        if (!g_dc[k].data) {
            g_dc[k].data = g_dc_mem[k];
            return k;
        }
        if (g_dc[k].stamp < g_dc[victim].stamp)
            victim = k;
    }
    if (g_dc[victim].ahead && g_dc[victim].blk >= 0)
        g_dc_unused[g_dc_nunused++ & 7] = g_dc[victim].blk;
    g_dc[victim].ahead = 0;
    return victim;
}

static int dc_unused(int64_t blk, int forget)
{
    int k;
    for (k = 0; k < 8; k++)
        if (g_dc_unused[k] == blk) {
            if (forget)
                g_dc_unused[k] = -1;
            return 1;
        }
    return 0;
}

/* the reader starts on a block (it is idle and settled) */
static int io_start(PlatFile *f, int64_t blk)
{
    int slot;
    if (g_io_thread == LWP_THREAD_NULL) {
        if (LWP_SemInit(&g_io_go, 0, 1) < 0 ||
            LWP_CreateThread(&g_io_thread, io_main, NULL, g_io_stack, sizeof g_io_stack, IO_PRIO) < 0) {
            g_io_thread = LWP_THREAD_NULL;
            return 0;
        }
    }
    slot = dc_victim();
    g_dc[slot].blk = -1;
    g_dc[slot].stamp = ++g_dc_clock;
    g_io.f = f;
    g_io.blk = blk;
    g_io.slot = slot;
    g_io_state = 1;
    g_io_reads++;
    LWP_SemPost(g_io_go);
    return 1;
}

static uint64_t g_io_held;              /* status: ticks the game stood still for reads that do not wait */

static const uint8_t *dc_block(PlatFile *f, int64_t blk, uint32_t *len)
{
    int k, victim;
    int64_t got;
    uint64_t t0 = gettime();
    io_finish();
    if ((k = dc_find(blk)) >= 0) {
        g_dc[k].stamp = ++g_dc_clock;
        g_dc[k].ahead = 0;
        *len = g_dc[k].len;
        g_io_held += gettime() - t0;
        return g_dc[k].data;
    }
    victim = dc_victim();
    got = file_read(f, g_dc[victim].data, DC_BLOCK, blk * DC_BLOCK);
    g_io_held += gettime() - t0;
    if (got <= 0) {
        g_dc[victim].blk = -1;
        return NULL;
    }
    g_dc[victim].blk = blk;
    g_dc[victim].len = (uint32_t)got;
    g_dc[victim].stamp = ++g_dc_clock;
    *len = (uint32_t)got;
    return g_dc[victim].data;
}

void plat_io_report(char *out, size_t size)
{
    snprintf(out, size, "%u blocks read by the reader thread (%u of them ahead), %u waits for it; %u read by sector, "
             "seeks over %u MB of the image for the others; the game held %u ms by reads that do not wait",
             (unsigned)g_io_reads, (unsigned)g_io_ahead, (unsigned)g_io_waits, (unsigned)g_raw_reads,
             (unsigned)(g_io_seek_kb >> 10), (unsigned)(ticks_to_microsecs(g_io_held) / 1000));
    g_raw_reads = 0;
    {
        /* debugging: the last reads, block (32 KB), handle and the walk in MB */
        size_t m = strlen(out);
        uint32_t k;
        if (g_io_seek_kb >> 10 && m + 30 < size) {
            m += (size_t)snprintf(out + m, size - m, "; last reads:");
            for (k = g_io_ntr > 16 ? g_io_ntr - 16 : 0; k != g_io_ntr && m + 24 < size; k++)
                m += (size_t)snprintf(out + m, size - m, " %u/%u/%u", (unsigned)g_io_tr[k & 15].blk,
                                      (unsigned)g_io_tr[k & 15].handle, (unsigned)g_io_tr[k & 15].mb);
        }
    }
    g_io_reads = g_io_ahead = g_io_waits = g_io_seek_kb = 0;
    g_io_held = 0;
    g_tick_count = 0;
}

int64_t plat_read_guest(PlatFile *f, void *buf, int64_t size, int64_t offset, void (*wait)(void))
{
    int64_t done = 0, next;
    if (!f->cached || !wait)
        return plat_read(f, buf, size, offset);
    while (done < size) {
        int64_t at = offset + done, blk = at / DC_BLOCK;
        uint32_t off = (uint32_t)(at % DC_BLOCK), len, n;
        int k;
        io_settle();
        k = dc_find(blk);
        if (k < 0) {
            dc_unused(blk, 1);
            if (g_io_failed == blk || (g_io_state == 0 && !io_start(f, blk))) {
                g_io_failed = -1;
                return plat_read(f, buf, size, offset);     /* no reader thread, or no such block */
            }
            g_io_waits++;
            wait();
            continue;
        }
        g_dc[k].stamp = ++g_dc_clock;
        g_dc[k].ahead = 0;
        len = g_dc[k].len;
        if (off >= len)
            break;
        n = len - off < size - done ? len - off : (uint32_t)(size - done);
        memcpy((uint8_t *)buf + done, g_dc[k].data + off, n);
        done += n;
        if (len < DC_BLOCK)
            break;                      /* the end of the file */
    }
    /* a stream reads on from here: its next block, while the game has other things to do */
    next = (offset + done + DC_BLOCK - 1) / DC_BLOCK;
    io_settle();
    if (g_io_state == 0 && next * DC_BLOCK < f->size && dc_find(next) < 0 && !dc_unused(next, 0) && io_start(f, next)) {
        g_dc[g_io.slot].ahead = 1;
        g_io_ahead++;
    }
    return done;
}

int64_t plat_read(PlatFile *f, void *buf, int64_t size, int64_t offset)
{
    int64_t done = 0;
    if (!f->cached)
        return file_read(f, buf, size, offset);
    while (done < size) {
        int64_t at = offset + done, blk = at / DC_BLOCK;
        uint32_t off = (uint32_t)(at % DC_BLOCK), len, n;
        const uint8_t *b = dc_block(f, blk, &len);
        if (!b)
            return done ? done : file_read(f, buf, size, offset);
        if (off >= len)
            break;
        n = len - off < size - done ? len - off : (uint32_t)(size - done);
        memcpy((uint8_t *)buf + done, b + off, n);
        done += n;
        if (len < DC_BLOCK)
            break;                      /* the end of the file */
    }
    return done;
}

static int64_t file_read(PlatFile *f, void *buf, int64_t size, int64_t offset)
{
    static uint32_t clock;
    int k, best = -1;
    int64_t got, cost = 0;
    if (f->cached && g_raw.on && !((uintptr_t)buf & 31) && !(offset & 511)) {
        got = raw_read(buf, size, offset);
        if (got >= 0)
            return got;
    }
    /* A seek forward walks the cluster chain from where the handle is, a seek back from the start of
     * the file: the handle at the offset (a stream reading on), else the one with the shortest walk
     * of those the last two reads did not use (they are other streams' as likely as not). */
    for (k = 0; k < f->n; k++) {
        const int64_t c = f->pos[k] >= 0 && f->pos[k] <= offset ? offset - f->pos[k] : offset;
        if (c && f->n > 2 && clock - f->used[k] < 2 && f->used[k])
            continue;
        if (best < 0 || c < cost) {
            best = k;
            cost = c;
        }
    }
    if (best < 0)
        best = 0;
    g_io_seek_kb += (uint32_t)(cost >> 10);
    g_io_tr[g_io_ntr & 15].blk = (uint32_t)(offset / DC_BLOCK);
    g_io_tr[g_io_ntr & 15].mb = (uint16_t)(cost >> 20);
    g_io_tr[g_io_ntr++ & 15].handle = (uint8_t)best;
    f->used[best] = ++clock;
    if (f->pos[best] != offset && fseeko(f->f[best], offset, SEEK_SET) != 0) {
        f->pos[best] = -1;
        return -1;
    }
    got = (int64_t)fread(buf, 1, (size_t)size, f->f[best]);
    f->pos[best] = got > 0 ? offset + got : -1;
    return got;
}

const char *plat_save_dir(void) { return g_save_dir; }

/* ---- video and input -------------------------------------------------------------------------------- */

void plat_present_rgba(const uint32_t *pixels, int width, int height, int stride)
{
    (void)pixels; (void)width; (void)height; (void)stride;
}

/* DualShock 2 button bits (0 = pressed in the PS2 report; set here as 1 = pressed) */
enum {
    DS_L2 = 1 << 0, DS_R2 = 1 << 1, DS_L1 = 1 << 2, DS_R1 = 1 << 3,
    DS_TRIANGLE = 1 << 4, DS_CIRCLE = 1 << 5, DS_CROSS = 1 << 6, DS_SQUARE = 1 << 7,
    DS_SELECT = 1 << 8, DS_L3 = 1 << 9, DS_R3 = 1 << 10, DS_START = 1 << 11,
    DS_UP = 1 << 12, DS_RIGHT = 1 << 13, DS_DOWN = 1 << 14, DS_LEFT = 1 << 15,
};

/* The game's vibration (runtime/iop_sio2.c pad_vibrate: 0 to 255) to the GameCube pad's motor and the Wii Remote's, which
 * are each on or off. A strength in between is the share of the polls the motor is on for (the error carried from one poll
 * to the next: 128 alternates on and off), which the motor's inertia turns into about that much shaking. Only a change is
 * sent. */
void plat_pad_rumble(int port, unsigned strength)
{
    static unsigned acc[2], was[2], logged;
    static uint8_t on[2];
    unsigned now = 0;
    u32 type;
    if (port < 0 || port > 1)
        return;
    if (!g_rumble)
        strength = 0;
    if (!strength != !was[port] && logged < 24) {
        logged++;
        plat_log("rumble: port %d %s (strength %u)", port + 1, strength ? "starts" : "stops", strength);
    }
    was[port] = strength;
    if (strength) {
        acc[port] += strength;
        now = acc[port] >= 255;
        if (now)
            acc[port] -= 255;
    } else
        acc[port] = 0;
    if (now == on[port])
        return;
    on[port] = (uint8_t)now;
    if (g_pad_mask & (1u << port))
        PAD_ControlMotor(port, now ? PAD_MOTOR_RUMBLE : strength ? PAD_MOTOR_STOP : PAD_MOTOR_STOP_HARD);
    if (WPAD_Probe(port, &type) == WPAD_ERR_NONE)
        WPAD_Rumble(port, (int)now);
}

static uint8_t axis(int v, int range)
{
    int x = 128 + v * 127 / range;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* Port 0 takes the GameCube pad in port 1 and the first Wii Remote (Classic
 * Controller, or Nunchuk + Remote held upright); port 1 (the second player)
 * the second pad and Remote, and is plugged in only while one of them is (or
 * the autopilot has "p2" presses). Burnout's driving controls fall on the
 * face buttons: Cross accelerate, Square brake, Circle boost. The default
 * profile puts Cross on A and Circle on B (select and back on the console's
 * own labels); args.txt "--pad 0" keeps the PS2 positions instead. */
void plat_read_pad(int port, PlatPad *out)
{
    static const int8_t pressure_index[16] = {10, 11, 8, 9, 4, 5, 6, 7, -1, -1, -1, -1, 2, 0, 3, 1};
    uint32_t b = 0, pads;
    int k;
    WPADData *w;
    memset(out, 0, sizeof *out);
    out->rx = out->ry = out->lx = out->ly = 0x80;
    out->buttons = 0xFFFF;
    if (port > 1)
        return;
    pads = PAD_ScanPads();
    g_pad_mask = pads;
    WPAD_ScanPads();
    {
        /* The console's reset and power buttons. This is the game's own
         * thread context (SYS_MainLoop is not safe from an interrupt). */
        static uint64_t last_check;
        uint64_t now = plat_time_us();
        if (now - last_check > 250000) {
            last_check = now;
            if (!SYS_MainLoop()) {
                plat_log("exit: reset or power button");
                plat_quit(0);
            }
        }
    }
    if (WPAD_ButtonsHeld(0) & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) {
        plat_log("exit: HOME button");
        plat_quit(0);
    }
    w = WPAD_Data(port);
    out->connected = port == 0 || (pads & 2) || (w && w->err == WPAD_ERR_NONE) || g_auto_p2;
    if (!out->connected)
        return;
    {
        u16 g = PAD_ButtonsHeld(port);
        if (g_pad_profile) {
            if (g & PAD_BUTTON_A) b |= DS_CROSS;
            if (g & PAD_BUTTON_B) b |= DS_CIRCLE;
            if (g & PAD_BUTTON_X) b |= DS_SQUARE;
            if (g & PAD_BUTTON_Y) b |= DS_TRIANGLE;
        } else {
            if (g & PAD_BUTTON_A) b |= DS_CROSS;
            if (g & PAD_BUTTON_B) b |= DS_SQUARE;
            if (g & PAD_BUTTON_X) b |= DS_CIRCLE;
            if (g & PAD_BUTTON_Y) b |= DS_TRIANGLE;
        }
        if (g & PAD_BUTTON_START) b |= DS_START;
        if (g & PAD_TRIGGER_Z) b |= DS_SELECT;
        if (g & PAD_TRIGGER_L) b |= DS_L1;
        if (g & PAD_TRIGGER_R) b |= DS_R1;
        if (g & PAD_BUTTON_UP) b |= DS_UP;
        if (g & PAD_BUTTON_DOWN) b |= DS_DOWN;
        if (g & PAD_BUTTON_LEFT) b |= DS_LEFT;
        if (g & PAD_BUTTON_RIGHT) b |= DS_RIGHT;
        if (PAD_TriggerL(port) > 100) b |= DS_L2;
        if (PAD_TriggerR(port) > 100) b |= DS_R2;
        if (PAD_StickX(port) || PAD_StickY(port)) {
            out->lx = axis(PAD_StickX(port), 100);
            out->ly = axis(-PAD_StickY(port), 100);
        }
        if (PAD_SubStickX(port) || PAD_SubStickY(port)) {
            out->rx = axis(PAD_SubStickX(port), 100);
            out->ry = axis(-PAD_SubStickY(port), 100);
        }
    }
    {
        if (w && w->err == WPAD_ERR_NONE) {
            u32 h = w->btns_h;
            if (w->exp.type == WPAD_EXP_CLASSIC) {
                if (g_pad_profile) {
                    if (h & WPAD_CLASSIC_BUTTON_A) b |= DS_CROSS;
                    if (h & WPAD_CLASSIC_BUTTON_B) b |= DS_CIRCLE;
                    if (h & WPAD_CLASSIC_BUTTON_Y) b |= DS_SQUARE;
                    if (h & WPAD_CLASSIC_BUTTON_X) b |= DS_TRIANGLE;
                } else {
                    if (h & WPAD_CLASSIC_BUTTON_B) b |= DS_CROSS;
                    if (h & WPAD_CLASSIC_BUTTON_A) b |= DS_CIRCLE;
                    if (h & WPAD_CLASSIC_BUTTON_Y) b |= DS_SQUARE;
                    if (h & WPAD_CLASSIC_BUTTON_X) b |= DS_TRIANGLE;
                }
                if (h & WPAD_CLASSIC_BUTTON_PLUS) b |= DS_START;
                if (h & WPAD_CLASSIC_BUTTON_MINUS) b |= DS_SELECT;
                if (h & WPAD_CLASSIC_BUTTON_FULL_L) b |= DS_L1;
                if (h & WPAD_CLASSIC_BUTTON_FULL_R) b |= DS_R1;
                if (h & WPAD_CLASSIC_BUTTON_ZL) b |= DS_L2;
                if (h & WPAD_CLASSIC_BUTTON_ZR) b |= DS_R2;
                if (h & WPAD_CLASSIC_BUTTON_UP) b |= DS_UP;
                if (h & WPAD_CLASSIC_BUTTON_DOWN) b |= DS_DOWN;
                if (h & WPAD_CLASSIC_BUTTON_LEFT) b |= DS_LEFT;
                if (h & WPAD_CLASSIC_BUTTON_RIGHT) b |= DS_RIGHT;
                {
                    joystick_t *l = &w->exp.classic.ljs, *r = &w->exp.classic.rjs;
                    if (l->mag > 0.15f) {
                        out->lx = axis((int)(l->mag * 100 * sinf(l->ang * 3.14159265f / 180)), 100);
                        out->ly = axis((int)(-l->mag * 100 * cosf(l->ang * 3.14159265f / 180)), 100);
                    }
                    if (r->mag > 0.15f) {
                        out->rx = axis((int)(r->mag * 100 * sinf(r->ang * 3.14159265f / 180)), 100);
                        out->ry = axis((int)(-r->mag * 100 * cosf(r->ang * 3.14159265f / 180)), 100);
                    }
                }
            } else {
                if (h & WPAD_BUTTON_A) b |= DS_CROSS;
                if (h & WPAD_BUTTON_B) b |= DS_CIRCLE;
                if (h & WPAD_BUTTON_1) b |= DS_SQUARE;
                if (h & WPAD_BUTTON_2) b |= DS_TRIANGLE;
                if (h & WPAD_BUTTON_PLUS) b |= DS_START;
                if (h & WPAD_BUTTON_MINUS) b |= DS_SELECT;
                if (h & WPAD_BUTTON_UP) b |= DS_UP;
                if (h & WPAD_BUTTON_DOWN) b |= DS_DOWN;
                if (h & WPAD_BUTTON_LEFT) b |= DS_LEFT;
                if (h & WPAD_BUTTON_RIGHT) b |= DS_RIGHT;
                if (w->exp.type == WPAD_EXP_NUNCHUK) {
                    joystick_t *l = &w->exp.nunchuk.js;
                    if (h & WPAD_NUNCHUK_BUTTON_Z) b |= DS_R1;
                    if (h & WPAD_NUNCHUK_BUTTON_C) b |= DS_L1;
                    if (l->mag > 0.15f) {
                        out->lx = axis((int)(l->mag * 100 * sinf(l->ang * 3.14159265f / 180)), 100);
                        out->ly = axis((int)(-l->mag * 100 * cosf(l->ang * 3.14159265f / 180)), 100);
                    }
                }
            }
        }
    }
    if (g_auto_nopad) {
        /* a scripted test that must not be disturbed: the script's presses only */
        b = 0;
        out->rx = out->ry = out->lx = out->ly = 0x80;
    }
    b |= port ? g_auto_b2 : autopilot_buttons();
    for (k = 0; k < 16; k++)
        if ((b >> k) & 1 && pressure_index[k] >= 0)
            out->pressure[pressure_index[k]] = 0xFF;
    out->buttons = (uint16_t)~b;
}
