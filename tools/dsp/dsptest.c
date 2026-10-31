/*
 * dsptest.c: stage 0 of the DSP mixer: a toy microcode (toy.s) loaded with libogc's DSP_AddTask,
 * a mail round trip and DMA to and from main memory, in a standalone dol.
 *
 *   sh tools/dsp/build.sh                    -> build-dsptest/dsptest.dol
 *   Dolphin with -C Dolphin.Core.DSPHLE=False (the DSP's low-level emulation: HLE has no idea of a custom ucode)
 *
 * The log goes to Dolphin's OSReport UART (SYS_Report): grep "dsptest" in Logs/dolphin.log.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "toy.h"

#define NWORDS 64

static dsptask_t g_task;
static volatile u32 g_init_cb;
static u16 g_mem1[NWORDS + 16] __attribute__((aligned(32)));

static void init_cb(dsptask_t *t)
{
    (void)t;
    g_init_cb = 1;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SYS_Report("dsptest: %s\n", line);
    printf("%s\n", line);
}

static void mail_send(u32 mail)
{
    DSP_SendMailTo(mail);
    while (DSP_CheckMailTo())
        ;
}

/* the DSP's reply (polled: the toy does not raise the interrupt for it) */
static int mail_wait(u32 *mail, u32 timeout_ms)
{
    const u64 t0 = gettime();
    while (!DSP_CheckMailFrom())
        if (ticks_to_millisecs(gettime() - t0) > timeout_ms)
            return 0;
    *mail = DSP_ReadMailFrom();
    return 1;
}

/* one run of the toy on a block of NWORDS words at `p` (cached memory), k added to each; returns the DSP's mail or 0 */
static u32 run_block(u16 *p, u32 k, u64 *us)
{
    const u32 phys = (u32)p & 0x1FFFFFFFu;
    u32 mail = 0;
    u64 t0;
    int i;
    for (i = 0; i < NWORDS; i++)
        p[i] = (u16)(i * 0x0101 + 7);
    DCFlushRange(p, NWORDS * 2);
    mail_send(0xFACE0080);
    mail_send(phys);
    t0 = gettime();
    mail_send(0xFACE0010);
    mail_send(k & 0xFFFF);
    if (!mail_wait(&mail, 2000))
        return 0;
    *us = ticks_to_microsecs(gettime() - t0);
    DCInvalidateRange(p, NWORDS * 2);
    return mail;
}

static int check_block(const u16 *p, u32 k)
{
    int i, bad = 0;
    for (i = 0; i < NWORDS; i++) {
        const u16 want = (u16)((i * 0x0101 + 7) + k);
        if (p[i] != want) {
            if (bad < 4)
                say("  word %d: got %04X want %04X", i, p[i], want);
            bad++;
        }
    }
    return bad;
}

int main(void)
{
    u64 t0, us = 0;
    u32 mail;
    int bad;

    DSP_Init();
    AUDIO_Init(NULL);
    AUDIO_StopDMA();
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);

    DCFlushRange((void *)toy, toy_size);
    memset(&g_task, 0, sizeof g_task);
    g_task.prio = 255;
    g_task.iram_maddr = (void *)((u32)toy & 0x1FFFFFFFu);
    g_task.iram_len = toy_size;
    g_task.iram_addr = 0x0000;
    g_task.dram_maddr = NULL;
    g_task.dram_len = 0;
    g_task.dram_addr = 0x0000;
    g_task.init_vec = 0x0010;
    g_task.init_cb = init_cb;
    say("adding the task (%u bytes of code)", (unsigned)toy_size);
    t0 = gettime();
    DSP_AddTask(&g_task);
    while (!g_init_cb && ticks_to_millisecs(gettime() - t0) < 3000)
        ;
    if (!g_init_cb) {
        say("FAIL: the init mail did not come in 3 s");
        goto stop;
    }
    say("PASS init: the task is up after %u us", (unsigned)ticks_to_microsecs(gettime() - t0));

    mail = run_block(g_mem1, 0x1111, &us);
    if (!mail) {
        say("FAIL: no reply from the DSP (MEM1 block)");
    } else {
        bad = check_block(g_mem1, 0x1111);
        say("%s MEM1 block: mail %08X, %d bad words, round trip %u us", bad || (mail >> 16) != 0xBEEF ? "FAIL" : "PASS", (unsigned)mail, bad,
            (unsigned)us);
    }

    /* the same block in MEM2 (the physical address is 0x10200000 and up): does the DSP's DMA reach it? */
    {
        u16 *m2 = (u16 *)0x90200000;
        mail = run_block(m2, 0x0222, &us);
        if (!mail) {
            say("FAIL: no reply from the DSP (MEM2 block)");
        } else {
            bad = check_block(m2, 0x0222);
            say("%s MEM2 block: mail %08X, %d bad words, round trip %u us", bad || (mail >> 16) != 0xBEEF ? "FAIL" : "PASS", (unsigned)mail, bad,
                (unsigned)us);
        }
    }

    {
        /* a hundred runs: the count in the mails goes on, the data stays right */
        int i, wrong = 0;
        u64 total = 0;
        for (i = 0; i < 100; i++) {
            const u32 k = (u32)(i * 37 + 5) & 0xFFFF;
            mail = run_block(g_mem1, k, &us);
            if (!mail || (mail & 0xFFFF) != (u32)(i + 3) || check_block(g_mem1, k))
                wrong++;
            total += us;
        }
        say("%s 100 more runs: %d wrong, mean round trip %u us (last mail %08X)", wrong ? "FAIL" : "PASS", wrong, (unsigned)(total / 100),
            (unsigned)mail);
    }
stop:
    say("DONE");
    for (;;)
        VIDEO_WaitVSync();
    return 0;
}
