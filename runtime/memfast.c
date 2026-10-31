/* memcpy, memcmp and memset for Broadway. The ones in newlib copy 16 bytes a turn (four loads, four stores, an
 * update and a branch), compare a word a turn, and take unaligned data a byte at a time; in a race frame they were
 * 5% of all time (the game's scratchpad DMA, the VIF's UNPACKs, the parse cache's blocks, the micro memory's
 * notes, the CLUT cache). Here: 32 bytes a turn through the floating point registers (a load or store of eight
 * bytes is one instruction on the 750 and Dolphin's JIT alike), words compared eight at a time, and a store of
 * eight bytes for memset. Nothing but speed changes: what they return and write is what newlib's do (memcmp
 * returns the sign only, as the standard asks). Linked into the program ahead of the libraries, so the library's
 * own are left out. Built with -fno-builtin and -fno-tree-loop-distribute-patterns (Makefile.wii): the compiler
 * must not turn these loops into calls of themselves. */
#ifdef GEKKO
#include <stddef.h>
#include <stdint.h>

/* dcbz is only for memory the data cache holds (the BATs of libogc: 0x80000000-0x9FFFFFFF cached, 0xC0000000 and up
 * not: dcbz on those is an alignment exception), and only where the cache costs something: under Dolphin's JIT, which has
 * no cache model, each dcbz flushes its register cache, so plat_init turns it off there (g_dcbz_on). A console leaves it
 * on; the cache model of work/cachesim is a Dolphin, so its runs say "0 dcbz 1" in their autopilot file. */
int g_dcbz_on = 1;
#define DCBZ_OK(p) (g_dcbz_on && ((uintptr_t)(p) >> 28) - 8u < 2u)

void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (n >= 8 && !(((uintptr_t)dp ^ (uintptr_t)sp) & 3)) {
        /* the same place in a word: the head to a word, then a word more if that leaves it half a doubleword out */
        while ((uintptr_t)dp & 3) {
            *dp++ = *sp++;
            n--;
        }
        if (((uintptr_t)dp & 4) && n >= 4) {
            *(uint32_t *)dp = *(const uint32_t *)sp;
            dp += 4;
            sp += 4;
            n -= 4;
        }
        if (!((uintptr_t)dp & 7)) {
            double *dd = (double *)dp;
            const double *sd = (const double *)sp;
            if (n >= 96 && DCBZ_OK(dp) && (dp + n <= sp || sp + n <= dp)) {
                /* a whole line is written, so the cache need not fetch it first (dcbz): the stores of a copy to
                 * memory that is not in the cache are the data cache's misses and the memory's reads */
                while ((uintptr_t)dd & 31) {
                    *dd++ = *sd++;
                    n -= 8;
                }
                while (n >= 32) {
                    const double a = sd[0], b = sd[1], c = sd[2], e = sd[3];
                    if (n >= 96 && DCBZ_OK(sd))
                        __builtin_prefetch(sd + 8, 0, 0);  /* two lines on: the source's misses overlap */
                    __asm__ volatile("dcbz 0,%0" : : "r"(dd) : "memory");
                    dd[0] = a;
                    dd[1] = b;
                    dd[2] = c;
                    dd[3] = e;
                    dd += 4;
                    sd += 4;
                    n -= 32;
                }
            }
            while (n >= 32) {
                const double a = sd[0], b = sd[1], c = sd[2], e = sd[3];
                dd[0] = a;
                dd[1] = b;
                dd[2] = c;
                dd[3] = e;
                dd += 4;
                sd += 4;
                n -= 32;
            }
            while (n >= 8) {
                *dd++ = *sd++;
                n -= 8;
            }
            dp = (uint8_t *)dd;
            sp = (const uint8_t *)sd;
        } else {
            while (n >= 4) {
                *(uint32_t *)dp = *(const uint32_t *)sp;
                dp += 4;
                sp += 4;
                n -= 4;
            }
        }
        if (n >= 4) {
            *(uint32_t *)dp = *(const uint32_t *)sp;
            dp += 4;
            sp += 4;
            n -= 4;
        }
    }
    while (n >= 4 && !(((uintptr_t)dp | (uintptr_t)sp) & 3)) {
        *(uint32_t *)dp = *(const uint32_t *)sp;
        dp += 4;
        sp += 4;
        n -= 4;
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a, *pb = b;
    if (n >= 4 && !(((uintptr_t)pa | (uintptr_t)pb) & 3)) {
        const uint32_t *wa = (const uint32_t *)pa, *wb = (const uint32_t *)pb;
        while (n >= 32) {
            if ((wa[0] ^ wb[0]) | (wa[1] ^ wb[1]) | (wa[2] ^ wb[2]) | (wa[3] ^ wb[3]) | (wa[4] ^ wb[4]) |
                (wa[5] ^ wb[5]) | (wa[6] ^ wb[6]) | (wa[7] ^ wb[7]))
                break;
            wa += 8;
            wb += 8;
            n -= 32;
        }
        while (n >= 4) {
            if (*wa != *wb)
                return *wa > *wb ? 1 : -1;     /* big endian: the words order as the bytes do */
            wa++;
            wb++;
            n -= 4;
        }
        pa = (const uint8_t *)wa;
        pb = (const uint8_t *)wb;
    }
    while (n--) {
        if (*pa != *pb)
            return *pa > *pb ? 1 : -1;
        pa++;
        pb++;
    }
    return 0;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    const uint32_t w = (uint32_t)(uint8_t)c * 0x01010101u;
    if (n >= 8) {
        while ((uintptr_t)dp & 3) {
            *dp++ = (uint8_t)c;
            n--;
        }
        if (((uintptr_t)dp & 4) && n >= 4) {
            *(uint32_t *)dp = w;
            dp += 4;
            n -= 4;
        }
        if (n >= 32 && !((uintptr_t)dp & 7)) {
            union { uint32_t u[2]; double f; } v;
            double *dd = (double *)dp;
            v.u[0] = v.u[1] = w;
            if (n >= 96 && DCBZ_OK(dp)) {
                while ((uintptr_t)dd & 31) {
                    *dd++ = v.f;
                    n -= 8;
                }
                while (n >= 32) {
                    __asm__ volatile("dcbz 0,%0" : : "r"(dd) : "memory");
                    dd[0] = v.f;
                    dd[1] = v.f;
                    dd[2] = v.f;
                    dd[3] = v.f;
                    dd += 4;
                    n -= 32;
                }
            }
            while (n >= 32) {
                dd[0] = v.f;
                dd[1] = v.f;
                dd[2] = v.f;
                dd[3] = v.f;
                dd += 4;
                n -= 32;
            }
            dp = (uint8_t *)dd;
        }
    }
    while (n >= 4 && !((uintptr_t)dp & 3)) {
        *(uint32_t *)dp = w;
        dp += 4;
        n -= 4;
    }
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
}
#endif
