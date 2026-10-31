/*
 * IOP side of the runtime: recompiled game modules (gen_iop/) running on a
 * native implementation of the IOP BIOS libraries they import.
 *
 * IOP RAM uses the same word layout as EE RAM (32-bit words in host order,
 * bytes found by XOR on big-endian hosts), so whole words copy between the
 * two with memcpy.
 */
#ifndef IOP_H
#define IOP_H

#include "rt.h"

#define IOP_MASK 0x001FFFFFu

/* IOP RAM map used by the runtime */
#define IOP_MODULE_BASE   0x00040000u   /* recompiled modules: see config/iop_modules.py */
#define IOP_HEAP_START    0x00100000u   /* AllocSysMemory */
#define IOP_HEAP_END      0x001F0000u
#define IOP_IRQ_STACK_TOP 0x001FFF00u   /* stack for IOP interrupt-context code */

static inline uint32_t iop_r32(uint32_t a) { return *(uint32_t *)(iop_ram + (a & IOP_MASK & ~3u)); }
static inline void iop_w32(uint32_t a, uint32_t v) { *(uint32_t *)(iop_ram + (a & IOP_MASK & ~3u)) = v; }
static inline uint16_t iop_r16(uint32_t a) { return *(uint16_t *)(iop_ram + (((a & IOP_MASK) ^ PS2_HXOR) & ~1u)); }
static inline void iop_w16(uint32_t a, uint16_t v) { *(uint16_t *)(iop_ram + (((a & IOP_MASK) ^ PS2_HXOR) & ~1u)) = v; }
static inline uint8_t iop_r8(uint32_t a) { return iop_ram[(a & IOP_MASK) ^ PS2_BXOR]; }
static inline void iop_w8(uint32_t a, uint8_t v) { iop_ram[(a & IOP_MASK) ^ PS2_BXOR] = v; }

void iop_read_str(char *dst, uint32_t addr, uint32_t max);
void iop_write_str(uint32_t addr, const char *s);
void iop_read_bytes(void *dst, uint32_t addr, uint32_t n);        /* IOP -> host byte stream */
void iop_write_bytes(uint32_t addr, const void *src, uint32_t n); /* host byte stream -> IOP */
void iop_memset(uint32_t addr, uint8_t v, uint32_t n);
void ee_to_iop(uint32_t iop_dst, uint32_t ee_src, uint32_t n);
void iop_to_ee(uint32_t ee_dst, uint32_t iop_src, uint32_t n);
void iop_to_iop(uint32_t dst, uint32_t src, uint32_t n);

/* ---- recompiled modules ---------------------------------------------------------- */
typedef struct IopModuleImage {
    const char *name;              /* file name without extension, upper case */
    uint32_t base;
    const uint32_t *image;
    uint32_t size, memsz, entry;
    const PS2FuncEntry *table;
    const unsigned *count;
} IopModuleImage;
extern const IopModuleImage *const iop_modules[];

const IopModuleImage *iop_find_module(const char *path);
PS2Func iop_lookup(uint32_t addr);
const char *iop_func_name(uint32_t addr);

/* calls from module code to another IOP library, resolved on first use: to a
 * native implementation, or to a function another recompiled module exported */
typedef void (*IopHle)(R5900 *c);
typedef struct IopImportSlot {
    const char *lib;
    int ord;
    IopHle fn;
    uint32_t addr;              /* guest export, when fn is NULL */
} IopImportSlot;
void iop_import(R5900 *c, IopImportSlot *s);

/* run guest IOP code from host code */
uint32_t iop_call_guest(R5900 *c, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t iop_call_irq(uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2);   /* on the IOP interrupt context */
int iop_in_irq(void);
R5900 *iop_current_ctx(void);

/* argument / return helpers for HLE functions */
static inline uint32_t iop_arg(R5900 *c, int n)
{
    return n < 4 ? c->r[4 + n][0] : iop_r32(c->r[29][0] + 16 + 4 * (uint32_t)(n - 4));
}
static inline void iop_ret(R5900 *c, int32_t v)
{
    c->r[2][0] = (uint32_t)v;
    c->r[2][1] = (uint32_t)(v >> 31);
}

/* ---- kernel ------------------------------------------------------------------------------ */
void iop_kernel_init(void);
uint32_t iop_heap_alloc(uint32_t size);
uint32_t iop_heap_alloc_mode(int mode, uint32_t size, uint32_t want);   /* 0 low, 1 high, 2 at want */
void iop_heap_free(uint32_t addr);
int iop_create_thread(uint32_t entry, uint32_t prio, uint32_t stack_size, uint32_t gp, uint32_t attr);
int iop_start_thread(int id, uint32_t arg, void (*on_exit)(void *t, uint32_t ret), void *user);
int iop_isignal_sema(int id);
int iop_iwakeup(int id);
int iop_iset_event(int id, uint32_t bits);
void iop_load_module(const IopModuleImage *m, const char *args, uint32_t args_len,
                     void (*done)(void *user, int32_t result), void *user);
int iop_vsformat(char *out, size_t size, uint32_t fmt, R5900 *c, int first_arg);
void iop_delay_until(uint64_t us);                       /* block the current IOP thread */
int iop_module_loaded(const IopModuleImage *m);

/* interrupts: devices raise them, handlers run at the next kernel tick */
void iop_raise_irq(int irq);
void iop_intr_tick(void);
int iop_intr_pending(void);

/* ---- SIF from the IOP side ---------------------------------------------------------------- */
void iop_sif_init(void);
int iop_sif_user_cmd(uint32_t cid, uint32_t pkt_addr);   /* EE -> IOP command to a module handler */
int iop_sif_guest_rpc(uint32_t sid, const uint32_t *req); /* RPC to a module server; 0 if none */
void sif_queue_packet(const uint32_t *words, int n, uint32_t data_src, uint32_t data_dst, uint32_t data_size);
void sif_rpc_end(const uint32_t *req, uint32_t cid, uint32_t server, uint32_t buff, uint32_t cbuff,
                 uint32_t data_src, uint32_t data_dst, uint32_t data_size);

/* ---- devices ------------------------------------------------------------------------------ */
void iop_cdvd_init(void);
void iop_sd_init(void);
void iop_sio2_init(void);
uint32_t iop_sio2_read(uint32_t p);
void iop_sio2_write(uint32_t p, uint32_t v);
void iop_sio2_dma(int ch, uint32_t addr, uint32_t block_words, uint32_t blocks);
void iop_sio2_flush(void);      /* write back a pending memory-card save and close it */
uint32_t iop_io_read(uint32_t a, int size);
void iop_io_write(uint32_t a, uint32_t v, int size);
float iop_io_readf(uint32_t a);
void iop_io_writef(uint32_t a, float v);

#endif
