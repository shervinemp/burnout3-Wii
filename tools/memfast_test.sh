#!/bin/sh
# memfast_test.sh: runtime/memfast.c (the Wii's memcpy, memset, memcmp) on the host, with dcbz emulated by zeroing the
# 32-byte line it names. For random sizes and alignments the results must be libc's, and nothing outside the destination
# may change: a dcbz of a line that is not wholly inside the copy would show as a guard byte gone.
#   sh tools/memfast_test.sh        (needs a host gcc; prints "memfast_test: ok" or the first difference)
set -e
cd "$(dirname "$0")/.."
T=${TMPDIR:-/tmp}/memfast_test_$$
mkdir -p "$T"
# the source without its GEKKO guard, dcbz as a zeroed line, every address eligible, names of its own
sed -e '/^#ifdef GEKKO/d' -e '/^#endif/d' \
    -e 's|__asm__ volatile("dcbz 0,%0" : : "r"(\([a-z]*\)) : "memory");|b3_dcbz((void *)\1);|' \
    -e 's|^#define DCBZ_OK(p).*|#define DCBZ_OK(p) (g_dcbz_on)|' runtime/memfast.c > "$T/memfast_h.c"
cat > "$T/main.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define memcpy b3_memcpy
#define memset b3_memset
#define memcmp b3_memcmp
static void b3_dcbz(void *p)
{
    unsigned char *q = (unsigned char *)((uintptr_t)p & ~(uintptr_t)31);
    size_t k;
    for (k = 0; k < 32; k++)
        q[k] = 0;
}
#include "memfast_h.c"
#undef memcpy
#undef memset
#undef memcmp

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main(void)
{
    enum { PAD = 96, MAX = 700 };
    static unsigned char a[PAD + MAX + PAD + 64] __attribute__((aligned(64))), b[PAD + MAX + PAD + 64] __attribute__((aligned(64)));
    static unsigned char ref[PAD + MAX + PAD + 64], src[PAD + MAX + PAD + 64];
    int it, k;
    for (it = 0; it < 200000; it++) {
        size_t n = rnd() % (MAX + 1), so = rnd() % 64, dof = rnd() % 64;
        int c = (int)(rnd() & 255);
        for (k = 0; k < (int)sizeof a; k++) {
            src[k] = (unsigned char)rnd();
            a[k] = ref[k] = (unsigned char)(rnd() | 1);        /* guards and old contents: never 0 */
        }
        /* memcpy: source in its own array, so the regions never overlap */
        b3_memcpy(a + PAD + dof, src + so, n);
        memcpy(ref + PAD + dof, src + so, n);
        if (memcmp(a, ref, sizeof a)) {
            printf("memfast_test: memcpy n %zu dst+%zu src+%zu differs\n", n, dof, so);
            for (k = 0; k < (int)sizeof a; k++)
                if (a[k] != ref[k]) { printf("  first at %d (dst offset %d): %02x want %02x\n", k, k - PAD - (int)dof, a[k], ref[k]); break; }
            return 1;
        }
        /* memset */
        for (k = 0; k < (int)sizeof b; k++)
            b[k] = ref[k] = (unsigned char)(rnd() | 1);
        b3_memset(b + PAD + dof, c, n);
        memset(ref + PAD + dof, c, n);
        if (memcmp(b, ref, sizeof b)) {
            printf("memfast_test: memset n %zu dst+%zu c %d differs\n", n, dof, c);
            return 1;
        }
        /* memcmp */
        {
            size_t at = n ? rnd() % n : 0;
            memcpy(b, src, MAX);
            memcpy(a, src, MAX);
            if (n && (rnd() & 1))
                a[at] ^= (unsigned char)(1 + rnd() % 255);
            {
                int x = b3_memcmp(a, b, n), y = memcmp(a, b, n);
                if ((x > 0) != (y > 0) || (x < 0) != (y < 0)) {
                    printf("memfast_test: memcmp n %zu differs (%d, %d)\n", n, x, y);
                    return 1;
                }
            }
        }
    }
    printf("memfast_test: ok (200000 random copies, fills and compares)\n");
    return 0;
}
EOF
gcc -O2 -fno-builtin -I"$T" -o "$T/t" "$T/main.c"
"$T/t"
rm -rf "$T"
