/* revtest.c: the reverb's chunk at a time (rev_run, runtime/iop_rev.h) against its sample at a time (rev_step), on the
 * host.
 *
 * Both start from the same state and the same work area, and run chunk after chunk of the same input; after each the
 * output, the state (every field) and the whole work area (with guard samples on either side) must be the same bit for
 * bit. The states are random beyond what the presets do: delay lines from 1 sample to 60000, the fourteen places
 * anywhere in them (equal ones, places next to the end, the base next to the end, so that a place or the base comes
 * round the end inside a chunk, often at once), volumes and inputs at the edges as well as between (so that the sums
 * stop at 16 bits, and the products wrap as Broadway's mullw does), the stores on and off, chunks of 0 to 256 samples
 * that start on either side.
 *
 * Build: gcc -O2 -fwrapv -I. -o revtest tools/iop/revtest.c && revtest [STATES [SEED]]
 * (-fwrapv: the products of the edge values overflow 32 bits; the port relies on them wrapping, as mullw does.) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int32_t sat16(int32_t v)     /* runtime/ps2_mmi.h */
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

#include "runtime/iop_rev.h"

#define GUARD 64
#define MAXLEN 60000
#define MAXN 256

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 7;
    g_seed ^= g_seed << 17;
    return (uint32_t)(g_seed >> 16);
}

static uint32_t below(uint32_t n)
{
    return n ? rnd() % n : 0;
}

/* a 16-bit value: an edge one a quarter of the time */
static int16_t value16(void)
{
    static const int16_t edge[8] = {32767, -32768, -32767, 0, 1, -1, 16384, -16384};
    return (rnd() & 3) == 0 ? edge[rnd() & 7] : (int16_t)rnd();
}

/* a place in a delay line of len: anywhere, next to its end, or one of the others */
static uint32_t place(uint32_t len, const uint32_t *others, int nothers)
{
    switch (rnd() & 3) {
    case 0:
        return len - 1 - below(len < 4 ? len : 4);
    case 1:
        if (nothers)
            return others[below((uint32_t)nothers)];
        /* fall through */
    default:
        return below(len);
    }
}

static void random_state(Reverb *r, uint32_t len)
{
    uint32_t *f[] = {
        &r->same_l_src, &r->same_r_src, &r->diff_l_src, &r->diff_r_src, &r->same_l_dst, &r->same_r_dst,
        &r->diff_l_dst, &r->diff_r_dst, &r->comb1_l_src, &r->comb1_r_src, &r->comb2_l_src, &r->comb2_r_src,
        &r->comb3_l_src, &r->comb3_r_src, &r->comb4_l_src, &r->comb4_r_src, &r->apf1_l_dst, &r->apf1_r_dst,
        &r->apf2_l_dst, &r->apf2_r_dst, &r->apf1_l_src, &r->apf1_r_src, &r->apf2_l_src, &r->apf2_r_src,
        &r->same_l_prv, &r->same_r_prv, &r->diff_l_prv, &r->diff_r_prv
    };
    uint32_t done[28];
    int k;
    memset(r, 0, sizeof *r);
    r->mode = 5;
    r->on = (rnd() & 7) != 0;
    r->iir_vol = value16();
    r->comb1_vol = value16();
    r->comb2_vol = value16();
    r->comb3_vol = value16();
    r->comb4_vol = value16();
    r->wall_vol = value16();
    r->apf1_vol = value16();
    r->apf2_vol = value16();
    r->in_coef_l = value16();
    r->in_coef_r = value16();
    r->start = GUARD + below(32);
    r->len = len;
    for (k = 0; k < 28; k++) {
        if ((rnd() & 15) == 0 && k >= 24) {
            /* a "prv" place one before its "dst", as rev_set makes it */
            *f[k] = (*f[k - 20] + len - 1) % len;
        } else {
            *f[k] = place(len, done, k);
        }
        done[k] = *f[k];
    }
    r->base = (rnd() & 3) == 0 ? len - 1 - below(len < 3 ? len : 3) : below(len);
    for (k = 0; k < 2; k++) {
        r->in_prev[k] = value16();
        r->in_cur[k] = value16();
        r->out_prev[k] = value16();
        r->out_cur[k] = value16();
    }
}

static uint32_t random_len(void)
{
    static const uint32_t small[8] = {1, 2, 3, 4, 5, 7, 8, 13};
    switch (rnd() & 3) {
    case 0:
        return small[rnd() & 7];
    case 1:
        return 1 + below(300);
    default:
        return 1 + below(MAXLEN);
    }
}

int main(int argc, char **argv)
{
    static int16_t mem_a[GUARD + 32 + MAXLEN + GUARD], mem_b[GUARD + 32 + MAXLEN + GUARD];
    static int16_t in[2 * MAXN], out_a[2 * MAXN], out_b[2 * MAXN];
    const int states = argc > 1 ? atoi(argv[1]) : 20000;
    uint64_t samples = 0, chunks = 0;
    int s, failed = 0;
    if (argc > 2)
        g_seed += strtoull(argv[2], NULL, 0) * 0x2545F4914F6CDD1Dull;
    for (s = 0; s < states && !failed; s++) {
        Reverb a, b;
        const uint32_t len = random_len();
        uint64_t first = rnd() & 1 ? rnd() : 0x1000000000ull + rnd();
        int c, nchunks = 1 + (int)below(6);
        uint32_t k;
        random_state(&a, len);
        for (k = 0; k < sizeof mem_a / sizeof mem_a[0]; k++)
            mem_a[k] = value16();
        memcpy(mem_b, mem_a, sizeof mem_a);
        b = a;
        for (c = 0; c < nchunks && !failed; c++) {
            uint32_t n, i;
            switch (rnd() & 3) {
            case 0:
                n = below(4);
                break;
            case 1:
                n = 256;
                break;
            default:
                n = below(MAXN + 1);
                break;
            }
            for (i = 0; i < 2 * n; i++)
                in[i] = value16();
            memset(out_a, 0x55, sizeof out_a);
            memset(out_b, 0x55, sizeof out_b);
            for (i = 0; i < n; i++)
                rev_step(&a, mem_a, first + i, in[2 * i], in[2 * i + 1], &out_a[2 * i], &out_a[2 * i + 1]);
            rev_run(&b, mem_b, first, n, in, out_b);
            if (memcmp(out_a, out_b, sizeof out_a) || memcmp(&a, &b, sizeof a) || memcmp(mem_a, mem_b, sizeof mem_a)) {
                uint32_t m;
                printf("FAIL state %d chunk %d: len %u base %u start %u, first %llu n %u, on %d:", s, c, (unsigned)len,
                       (unsigned)a.base, (unsigned)a.start, (unsigned long long)first, (unsigned)n, a.on);
                for (m = 0; m < 2 * MAXN; m++)
                    if (out_a[m] != out_b[m]) {
                        printf(" out[%u] %d/%d", (unsigned)m, out_a[m], out_b[m]);
                        break;
                    }
                if (memcmp(&a, &b, sizeof a))
                    printf(" state differs (base %u/%u, in_cur %d,%d/%d,%d, out_cur %d,%d/%d,%d)", (unsigned)a.base,
                           (unsigned)b.base, a.in_cur[0], a.in_cur[1], b.in_cur[0], b.in_cur[1], a.out_cur[0],
                           a.out_cur[1], b.out_cur[0], b.out_cur[1]);
                for (m = 0; m < sizeof mem_a / sizeof mem_a[0]; m++)
                    if (mem_a[m] != mem_b[m]) {
                        printf(" mem[%u] %d/%d", (unsigned)m, mem_a[m], mem_b[m]);
                        break;
                    }
                printf("\n");
                failed++;
            }
            first += n;
            samples += n;
            chunks++;
        }
    }
    printf("%s: %d states, %llu chunks, %llu samples, %d failed\n", failed ? "FAIL" : "PASS", s,
           (unsigned long long)chunks, (unsigned long long)samples, failed);
    return failed != 0;
}
