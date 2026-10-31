/*
 * The sound chip's mixer on the Wii's DSP (tools/dsp/dspmix.s): the driver. The CPU fills a slot's command block with
 * one job per voice (the runs of ADPCM blocks the voice's place enters, which the DSP takes from sound RAM and decodes
 * itself, its place and pitch, its gains, and its record: the voice's decoder, which the DSP reads and writes back),
 * calls dspmix_run, and some time later (the next tick) takes the sums with dspmix_poll or dspmix_wait. Four slots can
 * be out at once: one run going on, the next in the DSP's mail box, the others waiting here until the box is empty
 * (dspmix_poll mails them); they are answered in the order they went.
 * Command blocks, run lists, records and sums are in MEM1; the blocks are read wherever sound RAM is (MEM2).
 */
#ifndef DSP_MIX_H
#define DSP_MIX_H

#include <stdint.h>

#define DSPMIX_SLOTS 4
#define DSPMIX_MAX_JOBS 24
#define DSPMIX_CHUNK 256                /* samples in a run, at most */
#define DSPMIX_BLOCKS 38                /* blocks a job's place may enter, at most */

#define DSPMIX_LEFT 0x0100u             /* coref: the side's gain is not zero */
#define DSPMIX_RIGHT 0x0200u

/* all halfwords (the DSP reads 16-bit words) */
typedef struct DspMixJob {
    uint16_t runs_hi, runs_lo;          /* its runs' list (DspMixRun), physical address, 32-byte aligned */
    uint16_t nruns;                     /* runs in it */
    uint16_t coref;                     /* core in bit 0, DSPMIX_LEFT, DSPMIX_RIGHT */
    uint16_t cnt;                       /* samples (1..256) */
    uint16_t idx;                       /* index in the stream of the first sample's left neighbour (the record's block
                                         * is the stream's [0..28], each block entered 28 more) */
    uint16_t frac;                      /* fraction there, 16 bits (the place's 12 bits << 4) */
    uint16_t pint, pfrac;               /* pitch: integer part, 16 bits of fraction (pitch & 0xFFF) << 4 */
    int16_t gl, gr;                     /* gains at the start, of 32768 */
    uint16_t dl_hi, dl_lo, dr_hi, dr_lo;/* gain steps a sample, 32 bits with 16 of fraction */
    uint16_t voice;                     /* its record: the records' address + 64 * voice */
} DspMixJob;

/* consecutive 16-byte ADPCM blocks from `at` (physical, 16-byte aligned); a list is padded to 32 bytes */
typedef struct DspMixRun {
    uint16_t at_hi, at_lo, blocks, spare;
} DspMixRun;

/* a voice's decoder (64 bytes): the block it is in, the sample before it first, and its last two samples */
typedef struct DspMixRecord {
    int16_t smp[29], p1, p2, spare;
} DspMixRecord;

typedef struct DspMixCmd {
    uint16_t jobs, samples, res_hi, res_lo, rec_hi, rec_lo, pad[10];
    DspMixJob job[DSPMIX_MAX_JOBS];
} DspMixCmd;

/* one sample of a core's sum: (m << 16 | l) is the sum in 1/32768 of a sample step: left m, right m, left l, right l */
typedef struct DspMixSum {
    int16_t m[2];
    uint16_t l[2];
} DspMixSum;

extern int g_dsp_on;                    /* the platform's switch (args.txt "--dsp 1", autopilot "dsp 1") */

/* MEM1 for the DSP's side of things (32-byte aligned, zeroed): taken from the arena for good, NULL when there is none */
void *dspmix_mem(uint32_t bytes);
/* loads the microcode and waits for it to come up; 1 when it runs (once failed, it stays failed) */
int dspmix_start(void);
int dspmix_up(void);
DspMixCmd *dspmix_cmd(int slot);        /* a slot's command block to fill (its stream addresses are physical) */
/* the jobs written to a slot's command block go to the DSP (the caller has flushed their run lists and records, and
 * sound RAM's blocks are in memory); records: the voices' records (32-byte aligned, MEM1). 0 when the DSP is down (or
 * has just been given up on: it would not take the mail) */
int dspmix_run(int slot, uint32_t jobs, uint32_t samples, const DspMixRecord *records);
int dspmix_out(void);                   /* runs sent and not answered */
/* the oldest run's answer: its slot when it is in (its sums invalidated), -1 when not yet or the DSP is down. dspmix_wait
 * waits at most timeout_us for it, and gives the DSP up when it does not come (every run out is lost then) */
int dspmix_poll(void);
int dspmix_wait(uint32_t timeout_us);
/* a slot's sums of its last run, once answered; NULL when no job was for that core (nothing was sent: they are zero).
 * A sum that would pass twice full scale stops at m = 0x7FFF / 0x8000. */
const DspMixSum *dspmix_sums(int slot, int core);
void dspmix_report(char *line, unsigned size);

#endif
