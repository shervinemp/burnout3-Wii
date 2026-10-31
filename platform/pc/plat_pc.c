/*
 * PC (Cygwin/MSYS2 or Linux) development host: runs the recompiled game
 * headless with logging, for bring-up of the CPU, kernel and IOP layers.
 */
#define _GNU_SOURCE
#include "plat.h"
#include "rt.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <xmmintrin.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

void rt_dump_trace(void);

static volatile int g_in_signal;

/* guest memory snapshots for offline inspection (host byte order); suffix
 * distinguishes several snapshots of one run ("ramdump N" command) */
static void dump_memory(const char *suffix)
{
    char path[64];
    FILE *f;
    snprintf(path, sizeof path, "work/ee_ram%s.bin", suffix);
    if ((f = fopen(path, "wb"))) {
        fwrite(ps2_ram, 1, PS2_RAM_SIZE, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "work/iop_ram%s.bin", suffix);
    if (iop_ram && (f = fopen(path, "wb"))) {
        fwrite(iop_ram, 1, IOP_RAM_SIZE, f);
        fclose(f);
    }
    snprintf(path, sizeof path, "work/vu_mem%s.bin", suffix);
    if ((f = fopen(path, "wb"))) {          /* VU0/VU1 micro and data memory */
        extern uint32_t g_vu_mem[0x4000];
        fwrite(g_vu_mem, 1, sizeof g_vu_mem, f);
        fclose(f);
    }
}

static void on_signal(int sig)
{
    static volatile int once;
    if (once++)
        return;
    g_in_signal = 1;
    plat_log("signal %d: dumping trace", sig);
    rt_dump_trace();
    dump_memory("");
    _exit(2);
}

/* Scripted pad input for unattended runs: --input "1500:cross,1650:down:20,..."
 * presses a button at a vertical blank count (the frame numbers of the dumps)
 * for a number of fields (default 10). Game time keeps runs reproducible. */
typedef struct InputEvent { double t, hold; int bit; } InputEvent;
static InputEvent g_input[256];
static int g_ninput;
static uint8_t g_stick[4] = {0x7F, 0x7F, 0x7F, 0x7F};  /* lx, ly, rx, ry ("stick" command) */

static const struct { const char *name; int bit; } k_buttons[] = {
    {"l2", 0}, {"r2", 1}, {"l1", 2}, {"r1", 3}, {"triangle", 4}, {"circle", 5}, {"cross", 6},
    {"square", 7}, {"select", 8}, {"l3", 9}, {"r3", 10}, {"start", 11}, {"up", 12}, {"right", 13},
    {"down", 14}, {"left", 15},
};

static void parse_input_script(const char *spec)
{
    char buf[4096], *ev, *save = NULL;
    snprintf(buf, sizeof buf, "%s", spec);
    for (ev = strtok_r(buf, ",", &save); ev && g_ninput < 256; ev = strtok_r(NULL, ",", &save)) {
        char name[32] = "";
        double t = 0, hold = 10;
        size_t k;
        if (sscanf(ev, "%lf:%31[a-z0-9]:%lf", &t, name, &hold) < 2)
            continue;
        for (k = 0; k < sizeof k_buttons / sizeof k_buttons[0]; k++)
            if (!strcmp(k_buttons[k].name, name)) {
                g_input[g_ninput].t = t;
                g_input[g_ninput].hold = hold;
                g_input[g_ninput++].bit = k_buttons[k].bit;
            }
    }
}

struct PlatSem { sem_t s; };
struct PlatThread { pthread_t t; void (*fn)(void *); void *arg; };
struct PlatFile { FILE *f; int64_t size; };

static char g_data_root[1024] = "work/disc";
static char g_iso[1024];
static char g_save_dir[1024] = "work/saves";
static FILE *g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_t0;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* The EE FPU and the VUs round toward zero and treat denormals as zero; the
 * game's physics is tuned to that (--fpu-nearest keeps the host's rounding). */
static int g_fpu_nearest;

static void fpu_ps2_mode(void)
{
    if (g_fpu_nearest)
        return;
    fesetround(FE_TOWARDZERO);
    _mm_setcsr(_mm_getcsr() | 0x8040u);     /* FTZ | DAZ */
}

static void set_time_scale(double s);

void plat_init(int argc, char **argv)
{
    int i;
    double time_scale = 1.0;
    const char *logpath = "work/run.log";
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc)
            snprintf(g_data_root, sizeof g_data_root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            logpath = argv[++i];
        else if (!strcmp(argv[i], "--iso") && i + 1 < argc)
            snprintf(g_iso, sizeof g_iso, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)
            parse_input_script(argv[++i]);
        else if (!strcmp(argv[i], "--time-scale") && i + 1 < argc)
            time_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fpu-nearest"))
            g_fpu_nearest = 1;
    }
    g_log = fopen(logpath, "w");
    if (!g_log)
        g_log = stderr;
    mkdir(g_save_dir, 0755);
    g_t0 = now_us();
    fpu_ps2_mode();
    if (time_scale != 1.0)
        set_time_scale(time_scale);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
}

void plat_shutdown(void)
{
    if (g_log && g_log != stderr)
        fclose(g_log);
}

void plat_vlog(const char *fmt, va_list ap)
{
    /* the signal dump may interrupt a thread that holds the lock: do not wait for it then */
    int locked = g_in_signal ? pthread_mutex_trylock(&g_log_lock) == 0 : pthread_mutex_lock(&g_log_lock) == 0;
    fprintf(g_log, "[%9.3f] ", (now_us() - g_t0) / 1e6);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    if (locked)
        pthread_mutex_unlock(&g_log_lock);
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
    va_start(ap, fmt);
    plat_vlog(fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    plat_shutdown();
    exit(1);
}

/* --time-scale N (debugging): the emulated clock runs N times slower than the
 * host clock and idle waits skip ahead instead of sleeping. A slow debug build
 * then still shows the game about one field per rendered frame; the game steps
 * its physics by the fields elapsed, and 20 fields per frame make cars fall
 * through the road. Guest threads run one at a time, so no locking. */
static double g_time_scale = 1.0;
static uint64_t g_scale_real0, g_scale_virt0, g_virt_skip;

uint64_t plat_ticks(void) { return now_us(); }
const uint64_t plat_tick_hz = 1000000;

uint64_t plat_time_us(void)
{
    uint64_t real = now_us() - g_t0;
    if (g_time_scale == 1.0 && !g_virt_skip && !g_scale_virt0)
        return real;
    return g_scale_virt0 + (uint64_t)((double)(real - g_scale_real0) / g_time_scale) + g_virt_skip;
}

static void set_time_scale(double s)
{
    uint64_t v = plat_time_us();
    if (s < 1.0)
        s = 1.0;
    g_scale_real0 = now_us() - g_t0;
    g_scale_virt0 = v - g_virt_skip;
    g_time_scale = s;
    plat_log("time scale %.1f", s);
}

int plat_log_pending(void)
{
    return 0;                   /* the log is written where the lines are made */
}

void plat_sleep_us(uint32_t us)
{
    if (g_time_scale != 1.0) {
        g_virt_skip += us;
        return;
    }
    struct timespec ts = {us / 1000000u, (long)(us % 1000000u) * 1000};
    nanosleep(&ts, NULL);
}

PlatSem *plat_sem_create(int initial)
{
    PlatSem *s = malloc(sizeof *s);
    sem_init(&s->s, 0, (unsigned)initial);
    return s;
}

void plat_sem_post(PlatSem *s) { sem_post(&s->s); }

void plat_sem_wait(PlatSem *s)
{
    while (sem_wait(&s->s) != 0 && errno == EINTR)
        ;
}

static void *thread_tramp(void *p)
{
    PlatThread *t = p;
    fpu_ps2_mode();
    t->fn(t->arg);
    return NULL;
}

PlatThread *plat_thread_create(void (*fn)(void *), void *arg, size_t stack_size, const char *name)
{
    PlatThread *t = malloc(sizeof *t);
    pthread_attr_t a;
    (void)name;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_size);
    if (pthread_create(&t->t, &a, thread_tramp, t) != 0)
        plat_fatal("pthread_create failed");
    pthread_attr_destroy(&a);
    pthread_detach(t->t);
    return t;
}

void plat_thread_exit(void) { pthread_exit(NULL); }

/* debugging ("gsmem N"): GS memory to work/gsmem_<k>.bin at the next N vblanks */
static int g_gsmem_dumps, g_gsmem_k;

void plat_vblank(uint32_t count)
{
    extern uint32_t *gs_mem;
    (void)count;
    if (g_gsmem_dumps > 0 && gs_mem) {
        char name[64];
        FILE *f;
        snprintf(name, sizeof name, "work/gsmem_%d.bin", g_gsmem_k++);
        if ((f = fopen(name, "wb")) != NULL) {
            fwrite(gs_mem, 1, 4u << 20, f);
            fclose(f);
        }
        if (--g_gsmem_dumps == 0)
            plat_log("GS memory dumped (%d files so far)", g_gsmem_k);
    }
}

void *plat_alloc_guest_ram(size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, 65536, size) != 0)
        plat_fatal("cannot allocate guest RAM");
    memset(p, 0, size);
    return p;
}

void *plat_alloc(size_t size) { return malloc(size); }
void plat_free(void *p) { free(p); }

/* case-insensitive path resolution against the data root */
static int resolve(const char *rel, char *out, size_t outsz)
{
    char cur[1024], part[256];
    const char *p = rel;
    snprintf(cur, sizeof cur, "%s", g_data_root);
    while (*p) {
        size_t n = 0;
        DIR *d;
        struct dirent *e;
        int found = 0;
        while (*p == '/' || *p == '\\')
            p++;
        while (*p && *p != '/' && *p != '\\' && n < sizeof part - 1)
            part[n++] = *p++;
        part[n] = 0;
        if (!n)
            break;
        d = opendir(cur);
        if (!d)
            return 0;
        while ((e = readdir(d))) {
            if (!strcasecmp(e->d_name, part)) {
                size_t l = strlen(cur);
                snprintf(cur + l, sizeof cur - l, "/%s", e->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found)
            return 0;
    }
    snprintf(out, outsz, "%s", cur);
    return 1;
}

PlatFile *plat_open(const char *path)
{
    char full[1024];
    FILE *f;
    PlatFile *pf;
    if (!resolve(path, full, sizeof full))
        return NULL;
    f = fopen(full, "rb");
    if (!f)
        return NULL;
    pf = malloc(sizeof *pf);
    pf->f = f;
    fseeko(f, 0, SEEK_END);
    pf->size = ftello(f);
    return pf;
}

PlatFile *plat_open_disc_image(void)
{
    FILE *f;
    PlatFile *pf;
    if (!g_iso[0])
        return NULL;
    f = fopen(g_iso, "rb");
    if (!f)
        plat_fatal("cannot open disc image %s", g_iso);
    pf = malloc(sizeof *pf);
    pf->f = f;
    fseeko(f, 0, SEEK_END);
    pf->size = ftello(f);
    return pf;
}

void plat_close(PlatFile *f)
{
    if (f) {
        fclose(f->f);
        free(f);
    }
}

int64_t plat_size(PlatFile *f) { return f->size; }

int64_t plat_read(PlatFile *f, void *buf, int64_t size, int64_t offset)
{
    if (fseeko(f->f, offset, SEEK_SET) != 0)
        return -1;
    return (int64_t)fread(buf, 1, (size_t)size, f->f);
}

int64_t plat_read_guest(PlatFile *f, void *buf, int64_t size, int64_t offset, void (*wait)(void))
{
    (void)wait;
    return plat_read(f, buf, size, offset);
}

const char *plat_save_dir(void) { return g_save_dir; }

/* sound: nothing plays it here; with B3_PCM=FILE what the chip's model mixes is written there
 * (48 kHz, 16 bits, left and right, as the machine keeps them) */
static FILE *g_pcm;
static int g_pcm_tried;

int plat_audio_level(void)
{
    if (!g_pcm_tried) {
        const char *p = getenv("B3_PCM");
        g_pcm_tried = 1;
        if (p && *p)
            g_pcm = fopen(p, "wb");
    }
    return g_pcm ? 2400 : -1;
}

uint32_t plat_audio_write(const int16_t *lr, uint32_t frames)
{
    if (g_pcm)
        fwrite(lr, 4, frames, g_pcm);
    return frames;
}

void plat_present_rgba(const uint32_t *pixels, int width, int height, int stride)
{
    /* written as a PPM every N frames by the caller's request (see gs dump options) */
    (void)pixels; (void)width; (void)height; (void)stride;
}

static volatile int *g_tick_flag;
static uint32_t g_tick_us;

static void *ticker(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(g_tick_us);
        *g_tick_flag = 1;
    }
    return NULL;
}

void plat_start_ticker(volatile int *flag, uint32_t period_us)
{
    pthread_t t;
    g_tick_flag = flag;
    g_tick_us = period_us;
    pthread_create(&t, NULL, ticker, NULL);
    pthread_detach(t);
}

/* Interactive control: lines written to work/cmd.txt while the game runs,
 * "<button> [fields]" to press a button now, "shot" for a screenshot of the
 * next frame (work/frames/shot.png). The file is consumed. */
static void poll_commands(void)
{
    static uint32_t last;
    extern volatile int g_shot_request;
    uint32_t now = kernel_vblank_count();
    char line[128];
    FILE *f;
    if (now - last < 6)
        return;
    last = now;
    f = fopen("work/cmd.txt", "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char name[32] = "";
        double hold = 10;
        size_t k;
        if (sscanf(line, "%31s %lf", name, &hold) < 1)
            continue;
        if (!strcmp(name, "shot")) {
            g_shot_request = 1;
            continue;
        }
        if (!strcmp(name, "dump")) {
            rt_dump_trace();
            continue;
        }
        if (!strcmp(name, "gsmem")) {
            g_gsmem_dumps = (int)hold;
            continue;
        }
        if (!strcmp(name, "trace")) {
            g_trace = (int)hold;
            continue;
        }
        if (!strcmp(name, "stick")) {
            /* "stick LX LY RX RY" (hex): what the analog sticks report from now on */
            unsigned v[4] = {0x80, 0x80, 0x80, 0x80};
            int j;
            sscanf(line, "%*s %x %x %x %x", &v[0], &v[1], &v[2], &v[3]);
            for (j = 0; j < 4; j++)
                g_stick[j] = (uint8_t)v[j];
            plat_log("sticks: left %02X %02X, right %02X %02X", v[0], v[1], v[2], v[3]);
            continue;
        }
        if (!strcmp(name, "resthold")) {
            extern int g_rest_hold;
            g_rest_hold = (int)hold;
            plat_log("stopped cars held: %d", g_rest_hold);
            continue;
        }
        if (!strcmp(name, "vu0off") || !strcmp(name, "vu0on")) {
            extern int g_vu0_disabled;
            g_vu0_disabled = name[3] == 'o' && name[4] == 'f';
            plat_log("VU0 micro calls %s", g_vu0_disabled ? "disabled" : "enabled");
            continue;
        }
        if (!strcmp(name, "ramdump")) {
            char suffix[16];
            snprintf(suffix, sizeof suffix, "_%d", (int)hold);
            dump_memory(suffix);
            plat_log("memory dumped (%s)", suffix);
            continue;
        }
        if (!strcmp(name, "calltrace")) {
            extern int ps2_calltrace_n;
            ps2_calltrace_n = (int)hold;
            continue;
        }
        if (!strcmp(name, "vucap")) {
            extern int g_vucap_n;
            extern uint32_t g_vucap_code;
            unsigned code = 0;
            sscanf(line, "%*s %*s %x", &code);
            mkdir("work/vucap", 0755);
            g_vucap_code = code;
            g_vucap_n = (int)hold;
            plat_log("vucap: capturing %d VU1 runs of %06X", g_vucap_n, code);
            continue;
        }
        if (!strcmp(name, "kickdump")) {
            extern uint32_t g_kick_dump_code;
            extern int g_kick_dump_n;
            unsigned code = 0, n = 4;
            sscanf(line, "%*s %x %u", &code, &n);
            g_kick_dump_code = code;
            g_kick_dump_n = (int)n;
            continue;
        }
        if (!strcmp(name, "gsdump")) {
            extern int g_gs_dump;
            g_gs_dump = (int)hold;
            continue;
        }
        if (!strcmp(name, "gsskip")) {
            extern uint32_t g_gs_skip;
            unsigned v = 0xFFFFFFFFu;
            sscanf(line, "%*s %x", &v);
            g_gs_skip = v;
            plat_log("dropping draws of VU1 program %04X", v);
            continue;
        }
        if (!strcmp(name, "defertest")) {
            extern int g_vif_defer_test;
            g_vif_defer_test = (int)hold;
            plat_log("deferred render data test %s", g_vif_defer_test ? "on" : "off");
            continue;
        }
        if (!strcmp(name, "gsstat")) {
            extern int g_gs_stat;
            g_gs_stat = (int)hold;
            continue;
        }
        if (!strcmp(name, "timescale")) {
            set_time_scale(hold);
            continue;
        }
        if (!strcmp(name, "fclamp")) {
#if defined(PS2_FPU_CHECK)
            extern int g_fclamp;
            g_fclamp = hold != 0;
            plat_log("float saturation %s", g_fclamp ? "on" : "off");
#endif
            continue;
        }
        if (!strcmp(name, "prof")) {
#if defined(PS2_PROFILE)
            void ps2_prof_dump(const char *path);
            ps2_prof_dump("work/prof.txt");
#endif
            continue;
        }
        if (!strcmp(name, "watchaddr")) {
            extern uint32_t g_watch_addr;
            unsigned v = 0;
            sscanf(line, "%*s %x", &v);
            g_watch_addr = v & ~3u;
            continue;
        }
        if (!strcmp(name, "watch") || !strcmp(name, "watchrange")) {
            extern uint32_t g_watch_val, g_watch_span;
            unsigned v = 0, hi = 0;
            int n = sscanf(line, "%*s %x %x", &v, &hi);
            g_watch_val = v;
            g_watch_span = n == 2 && hi >= v ? hi - v + 1 : v != 0;
            plat_log("value watch %08X + %X", g_watch_val, g_watch_span);
            continue;
        }
        for (k = 0; k < sizeof k_buttons / sizeof k_buttons[0]; k++)
            if (!strcmp(k_buttons[k].name, name) && g_ninput < 256) {
                g_input[g_ninput].t = now;
                g_input[g_ninput].hold = hold;
                g_input[g_ninput++].bit = k_buttons[k].bit;
            }
        plat_log("command: %s", name);
    }
    fclose(f);
    remove("work/cmd.txt");
}

void plat_pad_rumble(int port, unsigned strength)
{
    (void)port;
    (void)strength;
}

void plat_read_pad(int port, PlatPad *out)
{
    static const int8_t pressure_index[16] = {10, 11, 8, 9, 4, 5, 6, 7, -1, -1, -1, -1, 2, 0, 3, 1};
    double now = (double)kernel_vblank_count();
    int i;
    memset(out, 0, sizeof *out);
    out->buttons = 0xFFFF;
    out->lx = g_stick[0];
    out->ly = g_stick[1];
    out->rx = g_stick[2];
    out->ry = g_stick[3];
    out->connected = port == 0;
    if (port)
        return;
    poll_commands();
    for (i = 0; i < g_ninput; i++)
        if (now >= g_input[i].t && now < g_input[i].t + g_input[i].hold) {
            int b = g_input[i].bit;
            out->buttons &= (uint16_t)~(1u << b);
            if (pressure_index[b] >= 0)
                out->pressure[pressure_index[b]] = 0xFF;
        }
}
