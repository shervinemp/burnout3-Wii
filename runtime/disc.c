/*
 * The game disc: an ISO9660 image read by sector (cdvdman) or by file
 * (ioman, the executable). Without an image, files come from an extracted
 * copy of the disc through plat_open and sector reads fail.
 */
#include "rt.h"

#include <string.h>
#include <ctype.h>

#define SECTOR 2048u
#define MAX_ENTRIES 4096

typedef struct DiscEntry {
    char path[96];          /* upper case, '/' separated, no ";1" */
    uint32_t lsn, size;
} DiscEntry;

struct DiscFile {
    PlatFile *pf;           /* extracted file, when there is no image */
    uint32_t lsn, size;
};

static PlatFile *g_image;
static DiscEntry *g_entries;
static int g_nentries;
static uint32_t g_sectors;

/* the ISO file a sector belongs to, for "trace 2" */
static const char *lsn_path(uint32_t lsn, uint32_t *off)
{
    int i;
    if (!g_entries)
        return NULL;
    for (i = 0; i < g_nentries; i++) {
        const uint32_t sectors = (g_entries[i].size + SECTOR - 1) / SECTOR;
        if (lsn >= g_entries[i].lsn && lsn < g_entries[i].lsn + sectors) {
            *off = (lsn - g_entries[i].lsn) * SECTOR;
            return g_entries[i].path;
        }
    }
    return NULL;
}

static void trace_read(uint32_t lsn, uint32_t count)
{
    if (g_trace > 1) {
        uint32_t off = 0;
        const char *path = lsn_path(lsn, &off);
        if (path)
            LOG("disc: read lsn %u x%u -> %s+%u", lsn, count, path, off);
        else
            LOG("disc: read lsn %u x%u -> no file", lsn, count);
    }
}

int disc_read_sectors(uint32_t lsn, uint32_t count, void *buf)
{
    trace_read(lsn, count);
    int64_t want = (int64_t)count * SECTOR;
    if (!g_image)
        return -1;
    if (plat_read(g_image, buf, want, (int64_t)lsn * SECTOR) != want) {
        LOG("disc read past the end: lsn %u count %u", lsn, count);
        return -1;
    }
    return 0;
}

int disc_read_sectors_guest(uint32_t lsn, uint32_t count, void *buf)
{
    int64_t want = (int64_t)count * SECTOR;
    trace_read(lsn, count);
    if (!g_image)
        return -1;
    if (plat_read_guest(g_image, buf, want, (int64_t)lsn * SECTOR, kern_can_wait() ? kern_wait_host : NULL) != want) {
        LOG("disc read past the end: lsn %u count %u", lsn, count);
        return -1;
    }
    return 0;
}

uint32_t disc_sector_count(void)
{
    return g_sectors;
}

static void walk_dir(uint32_t lsn, uint32_t size, const char *prefix, int depth)
{
    uint8_t *data;
    uint32_t pos = 0, n = (size + SECTOR - 1) / SECTOR;
    if (depth > 8)
        return;
    data = malloc((size_t)n * SECTOR);
    if (disc_read_sectors(lsn, n, data)) {
        free(data);
        return;
    }
    while (pos < n * SECTOR) {
        uint8_t len = data[pos];
        uint32_t ext, esize, k;
        uint8_t flags, nlen;
        char name[64], path[96];
        if (!len) {
            pos = (pos / SECTOR + 1) * SECTOR;
            continue;
        }
        ext = data[pos + 2] | data[pos + 3] << 8 | data[pos + 4] << 16 | (uint32_t)data[pos + 5] << 24;
        esize = data[pos + 10] | data[pos + 11] << 8 | data[pos + 12] << 16 | (uint32_t)data[pos + 13] << 24;
        flags = data[pos + 25];
        nlen = data[pos + 32];
        for (k = 0; k < nlen && k < sizeof name - 1 && data[pos + 33 + k] != ';'; k++)
            name[k] = (char)toupper(data[pos + 33 + k]);
        name[k] = 0;
        pos += len;
        if (nlen == 1 && (data[pos - len + 33] == 0 || data[pos - len + 33] == 1))
            continue;
        snprintf(path, sizeof path, "%s%s", prefix, name);
        if (flags & 2) {
            char sub[96];
            snprintf(sub, sizeof sub, "%s/", path);
            walk_dir(ext, esize, sub, depth + 1);
        } else if (g_nentries < MAX_ENTRIES) {
            DiscEntry *e = &g_entries[g_nentries++];
            snprintf(e->path, sizeof e->path, "%s", path);
            e->lsn = ext;
            e->size = esize;
        }
    }
    free(data);
}

void disc_init(void)
{
    uint8_t pvd[SECTOR];
    uint32_t root_lsn, root_size;
    g_image = plat_open_disc_image();
    if (!g_image) {
        LOG("no disc image: reading extracted files, CD sector reads will fail");
        return;
    }
    g_sectors = (uint32_t)(plat_size(g_image) / SECTOR);
    if (disc_read_sectors(16, 1, pvd) || memcmp(pvd + 1, "CD001", 5))
        FATAL("disc image is not ISO9660");
    root_lsn = pvd[158] | pvd[159] << 8 | pvd[160] << 16 | (uint32_t)pvd[161] << 24;
    root_size = pvd[166] | pvd[167] << 8 | pvd[168] << 16 | (uint32_t)pvd[169] << 24;
    g_entries = calloc(MAX_ENTRIES, sizeof *g_entries);
    walk_dir(root_lsn, root_size, "", 0);
    LOG("disc image: %u sectors, %d files", g_sectors, g_nentries);
}

static const DiscEntry *find_entry(const char *path)
{
    char want[96];
    size_t n = 0;
    int i;
    while (*path == '/' || *path == '\\')
        path++;
    for (; *path && *path != ';' && n + 1 < sizeof want; path++)
        want[n++] = *path == '\\' ? '/' : (char)toupper((unsigned char)*path);
    want[n] = 0;
    for (i = 0; i < g_nentries; i++)
        if (!strcmp(g_entries[i].path, want))
            return &g_entries[i];
    return NULL;
}

int disc_find(const char *path, uint32_t *lsn, uint32_t *size)
{
    const DiscEntry *e = find_entry(path);
    if (!e)
        return 0;
    *lsn = e->lsn;
    *size = e->size;
    return 1;
}

DiscFile *disc_open(const char *path)
{
    DiscFile *f;
    if (g_image) {
        const DiscEntry *e = find_entry(path);
        if (!e)
            return NULL;
        f = calloc(1, sizeof *f);
        f->lsn = e->lsn;
        f->size = e->size;
        return f;
    } else {
        PlatFile *pf = plat_open(path);
        if (!pf)
            return NULL;
        f = calloc(1, sizeof *f);
        f->pf = pf;
        f->size = (uint32_t)plat_size(pf);
        return f;
    }
}

void disc_close(DiscFile *f)
{
    if (f) {
        if (f->pf)
            plat_close(f->pf);
        free(f);
    }
}

uint32_t disc_size(DiscFile *f)
{
    return f->size;
}

/* disc_read for a guest thread that may wait (see disc_read_sectors_guest) */
int64_t disc_read_guest(DiscFile *f, void *buf, int64_t size, int64_t offset)
{
    void (*wait)(void) = kern_can_wait() ? kern_wait_host : NULL;
    if (offset >= f->size)
        return 0;
    if (offset + size > f->size)
        size = f->size - offset;
    if (f->pf)
        return plat_read(f->pf, buf, size, offset);
    return plat_read_guest(g_image, buf, size, (int64_t)f->lsn * SECTOR + offset, wait);
}

int64_t disc_read(DiscFile *f, void *buf, int64_t size, int64_t offset)
{
    if (offset >= f->size)
        return 0;
    if (offset + size > f->size)
        size = f->size - offset;
    if (f->pf)
        return plat_read(f->pf, buf, size, offset);
    return plat_read(g_image, buf, size, (int64_t)f->lsn * SECTOR + offset);
}
