/*
 * Kernel internals shared by the EE kernel (kernel.c) and the IOP kernel
 * (iop_kernel.c). Both CPUs' threads live in one table and one scheduler:
 * only one guest thread runs at a time, and every IOP thread outranks every
 * EE thread. IOP code is short and reactive (it mostly waits on semaphores),
 * so this approximates the two CPUs running side by side while keeping the
 * single-runner model that makes the emulation deterministic on a one-core Wii.
 */
#ifndef KERNEL_INT_H
#define KERNEL_INT_H

#include "rt.h"

#include <setjmp.h>

/* EE and IOP threads together (Burnout 3 has 28); each holds a CPU context */
#ifdef GEKKO
#define MAX_THREADS 64
#else
#define MAX_THREADS 256
#endif

/* thread states (EE and IOP kernels use the same values) */
#define THS_RUN 0x01
#define THS_READY 0x02
#define THS_WAIT 0x04
#define THS_SUSPEND 0x08
#define THS_WAITSUSPEND 0x0C
#define THS_DORMANT 0x10

enum {
    WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2, WAIT_EVENT = 3,
    WAIT_DELAY = 4,                 /* IOP DelayThread */
    WAIT_IOP_SEMA = 5, WAIT_IOP_EVENT = 6, WAIT_IOP_RPC = 7, WAIT_IOP_CDVD = 8,
    WAIT_HOST = 9,                  /* for a host thread's work (a disc read): kern_wait_host */
    WAIT_NAP = 10                   /* a native nap of an EE thread until a host time: kernel_nap */
};

typedef struct Thread Thread;
struct LongWait { uint32_t us, sid, fno; uint16_t id; uint8_t type, thread; };
typedef void (*ThreadExitFn)(Thread *t, uint32_t ret);

struct Thread {
    int used;
    int id;
    int state;
    int prio, init_prio;
    uint32_t entry, stack, stack_size, gp, attr, option, arg;
    int wait, wait_id;
    int64_t wait_seq;
    int wakeups;
    int suspends;
    int64_t ready_seq;
    uint32_t status;              /* COP0 Status as seen by this thread */
    R5900 ctx;
    PlatSem *run;
    PlatSem *ack;
    int host_running;
    int kill;
    jmp_buf exit_jmp;
    uint32_t event_bits, event_mode, event_res;
    /* IOP */
    int iop;
    uint64_t wake_us;             /* WAIT_DELAY deadline */
    int32_t wait_result;          /* return value for the woken wait call */
    ThreadExitFn on_exit;         /* called when the entry function returns */
    void *user;
};

extern Thread g_threads[MAX_THREADS];
extern Thread *g_cur;
extern int64_t g_seq;
extern int g_in_irq;
extern int g_need_resched;

Thread *kern_thread_get(int id);
int kern_thread_alloc(void);
void kern_make_ready(Thread *t, int front);
void kern_reschedule(void);
void kern_wait_current(int type, int id);
void kern_wake(Thread *t);
Thread *kern_first_waiter(int type, int id);
void kern_start_thread(Thread *t);         /* DORMANT -> READY with the context already set up */
void kern_exit_current(void);               /* ExitThread for the running thread */
int kern_terminate(Thread *t);

/* IOP kernel hooks called from the scheduler */
void iop_kernel_tick(uint64_t now);          /* wake expired DelayThread waits */
uint64_t iop_next_deadline(void);            /* earliest DelayThread deadline, or UINT64_MAX */

#endif
