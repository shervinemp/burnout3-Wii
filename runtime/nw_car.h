/* Native car shader (runtime/nw_car.c) */
#ifndef NW_CAR_H
#define NW_CAR_H

#include <stdint.h>

#define NC_CODE 0x0046BB80u     /* VU1 microcode chain */
#define NC_MAX 256

#define NC_BATCHES 64

/* one batch of render data in guest memory */
typedef struct NcBlock {
    uint32_t n, pos, nrm, uv, uvn, uvstep;
    uint32_t row[4];                    /* ROW for the normals */
    uint32_t start;                     /* its first VIFcode */
    uint32_t cl, wl, mode, srow[4];     /* VIF registers there */
    int mscnt;                          /* the packet starts the microprogram after it */
} NcBlock;

/* a packet of render data: its batches, and the VIF registers it leaves */
typedef struct NcPacket {
    int nb;
    NcBlock b[NC_BATCHES];
    uint32_t cl, wl, mode, row[4];
} NcPacket;

/* a batch as the microprogram finds it in VU memory */
typedef struct NcIn {
    uint32_t n;
    float pos[NC_MAX + 4][3];
    uint32_t posw[NC_MAX + 4];          /* w bits: 0x8000 = no triangle ends here */
    float nrm[NC_MAX + 4][3];           /* slot 2: ROW plus the byte, as floats */
    uint32_t q1[NC_MAX + 4][4];         /* slot 1: UV pairs (even vertices), skin data (odd) */
    int q1_odd;                         /* odd vertices have slot 1 data */
} NcIn;

typedef struct NcOut {
    uint32_t n;
    int env;                            /* pass 2: sphere map */
    float pos[NC_MAX][3];               /* object space, deformed */
    uint8_t adc[NC_MAX];
    uint8_t col[NC_MAX][4];             /* pass 1 colour, GS values */
    float uv0[NC_MAX][2];               /* pass 1 UVs */
    float uv1[NC_MAX][2];               /* pass 2 UVs */
    uint8_t col1[4];                    /* pass 2 colour */
} NcOut;

/* VIFcodes from a to end; p's VIF registers are the state on entry */
int nc_packet_parse(uint32_t a, uint32_t end, NcPacket *p);
/* the same, answered from the last parse of the packet (runtime/hle_render.c) */
int nc_packet_cached(uint32_t a, uint32_t end, NcPacket *p);
int nc_packet_ref(uint32_t a, uint32_t end, NcPacket *p, const NcBlock **b);  /* in place (nw_packet_ref) */
void nc_load(const NcBlock *b, NcIn *in);
/* vu: VU1 data memory; 0 when the batch needs something not handled */
int nc_shade(const uint32_t *vu, NcIn *in, NcOut *out);
/* the same from guest memory for undeformed batches: GX colours, pass 2 UVs
 * (as bytes, 0.8, when uv8 is given: the map is 64 pixels wide; else floats);
 * 0 deformed, 1 done, 2 done with the sphere map */
int nc_shade_direct(const uint32_t *vu, const NcBlock *b, uint32_t *col, float (*uv1)[2], uint8_t (*uv8)[2]);
/* the same with the batch's normals from a copy in host order (nrm8: the guest's bytes xor 0x80, four a vertex;
 * the car's display list keeps it): when the rows make every normal byte k into k - 128 exactly, the loop reads
 * them as signed bytes; otherwise (or nrm8 NULL) as nc_shade_direct */
int nc_shade_direct8(const uint32_t *vu, const NcBlock *b, uint32_t *col, float (*uv1)[2], uint8_t (*uv8)[2],
                     const int8_t *nrm8);
/* nc_load and nc_shade in one pass over the render data, for a batch with
 * crash deformation (a crashed car is all such batches: this is the car
 * path of the slowest frames). A vertex without weights is not blended.
 * 0 when nc_shade would refuse it. */
int nc_shade_skin(const uint32_t *vu, const NcBlock *b, NcOut *out);
void nc_matrix(const uint32_t *vu, float m[3][4]);

#endif
