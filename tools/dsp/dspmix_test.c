/*
 * dspmix_test.c: the DSP mixer's microcode alone (stage 2: ADPCM decoded on the DSP). A standalone dol: random voices -
 * their records (the block they are in, their decoder), the runs of ADPCM blocks their places enter (random blocks in
 * a MEM2 area, as sound RAM is, at any 16-byte place, some long enough for several pieces), places, pitches, gains and
 * gain steps, both cores and sides - go to the DSP through platform/wii/dsp_mix.c; what comes back is compared with
 *   1. a model of the microcode, step for step: runtime/iop_sd.c's decode_at for the blocks, the microcode's integer
 *      resampling and sums (the sums and the records written back must be exactly equal), and
 *   2. the CPU mixer's own arithmetic (voice_stretch: floats, the place's fraction to 8 bits, gains moving in a
 *      straight line): a few LSB apart is what is wanted.
 * Records carry on from run to run, also with two runs out at once on the same voices.
 *
 *   sh tools/dsp/build.sh dspmix dspmix_test       -> build-dsptest/dspmix_test.dol
 *   run it with Dolphin's DSP in LLE; the log lines are "dspmixtest: ..." in the OSReport log
 */
#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dsp_mix.h"

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SYS_Report("dspmixtest: %s\n", line);
}

void plat_log(const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SYS_Report("dspmixtest: [log] %s\n", line);
}

#define NJOB DSPMIX_MAX_JOBS
#define NVOICE 48
#define BLOCK_END (28u << 12)
#define SPU_BYTES (64 * 1024)
#define MAX_RUNS 8

static uint8_t *g_spu;                  /* the "sound RAM": random ADPCM blocks in MEM2 */
static DspMixRecord g_rec[NVOICE] __attribute__((aligned(32)));        /* what the DSP reads and writes */
static DspMixRecord g_mrec[NVOICE];                                     /* the model's */
static DspMixRun g_runs[DSPMIX_SLOTS][NJOB][MAX_RUNS] __attribute__((aligned(32)));

static uint32_t g_seed = 12345;
static uint32_t rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

typedef struct Spec {
    int core, left, right, voice;
    uint32_t cnt, idx, frac, pint, pfrac;
    int gl, gr;                 /* start gains */
    int32_t dl, dr;             /* steps, 16 fraction bits */
    uint32_t nb, nruns;         /* blocks entered, runs */
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];     /* offsets in g_spu, blocks */
} Spec;

/* runtime/iop_sd.c's decode_at: d[0] is the sample before (left as it is), d[1..28] the block's */
static void model_decode(const uint8_t *b, int16_t *d, int32_t *pp1, int32_t *pp2)
{
    static const int8_t f0[8] = {0, 60, 115, 98, 122, 0, 0, 0}, f1[8] = {0, 0, -52, -55, -60, 0, 0, 0};
    const int32_t a = f0[(b[0] >> 4) & 7], c = f1[(b[0] >> 4) & 7];
    int shift = b[0] & 15, k;
    int32_t p1 = *pp1, p2 = *pp2;
    if (shift > 12)
        shift = 9;
    b += 2;
    for (k = 0; k < 14; k++) {
        const uint32_t n = b[k];
        int32_t s1 = ((int16_t)(n << 12) >> shift) + ((p1 * a + p2 * c + 32) >> 6), s2;
        if ((uint32_t)(s1 + 32768) > 65535u)
            s1 = s1 < 0 ? -32768 : 32767;
        s2 = ((int16_t)((n & 0xF0u) << 8) >> shift) + ((s1 * a + p1 * c + 32) >> 6);
        if ((uint32_t)(s2 + 32768) > 65535u)
            s2 = s2 < 0 ? -32768 : 32767;
        d[2 * k + 1] = (int16_t)s1;
        d[2 * k + 2] = (int16_t)s2;
        p2 = s1;
        p1 = s2;
    }
    *pp1 = p1;
    *pp2 = p2;
}

/* the model of the microcode: sums[slot][core][n][ch] as the 32-bit value (m << 16 | l); g_sat marks a sum that
 * stopped at twice full scale on the way (left out of the comparison with the CPU mixer's floats) */
static uint32_t g_model[DSPMIX_SLOTS][2][DSPMIX_CHUNK][2];
static uint8_t g_sat[DSPMIX_SLOTS][2][DSPMIX_CHUNK][2];
static float g_ref[DSPMIX_SLOTS][2][DSPMIX_CHUNK][2];

static uint32_t sat_add(uint32_t sum, int32_t prod, uint8_t *sat)
{
    const int64_t s = (int64_t)(int32_t)sum + prod;
    if (s == (int64_t)(int32_t)s)
        return (uint32_t)s;
    *sat = 1;
    return (s > 0 ? 0x7FFF0000u : 0x80000000u) | ((uint32_t)s & 0xFFFFu);
}

/* a job on the model: the stream from the record and the blocks, the record's new state, the sums */
static void model_job(int slot, const Spec *s)
{
    static int16_t st[29 + 28 * DSPMIX_BLOCKS + 4];
    DspMixRecord *r = &g_mrec[s->voice];
    uint32_t idx = s->idx, frac = s->frac, n, k, e = 0;
    uint32_t gl = ((uint32_t)(int32_t)s->gl << 16) + 0x8000u, gr = ((uint32_t)(int32_t)s->gr << 16) + 0x8000u;
    int32_t p1 = r->p1, p2 = r->p2;
    int16_t v[DSPMIX_CHUNK];
    memcpy(st, r->smp, sizeof r->smp);
    for (k = 0; k < s->nruns; k++) {
        uint32_t b;
        for (b = 0; b < s->run_n[k]; b++, e++)
            model_decode(g_spu + s->run_at[k] + 16 * b, st + 28 * (e + 1), &p1, &p2);
    }
    memcpy(r->smp, st + 28 * e, sizeof r->smp);
    r->p1 = (int16_t)p1;
    r->p2 = (int16_t)p2;
    for (n = 0; n < s->cnt; n++) {
        const int32_t s0 = st[idx], s1 = st[idx + 1];
        const int32_t f15 = (int32_t)((frac >> 1) & 0x7F80u);
        int64_t prod = 2ll * f15 * s1 - 2ll * f15 * s0;
        int64_t rr, acc;
        if (prod & 0x10000)
            rr = (prod + 0x8000) & ~0xFFFFll;
        else
            rr = (prod + 0x7FFF) & ~0xFFFFll;
        acc = rr + ((int64_t)s0 << 16);
        v[n] = (int16_t)((acc >> 16) & 0xFFFF);
        frac += s->pfrac;
        idx += s->pint + (frac >> 16);
        frac &= 0xFFFF;
    }
    if (s->left)
        for (n = 0; n < s->cnt; n++) {
            const int16_t g = (int16_t)(gl >> 16);
            g_model[slot][s->core][n][0] = sat_add(g_model[slot][s->core][n][0], (int32_t)g * (int32_t)v[n], &g_sat[slot][s->core][n][0]);
            gl += (uint32_t)s->dl;
        }
    if (s->right)
        for (n = 0; n < s->cnt; n++) {
            const int16_t g = (int16_t)(gr >> 16);
            g_model[slot][s->core][n][1] = sat_add(g_model[slot][s->core][n][1], (int32_t)g * (int32_t)v[n], &g_sat[slot][s->core][n][1]);
            gr += (uint32_t)s->dr;
        }
    /* the CPU mixer's arithmetic on the same stream */
    {
        float fl = (float)s->gl, fr = (float)s->gr;
        const float dl = (float)s->dl / 65536.0f, dr = (float)s->dr / 65536.0f;
        idx = s->idx;
        frac = s->frac;
        for (n = 0; n < s->cnt; n++) {
            const float s0 = (float)st[idx], s1 = (float)st[idx + 1];
            const float sv = s0 + (s1 - s0) * ((float)(frac >> 8) * (1.0f / 256.0f));
            if (s->left)
                g_ref[slot][s->core][n][0] += sv * fl * (1.0f / 32768.0f);
            if (s->right)
                g_ref[slot][s->core][n][1] += sv * fr * (1.0f / 32768.0f);
            fl += dl;
            fr += dr;
            frac += s->pfrac;
            idx += s->pint + (frac >> 16);
            frac &= 0xFFFF;
        }
    }
}

static void make_spec(Spec *s, uint32_t m, int voice)
{
    static const uint32_t pitches[] = {0x1000, 0x0800, 0x3FFF, 0x2345, 0x0001, 0x1800, 0x0000, 0x0FFF, 0x2000, 0x1555, 0x3000};
    const uint32_t pitch = pitches[rnd() % 11];
    uint32_t pos, left, k;
    s->voice = voice;
    s->core = (int)(rnd() & 1);
    s->left = rnd() % 6 != 0;
    s->right = rnd() % 6 != 0;
    if (!s->left && !s->right)
        s->left = 1;
    s->cnt = 1 + rnd() % m;
    s->pint = pitch >> 12;
    s->pfrac = (pitch & 0xFFF) << 4;
    pos = rnd() % BLOCK_END;
    s->idx = pos >> 12;
    s->frac = (pos & 0xFFF) << 4;
    s->gl = (int)(rnd() % 65535) - 32767;
    s->gr = (int)(rnd() % 65535) - 32767;
    if (rnd() % 3 == 0)
        s->gl = (int)(rnd() % 20000);
    if (rnd() % 3 == 0)
        s->gr = (int)(rnd() % 20000);
    {
        const int el = (int)(rnd() % 65535) - 32767, er = (int)(rnd() % 65535) - 32767;
        s->dl = rnd() % 4 == 0 || s->cnt < 2 ? 0 : (int32_t)(((int64_t)(el - s->gl) * 65536) / (int64_t)s->cnt);
        s->dr = rnd() % 4 == 0 || s->cnt < 2 ? 0 : (int32_t)(((int64_t)(er - s->gr) * 65536) / (int64_t)s->cnt);
    }
    /* the blocks the place enters, as the CPU's walk counts them: one each time it passes a block's end */
    s->nb = (pos + s->cnt * pitch) / BLOCK_END;
    /* in up to four runs at random places (a loop's jump is a new run) */
    s->nruns = 0;
    left = s->nb;
    while (left) {
        const uint32_t n = s->nruns == 3 ? left : 1 + rnd() % left;
        s->run_n[s->nruns] = n;
        s->run_at[s->nruns] = 16 * (rnd() % (SPU_BYTES / 16 - n));
        s->nruns++;
        left -= n;
    }
    for (k = s->nruns; k < MAX_RUNS; k++)
        s->run_n[k] = s->run_at[k] = 0;
}

static uint32_t g_m[DSPMIX_SLOTS];
static int g_njobs[DSPMIX_SLOTS];

/* a slot's jobs made, modelled and sent; voices from `first`, one job each */
static int prepare(int slot, uint32_t m, int njobs, int first)
{
    DspMixCmd *c = dspmix_cmd(slot);
    static Spec spec;
    int j;
    memset(g_model[slot], 0, sizeof g_model[slot]);
    memset(g_sat[slot], 0, sizeof g_sat[slot]);
    memset(g_ref[slot], 0, sizeof g_ref[slot]);
    g_m[slot] = m;
    g_njobs[slot] = njobs;
    for (j = 0; j < njobs; j++) {
        Spec *s = &spec;
        DspMixJob *jb = &c->job[j];
        DspMixRun *rl = g_runs[slot][j];
        uint32_t k;
        const uint32_t phys = (uint32_t)rl & 0x1FFFFFFFu;
        make_spec(s, m, (first + j) % NVOICE);
        model_job(slot, s);
        for (k = 0; k < MAX_RUNS; k++) {
            const uint32_t at = ((uint32_t)g_spu & 0x1FFFFFFFu) + s->run_at[k];
            rl[k].at_hi = (uint16_t)(at >> 16);
            rl[k].at_lo = (uint16_t)at;
            rl[k].blocks = (uint16_t)s->run_n[k];
            rl[k].spare = 0;
        }
        DCFlushRange(rl, sizeof g_runs[0][0]);
        jb->runs_hi = (uint16_t)(phys >> 16);
        jb->runs_lo = (uint16_t)phys;
        jb->nruns = (uint16_t)s->nruns;
        jb->coref = (uint16_t)((s->core & 1) | (s->left ? DSPMIX_LEFT : 0) | (s->right ? DSPMIX_RIGHT : 0));
        jb->cnt = (uint16_t)s->cnt;
        jb->idx = (uint16_t)s->idx;
        jb->frac = (uint16_t)s->frac;
        jb->pint = (uint16_t)s->pint;
        jb->pfrac = (uint16_t)s->pfrac;
        jb->gl = (int16_t)s->gl;
        jb->gr = (int16_t)s->gr;
        jb->dl_hi = (uint16_t)((uint32_t)s->dl >> 16);
        jb->dl_lo = (uint16_t)s->dl;
        jb->dr_hi = (uint16_t)((uint32_t)s->dr >> 16);
        jb->dr_lo = (uint16_t)s->dr;
        jb->voice = (uint16_t)s->voice;
    }
    if (!dspmix_run(slot, (uint32_t)njobs, m, g_rec)) {
        say("FAIL: the run was not sent");
        return 0;
    }
    return 1;
}

/* the oldest run's answer, which has to be `slot`'s, against its model (the records are compared after every run) */
static int check(int slot, int records)
{
    const uint32_t m = g_m[slot];
    const int njobs = g_njobs[slot];
    int core, got_slot, v;
    uint32_t n, bad = 0, cmpn = 0, words = 0, nsat = 0, rbad = 0;
    double sum2 = 0.0, maxd = 0.0;
    got_slot = dspmix_wait(500000);
    if (got_slot != slot) {
        say("FAIL: answer for slot %d, wanted %d", got_slot, slot);
        return 0;
    }
    for (core = 0; core < 2; core++) {
        const DspMixSum *sm = dspmix_sums(slot, core);
        for (n = 0; n < m; n++) {
            int ch;
            for (ch = 0; ch < 2; ch++) {
                const uint32_t got = sm ? ((uint32_t)(uint16_t)sm[n].m[ch] << 16) | sm[n].l[ch] : 0;
                const uint32_t want = g_model[slot][core][n][ch];
                const double d = (double)(int32_t)got / 32768.0 - (double)g_ref[slot][core][n][ch];
                if (got != want) {
                    if (bad < 4)
                        say("  core %d sample %u ch %d: DSP %08X model %08X", core, (unsigned)n, ch, (unsigned)got, (unsigned)want);
                    bad++;
                }
                words++;
                if (g_sat[slot][core][n][ch]) {
                    nsat++;
                    continue;
                }
                if (fabs(d) > maxd)
                    maxd = fabs(d);
                sum2 += d * d;
                cmpn++;
            }
        }
    }
    DCInvalidateRange(g_rec, sizeof g_rec);
    for (v = 0; v < NVOICE && records; v++)
        if (memcmp(&g_rec[v], &g_mrec[v], 62)) {
            if (rbad < 3) {
                int k;
                for (k = 0; k < 31 && ((int16_t *)&g_rec[v])[k] == ((int16_t *)&g_mrec[v])[k]; k++)
                    ;
                say("  record %d differs from word %d: DSP %04X model %04X", v, k, (unsigned)(uint16_t)((int16_t *)&g_rec[v])[k],
                    (unsigned)(uint16_t)((int16_t *)&g_mrec[v])[k]);
            }
            rbad++;
        }
    say("%s slot %d m=%u jobs=%d: %u of %u words differ from the model (%u stopped at twice full scale), %u records differ; "
        "against the CPU mixer's arithmetic: max %.2f, rms %.3f LSB",
        bad || rbad ? "FAIL" : "PASS", slot, (unsigned)m, njobs, (unsigned)bad, (unsigned)words, (unsigned)nsat, (unsigned)rbad, maxd,
        cmpn ? sqrt(sum2 / (double)cmpn) : 0.0);
    if (rbad)       /* go on from what the DSP has, so one difference is not counted again */
        memcpy(g_mrec, g_rec, sizeof g_mrec);
    return bad == 0 && rbad == 0;
}

static int run_once(uint32_t m, int njobs, int first)
{
    return prepare(0, m, njobs, first) && check(0, 1);
}

/* The DSP's stream read back: one job at pitch 1.0 from index 0, left gain 0x4000 alone, so each sum is 0x4000 times
 * a sample of the stream; nb blocks entered in nruns runs from block `at0` on (or spread when nruns > 1). */
static int probe(int voice, uint32_t nb, uint32_t nruns, uint32_t at0)
{
    DspMixCmd *c = dspmix_cmd(0);
    DspMixJob *jb = &c->job[0];
    DspMixRun *rl = g_runs[0][0];
    static Spec s;
    static int16_t want[29 + 28 * DSPMIX_BLOCKS];
    const DspMixRecord before = g_mrec[voice];
    uint32_t k, n, bad = 0, left = nb;
    const uint32_t phys = (uint32_t)rl & 0x1FFFFFFFu;
    memset(&s, 0, sizeof s);
    s.voice = voice;
    s.left = 1;
    s.cnt = 28 + 28 * nb > 256 ? 256 : 28 + 28 * nb;
    s.pint = 1;
    s.gl = 0x4000;
    s.nb = nb;
    for (k = 0; left; k++) {
        const uint32_t n2 = k + 1 == nruns ? left : (left + 1) / 2;
        s.run_n[k] = n2;
        s.run_at[k] = 16 * (at0 + 37 * k);
        left -= n2;
    }
    s.nruns = k;
    memset(g_model[0], 0, sizeof g_model[0]);
    memset(g_sat[0], 0, sizeof g_sat[0]);
    memset(g_ref[0], 0, sizeof g_ref[0]);
    /* the model's stream, for the print */
    {
        int32_t p1 = before.p1, p2 = before.p2;
        uint32_t e = 0;
        memcpy(want, before.smp, sizeof before.smp);
        for (k = 0; k < s.nruns; k++)
            for (n = 0; n < s.run_n[k]; n++, e++)
                model_decode(g_spu + s.run_at[k] + 16 * n, want + 28 * (e + 1), &p1, &p2);
    }
    model_job(0, &s);
    for (k = 0; k < MAX_RUNS; k++) {
        const uint32_t at = ((uint32_t)g_spu & 0x1FFFFFFFu) + s.run_at[k];
        rl[k].at_hi = (uint16_t)(at >> 16);
        rl[k].at_lo = (uint16_t)at;
        rl[k].blocks = (uint16_t)s.run_n[k];
        rl[k].spare = 0;
    }
    DCFlushRange(rl, sizeof g_runs[0][0]);
    memset(jb, 0, sizeof *jb);
    jb->runs_hi = (uint16_t)(phys >> 16);
    jb->runs_lo = (uint16_t)phys;
    jb->nruns = (uint16_t)s.nruns;
    jb->coref = DSPMIX_LEFT;
    jb->cnt = (uint16_t)s.cnt;
    jb->pint = 1;
    jb->gl = 0x4000;
    jb->voice = (uint16_t)voice;
    g_m[0] = s.cnt;
    g_njobs[0] = 1;
    if (!dspmix_run(0, 1, s.cnt, g_rec) || dspmix_wait(500000) != 0) {
        say("FAIL: probe run");
        return 0;
    }
    {
        const DspMixSum *sm = dspmix_sums(0, 0);
        for (n = 0; n < s.cnt; n++) {
            const int32_t got = (int32_t)((uint32_t)(uint16_t)sm[n].m[0] << 16 | sm[n].l[0]) / 0x4000;
            if (got != want[n] && bad++ < 6)
                say("  probe nb=%u runs=%u: stream[%u] DSP %d model %d", (unsigned)nb, (unsigned)s.nruns, (unsigned)n, (int)got, (int)want[n]);
        }
    }
    DCInvalidateRange(g_rec, sizeof g_rec);
    say("%s probe voice %d nb=%u runs=%u at %u: %u of %u stream samples differ; record %s (DSP %04X %04X .. p1 %d p2 %d, model %04X %04X .. p1 %d p2 %d)",
        bad ? "FAIL" : "PASS", voice, (unsigned)nb, (unsigned)s.nruns, (unsigned)at0, (unsigned)bad, (unsigned)s.cnt,
        memcmp(&g_rec[voice], &g_mrec[voice], 62) ? "DIFFERS" : "same", (unsigned)(uint16_t)g_rec[voice].smp[0],
        (unsigned)(uint16_t)g_rec[voice].smp[1], g_rec[voice].p1, g_rec[voice].p2, (unsigned)(uint16_t)g_mrec[voice].smp[0],
        (unsigned)(uint16_t)g_mrec[voice].smp[1], g_mrec[voice].p1, g_mrec[voice].p2);
    memcpy(g_mrec, g_rec, sizeof g_mrec);
    return bad == 0;
}

/* two runs out at once, the second on the same voices as the first: it has to go on from the first's records */
static int run_two(uint32_t m0, int n0, uint32_t m1, int n1, int first)
{
    int ok = prepare(0, m0, n0, first) && prepare(1, m1, n1, first);
    if (!ok)
        return 0;
    ok &= check(0, 0);          /* (the second run may be writing the same records now) */
    ok &= check(1, 1);
    return ok;
}

int main(void)
{
    int i, ok = 1;
    uint8_t *lo;
    AUDIO_Init(NULL);
    AUDIO_StopDMA();
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    /* the sound RAM stand-in in MEM2: random blocks, every eighth with a big shift and filter 4 (clamps) */
    lo = (uint8_t *)(((uintptr_t)SYS_GetArena2Lo() + 31) & ~(uintptr_t)31);
    SYS_SetArena2Lo(lo + SPU_BYTES);
    g_spu = lo;
    for (i = 0; i < SPU_BYTES; i++)
        g_spu[i] = (uint8_t)rnd();
    for (i = 0; i < SPU_BYTES / 16; i += 8)
        g_spu[16 * i] = (uint8_t)(0x40 | (rnd() % 3));
    DCFlushRange(g_spu, SPU_BYTES);
    say("sound RAM stand-in at %08X (physical %08X)", (unsigned)(uintptr_t)g_spu, (unsigned)((uintptr_t)g_spu & 0x1FFFFFFFu));
    /* the records: random decoders */
    for (i = 0; i < NVOICE; i++) {
        int k;
        for (k = 0; k < 29; k++)
            g_rec[i].smp[k] = (int16_t)rnd();
        g_rec[i].p1 = (int16_t)rnd();
        g_rec[i].p2 = (int16_t)rnd();
        g_rec[i].spare = 0;
    }
    memcpy(g_mrec, g_rec, sizeof g_rec);
    DCFlushRange(g_rec, sizeof g_rec);
    if (!dspmix_start()) {
        say("FAIL: the microcode did not start");
        goto stop;
    }
    say("the microcode is up");
    ok &= probe(5, 0, 0, 0);
    ok &= probe(6, 1, 1, 2);
    ok &= probe(7, 1, 1, 3);
    ok &= probe(8, 3, 1, 10);
    ok &= probe(9, 8, 1, 20);
    ok &= probe(10, 8, 1, 21);
    ok &= probe(11, 8, 2, 40);
    ok &= run_once(8, 1, 0);
    ok &= run_once(256, 1, 1);
    ok &= run_once(256, 4, 2);
    ok &= run_once(256, 12, 6);
    ok &= run_once(256, 20, 18);
    ok &= run_once(7, 5, 40);
    ok &= run_once(1, 3, 45);
    for (i = 0; i < 40; i++)
        ok &= run_once((uint32_t)(1 + rnd() % 256), 1 + (int)(rnd() % NJOB), (int)(rnd() % NVOICE));
    say("four runs out at once, on the same voices (two of them wait for the mail box)");
    for (i = 0; i < 6; i++) {
        const int n = 1 + (int)(rnd() % NJOB), first = (int)(rnd() % NVOICE);
        int k;
        for (k = 0; k < DSPMIX_SLOTS; k++)
            ok &= prepare(k, (uint32_t)(1 + rnd() % 256), n, first);
        if (dspmix_out() != DSPMIX_SLOTS) {
            say("FAIL: %d runs out, wanted %d", dspmix_out(), DSPMIX_SLOTS);
            ok = 0;
        }
        for (k = 0; k < DSPMIX_SLOTS; k++)
            ok &= check(k, k == DSPMIX_SLOTS - 1);
    }
    say("two runs out at once, on the same voices");
    ok &= run_two(256, 12, 100, 12, 3);
    ok &= run_two(256, 20, 256, 20, 10);
    for (i = 0; i < 10; i++) {
        const int n = 1 + (int)(rnd() % NJOB);
        ok &= run_two((uint32_t)(1 + rnd() % 256), n, (uint32_t)(1 + rnd() % 256), n, (int)(rnd() % NVOICE));
    }
    {
        char line[240];
        dspmix_report(line, sizeof line);
        say("%s", line);
    }
    say("%s", ok ? "ALL PASS" : "SOME FAILED");
stop:
    say("DONE");
    for (;;)
        VIDEO_WaitVSync();
    return 0;
}
