/*
 * Host platform interface. The runtime is host-agnostic; each platform
 * (platform/pc, platform/wii) implements this.
 */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlatSem PlatSem;
typedef struct PlatThread PlatThread;
typedef struct PlatFile PlatFile;

void plat_init(int argc, char **argv);
void plat_shutdown(void);

/* logging: printf-style, always line-buffered to the log sink */
void plat_log(const char *fmt, ...);
void plat_vlog(const char *fmt, va_list ap);
void plat_fatal(const char *fmt, ...);

/* time */
uint64_t plat_time_us(void);
/* fast raw clock for profiling (plat_tick_hz ticks per second) */
uint64_t plat_ticks(void);
extern const uint64_t plat_tick_hz;
void plat_sleep_us(uint32_t us);

/* threads: each guest thread runs on its own host thread; the runtime hands
 * the CPU between them with semaphores so only one runs at a time */
PlatSem *plat_sem_create(int initial);
void plat_sem_post(PlatSem *s);
void plat_sem_wait(PlatSem *s);
PlatThread *plat_thread_create(void (*fn)(void *), void *arg, size_t stack_size, const char *name);
void plat_thread_exit(void);
/* set *flag to 1 every period_us, preempting whatever guest code runs */
void plat_start_ticker(volatile int *flag, uint32_t period_us);
/* once per emulated vblank, from whichever thread runs guest code */
void plat_vblank(uint32_t count);

/* memory */
void *plat_alloc_guest_ram(size_t size);   /* 32 MB of PS2 RAM */
void *plat_alloc(size_t size);
void plat_free(void *p);

/* game data: paths are relative to the extracted disc root, '/' separated,
 * matched case-insensitively */
PlatFile *plat_open(const char *path);
void plat_close(PlatFile *f);
int64_t plat_size(PlatFile *f);
int64_t plat_read(PlatFile *f, void *buf, int64_t size, int64_t offset);
/* The same for a guest thread: what is not at hand is fetched by a host thread
 * while the caller waits through wait() (the kernel runs the other guest
 * threads; it returns after the next host event), and what a stream will ask
 * for next is fetched ahead. wait NULL, or a platform without such a thread:
 * plat_read. */
int64_t plat_read_guest(PlatFile *f, void *buf, int64_t size, int64_t offset, void (*wait)(void));

/* the game disc as an ISO9660 image, or NULL to use extracted files */
PlatFile *plat_open_disc_image(void);

/* user data (saves): writable directory */
const char *plat_save_dir(void);

/* video: present a 32-bit RGBA frame (debug path on PC; the Wii renders through GX) */
void plat_present_rgba(const uint32_t *pixels, int width, int height, int stride);

/* sound: 48 kHz stereo, pairs of left and right. plat_audio_level: the samples (pairs) not yet played,
 * -1 when nothing plays them; plat_audio_write: how many of `frames` were taken */
int plat_audio_level(void);
uint32_t plat_audio_write(const int16_t *lr, uint32_t frames);

/* input: PS2 pad state, 16-bit button mask (active low like the DualShock) plus analog axes */
typedef struct PlatPad {
    uint16_t buttons;   /* PS2 bit layout, 0 = pressed */
    uint8_t rx, ry, lx, ly;
    uint8_t pressure[12];
    int connected;
} PlatPad;
void plat_read_pad(int port, PlatPad *out);

#ifdef __cplusplus
}
#endif

#endif /* PLAT_H */
