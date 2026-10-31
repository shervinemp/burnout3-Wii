/*
 * libsd exports over an SPU2 model.
 *
 * The libsd API and SPU2 behaviour follow the ps2sdk and PCSX2 references
 * listed in CREDITS.md; no ps2sdk source is included.
 *
 * Sound drivers stream by watching the hardware: voice play addresses (NAX),
 * end flags (ENDX), envelope levels (ENVX) and the IRQ address. So voices
 * advance in real time through their ADPCM blocks, honouring the loop flags
 * in each block header.
 *
 * The sound itself (spu_mix): every tick the 48 kHz samples that the time
 * since the last one makes are mixed, a voice's blocks decoded as it enters
 * them, its samples taken at its pitch (between two neighbours, in a straight
 * line: the chip's own four-point filter is not modelled), times its envelope
 * and its two volumes, summed per core into a dry and a wet sum by the mix
 * and the effect switches; core 0's output goes into core 1's sum by that
 * core's input volume, and core 1's sum times its volume is the output. Not
 * modelled: reverb (a wet sum goes nowhere but its capture ring), noise,
 * pitch modulation.
 *
 * The rings at the bottom of sound RAM (512 samples each, one a 48 kHz tick,
 * all at the same place): the chip writes each core's dry and wet sums into
 * four of them, and plays what is in the two "sound data input" rings of a
 * core as PCM, times BVOL. The game's engine sound goes that way: its voices
 * are in core 0's effect switches alone, the driver reads core 0's wet rings
 * (sceSdVoiceTrans), the EE filters what it read, and the driver writes the
 * result into core 1's input rings. A ring is written here once the driver
 * has read from it.
 *
 * The game's driver (RenderWare Audio) keeps some 34 voices playing all the
 * time, most of them at volume 0, and brings one in or out with a volume
 * slide. So: a voice nobody hears only has its place moved on (its blocks are
 * not decoded; when it is heard again the block before its place primes the
 * decoder, and what is left of the error is gone in a few milliseconds of a
 * slide's start); envelope and slides are worked out a tick at a time, by
 * their steps, and the gain goes in a straight line across the tick.
 *
 * The platform plays what it is given (plat_audio_write) and says how much
 * it still holds (plat_audio_level): the tick makes a little more or less
 * than the clock asks for to keep that near AUDIO_AHEAD, so the chip's time
 * follows the output's clock and a stall is made up for.
 *
 * Addresses are byte addresses into the 2 MB of sound RAM. Parameter entries
 * follow libsd: core in bit 0, voice in bits 1-5, register in bits 8-15.
 */
#include "iop.h"

#include <string.h>

#define SPU_RAM_SIZE 0x200000u
#define VOICES 24

enum {
    VP_VOLL = 0x00, VP_VOLR = 0x01, VP_PITCH = 0x02, VP_ADSR1 = 0x03, VP_ADSR2 = 0x04, VP_ENVX = 0x05,
    VP_VOLXL = 0x06, VP_VOLXR = 0x07,
    P_MMIX = 0x08, P_MVOLL = 0x09, P_MVOLR = 0x0A, P_EVOLL = 0x0B, P_EVOLR = 0x0C, P_AVOLL = 0x0D, P_AVOLR = 0x0E,
    P_BVOLL = 0x0F, P_BVOLR = 0x10,
    S_PMON = 0x13, S_NON = 0x14, S_KON = 0x15, S_KOFF = 0x16, S_ENDX = 0x17,
    S_VMIXL = 0x18, S_VMIXEL = 0x19, S_VMIXR = 0x1A, S_VMIXER = 0x1B,
    A_ESA = 0x1C, A_EEA = 0x1D, A_TSA = 0x1E, A_IRQA = 0x1F,
    VA_SSA = 0x20, VA_LSAX = 0x21, VA_NAX = 0x22
};
#define CORE_IRQ_ENABLE (2 << 1)

enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct Voice {
    uint16_t param[8];
    uint32_t ssa, lsax, nax;
    int playing;             /* a block is being played: blk holds it */
    int released;
    uint32_t pos;            /* place in the block: the sample (0..27) << 12, and the fraction */
    int32_t p1, p2;          /* the decoder's last two samples */
    int32_t env;             /* the envelope, 0..0x7FFF */
    int32_t env_wait;        /* samples it stays there (0: a step is due) */
    int32_t vol[2];          /* the volumes now, of 0x8000 (a slide moves them) */
    int32_t vol_wait[2];
    uint8_t phase;
    uint8_t cold;            /* its block is not decoded: 1 it went unheard, 2 it has just been keyed on */
    uint16_t pitch_seen;     /* the pitch at the last tick (the status line's count of voices a DSP could mix) */
    uint8_t ondsp;           /* its decoder (smp, p1, p2) is in its record with the DSP, not here (dsp_sync) */
    uint32_t dsp_seq;        /* the last DSP run its record went with (0: none) */
    int16_t smp[29];         /* the block's samples from [1], the one before them in [0] */
} Voice;

typedef struct Core {
    Voice v[VOICES];
    uint32_t endx;
    uint16_t param[0x20];    /* master params */
    uint32_t sw[0x20];       /* switches: a bit a voice */
    uint32_t addr[0x20];     /* ESA, EEA, TSA, IRQA */
    uint32_t attr[8];
} Core;

static uint8_t *g_spu_ram;
static Core g_core[2];
static uint64_t g_last_us;
static uint64_t g_sample_clock;      /* 48 kHz output ticks since start */
static struct { uint32_t func, arg; int pending; } g_trans[2];
static uint32_t g_spu2_func, g_spu2_arg;
static int g_irq_pending[2];

/* a core's volume register as a factor of 0x8000 (a fixed volume: 15 bits with a sign) */
static int32_t vol_of(uint32_t reg)
{
    return (int16_t)((reg & 0x7FFF) << 1);
}

/* ---- voices -------------------------------------------------------------------------------- */

static uint32_t g_k_blocks;     /* ADPCM blocks decoded (the status line) */

/* a block's 28 samples into d[1..28], the decoder's last two samples in *pp1, *pp2 (a byte, two samples, a turn) */
static inline void adpcm_block(const uint8_t *b, int16_t *d, int32_t *pp1, int32_t *pp2)
{
    static const int8_t f0[8] = {0, 60, 115, 98, 122, 0, 0, 0}, f1[8] = {0, 0, -52, -55, -60, 0, 0, 0};
    const int32_t a = f0[(b[0] >> 4) & 7], c = f1[(b[0] >> 4) & 7];
    int shift = b[0] & 15, k;
    if (shift > 12)
        shift = 9;
    b += 2;
    if (!(a | c)) {
        for (k = 0; k < 14; k++) {
            const uint32_t n = b[k];
            d[2 * k + 1] = (int16_t)((int16_t)(n << 12) >> shift);
            d[2 * k + 2] = (int16_t)((int16_t)((n & 0xF0u) << 8) >> shift);
        }
        *pp1 = d[28];
        *pp2 = d[27];
    } else {
        int32_t p1 = *pp1, p2 = *pp2;
        for (k = 0; k < 14; k++) {
            const uint32_t n = b[k];
            int32_t s1 = ((int16_t)(n << 12) >> shift) + ((p1 * a + p2 * c + 32) >> 6), s2;
            if (PS2_UNLIKELY((uint32_t)(s1 + 32768) > 65535u))
                s1 = s1 < 0 ? -32768 : 32767;
            s2 = ((int16_t)((n & 0xF0u) << 8) >> shift) + ((s1 * a + p1 * c + 32) >> 6);
            if (PS2_UNLIKELY((uint32_t)(s2 + 32768) > 65535u))
                s2 = s2 < 0 ? -32768 : 32767;
            d[2 * k + 1] = (int16_t)s1;
            d[2 * k + 2] = (int16_t)s2;
            p2 = s1;
            p1 = s2;
        }
        *pp1 = p1;
        *pp2 = p2;
    }
}

/* a voice enters the block at `at`: its 28 samples (this is a seventh of the mixer's time) */
static void decode_at(Voice *x, uint32_t at)
{
    int32_t p1 = x->p1, p2 = x->p2;
    g_k_blocks++;
    x->smp[0] = x->smp[28];
    adpcm_block(g_spu_ram + (at & (SPU_RAM_SIZE - 1) & ~15u), x->smp, &p1, &p2);
    x->p1 = p1;
    x->p2 = p2;
}

/* a voice that went unheard is heard again: its block, the one before it first for the decoder's history */
static void warm_up(Voice *x)
{
    x->p1 = x->p2 = 0;
    x->smp[28] = 0;
    if (x->cold == 1 && (x->nax & ~15u) >= 16)
        decode_at(x, x->nax - 16);
    decode_at(x, x->nax);
    x->cold = 0;
}

/* debugging ("spudump 1"): the voices' events since the last status block: when (ms), which kind
 * ('k' keyed on, 'o' keyed off, 'e' its data ended), core and voice, its place, its pitch */
extern int g_spu_dump;
/* the tick's rate and the interrupts it handed to the driver, since the last status block */
static uint32_t g_tk_n, g_tk_max_us, g_irq_calls[4];
static uint64_t g_tk_sum_us;

#define EV_MAX 480
static uint32_t g_ev_log[EV_MAX][4], g_ev_n, g_ev_all, g_tr_under;

static void voice_event(int kind, int core, int v)
{
    const Voice *x = &g_core[core].v[v];
    g_ev_all++;
    if (g_ev_n < EV_MAX) {
        g_ev_log[g_ev_n][0] = (uint32_t)(plat_time_us() / 1000u);
        g_ev_log[g_ev_n][1] = (uint32_t)kind << 16 | (uint32_t)core << 8 | (uint32_t)v;
        g_ev_log[g_ev_n][2] = kind == 'k' ? x->ssa : x->nax;
        g_ev_log[g_ev_n++][3] = x->param[VP_PITCH];
    }
}

static void key_on(int core, int v)
{
    Voice *x = &g_core[core].v[v];
    if (PS2_UNLIKELY(g_spu_dump))
        voice_event('k', core, v);
    x->nax = x->ssa & ~15u;
    x->pos = 0;
    x->playing = 1;
    x->released = 0;
    x->env = 0;
    x->env_wait = 0;
    x->phase = ENV_ATTACK;
    x->cold = 2;                /* its first block, with nothing before it */
    x->ondsp = 0;               /* (a record with the DSP is of the sound before) */
    g_core[core].endx &= ~(1u << v);
}

static void key_off(int core, int v)
{
    Voice *x = &g_core[core].v[v];
    if (PS2_UNLIKELY(g_spu_dump) && x->playing && !x->released)
        voice_event('o', core, v);
    x->released = 1;
    if (x->playing && x->phase != ENV_RELEASE) {
        x->phase = ENV_RELEASE;
        x->env_wait = 0;
    }
}

#define HOLD 0x40000000         /* a level that has nowhere to go until a register is written */

/* The chip's rule for a level that moves (the envelope's four phases and the volume slides): a step
 * of 7..4 up or 8..5 down, shifted left by 11 - shift, every 2^(shift - 11) samples; an exponential
 * rise goes at a quarter of the speed above 0x6000, an exponential fall steps by its level. Returns
 * the samples the new level stands. */
static int32_t rate_step(int32_t *level, int32_t shift, int32_t step, int expo, int down)
{
    int32_t cycles = shift > 11 ? 1 << (shift - 11) : 1, v = *level;
    if (shift < 11)
        step = step * (1 << (11 - shift));
    if (expo && !down && v > 0x6000)
        cycles *= 4;
    if (expo && down)
        step = (step * v) >> 15;
    v += step;
    *level = v > 0x7FFF ? 0x7FFF : v < 0 ? 0 : v;
    return cycles;
}

static void env_step(Voice *x)
{
    const uint32_t a1 = x->param[VP_ADSR1], a2 = x->param[VP_ADSR2];
    int down;
    switch (x->phase) {
    case ENV_ATTACK:
        x->env_wait = rate_step(&x->env, (int32_t)(a1 >> 10) & 31, 7 - (int32_t)((a1 >> 8) & 3), (int)(a1 >> 15), 0);
        if (x->env >= 0x7FFF)
            x->phase = ENV_DECAY;
        break;
    case ENV_DECAY:
        x->env_wait = rate_step(&x->env, (int32_t)(a1 >> 4) & 15, -8, 1, 1);
        break;
    case ENV_SUSTAIN:
        down = (int)(a2 >> 14) & 1;
        x->env_wait = rate_step(&x->env, (int32_t)(a2 >> 8) & 31, down ? -8 + (int32_t)((a2 >> 6) & 3) : 7 - (int32_t)((a2 >> 6) & 3),
                                (int)(a2 >> 15), down);
        if (x->env == (down ? 0 : 0x7FFF))
            x->env_wait = HOLD;
        break;
    case ENV_RELEASE:
        x->env_wait = rate_step(&x->env, (int32_t)a2 & 31, -8, (int)(a2 >> 5) & 1, 1);
        if (x->env <= 0)
            x->phase = ENV_OFF;
        break;
    default:
        x->env_wait = HOLD;
        return;
    }
    if (x->phase == ENV_DECAY && x->env <= (int32_t)((a1 & 15) + 1) * 0x800)
        x->phase = ENV_SUSTAIN;
}

/* the envelope over n samples */
static void env_run(Voice *x, uint32_t n)
{
    while (n && x->phase != ENV_OFF) {
        uint32_t t;
        if (x->env_wait <= 0)
            env_step(x);
        t = n < (uint32_t)x->env_wait ? n : (uint32_t)x->env_wait;
        x->env_wait -= (int32_t)t;
        n -= t;
    }
}

/* a volume register is written: a fixed volume is there at once (15 bits with a sign), a slide
 * (bit 15) starts from where the volume is */
static void vol_set(Voice *x, int ch, uint32_t reg)
{
    if (!(reg & 0x8000))
        x->vol[ch] = (int16_t)(reg << 1);
    else if (x->vol[ch] < 0)
        x->vol[ch] = -x->vol[ch];
    x->vol_wait[ch] = 0;
}

/* a volume over n samples: bit 14 exponential, bit 13 down, bits 6..2 the shift, 1..0 the step */
static void vol_run(Voice *x, int ch, uint32_t n)
{
    const uint32_t reg = x->param[ch ? VP_VOLR : VP_VOLL];
    const int down = (int)(reg >> 13) & 1;
    if (!(reg & 0x8000))
        return;
    while (n) {
        uint32_t t;
        if (x->vol_wait[ch] <= 0) {
            if ((reg & 0x7F) != 0x7F)
                x->vol_wait[ch] = rate_step(&x->vol[ch], (int32_t)(reg >> 2) & 31,
                                            down ? -8 + (int32_t)(reg & 3) : 7 - (int32_t)(reg & 3), (int)(reg >> 14) & 1, down);
            if ((reg & 0x7F) == 0x7F || x->vol[ch] == (down ? 0 : 0x7FFF))
                x->vol_wait[ch] = HOLD;
        }
        t = n < (uint32_t)x->vol_wait[ch] ? n : (uint32_t)x->vol_wait[ch];
        x->vol_wait[ch] -= (int32_t)t;
        n -= t;
    }
}

static void check_irq(int core, uint32_t block)
{
    Core *k = &g_core[core];
    if ((k->attr[CORE_IRQ_ENABLE >> 1] & 1) && (k->addr[A_IRQA - A_ESA] & ~15u) == block)
        g_irq_pending[core] = 1;
}

/* a voice has played its block out: the next one, by the block's flags; 0 when the voice ends */
static int next_block(int core, int v)
{
    Voice *x = &g_core[core].v[v];
    uint32_t block = x->nax & (SPU_RAM_SIZE - 1) & ~15u;
    uint8_t flags = g_spu_ram[block + 1];
    check_irq(core, block);
    if (flags & 4)
        x->lsax = block;
    if (flags & 1) {
        g_core[core].endx |= 1u << v;
        if (flags & 2) {
            x->nax = x->lsax;
        } else {
            if (PS2_UNLIKELY(g_spu_dump))
                voice_event('e', core, v);
            x->playing = 0;
            x->phase = ENV_OFF;
            x->env = 0;
            x->nax = block + 16;
            return 0;
        }
    } else {
        x->nax = block + 16;
    }
    return 1;
}

/* debugging and the status line */
int g_spu_mute;                 /* autopilot "mute 1": the chip's time runs, nothing is mixed */
int g_spu_dump;                 /* autopilot "spudump 1": the playing voices with every status block */
static uint32_t g_tr_stat[2][4] = {{0, 0, 0xFFFFFFFFu, 0}, {0, 0, 0xFFFFFFFFu, 0}};    /* writes, reads: count, bytes, lowest, highest */
static uint32_t g_fx_calls, g_fx_mode;
static uint32_t g_tr_log[48][4], g_tr_log_n;
static uint32_t g_spu_peak, g_spu_frames, g_spu_voices, g_spu_heard, g_spu_clipped, g_spu_dropped, g_spu_warm;
/* what a DSP could take over (the status line): the samples of heard voices by kind (dry only with a pitch that did not
 * move since the last tick, dry only with a pitch that moved, with a send to the effect unit), by core, and the blocks decoded */
static uint32_t g_k_heard[3], g_k_core[2], g_k_ck[2][3];
static uint64_t g_spu_ticks;

#define BLOCK_END (28u << 12)

#define MIX_CHUNK_MAX 256

/* Gains below this are not mixed (60 dB down: the driver fades a voice out over five seconds and
 * leaves it playing at nothing, 34 of them at a time). A setting, to try louder floors: the autopilot's "heard N" (0x100 is
 * 42 dB down); 0x20 until told otherwise. */
int g_gain_heard = 0x20;
#define GAIN_HEARD g_gain_heard

/* The sums are floats: a sample is 16 bits, a gain a fraction, and Broadway's paired singles take
 * two 16-bit samples out of memory as floats in one instruction and do a pair (left and right) in
 * each of the next (the integer loops were 25 to 36 cycles a sample with their two or three
 * multiplications, this one is 14 instructions whatever the voice's sides and slides). */
static float g_frac[256];               /* a place's fraction, 8 bits of it */
static float g_per[MIX_CHUNK_MAX + 1];  /* 1 / (32768 n): a gain's step over n samples */

static void mix_tables(void)
{
    int k;
    for (k = 0; k < 256; k++)
        g_frac[k] = (float)k * (1.0f / 256.0f);
    for (k = 1; k <= MIX_CHUNK_MAX; k++)
        g_per[k] = 1.0f / (32768.0f * (float)k);
}

/* A stretch of a block, cnt samples from `pos` at `pitch` a sample, into acc (pairs: left, right):
 * the sample between two of the block's by the place's fraction, times the gains G[0], G[1], which
 * move by G[2], G[3] a sample (and are left where they got to). Returns the place after. */
#if defined(GEKKO)
uint32_t voice_stretch(const int16_t *smp, uint32_t pos, uint32_t pitch, uint32_t cnt, float *acc, float *G,
                       const float *frac);
void mix_out(const float *c0, const float *c1, const int16_t *eng, int16_t *out, uint32_t n, const float *K);
void mix_f2s16(const float *src, int16_t *dst, uint32_t pairs);
void mix_s16_add(const int16_t *src, float *dst, uint32_t pairs);
/* GQR 4: 16-bit samples with a sign, in and out (the store stops at the 16 bits) */
#define MIX_GQR() __asm__ volatile("mtspr 916,%0" : : "r"(0x00070007u))
__asm__(
    "\t.pushsection .text\n"
    "\t.align 2\n"
    "\t.globl voice_stretch\n"
    "\t.type voice_stretch, @function\n"
    "voice_stretch:\n"
    "\tmtctr 6\n"
    "\tpsq_l 5,0(8),0,0\n"            /* the gains */
    "\tpsq_l 6,8(8),0,0\n"            /* their steps */
    "1:\n"
    "\trlwinm 10,4,21,11,30\n"        /* the sample's index, in bytes */
    "\trlwinm 11,4,30,22,29\n"        /* the fraction's, in the table */
    "\tpsq_lx 1,3,10,0,4\n"           /* this sample and the next */
    "\tlfsx 2,9,11\n"
    "\tps_merge11 3,1,1\n"
    "\tfsubs 3,3,1\n"
    "\tfmadds 3,3,2,1\n"              /* the sample at the place */
    "\tpsq_l 4,0(7),0,0\n"
    "\tps_madds0 4,5,3,4\n"           /* left and right */
    "\tpsq_st 4,0(7),0,0\n"
    "\tps_add 5,5,6\n"
    "\tadd 4,4,5\n"
    "\taddi 7,7,8\n"
    "\tbdnz 1b\n"
    "\tpsq_st 5,0(8),0,0\n"
    "\tmr 3,4\n"
    "\tblr\n"
    "\t.size voice_stretch, .-voice_stretch\n"
    /* mix_out: K = core 0's factor, core 1's switch, the engine's volume, core 1's volume, 32767
     * and -32768, a pair each. Core 0's sum stops at 16 bits, the whole does before the volume. */
    "\t.align 2\n"
    "\t.globl mix_out\n"
    "\t.type mix_out, @function\n"
    "mix_out:\n"
    "\tmtctr 7\n"
    "\tpsq_l 6,0(8),0,0\n"
    "\tpsq_l 7,8(8),0,0\n"
    "\tpsq_l 8,16(8),0,0\n"
    "\tpsq_l 9,24(8),0,0\n"
    "\tpsq_l 10,32(8),0,0\n"
    "\tpsq_l 11,40(8),0,0\n"
    "1:\n"
    "\tpsq_l 1,0(3),0,0\n"
    "\tpsq_l 2,0(4),0,0\n"
    "\tpsq_l 3,0(5),0,4\n"
    "\tps_sub 0,1,10\n"
    "\tps_sel 1,0,10,1\n"
    "\tps_sub 0,1,11\n"
    "\tps_sel 1,0,1,11\n"
    "\tps_mul 2,2,7\n"
    "\tps_madd 2,1,6,2\n"
    "\tps_madd 2,3,8,2\n"
    "\tps_sub 0,2,10\n"
    "\tps_sel 2,0,10,2\n"
    "\tps_sub 0,2,11\n"
    "\tps_sel 2,0,2,11\n"
    "\tps_mul 2,2,9\n"
    "\tpsq_st 2,0(6),0,4\n"
    "\taddi 3,3,8\n"
    "\taddi 4,4,8\n"
    "\taddi 5,5,4\n"
    "\taddi 6,6,4\n"
    "\tbdnz 1b\n"
    "\tblr\n"
    "\t.size mix_out, .-mix_out\n"
    /* mix_f2s16: pairs of floats as 16-bit samples (stopped at the 16 bits) */
    "\t.align 2\n"
    "\t.globl mix_f2s16\n"
    "\t.type mix_f2s16, @function\n"
    "mix_f2s16:\n"
    "\tmtctr 5\n"
    "1:\n"
    "\tpsq_l 1,0(3),0,0\n"
    "\tpsq_st 1,0(4),0,4\n"
    "\taddi 3,3,8\n"
    "\taddi 4,4,4\n"
    "\tbdnz 1b\n"
    "\tblr\n"
    "\t.size mix_f2s16, .-mix_f2s16\n"
    /* mix_s16_add: pairs of 16-bit samples added to pairs of floats */
    "\t.align 2\n"
    "\t.globl mix_s16_add\n"
    "\t.type mix_s16_add, @function\n"
    "mix_s16_add:\n"
    "\tmtctr 5\n"
    "1:\n"
    "\tpsq_l 1,0(3),0,4\n"
    "\tpsq_l 2,0(4),0,0\n"
    "\tps_add 2,2,1\n"
    "\tpsq_st 2,0(4),0,0\n"
    "\taddi 3,3,4\n"
    "\taddi 4,4,8\n"
    "\tbdnz 1b\n"
    "\tblr\n"
    "\t.size mix_s16_add, .-mix_s16_add\n"
    "\t.popsection\n");
#else
#define MIX_GQR() ((void)0)
static inline float mix_stop(float v)
{
    return v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
}

static uint32_t voice_stretch(const int16_t *smp, uint32_t pos, uint32_t pitch, uint32_t cnt, float *acc, float *G,
                              const float *frac)
{
    float gl = G[0], gr = G[1];
    const float dl = G[2], dr = G[3];
    do {
        const float s0 = (float)smp[pos >> 12], s1 = (float)smp[(pos >> 12) + 1];
        const float sv = s0 + (s1 - s0) * frac[(pos >> 4) & 0xFF];
        acc[0] += sv * gl;
        acc[1] += sv * gr;
        acc += 2;
        gl += dl;
        gr += dr;
        pos += pitch;
    } while (--cnt);
    G[0] = gl;
    G[1] = gr;
    return pos;
}

static void mix_out(const float *c0, const float *c1, const int16_t *eng, int16_t *out, uint32_t n, const float *K)
{
    uint32_t i;
    for (i = 0; i < 2 * n; i++) {
        const int ch = (int)(i & 1);
        const float v = mix_stop(c0[i]) * K[ch] + c1[i] * K[2 + ch] + (float)eng[i] * K[4 + ch];
        out[i] = (int16_t)(mix_stop(v) * K[6 + ch]);
    }
}

static void mix_f2s16(const float *src, int16_t *dst, uint32_t pairs)
{
    uint32_t i;
    for (i = 0; i < 2 * pairs; i++)
        dst[i] = (int16_t)mix_stop(src[i]);
}

static void mix_s16_add(const int16_t *src, float *dst, uint32_t pairs)
{
    uint32_t i;
    for (i = 0; i < 2 * pairs; i++)
        dst[i] += (float)src[i];
}
#endif

static inline int32_t gain_size(int32_t g)
{
    return g < 0 ? -g : g;
}

/* ---- the DSP's share ------------------------------------------------------------------------ */
/* "--dsp 1" (autopilot "dsp 1"; on by default on a console): in a race (the engine's sound made here) the voices heard
 * dry only are decoded, stretched and summed by the Wii's DSP (platform/wii/dsp_mix.c, tools/dsp/dspmix.s). Their
 * places, blocks, end flags and interrupts stay here and on time: voice_run walks them as ever, but only lists the
 * blocks the place enters (runs of them in sound RAM), and the DSP takes those from memory and decodes them itself.
 * The CPU touches none of their samples. A voice's decoder (its block, the sample before, p1, p2) is its record while
 * the DSP has it (ondsp): written by dsp_hand when the voice goes to the DSP, read back by dsp_sync when it comes back
 * to the CPU's way (both after the last run with the voice is out); going unheard past a block end or being keyed on
 * drops it (warm_up begins the decoder again, as ever). The run goes when the chunk's voices are done, the DSP works
 * on it while the reverb and the engine are made here, and the chunk (its dry sums, engine samples and factors) waits
 * until the run's sums are in: at the latest at the next tick, so the sound leaves one tick (some 6 ms) later. Four
 * runs can be out; a run the DSP does not do (it was given up on) is done here. "dsp 2": the jobs are made and done
 * here at once (job_cpu: decode_at's arithmetic, voice_stretch), in the CPU's order; "dsp 3": each voice both ways,
 * compared (voice_check). Neither needs the DSP. */
#if defined(GEKKO)
#include <ogc/cache.h>
#include "wii/dsp_mix.h"
#define DSP_MIX 1
#else
#define DSP_MIX 0
#endif

typedef struct DspRun DspRun;
#if DSP_MIX
#define DSP_RUNS_WORDS (DSPMIX_BLOCKS * 4 + 8)   /* a job's runs' list at most (a run a block entered), padded to 32 bytes */
#define DSP_POOL_WORDS (DSPMIX_MAX_JOBS * DSP_RUNS_WORDS)

struct DspRun {
    DspMixCmd *cmd;
    int16_t *pool;                      /* the jobs' runs' lists */
    uint32_t jobs, used;                /* jobs written, words of the pool given out */
    uint32_t seq;                       /* the run's number (the voices' dsp_seq) */
    int here;                           /* "dsp 2" (and "dsp 3"'s second way): each job done here at once */
    int check;                          /* "dsp 3": each voice both ways, compared (voice_check) */
};

/* a chunk waiting for its run's sums */
typedef struct MixLater {
    float dry[2][MIX_CHUNK_MAX * 2];
    int16_t eng[MIX_CHUNK_MAX * 2];
    float K[12];
    uint32_t m;
    int sent;
    DspRun run;
} MixLater;

/* MEM1 (the DSP reads it), taken when the DSP is first used: no room is spent on it otherwise */
static int16_t (*g_dsp_pool)[DSP_POOL_WORDS];   /* a run's runs' lists, DSPMIX_SLOTS of them */
static MixLater *g_later;                       /* entry k's run is the DSP's slot k */
static uint32_t g_later_head, g_later_n;
static int16_t *g_mix_out;
static DspMixRecord *g_rec;                     /* the voices' decoders while the DSP has them: core * VOICES + voice */
static uint32_t g_dsp_seq, g_retired_seq;       /* runs made, and the last one whose chunk went out */
/* the status line: samples of voices the DSP took, its jobs and the blocks it decoded, voices that found no room,
 * chunks that waited for their sums (and the longest wait), runs done here after all, voices taken back (and how
 * many of those had to wait for their run) */
static uint32_t g_dsp_samples, g_dsp_jobs, g_dsp_blocks, g_dsp_full, g_dsp_waits, g_dsp_lost, g_dsp_back, g_dsp_back_waits;
static uint64_t g_dsp_wait_max, g_dsp_wait_sum;
/* "dsp 3": voices checked, those whose state after the two ways differed, those whose sums did */
static uint32_t g_chk_voices, g_chk_state, g_chk_jobs, g_chk_sums;
static float g_chk_maxd;

static void dsp_add_selftest(void);
static void mix_retire_until(uint32_t seq);

static int dsp_mem(void)
{
    static int tried;
    if (!g_later && !tried) {
        tried = 1;
        g_dsp_pool = dspmix_mem(DSPMIX_SLOTS * sizeof g_dsp_pool[0]);
        g_later = dspmix_mem(DSPMIX_SLOTS * sizeof g_later[0]);
        g_mix_out = dspmix_mem(MIX_CHUNK_MAX * 2 * sizeof(int16_t));
        g_rec = dspmix_mem(2 * VOICES * sizeof(DspMixRecord));
        if (!g_dsp_pool || !g_later || !g_mix_out || !g_rec) {
            g_later = NULL;
            LOG("sound: no room in MEM1 for the DSP's run lists and records: the CPU mixes everything");
        } else {
            dsp_add_selftest();
        }
    }
    return g_later != NULL;
}

/* a voice's decoder back from its record (its last run's chunk out first: the DSP has written it then) */
static void dsp_sync(Voice *x, DspMixRecord *rec)
{
    g_dsp_back++;
    if (x->dsp_seq > g_retired_seq) {
        g_dsp_back_waits++;
        mix_retire_until(x->dsp_seq);
    }
    DCInvalidateRange(rec, sizeof *rec);
    memcpy(x->smp, rec->smp, sizeof x->smp);
    x->p1 = rec->p1;
    x->p2 = rec->p2;
    x->ondsp = 0;
}

/* the voice's decoder into its record, for the DSP (no run still out may write it after this) */
static void dsp_hand(Voice *x, DspMixRecord *rec)
{
    if (x->dsp_seq > g_retired_seq)
        mix_retire_until(x->dsp_seq);
    memcpy(rec->smp, x->smp, sizeof x->smp);
    rec->p1 = (int16_t)x->p1;
    rec->p2 = (int16_t)x->p2;
    rec->spare = 0;
    DCFlushRange(rec, sizeof *rec);
    x->ondsp = 1;
}

/* A job done here as the DSP does it: the stream from the record and its runs' blocks (decode_at's arithmetic), the
 * record's new state, and the stretch by voice_stretch with the gains G (left where they got to). */
static void job_cpu(DspMixRecord *rec, const DspMixRun *rl, uint32_t nruns, uint32_t pos, uint32_t pitch, uint32_t cnt, float *acc,
                    float *G)
{
    static int16_t st[29 + 28 * DSPMIX_BLOCKS + 3];
    int32_t p1, p2;
    uint32_t k, e = 0;
    DCInvalidateRange(rec, sizeof *rec);
    memcpy(st, rec->smp, sizeof rec->smp);
    p1 = rec->p1;
    p2 = rec->p2;
    for (k = 0; k < nruns; k++) {
        const uint8_t *b = (const uint8_t *)(((uint32_t)rl[k].at_hi << 16 | rl[k].at_lo) | 0x80000000u);
        uint32_t n;
        for (n = 0; n < rl[k].blocks; n++, e++)
            adpcm_block(b + 16 * n, st + 28 * (e + 1), &p1, &p2);
    }
    memcpy(rec->smp, st + 28 * e, sizeof rec->smp);
    rec->p1 = (int16_t)p1;
    rec->p2 = (int16_t)p2;
    DCFlushRange(rec, sizeof *rec);
    if (cnt)
        voice_stretch(st, pos, pitch, cnt, acc, G, g_frac);
}
#endif

/* One voice for n samples (256 at most), into acc (pairs: left, right) when anything listens; with a run, a voice
 * heard is the run's job (or done here at once, "dsp 2") when there is room. */
static void voice_run(int core, int v, uint32_t n, float *acc, uint32_t mixl, uint32_t mixr, DspRun *run)
{
    Voice *x = &g_core[core].v[v];
    const uint32_t pitch = x->param[VP_PITCH] > 0x3FFFu ? 0x3FFFu : x->param[VP_PITCH];
    const int32_t e0 = x->env, l0 = x->vol[0], r0 = x->vol[1];
    int32_t gl0, gr0, gl1, gr1;
    uint32_t pos = x->pos, left = n;
    env_run(x, n);
    vol_run(x, 0, n);
    vol_run(x, 1, n);
    gl0 = mixl ? (e0 * l0) >> 15 : 0;
    gr0 = mixr ? (e0 * r0) >> 15 : 0;
    gl1 = mixl ? (x->env * x->vol[0]) >> 15 : 0;
    gr1 = mixr ? (x->env * x->vol[1]) >> 15 : 0;
    if (!acc || (gain_size(gl0) | gain_size(gr0) | gain_size(gl1) | gain_size(gr1)) < GAIN_HEARD) {
        /* unheard: its place alone */
        pos += n * pitch;
        while (pos >= BLOCK_END) {
            pos -= BLOCK_END;
            x->cold = 1;
            x->ondsp = 0;       /* (its decoder is begun again when it is heard: a record with the DSP is of no use) */
            if (!next_block(core, v))
                break;
        }
    } else {
        /* the gains in a straight line from the tick's start to its end */
        float G[4];
        G[0] = (float)gl0 * (1.0f / 32768.0f);
        G[1] = (float)gr0 * (1.0f / 32768.0f);
        G[2] = gl1 == gl0 ? 0.0f : (float)(gl1 - gl0) * g_per[n];
        G[3] = gr1 == gr0 ? 0.0f : (float)(gr1 - gr0) * g_per[n];
        g_spu_heard += n;
        if (x->cold) {
            warm_up(x);
            g_spu_warm++;
        }
#if DSP_MIX
        if (run && (run->here || run->jobs < DSPMIX_MAX_JOBS) && run->used + DSP_RUNS_WORDS <= DSP_POOL_WORDS) {
            /* the DSP's job: the place walked as below, the blocks it enters listed as runs, not decoded */
            DspMixRecord *const rec = &g_rec[core * VOICES + v];
            DspMixRun *const rl = (DspMixRun *)(run->pool + run->used);
            const uint32_t pos0 = pos;
            uint32_t cnt = 0, nruns = 0, nb = 0;
            if (!x->ondsp)
                dsp_hand(x, rec);
            while (left) {
                uint32_t c = left;
                if (pitch) {
                    const uint32_t room = (BLOCK_END - pos + pitch - 1) / pitch;
                    if (c > room)
                        c = room;
                }
                pos += c * pitch;
                cnt += c;
                left -= c;
                if (pos >= BLOCK_END) {
                    uint32_t at;
                    pos -= BLOCK_END;
                    if (!next_block(core, v))
                        break;
                    at = (uint32_t)(g_spu_ram + (x->nax & (SPU_RAM_SIZE - 1) & ~15u)) & 0x1FFFFFFFu;
                    nb++;
                    if (nruns && ((uint32_t)rl[nruns - 1].at_hi << 16 | rl[nruns - 1].at_lo) + 16u * rl[nruns - 1].blocks == at) {
                        rl[nruns - 1].blocks++;
                    } else {
                        rl[nruns].at_hi = (uint16_t)(at >> 16);
                        rl[nruns].at_lo = (uint16_t)at;
                        rl[nruns].blocks = 1;
                        rl[nruns].spare = 0;
                        nruns++;
                    }
                }
            }
            g_dsp_samples += cnt;
            g_dsp_jobs++;
            g_dsp_blocks += nb;
            if (run->here) {
                job_cpu(rec, rl, nruns, pos0, pitch, cnt, acc, G);
            } else {
                DspMixJob *const jb = &run->cmd->job[run->jobs++];
                const uint32_t phys = (uint32_t)rl & 0x1FFFFFFFu;
                const int32_t dl = n > 1 ? (int32_t)((int64_t)(gl1 - gl0) * 65536 / (int32_t)n) : 0;
                const int32_t dr = n > 1 ? (int32_t)((int64_t)(gr1 - gr0) * 65536 / (int32_t)n) : 0;
                jb->runs_hi = (uint16_t)(phys >> 16);
                jb->runs_lo = (uint16_t)phys;
                jb->nruns = (uint16_t)nruns;
                jb->coref = (uint16_t)(core | ((gl0 | gl1) ? DSPMIX_LEFT : 0) | ((gr0 | gr1) ? DSPMIX_RIGHT : 0));
                jb->cnt = (uint16_t)cnt;
                jb->idx = (uint16_t)(pos0 >> 12);
                jb->frac = (uint16_t)((pos0 & 0xFFF) << 4);
                jb->pint = (uint16_t)(pitch >> 12);
                jb->pfrac = (uint16_t)((pitch & 0xFFF) << 4);
                jb->gl = (int16_t)gl0;
                jb->gr = (int16_t)gr0;
                jb->dl_hi = (uint16_t)((uint32_t)dl >> 16);
                jb->dl_lo = (uint16_t)dl;
                jb->dr_hi = (uint16_t)((uint32_t)dr >> 16);
                jb->dr_lo = (uint16_t)dr;
                jb->voice = (uint16_t)(core * VOICES + v);
                run->used += (nruns * 4 + 15) & ~15u;
                x->dsp_seq = run->seq;
            }
            left = 0;
        } else {
            if (run)
                g_dsp_full++;
            if (x->ondsp)
                dsp_sync(x, &g_rec[core * VOICES + v]);
        }
#endif
        while (left) {
            uint32_t cnt = left;
            if (pitch) {
                const uint32_t room = (BLOCK_END - pos + pitch - 1) / pitch;   /* samples the block still gives */
                if (cnt > room)
                    cnt = room;
            }
            pos = voice_stretch(x->smp, pos, pitch, cnt, acc, G, g_frac);
            acc += 2 * cnt;
            left -= cnt;
            if (pos >= BLOCK_END) {
                pos -= BLOCK_END;
                if (!next_block(core, v))
                    break;
                decode_at(x, x->nax);
            }
        }
    }
    x->pos = pos;
    if (x->phase == ENV_OFF)
        x->playing = 0;         /* released, and its envelope has come down */
}

#if DSP_MIX
/* "dsp 3": a voice the DSP would take, both ways from the same state: the CPU's into a, then (the state put back) its
 * job done here into b. The voice's state after the two - its place, blocks, end flag, the core's interrupt, and its
 * decoder (the CPU's against the record the job wrote) - and the sums must be the same to the bit. What goes on is the
 * CPU's. */
typedef struct DspCheck {
    float a[MIX_CHUNK_MAX * 2], b[MIX_CHUNK_MAX * 2];
} DspCheck;
static DspCheck *g_chk;

static int same_place(const Voice *p, const Voice *q)
{
    return p->ssa == q->ssa && p->lsax == q->lsax && p->nax == q->nax && p->playing == q->playing && p->released == q->released &&
           p->pos == q->pos && p->env == q->env && p->env_wait == q->env_wait && p->vol[0] == q->vol[0] && p->vol[1] == q->vol[1] &&
           p->vol_wait[0] == q->vol_wait[0] && p->vol_wait[1] == q->vol_wait[1] && p->phase == q->phase && p->cold == q->cold;
}

static void voice_check(int core, int v, uint32_t n, float *acc, uint32_t mixl, uint32_t mixr, DspRun *run)
{
    Voice *const x = &g_core[core].v[v];
    DspMixRecord *const rec = &g_rec[core * VOICES + v];
    Voice x0, x1;
    uint32_t endx0, endx1, heard0, warm0, blocks0, jobs0, i;
    int irq0, irq1, diff = 0;
    if (!g_chk && !(g_chk = dspmix_mem(sizeof *g_chk))) {
        voice_run(core, v, n, acc, mixl, mixr, NULL);
        return;
    }
    if (x->ondsp)
        dsp_sync(x, rec);
    x0 = *x;
    endx0 = g_core[core].endx;
    irq0 = g_irq_pending[core];
    heard0 = g_spu_heard;
    warm0 = g_spu_warm;
    blocks0 = g_k_blocks;
    memset(g_chk->a, 0, n * 8);
    memset(g_chk->b, 0, n * 8);
    voice_run(core, v, n, g_chk->a, mixl, mixr, NULL);
    x1 = *x;
    endx1 = g_core[core].endx;
    irq1 = g_irq_pending[core];
    *x = x0;
    g_core[core].endx = endx0;
    g_irq_pending[core] = irq0;
    g_spu_heard = heard0;
    g_spu_warm = warm0;
    g_k_blocks = blocks0;
    run->jobs = run->used = 0;
    run->here = 1;
    jobs0 = g_dsp_jobs;
    voice_run(core, v, n, g_chk->b, mixl, mixr, run);
    g_chk_voices++;
    if (g_dsp_jobs != jobs0) {
        /* it was a job: the decoder is in the record */
        g_chk_jobs++;
        DCInvalidateRange(rec, sizeof *rec);
        diff = memcmp(rec->smp, x1.smp, sizeof x1.smp) || rec->p1 != (int16_t)x1.p1 || rec->p2 != (int16_t)x1.p2;
    } else {
        diff = memcmp(x->smp, x1.smp, sizeof x1.smp) || x->p1 != x1.p1 || x->p2 != x1.p2;
    }
    if (diff || !same_place(x, &x1) || g_core[core].endx != endx1 || g_irq_pending[core] != irq1)
        g_chk_state++;
    diff = 0;
    for (i = 0; i < 2 * n; i++) {
        const float d = g_chk->a[i] > g_chk->b[i] ? g_chk->a[i] - g_chk->b[i] : g_chk->b[i] - g_chk->a[i];
        if (d > g_chk_maxd)
            g_chk_maxd = d;
        diff |= memcmp(&g_chk->a[i], &g_chk->b[i], sizeof(float)) != 0;
        acc[i] += g_chk->a[i];
    }
    g_chk_sums += diff;
    *x = x1;
    g_core[core].endx = endx1;
    g_irq_pending[core] = irq1;
}
#endif

#define MIX_CHUNK MIX_CHUNK_MAX
#define AUDIO_AHEAD 2400        /* 50 ms kept with the output */

/* MMIX: what a core's output is made of */
#define MMIX_SINR 0x004u        /* the other core's output (core 1 alone has that input) */
#define MMIX_SINL 0x008u
#define MMIX_MINR 0x040u        /* the sound data input rings */
#define MMIX_MINL 0x080u
#define MMIX_MSNDR 0x400u       /* the voices' dry sum */
#define MMIX_MSNDL 0x800u

/* the rings, in bytes of sound RAM: a core's dry and wet sums, its sound data input */
#define RING_DRY(core_, ch_) (0x2000u + (uint32_t)(core_) * 0x1000u + (uint32_t)(ch_) * 0x400u)
#define RING_WET(core_, ch_) (0x2800u + (uint32_t)(core_) * 0x1000u + (uint32_t)(ch_) * 0x400u)
#define RING_IN(core_, ch_) (0x4000u + (uint32_t)(core_) * 0x800u + (uint32_t)(ch_) * 0x400u)
static uint32_t g_ring_read;    /* rings the driver has read from: bit (address / 0x400) */

static inline void ring_put(uint32_t at, int32_t v)
{
    g_spu_ram[at] = (uint8_t)v;
    g_spu_ram[at + 1] = (uint8_t)(v >> 8);
}

static inline int32_t ring_get(uint32_t at)
{
    return (int16_t)(g_spu_ram[at] | g_spu_ram[at + 1] << 8);
}

/* ---- reverb: the SPU2 effect unit ------------------------------------------------ */
/* The presets and coefficients are the ones libsd writes into the chip (ps2sdk
 * iop/sound/libsd/src/effect.c); the algorithm, its state and its two ways (a sample
 * at a time, a chunk at a time) are in iop_rev.h. A race uses mode 5 (hall) on core 1. */

static const uint16_t k_effect_sizes[10] = {
    0x1, 0x26C, 0x1F4, 0x484, 0x6FE, 0xADE, 0xF6C, 0x1804, 0x1804, 0x3C0
};
static const uint32_t k_effect_params[160] = {
    /* off */
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00010001, 0x00000000, 0x00000000,
    0x00000000, 0x00010001, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* room */
    0x005B007D, 0x54B86D80, 0x0000BED0, 0xBA800000, 0x53005800, 0x033304D6, 0x022703F0, 0x01EF0374,
    0x01B70336, 0x01B60335, 0x01B50334, 0x01B50334, 0x01B50334, 0x013601B4, 0x005C00B8, 0x80008000,
    /* studio 1 */
    0x00250033, 0x4FA870F0, 0x4410BCE0, 0x9C00C0F0, 0x4EC05280, 0x031B03E4, 0x02AF03A4, 0x02660372,
    0x025D031C, 0x018E025C, 0x0135022F, 0x00B701D2, 0x00B5018F, 0x008000B4, 0x0026004C, 0x80008000,
    /* studio 2 */
    0x007F00B1, 0x4FA870F0, 0x4510BCE0, 0xB4C0BEF0, 0x4EC05280, 0x076B0904, 0x065F0824, 0x061607A2,
    0x05ED076C, 0x042E05EC, 0x0305050F, 0x02B70462, 0x0265042F, 0x01B20264, 0x00800100, 0x80008000,
    /* studio 3 */
    0x00A900E3, 0x4FA86F60, 0x4510BCE0, 0xA680BEF0, 0x52C05680, 0x0B580DFB, 0x0A3C0D09, 0x09730BD9,
    0x08DA0B59, 0x05E908D9, 0x04B007EC, 0x03D206EF, 0x031D05EA, 0x0238031C, 0x00AA0154, 0x80008000,
    /* hall */
    0x013901A5, 0x50006000, 0xB8004C00, 0xC000BC00, 0x5C006000, 0x11BB15BA, 0x10BD14C2, 0x0DC111BC,
    0x0DC311C0, 0x09C10DC0, 0x07C10BC4, 0x06CD0A00, 0x05C109C2, 0x041A05C0, 0x013A0274, 0x80008000,
    /* space */
    0x0231033D, 0x50007E00, 0xB000B400, 0xB0004C00, 0x54006000, 0x1A311ED6, 0x183B1D14, 0x16B21BC2,
    0x15EF1A32, 0x105515EE, 0x0F2D1334, 0x0C5D11F6, 0x0AE11056, 0x07A20AE0, 0x02320464, 0x80008000,
    /* echo */
    0x00030003, 0x7FFF7FFF, 0x00000000, 0x81000000, 0x00000000, 0x0FFD1FFD, 0x00091009, 0x00000000,
    0x00091009, 0x1FFF1FFF, 0x1FFE1FFE, 0x1FFE1FFE, 0x1FFE1FFE, 0x10041008, 0x00040008, 0x80008000,
    /* delay */
    0x00030003, 0x7FFF7FFF, 0x00000000, 0x00000000, 0x00000000, 0x0FFD1FFD, 0x00091009, 0x00000000,
    0x00091009, 0x1FFF1FFF, 0x1FFE1FFE, 0x1FFE1FFE, 0x1FFE1FFE, 0x10041008, 0x00040008, 0x80008000,
    /* pipe */
    0x00130017, 0x4FA870F0, 0x4510BCE0, 0x8500BEF0, 0x54C05F80, 0x02AF0371, 0x01DF02E5, 0x01D702B0,
    0x026A0358, 0x011E01D6, 0x00B1012D, 0x0059011F, 0x00E301A0, 0x00400058, 0x00140028, 0x80008000,
};
static const char *const k_effect_name[10] = {
    "off", "room", "studio 1", "studio 2", "studio 3", "hall", "space", "echo", "delay", "pipe"
};

#include "iop_rev.h"

static Reverb g_rev[2];
static uint32_t g_rev_peak[2];
static int g_rev_logged[2];

/* "rev N" (autopilot): the reverb a chunk at a time (1: rev_run, its input and output converted a pair at a time), as
 * first written (0: rev_step a sample at a time), or both ways from the same start, compared (2: the dry sums, the
 * largest sample, the state and the work area after the chunk; the first way's result is kept) */
int g_rev_new = 1;
static uint64_t g_rev_ticks;            /* status line: the time in the reverb */
static uint32_t g_rev_chk_calls, g_rev_chk_bad, g_rev_chk_skipped, g_rev_chk_logged;
static int16_t *g_rev_chk_area;         /* the check's two copies of the work area (taken when it is first wanted) */
static uint32_t g_rev_chk_cap;
static int g_rev_ps;                    /* the quantised stores and loads gave the C conversions' values in the self-test */

static int rev_enabled(int core)
{
    return (g_core[core].attr[1] & 1) && g_rev[core].mode > 0 && g_rev[core].esa < g_rev[core].eea;
}

/* the wet sum through the effect, a sample at a time (as first written); the result times EVOL goes into the core's
 * dry sum */
static void rev_add_samples(Reverb *r, int16_t *mem, uint64_t first, uint32_t n, const float *wet, const int16_t *extra,
                            int32_t gain_l, int32_t gain_r, float *dry, int32_t evol_l, int32_t evol_r, uint32_t *peakp)
{
    uint32_t i;
    uint32_t peak = *peakp;
    for (i = 0; i < n; i++) {
        int32_t l = sat16((int32_t)wet[2 * i]), rr = sat16((int32_t)wet[2 * i + 1]);
        int16_t ol, orr;
        if (extra) {
            l = sat16(l + ((extra[2 * i] * gain_l) >> 15));
            rr = sat16(rr + ((extra[2 * i + 1] * gain_r) >> 15));
        }
        rev_step(r, mem, first + i, (int16_t)l, (int16_t)rr, &ol, &orr);
        dry[2 * i] += (float)((ol * evol_l) >> 15);
        dry[2 * i + 1] += (float)((orr * evol_r) >> 15);
        {
            const uint32_t top = (uint32_t)(ol < 0 ? -ol : ol) | (uint32_t)(orr < 0 ? -orr : orr);
            if (top > peak)
                peak = top;
        }
    }
    *peakp = peak;
}

/* The same a chunk at a time: the wet sum to 16 bits (and the extra input added), the chunk through rev_run, its output
 * times EVOL into the dry sum. Both conversions are pairs at a time (quantised stores and loads) where the self-test
 * found them the same as the C ones; an EVOL of -32768 can give 32768, which a 16-bit sample does not hold, so its
 * output is added by the C conversion. */
static void rev_add_chunk(Reverb *r, int16_t *mem, uint64_t first, uint32_t n, const float *wet, const int16_t *extra,
                          int32_t gain_l, int32_t gain_r, float *dry, int32_t evol_l, int32_t evol_r, uint32_t *peakp)
{
    int16_t in[MIX_CHUNK_MAX * 2] __attribute__((aligned(32))), out[MIX_CHUNK_MAX * 2] __attribute__((aligned(32)));
    uint32_t i, peak = *peakp;
    if (!n || n > MIX_CHUNK_MAX)
        return;
    MIX_GQR();
    if (g_rev_ps)
        mix_f2s16(wet, in, n);
    else
        for (i = 0; i < 2 * n; i++)
            in[i] = (int16_t)sat16((int32_t)wet[i]);
    if (extra)
        for (i = 0; i < 2 * n; i += 2) {
            in[i] = (int16_t)sat16(in[i] + ((extra[i] * gain_l) >> 15));
            in[i + 1] = (int16_t)sat16(in[i + 1] + ((extra[i + 1] * gain_r) >> 15));
        }
    rev_run(r, mem, first, n, in, out);
    if (g_rev_ps && evol_l != -32768 && evol_r != -32768) {
        for (i = 0; i < 2 * n; i += 2) {
            const int32_t ol = out[i], orr = out[i + 1];
            const uint32_t top = (uint32_t)(ol < 0 ? -ol : ol) | (uint32_t)(orr < 0 ? -orr : orr);
            if (top > peak)
                peak = top;
            out[i] = (int16_t)((ol * evol_l) >> 15);
            out[i + 1] = (int16_t)((orr * evol_r) >> 15);
        }
        mix_s16_add(out, dry, n);
    } else {
        for (i = 0; i < 2 * n; i += 2) {
            const int32_t ol = out[i], orr = out[i + 1];
            const uint32_t top = (uint32_t)(ol < 0 ? -ol : ol) | (uint32_t)(orr < 0 ? -orr : orr);
            if (top > peak)
                peak = top;
            dry[i] += (float)((ol * evol_l) >> 15);
            dry[i + 1] += (float)((orr * evol_r) >> 15);
        }
    }
    *peakp = peak;
}

/* "rev 2": the chunk's way from a copy of the state and the work area, then the sample's from the start again (its
 * result is the one kept), and the two compared */
static void rev_check(Reverb *r, int16_t *mem, uint64_t first, uint32_t n, const float *wet, const int16_t *extra,
                      int32_t gain_l, int32_t gain_r, float *dry, int32_t evol_l, int32_t evol_r, uint32_t *peakp)
{
    float d[MIX_CHUNK_MAX * 2];
    const Reverb s = *r;
    Reverb a;
    int16_t *area = mem + r->start;
    const uint32_t len = r->len;
    uint32_t peak = *peakp;
    int dry_bad, peak_bad, state_bad, area_bad;
    if (n > MIX_CHUNK_MAX || r->start + len > SPU_RAM_SIZE / 2) {
        g_rev_chk_skipped++;
        rev_add_samples(r, mem, first, n, wet, extra, gain_l, gain_r, dry, evol_l, evol_r, peakp);
        return;
    }
    if (len > g_rev_chk_cap) {
        if (g_rev_chk_area)
            plat_free(g_rev_chk_area);
        g_rev_chk_area = plat_alloc((size_t)len * 2 * sizeof(int16_t));
        g_rev_chk_cap = g_rev_chk_area ? len : 0;
        if (!g_rev_chk_area) {
            g_rev_chk_skipped++;
            rev_add_samples(r, mem, first, n, wet, extra, gain_l, gain_r, dry, evol_l, evol_r, peakp);
            return;
        }
    }
    memcpy(g_rev_chk_area, area, len * sizeof(int16_t));
    memcpy(d, dry, n * 2 * sizeof(float));
    rev_add_chunk(r, mem, first, n, wet, extra, gain_l, gain_r, d, evol_l, evol_r, &peak);
    a = *r;
    memcpy(g_rev_chk_area + len, area, len * sizeof(int16_t));
    memcpy(area, g_rev_chk_area, len * sizeof(int16_t));
    *r = s;
    rev_add_samples(r, mem, first, n, wet, extra, gain_l, gain_r, dry, evol_l, evol_r, peakp);
    g_rev_chk_calls++;
    dry_bad = memcmp(d, dry, n * 2 * sizeof(float)) != 0;
    peak_bad = peak != *peakp;
    state_bad = memcmp(&a, r, sizeof a) != 0;
    area_bad = memcmp(g_rev_chk_area + len, area, len * sizeof(int16_t)) != 0;
    if (dry_bad | peak_bad | state_bad | area_bad) {
        g_rev_chk_bad++;
        if (g_rev_chk_logged < 8) {
            g_rev_chk_logged++;
            LOG("sound: reverb check: the chunk at sample %u (%u samples) differed: dry sums %d, largest sample %d, state %d, "
                "work area %d", (unsigned)first, (unsigned)n, dry_bad, peak_bad, state_bad, area_bad);
        }
    }
}

/* the wet sum through the effect; the result times EVOL goes into the core's dry sum */
static void rev_add(int core, uint64_t first, uint32_t n, const float *wet, const int16_t *extra,
                    int32_t gain_l, int32_t gain_r, float *dry, int32_t evol_l, int32_t evol_r)
{
    Reverb *r = &g_rev[core];
    int16_t *mem = (int16_t *)g_spu_ram;
    uint64_t t0;
    if (!rev_enabled(core))
        return;
    if (!g_rev_logged[core]) {
        g_rev_logged[core] = 1;
        LOG("sound: reverb core %d in the mixer: mode %d, EVOL %04X %04X, ESA/EEA %05X/%05X",
            core, r->mode, (unsigned)(uint16_t)evol_l, (unsigned)(uint16_t)evol_r,
            (unsigned)r->esa, (unsigned)r->eea);
    }
    r->on = 1;
    t0 = plat_ticks();
    if (g_rev_new == 2)
        rev_check(r, mem, first, n, wet, extra, gain_l, gain_r, dry, evol_l, evol_r, &g_rev_peak[core]);
    else if (g_rev_new)
        rev_add_chunk(r, mem, first, n, wet, extra, gain_l, gain_r, dry, evol_l, evol_r, &g_rev_peak[core]);
    else
        rev_add_samples(r, mem, first, n, wet, extra, gain_l, gain_r, dry, evol_l, evol_r, &g_rev_peak[core]);
    g_rev_ticks += plat_ticks() - t0;
}

/* the quantised conversions against the C ones on values at the edges (and some between): the same, or the C ones
 * are used */
static void rev_selftest(void)
{
#if defined(GEKKO)
    static const float v[16] __attribute__((aligned(32))) = {
        0.5f, -0.5f, 1.5f, -1.5f, 0.99999f, -0.99999f, 32766.75f, 32767.5f,
        32768.0f, 40000.25f, -32767.5f, -32768.75f, -40000.5f, 1.0e9f, -1.0e9f, -0.0f
    };
    float a[16] __attribute__((aligned(32))), b[16] __attribute__((aligned(32)));
    int16_t q[16] __attribute__((aligned(32))), t[16] __attribute__((aligned(32)));
    uint32_t i, bad = 0;
    MIX_GQR();
    mix_f2s16(v, q, 8);
    for (i = 0; i < 16; i++) {
        bad += q[i] != (int16_t)sat16((int32_t)v[i]);
        t[i] = (int16_t)(i * 4099u - 32768u);
        a[i] = b[i] = v[i] * 0.37f + (i & 1 ? 16777215.0f : -3.25f);
    }
    mix_s16_add(t, a, 8);
    for (i = 0; i < 16; i++) {
        b[i] += (float)t[i];
        bad += memcmp(&a[i], &b[i], sizeof a[i]) != 0;
    }
    g_rev_ps = bad == 0;
    LOG("sound: the reverb's samples are converted %s",
        bad ? "by C (the quantised stores or loads gave other values)" : "by quantised stores and loads");
#endif
}

static uint32_t rev_eea(const Core *k)
{
    return (k->addr[A_EEA - A_ESA] | 0x1FFFFu) & (SPU_RAM_SIZE - 1);
}

static void rev_clear_work(int mode, uint32_t eea)
{
    uint32_t size, at;
    if (mode <= 0 || mode > 9)
        return;
    size = (uint32_t)k_effect_sizes[mode] << 4;
    at = (eea - (size - 2)) & (SPU_RAM_SIZE - 1);
    if (size > SPU_RAM_SIZE - at)
        size = SPU_RAM_SIZE - at;
    memset(g_spu_ram + at, 0, size);
}

static void rev_set(int core, int mode, uint32_t esa, uint32_t eea, const uint16_t *md, int depth_l, int depth_r)
{
    Reverb *r = &g_rev[core];
    const uint32_t end = (eea & 0x3FFFFFu) | 0xFFFFu;
    memset(r, 0, sizeof *r);
    r->mode = mode;
    r->esa = esa;
    r->eea = eea;
    r->start = esa & 0x3FFFFFu;
    r->len = end > r->start ? end - r->start + 1 : 1;
    r->apf1_size = (uint32_t)md[0] << 2;
    r->apf2_size = (uint32_t)md[1] << 2;
    r->iir_vol = (int16_t)md[2];
    r->comb1_vol = (int16_t)md[3];
    r->comb2_vol = (int16_t)md[4];
    r->comb3_vol = (int16_t)md[5];
    r->comb4_vol = (int16_t)md[6];
    r->wall_vol = (int16_t)md[7];
    r->apf1_vol = (int16_t)md[8];
    r->apf2_vol = (int16_t)md[9];
    r->same_l_dst = (uint32_t)md[10] << 2;
    r->same_r_dst = (uint32_t)md[11] << 2;
    r->comb1_l_src = (uint32_t)md[12] << 2;
    r->comb1_r_src = (uint32_t)md[13] << 2;
    r->comb2_l_src = (uint32_t)md[14] << 2;
    r->comb2_r_src = (uint32_t)md[15] << 2;
    r->same_l_src = (uint32_t)md[16] << 2;
    r->same_r_src = (uint32_t)md[17] << 2;
    r->diff_l_dst = (uint32_t)md[18] << 2;
    r->diff_r_dst = (uint32_t)md[19] << 2;
    r->comb3_l_src = (uint32_t)md[20] << 2;
    r->comb3_r_src = (uint32_t)md[21] << 2;
    r->comb4_l_src = (uint32_t)md[22] << 2;
    r->comb4_r_src = (uint32_t)md[23] << 2;
    r->diff_l_src = (uint32_t)md[24] << 2;
    r->diff_r_src = (uint32_t)md[25] << 2;
    r->apf1_l_dst = (uint32_t)md[26] << 2;
    r->apf1_r_dst = (uint32_t)md[27] << 2;
    r->apf2_l_dst = (uint32_t)md[28] << 2;
    r->apf2_r_dst = (uint32_t)md[29] << 2;
    r->in_coef_l = (int16_t)md[30];
    r->in_coef_r = (int16_t)md[31];
    /* the addresses relative to the delay line, one subtract each at run time */
    r->same_l_prv = (r->same_l_dst + r->len - 1) % r->len;
    r->same_r_prv = (r->same_r_dst + r->len - 1) % r->len;
    r->diff_l_prv = (r->diff_l_dst + r->len - 1) % r->len;
    r->diff_r_prv = (r->diff_r_dst + r->len - 1) % r->len;
    r->apf1_l_src = (r->apf1_l_dst - r->apf1_size) % r->len;
    r->apf1_r_src = (r->apf1_r_dst - r->apf1_size) % r->len;
    r->apf2_l_src = (r->apf2_l_dst - r->apf2_size) % r->len;
    r->apf2_r_src = (r->apf2_r_dst - r->apf2_size) % r->len;
    r->same_l_dst %= r->len;
    r->same_r_dst %= r->len;
    r->comb1_l_src %= r->len;
    r->comb1_r_src %= r->len;
    r->comb2_l_src %= r->len;
    r->comb2_r_src %= r->len;
    r->same_l_src %= r->len;
    r->same_r_src %= r->len;
    r->diff_l_dst %= r->len;
    r->diff_r_dst %= r->len;
    r->comb3_l_src %= r->len;
    r->comb3_r_src %= r->len;
    r->comb4_l_src %= r->len;
    r->comb4_r_src %= r->len;
    r->diff_l_src %= r->len;
    r->diff_r_src %= r->len;
    r->apf1_l_dst %= r->len;
    r->apf1_r_dst %= r->len;
    r->apf2_l_dst %= r->len;
    r->apf2_r_dst %= r->len;
    g_core[core].param[P_EVOLL] = (uint16_t)depth_l;
    g_core[core].param[P_EVOLR] = (uint16_t)depth_r;
    g_rev_peak[core] = 0;
    g_rev_logged[core] = 0;
    LOG("sound: reverb core %d: %s, work area %05X-%05X (samples), depth %04X %04X",
        core, k_effect_name[mode], (unsigned)esa, (unsigned)eea, (unsigned)(uint16_t)depth_l,
        (unsigned)(uint16_t)depth_r);
}

/* the engine's sound: core 0's effect sum through the game's filters (runtime/hle_audio.c) */
int hle_engine_ready(void);
int hle_engine_dsp(int16_t *lr, uint32_t n, int input);
void hle_engine_report(char *line, size_t size);

#if DSP_MIX
/* a run's sums into the chunk's dry sums of that core: (m << 16 | l) in 1/32768 of a sample step */
static void dsp_add_c(float *dry, const DspMixSum *s, uint32_t m)
{
    uint32_t i;
    for (i = 0; i < m; i++) {
        dry[2 * i] += (float)(int32_t)((uint32_t)(uint16_t)s[i].m[0] << 16 | s[i].l[0]) * (1.0f / 32768.0f);
        dry[2 * i + 1] += (float)(int32_t)((uint32_t)(uint16_t)s[i].m[1] << 16 | s[i].l[1]) * (1.0f / 32768.0f);
    }
}

/* The same with quantised loads: (m << 16 | l) / 32768 is 2m + l / 32768, each half exact as a float (GQR 7: m's pair with
 * a sign, times 2; GQR 2: l's pair without, over 32768), and their one rounded add is the float the conversion above
 * gives. Five instructions a sample against two int-to-float conversions (some ten each on Broadway). */
void dsp_add_ps(const DspMixSum *s, float *dry, uint32_t m);
__asm__(
    "\t.pushsection .text\n"
    "\t.align 2\n"
    "\t.globl dsp_add_ps\n"
    "\t.type dsp_add_ps, @function\n"
    "dsp_add_ps:\n"
    "\tmtctr 5\n"
    "1:\n"
    "\tpsq_l 1,0(3),0,7\n"               /* 2 mL, 2 mR */
    "\tpsq_l 2,4(3),0,2\n"               /* lL / 32768, lR / 32768 */
    "\tpsq_l 3,0(4),0,0\n"
    "\tps_add 1,1,2\n"
    "\tps_add 3,3,1\n"
    "\tpsq_st 3,0(4),0,0\n"
    "\taddi 3,3,8\n"
    "\taddi 4,4,8\n"
    "\tbdnz 1b\n"
    "\tblr\n"
    "\t.size dsp_add_ps, .-dsp_add_ps\n"
    "\t.popsection\n");

static int g_dsp_add_ps;                /* the quantised loads gave the conversion's floats in the self-test */

static void dsp_add(float *dry, const DspMixSum *s, uint32_t m)
{
    if (g_dsp_add_ps) {
        __asm__ volatile("mtspr 919,%0\n\tmtspr 914,%1" : : "r"(0x3F070000u), "r"(0x0F050000u));
        dsp_add_ps(s, dry, m);
    } else {
        dsp_add_c(dry, s, m);
    }
}

/* the two ways on sums at the edges (and some between) from the same start: the same bits, or the C one is used */
static void dsp_add_selftest(void)
{
    static const int16_t ms[8] = {0, 1, -1, 0x7FFF, -0x8000, 0x1234, -0x2345, 0x4000};
    static const uint16_t ls[8] = {0, 1, 0xFFFF, 0x8000, 0x7FFF, 0x0001, 0xABCD, 0x3333};
    DspMixSum s[64] __attribute__((aligned(32)));
    float a[128] __attribute__((aligned(32))), b[128] __attribute__((aligned(32)));
    uint32_t i, bad = 0;
    for (i = 0; i < 64; i++) {
        s[i].m[0] = ms[i & 7];
        s[i].m[1] = ms[(i >> 3) & 7];
        s[i].l[0] = ls[(i >> 3) & 7];
        s[i].l[1] = ls[i & 7];
        a[2 * i] = b[2 * i] = (float)(int32_t)(i * 77u) - 1000.25f;
        a[2 * i + 1] = b[2 * i + 1] = 0.0f;
    }
    dsp_add_c(a, s, 64);
    g_dsp_add_ps = 1;
    dsp_add(b, s, 64);
    for (i = 0; i < 128; i++)
        bad += memcmp(&a[i], &b[i], sizeof a[i]) != 0;
    g_dsp_add_ps = bad == 0;
    LOG("sound: the DSP's sums are added %s", bad ? "by conversions (the quantised loads gave other floats)" : "by quantised loads");
}

/* A run the DSP did not do (it was given up on): its jobs done here, in order, from their records. (One it gave up in
 * the middle of may have written some records already: those voices are a block off for a moment.) */
static void dsp_jobs_here(MixLater *L)
{
    uint32_t j;
    for (j = 0; j < L->run.jobs; j++) {
        const DspMixJob *jb = &L->run.cmd->job[j];
        const DspMixRun *rl = (const DspMixRun *)(((uint32_t)jb->runs_hi << 16 | jb->runs_lo) | 0x80000000u);
        const int32_t dl = (int32_t)((uint32_t)jb->dl_hi << 16 | jb->dl_lo), dr = (int32_t)((uint32_t)jb->dr_hi << 16 | jb->dr_lo);
        float G[4];
        G[0] = jb->coref & DSPMIX_LEFT ? (float)jb->gl * (1.0f / 32768.0f) : 0.0f;
        G[1] = jb->coref & DSPMIX_RIGHT ? (float)jb->gr * (1.0f / 32768.0f) : 0.0f;
        G[2] = jb->coref & DSPMIX_LEFT ? (float)dl * (1.0f / 2147483648.0f) : 0.0f;
        G[3] = jb->coref & DSPMIX_RIGHT ? (float)dr * (1.0f / 2147483648.0f) : 0.0f;
        job_cpu(&g_rec[jb->voice], rl, jb->nruns, (uint32_t)jb->idx << 12 | (uint32_t)jb->frac >> 4,
                (uint32_t)jb->pint << 12 | (uint32_t)jb->pfrac >> 4, jb->cnt, L->dry[jb->coref & 1], G);
    }
}

/* The chunks waiting for their runs, oldest first, mixed out once their sums are in: those in already (how 0), and
 * waiting for the oldest when there is no room for another (how 1), until none is left (how 2), or until the run
 * `seq` is out (how 3: a voice's record is wanted). */
static void mix_retire_to(int how, uint32_t seq)
{
    while (g_later_n) {
        MixLater *L = &g_later[g_later_head];
        if (how == 3 && g_retired_seq >= seq)
            return;
        if (L->run.jobs) {
            int slot = -1;
            if (L->sent && dspmix_up()) {
                slot = dspmix_poll();
                if (slot < 0 && dspmix_up()) {
                    uint64_t t0, dt;
                    if (how == 0 || (how == 1 && g_later_n < DSPMIX_SLOTS))
                        return;                 /* not in yet: it and those after it wait */
                    t0 = plat_ticks();
                    slot = dspmix_wait(20000);
                    dt = plat_ticks() - t0;
                    g_dsp_waits++;
                    g_dsp_wait_sum += dt;
                    if (dt > g_dsp_wait_max)
                        g_dsp_wait_max = dt;
                }
            }
            if (slot == (int)g_later_head) {
                const DspMixSum *s0 = dspmix_sums(slot, 0), *s1 = dspmix_sums(slot, 1);
                if (s0)
                    dsp_add(L->dry[0], s0, L->m);
                if (s1)
                    dsp_add(L->dry[1], s1, L->m);
            } else {
                g_dsp_lost++;
                dsp_jobs_here(L);
            }
        }
        mix_out(L->dry[0], L->dry[1], L->eng, g_mix_out, L->m, L->K);
        g_spu_dropped += L->m - plat_audio_write(g_mix_out, L->m);
        g_retired_seq = L->run.seq;
        g_later_head = (g_later_head + 1) % DSPMIX_SLOTS;
        g_later_n--;
    }
}

static void mix_retire(int how)
{
    mix_retire_to(how, 0);
}

static void mix_retire_until(uint32_t seq)
{
    mix_retire_to(3, seq);
}
#endif

/* n samples from the chip's clock `first` on */
static void spu_mix(uint64_t first, uint32_t n)
{
    static float acc[2][2][MIX_CHUNK * 2];      /* a core's dry and wet sums */
    static float both[MIX_CHUNK * 2];
    static int16_t out[MIX_CHUNK * 2], eng[MIX_CHUNK * 2], rev_in[2][MIX_CHUNK * 2];
    static int ready;
    const uint64_t t0 = plat_ticks();
    const int level = plat_audio_level(), on = level >= 0 && !g_spu_mute;
    /* with the engine's sound made here the driver's loop through the rings carries nothing */
    const int native = hle_engine_ready();
    int core, v;
    if (!ready) {
        mix_tables();
        ready = 1;
    }
    MIX_GQR();
#if DSP_MIX
    if (g_dsp_on == 1 && !dspmix_up())
        dspmix_start();         /* (once: a microcode that did not come up is not tried again) */
#endif
    while (n) {
        const uint32_t m = n < MIX_CHUNK ? n : MIX_CHUNK;
        uint32_t i;
        int wet_heard = 0, eng_on = 0;
        const int rev0 = on && rev_enabled(0), rev1 = on && rev_enabled(1);
        /* the dry sums: the chunk's own when it waits for a run */
        float *drys[2] = {acc[0][0], acc[1][0]};
        DspRun *run = NULL;
#if DSP_MIX
        MixLater *later = NULL;
        static DspRun here;
        const int mode = on && native && g_dsp_on && dsp_mem() ? g_dsp_on : 0;
        if (mode == 1 && dspmix_up()) {
            uint32_t slot;
            mix_retire(1);
            slot = (g_later_head + g_later_n) % DSPMIX_SLOTS;
            later = &g_later[slot];
            run = &later->run;
            run->cmd = dspmix_cmd((int)slot);
            run->pool = g_dsp_pool[slot];
            run->jobs = run->used = 0;
            run->here = run->check = 0;
            run->seq = ++g_dsp_seq;
            drys[0] = later->dry[0];
            drys[1] = later->dry[1];
        } else {
            if (g_later_n)
                mix_retire(2);
            if (mode == 2 || mode == 3) {
                here.pool = g_dsp_pool[0];
                here.jobs = here.used = 0;
                here.seq = 0;
                here.here = mode == 2;
                here.check = mode == 3;
                run = &here;
            }
        }
#endif
        if (on) {
            memset(drys[0], 0, m * 8);
            memset(drys[1], 0, m * 8);
            if (native || rev0 || (g_ring_read >> (RING_WET(0, 0) / 0x400u) & 3))
                memset(acc[0][1], 0, m * 8);
            if (rev1 || (!native && (g_ring_read >> (RING_WET(1, 0) / 0x400u) & 3)))
                memset(acc[1][1], 0, m * 8);
        }
        for (core = 0; core < 2; core++) {
            const Core *k = &g_core[core];
            /* a core nobody has set the mix switches of mixes every voice */
            const int unset = !(k->sw[S_VMIXL] | k->sw[S_VMIXR] | k->sw[S_VMIXEL] | k->sw[S_VMIXER]);
            const uint32_t ml = unset ? ~0u : k->sw[S_VMIXL], mr = unset ? ~0u : k->sw[S_VMIXR];
            const uint32_t el = k->sw[S_VMIXEL], er = k->sw[S_VMIXER];
            /* a wet sum is for the engine's filters, the reverb, or its ring: nobody reads it, nobody needs it */
            const int wet_on = (core ? rev1 : rev0) ||
                               (native ? !core : (g_ring_read >> (RING_WET(core, 0) / 0x400u) & 3) != 0);
            for (v = 0; v < VOICES; v++) {
                const int dry = (int)((ml | mr) >> v & 1), wet = wet_on && ((el | er) >> v & 1);
                Voice *const xv = &g_core[core].v[v];
                const int steady = xv->param[VP_PITCH] == xv->pitch_seen;
                const uint32_t h0 = g_spu_heard;
                if (!xv->playing)
                    continue;
                xv->pitch_seen = xv->param[VP_PITCH];
                g_spu_voices += m;
                if (!on || !(dry | wet)) {
                    voice_run(core, v, m, NULL, 0, 0, NULL);
                } else if (dry && wet) {
                    const uint32_t heard = g_spu_heard;
                    memset(both, 0, m * 8);
                    voice_run(core, v, m, both, 1, 1, NULL);
                    if (heard != g_spu_heard)
                        wet_heard |= !core;
                    if (heard != g_spu_heard)
                        for (i = 0; i < m; i++) {
                            const float l = both[2 * i], r = both[2 * i + 1];
                            if (ml >> v & 1)
                                drys[core][2 * i] += l;
                            if (mr >> v & 1)
                                drys[core][2 * i + 1] += r;
                            if (el >> v & 1)
                                acc[core][1][2 * i] += l;
                            if (er >> v & 1)
                                acc[core][1][2 * i + 1] += r;
                        }
                } else if (dry) {
#if DSP_MIX
                    if (run && run->check)
                        voice_check(core, v, m, drys[core], ml >> v & 1, mr >> v & 1, run);
                    else
#endif
                        voice_run(core, v, m, drys[core], ml >> v & 1, mr >> v & 1, run);
                } else {
                    const uint32_t heard = g_spu_heard;
                    voice_run(core, v, m, acc[core][1], el >> v & 1, er >> v & 1, NULL);
                    if (heard != g_spu_heard)
                        wet_heard |= !core;
                }
                if (g_spu_heard != h0) {
                    const uint32_t d = g_spu_heard - h0;
                    g_k_heard[(dry && wet) || (!dry && wet) ? 2 : steady ? 0 : 1] += d;
                    g_k_core[core] += d;
                    g_k_ck[core][(dry && wet) || (!dry && wet) ? 2 : steady ? 0 : 1] += d;
                }
            }
        }
#if DSP_MIX
        /* the run goes now: the DSP stretches while the reverb and the engine are made here */
        if (later) {
            later->sent = 0;
            if (run->jobs) {
                DCFlushRange(run->pool, run->used * sizeof(int16_t));
                later->sent = dspmix_run((int)(later - g_later), run->jobs, m, g_rec);
            }
        }
#endif
        if (on) {
            /* A core: its voices' dry sum and its sound data input times BVOL, as MMIX says, stopped at
             * 16 bits, times its volume. Core 0's output goes into core 1's sum by that core's input
             * volume; core 1's is what is heard. */
            const uint32_t mm0 = g_core[0].param[P_MMIX], mm1 = g_core[1].param[P_MMIX];
            const int32_t v0l = vol_of(g_core[0].param[P_MVOLL]), v0r = vol_of(g_core[0].param[P_MVOLR]);
            const int32_t v1l = vol_of(g_core[1].param[P_MVOLL]), v1r = vol_of(g_core[1].param[P_MVOLR]);
            const int32_t d0l = (mm0 & MMIX_MSNDL) != 0, d0r = (mm0 & MMIX_MSNDR) != 0;
            const int32_t d1l = (mm1 & MMIX_MSNDL) != 0, d1r = (mm1 & MMIX_MSNDR) != 0;
            const int32_t b0l = mm0 & MMIX_MINL ? (int16_t)g_core[0].param[P_BVOLL] : 0;
            const int32_t b0r = mm0 & MMIX_MINR ? (int16_t)g_core[0].param[P_BVOLR] : 0;
            const int32_t b1l = mm1 & MMIX_MINL ? (int16_t)g_core[1].param[P_BVOLL] : 0;
            const int32_t b1r = mm1 & MMIX_MINR ? (int16_t)g_core[1].param[P_BVOLR] : 0;
            const int32_t a1l = mm1 & MMIX_SINL ? (int16_t)g_core[1].param[P_AVOLL] : 0;
            const int32_t a1r = mm1 & MMIX_SINR ? (int16_t)g_core[1].param[P_AVOLR] : 0;
            /* the sound data input into the effect side (MMIX bits 0x10/0x20), before its dry gate */
            const int32_t w0l = mm0 & 0x020u ? (int16_t)g_core[0].param[P_BVOLL] : 0;
            const int32_t w0r = mm0 & 0x010u ? (int16_t)g_core[0].param[P_BVOLR] : 0;
            const int32_t w1l = mm1 & 0x020u ? (int16_t)g_core[1].param[P_BVOLL] : 0;
            const int32_t w1r = mm1 & 0x010u ? (int16_t)g_core[1].param[P_BVOLR] : 0;
            const int32_t ev0l = (int16_t)g_core[0].param[P_EVOLL], ev0r = (int16_t)g_core[0].param[P_EVOLR];
            const int32_t ev1l = (int16_t)g_core[1].param[P_EVOLL], ev1r = (int16_t)g_core[1].param[P_EVOLR];
            const uint32_t caps = native ? 0 : g_ring_read;
            if (native) {
                /* The engine's sound is made here, nothing goes through the rings: core 0 by its
                 * volume and core 1's input volume in one factor, core 1's own sum, the engine's
                 * samples by the sound data input's volume (they stand at zero while it is silent). */
                static int eng_clear;
                const float u = 1.0f / 32768.0f;
                float K[12];
                K[0] = d0l ? (float)v0l * (float)a1l * (u * u) : 0.0f;
                K[1] = d0r ? (float)v0r * (float)a1r * (u * u) : 0.0f;
                K[2] = (float)d1l;
                K[3] = (float)d1r;
                K[4] = (float)b1l * u;
                K[5] = (float)b1r * u;
                K[6] = (float)v1l * u;
                K[7] = (float)v1r * u;
                K[8] = K[9] = 32767.0f;
                K[10] = K[11] = -32768.0f;
                if (b1l | b1r) {
                    if (wet_heard)
                        mix_f2s16(acc[0][1], eng, m);
                    eng_on = hle_engine_dsp(eng, m, wet_heard);
                }
                if (eng_on) {
                    eng_clear = 0;
                } else if (!eng_clear) {
                    memset(eng, 0, sizeof eng);
                    eng_clear = 1;
                }
                if (PS2_UNLIKELY(g_spu_dump)) {
                    /* (the status line's count of samples that did not fit) */
                    for (i = 0; i < 2 * m; i++) {
                        const float c0 = drys[0][i];
                        const float w = (c0 > 32767.0f ? 32767.0f : c0 < -32768.0f ? -32768.0f : c0) * K[i & 1] +
                                        drys[1][i] * K[2 + (i & 1)] + (float)eng[i] * K[4 + (i & 1)];
                        if (w > 32767.0f || w < -32768.0f)
                            g_spu_clipped++;
                    }
                }
                if (rev0)
                    rev_add(0, first, m, acc[0][1], NULL, 0, 0, drys[0], ev0l, ev0r);
                if (rev1)
                    rev_add(1, first, m, acc[1][1], (w1l | w1r) ? eng : NULL, w1l, w1r, drys[1], ev1l, ev1r);
#if DSP_MIX
                if (later) {
                    /* mixed out when the run's sums are in */
                    memcpy(later->K, K, sizeof K);
                    memcpy(later->eng, eng, m * 2 * sizeof(int16_t));
                    later->m = m;
                    g_later_n++;
                } else
#endif
                    mix_out(drys[0], drys[1], eng, out, m, K);
            } else {
                uint32_t peak = g_spu_peak;
                if (rev0 && (w0l | w0r))
                    for (i = 0; i < m; i++) {
                        const uint32_t p = (uint32_t)((first + i) & 511u) * 2;
                        rev_in[0][2 * i] = (int16_t)ring_get(RING_IN(0, 0) + p);
                        rev_in[0][2 * i + 1] = (int16_t)ring_get(RING_IN(0, 1) + p);
                    }
                if (rev1 && (w1l | w1r))
                    for (i = 0; i < m; i++) {
                        const uint32_t p = (uint32_t)((first + i) & 511u) * 2;
                        rev_in[1][2 * i] = (int16_t)ring_get(RING_IN(1, 0) + p);
                        rev_in[1][2 * i + 1] = (int16_t)ring_get(RING_IN(1, 1) + p);
                    }
                if (rev0)
                    rev_add(0, first, m, acc[0][1], (w0l | w0r) ? rev_in[0] : NULL, w0l, w0r,
                            acc[0][0], ev0l, ev0r);
                if (rev1)
                    rev_add(1, first, m, acc[1][1], (w1l | w1r) ? rev_in[1] : NULL, w1l, w1r,
                            acc[1][0], ev1l, ev1r);
                for (i = 0; i < m; i++) {
                    const uint32_t p = (uint32_t)((first + i) & 511u) * 2;
                    int32_t l = sat16((int32_t)acc[0][0][2 * i]) * d0l, r = sat16((int32_t)acc[0][0][2 * i + 1]) * d0r, sl, sr;
                    uint32_t top;
                    if (PS2_UNLIKELY(caps)) {
                        for (core = 0; core < 2; core++) {
                            if (caps >> (RING_DRY(core, 0) / 0x400u) & 3) {
                                ring_put(RING_DRY(core, 0) + p, sat16((int32_t)acc[core][0][2 * i]));
                                ring_put(RING_DRY(core, 1) + p, sat16((int32_t)acc[core][0][2 * i + 1]));
                            }
                            if (caps >> (RING_WET(core, 0) / 0x400u) & 3) {
                                ring_put(RING_WET(core, 0) + p, sat16((int32_t)acc[core][1][2 * i]));
                                ring_put(RING_WET(core, 1) + p, sat16((int32_t)acc[core][1][2 * i + 1]));
                            }
                        }
                    }
                    if (b0l | b0r) {
                        l += (ring_get(RING_IN(0, 0) + p) * b0l) >> 15;
                        r += (ring_get(RING_IN(0, 1) + p) * b0r) >> 15;
                    }
                    sl = ((sat16(l) * v0l) >> 15) * a1l >> 15;
                    sr = ((sat16(r) * v0r) >> 15) * a1r >> 15;
                    sl += (int32_t)acc[1][0][2 * i] * d1l;
                    sr += (int32_t)acc[1][0][2 * i + 1] * d1r;
                    if (b1l | b1r) {
                        sl += (ring_get(RING_IN(1, 0) + p) * b1l) >> 15;
                        sr += (ring_get(RING_IN(1, 1) + p) * b1r) >> 15;
                    }
                    top = (uint32_t)(sl < 0 ? -sl : sl) | (uint32_t)(sr < 0 ? -sr : sr);
                    if (PS2_UNLIKELY(top > peak)) {
                        if (top > 32767)
                            g_spu_clipped++;
                        else
                            peak = top;         /* (the two together, at least the larger: near enough) */
                    }
                    out[2 * i] = (int16_t)((sat16(sl) * v1l) >> 15);
                    out[2 * i + 1] = (int16_t)((sat16(sr) * v1r) >> 15);
                }
                if (peak > g_spu_peak)
                    g_spu_peak = peak;
            }
#if DSP_MIX
            if (later)
                mix_retire(0);          /* (a chunk without a job for the DSP goes at once when nothing is ahead of it) */
            else
#endif
                g_spu_dropped += m - plat_audio_write(out, m);
        }
        g_spu_frames += m;
        first += m;
        n -= m;
    }
    g_spu_ticks += plat_ticks() - t0;
}

/* status line (once every ten seconds) */
void iop_sd_report(char *line, size_t size)
{
    const uint32_t f = g_spu_frames ? g_spu_frames : 1;
    const uint32_t share = (uint32_t)(g_spu_ticks * 480000000ull / plat_tick_hz / f);     /* 0.01% */
    snprintf(line, size, "%u.%u voices playing, %u.%u of them heard (%u came back from unheard), peak %u of 32767, %u "
             "samples clipped, %u not taken by the output (it holds %d now); mixing took %u.%02u%% of the time; core "
             "volumes %04X %04X / %04X %04X, mix switches %06X %06X / %06X %06X, MMIX %04X %04X, core 1 input volume "
             "%04X %04X",
             (unsigned)(g_spu_voices / f), (unsigned)(g_spu_voices * 10 / f % 10), (unsigned)(g_spu_heard / f),
             (unsigned)(g_spu_heard * 10 / f % 10), (unsigned)g_spu_warm, (unsigned)g_spu_peak,
             (unsigned)g_spu_clipped, (unsigned)g_spu_dropped, plat_audio_level(),
             (unsigned)(share / 100), (unsigned)(share % 100),
             g_core[0].param[P_MVOLL], g_core[0].param[P_MVOLR], g_core[1].param[P_MVOLL], g_core[1].param[P_MVOLR],
             (unsigned)g_core[0].sw[S_VMIXL], (unsigned)g_core[0].sw[S_VMIXR], (unsigned)g_core[1].sw[S_VMIXL],
             (unsigned)g_core[1].sw[S_VMIXR], g_core[0].param[P_MMIX], g_core[1].param[P_MMIX],
             g_core[1].param[P_AVOLL], g_core[1].param[P_AVOLR]);
    LOG("sound: voices heard on average: dry only with a steady pitch %u.%u, dry only with a pitch that moved %u.%u, with a send to the "
        "effect unit %u.%u (core 0 %u.%u, core 1 %u.%u); ADPCM blocks decoded a second %u",
        (unsigned)(g_k_heard[0] / f), (unsigned)(g_k_heard[0] * 10 / f % 10), (unsigned)(g_k_heard[1] / f),
        (unsigned)(g_k_heard[1] * 10 / f % 10), (unsigned)(g_k_heard[2] / f), (unsigned)(g_k_heard[2] * 10 / f % 10),
        (unsigned)(g_k_core[0] / f), (unsigned)(g_k_core[0] * 10 / f % 10), (unsigned)(g_k_core[1] / f),
        (unsigned)(g_k_core[1] * 10 / f % 10), (unsigned)((uint64_t)g_k_blocks * 48000u / f));
    LOG("sound: voices heard on average by core: core 0 dry steady %u.%u, dry moved %u.%u, with a send %u.%u; core 1 dry steady %u.%u, "
        "dry moved %u.%u, with a send %u.%u",
        (unsigned)(g_k_ck[0][0] / f), (unsigned)(g_k_ck[0][0] * 10 / f % 10), (unsigned)(g_k_ck[0][1] / f),
        (unsigned)(g_k_ck[0][1] * 10 / f % 10), (unsigned)(g_k_ck[0][2] / f), (unsigned)(g_k_ck[0][2] * 10 / f % 10),
        (unsigned)(g_k_ck[1][0] / f), (unsigned)(g_k_ck[1][0] * 10 / f % 10), (unsigned)(g_k_ck[1][1] / f),
        (unsigned)(g_k_ck[1][1] * 10 / f % 10), (unsigned)(g_k_ck[1][2] / f), (unsigned)(g_k_ck[1][2] * 10 / f % 10));
#if DSP_MIX
    if (g_dsp_on || g_dsp_jobs || g_dsp_lost) {
        char d[300];
        dspmix_report(d, sizeof d);
        LOG("sound: dsp %d: %u.%u voices a sample by the DSP (%u jobs, %u blocks decoded there), %u voices it had no room for, %u "
            "chunks that waited for their sums (%u us in all, the longest %u us), %u runs done here after all, %u voices taken back "
            "(%u waited for their run); %s",
            g_dsp_on, (unsigned)(g_dsp_samples / f), (unsigned)(g_dsp_samples * 10 / f % 10), (unsigned)g_dsp_jobs, (unsigned)g_dsp_blocks,
            (unsigned)g_dsp_full, (unsigned)g_dsp_waits, (unsigned)(g_dsp_wait_sum * 1000000u / plat_tick_hz),
            (unsigned)(g_dsp_wait_max * 1000000u / plat_tick_hz), (unsigned)g_dsp_lost,
            (unsigned)g_dsp_back, (unsigned)g_dsp_back_waits, d);
        if (g_chk_voices)
            LOG("sound: dsp check: %u voices both ways (%u as jobs), %u whose state differed after them, %u whose sums did (at "
                "most %d.%04d samples apart)", (unsigned)g_chk_voices, (unsigned)g_chk_jobs, (unsigned)g_chk_state, (unsigned)g_chk_sums,
                (int)g_chk_maxd, (int)((g_chk_maxd - (float)(int)g_chk_maxd) * 10000.0f));
        g_dsp_samples = g_dsp_jobs = g_dsp_blocks = g_dsp_full = g_dsp_waits = g_dsp_lost = g_dsp_back = g_dsp_back_waits = 0;
        g_chk_voices = g_chk_jobs = g_chk_state = g_chk_sums = 0;
        g_chk_maxd = 0.0f;
        g_dsp_wait_max = g_dsp_wait_sum = 0;
    }
#endif
    memset(g_k_ck, 0, sizeof g_k_ck);
    g_k_heard[0] = g_k_heard[1] = g_k_heard[2] = g_k_core[0] = g_k_core[1] = g_k_blocks = 0;
    g_spu_peak = g_spu_frames = g_spu_voices = g_spu_heard = g_spu_clipped = g_spu_dropped = g_spu_warm = 0;
    g_spu_ticks = 0;
    if (g_rev[0].mode || g_rev[1].mode) {
        const uint32_t rs = (uint32_t)(g_rev_ticks * 480000000ull / plat_tick_hz / f);     /* 0.01% */
        LOG("sound: reverb core 0 %s%s, core 1 %s%s; EVOL %04X %04X, the effect's largest sample %u %u; it took %u.%02u%% "
            "of the time (rev %d)",
            k_effect_name[g_rev[0].mode], (g_core[0].attr[1] & 1) ? " on" : " off",
            k_effect_name[g_rev[1].mode], (g_core[1].attr[1] & 1) ? " on" : " off",
            g_core[1].param[P_EVOLL], g_core[1].param[P_EVOLR],
            (unsigned)g_rev_peak[0], (unsigned)g_rev_peak[1], (unsigned)(rs / 100), (unsigned)(rs % 100), g_rev_new);
        if (g_rev_chk_calls || g_rev_chk_skipped)
            LOG("sound: reverb check: %u chunks both ways, %u that differed, %u not checked", (unsigned)g_rev_chk_calls,
                (unsigned)g_rev_chk_bad, (unsigned)g_rev_chk_skipped);
    }
    g_rev_peak[0] = g_rev_peak[1] = 0;
    g_rev_ticks = 0;
    g_rev_chk_calls = g_rev_chk_bad = g_rev_chk_skipped = 0;
    {
        char e[300];
        hle_engine_report(e, sizeof e);
        LOG("sound: %s", e);
    }
    if (g_spu_dump) {
        LOG("sound: the tick ran %u times (mean %u us apart, longest %u us); the driver's interrupt was called %u times "
            "(core 0 alone %u, core 1 alone %u, both %u)",
            (unsigned)g_tk_n, (unsigned)(g_tk_n ? g_tk_sum_us / g_tk_n : 0), (unsigned)g_tk_max_us,
            (unsigned)(g_irq_calls[1] + g_irq_calls[2] + g_irq_calls[3]), (unsigned)g_irq_calls[1],
            (unsigned)g_irq_calls[2], (unsigned)g_irq_calls[3]);
        g_tk_n = g_tk_max_us = 0;
        g_tk_sum_us = 0;
        memset(g_irq_calls, 0, sizeof g_irq_calls);
        LOG("sound: effect switches %06X %06X / %06X %06X, effect volumes %04X %04X / %04X %04X, IRQ addresses %05X %05X "
            "(enabled %u %u), effect areas %05X-%05X %05X-%05X, core attributes %X %X %X %X / %X %X %X %X; transfers: %u "
            "writes (%u KB, %05X..%05X), %u reads (%u KB, %05X..%05X); effect attribute calls %u (last mode %X)",
            (unsigned)g_core[0].sw[S_VMIXEL], (unsigned)g_core[0].sw[S_VMIXER], (unsigned)g_core[1].sw[S_VMIXEL],
            (unsigned)g_core[1].sw[S_VMIXER], g_core[0].param[P_EVOLL], g_core[0].param[P_EVOLR], g_core[1].param[P_EVOLL],
            g_core[1].param[P_EVOLR], (unsigned)g_core[0].addr[A_IRQA - A_ESA], (unsigned)g_core[1].addr[A_IRQA - A_ESA],
            (unsigned)g_core[0].attr[CORE_IRQ_ENABLE >> 1], (unsigned)g_core[1].attr[CORE_IRQ_ENABLE >> 1],
            (unsigned)g_core[0].addr[0], (unsigned)g_core[0].addr[1], (unsigned)g_core[1].addr[0], (unsigned)g_core[1].addr[1],
            (unsigned)g_core[0].attr[0], (unsigned)g_core[0].attr[1], (unsigned)g_core[0].attr[2], (unsigned)g_core[0].attr[3],
            (unsigned)g_core[1].attr[0], (unsigned)g_core[1].attr[1], (unsigned)g_core[1].attr[2], (unsigned)g_core[1].attr[3],
            (unsigned)g_tr_stat[0][0], (unsigned)(g_tr_stat[0][1] >> 10), (unsigned)g_tr_stat[0][2], (unsigned)g_tr_stat[0][3],
            (unsigned)g_tr_stat[1][0], (unsigned)(g_tr_stat[1][1] >> 10), (unsigned)g_tr_stat[1][2], (unsigned)g_tr_stat[1][3],
            (unsigned)g_fx_calls, (unsigned)g_fx_mode);
        memset(g_tr_stat, 0, sizeof g_tr_stat);
        g_tr_stat[0][2] = g_tr_stat[1][2] = 0xFFFFFFFFu;
        LOG("sound: input volumes %04X %04X / %04X %04X, rings read %04X", g_core[0].param[P_BVOLL], g_core[0].param[P_BVOLR],
            g_core[1].param[P_BVOLL], g_core[1].param[P_BVOLR], (unsigned)g_ring_read);
        if (g_tr_log_n) {
            char t[420];
            size_t m = 0;
            uint32_t k;
            for (k = 0; k < g_tr_log_n; k++) {
                m += (size_t)snprintf(t + m, sizeof t - m, " +%u@%u:%c%04X+%X", (unsigned)(g_tr_log[k][0] - g_tr_log[0][0]),
                                      (unsigned)g_tr_log[k][1], g_tr_log[k][2] >> 31 ? 'r' : 'w',
                                      (unsigned)(g_tr_log[k][2] & 0xFFFFFF), (unsigned)g_tr_log[k][3]);
                if (m > 380 || k + 1 == g_tr_log_n) {
                    LOG("sound: ring transfers (us from the first @ the chip's place: read or write, address + size):%s", t);
                    m = 0;
                }
            }
            g_tr_log_n = 0;
        }
        if (g_ev_all || g_tr_under) {
            char t[420];
            size_t m = 0;
            uint32_t k;
            LOG("sound: %u voice events (the first %u follow), %u writes over a block being played", (unsigned)g_ev_all,
                (unsigned)g_ev_n, (unsigned)g_tr_under);
            for (k = 0; k < g_ev_n; k++) {
                m += (size_t)snprintf(t + m, sizeof t - m, " %u:%c%u.%u@%05X/%X", (unsigned)(g_ev_log[k][0] % 100000u),
                                      (int)(g_ev_log[k][1] >> 16), (unsigned)(g_ev_log[k][1] >> 8 & 0xFF),
                                      (unsigned)(g_ev_log[k][1] & 0xFF), (unsigned)g_ev_log[k][2], (unsigned)g_ev_log[k][3]);
                if (m > 380 || k + 1 == g_ev_n) {
                    LOG("sound: events (ms: kind core.voice @ place / pitch or size):%s", t);
                    m = 0;
                }
            }
            g_ev_n = g_ev_all = g_tr_under = 0;
        }
        /* debugging ("spudump 1"): every playing voice: pitch, volumes, envelope and phase, ADSR, its block and
         * loop addresses, the block's header, and the largest sample in the block */
        int core, v, k;
        for (core = 0; core < 2; core++) {
            char t[400];
            size_t m = 0;
            t[0] = 0;
            for (v = 0; v < VOICES; v++) {
                const Voice *x = &g_core[core].v[v];
                const uint8_t *b = g_spu_ram + (x->nax & (SPU_RAM_SIZE - 1) & ~15u);
                int top = 0;
                if (!x->playing)
                    continue;
                for (k = 0; k < 29; k++)
                    if ((x->smp[k] < 0 ? -x->smp[k] : x->smp[k]) > top)
                        top = x->smp[k] < 0 ? -x->smp[k] : x->smp[k];
                if (m + 96 < sizeof t)
                    m += (size_t)snprintf(t + m, sizeof t - m, " %d:p%X v%X/%X=%X/%X e%X.%u a%04X/%04X @%05X l%05X h%02X%02X s%d%s",
                                          v, x->param[VP_PITCH], x->param[VP_VOLL], x->param[VP_VOLR], (unsigned)x->vol[0],
                                          (unsigned)x->vol[1], (unsigned)x->env, x->phase, x->param[VP_ADSR1],
                                          x->param[VP_ADSR2], (unsigned)x->nax, (unsigned)x->lsax, b[0], b[1], top,
                                          x->cold ? "c" : "");
                if (m > 300) {
                    LOG("sound: core %d voices:%s", core, t);
                    m = 0;
                    t[0] = 0;
                }
            }
            if (m)
                LOG("sound: core %d voices:%s", core, t);
        }
    }
}

/* The SPU2 writes 16 capture rings at the bottom of sound RAM (bytes
 * 0x0000-0x3FFF: ADMA input, voice 1/3 output, dry and wet mix of each core),
 * 512 samples each, one sample per 48 kHz tick. A write at the IRQ address
 * raises the core's interrupt, which sound drivers use as a steady timer. */
static void capture_irqs(uint64_t from, uint64_t to)
{
    int core;
    for (core = 0; core < 2; core++) {
        Core *k = &g_core[core];
        uint32_t irqa = k->addr[A_IRQA - A_ESA];
        uint64_t s, first;
        if (!(k->attr[CORE_IRQ_ENABLE >> 1] & 1) || irqa >= 0x4000)
            continue;
        s = (irqa & 0x3FF) >> 1;                   /* sample index inside its ring */
        first = from - from % 512 + s;
        if (first < from)
            first += 512;
        if (first < to)
            g_irq_pending[core] = 1;
    }
}

static void spu_tick(uint64_t now)
{
    static uint32_t rem;        /* thousandths of a sample the last tick left */
    uint64_t dt;
    uint32_t q, samples;
    int level;
    if (!g_last_us) {
        g_last_us = now;
        return;
    }
    dt = now - g_last_us;
    if (dt < 5000)
        return;                 /* (every voice's envelope and slides are moved on a tick, heard or not) */
    if (dt > 100000)
        dt = 100000;                 /* after a stall, do not race ahead */
    g_tk_n++;
    g_tk_sum_us += dt;
    if (dt > g_tk_max_us)
        g_tk_max_us = (uint32_t)dt;
    g_last_us = now;
    q = (uint32_t)dt * 48u + rem;
    samples = q / 1000u;
    rem = q % 1000u;
    /* a little more or less than the clock's, towards AUDIO_AHEAD with the output */
    level = plat_audio_level();
    if (level >= 0 && !g_spu_mute) {
        /* (the output takes a block of 512 at a time: within one of the mark nothing is changed) */
        const int off = AUDIO_AHEAD - (level + (int)samples);
        const int want = off > 512 ? (off - 512) / 8 + 1 : off < -512 ? (off + 512) / 8 - 1 : 0;
        const int most = (int)samples / 8 + 1;
        samples = (uint32_t)((int)samples + (want > most ? most : want < -most ? -most : want));
    }
    capture_irqs(g_sample_clock, g_sample_clock + samples);
    spu_mix(g_sample_clock, samples);
    g_sample_clock += samples;
}

/* When the next interrupt of the chip is due, in plat_time_us's terms (UINT64_MAX when none is armed, 0
 * when one waits to be handed over). The kernel's idle wait sleeps until the earliest event it knows;
 * without this one it slept on to the next vblank, so the driver's interrupt (its clock: it moves the
 * IRQ address on by 256 samples each time, 187.5 a second) came at 87 a second, and what it runs
 * every second one (the volume slides of the music, whose rates it picks for 512 samples) ran twice
 * too long: the music's volume swung between nothing and full about thirty times a second. */
int g_spu_irqdl = 1;            /* autopilot "spudl 0" turns it off: the chip's interrupt is a deadline of the idle
                                 * wait. (Under Dolphin on Windows alone it made the emulation 16 to 27% slower
                                 * than real time: the host's short sleeps run late and the guest has nothing to
                                 * do in the time lost. plat_wii.c has the idle waits spin there, "idlespin".) */

uint64_t iop_sd_next_us(void)
{
    uint64_t best = UINT64_MAX;
    int core;
    if (!g_spu_irqdl || !g_last_us)
        return best;
    for (core = 0; core < 2; core++) {
        const Core *k = &g_core[core];
        const uint32_t irqa = k->addr[A_IRQA - A_ESA];
        uint64_t s, first, due;
        if (g_irq_pending[core])
            return 0;
        if (!(k->attr[CORE_IRQ_ENABLE >> 1] & 1) || irqa >= 0x4000)
            continue;
        s = (irqa & 0x3FF) >> 1;
        first = g_sample_clock - g_sample_clock % 512 + s;
        if (first < g_sample_clock)
            first += 512;
        /* the tick that covers sample `first` has advanced the clock past it (and a tick takes 5 ms at least) */
        due = g_last_us + (first + 1 - g_sample_clock) * 1000u / 48u;
        if (due < g_last_us + 5000)
            due = g_last_us + 5000;
        if (due < best)
            best = due;
    }
    return best;
}

/* Sound while the game cannot tick: long stretches of native work call this (a track's textures
 * decoded and its display lists made at the race's first frames left a third of a second of
 * silence in every loading: the output holds 50 ms). It mixes when the output is running low; what
 * a tick hands to the driver (interrupts, transfers done) waits for the tick. */
void iop_sd_pump(void)
{
    static int inside;
    const int level = plat_audio_level();
    if (inside || level < 0 || level >= AUDIO_AHEAD / 2 || !g_spu_ram || !g_last_us)
        return;
    inside = 1;
    spu_tick(plat_time_us());
    inside = 0;
}

/* called from the IOP kernel tick: deferred DMA completions and SPU2 interrupts */
void iop_sd_tick(uint64_t now)
{
    int ch;
    spu_tick(now);
    for (ch = 0; ch < 2; ch++) {
        if (g_trans[ch].pending) {
            g_trans[ch].pending = 0;
            if (g_trans[ch].func)
                iop_call_irq(g_trans[ch].func, (uint32_t)ch, g_trans[ch].arg, 0);
        }
    }
    {
        /* one call for both cores: the handler gets a mask, bit n = core n */
        uint32_t mask = 0;
        for (ch = 0; ch < 2; ch++) {
            if (g_irq_pending[ch]) {
                g_irq_pending[ch] = 0;
                g_core[ch].attr[CORE_IRQ_ENABLE >> 1] = 0;
                mask |= 1u << ch;
            }
        }
        if (mask && g_spu2_func) {
            g_irq_calls[mask]++;
            iop_call_irq(g_spu2_func, mask, g_spu2_arg, 0);
        }
    }
}

/* ---- exports -------------------------------------------------------------------------------- */

void iop_hle_sceSdInit(R5900 *c)
{
    LOG("IOP sceSdInit(%u)", iop_arg(c, 0));
    memset(g_core, 0, sizeof g_core);
    /* as the library leaves the chip: core 0's output into core 1 at full volume */
    g_core[1].param[P_AVOLL] = g_core[1].param[P_AVOLR] = 0x7FFF;
    g_core[0].param[P_MMIX] = 0xFF0;
    g_core[1].param[P_MMIX] = 0xFFC;
    iop_ret(c, 0);
}

void iop_hle_sceSdSetParam(R5900 *c)
{
    uint32_t e = iop_arg(c, 0);
    uint16_t val = (uint16_t)iop_arg(c, 1);
    int core = e & 1, v = (e >> 1) & 31, reg = (e >> 8) & 0xFF;
    if (reg < 8 && v < VOICES) {
        Voice *x = &g_core[core].v[v];
        if (PS2_UNLIKELY(g_spu_dump) && core == 1 && v >= 16 && v <= 19 && (reg == VP_VOLL || reg == VP_VOLR) &&
            g_ev_n < EV_MAX) {
            /* the music's and the DJ's volumes: 'L'/'R' when the register is written, the register's new
             * value as the place, the level it stood at as the second number */
            g_ev_all++;
            g_ev_log[g_ev_n][0] = (uint32_t)(plat_time_us() / 1000u);
            g_ev_log[g_ev_n][1] = (uint32_t)(reg == VP_VOLL ? 'L' : 'R') << 16 | (uint32_t)core << 8 | (uint32_t)v;
            g_ev_log[g_ev_n][2] = val;
            g_ev_log[g_ev_n++][3] = (uint32_t)x->vol[reg == VP_VOLR] & 0xFFFF;
            {
                static int shown;
                if (shown < 6 && v == 16 && reg == VP_VOLL) {
                    shown++;
                    LOG("sound: volume write core 1 voice 16 left %04X, level %04X, from ra %08X", (unsigned)val,
                        (unsigned)x->vol[0] & 0xFFFFu, (unsigned)c->r[31][0]);
                }
            }
        }
        x->param[reg] = val;
        if (reg == VP_VOLL || reg == VP_VOLR)
            vol_set(x, reg == VP_VOLR, val);
        else if ((reg == VP_ADSR1 || reg == VP_ADSR2) && x->env_wait > 1)
            x->env_wait = 1;            /* a level that stood: by the new rates from the next sample */
    } else if (reg < 0x20) {
        g_core[core].param[reg] = val;
    }
    iop_ret(c, 0);
}

void iop_hle_sceSdGetParam(R5900 *c)
{
    uint32_t e = iop_arg(c, 0);
    int core = e & 1, v = (e >> 1) & 31, reg = (e >> 8) & 0xFF;
    uint32_t r = 0;
    if (reg == VP_ENVX && v < VOICES)
        r = g_core[core].v[v].playing ? (uint32_t)g_core[core].v[v].env : 0;
    else if ((reg == VP_VOLXL || reg == VP_VOLXR) && v < VOICES) {
        r = (uint32_t)g_core[core].v[v].vol[reg == VP_VOLXR] & 0xFFFF;
        if (PS2_UNLIKELY(g_spu_dump)) {
            static int shown;
            if (shown < 12 && core == 1 && v >= 16) {
                shown++;
                LOG("sound: volume read core 1 voice %d %s = %04X, from ra %08X", v, reg == VP_VOLXL ? "left" : "right",
                    (unsigned)r, (unsigned)c->r[31][0]);
            }
        }
    }
    else if (reg < 8 && v < VOICES)
        r = g_core[core].v[v].param[reg];
    else if (reg < 0x20)
        r = g_core[core].param[reg];
    iop_ret(c, (int32_t)r);
}

void iop_hle_sceSdSetSwitch(R5900 *c)
{
    uint32_t e = iop_arg(c, 0), val = iop_arg(c, 1);
    int core = e & 1, reg = (e >> 8) & 0xFF, v;
    if (reg == S_KON) {
        for (v = 0; v < VOICES; v++)
            if (val & (1u << v))
                key_on(core, v);
    } else if (reg == S_KOFF) {
        for (v = 0; v < VOICES; v++)
            if (val & (1u << v))
                key_off(core, v);
    } else if (reg == S_ENDX) {
        g_core[core].endx = 0;
    } else if (reg < 0x20) {
        g_core[core].sw[reg] = val;
    }
    iop_ret(c, 0);
}

void iop_hle_sceSdGetSwitch(R5900 *c)
{
    uint32_t e = iop_arg(c, 0);
    int core = e & 1, reg = (e >> 8) & 0xFF;
    uint32_t r = 0, v;
    if (reg == S_ENDX)
        r = g_core[core].endx;
    else if (reg == S_KON)
        for (v = 0; v < VOICES; v++)
            r |= (uint32_t)(g_core[core].v[v].playing && !g_core[core].v[v].released) << v;
    else if (reg < 0x20)
        r = g_core[core].sw[reg];
    iop_ret(c, (int32_t)r);
}

void iop_hle_sceSdSetAddr(R5900 *c)
{
    uint32_t e = iop_arg(c, 0), a = iop_arg(c, 1) & (SPU_RAM_SIZE - 1);
    int core = e & 1, v = (e >> 1) & 31, reg = (e >> 8) & 0xFF;
    if (reg >= VA_SSA && v < VOICES) {
        Voice *x = &g_core[core].v[v];
        if (reg == VA_SSA)
            x->ssa = a;
        else if (reg == VA_LSAX)
            x->lsax = a;
        else
            x->nax = a;
    } else if (reg >= A_ESA && reg <= A_IRQA) {
        g_core[core].addr[reg - A_ESA] = a;
        if (reg == A_IRQA && g_trace > 1)
            LOG("IOP SPU2 core %d IRQA = %06X", core, a);
    }
    iop_ret(c, 0);
}

void iop_hle_sceSdGetAddr(R5900 *c)
{
    uint32_t e = iop_arg(c, 0), r = 0;
    int core = e & 1, v = (e >> 1) & 31, reg = (e >> 8) & 0xFF;
    spu_tick(plat_time_us());
    if (reg >= VA_SSA && v < VOICES) {
        Voice *x = &g_core[core].v[v];
        r = reg == VA_SSA ? x->ssa : reg == VA_LSAX ? x->lsax : x->nax;
    } else if (reg >= A_ESA && reg <= A_IRQA) {
        r = g_core[core].addr[reg - A_ESA];
    }
    iop_ret(c, (int32_t)r);
}

void iop_hle_sceSdSetCoreAttr(R5900 *c)
{
    uint32_t e = iop_arg(c, 0);
    if (g_trace > 1)
        LOG("IOP sceSdSetCoreAttr core %u attr %u = %u", e & 1, (e >> 1) & 7, iop_arg(c, 1));
    g_core[e & 1].attr[(e >> 1) & 7] = iop_arg(c, 1);
    iop_ret(c, 0);
}

void iop_hle_sceSdGetCoreAttr(R5900 *c)
{
    uint32_t e = iop_arg(c, 0);
    iop_ret(c, (int32_t)g_core[e & 1].attr[(e >> 1) & 7]);
}

/* sceSdVoiceTrans(channel, mode, iop_addr, spu_addr, size): mode bit 0 = read from sound RAM,
 * bit 3 = by PIO instead of DMA */
void iop_hle_sceSdVoiceTrans(R5900 *c)
{
    uint32_t ch = iop_arg(c, 0) & 1, mode = iop_arg(c, 1), iop = iop_arg(c, 2);
    uint32_t spu = iop_arg(c, 3) & (SPU_RAM_SIZE - 1), size = iop_arg(c, 4);
    if (spu + size > SPU_RAM_SIZE)
        size = SPU_RAM_SIZE - spu;
    if (mode & 1) {
        iop_write_bytes(iop, g_spu_ram + spu, size);
    } else {
        iop_read_bytes(g_spu_ram + spu, iop, size);
#if DSP_MIX
        DCStoreRange(g_spu_ram + spu, size);    /* the DSP reads the blocks from memory */
#endif
    }
    {
        uint32_t *st = g_tr_stat[mode & 1], k;
        st[0]++;
        st[1] += size;
        if (spu < st[2])
            st[2] = spu;
        if (spu + size > st[3])
            st[3] = spu + size;
        if ((mode & 1) && spu < 0x4000)
            for (k = spu / 0x400u; k <= (spu + size - 1) / 0x400u && k < 16; k++)
                g_ring_read |= 1u << k;
        if (!(mode & 1) && spu >= 0x5000 && g_spu_dump) {
            /* debugging: a write over the block a voice is playing */
            int core, v;
            for (core = 0; core < 2; core++)
                for (v = 0; v < VOICES; v++) {
                    const Voice *x = &g_core[core].v[v];
                    if (x->playing && (x->nax & ~15u) >= spu && (x->nax & ~15u) < spu + size) {
                        g_tr_under++;
                        if (g_ev_n < EV_MAX) {
                            g_ev_log[g_ev_n][0] = (uint32_t)(plat_time_us() / 1000u);
                            g_ev_log[g_ev_n][1] = (uint32_t)'w' << 16 | (uint32_t)core << 8 | (uint32_t)v;
                            g_ev_log[g_ev_n][2] = spu;
                            g_ev_log[g_ev_n++][3] = size;
                        }
                    }
                }
        }
        if (spu < 0x5000 && g_spu_dump && g_tr_log_n < 48) {
            /* debugging: the transfers at the rings: time, the chip's place in the rings, direction, where, how much */
            g_tr_log[g_tr_log_n][0] = (uint32_t)plat_time_us();
            g_tr_log[g_tr_log_n][1] = (uint32_t)(g_sample_clock & 511);
            g_tr_log[g_tr_log_n][2] = (mode & 1) << 31 | spu;
            g_tr_log[g_tr_log_n++][3] = size;
        }
    }
    if (g_trace > 1)
        LOG("IOP sceSdVoiceTrans ch %u mode %X %08X <-> spu %06X size %X", ch, mode, iop,
            spu, size);
    if (!(mode & 8))
        g_trans[ch].pending = 1;       /* DMA transfers interrupt on completion; PIO (mode bit 3) does not */
    iop_ret(c, (int32_t)size);
}

void iop_hle_sceSdVoiceTransStatus(R5900 *c)
{
    iop_ret(c, 1);
}

void iop_hle_sceSdSetEffectAttr(R5900 *c)
{
    /* (core, attr): attr = {core, mode, depth L, depth R, delay, feedback}; the mode's
     * 16-bit preset goes into the chip's effect registers as libsd writes it */
    const uint32_t core = iop_arg(c, 0) & 1, p = iop_arg(c, 1);
    const uint32_t raw = p ? iop_r32(p + 4) : 0;
    const int clear = (raw & 0x100u) != 0;
    const int mode = (int)(raw & ~0x100u);
    uint16_t md[32];
    uint32_t eea, size, esa, k;
    int depth_l = 0, depth_r = 0;
    g_fx_calls++;
    if (mode > 9) {
        iop_ret(c, -1);
        return;
    }
    if (p) {
        depth_l = (int16_t)iop_r16(p + 8);
        depth_r = (int16_t)iop_r16(p + 10);
    }
    eea = rev_eea(&g_core[core]);
    size = (uint32_t)k_effect_sizes[mode] << 4;
    esa = ((eea - (size - 2)) >> 1) & 0xFFFFFu;
    for (k = 0; k < 16; k++) {
        const uint32_t v = k_effect_params[mode * 16 + k];
        md[2 * k] = (uint16_t)v;
        md[2 * k + 1] = (uint16_t)(v >> 16);
    }
    if (mode == 7 || mode == 8) {       /* echo and delay: the depth and feedback in the preset */
        const uint32_t delay = p ? iop_r32(p + 12) : 0, feedback = p ? iop_r32(p + 16) : 0;
        const uint16_t d = (uint16_t)((delay + 1) << 5);
        const uint16_t fb = (uint16_t)(((feedback << 7) + feedback) << 1);
        md[7] = fb;
        md[10] = (uint16_t)((d << 1) - md[0]);
        md[11] = (uint16_t)(d - md[1]);
        md[16] = (uint16_t)(md[17] + d);
        md[12] = (uint16_t)(md[13] + d);
        md[27] = (uint16_t)(md[29] + d);
        md[26] = (uint16_t)(md[28] + d);
    }
    if (clear || g_rev[core].mode != mode)
        rev_clear_work(mode, eea);
    rev_set(core, mode, esa, eea >> 1, md, depth_l, depth_r);
    g_fx_mode = (uint32_t)mode;
    iop_ret(c, 0);
}

void iop_hle_sceSdGetEffectAttr(R5900 *c)
{
    const uint32_t core = iop_arg(c, 0) & 1, p = iop_arg(c, 1);
    if (p) {
        iop_w32(p + 0, core);
        iop_w32(p + 4, (uint32_t)g_rev[core].mode);
        iop_w16(p + 8, g_core[core].param[P_EVOLL]);
        iop_w16(p + 10, g_core[core].param[P_EVOLR]);
        iop_w32(p + 12, 0);
        iop_w32(p + 16, 0);
    }
    iop_ret(c, 0);
}

void iop_hle_sceSdClearEffectWorkArea(R5900 *c)
{
    const uint32_t core = iop_arg(c, 0) & 1;
    const int mode = (int)iop_arg(c, 2);
    if (mode < 0 || mode > 9) {
        iop_ret(c, -1);
        return;
    }
    rev_clear_work(mode, rev_eea(&g_core[core]));
    iop_ret(c, 0);
}

void iop_hle_sceSdSetTransIntrHandler(R5900 *c)
{
    uint32_t ch = iop_arg(c, 0) & 1;
    iop_ret(c, (int32_t)g_trans[ch].func);
    g_trans[ch].func = iop_arg(c, 1);
    g_trans[ch].arg = iop_arg(c, 2);
}

void iop_hle_sceSdSetSpu2IntrHandler(R5900 *c)
{
    iop_ret(c, (int32_t)g_spu2_func);
    g_spu2_func = iop_arg(c, 0);
    g_spu2_arg = iop_arg(c, 1);
}

void iop_hle_sceSd28(R5900 *c)     /* sceSdGetTransIntrHandlerArgument(channel) */
{
    iop_ret(c, (int32_t)g_trans[iop_arg(c, 0) & 1].arg);
}

void iop_hle_sceSd29(R5900 *c)     /* sceSdGetSpu2IntrHandlerArgument() */
{
    iop_ret(c, (int32_t)g_spu2_arg);
}

void iop_sd_init(void)
{
    g_spu_ram = plat_alloc(SPU_RAM_SIZE);
    memset(g_spu_ram, 0, SPU_RAM_SIZE);
#if DSP_MIX
    DCStoreRange(g_spu_ram, SPU_RAM_SIZE);
#endif
    memset(g_core, 0, sizeof g_core);
    memset(g_rev, 0, sizeof g_rev);
    memset(g_rev_peak, 0, sizeof g_rev_peak);
    rev_selftest();
}
