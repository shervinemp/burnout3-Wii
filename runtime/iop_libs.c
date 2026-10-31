/*
 * IOP library exports used by the recompiled modules: stdio, sysclib, ioman,
 * loadcore, and the table that resolves a module's imports (library name +
 * export ordinal) to native functions.
 *
 * The API names, ordinals and return conventions follow ps2sdk's libsd,
 * libmc, libpad and libkernel; no ps2sdk source is included. See CREDITS.md.
 */
#include "iop.h"
#include "kernel_int.h"

#include <string.h>
#include <ctype.h>

/* ---- printf ---------------------------------------------------------------------------- */

typedef struct ArgSrc {
    R5900 *c;
    int next;            /* argument index for register/stack arguments */
    uint32_t va;         /* va_list pointer, or 0 */
} ArgSrc;

static uint32_t next_arg(ArgSrc *a)
{
    if (a->va) {
        uint32_t v = iop_r32(a->va);
        a->va += 4;
        return v;
    }
    return iop_arg(a->c, a->next++);
}

static int format(char *out, size_t size, uint32_t fmt, ArgSrc *a)
{
    size_t n = 0;
    char spec[32], tmp[300];
    for (;;) {
        char ch = (char)iop_r8(fmt++);
        size_t k = 0;
        int len;
        if (!ch)
            break;
        if (ch != '%') {
            if (n + 1 < size)
                out[n] = ch;
            n++;
            continue;
        }
        spec[k++] = '%';
        for (;;) {
            ch = (char)iop_r8(fmt++);
            if (!ch || k >= sizeof spec - 2)
                break;
            if (ch == 'l' || ch == 'h')
                continue;              /* 32-bit target: length modifiers do not matter */
            if (ch == '*') {
                k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", (int)next_arg(a));
                continue;
            }
            spec[k++] = ch;
            if (strchr("diouxXcspn%", ch))
                break;
        }
        spec[k] = 0;
        if (!ch)
            break;
        switch (ch) {
        case 's': {
            char s[256];
            uint32_t p = next_arg(a);
            if (p)
                iop_read_str(s, p, sizeof s);
            else
                strcpy(s, "(null)");
            len = snprintf(tmp, sizeof tmp, spec, s);
            break;
        }
        case 'p':
            len = snprintf(tmp, sizeof tmp, "%08x", next_arg(a));
            break;
        case 'n':
            next_arg(a);
            len = 0;
            break;
        case '%':
            len = snprintf(tmp, sizeof tmp, "%%");
            break;
        case 'd': case 'i': case 'c':
            len = snprintf(tmp, sizeof tmp, spec, (int)next_arg(a));
            break;
        default:
            len = snprintf(tmp, sizeof tmp, spec, next_arg(a));
            break;
        }
        if (len > 0) {
            int j;
            for (j = 0; j < len && j < (int)sizeof tmp - 1; j++) {
                if (n + 1 < size)
                    out[n] = tmp[j];
                n++;
            }
        }
    }
    if (size)
        out[n < size ? n : size - 1] = 0;
    return (int)n;
}

int iop_vsformat(char *out, size_t size, uint32_t fmt, R5900 *c, int first_arg)
{
    ArgSrc a = {c, first_arg, 0};
    return format(out, size, fmt, &a);
}

static void log_text(const char *s)
{
    static char line[512];
    static size_t len;
    for (; *s; s++) {
        if (*s == '\n' || len == sizeof line - 1) {
            line[len] = 0;
            LOG("iop%s%d: %s", iop_in_irq() ? "-irq" : "-t", g_cur ? g_cur->id : -1, line);
            len = 0;
            if (*s == '\n')
                continue;
        }
        if (*s != '\r')
            line[len++] = *s;
    }
}

static void hle_printf(R5900 *c)
{
    char buf[1024];
    int n = iop_vsformat(buf, sizeof buf, iop_arg(c, 0), c, 1);
    log_text(buf);
    iop_ret(c, n);
}

static void hle_puts(R5900 *c)
{
    char buf[512];
    iop_read_str(buf, iop_arg(c, 0), sizeof buf);
    log_text(buf);
    log_text("\n");
    iop_ret(c, 0);
}

/* ---- sysclib ------------------------------------------------------------------------------ */

static uint32_t istrlen(uint32_t p)
{
    uint32_t n = 0;
    while (iop_r8(p + n))
        n++;
    return n;
}

static void hle_memcpy(R5900 *c)
{
    iop_to_iop(iop_arg(c, 0), iop_arg(c, 1), iop_arg(c, 2));
    iop_ret(c, (int32_t)iop_arg(c, 0));
}
static void hle_bcopy(R5900 *c)
{
    iop_to_iop(iop_arg(c, 1), iop_arg(c, 0), iop_arg(c, 2));
}
static void hle_memset(R5900 *c)
{
    iop_memset(iop_arg(c, 0), (uint8_t)iop_arg(c, 1), iop_arg(c, 2));
    iop_ret(c, (int32_t)iop_arg(c, 0));
}
static void hle_bzero(R5900 *c)
{
    iop_memset(iop_arg(c, 0), 0, iop_arg(c, 1));
}
static void hle_memcmp(R5900 *c)
{
    uint32_t a = iop_arg(c, 0), b = iop_arg(c, 1), n = iop_arg(c, 2), k;
    for (k = 0; k < n; k++) {
        int d = iop_r8(a + k) - iop_r8(b + k);
        if (d) {
            iop_ret(c, d);
            return;
        }
    }
    iop_ret(c, 0);
}
static void hle_strlen(R5900 *c)
{
    iop_ret(c, (int32_t)istrlen(iop_arg(c, 0)));
}
static void hle_strcpy(R5900 *c)
{
    uint32_t d = iop_arg(c, 0), s = iop_arg(c, 1);
    uint8_t ch;
    do {
        ch = iop_r8(s++);
        iop_w8(d++, ch);
    } while (ch);
    iop_ret(c, (int32_t)iop_arg(c, 0));
}
static void hle_strncpy(R5900 *c)
{
    uint32_t d = iop_arg(c, 0), s = iop_arg(c, 1), n = iop_arg(c, 2), k;
    int end = 0;
    for (k = 0; k < n; k++) {
        uint8_t ch = end ? 0 : iop_r8(s + k);
        if (!ch)
            end = 1;
        iop_w8(d + k, ch);
    }
    iop_ret(c, (int32_t)d);
}
static void hle_strcat(R5900 *c)
{
    uint32_t d = iop_arg(c, 0), s = iop_arg(c, 1);
    uint32_t e = d + istrlen(d);
    uint8_t ch;
    do {
        ch = iop_r8(s++);
        iop_w8(e++, ch);
    } while (ch);
    iop_ret(c, (int32_t)d);
}
static void hle_strncat(R5900 *c)
{
    uint32_t d = iop_arg(c, 0), s = iop_arg(c, 1), n = iop_arg(c, 2), k;
    uint32_t e = d + istrlen(d);
    for (k = 0; k < n; k++) {
        uint8_t ch = iop_r8(s + k);
        if (!ch)
            break;
        iop_w8(e++, ch);
    }
    iop_w8(e, 0);
    iop_ret(c, (int32_t)d);
}
static int cmp_n(uint32_t a, uint32_t b, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        uint8_t x = iop_r8(a + k), y = iop_r8(b + k);
        if (x != y)
            return x - y;
        if (!x)
            break;
    }
    return 0;
}
static void hle_strcmp(R5900 *c)
{
    iop_ret(c, cmp_n(iop_arg(c, 0), iop_arg(c, 1), 0xFFFFFFFFu));
}
static void hle_strncmp(R5900 *c)
{
    iop_ret(c, cmp_n(iop_arg(c, 0), iop_arg(c, 1), iop_arg(c, 2)));
}
static void hle_strchr(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    uint8_t want = (uint8_t)iop_arg(c, 1);
    for (;; p++) {
        uint8_t ch = iop_r8(p);
        if (ch == want) {
            iop_ret(c, (int32_t)p);
            return;
        }
        if (!ch)
            break;
    }
    iop_ret(c, 0);
}
static void hle_strrchr(R5900 *c)
{
    uint32_t p = iop_arg(c, 0), found = 0;
    uint8_t want = (uint8_t)iop_arg(c, 1);
    for (;; p++) {
        uint8_t ch = iop_r8(p);
        if (ch == want)
            found = p;
        if (!ch)
            break;
    }
    iop_ret(c, (int32_t)found);
}
static void hle_strstr(R5900 *c)
{
    uint32_t h = iop_arg(c, 0), nd = iop_arg(c, 1), n = istrlen(nd);
    for (; iop_r8(h); h++) {
        if (!cmp_n(h, nd, n)) {
            iop_ret(c, (int32_t)h);
            return;
        }
    }
    iop_ret(c, n ? 0 : (int32_t)iop_arg(c, 0));
}
static void hle_toupper(R5900 *c)
{
    iop_ret(c, toupper((int)(iop_arg(c, 0) & 0xFF)));
}
static void hle_tolower(R5900 *c)
{
    iop_ret(c, tolower((int)(iop_arg(c, 0) & 0xFF)));
}
static void hle_strtol(R5900 *c)
{
    char buf[64];
    char *end;
    long v;
    iop_read_str(buf, iop_arg(c, 0), sizeof buf);
    v = strtol(buf, &end, (int)iop_arg(c, 2));
    if (iop_arg(c, 1))
        iop_w32(iop_arg(c, 1), iop_arg(c, 0) + (uint32_t)(end - buf));
    iop_ret(c, (int32_t)v);
}
static void hle_sprintf(R5900 *c)
{
    char buf[1024];
    int n = iop_vsformat(buf, sizeof buf, iop_arg(c, 1), c, 2);
    iop_write_str(iop_arg(c, 0), buf);
    iop_ret(c, n);
}
static void hle_vsprintf(R5900 *c)
{
    char buf[1024];
    ArgSrc a = {c, 0, iop_arg(c, 2)};
    int n = format(buf, sizeof buf, iop_arg(c, 1), &a);
    iop_write_str(iop_arg(c, 0), buf);
    iop_ret(c, n);
}
static void hle_strtok(R5900 *c)
{
    static uint32_t save;
    uint32_t s = iop_arg(c, 0), delim = iop_arg(c, 1), start;
    char d[64];
    iop_read_str(d, delim, sizeof d);
    if (!s)
        s = save;
    if (!s) {
        iop_ret(c, 0);
        return;
    }
    while (iop_r8(s) && strchr(d, iop_r8(s)))
        s++;
    if (!iop_r8(s)) {
        save = 0;
        iop_ret(c, 0);
        return;
    }
    start = s;
    while (iop_r8(s) && !strchr(d, iop_r8(s)))
        s++;
    if (iop_r8(s)) {
        iop_w8(s, 0);
        save = s + 1;
    } else {
        save = 0;
    }
    iop_ret(c, (int32_t)start);
}

/* ---- ioman: files from the disc ------------------------------------------------------------ */

#define MAX_FILES 32
static DiscFile *g_files[MAX_FILES];
static int64_t g_fpos[MAX_FILES];

static void disc_path(char *out, size_t size, const char *in)
{
    const char *p = strchr(in, ':');
    size_t n = 0;
    p = p ? p + 1 : in;
    while (*p == '\\' || *p == '/')
        p++;
    for (; *p && *p != ';' && n + 1 < size; p++)
        out[n++] = *p == '\\' ? '/' : *p;
    out[n] = 0;
}

static void hle_open(R5900 *c)
{
    char path[256], rel[256];
    int fd;
    iop_read_str(path, iop_arg(c, 0), sizeof path);
    disc_path(rel, sizeof rel, path);
    for (fd = 1; fd < MAX_FILES && g_files[fd]; fd++)
        ;
    if (fd == MAX_FILES || !(g_files[fd] = disc_open(rel))) {
        LOG("IOP open(%s, %X) failed", path, iop_arg(c, 1));
        iop_ret(c, -1);
        return;
    }
    g_fpos[fd] = 0;
    if (g_trace)
        LOG("IOP open(%s) = %d", path, fd);
    iop_ret(c, fd);
}
static void hle_close(R5900 *c)
{
    int fd = (int)iop_arg(c, 0);
    if (fd > 0 && fd < MAX_FILES && g_files[fd]) {
        disc_close(g_files[fd]);
        g_files[fd] = NULL;
        iop_ret(c, 0);
    } else {
        iop_ret(c, -1);
    }
}
static void hle_read(R5900 *c)
{
    int fd = (int)iop_arg(c, 0);
    uint32_t buf = iop_arg(c, 1), size = iop_arg(c, 2);
    uint8_t tmp[4096];
    int64_t total = 0;
    if (fd <= 0 || fd >= MAX_FILES || !g_files[fd]) {
        iop_ret(c, -1);
        return;
    }
    while (size) {
        uint32_t chunk = size < sizeof tmp ? size : (uint32_t)sizeof tmp;
        /* (the calling thread waits as a guest thread while the card is read: see iop_hle_sceCdRead) */
        int64_t got = disc_read_guest(g_files[fd], tmp, chunk, g_fpos[fd]);
        if (got <= 0)
            break;
        iop_write_bytes(buf, tmp, (uint32_t)got);
        buf += (uint32_t)got;
        size -= (uint32_t)got;
        g_fpos[fd] += got;
        total += got;
        if ((uint32_t)got < chunk)
            break;
    }
    iop_ret(c, (int32_t)total);
}
static void hle_lseek(R5900 *c)
{
    int fd = (int)iop_arg(c, 0);
    int32_t off = (int32_t)iop_arg(c, 1);
    int whence = (int)iop_arg(c, 2);
    if (fd <= 0 || fd >= MAX_FILES || !g_files[fd]) {
        iop_ret(c, -1);
        return;
    }
    if (whence == 0)
        g_fpos[fd] = off;
    else if (whence == 1)
        g_fpos[fd] += off;
    else
        g_fpos[fd] = disc_size(g_files[fd]) + off;
    iop_ret(c, (int32_t)g_fpos[fd]);
}
static void hle_write(R5900 *c)
{
    LOG("IOP write(%d, %u bytes) ignored", (int)iop_arg(c, 0), iop_arg(c, 2));
    iop_ret(c, (int32_t)iop_arg(c, 2));
}

/* ---- loadcore ---------------------------------------------------------------------------- */

/* Libraries exported by recompiled modules. An export table is {magic,
 * next, version, mode, name[8], functions..., 0}; imports of a library that
 * has no native implementation call into these. */
#define MAX_GUEST_LIBS 32
typedef struct GuestLib {
    char name[9];
    uint32_t table;
    int count;
} GuestLib;
static GuestLib g_libs[MAX_GUEST_LIBS];
static int g_nlibs;

static GuestLib *find_guest_lib(const char *name)
{
    int i;
    for (i = 0; i < g_nlibs; i++)
        if (!strcmp(g_libs[i].name, name))
            return &g_libs[i];
    return NULL;
}

static void hle_register_library(R5900 *c)
{
    char name[9];
    uint32_t e = iop_arg(c, 0);
    GuestLib *l;
    int n = 0;
    iop_read_bytes(name, e + 12, 8);
    name[8] = 0;
    if (find_guest_lib(name)) {
        LOG("IOP RegisterLibraryEntries(%s): already registered", name);
        iop_ret(c, -1);
        return;
    }
    if (g_nlibs == MAX_GUEST_LIBS)
        FATAL("too many IOP libraries");
    while (n < 256 && iop_r32(e + 20 + 4 * (uint32_t)n))
        n++;
    l = &g_libs[g_nlibs++];
    memcpy(l->name, name, sizeof l->name);
    l->table = e + 20;
    l->count = n;
    LOG("IOP RegisterLibraryEntries(%s): %d functions", name, n);
    iop_ret(c, 0);
}

/* ---- intrman: handlers run on the interrupt context at the next kernel tick ---------------- */

#define IOP_IRQS 64
static uint32_t g_intr_func[IOP_IRQS], g_intr_arg[IOP_IRQS];
static uint64_t g_intr_pending, g_intr_enabled;

void iop_raise_irq(int irq)
{
    g_intr_pending |= 1ull << irq;
}

int iop_intr_pending(void)
{
    return (g_intr_pending & g_intr_enabled) != 0;
}

void iop_intr_tick(void)
{
    uint64_t p;
    while ((p = g_intr_pending & g_intr_enabled) != 0) {
        int irq = 0;
        while (!(p & (1ull << irq)))
            irq++;
        g_intr_pending &= ~(1ull << irq);
        if (g_intr_func[irq])
            iop_call_irq(g_intr_func[irq], g_intr_arg[irq], 0, 0);
    }
}

/* RegisterIntrHandler(irq, mode, handler, arg) */
static void hle_RegisterIntrHandler(R5900 *c)
{
    uint32_t irq = iop_arg(c, 0);
    if (irq >= IOP_IRQS) {
        iop_ret(c, -1);
        return;
    }
    g_intr_func[irq] = iop_arg(c, 2);
    g_intr_arg[irq] = iop_arg(c, 3);
    LOG("IOP interrupt %u -> %s", irq, iop_func_name(g_intr_func[irq]));
    iop_ret(c, 0);
}

static void hle_ReleaseIntrHandler(R5900 *c)
{
    uint32_t irq = iop_arg(c, 0);
    if (irq < IOP_IRQS)
        g_intr_func[irq] = 0;
    iop_ret(c, 0);
}

static void hle_EnableIntr(R5900 *c)
{
    uint32_t irq = iop_arg(c, 0) & 0xFF;
    if (irq < IOP_IRQS)
        g_intr_enabled |= 1ull << irq;
    iop_ret(c, 0);
}

static void hle_DisableIntr(R5900 *c)
{
    uint32_t irq = iop_arg(c, 0) & 0xFF, res = iop_arg(c, 1);
    if (irq < IOP_IRQS)
        g_intr_enabled &= ~(1ull << irq);
    if (res)
        iop_w32(res, irq);
    iop_ret(c, 0);
}

/* ---- dmacman: only the SIO2 channels are used by recompiled modules ------------------------ */

static uint32_t g_dma_addr[16], g_dma_bs[16], g_dma_bc[16];

/* dmac_request(ch, addr, block_words, blocks, dir) */
static void hle_dmac_request(R5900 *c)
{
    uint32_t ch = iop_arg(c, 0);
    if (ch < 16) {
        g_dma_addr[ch] = iop_arg(c, 1);
        g_dma_bs[ch] = iop_arg(c, 2);
        g_dma_bc[ch] = iop_arg(c, 3);
    }
    iop_ret(c, 1);
}

/* dmac_transfer(ch): SIO2 transfers complete at once */
static void hle_dmac_transfer(R5900 *c)
{
    uint32_t ch = iop_arg(c, 0);
    if (ch == 11 || ch == 12)
        iop_sio2_dma((int)ch, g_dma_addr[ch], g_dma_bs[ch], g_dma_bc[ch]);
    else
        LOG("IOP dmac_transfer on channel %u ignored", ch);
    iop_ret(c, 1);
}

/* ---- vblank: waits follow the EE's vertical blank clock ----------------------------------- */

static void hle_WaitVblankStart(R5900 *c)
{
    iop_delay_until(kernel_next_vblank_us());
    iop_ret(c, 0);
}

static void hle_WaitVblankEnd(R5900 *c)
{
    iop_delay_until(kernel_next_vblank_us() + 1400);   /* vertical blanking lasts ~22 lines */
    iop_ret(c, 0);
}

/* ---- secrman: memory card authentication always succeeds --------------------------------- */

static void hle_SecrAuthCard(R5900 *c)
{
    iop_ret(c, 1);
}

/* ---- import resolution ------------------------------------------------------------------ */

void iop_hle_CreateThread(R5900 *c);
void iop_hle_DeleteThread(R5900 *c);
void iop_hle_StartThread(R5900 *c);
void iop_hle_ExitThread(R5900 *c);
void iop_hle_ExitDeleteThread(R5900 *c);
void iop_hle_TerminateThread(R5900 *c);
void iop_hle_ChangeThreadPriority(R5900 *c);
void iop_hle_RotateThreadReadyQueue(R5900 *c);
void iop_hle_GetThreadId(R5900 *c);
void iop_hle_ReferThreadStatus(R5900 *c);
void iop_hle_SleepThread(R5900 *c);
void iop_hle_WakeupThread(R5900 *c);
void iop_hle_iWakeupThread(R5900 *c);
void iop_hle_CancelWakeupThread(R5900 *c);
void iop_hle_DelayThread(R5900 *c);
void iop_hle_GetSystemTime(R5900 *c);
void iop_hle_GetSystemTimeLow(R5900 *c);
void iop_hle_USec2SysClock(R5900 *c);
void iop_hle_SysClock2USec(R5900 *c);
void iop_hle_CreateSema(R5900 *c);
void iop_hle_DeleteSema(R5900 *c);
void iop_hle_SignalSema(R5900 *c);
void iop_hle_iSignalSema(R5900 *c);
void iop_hle_WaitSema(R5900 *c);
void iop_hle_PollSema(R5900 *c);
void iop_hle_ReferSemaStatus(R5900 *c);
void iop_hle_CreateEventFlag(R5900 *c);
void iop_hle_DeleteEventFlag(R5900 *c);
void iop_hle_SetEventFlag(R5900 *c);
void iop_hle_iSetEventFlag(R5900 *c);
void iop_hle_ClearEventFlag(R5900 *c);
void iop_hle_WaitEventFlag(R5900 *c);
void iop_hle_PollEventFlag(R5900 *c);
void iop_hle_ReferEventFlagStatus(R5900 *c);
void iop_hle_CpuSuspendIntr(R5900 *c);
void iop_hle_ok(R5900 *c);
void iop_hle_QueryIntrContext(R5900 *c);
void iop_hle_AllocSysMemory(R5900 *c);
void iop_hle_FreeSysMemory(R5900 *c);
void iop_hle_QueryMaxFreeMemSize(R5900 *c);
void iop_hle_QueryTotalFreeMemSize(R5900 *c);
void iop_hle_QueryMemSize(R5900 *c);
/* iop_sif.c */
void iop_hle_sceSifInitCmd(R5900 *c);
void iop_hle_sceSifAddCmdHandler(R5900 *c);
void iop_hle_sceSifRemoveCmdHandler(R5900 *c);
void iop_hle_sceSifSendCmd(R5900 *c);
void iop_hle_sceSifRegisterRpc(R5900 *c);
void iop_hle_sceSifSetRpcQueue(R5900 *c);
void iop_hle_sceSifRpcLoop(R5900 *c);
void iop_hle_sceSifSetDma(R5900 *c);
void iop_hle_sceSifDmaStat(R5900 *c);
void iop_hle_sceSifSetDmaIntr(R5900 *c);
void iop_hle_sceSifCheckInit(R5900 *c);
/* iop_cdvd.c */
void iop_hle_sceCdRead(R5900 *c);
void iop_hle_sceCdSync(R5900 *c);
void iop_hle_sceCdGetError(R5900 *c);
void iop_hle_sceCdGetDiskType(R5900 *c);
void iop_hle_sceCdDiskReady(R5900 *c);
void iop_hle_sceCdReadClock(R5900 *c);
/* iop_sd.c */
void iop_hle_sceSdInit(R5900 *c);
void iop_hle_sceSdSetParam(R5900 *c);
void iop_hle_sceSdGetParam(R5900 *c);
void iop_hle_sceSdSetSwitch(R5900 *c);
void iop_hle_sceSdGetSwitch(R5900 *c);
void iop_hle_sceSdSetAddr(R5900 *c);
void iop_hle_sceSdGetAddr(R5900 *c);
void iop_hle_sceSdSetCoreAttr(R5900 *c);
void iop_hle_sceSdGetCoreAttr(R5900 *c);
void iop_hle_sceSdVoiceTrans(R5900 *c);
void iop_hle_sceSdVoiceTransStatus(R5900 *c);
void iop_hle_sceSdSetEffectAttr(R5900 *c);
void iop_hle_sceSdGetEffectAttr(R5900 *c);
void iop_hle_sceSdClearEffectWorkArea(R5900 *c);
void iop_hle_sceSdSetTransIntrHandler(R5900 *c);
void iop_hle_sceSdSetSpu2IntrHandler(R5900 *c);
void iop_hle_sceSd28(R5900 *c);
void iop_hle_sceSd29(R5900 *c);

typedef struct ImportDef {
    const char *lib;
    int ord;
    IopHle fn;
} ImportDef;

static const ImportDef k_imports[] = {
    {"sysmem", 4, iop_hle_AllocSysMemory}, {"sysmem", 5, iop_hle_FreeSysMemory},
    {"sysmem", 6, iop_hle_QueryMemSize}, {"sysmem", 7, iop_hle_QueryMaxFreeMemSize},
    {"sysmem", 8, iop_hle_QueryTotalFreeMemSize}, {"sysmem", 14, hle_printf},
    {"loadcore", 4, iop_hle_ok}, {"loadcore", 5, iop_hle_ok},     /* FlushIcache, FlushDcache */
    {"loadcore", 6, hle_register_library}, {"loadcore", 7, iop_hle_ok},
    {"intrman", 4, hle_RegisterIntrHandler}, {"intrman", 5, hle_ReleaseIntrHandler},
    {"intrman", 6, hle_EnableIntr}, {"intrman", 7, hle_DisableIntr},
    {"intrman", 8, iop_hle_ok}, {"intrman", 9, iop_hle_ok},
    {"dmacman", 28, hle_dmac_request}, {"dmacman", 32, hle_dmac_transfer},
    {"dmacman", 33, iop_hle_ok}, {"dmacman", 34, iop_hle_ok}, {"dmacman", 35, iop_hle_ok},
    {"vblank", 4, hle_WaitVblankStart}, {"vblank", 5, hle_WaitVblankEnd},
    {"secrman", 4, iop_hle_ok}, {"secrman", 5, iop_hle_ok}, {"secrman", 6, hle_SecrAuthCard},
    {"secrman", 7, iop_hle_ok},
    {"intrman", 17, iop_hle_CpuSuspendIntr}, {"intrman", 18, iop_hle_ok},
    {"intrman", 23, iop_hle_QueryIntrContext},
    {"thbase", 4, iop_hle_CreateThread}, {"thbase", 5, iop_hle_DeleteThread},
    {"thbase", 6, iop_hle_StartThread}, {"thbase", 8, iop_hle_ExitThread},
    {"thbase", 9, iop_hle_ExitDeleteThread}, {"thbase", 10, iop_hle_TerminateThread},
    {"thbase", 11, iop_hle_TerminateThread}, {"thbase", 14, iop_hle_ChangeThreadPriority},
    {"thbase", 15, iop_hle_ChangeThreadPriority}, {"thbase", 16, iop_hle_RotateThreadReadyQueue},
    {"thbase", 20, iop_hle_GetThreadId}, {"thbase", 22, iop_hle_ReferThreadStatus},
    {"thbase", 23, iop_hle_ReferThreadStatus}, {"thbase", 24, iop_hle_SleepThread},
    {"thbase", 25, iop_hle_WakeupThread}, {"thbase", 26, iop_hle_iWakeupThread},
    {"thbase", 27, iop_hle_CancelWakeupThread}, {"thbase", 28, iop_hle_CancelWakeupThread},
    {"thbase", 33, iop_hle_DelayThread}, {"thbase", 34, iop_hle_GetSystemTime},
    {"thbase", 39, iop_hle_USec2SysClock}, {"thbase", 40, iop_hle_SysClock2USec},
    {"thbase", 43, iop_hle_GetSystemTimeLow},
    {"thsemap", 4, iop_hle_CreateSema}, {"thsemap", 5, iop_hle_DeleteSema},
    {"thsemap", 6, iop_hle_SignalSema}, {"thsemap", 7, iop_hle_iSignalSema},
    {"thsemap", 8, iop_hle_WaitSema}, {"thsemap", 9, iop_hle_PollSema},
    {"thsemap", 10, iop_hle_ReferSemaStatus}, {"thsemap", 11, iop_hle_ReferSemaStatus},
    {"thevent", 4, iop_hle_CreateEventFlag}, {"thevent", 5, iop_hle_DeleteEventFlag},
    {"thevent", 6, iop_hle_SetEventFlag}, {"thevent", 7, iop_hle_iSetEventFlag},
    {"thevent", 8, iop_hle_ClearEventFlag}, {"thevent", 9, iop_hle_ClearEventFlag},
    {"thevent", 10, iop_hle_WaitEventFlag}, {"thevent", 11, iop_hle_PollEventFlag},
    {"thevent", 12, iop_hle_ReferEventFlagStatus}, {"thevent", 13, iop_hle_ReferEventFlagStatus},
    {"stdio", 4, hle_printf}, {"stdio", 7, hle_puts},
    {"sysclib", 6, hle_toupper}, {"sysclib", 7, hle_tolower}, {"sysclib", 11, hle_memcmp},
    {"sysclib", 12, hle_memcpy}, {"sysclib", 13, hle_memcpy}, {"sysclib", 14, hle_memset},
    {"sysclib", 15, hle_memcmp}, {"sysclib", 16, hle_bcopy}, {"sysclib", 17, hle_bzero},
    {"sysclib", 19, hle_sprintf}, {"sysclib", 20, hle_strcat}, {"sysclib", 21, hle_strchr},
    {"sysclib", 22, hle_strcmp}, {"sysclib", 23, hle_strcpy}, {"sysclib", 25, hle_strchr},
    {"sysclib", 26, hle_strrchr}, {"sysclib", 27, hle_strlen}, {"sysclib", 28, hle_strncat},
    {"sysclib", 29, hle_strncmp}, {"sysclib", 30, hle_strncpy}, {"sysclib", 32, hle_strrchr},
    {"sysclib", 34, hle_strstr}, {"sysclib", 35, hle_strtok}, {"sysclib", 36, hle_strtol},
    {"sysclib", 38, hle_strtol}, {"sysclib", 42, hle_vsprintf},
    {"ioman", 4, hle_open}, {"ioman", 5, hle_close}, {"ioman", 6, hle_read}, {"ioman", 7, hle_write},
    {"ioman", 8, hle_lseek},
    {"sifcmd", 4, iop_hle_sceSifInitCmd}, {"sifcmd", 8, iop_hle_ok}, {"sifcmd", 9, iop_hle_ok},
    {"sifcmd", 10, iop_hle_sceSifAddCmdHandler}, {"sifcmd", 11, iop_hle_sceSifRemoveCmdHandler},
    {"sifcmd", 12, iop_hle_sceSifSendCmd}, {"sifcmd", 13, iop_hle_sceSifSendCmd},
    {"sifcmd", 14, iop_hle_ok}, {"sifcmd", 17, iop_hle_sceSifRegisterRpc},
    {"sifcmd", 19, iop_hle_sceSifSetRpcQueue}, {"sifcmd", 22, iop_hle_sceSifRpcLoop},
    {"sifman", 5, iop_hle_ok}, {"sifman", 7, iop_hle_sceSifSetDma}, {"sifman", 8, iop_hle_sceSifDmaStat},
    {"sifman", 29, iop_hle_sceSifCheckInit}, {"sifman", 32, iop_hle_sceSifSetDmaIntr},
    {"cdvdman", 6, iop_hle_sceCdRead}, {"cdvdman", 8, iop_hle_sceCdGetError},
    {"cdvdman", 11, iop_hle_sceCdSync}, {"cdvdman", 12, iop_hle_sceCdGetDiskType},
    {"cdvdman", 13, iop_hle_sceCdDiskReady}, {"cdvdman", 24, iop_hle_sceCdReadClock},
    {"libsd", 4, iop_hle_sceSdInit}, {"libsd", 5, iop_hle_sceSdSetParam}, {"libsd", 6, iop_hle_sceSdGetParam},
    {"libsd", 7, iop_hle_sceSdSetSwitch}, {"libsd", 8, iop_hle_sceSdGetSwitch},
    {"libsd", 9, iop_hle_sceSdSetAddr}, {"libsd", 10, iop_hle_sceSdGetAddr},
    {"libsd", 11, iop_hle_sceSdSetCoreAttr}, {"libsd", 12, iop_hle_sceSdGetCoreAttr},
    {"libsd", 17, iop_hle_sceSdVoiceTrans}, {"libsd", 19, iop_hle_sceSdVoiceTransStatus},
    {"libsd", 23, iop_hle_sceSdSetEffectAttr}, {"libsd", 24, iop_hle_sceSdGetEffectAttr},
    {"libsd", 25, iop_hle_sceSdClearEffectWorkArea}, {"libsd", 26, iop_hle_sceSdSetTransIntrHandler},
    {"libsd", 27, iop_hle_sceSdSetSpu2IntrHandler}, {"libsd", 28, iop_hle_sceSd28},
    {"libsd", 29, iop_hle_sceSd29},
};

void iop_call(R5900 *c, uint32_t addr);

void iop_import(R5900 *c, IopImportSlot *s)
{
    if (!s->fn && !s->addr) {
        size_t i;
        GuestLib *l;
        for (i = 0; i < sizeof k_imports / sizeof k_imports[0]; i++) {
            if (k_imports[i].ord == s->ord && !strcmp(k_imports[i].lib, s->lib)) {
                s->fn = k_imports[i].fn;
                break;
            }
        }
        if (!s->fn && (l = find_guest_lib(s->lib)) != NULL && s->ord < l->count)
            s->addr = iop_r32(l->table + 4 * (uint32_t)s->ord);
        if (!s->fn && !s->addr) {
            LOG("IOP import %s #%d is not implemented (ra %s)", s->lib, s->ord, iop_func_name(c->r[31][0]));
            FATAL("unimplemented IOP import %s #%d", s->lib, s->ord);
        }
    }
    if (s->fn)
        s->fn(c);
    else
        iop_call(c, s->addr);
}

/* ---- IOP hardware -------------------------------------------------------------------------- */

static uint32_t g_iop_spad[0x100];   /* 1 KB scratchpad at 0x1F800000 */

uint32_t iop_io_read(uint32_t a, int size)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x1F800400u) {
        uint32_t w = g_iop_spad[(p >> 2) & 0xFF];
        return size == 4 ? w : size == 2 ? (w >> (8 * (p & 2))) & 0xFFFF : (w >> (8 * (p & 3))) & 0xFF;
    }
    if (p >= 0x1F808200u && p < 0x1F808300u)
        return iop_sio2_read(p);
    LOG("IOP hardware read %08X (%d bytes)", a, size);
    return 0;
}

void iop_io_write(uint32_t a, uint32_t v, int size)
{
    uint32_t p = a & 0x1FFFFFFFu;
    if (p < 0x1F800400u) {
        uint32_t *w = &g_iop_spad[(p >> 2) & 0xFF];
        if (size == 4)
            *w = v;
        else if (size == 2)
            *w = (*w & ~(0xFFFFu << (8 * (p & 2)))) | (v & 0xFFFF) << (8 * (p & 2));
        else
            *w = (*w & ~(0xFFu << (8 * (p & 3)))) | (v & 0xFF) << (8 * (p & 3));
        return;
    }
    if (p >= 0x1F808200u && p < 0x1F808300u) {
        iop_sio2_write(p, v);
        return;
    }
    LOG("IOP hardware write %08X = %08X (%d bytes)", a, v, size);
    {
        R5900 *c = iop_current_ctx();
        if (c) {
            LOG("  ra %s", iop_func_name(c->r[31][0]));
            rt_dump_stack(c, "  IOP");
        }
    }
}

float iop_io_readf(uint32_t a)
{
    uint32_t v = iop_io_read(a, 4);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

void iop_io_writef(uint32_t a, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    iop_io_write(a, v, 4);
}
