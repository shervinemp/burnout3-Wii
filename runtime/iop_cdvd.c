/*
 * cdvdman exports for recompiled IOP modules (the GTFS file system reads the
 * disc by sector) and the EE-facing CDVD RPC servers.
 */
#include "iop.h"

#include <string.h>

#define SCECdPS2DVD 0x14
#define SCECdComplete 0x02

static int g_last_error;

/* sceCdRead(lsn, sectors, buf, mode). The calling thread waits for the data
 * as a guest thread (disc_read_sectors_guest): the read used to hold every
 * thread for the 10 to 20 ms a block takes from the card, the game's main
 * thread among them, in the middle of a frame (a stream starting, a sound
 * loaded: the collisions' hitches). */
void iop_hle_sceCdRead(R5900 *c)
{
    uint32_t lsn = iop_arg(c, 0), n = iop_arg(c, 1), buf = iop_arg(c, 2);
    static uint8_t tmp[16 * 2048];         /* not on the (small) host thread stack */
    static int busy;                       /* tmp is one thread's at a time */
    if (g_trace)
        LOG("IOP sceCdRead lsn %u x%u -> %08X", lsn, n, buf);
    while (busy && kern_can_wait())
        kern_wait_host();
    busy = 1;
    g_last_error = 0;
    while (n) {
        uint32_t k = n < 16 ? n : 16;
        if (disc_read_sectors_guest(lsn, k, tmp)) {
            g_last_error = 0x32;           /* SCECdErREAD */
            break;
        }
        iop_write_bytes(buf, tmp, k * 2048);
        buf += k * 2048;
        lsn += k;
        n -= k;
    }
    busy = 0;
    g_host_event = 1;                      /* (another thread may be waiting for tmp) */
    iop_ret(c, 1);
}

void iop_hle_sceCdSync(R5900 *c)
{
    iop_ret(c, 0);                         /* reads complete synchronously */
}

void iop_hle_sceCdGetError(R5900 *c)
{
    iop_ret(c, g_last_error);
}

void iop_hle_sceCdGetDiskType(R5900 *c)
{
    iop_ret(c, SCECdPS2DVD);
}

void iop_hle_sceCdDiskReady(R5900 *c)
{
    iop_ret(c, SCECdComplete);
}

static uint8_t bcd(int v)
{
    return (uint8_t)((v / 10) << 4 | (v % 10));
}

/* sceCdReadClock(clock): {stat, second, minute, hour, pad, day, month, year} in BCD */
void iop_hle_sceCdReadClock(R5900 *c)
{
    uint32_t p = iop_arg(c, 0);
    uint64_t s = plat_time_us() / 1000000u;
    iop_w8(p + 0, 0);
    iop_w8(p + 1, bcd((int)(s % 60)));
    iop_w8(p + 2, bcd((int)(s / 60 % 60)));
    iop_w8(p + 3, bcd((int)(s / 3600 % 24)));
    iop_w8(p + 4, 0);
    iop_w8(p + 5, bcd(1));
    iop_w8(p + 6, bcd(1));
    iop_w8(p + 7, bcd(5));
    iop_ret(c, 1);
}

void iop_cdvd_init(void)
{
    g_last_error = 0;
}
