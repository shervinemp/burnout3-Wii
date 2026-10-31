/* Guest memory: RAM allocation, layout-aware byte copies, runtime scratch. */
#include "rt.h"

#include <string.h>

#if !defined(PS2_RAM_FIXED_BASE)
uint8_t *ps2_ram;
#endif

#if defined(PS2_TRACK)
uint8_t g_wdirty[PS2_RAM_SIZE >> PS2_TRACK_SHIFT];     /* ps2_recomp.h: the pages the guest's stores touched */
uint8_t g_wtick = 1;
#endif

static uint32_t g_scratch_brk = RT_SCRATCH_BASE;

uint32_t rt_scratch_alloc(uint32_t size, uint32_t align)
{
    uint32_t a = (g_scratch_brk + align - 1) & ~(align - 1);
    if (a + size > RT_SCRATCH_BASE + RT_SCRATCH_SIZE)
        FATAL("runtime scratch exhausted");
    g_scratch_brk = a + size;
    return a;
}

void ps2_mem_write_bytes(uint32_t addr, const void *src, uint32_t n)
{
    const uint8_t *s = src;
#if PS2_HOST_BE
    while (n && (addr & 3)) {
        SB(addr, *s);
        addr++, s++, n--;
    }
    while (n >= 4) {
        SW(addr, (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24);
        addr += 4, s += 4, n -= 4;
    }
    while (n) {
        SB(addr, *s);
        addr++, s++, n--;
    }
#else
    while (n) {
        uint32_t off = addr & PS2_RAM_MASK;
        uint32_t chunk = PS2_RAM_SIZE - off < n ? PS2_RAM_SIZE - off : n;
        memcpy(ps2_ram + off, s, chunk);
        addr += chunk, s += chunk, n -= chunk;
    }
#endif
}

void ps2_mem_read_bytes(void *dst, uint32_t addr, uint32_t n)
{
    uint8_t *d = dst;
#if PS2_HOST_BE
    while (n && (addr & 3)) {
        *d++ = LB(addr);
        addr++, n--;
    }
    while (n >= 4) {
        uint32_t w = LW(addr);
        d[0] = (uint8_t)w;
        d[1] = (uint8_t)(w >> 8);
        d[2] = (uint8_t)(w >> 16);
        d[3] = (uint8_t)(w >> 24);
        addr += 4, d += 4, n -= 4;
    }
    while (n) {
        *d++ = LB(addr);
        addr++, n--;
    }
#else
    while (n) {
        uint32_t off = addr & PS2_RAM_MASK;
        uint32_t chunk = PS2_RAM_SIZE - off < n ? PS2_RAM_SIZE - off : n;
        memcpy(d, ps2_ram + off, chunk);
        addr += chunk, d += chunk, n -= chunk;
    }
#endif
}

void g_read_str(char *dst, uint32_t addr, uint32_t max)
{
    uint32_t k;
    for (k = 0; k + 1 < max; k++) {
        char ch = (char)LB(addr + k);
        dst[k] = ch;
        if (!ch)
            return;
    }
    dst[k] = 0;
}

void g_write_str(uint32_t addr, const char *s)
{
    do {
        SB(addr++, (uint8_t)*s);
    } while (*s++);
}

/* ---- ELF ------------------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* Segments are read straight into guest memory in chunks: on the Wii a
 * whole-file buffer would come from MEM2, which is never given back. */
uint32_t elf_load(const char *path)
{
    DiscFile *f = disc_open(path);
    uint8_t hdr[52], ph[32], *tmp;
    uint32_t entry, phoff, phnum, k;
    if (!f)
        FATAL("cannot open %s", path);
    if (disc_read(f, hdr, sizeof hdr, 0) != (int64_t)sizeof hdr || memcmp(hdr, "\x7f" "ELF", 4))
        FATAL("%s is not an ELF", path);
    entry = le32(hdr + 24);
    phoff = le32(hdr + 28);
    phnum = le16(hdr + 44);
    tmp = malloc(64 * 1024);
    for (k = 0; k < phnum; k++) {
        uint32_t type, off, vaddr, filesz, memsz, done, n;
        if (disc_read(f, ph, sizeof ph, phoff + 32 * k) != (int64_t)sizeof ph)
            FATAL("short read on %s", path);
        type = le32(ph), off = le32(ph + 4), vaddr = le32(ph + 8);
        filesz = le32(ph + 16), memsz = le32(ph + 20);
        if (type != 1 || !memsz)
            continue;
        for (done = 0; done < filesz; done += n) {
            n = filesz - done < 64 * 1024 ? filesz - done : 64 * 1024;
            if (disc_read(f, tmp, n, (int64_t)off + done) != (int64_t)n)
                FATAL("short read on %s", path);
            ps2_mem_write_bytes(vaddr + done, tmp, n);
        }
        LOG("ELF segment %08X: %X bytes (%X in memory)", vaddr, filesz, memsz);
    }
    free(tmp);
    disc_close(f);
    return entry;
}
