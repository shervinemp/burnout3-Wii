/* Runtime internals shared between runtime modules. */
#ifndef RT_H
#define RT_H

#include "ps2_recomp.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define LOG(...) plat_log(__VA_ARGS__)
#define FATAL(...) plat_fatal(__VA_ARGS__)

extern int g_trace;          /* verbose logging level */

/* Where host time goes, by exclusive category (status line in the logs):
 * prev = pt_enter(PT_x) ... pt_leave(prev). Everything outside the marked
 * parts counts as EE (recompiled code, kernel and HLE). */
enum { PT_EE, PT_VU0, PT_VU1, PT_VIF, PT_GIF, PT_XFER, PT_SYNC, PT_PRIM, PT_FLUSH, PT_TEX, PT_GX, PT_IDLE, PT_KICK, PT_NAT,
       PT_COUNT };
extern uint64_t g_pt[PT_COUNT];
extern int g_pt_cur;
extern uint64_t g_pt_last;
extern uint32_t g_pt_n;
/* The accounting is a debugging aid and costs a read of the time base at every change of category (some 3000 changes
 * in a race frame, 1.4% of it): off unless autopilot "ptstat 1" (or the frame meter) turns it on. */
extern int g_pt_on;
static inline int pt_enter(int cat)
{
    uint64_t now;
    int prev;
    if (__builtin_expect(!g_pt_on, 1))
        return 0;
    now = plat_ticks();
    g_pt_n++;
    if (!g_pt_last)
        g_pt_last = now;
    prev = g_pt_cur;
    g_pt[prev] += now - g_pt_last;
    g_pt_last = now;
    g_pt_cur = cat;
    return prev;
}
static inline void pt_leave(int prev)
{
    uint64_t now;
    if (__builtin_expect(!g_pt_on, 1))
        return;
    now = plat_ticks();
    g_pt[g_pt_cur] += now - g_pt_last;
    g_pt_last = now;
    g_pt_cur = prev;
}
/* "EE 40% VU1 30% ..." for the time since the last call */
void pt_report(char *out, size_t size);
/* VU1 time by microcode chain since the last call: "46A590 40% ..." */
void vu1_prof_report(char *out, size_t size);

/* 1/sqrt(x) for native code that needs no exact result (0 for x <= 0): the
 * paired-single estimate and one Newton step, ~22 bits */
static inline float rt_rsqrtf(float x)
{
    if (!(x > 0.0f))
        return 0.0f;
#if defined(GEKKO)
    {
        double e, d = x;
        float y;
        __asm__("ps_rsqrte %0,%1" : "=f"(e) : "f"(d));
        y = (float)e;
        return y * (1.5f - 0.5f * x * y * y);
    }
#else
    return 1.0f / sqrtf(x);
#endif
}

/* The game's data patched as PCSX2's .pnach files do (runtime/patches.c): widescreen 16:9, args.txt
 * "--widescreen N" / autopilot "widescreen N" (0 off; 1 on; 2 as the Wii's 16:9 setting says: the Wii's default,
 * the PC host's is 0) */
extern int g_widescreen;
void game_patches_apply(void);

/* ---- guest memory layout used by the runtime ---------------------------------------- */
#define RT_SCRATCH_BASE   0x00080000u   /* runtime-owned guest memory (strings, structs) */
#define RT_SCRATCH_SIZE   0x00040000u
#define RT_IRQ_STACK_TOP  0x000FFF00u   /* stack for interrupt handlers */

uint32_t rt_scratch_alloc(uint32_t size, uint32_t align);

/* guest memory helpers for host code */
static inline uint32_t g_r32(uint32_t a) { return LW(a); }
static inline void g_w32(uint32_t a, uint32_t v) { SW(a, v); }
static inline uint16_t g_r16(uint32_t a) { return LH(a); }
static inline void g_w16(uint32_t a, uint16_t v) { SH(a, v); }
static inline uint8_t g_r8(uint32_t a) { return LB(a); }
static inline void g_w8(uint32_t a, uint8_t v) { SB(a, v); }
void g_read_str(char *dst, uint32_t addr, uint32_t max);
void g_write_str(uint32_t addr, const char *s);

/* ---- CPU ----------------------------------------------------------------------------------- */
PS2Func rt_lookup(uint32_t addr);
void kernel_spin_wait(void);    /* a guest busy-wait: sleep to the next event instead */
void rt_run_guest(R5900 *c, uint32_t addr);     /* call a guest function from host code */
const char *rt_func_name(uint32_t addr);
void rt_dump_trace(void);
void rt_dump_stack(R5900 *c, const char *what);
void kernel_dump_threads(void);
R5900 *kernel_current_ctx(void);

/* ---- kernel ----------------------------------------------------------------------------- */
void kernel_init(void);
void kernel_start_main(uint32_t entry);         /* runs the game; returns when it exits */
void kernel_syscall(R5900 *c, int32_t num);
void kernel_poll(void);                          /* deliver due interrupts; may switch threads */
void kernel_idle_wait(void);                     /* nothing ready: block until an event */
uint64_t kernel_cycles(void);                    /* EE cycle count derived from host time */
uint64_t kernel_next_vblank_us(void);            /* host time of the next vertical blank */
uint32_t kernel_vblank_count(void);              /* fields since boot: the game's clock */

/* interrupt sources */
enum {
    INTC_GS = 0, INTC_SBUS = 1, INTC_VBLANK_S = 2, INTC_VBLANK_E = 3, INTC_VIF0 = 4, INTC_VIF1 = 5,
    INTC_VU0 = 6, INTC_VU1 = 7, INTC_IPU = 8, INTC_TIM0 = 9, INTC_TIM1 = 10, INTC_TIM2 = 11,
    INTC_TIM3 = 12, INTC_SFIFO = 13, INTC_VU0WD = 14
};
enum {
    DMAC_VIF0 = 0, DMAC_VIF1 = 1, DMAC_GIF = 2, DMAC_FROM_IPU = 3, DMAC_TO_IPU = 4,
    DMAC_SIF0 = 5, DMAC_SIF1 = 6, DMAC_SIF2 = 7, DMAC_FROM_SPR = 8, DMAC_TO_SPR = 9
};
void kernel_raise_intc(int cause);
void kernel_raise_dmac(int channel);
int kernel_dmac_is_pending(int channel);

/* ---- SIF / IOP ---------------------------------------------------------------------------- */
void sif_init(void);
uint32_t sif_get_reg(uint32_t reg);
void sif_set_reg(uint32_t reg, uint32_t val);
int sif_set_dma(uint32_t transfers, uint32_t count);   /* EE -> IOP */
int sif_dma_stat(int id);
void sif_poll(void);                                    /* deliver IOP -> EE packets */

#define IOP_RAM_SIZE 0x200000u
extern uint8_t *iop_ram;       /* same word layout as EE RAM (see iop.h) */

/* an IOP RPC server: native (fn) or registered by a recompiled module (guest) */
#define IOP_RPC_DEFER (-0x7FFF)  /* native server: reply later with sif_rpc_complete */
typedef int (*IopRpcFn)(uint32_t rpc_number, uint32_t data, int size, void *user);
typedef struct IopServer {
    uint32_t sid;
    const char *name;
    IopRpcFn fn;
    void *user;
    uint32_t buff;          /* IOP address of the receive buffer */
    uint32_t buff_size;
    uint32_t addr;          /* IOP address of the server struct (the EE's handle) */
    int guest;
    uint32_t func, cfunc, cbuff, qd;
    uint32_t req[16];       /* the call being served */
    int busy;
} IopServer;
IopServer *iop_register_server(uint32_t sid, const char *name, IopRpcFn fn, void *user, uint32_t buff_size);
IopServer *sif_register_guest_server(uint32_t sid, uint32_t sd, uint32_t func, uint32_t buff, uint32_t cfunc,
                                     uint32_t cbuff, uint32_t qd);
void sif_rpc_complete(IopServer *s, int out);
void iop_sif_queue_call(IopServer *s);
int iop_modules_starting(void);
uint32_t iop_alloc(uint32_t size);

/* IOP services */
void iop_services_init(void);

/* ---- hardware ---------------------------------------------------------------------------- */
void hw_init(void);
void hw_vblank(void);
extern uint64_t g_kernel_idle_us;                /* time slept with nothing to run (kernel.c) */
void hw_timers_poll(uint64_t now_us);            /* EE timer overflow and compare interrupts that are due */
uint64_t hw_timers_next_us(void);                /* host time of the next one (~0: none) */

/* ---- VU0 micro ------------------------------------------------------------------------ */
void vu0_init(void);

int rt_main(int argc, char **argv);

/* ---- disc ------------------------------------------------------------------------------------ */
typedef struct DiscFile DiscFile;
void disc_init(void);
int disc_read_sectors(uint32_t lsn, uint32_t count, void *buf);   /* 0 on success */
/* the same from a guest thread that may wait (kern_can_wait): the other threads run while the data is fetched */
int disc_read_sectors_guest(uint32_t lsn, uint32_t count, void *buf);
int kern_can_wait(void);
void kern_wait_host(void);          /* the current guest thread waits for the next host event (g_host_event) */
extern volatile int g_host_event;
uint32_t disc_sector_count(void);
int disc_find(const char *path, uint32_t *lsn, uint32_t *size);
DiscFile *disc_open(const char *path);     /* ISO path, case-insensitive, ";1" optional */
void disc_close(DiscFile *f);
uint32_t disc_size(DiscFile *f);
int64_t disc_read(DiscFile *f, void *buf, int64_t size, int64_t offset);
int64_t disc_read_guest(DiscFile *f, void *buf, int64_t size, int64_t offset);

/* ---- ELF ------------------------------------------------------------------------------------ */
uint32_t elf_load(const char *path);

#endif
