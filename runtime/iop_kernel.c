/*
 * IOP kernel: thbase / thsemap / thevent / intrman / sysmem for recompiled
 * IOP modules, plus module loading.
 *
 * IOP threads are kernel.c threads flagged `iop`: they share the single-runner
 * scheduler and outrank all EE threads (see kernel_int.h). Return codes follow
 * the IOP kernel (KE_*), as ps2sdk's headers document them; no ps2sdk source is
 * included. See CREDITS.md.
 */
#include "iop.h"
#include "kernel_int.h"

#include <string.h>

#define KE_OK 0
#define KE_ILLEGAL_THID (-406)
#define KE_UNKNOWN_THID (-407)
#define KE_UNKNOWN_SEMID (-408)
#define KE_UNKNOWN_EVFID (-409)
#define KE_NOT_DORMANT (-414)
#define KE_SEMA_ZERO (-419)
#define KE_SEMA_OVF (-420)
#define KE_EVF_COND (-421)
#define KE_EVF_MULTI (-422)
#define KE_EVF_ILPAT (-423)
#define KE_RELEASE_WAIT (-418)
#define KE_WAIT_DELETE (-425)
#define KE_NO_MEMORY (-400)

#define WEF_OR 1
#define WEF_CLEAR 0x10
#define WEF_CLEAR_ALL 0x20

#define IOP_CLOCK_HZ 36864000ull

#define MAX_IOP_SEMAS 128
#define MAX_IOP_EVENTS 64
#define MAX_HEAP_BLOCKS 512

typedef struct IopSema { int used, count, max, initial; uint32_t attr, option; } IopSema;
typedef struct IopEvent { int used; uint32_t bits, attr, option, initial; } IopEvent;
typedef struct HeapBlock { uint32_t addr, size; int used; } HeapBlock;

static IopSema g_isemas[MAX_IOP_SEMAS];
static IopEvent g_ievents[MAX_IOP_EVENTS];
static HeapBlock g_heap[MAX_HEAP_BLOCKS];
static int g_nheap;
static R5900 g_iop_irq_ctx;
static int g_iop_irq_depth;
static uint32_t g_irq_sp = IOP_IRQ_STACK_TOP;

/* ---- memory helpers ------------------------------------------------------------------ */

void iop_read_str(char *dst, uint32_t addr, uint32_t max)
{
    uint32_t k;
    for (k = 0; k + 1 < max; k++) {
        char ch = (char)iop_r8(addr + k);
        dst[k] = ch;
        if (!ch)
            return;
    }
    dst[k] = 0;
}

void iop_write_str(uint32_t addr, const char *s)
{
    do
        iop_w8(addr++, (uint8_t)*s);
    while (*s++);
}

void iop_read_bytes(void *dst, uint32_t addr, uint32_t n)
{
    uint8_t *d = dst;
    uint32_t k;
    for (k = 0; k < n; k++)
        d[k] = iop_r8(addr + k);
}

void iop_write_bytes(uint32_t addr, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t k;
    for (k = 0; k < n; k++)
        iop_w8(addr + k, s[k]);
}

void iop_memset(uint32_t addr, uint8_t v, uint32_t n)
{
    if (!((addr | n) & 3)) {
        memset(iop_ram + (addr & IOP_MASK), v, n);
        return;
    }
    while (n--)
        iop_w8(addr++, v);
}

void ee_to_iop(uint32_t iop_dst, uint32_t ee_src, uint32_t n)
{
    iop_dst &= IOP_MASK;
    if (iop_dst + n > IOP_RAM_SIZE)
        FATAL("EE->IOP copy past IOP RAM: %08X+%X", iop_dst, n);
    if (!((iop_dst | ee_src | n) & 3)) {
        memcpy(iop_ram + iop_dst, GPTR(ee_src), n);
        return;
    }
    while (n--)
        iop_w8(iop_dst++, g_r8(ee_src++));
}

void iop_to_ee(uint32_t ee_dst, uint32_t iop_src, uint32_t n)
{
    iop_src &= IOP_MASK;
#if defined(PS2_DYNAMIC_IO)
    {
        extern uint32_t g_watch_addr;
        uint32_t d = ee_dst & 0x1FFFFFFFu;
        if (g_watch_addr - d < n) {
            uint32_t off = g_watch_addr - d;
            R5900 *c = iop_current_ctx();
            LOG("watch: IOP->EE copy %08X <- IOP %06X (%u bytes) writes %08X to %08X", ee_dst, iop_src, n,
                iop_r32(iop_src + off), g_watch_addr);
            if (c)
                rt_dump_stack(c, "watch IOP");
        }
    }
#endif
    if (!((ee_dst | iop_src | n) & 3)) {
        memcpy(GPTR(ee_dst), iop_ram + iop_src, n);
        return;
    }
    while (n--)
        g_w8(ee_dst++, iop_r8(iop_src++));
}

void iop_to_iop(uint32_t dst, uint32_t src, uint32_t n)
{
    dst &= IOP_MASK;
    src &= IOP_MASK;
    if (!((dst | src | n) & 3)) {
        memmove(iop_ram + dst, iop_ram + src, n);
        return;
    }
    if (dst < src || dst >= src + n) {
        uint32_t k;
        for (k = 0; k < n; k++)
            iop_w8(dst + k, iop_r8(src + k));
    } else {
        while (n--)
            iop_w8(dst + n, iop_r8(src + n));
    }
}

/* ---- modules and calls ------------------------------------------------------------------ */

static const IopModuleImage *module_at(uint32_t addr)
{
    int i;
    for (i = 0; iop_modules[i]; i++) {
        const IopModuleImage *m = iop_modules[i];
        if (addr >= m->base && addr < m->base + m->memsz)
            return m;
    }
    return NULL;
}

PS2Func iop_lookup(uint32_t addr)
{
    const IopModuleImage *m = module_at(addr & IOP_MASK);
    unsigned lo = 0, hi;
    if (!m)
        return NULL;
    addr &= IOP_MASK;
    hi = *m->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t a = m->table[mid].addr;
        if (a == addr)
            return m->table[mid].fn;
        if (a < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

const char *iop_func_name(uint32_t addr)
{
    static char buf[4][48];
    static int k;
    const IopModuleImage *m = module_at(addr & IOP_MASK);
    k = (k + 1) & 3;
    if (m)
        snprintf(buf[k], sizeof buf[k], "%s+%X", m->name, addr - m->base);
    else
        snprintf(buf[k], sizeof buf[k], "iop:%08X", addr);
    return buf[k];
}

const IopModuleImage *iop_find_module(const char *path)
{
    char name[64];
    const char *b = path, *p;
    const IopModuleImage *found = NULL;
    size_t n = 0;
    int i;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            b = p + 1;
    while (b[n] && b[n] != '.' && b[n] != ';' && n + 1 < sizeof name) {
        char ch = b[n];
        name[n++] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    name[n] = 0;
    /* a module loaded more than once has an image per copy (config/iop_modules.py): the first one not loaded yet */
    for (i = 0; iop_modules[i]; i++)
        if (!strcmp(iop_modules[i]->name, name)) {
            if (!iop_module_loaded(iop_modules[i]))
                return iop_modules[i];
            if (!found)
                found = iop_modules[i];
        }
    return found;
}

void iop_call(R5900 *c, uint32_t addr)
{
    PS2Func fn = iop_lookup(addr);
    if (!fn) {
        LOG("IOP call to unknown address %08X (ra %08X)", addr, c->r[31][0]);
        FATAL("IOP call to unknown address %08X", addr);
    }
    fn(c);
}

uint32_t iop_call_guest(R5900 *c, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    uint32_t sp = c->r[29][0], ra = c->r[31][0];
    c->r[4][0] = a0;
    c->r[5][0] = a1;
    c->r[6][0] = a2;
    c->r[7][0] = a3;
    c->r[29][0] = (sp - 32) & ~7u;     /* home area for the callee */
    c->r[31][0] = 0;
    iop_call(c, func);
    c->r[29][0] = sp;
    c->r[31][0] = ra;
    return c->r[2][0];
}

int iop_in_irq(void)
{
    return g_iop_irq_depth > 0;
}

R5900 *iop_current_ctx(void)
{
    return g_iop_irq_depth ? &g_iop_irq_ctx : g_cur ? &g_cur->ctx : NULL;
}

/* Interrupt-context call (SIF command handlers, libsd callbacks). Nested calls
 * get their own stack slice so a handler interrupting a handler is safe. */
uint32_t iop_call_irq(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2)
{
    R5900 saved, *c = &g_iop_irq_ctx;
    uint32_t v, sp = g_irq_sp;
    if (g_iop_irq_depth)
        saved = *c;
    g_iop_irq_depth++;
    g_irq_sp -= 0x2000;
    c->iop = 1;
    c->r[29][0] = sp - 16;
    c->r[28][0] = 0;
    v = iop_call_guest(c, func, a0, a1, a2, 0);
    g_irq_sp = sp;
    g_iop_irq_depth--;
    if (g_iop_irq_depth)
        *c = saved;
    return v;
}

/* ---- heap (sysmem) ------------------------------------------------------------------------ */

static void heap_init(void)
{
    g_nheap = 1;
    g_heap[0].addr = IOP_HEAP_START;
    g_heap[0].size = IOP_HEAP_END - IOP_HEAP_START;
    g_heap[0].used = 0;
}

static void heap_split(int i, uint32_t size)
{
    if (g_heap[i].size > size && g_nheap < MAX_HEAP_BLOCKS) {
        memmove(&g_heap[i + 2], &g_heap[i + 1], (size_t)(g_nheap - i - 1) * sizeof g_heap[0]);
        g_heap[i + 1].addr = g_heap[i].addr + size;
        g_heap[i + 1].size = g_heap[i].size - size;
        g_heap[i + 1].used = 0;
        g_heap[i].size = size;
        g_nheap++;
    }
}

/* mode 0: lowest fitting block, 1: highest, 2: at `want` */
uint32_t iop_heap_alloc_mode(int mode, uint32_t size, uint32_t want)
{
    int i, pick = -1;
    if (!size)
        return 0;
    size = (size + 255) & ~255u;
    for (i = 0; i < g_nheap; i++) {
        HeapBlock *b = &g_heap[i];
        if (b->used || b->size < size)
            continue;
        if (mode == 2) {
            if (want >= b->addr && want + size <= b->addr + b->size) {
                uint32_t pre = want - b->addr;
                if (pre) {
                    heap_split(i, pre);
                    i++;
                }
                heap_split(i, size);
                g_heap[i].used = 1;
                return g_heap[i].addr;
            }
            continue;
        }
        pick = i;
        if (mode == 0)
            break;
    }
    if (pick < 0)
        return 0;
    if (mode == 1 && g_heap[pick].size > size) {
        heap_split(pick, g_heap[pick].size - size);
        pick++;
    } else {
        heap_split(pick, size);
    }
    g_heap[pick].used = 1;
    return g_heap[pick].addr;
}

uint32_t iop_heap_alloc(uint32_t size)
{
    return iop_heap_alloc_mode(0, size, 0);
}

void iop_heap_free(uint32_t addr)
{
    int i;
    for (i = 0; i < g_nheap; i++) {
        if (g_heap[i].addr == addr && g_heap[i].used) {
            g_heap[i].used = 0;
            if (i + 1 < g_nheap && !g_heap[i + 1].used) {
                g_heap[i].size += g_heap[i + 1].size;
                memmove(&g_heap[i + 1], &g_heap[i + 2], (size_t)(g_nheap - i - 2) * sizeof g_heap[0]);
                g_nheap--;
            }
            if (i > 0 && !g_heap[i - 1].used) {
                g_heap[i - 1].size += g_heap[i].size;
                memmove(&g_heap[i], &g_heap[i + 1], (size_t)(g_nheap - i - 1) * sizeof g_heap[0]);
                g_nheap--;
            }
            return;
        }
    }
}

static uint32_t heap_max_free(void)
{
    uint32_t m = 0;
    int i;
    for (i = 0; i < g_nheap; i++)
        if (!g_heap[i].used && g_heap[i].size > m)
            m = g_heap[i].size;
    return m;
}

static uint32_t heap_total_free(void)
{
    uint32_t m = 0;
    int i;
    for (i = 0; i < g_nheap; i++)
        if (!g_heap[i].used)
            m += g_heap[i].size;
    return m;
}

/* ---- threads ------------------------------------------------------------------------------- */

static Thread *iop_thread(int id)
{
    Thread *t = id == 0 ? g_cur : kern_thread_get(id);
    return t && t->iop ? t : NULL;
}

int iop_create_thread(uint32_t entry, uint32_t prio, uint32_t stack_size, uint32_t gp, uint32_t attr)
{
    int id = kern_thread_alloc();
    Thread *t;
    uint32_t stack;
    if (id < 0)
        return KE_NO_MEMORY;
    stack_size = (stack_size + 255) & ~255u;
    stack = iop_heap_alloc(stack_size);
    if (!stack)
        return KE_NO_MEMORY;
    t = &g_threads[id];
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->iop = 1;
    t->id = id;
    t->state = THS_DORMANT;
    t->entry = entry;
    t->stack = stack;
    t->stack_size = stack_size;
    t->gp = gp;
    t->attr = attr;
    t->prio = t->init_prio = (int)prio;
    LOG("IOP CreateThread %d entry %s prio %u stack %X", id, iop_func_name(entry), prio, stack_size);
    return id;
}

static void thread_exit_hook(Thread *t, uint32_t ret)
{
    void (*fn)(void *, uint32_t) = (void (*)(void *, uint32_t))t->user;
    t->user = NULL;
    t->on_exit = NULL;
    if (fn)
        fn(t, ret);
}

static void *g_exit_user[MAX_THREADS];

void *iop_thread_user(void *thread)
{
    return g_exit_user[((Thread *)thread)->id];
}

int iop_start_thread(int id, uint32_t arg, void (*on_exit)(void *t, uint32_t ret), void *user)
{
    Thread *t = iop_thread(id);
    R5900 *c;
    if (!t)
        return KE_UNKNOWN_THID;
    if (t->state != THS_DORMANT)
        return KE_NOT_DORMANT;
    c = &t->ctx;
    memset(c, 0, sizeof *c);
    c->iop = 1;
    c->thread = t;
    c->r[4][0] = arg;
    c->r[28][0] = t->gp;
    c->r[29][0] = (t->stack + t->stack_size - 16) & ~15u;
    t->arg = arg;
    t->prio = t->init_prio;
    t->wakeups = 0;
    t->on_exit = on_exit ? thread_exit_hook : NULL;
    t->user = (void *)on_exit;
    g_exit_user[t->id] = user;
    kern_start_thread(t);
    return KE_OK;
}

int iop_iwakeup(int id)
{
    Thread *t = iop_thread(id);
    if (!t)
        return KE_UNKNOWN_THID;
    if ((t->state & THS_WAIT) && t->wait == WAIT_SLEEP)
        kern_wake(t);
    else
        t->wakeups++;
    return KE_OK;
}

void iop_sd_tick(uint64_t now);

/* the earliest wake time of the IOP threads in DelayThread (no later than
 * it: a thread woken otherwise leaves it early, which costs one scan) */
static uint64_t g_iop_wake_min = UINT64_MAX;

static uint64_t iop_scan_delays(uint64_t now, int wake)
{
    uint64_t d = UINT64_MAX;
    int i;
    for (i = 1; i < MAX_THREADS; i++) {
        Thread *t = &g_threads[i];
        if (t->used && t->iop && (t->state & THS_WAIT) && t->wait == WAIT_DELAY) {
            if (wake && now >= t->wake_us)
                kern_wake(t);
            else if (t->wake_us < d)
                d = t->wake_us;
        }
    }
    return d;
}

void iop_kernel_tick(uint64_t now)
{
    if (!iop_in_irq()) {
        iop_intr_tick();
        iop_sd_tick(now);
    }
    if (now >= g_iop_wake_min)
        g_iop_wake_min = iop_scan_delays(now, 1);
}

uint64_t iop_sd_next_us(void);

uint64_t iop_next_deadline(void)
{
    uint64_t d = g_iop_wake_min, s;
    if (iop_intr_pending())
        return 0;
    s = iop_sd_next_us();       /* the sound chip's interrupt is the driver's clock */
    return s < d ? s : d;
}

static void maybe_resched(void)
{
    if (!iop_in_irq() && !g_in_irq)
        kern_reschedule();
}

/* ---- semaphores ---------------------------------------------------------------------------- */

static IopSema *isema(int id)
{
    return id > 0 && id < MAX_IOP_SEMAS && g_isemas[id].used ? &g_isemas[id] : NULL;
}

static int sema_create(uint32_t attr, uint32_t option, int initial, int max)
{
    int i;
    for (i = 1; i < MAX_IOP_SEMAS; i++) {
        if (!g_isemas[i].used) {
            IopSema *s = &g_isemas[i];
            s->used = 1;
            s->attr = attr;
            s->option = option;
            s->count = s->initial = initial;
            s->max = max;
            return i;
        }
    }
    return KE_NO_MEMORY;
}

int iop_isignal_sema(int id)
{
    IopSema *s = isema(id);
    Thread *w;
    if (!s)
        return KE_UNKNOWN_SEMID;
    w = kern_first_waiter(WAIT_IOP_SEMA, id);
    if (w) {
        w->wait_result = KE_OK;
        kern_wake(w);
    } else if (s->count < s->max) {
        s->count++;
    } else {
        return KE_SEMA_OVF;
    }
    return KE_OK;
}

static int sema_wait(int id)
{
    IopSema *s = isema(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    if (s->count > 0) {
        s->count--;
        return KE_OK;
    }
    g_cur->wait_result = KE_OK;
    kern_wait_current(WAIT_IOP_SEMA, id);
    return g_cur->wait_result;
}

static int sema_delete(int id)
{
    IopSema *s = isema(id);
    Thread *w;
    if (!s)
        return KE_UNKNOWN_SEMID;
    while ((w = kern_first_waiter(WAIT_IOP_SEMA, id))) {
        w->wait_result = KE_WAIT_DELETE;
        kern_wake(w);
    }
    s->used = 0;
    return KE_OK;
}

static int sema_waiters(int id)
{
    int i, n = 0;
    for (i = 1; i < MAX_THREADS; i++)
        if (g_threads[i].used && (g_threads[i].state & THS_WAIT) && g_threads[i].wait == WAIT_IOP_SEMA &&
            g_threads[i].wait_id == id)
            n++;
    return n;
}

/* ---- event flags --------------------------------------------------------------------------- */

static IopEvent *ievent(int id)
{
    return id > 0 && id < MAX_IOP_EVENTS && g_ievents[id].used ? &g_ievents[id] : NULL;
}

static int event_match(IopEvent *e, uint32_t want, uint32_t mode)
{
    return (mode & WEF_OR) ? (e->bits & want) != 0 : (e->bits & want) == want;
}

static void event_consume(IopEvent *e, uint32_t want, uint32_t mode)
{
    if (mode & WEF_CLEAR_ALL)
        e->bits = 0;
    else if (mode & WEF_CLEAR)
        e->bits &= ~want;
}

int iop_iset_event(int id, uint32_t bits)
{
    IopEvent *e = ievent(id);
    int i;
    if (!e)
        return KE_UNKNOWN_EVFID;
    e->bits |= bits;
    for (;;) {
        Thread *best = NULL;
        for (i = 1; i < MAX_THREADS; i++) {
            Thread *t = &g_threads[i];
            if (t->used && (t->state & THS_WAIT) && t->wait == WAIT_IOP_EVENT && t->wait_id == id &&
                event_match(e, t->event_bits, t->event_mode) && (!best || t->wait_seq < best->wait_seq))
                best = t;
        }
        if (!best)
            break;
        best->event_res = e->bits;
        best->wait_result = KE_OK;
        event_consume(e, best->event_bits, best->event_mode);
        kern_wake(best);
    }
    return KE_OK;
}

static int event_wait(int id, uint32_t bits, uint32_t mode, uint32_t res_addr, int poll)
{
    IopEvent *e = ievent(id);
    if (!e)
        return KE_UNKNOWN_EVFID;
    if (!bits)
        return KE_EVF_ILPAT;
    if (event_match(e, bits, mode)) {
        if (res_addr)
            iop_w32(res_addr, e->bits);
        event_consume(e, bits, mode);
        return KE_OK;
    }
    if (poll)
        return KE_EVF_COND;
    g_cur->event_bits = bits;
    g_cur->event_mode = mode;
    g_cur->wait_result = KE_OK;
    kern_wait_current(WAIT_IOP_EVENT, id);
    if (res_addr && g_cur->wait_result == KE_OK)
        iop_w32(res_addr, g_cur->event_res);
    return g_cur->wait_result;
}

/* ---- HLE entry points --------------------------------------------------------------------- */

/* thbase */
void iop_hle_CreateThread(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    iop_ret(c, iop_create_thread(iop_r32(p + 8), iop_r32(p + 16), iop_r32(p + 12), c->r[28][0], iop_r32(p)));
}
void iop_hle_DeleteThread(R5900 *c)
{
    Thread *t = iop_thread((int)iop_arg(c, 0));
    if (!t)
        iop_ret(c, KE_UNKNOWN_THID);
    else if (t->state != THS_DORMANT)
        iop_ret(c, KE_NOT_DORMANT);
    else {
        iop_heap_free(t->stack);
        t->used = 0;
        iop_ret(c, KE_OK);
    }
}
void iop_hle_StartThread(R5900 *c)
{
    int r = iop_start_thread((int)iop_arg(c, 0), iop_arg(c, 1), NULL, NULL);
    iop_ret(c, r);
    if (r == KE_OK)
        maybe_resched();
}
void iop_hle_ExitThread(R5900 *c)
{
    (void)c;
    kern_exit_current();
}
void iop_hle_ExitDeleteThread(R5900 *c)
{
    (void)c;
    g_cur->used = 0;
    kern_exit_current();
}
void iop_hle_TerminateThread(R5900 *c)
{
    Thread *t = iop_thread((int)iop_arg(c, 0));
    if (!t || t == g_cur)
        iop_ret(c, KE_ILLEGAL_THID);
    else {
        kern_terminate(t);
        iop_ret(c, KE_OK);
    }
}
void iop_hle_ChangeThreadPriority(R5900 *c)
{
    Thread *t = iop_thread((int)iop_arg(c, 0));
    if (!t) {
        iop_ret(c, KE_UNKNOWN_THID);
        return;
    }
    t->prio = (int)iop_arg(c, 1);
    if (t->state == THS_READY)
        kern_make_ready(t, 0);
    iop_ret(c, KE_OK);
    maybe_resched();
}
void iop_hle_RotateThreadReadyQueue(R5900 *c)
{
    iop_ret(c, KE_OK);
    if (g_cur && g_cur->iop) {
        kern_make_ready(g_cur, 0);
        kern_reschedule();
    }
}
void iop_hle_GetThreadId(R5900 *c)
{
    iop_ret(c, g_cur && g_cur->iop ? g_cur->id : KE_ILLEGAL_THID);
}
void iop_hle_ReferThreadStatus(R5900 *c)
{
    Thread *t = iop_thread((int)iop_arg(c, 0));
    uint32_t info = iop_arg(c, 1);
    if (!t) {
        iop_ret(c, KE_UNKNOWN_THID);
        return;
    }
    if (info) {
        iop_w32(info + 0, t->attr);
        iop_w32(info + 4, t->option);
        iop_w32(info + 8, (uint32_t)t->state);
        iop_w32(info + 12, t->entry);
        iop_w32(info + 16, t->stack);
        iop_w32(info + 20, t->stack_size);
        iop_w32(info + 24, t->gp);
        iop_w32(info + 28, (uint32_t)t->init_prio);
        iop_w32(info + 32, (uint32_t)t->prio);
        iop_w32(info + 36, (uint32_t)t->wait);
        iop_w32(info + 40, (uint32_t)t->wait_id);
        iop_w32(info + 44, (uint32_t)t->wakeups);
    }
    iop_ret(c, KE_OK);
}
void iop_hle_SleepThread(R5900 *c)
{
    Thread *t = g_cur;
    if (t->wakeups > 0)
        t->wakeups--;
    else
        kern_wait_current(WAIT_SLEEP, 0);
    iop_ret(c, KE_OK);
}
void iop_hle_WakeupThread(R5900 *c)
{
    iop_ret(c, iop_iwakeup((int)iop_arg(c, 0)));
    maybe_resched();
}
void iop_hle_iWakeupThread(R5900 *c)
{
    iop_ret(c, iop_iwakeup((int)iop_arg(c, 0)));
}
void iop_hle_CancelWakeupThread(R5900 *c)
{
    Thread *t = iop_thread((int)iop_arg(c, 0));
    int n = t ? t->wakeups : KE_UNKNOWN_THID;
    if (t)
        t->wakeups = 0;
    iop_ret(c, n);
}
void iop_delay_until(uint64_t us)
{
    g_cur->wake_us = us;
    if (us < g_iop_wake_min)
        g_iop_wake_min = us;
    kern_wait_current(WAIT_DELAY, 0);
}

void iop_hle_DelayThread(R5900 *c)
{
    iop_delay_until(plat_time_us() + iop_arg(c, 0));
    iop_ret(c, KE_OK);
}
static uint64_t sysclock(void)
{
    return plat_time_us() * IOP_CLOCK_HZ / 1000000u;
}
void iop_hle_GetSystemTime(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    uint64_t t = sysclock();
    iop_w32(p, (uint32_t)t);
    iop_w32(p + 4, (uint32_t)(t >> 32));
    iop_ret(c, KE_OK);
}
void iop_hle_GetSystemTimeLow(R5900 *c)
{
    iop_ret(c, (int32_t)(uint32_t)sysclock());
}
void iop_hle_USec2SysClock(R5900 *c)
{
    uint64_t t = (uint64_t)iop_arg(c, 0) * IOP_CLOCK_HZ / 1000000u;
    uint32_t p = iop_arg(c, 1);
    iop_w32(p, (uint32_t)t);
    iop_w32(p + 4, (uint32_t)(t >> 32));
    iop_ret(c, KE_OK);
}
void iop_hle_SysClock2USec(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    uint64_t t = ((uint64_t)iop_r32(p + 4) << 32 | iop_r32(p)) * 1000000u / IOP_CLOCK_HZ;
    iop_w32(iop_arg(c, 1), (uint32_t)(t / 1000000u));
    iop_w32(iop_arg(c, 2), (uint32_t)(t % 1000000u));
    iop_ret(c, KE_OK);
}

/* thsemap */
void iop_hle_CreateSema(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    int id = sema_create(iop_r32(p), iop_r32(p + 4), (int)iop_r32(p + 8), (int)iop_r32(p + 12));
    if (g_trace)
        LOG("IOP CreateSema %d init %d max %d", id, (int)iop_r32(p + 8), (int)iop_r32(p + 12));
    iop_ret(c, id);
}
void iop_hle_DeleteSema(R5900 *c)
{
    iop_ret(c, sema_delete((int)iop_arg(c, 0)));
    maybe_resched();
}
void iop_hle_SignalSema(R5900 *c)
{
    iop_ret(c, iop_isignal_sema((int)iop_arg(c, 0)));
    maybe_resched();
}
void iop_hle_iSignalSema(R5900 *c)
{
    iop_ret(c, iop_isignal_sema((int)iop_arg(c, 0)));
}
void iop_hle_WaitSema(R5900 *c)
{
    iop_ret(c, sema_wait((int)iop_arg(c, 0)));
}
void iop_hle_PollSema(R5900 *c)
{
    IopSema *s = isema((int)iop_arg(c, 0));
    if (!s)
        iop_ret(c, KE_UNKNOWN_SEMID);
    else if (s->count <= 0)
        iop_ret(c, KE_SEMA_ZERO);
    else {
        s->count--;
        iop_ret(c, KE_OK);
    }
}
void iop_hle_ReferSemaStatus(R5900 *c)
{
    int id = (int)iop_arg(c, 0);
    IopSema *s = isema(id);
    uint32_t info = iop_arg(c, 1);
    if (!s) {
        iop_ret(c, KE_UNKNOWN_SEMID);
        return;
    }
    iop_w32(info + 0, s->attr);
    iop_w32(info + 4, s->option);
    iop_w32(info + 8, (uint32_t)s->initial);
    iop_w32(info + 12, (uint32_t)s->max);
    iop_w32(info + 16, (uint32_t)s->count);
    iop_w32(info + 20, (uint32_t)sema_waiters(id));
    iop_ret(c, KE_OK);
}

/* thevent */
void iop_hle_CreateEventFlag(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    int i;
    for (i = 1; i < MAX_IOP_EVENTS; i++) {
        if (!g_ievents[i].used) {
            IopEvent *e = &g_ievents[i];
            e->used = 1;
            e->attr = iop_r32(p);
            e->option = iop_r32(p + 4);
            e->bits = e->initial = iop_r32(p + 8);
            iop_ret(c, i);
            return;
        }
    }
    iop_ret(c, KE_NO_MEMORY);
}
void iop_hle_DeleteEventFlag(R5900 *c)
{
    int id = (int)iop_arg(c, 0);
    Thread *w;
    if (!ievent(id)) {
        iop_ret(c, KE_UNKNOWN_EVFID);
        return;
    }
    while ((w = kern_first_waiter(WAIT_IOP_EVENT, id))) {
        w->wait_result = KE_WAIT_DELETE;
        kern_wake(w);
    }
    g_ievents[id].used = 0;
    iop_ret(c, KE_OK);
    maybe_resched();
}
void iop_hle_SetEventFlag(R5900 *c)
{
    iop_ret(c, iop_iset_event((int)iop_arg(c, 0), iop_arg(c, 1)));
    maybe_resched();
}
void iop_hle_iSetEventFlag(R5900 *c)
{
    iop_ret(c, iop_iset_event((int)iop_arg(c, 0), iop_arg(c, 1)));
}
void iop_hle_ClearEventFlag(R5900 *c)
{
    IopEvent *e = ievent((int)iop_arg(c, 0));
    if (!e) {
        iop_ret(c, KE_UNKNOWN_EVFID);
        return;
    }
    e->bits &= iop_arg(c, 1);
    iop_ret(c, KE_OK);
}
void iop_hle_WaitEventFlag(R5900 *c)
{
    iop_ret(c, event_wait((int)iop_arg(c, 0), iop_arg(c, 1), iop_arg(c, 2), iop_arg(c, 3), 0));
}
void iop_hle_PollEventFlag(R5900 *c)
{
    iop_ret(c, event_wait((int)iop_arg(c, 0), iop_arg(c, 1), iop_arg(c, 2), iop_arg(c, 3), 1));
}
void iop_hle_ReferEventFlagStatus(R5900 *c)
{
    IopEvent *e = ievent((int)iop_arg(c, 0));
    uint32_t info = iop_arg(c, 1);
    if (!e) {
        iop_ret(c, KE_UNKNOWN_EVFID);
        return;
    }
    iop_w32(info + 0, e->attr);
    iop_w32(info + 4, e->option);
    iop_w32(info + 8, e->initial);
    iop_w32(info + 12, e->bits);
    iop_w32(info + 16, 0);
    iop_ret(c, KE_OK);
}

/* intrman: interrupts are delivered only between guest threads' kernel calls,
 * so masking them is a no-op */
void iop_hle_CpuSuspendIntr(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    if (p)
        iop_w32(p, 1);
    iop_ret(c, KE_OK);
}
void iop_hle_ok(R5900 *c)
{
    iop_ret(c, KE_OK);
}
void iop_hle_QueryIntrContext(R5900 *c)
{
    iop_ret(c, iop_in_irq());
}

/* sysmem */
void iop_hle_AllocSysMemory(R5900 *c)
{
    int mode = (int)iop_arg(c, 0);
    uint32_t size = iop_arg(c, 1), want = iop_arg(c, 2);
    uint32_t a = iop_heap_alloc_mode(mode, size, want);
    if (g_trace)
        LOG("IOP AllocSysMemory(%d, %X) = %08X", mode, size, a);
    if (!a)
        LOG("IOP AllocSysMemory(%d, %X) failed: largest free block %X", mode, size, heap_max_free());
    iop_ret(c, (int32_t)a);
}
void iop_hle_FreeSysMemory(R5900 *c)
{
    iop_heap_free(iop_arg(c, 0));
    iop_ret(c, KE_OK);
}
void iop_hle_QueryMaxFreeMemSize(R5900 *c)
{
    iop_ret(c, (int32_t)heap_max_free());
}
void iop_hle_QueryTotalFreeMemSize(R5900 *c)
{
    iop_ret(c, (int32_t)heap_total_free());
}
void iop_hle_QueryMemSize(R5900 *c)
{
    iop_ret(c, (int32_t)IOP_RAM_SIZE);
}

/* ---- module loading ------------------------------------------------------------------------- */

typedef struct LoadJob {
    const IopModuleImage *m;
    void (*done)(void *, int32_t);
    void *user;
} LoadJob;

static void module_started(void *thread, uint32_t ret)
{
    LoadJob *j = iop_thread_user(thread);
    LOG("IOP module %s started: %s", j->m->name,
        (ret & 3) == 0 ? "resident" : (ret & 3) == 1 ? "not resident" : "removable");
    if (j->done)
        j->done(j->user, (int32_t)ret);
    free(j);
}

static const IopModuleImage *g_loaded[32];

int iop_module_loaded(const IopModuleImage *m)
{
    int i;
    for (i = 0; i < 32; i++)
        if (g_loaded[i] == m)
            return 1;
    return 0;
}

void iop_load_module(const IopModuleImage *m, const char *args, uint32_t args_len,
                     void (*done)(void *user, int32_t result), void *user)
{
    uint32_t argv, strs, k, argc = 1, off;
    LoadJob *j;
    int id;
    for (k = 0; k < 32; k++)
        if (!g_loaded[k]) {
            g_loaded[k] = m;
            break;
        }
    memcpy(iop_ram + m->base, m->image, m->size);
    if (m->memsz > m->size)
        memset(iop_ram + m->base + m->size, 0, m->memsz - m->size);
    /* argv: module name then the NUL-separated argument strings */
    strs = iop_heap_alloc(64 + args_len + 1);
    argv = iop_heap_alloc(4 * 34);
    iop_write_str(strs, m->name);
    iop_w32(argv, strs);
    off = (uint32_t)strlen(m->name) + 1;
    for (k = 0; k < args_len && argc < 32;) {
        uint32_t n = (uint32_t)strnlen(args + k, args_len - k);
        char tmp[256];
        if (n >= sizeof tmp)
            n = sizeof tmp - 1;
        memcpy(tmp, args + k, n);
        tmp[n] = 0;
        iop_write_str(strs + off, tmp);
        iop_w32(argv + 4 * argc++, strs + off);
        off += n + 1;
        k += n + 1;
    }
    iop_w32(argv + 4 * argc, 0);
    j = calloc(1, sizeof *j);
    j->m = m;
    j->done = done;
    j->user = user;
    id = iop_create_thread(m->entry, 8, 0x4000, 0, 0x02000000u);
    LOG("IOP load %s at %08X (%u args)", m->name, m->base, argc - 1);
    g_threads[id].ctx.r[5][0] = argv;
    iop_start_thread(id, argc, module_started, j);
    g_threads[id].ctx.r[5][0] = argv;
}

void iop_kernel_init(void)
{
    memset(g_isemas, 0, sizeof g_isemas);
    memset(g_ievents, 0, sizeof g_ievents);
    heap_init();
    g_iop_irq_ctx.iop = 1;
}
