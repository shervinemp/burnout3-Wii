/*
 * EE kernel emulation: threads, semaphores, interrupt handlers, alarms and
 * the syscall table.
 *
 * Each guest thread runs on its own host thread, but only one at a time:
 * g_cur holds the CPU and hands it over with a semaphore post/wait pair
 * (switch_to). Scheduling follows the EE kernel: strict priorities (0 is
 * highest), no time slicing, and switches only at kernel entry points, which
 * here are syscalls and the polling points in kernel_poll().
 *
 * Interrupt handlers run on whichever host thread holds the CPU, on a
 * dedicated guest context and stack.
 */
#include "hw.h"
#include "wtime.h"

#include "kernel_int.h"

#include <string.h>

#define MAX_SEMAS 256
#define MAX_HANDLERS 64
#define MAX_ALARMS 64
#if defined(HW_RVL)
#define HOST_STACK (32 * 1024)          /* measured: guest code stays below 10 KB (canary checked) */
#define IOP_HOST_STACK (24 * 1024)
#else
#define HOST_STACK (1024 * 1024)
#define IOP_HOST_STACK (1024 * 1024)
#endif

#define EE_HZ 294912000ull
#define VBLANK_US 16683u          /* NTSC field period */
#define HSYNC_US_X1000 63556u     /* 63.556 us per line */

typedef struct Sema {
    int used;
    int count, max, init;
    uint32_t attr, option;
} Sema;

typedef struct Handler {
    int used;
    int cause;          /* INTC cause or DMAC channel */
    uint32_t func, arg, gp;
    int enabled;
    int order;
} Handler;

typedef struct Alarm {
    int used;
    uint64_t due_us;
    uint32_t func, arg, gp;
    uint16_t time;
} Alarm;

Thread g_threads[MAX_THREADS];
static Sema g_semas[MAX_SEMAS];
static Handler g_intc_handlers[MAX_HANDLERS];
static Handler g_dmac_handlers[MAX_HANDLERS];
static Alarm g_alarms[MAX_ALARMS];
Thread *g_cur;
int64_t g_seq;
static int64_t g_front_seq;
int g_in_irq;
int g_need_resched;
static int g_dispatch_disabled;
static uint32_t g_intc_mask;          /* enabled INTC causes */
static uint32_t g_dmac_mask;          /* enabled DMAC channels */
static uint32_t g_intc_pending;
static uint32_t g_dmac_pending;
static uint64_t g_next_vblank_us;
static uint32_t g_vblank_count;
static R5900 g_irq_ctx;
static uint32_t g_vsync_flag_addr, g_vsync_field_addr;
static uint32_t g_heap_start, g_heap_end;
static uint32_t g_gs_imr = 0xFF00;
static int g_handler_order;
static int g_field;
static uint32_t g_user_syscalls[256];
/* OSD config: language English (bits 16-20 = 1), config version 2 (bits 13-15) */
static uint32_t g_osd_config = (1u << 16) | (2u << 13);

/* Sony's libkernl finds the kernel syscall table by installing two handlers
 * with SetSyscall and scanning kernel memory for their addresses; keep a table
 * where that scan will find it (0x80010000 folds onto guest 0x00010000). */
#define FAKE_SYSCALL_TABLE 0x80010000u

int g_trace;
volatile int g_irq_flag;          /* set when an interrupt is raised; cleared on delivery */

/* ---- helpers ---------------------------------------------------------------------- */

uint64_t kernel_cycles(void)
{
    return plat_time_us() * (EE_HZ / 1000000u);
}

Thread *kern_thread_get(int id)
{
    if (id <= 0 || id >= MAX_THREADS || !g_threads[id].used)
        return NULL;
    return &g_threads[id];
}

int kern_thread_alloc(void)
{
    int i;
    for (i = 1; i < MAX_THREADS; i++)
        if (!g_threads[i].used)
            return i;
    return -1;
}

void kern_make_ready(Thread *t, int front)
{
    t->state = THS_READY;
    t->ready_seq = front ? --g_front_seq : ++g_seq;
}

static int eff_prio(const Thread *t)
{
    return t->iop ? t->prio - 1000 : t->prio;
}

static Thread *pick_ready(void)
{
    Thread *best = NULL;
    int i;
    for (i = 1; i < MAX_THREADS; i++) {
        Thread *t = &g_threads[i];
        if (t->used && t->state == THS_READY &&
            (!best || eff_prio(t) < eff_prio(best) || (eff_prio(t) == eff_prio(best) && t->ready_seq < best->ready_seq)))
            best = t;
    }
    return best;
}

/* Host threads are pooled: when a guest thread ends, its host thread waits to
 * run the next guest thread that starts. Their stacks (MEM2 on the Wii, which
 * is never given back) are reused instead of piling up as the IOP starts and
 * ends module threads. EE and IOP threads have separate pools (stack sizes).
 * A worker rejoins the pool before it hands the CPU on, so the next start can
 * already take it: its wake semaphore keeps the request until it loops. */
typedef struct HostWorker {
    PlatSem *wake;
    Thread *t;
    int iop, id;
    struct HostWorker *next;
} HostWorker;

static HostWorker *g_idle_hosts[2];
static PlatSem *g_host_pool_lock;
static int g_host_count;

static void thread_host_run(HostWorker *w);

static void host_release(HostWorker *w)
{
    plat_sem_wait(g_host_pool_lock);
    w->next = g_idle_hosts[w->iop];
    g_idle_hosts[w->iop] = w;
    plat_sem_post(g_host_pool_lock);
}

static void host_worker(void *p)
{
    HostWorker *w = p;
    for (;;) {
        plat_sem_wait(w->wake);
        thread_host_run(w);
    }
}

static void start_host(Thread *t)
{
    char name[32];
    HostWorker *w;
    int iop = t->iop != 0;
    if (t->host_running)
        return;
    t->host_running = 1;
    if (!g_host_pool_lock)
        g_host_pool_lock = plat_sem_create(1);
    plat_sem_wait(g_host_pool_lock);
    w = g_idle_hosts[iop];
    if (w)
        g_idle_hosts[iop] = w->next;
    plat_sem_post(g_host_pool_lock);
    if (w) {
        if (g_trace)
            LOG("host thread %d reused for %s thread %d", w->id, iop ? "IOP" : "EE", t->id);
        w->t = t;
        plat_sem_post(w->wake);
        return;
    }
    w = calloc(1, sizeof *w);
    w->wake = plat_sem_create(1);
    w->t = t;
    w->iop = iop;
    w->id = ++g_host_count;
    snprintf(name, sizeof name, "%s-host-%d", iop ? "iop" : "ee", w->id);
    LOG("host thread %d for %s thread %d", w->id, iop ? "IOP" : "EE", t->id);
    plat_thread_create(host_worker, w, iop ? IOP_HOST_STACK : HOST_STACK, name);
}

/* Hand the CPU to `next`. Returns when the calling thread is scheduled again,
 * unless the caller is exiting (it then returns immediately). */
static uint32_t g_dbg_switches;     /* debugging: thread switches so far */
static uint32_t g_dbg_polls, g_dbg_noirq, g_dbg_noirq_status, g_dbg_early, g_dbg_scans;   /* debugging: kernel_poll's ways */
static int64_t g_dbg_early_by;
static uint64_t g_dbg_set_us, g_dbg_set_due, g_dbg_fire_due;
static uint32_t g_dbg_set_time, g_dbg_set_slot, g_dbg_fire_slot, g_dbg_fire_time;
static struct { uint8_t to, wait; uint16_t wait_id; uint32_t us; } g_dbg_sw[32];   /* the last of them: to whom, what the one before waits for, when */

static void switch_to(Thread *next, int caller_exits)
{
    Thread *prev = g_cur;
    if (next == prev)
        return;
    g_dbg_sw[g_dbg_switches & 31].to = (uint8_t)next->id;
    g_dbg_sw[g_dbg_switches & 31].wait = (uint8_t)(prev ? prev->wait : 0);
    g_dbg_sw[g_dbg_switches & 31].wait_id = (uint16_t)(prev ? prev->wait_id : 0);
    g_dbg_sw[g_dbg_switches & 31].us = (uint32_t)plat_time_us();
    g_dbg_switches++;
    g_cur = next;
    next->state = THS_RUN;
    if (g_trace > 1)
        LOG("switch %d -> %d", prev ? prev->id : 0, next->id);
    start_host(next);
    plat_sem_post(next->run);
    if (caller_exits || !prev)
        return;
    plat_sem_wait(prev->run);
    if (prev->kill) {
        prev->kill = 0;
        longjmp(prev->exit_jmp, 1);
    }
}

/* Called by the running thread at a kernel entry point. */
void kern_reschedule(void)
{
    if (g_in_irq) {
        g_need_resched = 1;
        return;
    }
    for (;;) {
        Thread *cur = g_cur;
        Thread *best;
        g_need_resched = 0;
        best = pick_ready();
        if (cur->state == THS_RUN) {
            if ((g_dispatch_disabled && !(best && best->iop)) || !best || eff_prio(best) >= eff_prio(cur))
                return;
            kern_make_ready(cur, 1);
        }
        if (!best) {
            kernel_idle_wait();
            continue;
        }
        switch_to(best, 0);
        return;
    }
}

/* ---- interrupts ------------------------------------------------------------------- */

static int irq_allowed(void)
{
    return !g_in_irq && g_cur && (g_cur->status & 0x10000u);
}

/* time in interrupt handlers so far (the vblank handler kicks a frame's chains: its whole render is in here) */
static uint64_t g_irq_us;

static void call_handler(uint32_t func, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t a2)
{
    R5900 *c = &g_irq_ctx;
    const uint64_t irq_t0 = g_in_irq ? 0 : plat_time_us();
    g_in_irq++;
    c->r[4][0] = a0; c->r[4][1] = (uint32_t)((int32_t)a0 >> 31);
    c->r[5][0] = a1; c->r[5][1] = (uint32_t)((int32_t)a1 >> 31);
    c->r[6][0] = a2; c->r[6][1] = (uint32_t)((int32_t)a2 >> 31);
    c->r[28][0] = gp; c->r[28][1] = 0;
    c->r[29][0] = RT_IRQ_STACK_TOP; c->r[29][1] = 0;
    c->r[31][0] = 0; c->r[31][1] = 0;
    c->thread = NULL;
    c->cop0[12] = 0x70030C12u;
    rt_run_guest(c, func);
    g_in_irq--;
    if (!g_in_irq)
        g_irq_us += plat_time_us() - irq_t0;
}

static void deliver(Handler *tab, uint32_t *pending, uint32_t mask)
{
    uint32_t p = *pending & mask;
    int cause;
    if (!p)
        return;
    *pending &= ~p;
    for (cause = 0; cause < 32; cause++) {
        int order, n;
        if (!(p & (1u << cause)))
            continue;
        /* handlers in registration order */
        for (order = 0, n = 0; order <= g_handler_order && n < MAX_HANDLERS; order++) {
            int i;
            for (i = 0; i < MAX_HANDLERS; i++) {
                Handler *h = &tab[i];
                if (h->used && h->cause == cause && h->order == order) {
                    n++;
                    if (h->enabled)
                        call_handler(h->func, h->gp, (uint32_t)cause, h->arg, 0);
                }
            }
        }
    }
}

/* (kernel_poll comes here some 500 times a frame, with every interrupt raised; walking the 64 slots each
 * time was 0.6% of a race frame. g_alarm_soonest: no alarm is due before it; 0 after any alarm was set.) */
static uint64_t g_alarm_soonest;
static uint64_t g_dbg_alarm_us, g_dbg_wake_us;  /* debugging: when a handler last ran, when a thread was last woken */

static void run_alarms(uint64_t now)
{
    uint64_t soonest = ~(uint64_t)0;
    int i;
    if (now < g_alarm_soonest) {
        g_dbg_early++;
        g_dbg_early_by = (int64_t)(g_alarm_soonest - now);
        return;
    }
    g_dbg_scans++;
    for (i = 0; i < MAX_ALARMS; i++) {
        Alarm *a = &g_alarms[i];
        if (a->used && now >= a->due_us) {
            a->used = 0;
            g_alarm_soonest = 0;        /* (the handler may set alarms: found in the next pass) */
            g_dbg_alarm_us = now;
            g_dbg_fire_slot = (uint32_t)i;
            g_dbg_fire_time = a->time;
            g_dbg_fire_due = a->due_us;
            call_handler(a->func, a->gp, (uint32_t)i + 1, a->time, a->arg);
            soonest = 0;
        } else if (a->used && a->due_us < soonest) {
            soonest = a->due_us;
        }
    }
    g_alarm_soonest = soonest;
}

static void timebase(uint64_t now)
{
    hw_timers_poll(now);
    if (now >= g_next_vblank_us) {
        if (now - g_next_vblank_us > 10 * VBLANK_US)
            g_next_vblank_us = now;
        g_next_vblank_us += VBLANK_US;
        g_vblank_count++;
        {
            /* with the game's flip state as its vblank handler (0x1F1CE8) will find it: the vblanks counted
             * since the last flip (0x4E2841), the frame drawn and waiting for its flip (0x4E27CF: 16), DMA the
             * handler started still running (0x4E2830: 32), the frame's queue not empty (0x4E2838, 0x4E283C:
             * 64), the interval (0x4E17DC, from 256) */
            void tl_add(int what, uint32_t arg);
            tl_add('V', (LB(0x004E2841u) & 15) | (LB(0x004E27CFu) ? 16 : 0) | (LB(0x004E2830u) ? 32 : 0) |
                            (LW(0x004E2838u) != LW(0x004E283Cu) ? 64 : 0) | (LB(0x004E17DCu) & 15) << 8);
        }
        g_field ^= 1;
        hw_vblank();
        if (g_vsync_flag_addr)
            g_w32(g_vsync_flag_addr, 1);
        if (g_vsync_field_addr)
            g_w32(g_vsync_field_addr, (uint32_t)g_field);
        kernel_raise_intc(INTC_VBLANK_S);
        kernel_raise_intc(INTC_VBLANK_E);
        game_patches_apply();           /* pnach "patch=1": again every vblank (runtime/patches.c) */
        plat_vblank(g_vblank_count);
    }
}

/* A host thread did something guest threads wait for (the disc image's
 * reader, platform code): it sets g_host_event, and whoever waits looks
 * again. The kernel's own sleeps are short while anyone waits so. */
volatile int g_host_event;
static int g_host_waiters;

static void host_events(void)
{
    int i;
    if (!g_host_event)
        return;
    g_host_event = 0;
    for (i = 1; i < MAX_THREADS; i++)
        if (g_threads[i].used && (g_threads[i].state & THS_WAIT) && g_threads[i].wait == WAIT_HOST)
            kern_wake(&g_threads[i]);
}

int kern_can_wait(void)
{
    return !g_in_irq && g_cur != NULL;
}

/* A native nap: the running guest thread waits until host time `until` (runtime/hle.c load_step, a load's
 * step taken again within the frame). Every other thread runs meanwhile, what is due is delivered, and
 * with nothing to run the kernel sleeps to the next event. A wait of its own: the thread's wakeup count,
 * alarms and semaphores are left alone (the game's sleep, SetAlarm and SleepThread, took a wakeup that
 * the game waits for elsewhere, and the game crashed). */
static Thread *g_nap;
static uint64_t g_nap_until;

static void nap_check(uint64_t now)
{
    if (g_nap && now >= g_nap_until && (g_nap->state & THS_WAIT) && g_nap->wait == WAIT_NAP)
        kern_wake(g_nap);
}

void kernel_nap(uint64_t until)
{
    if (g_in_irq || !g_cur || g_nap || plat_time_us() >= until)
        return;
    g_nap = g_cur;
    g_nap_until = until;
    kern_wait_current(WAIT_NAP, 0);
    g_nap = NULL;
}

/* the threads in one line (debugging): kind, id, priority, state/wait kind */
void kernel_log_threads(const char *why)
{
    char line[400];
    size_t m = 0;
    int i;
    for (i = 1; i < MAX_THREADS && m < sizeof line - 24; i++) {
        const Thread *t = &g_threads[i];
        if (t->used && t->state != THS_DORMANT)
            m += (size_t)snprintf(line + m, sizeof line - m, " %s%d p%d %s%x/%d", t->iop ? "i" : "e", i, t->prio,
                                  t == g_cur ? "*" : "", t->state, t->wait);
    }
    line[m] = 0;
    LOG("threads %s:%s", why, line);
}

void kern_wait_host(void)
{
    g_host_waiters++;
    kern_wait_current(WAIT_HOST, 0);
    g_host_waiters--;
}

/* Deliver whatever is due. Safe to call from any polling point. */
#ifdef WTIME
static void kernel_poll_inner(void);
void kernel_poll(void)
{
    WT_ENTER();
    kernel_poll_inner();
    WT_LEAVE();
}
static void kernel_poll_inner(void)
#else
void kernel_poll(void)
#endif
{
    uint64_t now;
    if (g_in_irq || !g_cur)
        return;
    host_events();
    now = plat_time_us();
    timebase(now);
    iop_kernel_tick(now);
    sif_poll();
    nap_check(now);
    g_dbg_polls++;
    if (!irq_allowed()) {
        g_dbg_noirq++;
        g_dbg_noirq_status = g_cur->status;
        if (g_need_resched)
            kern_reschedule();
        return;
    }
    g_irq_flag = 0;
    deliver(g_intc_handlers, &g_intc_pending, g_intc_mask);
    deliver(g_dmac_handlers, &g_dmac_pending, g_dmac_mask);
    run_alarms(now);
    if (g_need_resched)
        kern_reschedule();
}

volatile int g_poll_request;

void ps2_poll(void)
{
    g_poll_request = 0;
    kernel_poll();
}

void kernel_raise_intc(int cause)
{
    g_irq_flag = 1;
    g_poll_request = 1;
    g_intc_pending |= 1u << cause;
    hw_set_reg(0x1000F000u, hw_reg(0x1000F000u) | (1u << cause));   /* INTC_STAT, for pollers */
}
void kernel_raise_dmac(int channel)
{
    g_irq_flag = 1;
    g_poll_request = 1;
    g_dmac_pending |= 1u << channel;
}
int kernel_dmac_is_pending(int channel) { return (g_dmac_pending >> channel) & 1; }

static uint64_t next_event_us(void)
{
    uint64_t t = g_next_vblank_us, d = iop_next_deadline();
    if (d < t)
        t = d;
    d = hw_timers_next_us();
    if (d < t)
        t = d;
    if (g_nap && g_nap_until < t)
        t = g_nap_until;
    int i;
    for (i = 0; i < MAX_ALARMS; i++)
        if (g_alarms[i].used && g_alarms[i].due_us < t)
            t = g_alarms[i].due_us;
    return t;
}

/* The running thread is busy-waiting for something an interrupt handler
 * sets (the game's wait for the flip of the last frame): sleep to the next
 * event instead of spinning through it, as idle time, then deliver what is
 * due. Other threads could not run during the spin either. */
uint64_t g_kernel_idle_us;      /* time slept with nothing to run, in all */
int sif_pending(void);          /* sif.c: a packet for the EE can be delivered now */

/* Wait for the next event: asleep, or (autopilot "idlespin N": N ms and shorter waits) on the clock. A
 * host that wakes late from a short sleep (Dolphin on Windows: a millisecond or so) loses that time in
 * every wait when events come 5 ms apart. */
int g_idle_spin_ms;
int plat_log_pending(void);     /* the platform's log has lines not yet written out */
static void idle_sleep(uint64_t now, uint64_t t, uint32_t most)
{
    const uint64_t until = t - now > most ? now + most : t;
    if (g_idle_spin_ms && until - now <= (uint64_t)g_idle_spin_ms * 1000u) {
        uint64_t n;
        while ((n = plat_time_us()) < until) {
            /* the log thread has a lower priority than this one on purpose: while it has lines to write,
             * sleep in short pieces so that it runs */
            if (plat_log_pending() && until - n > 1500)
                plat_sleep_us(1000);
        }
    } else {
        plat_sleep_us((uint32_t)(until - now));
    }
}

void kernel_spin_wait(void)
{
    uint64_t now, t;
    static uint32_t n, why[4];
    if (g_in_irq || !g_cur)
        return;
    if (g_irq_flag)
        why[0]++;
    else if ((g_intc_pending & g_intc_mask) || (g_dmac_pending & g_dmac_mask) || sif_pending())
        why[1]++;
    else {
        now = plat_time_us();
        t = next_event_us();
        if (t > now) {
            const uint32_t most = g_host_waiters ? 300 : 20000;
            int prev = pt_enter(PT_IDLE);
            idle_sleep(now, t, most);
            pt_leave(prev);
            g_kernel_idle_us += plat_time_us() - now;
            if (plat_time_us() - now >= 1000) {
                void tl_add(int what, uint32_t arg);
                tl_add('I', (uint32_t)((plat_time_us() - now) / 100));
            }
            why[3]++;
        } else {
            why[2]++;
        }
    }
    if (++n == 20000) {
        LOG("spin waits: %u with an interrupt raised, %u pending, %u with an event due, %u slept", why[0], why[1], why[2],
            why[3]);
        n = 0;
        memset(why, 0, sizeof why);
    }
    kernel_poll();
}

/* is an alarm due within 250 us of now (and not yet) */
static int alarm_soon(uint64_t now)
{
    int i;
    for (i = 0; i < MAX_ALARMS; i++)
        if (g_alarms[i].used && g_alarms[i].due_us > now && g_alarms[i].due_us <= now + 250)
            return 1;
    return 0;
}

void kernel_idle_wait(void)
{
    uint64_t now = plat_time_us(), t, slept = 0;
    static uint64_t idle_since;
    if (g_intc_pending & g_intc_mask || g_dmac_pending & g_dmac_mask || sif_pending() || g_host_event) {
        /* deliver what is already pending even though the current thread waits (a packet from the IOP too) */
    } else if (alarm_soon(now)) {
        /* An alarm within a quarter of a millisecond, and nothing to run until then: it rings now. The game's
         * main thread sleeps three lines (190 us) in every update pass to let its other threads run
         * (0x131B90 -> 0x237050); when they have nothing to do that was time asleep, twice a 30 fps frame. */
        timebase(now);
        iop_kernel_tick(now);
        sif_poll();
        nap_check(now);
        g_in_irq = 0;
        deliver(g_intc_handlers, &g_intc_pending, g_intc_mask);
        deliver(g_dmac_handlers, &g_dmac_pending, g_dmac_mask);
        run_alarms(now + 250);
        idle_since = 0;
        return;
    } else {
        t = next_event_us();
        if (t > now) {
            const uint32_t most = g_host_waiters ? 300 : 20000;
            int prev = pt_enter(PT_IDLE);
            idle_sleep(now, t, most);
            pt_leave(prev);
            slept = now;
        }
    }
    host_events();
    now = plat_time_us();
    if (slept) {
        g_kernel_idle_us += now - slept;
        if (now - slept >= 1000) {
            void tl_add(int what, uint32_t arg);
            tl_add('I', (uint32_t)((now - slept) / 100));
        }
    }
    timebase(now);
    iop_kernel_tick(now);
    sif_poll();
    nap_check(now);
    g_in_irq = 0;
    deliver(g_intc_handlers, &g_intc_pending, g_intc_mask);
    deliver(g_dmac_handlers, &g_dmac_pending, g_dmac_mask);
    run_alarms(now);
    if (!pick_ready()) {
        if (!idle_since)
            idle_since = now;
        else if (now - idle_since > 10000000u) {
            int i;
            LOG("deadlock? no thread ready for 10 s");
            for (i = 1; i < MAX_THREADS; i++)
                if (g_threads[i].used)
                    LOG("  thread %d state %x wait %d/%d prio %d entry %08X", i, g_threads[i].state,
                        g_threads[i].wait, g_threads[i].wait_id, g_threads[i].prio, g_threads[i].entry);
            idle_since = now;
        }
    } else {
        idle_since = 0;
    }
}

/* ---- threads --------------------------------------------------------------------------- */

static void thread_finish(Thread *t)
{
    Thread *next;
    t->state = THS_DORMANT;
    t->wait = WAIT_NONE;
    t->host_running = 0;
    for (;;) {
        next = pick_ready();
        if (next)
            break;
        kernel_idle_wait();
    }
    switch_to(next, 1);
}

static void thread_host_run(HostWorker *w)
{
    Thread *t = w->t;
    plat_sem_wait(t->run);
    if (!setjmp(t->exit_jmp)) {
        if (t->kill) {
            t->kill = 0;
        } else {
            rt_run_guest(&t->ctx, t->entry);
            if (g_trace)
                LOG("thread %d returned from %08X", t->id, t->entry);
            if (t->on_exit)
                t->on_exit(t, t->ctx.r[2][0]);
        }
    }
    /* returned, ExitThread from guest code, or killed (before it ran, or
     * while blocked: the killer waits for the ack) */
    host_release(w);
    if (g_cur == t)
        thread_finish(t);
    else
        plat_sem_post(t->ack);
}

static void init_thread_ctx(Thread *t)
{
    R5900 *c = &t->ctx;
    memset(c, 0, sizeof *c);
    c->r[4][0] = t->arg;
    c->r[4][1] = (uint32_t)((int32_t)t->arg >> 31);
    c->r[28][0] = t->gp;
    c->r[29][0] = (t->stack + t->stack_size - 0x2A0u) & ~15u;
    c->r[31][0] = 0;
    c->cop0[12] = 0x70030C13u;
    c->vf[0][3] = 1.0f;
    c->thread = t;
    t->status = 0x70030C13u;
}

void kern_start_thread(Thread *t)
{
    if (!t->run) {
        t->run = plat_sem_create(0);
        t->ack = plat_sem_create(0);
    }
    kern_make_ready(t, 0);
}

void kern_exit_current(void)
{
    longjmp(g_cur->exit_jmp, 1);
}

static int k_create_thread(uint32_t param)
{
    int id = kern_thread_alloc();
    Thread *t;
    if (id < 0)
        return -1;
    t = &g_threads[id];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->id = id;
    t->state = THS_DORMANT;
    t->entry = g_r32(param + 4);
    t->stack = g_r32(param + 8);
    t->stack_size = g_r32(param + 12);
    t->gp = g_r32(param + 16);
    t->init_prio = t->prio = (int)g_r32(param + 20);
    t->attr = g_r32(param + 28);
    t->option = g_r32(param + 32);
    t->run = plat_sem_create(0);
    t->ack = plat_sem_create(0);
    LOG("CreateThread %d entry %08X (%s) stack %08X+%X prio %d", id, t->entry, rt_func_name(t->entry),
        t->stack, t->stack_size, t->prio);
    return id;
}

static int k_start_thread(int id, uint32_t arg)
{
    Thread *t = kern_thread_get(id);
    if (!t || t->state != THS_DORMANT)
        return -1;
    t->arg = arg;
    t->prio = t->init_prio;
    init_thread_ctx(t);
    kern_make_ready(t, 0);
    LOG("StartThread %d arg %08X", id, arg);
    kern_reschedule();
    return 0;
}

static void k_exit_thread(void)
{
    Thread *t = g_cur;
    LOG("ExitThread %d", t->id);
    longjmp(t->exit_jmp, 1);
}

static int k_terminate_thread(int id)
{
    Thread *t = kern_thread_get(id);
    if (!t || t == g_cur)
        return -1;
    return kern_terminate(t);
}

int kern_terminate(Thread *t)
{
    int id = t->id;
    if (t->state == THS_DORMANT)
        return id;
    LOG("TerminateThread %d", id);
    if (t->host_running) {
        t->kill = 1;
        t->state = THS_DORMANT;
        t->wait = WAIT_NONE;
        t->host_running = 0;
        plat_sem_post(t->run);
        plat_sem_wait(t->ack);
    }
    t->state = THS_DORMANT;
    t->wait = WAIT_NONE;
    return id;
}

/* debugging (the frame history, gs_gx.c): the longest wait of an EE thread since it was last read, with
 * the RPC call the IOP took last before the wait ended (sif.c) */
struct LongWait g_long_wait;
void *g_long_wait_who;          /* the thread watched: the one that kicks the frames (hle_flip_wait_entry) */
extern uint32_t g_sif_last_call[2];

void kern_wait_current(int type, int id)
{
    Thread *t = g_cur;
    uint64_t t0 = 0, idle0 = g_kernel_idle_us, irq0 = g_irq_us;
    uint32_t sw0 = g_dbg_switches, vb0 = g_vblank_count;
    int ee = (void *)t == g_long_wait_who;
    if (ee) {
        t0 = plat_time_us();
        g_dbg_polls = g_dbg_noirq = g_dbg_early = g_dbg_scans = 0;
    }
    t->state = THS_WAIT;
    t->wait = type;
    t->wait_id = id;
    t->wait_seq = ++g_seq;
    kern_reschedule();
    if (ee) {
        /* (less the interrupt handlers' time: a frame rendered from the vblank handler meanwhile is the
         * frame's own work, not a wait) */
        uint64_t dt = plat_time_us() - t0, in_irq = g_irq_us - irq0;
        dt = dt > in_irq ? dt - in_irq : 0;
#if defined(GEKKO)
        {
            /* debugging: where the frame's thread waited 6 ms and more (host return addresses, innermost
             * first: powerpc-eabi-addr2line -f -e burnout3.elf names them), the first 24 after the menus */
            static uint32_t logged;
            if (dt >= 6000 && g_vblank_count > 10000 && logged < 24) {
                uint32_t *sp = (uint32_t *)__builtin_frame_address(0), ra[14], n = 0, k;
                char line[420];
                size_t m = 0;
                while (sp && n < 14) {
                    uint32_t *next = (uint32_t *)sp[0];
                    if (!next || next <= sp || ((uintptr_t)next & 3) || (uintptr_t)next - (uintptr_t)sp > 0x100000)
                        break;
                    ra[n++] = next[1];
                    sp = next;
                }
                for (k = 0; k < n; k++)
                    m += (size_t)snprintf(line + m, sizeof line - m, " %08X", (unsigned)ra[k]);
                logged++;
                LOG("wait of %u.%u ms and %u.%u ms in interrupt handlers (kind %d id %d) from:%s", (unsigned)(dt / 1000),
                    (unsigned)(dt / 100 % 10), (unsigned)(in_irq / 1000), (unsigned)(in_irq / 100 % 10), type, id, line);
                LOG("  of it %u.%u ms asleep, %u thread switches, %u vblanks; an alarm ran %d us and the thread was woken "
                    "%d us after it began", (unsigned)((g_kernel_idle_us - idle0) / 1000),
                    (unsigned)((g_kernel_idle_us - idle0) / 100 % 10), g_dbg_switches - sw0, g_vblank_count - vb0,
                    (int)(int64_t)(g_dbg_alarm_us - t0), (int)(int64_t)(g_dbg_wake_us - t0));
                LOG("  polls %u, %u of them with interrupts off (status %08X); alarm scans %u, %u skipped (the last by %d us)",
                    g_dbg_polls, g_dbg_noirq, g_dbg_noirq_status, g_dbg_scans, g_dbg_early, (int)g_dbg_early_by);
                {
                    void plat_tick_trace(char *out, size_t size);
                    plat_tick_trace(line, sizeof line);
                    LOG("  the last ticks found the processor at (pc/lr):%s", line);
                }
                LOG("  the last alarm set: slot %u, %u lines, %d us before the wait began, due %d us after; the last to run: slot "
                    "%u, %u lines, due %d us after the wait began", g_dbg_set_slot, g_dbg_set_time, (int)(int64_t)(t0 - g_dbg_set_us),
                    (int)(int64_t)(g_dbg_set_due - t0), g_dbg_fire_slot, g_dbg_fire_time, (int)(int64_t)(g_dbg_fire_due - t0));
                {
                    /* the switches of the wait: time (us from its start), the thread switched to, and what the
                     * thread before it went to wait for (kind/id) */
                    uint32_t i, first = g_dbg_switches - sw0 > 30 ? g_dbg_switches - 30 : sw0;
                    m = 0;
                    for (i = first; i != g_dbg_switches && m < sizeof line - 28; i++)
                        m += (size_t)snprintf(line + m, sizeof line - m, " %d:t%u(%u/%u)",
                                              (int)(g_dbg_sw[i & 31].us - (uint32_t)t0), g_dbg_sw[i & 31].to,
                                              g_dbg_sw[i & 31].wait, g_dbg_sw[i & 31].wait_id);
                    LOG("  switches:%s", line);
                }
                if (logged == 1) {
                    const char *iop_func_name(uint32_t addr);
                    int i;
                    for (i = 1; i < MAX_THREADS; i++)
                        if (g_threads[i].used)
                            LOG("  thread %d%s prio %d entry %08X (%s) state %x wait %d/%d", i, g_threads[i].iop ? " (IOP)" : "",
                                g_threads[i].prio, g_threads[i].entry,
                                g_threads[i].iop ? iop_func_name(g_threads[i].entry) : rt_func_name(g_threads[i].entry),
                                g_threads[i].state, g_threads[i].wait, g_threads[i].wait_id);
                }
            }
        }
#endif
        if (dt > g_long_wait.us) {
            g_long_wait.us = (uint32_t)dt;
            g_long_wait.type = (uint8_t)type;
            g_long_wait.thread = (uint8_t)t->id;
            g_long_wait.id = (uint16_t)id;
            g_long_wait.sid = g_sif_last_call[0];
            g_long_wait.fno = g_sif_last_call[1];
        }
    }
}

void kern_wake(Thread *t)
{
    if ((void *)t == g_long_wait_who)
        g_dbg_wake_us = plat_time_us();
    t->wait = WAIT_NONE;
    if (t->state == THS_WAITSUSPEND)
        t->state = THS_SUSPEND;
    else
        kern_make_ready(t, 0);
    g_need_resched = 1;
}

static int k_sleep_thread(void)
{
    Thread *t = g_cur;
    if (t->wakeups > 0) {
        t->wakeups--;
        return t->id;
    }
    kern_wait_current(WAIT_SLEEP, 0);
    return t->id;
}

static int k_wakeup_thread(int id, int from_irq)
{
    Thread *t = kern_thread_get(id);
    if (!t || t == g_cur)
        return -1;
    if ((t->state == THS_WAIT || t->state == THS_WAITSUSPEND) && t->wait == WAIT_SLEEP)
        kern_wake(t);
    else
        t->wakeups++;
    if (!from_irq)
        kern_reschedule();
    return id;
}

static int k_change_priority(int id, int prio, int from_irq)
{
    Thread *t = id == 0 ? g_cur : kern_thread_get(id);
    int old;
    if (!t)
        return -1;
    old = t->prio;
    t->prio = prio;
    if (t->state == THS_READY)
        kern_make_ready(t, 0);
    if (!from_irq)
        kern_reschedule();
    return old;
}

static int k_rotate(int prio, int from_irq)
{
    Thread *cur = g_cur;
    if (cur && cur->prio == prio && cur->state == THS_RUN && !from_irq) {
        Thread *best;
        kern_make_ready(cur, 0);
        best = pick_ready();
        if (best == cur) {
            cur->state = THS_RUN;
            return 0;
        }
        switch_to(best, 0);
    } else {
        int i;
        /* move the first ready thread of that priority to the back */
        Thread *first = NULL;
        for (i = 1; i < MAX_THREADS; i++) {
            Thread *t = &g_threads[i];
            if (t->used && t->state == THS_READY && t->prio == prio && (!first || t->ready_seq < first->ready_seq))
                first = t;
        }
        if (first)
            first->ready_seq = ++g_seq;
        g_need_resched = 1;
    }
    return 0;
}

static int k_refer_thread(int id, uint32_t info)
{
    Thread *t = id == 0 ? g_cur : kern_thread_get(id);
    if (!t)
        return -1;
    if (info) {
        g_w32(info + 0, (uint32_t)t->state);
        g_w32(info + 4, t->entry);
        g_w32(info + 8, t->stack);
        g_w32(info + 12, t->stack_size);
        g_w32(info + 16, t->gp);
        g_w32(info + 20, (uint32_t)t->init_prio);
        g_w32(info + 24, (uint32_t)t->prio);
        g_w32(info + 28, t->attr);
        g_w32(info + 32, t->option);
        g_w32(info + 36, (uint32_t)t->wait);
        g_w32(info + 40, (uint32_t)t->wait_id);
        g_w32(info + 44, (uint32_t)t->wakeups);
    }
    return t->state;
}

static int k_suspend(int id)
{
    Thread *t = kern_thread_get(id);
    if (!t || t == g_cur)
        return -1;
    if (t->state == THS_READY)
        t->state = THS_SUSPEND;
    else if (t->state == THS_WAIT)
        t->state = THS_WAITSUSPEND;
    t->suspends++;
    return id;
}

static int k_resume(int id, int from_irq)
{
    Thread *t = kern_thread_get(id);
    if (!t)
        return -1;
    if (t->suspends > 0 && --t->suspends == 0) {
        if (t->state == THS_SUSPEND)
            kern_make_ready(t, 0);
        else if (t->state == THS_WAITSUSPEND)
            t->state = THS_WAIT;
    }
    if (!from_irq)
        kern_reschedule();
    return id;
}

static int k_release_wait(int id, int from_irq)
{
    Thread *t = kern_thread_get(id);
    if (!t || (t->state != THS_WAIT && t->state != THS_WAITSUSPEND))
        return -1;
    kern_wake(t);
    if (!from_irq)
        kern_reschedule();
    return id;
}

/* ---- semaphores ------------------------------------------------------------------------- */

static Sema *sema_get(int id)
{
    if (id < 0 || id >= MAX_SEMAS || !g_semas[id].used)
        return NULL;
    return &g_semas[id];
}

static int k_create_sema(uint32_t param)
{
    int i;
    for (i = 1; i < MAX_SEMAS; i++) {
        if (!g_semas[i].used) {
            Sema *s = &g_semas[i];
            s->used = 1;
            s->count = (int)g_r32(param + 0);
            s->max = (int)g_r32(param + 4);
            s->init = (int)g_r32(param + 8);
            s->attr = g_r32(param + 16);
            s->option = g_r32(param + 20);
            s->count = s->init;
            if (g_trace > 1)
                LOG("CreateSema %d init %d max %d", i, s->init, s->max);
            return i;
        }
    }
    return -1;
}

Thread *kern_first_waiter(int type, int id)
{
    Thread *best = NULL;
    int i;
    for (i = 1; i < MAX_THREADS; i++) {
        Thread *t = &g_threads[i];
        if (t->used && (t->state == THS_WAIT || t->state == THS_WAITSUSPEND) && t->wait == type &&
            t->wait_id == id && (!best || t->wait_seq < best->wait_seq))
            best = t;
    }
    return best;
}

static int k_signal_sema(int id, int from_irq)
{
    Sema *s = sema_get(id);
    Thread *w;
    if (!s)
        return -1;
    w = kern_first_waiter(WAIT_SEMA, id);
    if (w)
        kern_wake(w);
    else
        s->count++;
    if (!from_irq)
        kern_reschedule();
    return id;
}

static int k_wait_sema(int id)
{
    Sema *s = sema_get(id);
    if (!s)
        return -1;
    if (s->count > 0) {
        s->count--;
        return id;
    }
    kern_wait_current(WAIT_SEMA, id);
    return id;
}

static int k_poll_sema(int id)
{
    Sema *s = sema_get(id);
    if (!s || s->count <= 0)
        return -1;
    s->count--;
    return id;
}

static int k_delete_sema(int id)
{
    Sema *s = sema_get(id);
    Thread *w;
    if (!s)
        return -1;
    while ((w = kern_first_waiter(WAIT_SEMA, id)))
        kern_wake(w);
    s->used = 0;
    return id;
}

static int k_refer_sema(int id, uint32_t info)
{
    Sema *s = sema_get(id);
    int i, n = 0;
    if (!s)
        return -1;
    for (i = 1; i < MAX_THREADS; i++)
        if (g_threads[i].used && g_threads[i].wait == WAIT_SEMA && g_threads[i].wait_id == id &&
            (g_threads[i].state & THS_WAIT))
            n++;
    g_w32(info + 0, (uint32_t)s->count);
    g_w32(info + 4, (uint32_t)s->max);
    g_w32(info + 8, (uint32_t)s->init);
    g_w32(info + 12, (uint32_t)n);
    g_w32(info + 16, s->attr);
    g_w32(info + 20, s->option);
    return id;
}

/* ---- handlers ------------------------------------------------------------------------- */

static int add_handler(Handler *tab, int cause, uint32_t func, uint32_t arg, uint32_t gp)
{
    int i;
    for (i = 0; i < MAX_HANDLERS; i++) {
        if (!tab[i].used) {
            Handler *h = &tab[i];
            h->used = 1;
            h->cause = cause;
            h->func = func;
            h->arg = arg;
            h->gp = gp;
            h->enabled = 1;
            h->order = ++g_handler_order;
            return i + 1;
        }
    }
    return -1;
}

static int set_handler_enabled(Handler *tab, int cause, int id, int en)
{
    if (id <= 0 || id > MAX_HANDLERS || !tab[id - 1].used || tab[id - 1].cause != cause)
        return -1;
    tab[id - 1].enabled = en;
    return 0;
}

static int k_set_alarm(uint32_t time, uint32_t func, uint32_t arg, uint32_t gp)
{
    int i;
    for (i = 0; i < MAX_ALARMS; i++) {
        if (!g_alarms[i].used) {
            Alarm *a = &g_alarms[i];
            a->used = 1;
            g_alarm_soonest = 0;
            a->func = func;
            a->arg = arg;
            a->gp = gp;
            a->time = (uint16_t)time;
            a->due_us = plat_time_us() + (uint64_t)time * HSYNC_US_X1000 / 1000u;
            g_dbg_set_us = plat_time_us();
            g_dbg_set_due = a->due_us;
            g_dbg_set_time = time;
            g_dbg_set_slot = (uint32_t)i;
            return i + 1;
        }
    }
    return -1;
}

/* ---- syscalls ------------------------------------------------------------------------ */

static void ret(R5900 *c, int32_t v)
{
    c->r[2][0] = (uint32_t)v;
    c->r[2][1] = (uint32_t)(v >> 31);
}

static void k_setup_thread(R5900 *c)
{
    uint32_t gp = c->r[4][0], stack = c->r[5][0], size = c->r[6][0], args = c->r[7][0];
    Thread *t = g_cur;
    const char *argv0 = "cdrom0:\\SLUS_210.50;1";
    if (stack == 0xFFFFFFFFu)
        stack = PS2_RAM_SIZE - size;
    t->gp = gp;
    t->stack = stack;
    t->stack_size = size;
    if (args) {
        g_w32(args, 1);
        g_w32(args + 4, args + 0x40);
        g_w32(args + 8, 0);
        g_write_str(args + 0x40, argv0);
    }
    LOG("SetupThread gp %08X stack %08X+%X args %08X", gp, stack, size, args);
    ret(c, (int32_t)((stack + size - 0x2A0u) & ~15u));
}

/* syscalls this kernel implements itself; SetSyscall must not replace them */
static int syscall_is_native(int n)
{
    if (n < 0)
        n = -n;
    if (n >= 16 && n <= 79)
        return n != 63 && n != 73;
    if (n >= 92 && n <= 130)
        return 1;
    if (n >= 252 && n <= 255)
        return 1;
    return n == 2 || n == 4;
}

void kernel_syscall(R5900 *c, int32_t num)
{
    uint32_t a0 = c->r[4][0], a1 = c->r[5][0], a2 = c->r[6][0], a3 = c->r[7][0];
    int from_irq = num < 0 || g_in_irq;
    int n = num < 0 ? -num : num;
    if (g_trace > 1)
        LOG("syscall %d (%08X %08X %08X %08X)", num, a0, a1, a2, a3);
    if (n < 256 && g_user_syscalls[n]) {
        uint32_t h = g_user_syscalls[n];
        if (h >= 0x80000000u && !rt_lookup(h)) {
            /* handler copied into kernel memory by a libkernl patch: not needed here */
            static int logged;
            if (logged++ < 8)
                LOG("syscall %d -> kernel-resident handler %08X skipped", num, h);
            ret(c, 0);
            return;
        }
        rt_run_guest(c, h);
        return;
    }
    switch (n) {
    case 2: /* SetGsCrt(interlace, mode, ffmd) */
        LOG("SetGsCrt interlace %u mode %u ffmd %u", a0, a1, a2);
        ret(c, 0);
        break;
    case 4: /* Exit */
        LOG("Exit(%d)", (int)a0);
        plat_shutdown();
        exit((int)a0);
    case 5: case 8: case 1: case 0: case 3: case 9: case 12:
        ret(c, 0);
        break;
    case 6: case 7: case 123:
        FATAL("game asked to execute another program (syscall %d)", num);
    case 10: case 11: case 13: case 14: case 15:
        LOG("syscall %d (%08X %08X) ignored", num, a0, a1);
        ret(c, 0);
        break;
    case 16: /* AddIntcHandler(cause, handler, next[, arg]) */
        ret(c, add_handler(g_intc_handlers, (int)a0, a1, a3, c->r[28][0]));
        LOG("AddIntcHandler cause %u -> %08X (%s) arg %08X = %d", a0, a1, rt_func_name(a1), a3, (int)c->r[2][0]);
        break;
    case 17:
        if (a1 > 0 && a1 <= MAX_HANDLERS)
            g_intc_handlers[a1 - 1].used = 0;
        ret(c, 0);
        break;
    case 18:
        ret(c, add_handler(g_dmac_handlers, (int)a0, a1, a3, c->r[28][0]));
        LOG("AddDmacHandler ch %u -> %08X (%s) arg %08X = %d", a0, a1, rt_func_name(a1), a3, (int)c->r[2][0]);
        break;
    case 19:
        if (a1 > 0 && a1 <= MAX_HANDLERS)
            g_dmac_handlers[a1 - 1].used = 0;
        ret(c, 0);
        break;
    case 20: case 26:
        ret(c, (g_intc_mask >> a0) & 1);
        g_intc_mask |= 1u << a0;
        break;
    case 21: case 27:
        ret(c, (g_intc_mask >> a0) & 1);
        g_intc_mask &= ~(1u << a0);
        break;
    case 22: case 28:
        ret(c, (g_dmac_mask >> a0) & 1);
        g_dmac_mask |= 1u << a0;
        hw_set_reg(0x1000E010u, hw_reg(0x1000E010u) | (1u << (16 + a0)));
        break;
    case 23: case 29:
        ret(c, (g_dmac_mask >> a0) & 1);
        g_dmac_mask &= ~(1u << a0);
        hw_set_reg(0x1000E010u, hw_reg(0x1000E010u) & ~(1u << (16 + a0)));
        break;
    case 252: case 254: /* SetAlarm(time, handler, arg) */
        ret(c, k_set_alarm(a0, a1, a2, c->r[28][0]));
        break;
    case 253: case 255:
        if (a0 > 0 && a0 <= MAX_ALARMS)
            g_alarms[a0 - 1].used = 0;
        ret(c, 0);
        break;
    case 32:
        ret(c, k_create_thread(a0));
        break;
    case 33: {
        Thread *t = kern_thread_get((int)a0);
        if (t && t->state == THS_DORMANT && t != g_cur) {
            t->used = 0;
            ret(c, (int32_t)a0);
        } else {
            ret(c, -1);
        }
        break;
    }
    case 34:
        ret(c, k_start_thread((int)a0, a1));
        break;
    case 35:
        k_exit_thread();
        break;
    case 36: {
        Thread *t = g_cur;
        t->used = 0;
        k_exit_thread();
        break;
    }
    case 37: case 38:
        ret(c, k_terminate_thread((int)a0));
        break;
    case 39:
        g_dispatch_disabled = 1;
        ret(c, 0);
        break;
    case 40:
        g_dispatch_disabled = 0;
        ret(c, 0);
        kern_reschedule();
        break;
    case 41: case 42:
        ret(c, k_change_priority((int)a0, (int)a1, from_irq));
        break;
    case 43: case 44:
        ret(c, k_rotate((int)a0, from_irq));
        break;
    case 45: case 46:
        ret(c, k_release_wait((int)a0, from_irq));
        break;
    case 47:
        ret(c, g_cur ? g_cur->id : 0);
        break;
    case 48: case 49:
        ret(c, k_refer_thread((int)a0, a1));
        break;
    case 50:
        ret(c, k_sleep_thread());
        break;
    case 51: case 52:
        ret(c, k_wakeup_thread((int)a0, from_irq));
        break;
    case 53: case 54: {
        Thread *t = a0 == 0 ? g_cur : kern_thread_get((int)a0);
        int n2 = t ? t->wakeups : -1;
        if (t)
            t->wakeups = 0;
        ret(c, n2);
        break;
    }
    case 55: case 56:
        ret(c, k_suspend((int)a0));
        break;
    case 57: case 58:
        ret(c, k_resume((int)a0, from_irq));
        break;
    case 60:
        k_setup_thread(c);
        break;
    case 61: /* SetupHeap(start, size) */
        g_heap_start = a0;
        g_heap_end = (a1 == 0xFFFFFFFFu) ? g_cur->stack : a0 + a1;
        LOG("SetupHeap %08X-%08X", g_heap_start, g_heap_end);
        ret(c, (int32_t)g_heap_start);
        break;
    case 62:
        ret(c, (int32_t)g_heap_end);
        break;
    case 64:
        ret(c, k_create_sema(a0));
        break;
    case 65:
        ret(c, k_delete_sema((int)a0));
        break;
    case 66: case 67:
        ret(c, k_signal_sema((int)a0, from_irq));
        break;
    case 68:
        ret(c, k_wait_sema((int)a0));
        break;
    case 69: case 70:
        ret(c, k_poll_sema((int)a0));
        break;
    case 71: case 72:
        ret(c, k_refer_sema((int)a0, a1));
        break;
    case 74: /* SetOsdConfigParam(&param) */
        g_osd_config = g_r32(a0);
        ret(c, 0);
        break;
    case 75: /* GetOsdConfigParam(&param) */
        g_w32(a0, g_osd_config);
        ret(c, 0);
        break;
    case 110: /* SetOsdConfigParam2(buf, size, offset) */
        ret(c, 0);
        break;
    case 111: { /* GetOsdConfigParam2(buf, size, offset): zeros */
        uint32_t k;
        for (k = 0; k < a1; k++)
            g_w8(a0 + k, 0);
        ret(c, 0);
        break;
    }
    case 76: case 77: case 78: case 79:
        ret(c, 0);
        break;
    case 92: case 93:
        ret(c, set_handler_enabled(g_intc_handlers, (int)a0, (int)a1, n == 92));
        break;
    case 94: case 95:
        ret(c, set_handler_enabled(g_dmac_handlers, (int)a0, (int)a1, n == 94));
        break;
    case 96: case 97: case 98: case 100: case 104: case 102: case 106: case 130:
        ret(c, 0);
        break;
    case 99: case 103:
        ret(c, (int32_t)c->cop0[a0 & 31]);
        break;
    case 107:
        ret(c, 0);
        break;
    case 108: case 109:
        LOG("SetCPUTimer%s ignored", n == 108 ? "Handler" : "");
        ret(c, 0);
        break;
    case 112:
        ret(c, (int32_t)g_gs_imr);
        break;
    case 113:
        ret(c, (int32_t)g_gs_imr);
        g_gs_imr = a0;
        break;
    case 114:
        ret(c, 0);
        break;
    case 115:
        g_vsync_flag_addr = a0;
        g_vsync_field_addr = a1;
        ret(c, 0);
        break;
    case 116: /* SetSyscall(num, handler) */
        LOG("SetSyscall(%u, %08X)%s", a0, a1, syscall_is_native((int)a0) ? " (native, kept)" : "");
        if (!syscall_is_native((int)a0))
            g_user_syscalls[a0 & 255] = a1;
        g_w32(FAKE_SYSCALL_TABLE + 4 * (a0 & 255), a1);
        ret(c, 0);
        break;
    case 117: {
        char buf[256];
        g_read_str(buf, a0, sizeof buf);
        LOG("_print: %s", buf);
        ret(c, 0);
        break;
    }
    case 118:
        ret(c, sif_dma_stat((int)a0));
        break;
    case 119:
        ret(c, sif_set_dma(a0, a1));
        break;
    case 120:
        ret(c, 0);
        break;
    case 121:
        sif_set_reg(a0, a1);
        ret(c, 0);
        break;
    case 122:
        ret(c, (int32_t)sif_get_reg(a0));
        break;
    case 124:
        ret(c, 0);
        break;
    case 125:
        ret(c, 0);
        break;
    case 126:
        ret(c, 0);
        break;
    case 127:
        ret(c, (int32_t)PS2_RAM_SIZE);
        break;
    default:
        LOG("unimplemented syscall %d (a0 %08X a1 %08X)", num, a0, a1);
        ret(c, 0);
        break;
    }
}

void ps2_syscall(R5900 *c, uint32_t pc)
{
    int32_t num = (int32_t)c->r[3][0];
    (void)pc;
    kernel_syscall(c, num);
    if (!g_in_irq)
        kernel_poll();
}

void ps2_cop0_status_written(R5900 *c, uint32_t status)
{
    Thread *t = c->thread;
    if (t) {
        t->status = status;
        if ((status & 0x10000u) && !g_in_irq)
            kernel_poll();
    }
}

uint32_t kernel_vblank_count(void)
{
    return g_vblank_count;
}

uint64_t kernel_next_vblank_us(void)
{
    return g_next_vblank_us;
}

uint32_t ps2_cop0_count(R5900 *c)
{
    (void)c;
    kernel_poll();
    return (uint32_t)kernel_cycles();
}

int ps2_cpcond0(R5900 *c)
{
    (void)c;
    return 1;
}

R5900 *kernel_current_ctx(void)
{
    return g_in_irq ? &g_irq_ctx : g_cur ? &g_cur->ctx : NULL;
}

void kernel_dump_threads(void)
{
    int i;
    for (i = 1; i < MAX_THREADS; i++) {
        Thread *t = &g_threads[i];
        char what[64];
        if (!t->used || t->state == THS_DORMANT)
            continue;
        snprintf(what, sizeof what, "%s thread %d%s (state %x wait %d/%d prio %d)", t->iop ? "IOP" : "EE", i,
                 t == g_cur ? " [running]" : "", t->state, t->wait, t->wait_id, t->prio);
        rt_dump_stack(&t->ctx, what);
    }
}

/* ---- startup ----------------------------------------------------------------------- */

void kernel_init(void)
{
    memset(g_threads, 0, sizeof g_threads);
    g_irq_ctx.vf[0][3] = 1.0f;
    g_next_vblank_us = plat_time_us() + VBLANK_US;
    plat_start_ticker(&g_poll_request, 1000);
}

void kernel_start_main(uint32_t entry)
{
    Thread *t = &g_threads[1];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->id = 1;
    t->entry = entry;
    t->prio = t->init_prio = 0;
    t->stack = PS2_RAM_SIZE - 0x10000u;
    t->stack_size = 0x10000u;
    t->run = plat_sem_create(0);
    t->ack = plat_sem_create(0);
    init_thread_ctx(t);
    t->status = t->ctx.cop0[12] = 0x70030C12u;  /* crt0 enables interrupts itself */
    g_cur = NULL;
    switch_to(t, 1);
}
