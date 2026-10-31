/* chd2iso: write the 2048-byte user data sectors of a CHD as an ISO.
 *
 * Handles DVD CHDs (chdman createdvd, 2048-byte units) and single-data-track
 * CD CHDs (chdman createcd, 2448-byte frames = 2352 sector + 96 subcode),
 * which is how many PS2 DVD images are distributed.
 *
 * libchdr is BSD-3-Clause and is not vendored here; see THIRD_PARTY_NOTICES.md.
 *
 * Usage: chd2iso in.chd out.iso
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>

#define CD_FRAME 2448

/* Offset of the 2048 bytes of user data inside a 2352-byte frame. */
static int user_data_offset(const char *type)
{
    if (!strcmp(type, "MODE1") || !strcmp(type, "MODE1/2048") || !strcmp(type, "MODE2_FORM1"))
        return 0;   /* cooked: stored without the sync/header */
    if (!strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE1/2352"))
        return 16;  /* 12 sync + 4 header */
    if (!strcmp(type, "MODE2_RAW") || !strcmp(type, "MODE2/2352"))
        return 24;  /* XA form 1: + 8 subheader */
    return -1;
}

int main(int argc, char **argv)
{
    chd_file *chd;
    const chd_header *hdr;
    unsigned char *buf;
    FILE *out;
    unsigned int hunk, frames_per_hunk, total_frames, frame_index = 0;
    char meta[512], type[64] = "";
    int track = 0, frames = 0, offset;
    chd_error err;

    if (argc != 3) {
        fprintf(stderr, "usage: chd2iso in.chd out.iso\n");
        return 1;
    }
    err = chd_open(argv[1], CHD_OPEN_READ, NULL, &chd);
    if (err != CHDERR_NONE) {
        fprintf(stderr, "chd_open: %s\n", chd_error_string(err));
        return 1;
    }
    hdr = chd_get_header(chd);
    fprintf(stderr, "version %u, hunkbytes %u, unitbytes %u, totalhunks %u, logical %llu\n",
            hdr->version, hdr->hunkbytes, hdr->unitbytes, hdr->totalhunks,
            (unsigned long long)hdr->logicalbytes);
    buf = malloc(hdr->hunkbytes);
    out = fopen(argv[2], "wb");
    if (!buf || !out) {
        fprintf(stderr, "cannot open output\n");
        return 1;
    }

    if (hdr->unitbytes == 2048) {
        for (hunk = 0; hunk < hdr->totalhunks; hunk++) {
            unsigned long long remain = hdr->logicalbytes - (unsigned long long)hunk * hdr->hunkbytes;
            size_t n = remain < hdr->hunkbytes ? (size_t)remain : hdr->hunkbytes;
            if ((err = chd_read(chd, hunk, buf)) != CHDERR_NONE) {
                fprintf(stderr, "chd_read hunk %u: %s\n", hunk, chd_error_string(err));
                return 1;
            }
            fwrite(buf, 1, n, out);
        }
        fclose(out);
        return 0;
    }

    if (hdr->unitbytes != CD_FRAME) {
        fprintf(stderr, "unsupported unit size %u\n", hdr->unitbytes);
        return 1;
    }
    if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta), NULL, NULL, NULL) == CHDERR_NONE)
        sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%*s FRAMES:%d", &track, type, &frames);
    else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof(meta), NULL, NULL, NULL) == CHDERR_NONE)
        sscanf(meta, "TRACK:%d TYPE:%63s SUBTYPE:%*s FRAMES:%d", &track, type, &frames);
    else {
        fprintf(stderr, "no CD track metadata\n");
        return 1;
    }
    if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, 1, meta, sizeof(meta), NULL, NULL, NULL) == CHDERR_NONE ||
        chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, 1, meta, sizeof(meta), NULL, NULL, NULL) == CHDERR_NONE)
        fprintf(stderr, "warning: more than one track; writing track 1 only\n");
    offset = user_data_offset(type);
    fprintf(stderr, "track %d type %s frames %d (user data at +%d)\n", track, type, frames, offset);
    if (offset < 0) {
        fprintf(stderr, "unsupported track type\n");
        return 1;
    }

    frames_per_hunk = hdr->hunkbytes / CD_FRAME;
    total_frames = (unsigned int)frames;
    for (hunk = 0; hunk < hdr->totalhunks && frame_index < total_frames; hunk++) {
        unsigned int f;
        if ((err = chd_read(chd, hunk, buf)) != CHDERR_NONE) {
            fprintf(stderr, "chd_read hunk %u: %s\n", hunk, chd_error_string(err));
            return 1;
        }
        for (f = 0; f < frames_per_hunk && frame_index < total_frames; f++, frame_index++) {
            if (fwrite(buf + f * CD_FRAME + offset, 1, 2048, out) != 2048) {
                fprintf(stderr, "write failed\n");
                return 1;
            }
        }
    }
    fclose(out);
    chd_close(chd);
    fprintf(stderr, "wrote %u sectors\n", frame_index);
    return 0;
}
