/*
 * SIO2, the IOP's serial controller for pads and memory cards, with the
 * devices behind it: a DualShock 2 on port 0 and an 8 MB memory card in
 * slot 0. The game's own drivers (SIO2MAN, SIO2D, DBCMAN, DS2O, MC2_S1)
 * run recompiled on top of this, so the libpad2/libmc2 logic is the real one.
 *
 * A transfer is a queue of up to 16 commands (SEND3 words: port in bit 0,
 * length in bits 8-17). Command bytes arrive through the FIFO register or
 * DMA channel 11; when a command is complete the device answers one byte
 * per byte sent, and the answer is read back through the FIFO or DMA
 * channel 12. Byte formats follow PCSX2's SIO2 / DualShock 2 / memory card
 * protocol implementations.
 */
#include "iop.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SIO2_IRQ 17

/* RECV1 (command status) */
#define STAT_DISCONNECTED 0x1D100u
#define STAT_CONNECTED 0x1100u
#define STAT_NO_DEVICES_MISSING 0x1000u
#define STAT_PORT1_MISSING 0x1D000u
#define STAT_PORT2_MISSING 0x2D000u
#define STAT_ONE_PORT_OPEN 0x100u
#define STAT_TWO_PORTS_OPEN 0x200u

/* memory card geometry: 512-byte pages with 16 bytes of ECC, erased 16 pages at a time */
#define MC_PAGE 512u
#define MC_RAW_PAGE (MC_PAGE + 16u)
#define MC_PAGES 0x4000u
#define MC_ERASE_PAGES 16u
#define MC_SIZE (MC_RAW_PAGE * MC_PAGES)

typedef struct Pad {
    uint8_t mode;               /* 0x41 digital, 0x73 analog, 0x79 DualShock 2 */
    uint8_t config;             /* in config mode (answers 0xF3) */
    uint8_t cmd;
    uint8_t stage;
    uint8_t light;
    uint8_t motor[2], small_cfg, large_cfg;
    uint32_t resp_mask;
    PlatPad in;                 /* host state sampled for the current poll */
} Pad;

typedef struct Card {
    FILE *f;
    uint8_t term;
    uint32_t addr;              /* byte offset of the next raw transfer */
} Card;

static struct {
    uint32_t send3[16], send1[4], send2[4];
    uint32_t ctrl, recv1, recv3, istat, unk78, unk7c;
    int pos;                    /* SEND3 entry of the command being collected */
    int len;                    /* its length */
    int collecting;
    int queue_done;
    int dma_block;              /* bytes per command when fed by DMA, else 0 */
    uint8_t in[1024];
    int nin;
    uint8_t out[16384];
    int out_r, out_w;
} s;

static Pad g_pad[2];
static Card g_card;
static int g_card_dirty;        /* writes not yet fsynced */
static int g_card_writes, g_card_erases;
static int g_card_slot2_logged;

/* ---- DualShock 2 ---------------------------------------------------------------------------- */

static int pressed(const Pad *p, int bit)
{
    return !(p->in.buttons & (1u << bit));
}

static uint8_t pad_poll(Pad *p, int k, uint8_t b)
{
    static const int8_t bits[12] = {13, 15, 12, 14, 4, 5, 6, 7, 2, 3, 0, 1};
    switch (k) {
    case 3: p->motor[0] = b; return (uint8_t)(p->in.buttons >> 8);
    case 4: p->motor[1] = b; return (uint8_t)p->in.buttons;
    case 5: return p->in.rx;
    case 6: return p->in.ry;
    case 7: return p->in.lx;
    case 8: return p->in.ly;
    default:
        if (k >= 9 && k <= 20)
            return pressed(p, bits[k - 9]) ? p->in.pressure[k - 9] : 0;
        return 0;
    }
}

static uint8_t pad_byte(Pad *p, int k, uint8_t b)
{
    if (k == 1) {
        p->cmd = b;
        return p->config ? 0xF3 : p->mode;
    }
    if (k == 2)
        return 0x5A;
    switch (p->cmd) {
    case 0x40:
        return k == 5 ? 0x02 : k == 8 ? 0x5A : 0x00;
    case 0x41:                  /* which buttons report pressure */
        if (p->mode == 0x41)
            return 0x00;
        return k == 3 || k == 4 ? 0xFF : k == 5 ? 0x03 : k == 8 ? 0x5A : 0x00;
    case 0x42:
        return pad_poll(p, k, b);
    case 0x43:                  /* enter / leave config mode */
        if (k == 3)
            p->config = b != 0;
        return 0x00;
    case 0x44:                  /* analog on/off and lock */
        if (k == 3) {
            p->light = b != 0;
            p->mode = b ? 0x73 : 0x41;
        }
        return 0x00;
    case 0x45:                  /* model: standard DualShock 2 */
        switch (k) {
        case 3: return 0x03;
        case 4: return 0x02;
        case 5: return p->light;
        case 6: return 0x02;
        case 7: return 0x01;
        default: return 0x00;
        }
    case 0x46:
        if (k == 3)
            p->stage = b != 0;
        return k == 5 ? 0x01 : k == 6 ? (p->stage ? 0x01 : 0x02) : k == 7 ? (p->stage ? 0x01 : 0x00)
             : k == 8 ? (p->stage ? 0x14 : 0x0A) : 0x00;
    case 0x47:
        return k == 5 ? 0x02 : k == 7 ? 0x01 : 0x00;
    case 0x4C:
        if (k == 3)
            p->stage = b != 0;
        return k == 6 ? (p->stage ? 0x07 : 0x04) : 0x00;
    case 0x4D: {                /* vibration motor mapping */
        uint8_t r = 0xFF;
        if (k == 3) {
            r = p->small_cfg;
            p->small_cfg = b;
        } else if (k == 4) {
            r = p->large_cfg;
            p->large_cfg = b;
        }
        return r;
    }
    case 0x4F:                  /* response bytes: analog or analog + pressure */
        if (k == 3)
            p->resp_mask = b;
        else if (k == 4)
            p->resp_mask |= (uint32_t)b << 8;
        else if (k == 5) {
            p->resp_mask |= (uint32_t)b << 16;
            p->light = p->resp_mask == 0x3F || p->resp_mask == 0x3FFFF;
            p->mode = p->resp_mask == 0x3FFFF ? 0x79 : p->resp_mask == 0x3F ? 0x73 : 0x41;
        }
        return k == 8 ? 0x5A : 0x00;
    default:
        return 0x00;
    }
}

/* The vibration the game set in its last poll (command 0x42) goes to the platform as one strength, 0 to 255. The bytes it
 * puts in the poll's places 3 and 4 drive the motors it mapped to them with command 0x4D (0x00 the small motor, on with
 * bit 0; 0x01 the large one, any strength; 0xFF none). A platform has one motor to drive: the stronger of the two, the
 * small motor's buzz counting for about a third. */
extern void plat_pad_rumble(int port, unsigned strength);
#define SMALL_MOTOR_STRENGTH 0x50u

static void pad_vibrate(int port, const Pad *p)
{
    static unsigned last[2] = {0, 0};
    static int logged;
    unsigned small = 0, large = 0, s;
    if (p->small_cfg == 0x00)
        small = p->motor[0] & 1u;
    else if (p->small_cfg == 0x01)
        large = p->motor[0];
    if (p->large_cfg == 0x00)
        small = p->motor[1] & 1u;
    else if (p->large_cfg == 0x01)
        large = p->motor[1];
    s = small && large < SMALL_MOTOR_STRENGTH ? SMALL_MOTOR_STRENGTH : large;
    if (s != last[port] && (s == 0 || last[port] == 0) && logged < 24) {
        logged++;
        LOG("pad port %d: vibration %s (small %u, large %u; motors mapped %02X %02X)", port + 1, s ? "on" : "off", small, large,
            p->small_cfg, p->large_cfg);
    }
    last[port] = s;
    plat_pad_rumble(port, s);
}

static int pad_command(int port, const uint8_t *in, int n, uint8_t *out)
{
    static int seen[2] = {-1, -1};
    Pad *p = &g_pad[port];
    int k;
    plat_read_pad(port, &p->in);
    if (p->in.connected != seen[port]) {
        seen[port] = p->in.connected;
        LOG("pad port %d: %s", port + 1, p->in.connected ? "plugged in" : "empty");
    }
    if (s.recv1 & STAT_ONE_PORT_OPEN)
        s.recv1 = (s.recv1 & ~STAT_ONE_PORT_OPEN) | STAT_TWO_PORTS_OPEN;
    else
        s.recv1 |= STAT_ONE_PORT_OPEN;
    s.recv1 |= STAT_NO_DEVICES_MISSING;
    out[0] = 0xFF;
    if (!p->in.connected) {
        s.recv1 |= port ? STAT_PORT2_MISSING : STAT_PORT1_MISSING;
        memset(out + 1, 0xFF, (size_t)(n - 1));
        plat_pad_rumble(port, 0);
        return n;
    }
    for (k = 1; k < n; k++)
        out[k] = pad_byte(p, k, in[k]);
    if (n > 1 && in[1] == 0x42)
        pad_vibrate(port, p);
    return n;
}

/* ---- memory card ---------------------------------------------------------------------------- */

static void card_open(void)
{
    char path[512];
    int err;
    snprintf(path, sizeof path, "%s/Mcd001.ps2", plat_save_dir());
    g_card.term = 0x55;
    g_card.f = fopen(path, "r+b");
    if (g_card.f) {
        LOG("memory card: opened %s", path);
        return;
    }
    err = errno;
    if (err != ENOENT) {
        LOG("memory card: cannot open %s (errno %d), slot 1 is empty", path, err);
        return;
    }
    /* a new card is erased flash: all ones; the game offers to format it */
    {
        static uint8_t blank[MC_RAW_PAGE * MC_ERASE_PAGES];
        uint32_t k;
        g_card.f = fopen(path, "w+b");
        if (!g_card.f) {
            LOG("memory card: cannot create %s (errno %d), slot 1 is empty", path, errno);
            return;
        }
        memset(blank, 0xFF, sizeof blank);
        for (k = 0; k < MC_SIZE; k += sizeof blank)
            fwrite(blank, 1, sizeof blank, g_card.f);
        fflush(g_card.f);
        LOG("memory card: created %s", path);
    }
}

uint64_t g_card_last_us;        /* last memory card read or write (the test autopilot waits on it) */

/* The card file in RAM, a block at a time: games poll the card all the time
 * (Burnout 3 reads its 40 KB directory and save ~3000 times a second at the
 * "load successful" prompt), and on the Wii every fseek/fread goes to the SD
 * card. Writes stay in the cache until an operation ends; formatting the
 * card is 8 MB and a flush per 128-byte chunk made it take minutes. */
#define MCC_BLOCK 8192u
#define MCC_N 16
static struct { uint32_t block, stamp; int valid, dirty; uint8_t data[MCC_BLOCK]; } g_mcc[MCC_N];
static uint32_t g_mcc_clock;

static int mcc_flush_block(int k)
{
    uint32_t block = g_mcc[k].block;
    uint32_t off = block * MCC_BLOCK;
    uint32_t len = off + MCC_BLOCK <= MC_SIZE ? MCC_BLOCK : MC_SIZE - off;
    if (!g_mcc[k].valid || !g_mcc[k].dirty || !g_card.f)
        return 0;
    if (off >= MC_SIZE || fseek(g_card.f, (long)off, SEEK_SET) ||
        fwrite(g_mcc[k].data, 1, len, g_card.f) != len) {
        LOG("memory card: write failed at %06X (%u bytes, errno %d)", off, len, errno);
        return 0;
    }
    g_mcc[k].dirty = 0;
    return 1;
}

/* A save is many 128-byte writes. Sync once an operation ends, so a console
 * switched off after "saved" keeps it; fflush alone does not reach NTFS's
 * MFT record or the USB device. */
static void card_sync(void)
{
    int k, failed = 0;
    if (!g_card.f || !g_card_dirty)
        return;
    for (k = 0; k < MCC_N; k++)
        if (g_mcc[k].valid && g_mcc[k].dirty && !mcc_flush_block(k))
            failed = 1;
    if (fflush(g_card.f) || fsync(fileno(g_card.f))) {
        LOG("memory card: sync failed (errno %d)", errno);
        failed = 1;
    }
    g_card_dirty = failed;
}

static uint8_t *mcc_block(uint32_t block, int *index)
{
    int k, victim = 0;
    for (k = 0; k < MCC_N; k++)
        if (g_mcc[k].valid && g_mcc[k].block == block) {
            g_mcc[k].stamp = ++g_mcc_clock;
            if (index)
                *index = k;
            return g_mcc[k].data;
        }
    for (k = 1; k < MCC_N; k++)
        if (!g_mcc[k].valid || (g_mcc[victim].valid && g_mcc[k].stamp < g_mcc[victim].stamp))
            victim = k;
    if (g_mcc[victim].valid && g_mcc[victim].dirty)
        mcc_flush_block(victim);
    memset(g_mcc[victim].data, 0xFF, MCC_BLOCK);
    if (!fseek(g_card.f, (long)(block * MCC_BLOCK), SEEK_SET))
        fread(g_mcc[victim].data, 1, block * MCC_BLOCK + MCC_BLOCK <= MC_SIZE ? MCC_BLOCK : MC_SIZE - block * MCC_BLOCK,
              g_card.f);
    g_mcc[victim].block = block;
    g_mcc[victim].valid = 1;
    g_mcc[victim].dirty = 0;
    g_mcc[victim].stamp = ++g_mcc_clock;
    if (index)
        *index = victim;
    return g_mcc[victim].data;
}

/* n bytes at the card offset: from the cache (copy out), or into it (write) */
static void mcc_access(uint32_t addr, uint8_t *buf, uint32_t n, int write)
{
    while (n) {
        uint32_t off = addr % MCC_BLOCK, len = MCC_BLOCK - off < n ? MCC_BLOCK - off : n;
        int k;
        uint8_t *b = mcc_block(addr / MCC_BLOCK, &k) + off;
        if (write) {
            memcpy(b, buf, len);
            g_mcc[k].dirty = 1;
            g_card_dirty = 1;
        } else {
            memcpy(buf, b, len);
        }
        addr += len;
        buf += len;
        n -= len;
    }
}

static void card_read(uint8_t *buf, uint32_t n)
{
    g_card_last_us = plat_time_us();
    memset(buf, 0xFF, n);
    if (g_card.addr + n <= MC_SIZE)
        mcc_access(g_card.addr, buf, n, 0);
    g_card.addr += n;
}

static void card_write(const uint8_t *buf, uint32_t n)
{
    g_card_last_us = plat_time_us();
    uint8_t cur[256];
    uint32_t k;
#if defined(PS2_DYNAMIC_IO)
    {
        extern uint32_t g_watch_val;
        for (k = 0; g_watch_val && k + 4 <= n; k++)
            if ((buf[k] | buf[k + 1] << 8 | buf[k + 2] << 16 | (uint32_t)buf[k + 3] << 24) == g_watch_val) {
                R5900 *c = iop_current_ctx();
                LOG("watch: card write at %06X+%u holds %08X", g_card.addr, k, g_watch_val);
                if (c)
                    rt_dump_stack(c, "watch IOP");
                {
                    static int dumped;
                    if (!dumped++) {
                        FILE *f = fopen("work/hit_chunk.bin", "wb");
                        if (f) { fwrite(buf, 1, n, f); fclose(f); }
                        f = fopen("work/hit_ee_ram.bin", "wb");
                        if (f) { fwrite(ps2_ram, 1, PS2_RAM_SIZE, f); fclose(f); }
                        f = fopen("work/hit_iop_ram.bin", "wb");
                        if (f) { fwrite(iop_ram, 1, IOP_RAM_SIZE, f); fclose(f); }
                    }
                }
            }
    }
#endif
    if (n <= sizeof cur && g_card.addr + n <= MC_SIZE) {
        mcc_access(g_card.addr, cur, n, 0);
        for (k = 0; k < n; k++)
            cur[k] &= buf[k];          /* flash: programming only clears bits */
        mcc_access(g_card.addr, cur, n, 1);
        if (!g_card_writes++)
            LOG("memory card: first write %u bytes at %06X", n, g_card.addr);
    }
    g_card.addr += n;
}

static void card_erase(void)
{
    static uint8_t ones[MC_RAW_PAGE * MC_ERASE_PAGES];
    if (g_card.addr + sizeof ones <= MC_SIZE) {
        memset(ones, 0xFF, sizeof ones);
        mcc_access(g_card.addr, ones, sizeof ones, 1);
        if (!g_card_erases++)
            LOG("memory card: first erase at %06X, %u bytes", g_card.addr, (unsigned)sizeof ones);
    }
}

/* answer padded with zeros to `len` bytes, ending in '+' and the terminator */
static int term2b(uint8_t *out, int n, int len)
{
    while (n < len - 2)
        out[n++] = 0x00;
    out[n++] = 0x2B;
    out[n++] = g_card.term;
    return n;
}

static int card_command(int port, const uint8_t *in, int nin, uint8_t *out)
{
    const uint8_t *a = in + 2;           /* arguments after 0x81 and the command */
    int n = 2, k;
    uint8_t x = 0;
    if (port && !g_card_slot2_logged) {
        g_card_slot2_logged = 1;
        LOG("memory card: slot 2 probed, only slot 1 is emulated");
    }
    if (g_trace > 1)
        LOG("memory card: cmd %02X, %d bytes, addr %06X, term %02X", nin > 1 ? in[1] : 0, nin, g_card.addr, g_card.term);
    if (port != 0 || !g_card.f) {
        s.recv1 = STAT_DISCONNECTED;
        memset(out, 0xFF, (size_t)nin);
        return nin;
    }
    s.recv1 = STAT_CONNECTED;
    out[0] = out[1] = 0x00;
    /* Sync at operation boundaries, not between the 128-byte chunks of one
     * transfer (0x42/0x43) or the MagicGate exchange (0xF0). */
    if (nin > 1 && in[1] != 0x42 && in[1] != 0x43 && in[1] != 0xF0)
        card_sync();
    switch (nin > 1 ? in[1] : 0) {
    case 0x11:                  /* probe */
    case 0x12:
    case 0x81:                  /* end of a read or write */
        return term2b(out, n, 4);
    case 0x21:                  /* set erase / write / read page: 4 address bytes and their xor */
    case 0x22:
    case 0x23: {
        uint32_t page = a[0] | a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24;
        if ((a[0] ^ a[1] ^ a[2] ^ a[3]) != a[4])
            LOG("memory card: bad page checksum");
        g_card.addr = MC_RAW_PAGE * page;
        return term2b(out, n, 9);
    }
    case 0x26: {                /* specs: page size, erase block, page count */
        static const uint8_t spec[8] = {MC_PAGE & 0xFF, MC_PAGE >> 8, MC_ERASE_PAGES, 0,
                                        MC_PAGES & 0xFF, (MC_PAGES >> 8) & 0xFF, 0, 0};
        out[n++] = 0x2B;
        for (k = 0; k < 8; k++) {
            out[n++] = spec[k];
            x ^= spec[k];
        }
        out[n++] = x;
        out[n++] = g_card.term;
        return n;
    }
    case 0x27:                  /* set terminator */
        g_card.term = a[0];
        out[n++] = 0x00;
        out[n++] = 0x2B;
        out[n++] = g_card.term;
        return n;
    case 0x28:                  /* get terminator */
        out[n++] = 0x2B;
        out[n++] = g_card.term;
        out[n++] = g_card.term;
        return n;
    case 0x42: {                /* write data: length, bytes */
        int len = a[0];
        out[n++] = 0x00;
        out[n++] = 0x2B;
        for (k = 0; k < len; k++) {
            x ^= a[1 + k];
            out[n++] = 0x00;
        }
        card_write(a + 1, (uint32_t)len);
        out[n++] = x;
        out[n++] = g_card.term;
        return n;
    }
    case 0x43: {                /* read data: length */
        int len = a[0];
        out[n++] = 0x00;
        out[n++] = 0x2B;
        card_read(out + n, (uint32_t)len);
        for (k = 0; k < len; k++)
            x ^= out[n + k];
        n += len;
        out[n++] = x;
        out[n++] = g_card.term;
        return n;
    }
    case 0x82:                  /* erase the block at the erase address */
        card_erase();
        card_sync();
        return term2b(out, n, 4);
    case 0xBF:
        return term2b(out, n, 5);
    case 0xF0:                  /* MagicGate authentication steps */
        switch (a[0]) {
        case 0x01: case 0x02: case 0x04: case 0x0F: case 0x11: case 0x13:
            out[n++] = 0x00;
            out[n++] = 0x2B;
            for (k = 0; k < 8; k++) {
                x ^= a[1 + k];
                out[n++] = 0x00;
            }
            out[n++] = x;
            out[n++] = g_card.term;
            return n;
        case 0x06: case 0x07: case 0x0B:
            return term2b(out, n, 14);
        default:
            return term2b(out, n, 5);
        }
    case 0xF3:
        g_card.term = 0x55;
        return term2b(out, n, 5);
    case 0xF7:
        return term2b(out, n, 5);
    default:
        LOG("memory card: unhandled command %02X", nin > 1 ? in[1] : 0);
        return n;
    }
}

/* ---- the controller ------------------------------------------------------------------------- */

static void out_push(const uint8_t *b, int n)
{
    if (s.out_r == s.out_w)
        s.out_r = s.out_w = 0;
    if (s.out_w + n > (int)sizeof s.out) {
        LOG("SIO2: output FIFO overflow");
        n = (int)sizeof s.out - s.out_w;
    }
    memcpy(s.out + s.out_w, b, (size_t)n);
    s.out_w += n;
}

static void run_command(void)
{
    static uint8_t resp[1024];
    int port = (int)(s.send3[s.pos] & 1), n;
    static uint32_t ports_seen;
    if (!(ports_seen & 1u << (s.send3[s.pos] & 3) * 8 << (s.in[0] == 0x81))) {
        ports_seen |= 1u << (s.send3[s.pos] & 3) * 8 << (s.in[0] == 0x81);
        LOG("SIO2: first %s command on port %u (SEND3 %08X)", s.in[0] == 0x81 ? "card" : s.in[0] == 0x01 ? "pad" : "other",
            s.send3[s.pos] & 3, s.send3[s.pos]);
    }
    switch (s.in[0]) {
    case 0x01:
        n = pad_command(port, s.in, s.nin, resp);
        break;
    case 0x81:
        n = card_command(port, s.in, s.nin, resp);
        break;
    case 0x21:                  /* multitap: not connected */
    default:
        if (g_trace > 1 && s.in[0] != 0x21)
            LOG("SIO2: command for unknown device %02X", s.in[0]);
        s.recv1 = STAT_DISCONNECTED;
        memset(resp, 0xFF, (size_t)s.nin);
        n = s.nin;
        break;
    }
    if (g_trace > 2) {
        char line[256];
        int k, m = snprintf(line, sizeof line, "SIO2 port %d:", port);
        for (k = 0; k < s.nin && k < 24; k++)
            m += snprintf(line + m, sizeof line - (size_t)m, " %02X/%02X", s.in[k], k < n ? resp[k] : 0);
        LOG("%s recv1 %X", line, s.recv1);
    }
    out_push(resp, n);
    if (s.dma_block && (s.out_w - s.out_r) % s.dma_block) {
        static const uint8_t zeros[1024];
        out_push(zeros, s.dma_block - (s.out_w - s.out_r) % s.dma_block);
    }
}

static void soft_reset(void)
{
    s.collecting = 0;
    s.pos = 0;
    s.len = 0;
    s.dma_block = 0;
    s.queue_done = 0;
    s.nin = 0;
    s.recv1 = 0;
}

static void sio2_write_byte(uint8_t b)
{
    if (!s.collecting) {
        if (s.pos >= 16) {
            s.queue_done = 1;
        } else {
            s.len = (int)((s.send3[s.pos] >> 8) & 0x3FF);
            if (!s.len)
                s.queue_done = 1;
        }
        s.collecting = 1;
        s.nin = 0;
    }
    if (s.queue_done)
        return;
    if (s.nin < (int)sizeof s.in)
        s.in[s.nin++] = b;
    if ((!s.dma_block && s.nin == s.len) || (s.dma_block && s.nin == s.dma_block)) {
        run_command();
        s.collecting = 0;
        s.pos++;
    }
}

static uint8_t sio2_read_byte(void)
{
    return s.out_r < s.out_w ? s.out[s.out_r++] : 0xFF;
}

/* DMA channel 11 (to the SIO2) and 12 (from it), started through dmacman */
void iop_sio2_dma(int ch, uint32_t addr, uint32_t block_words, uint32_t blocks)
{
    uint32_t k, n = block_words * 4 * blocks;
    if (ch == 11) {
        s.dma_block = (int)(block_words * 4);
        for (k = 0; k < n; k++)
            sio2_write_byte(iop_r8(addr + k));
    } else if (ch == 12) {
        for (k = 0; k < n; k++)
            iop_w8(addr + k, sio2_read_byte());
    }
}

uint32_t iop_sio2_read(uint32_t p)
{
    uint32_t r = p & 0xFF;
    if (r < 0x40)
        return s.send3[r >> 2];
    if (r < 0x60)
        return (r & 4) ? s.send2[(r - 0x40) >> 3] : s.send1[(r - 0x40) >> 3];
    switch (r) {
    case 0x64: return sio2_read_byte();
    case 0x68: return s.ctrl;
    case 0x6C: return s.recv1;
    case 0x70: return 0xF;
    case 0x74: return s.recv3;
    case 0x78: return s.unk78;
    case 0x7C: return s.unk7c;
    case 0x80: return s.istat;
    default:
        LOG("SIO2 read %08X", p);
        return 0;
    }
}

void iop_sio2_write(uint32_t p, uint32_t v)
{
    uint32_t r = p & 0xFF;
    if (r < 0x40) {
        s.send3[r >> 2] = v;
        if (r == 0)
            soft_reset();
        return;
    }
    if (r < 0x60) {
        if (r & 4)
            s.send2[(r - 0x40) >> 3] = v;
        else
            s.send1[(r - 0x40) >> 3] = v;
        return;
    }
    switch (r) {
    case 0x60:
        sio2_write_byte((uint8_t)v);
        break;
    case 0x68:
        /* the start (bit 0) and FIFO reset (bits 2-3) bits act once and read back as 0 */
        s.ctrl = v & ~0xDu;
        if ((v & 0xC) && !(v & 1))
            s.out_r = s.out_w = 0;
        if (v & 1) {                    /* start: the transfer is already done, signal it */
            if (!s.istat)
                iop_raise_irq(SIO2_IRQ);
            s.istat |= 1;
        }
        break;
    case 0x78: s.unk78 = v; break;
    case 0x7C: s.unk7c = v; break;
    case 0x80: s.istat &= ~v; break;
    default:
        LOG("SIO2 write %08X = %08X", p, v);
        break;
    }
}

void iop_sio2_init(void)
{
    memset(&s, 0, sizeof s);
    memset(g_pad, 0, sizeof g_pad);
    g_pad[0].mode = g_pad[1].mode = 0x41;
    g_pad[0].small_cfg = g_pad[0].large_cfg = g_pad[1].small_cfg = g_pad[1].large_cfg = 0xFF;
    s.ctrl = 0x3BC;
    s.recv1 = STAT_DISCONNECTED;
    card_open();
}

void iop_sio2_flush(void)
{
    card_sync();
    if (g_card.f) {
        fclose(g_card.f);
        g_card.f = NULL;
    }
}
