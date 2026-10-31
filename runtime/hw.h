/* EE/GS hardware register map and the graphics pipeline entry points. */
#ifndef HW_H
#define HW_H

#include "rt.h"

#define T0_COUNT   0x10000000u
#define T0_MODE    0x10000010u
#define GIF_CTRL   0x10003000u
#define GIF_MODE   0x10003010u
#define GIF_STAT   0x10003020u
#define VIF0_STAT  0x10003800u
#define VIF0_FBRST 0x10003810u
#define VIF1_STAT  0x10003C00u
#define VIF1_FBRST 0x10003C10u
#define VIF0_FIFO  0x10004000u
#define VIF1_FIFO  0x10005000u
#define GIF_FIFO   0x10006000u
#define D_CTRL     0x1000E000u
#define D_STAT     0x1000E010u
#define D_PCR      0x1000E020u
#define D_SQWC     0x1000E030u
#define D_RBSR     0x1000E040u
#define D_RBOR     0x1000E050u
#define INTC_STAT  0x1000F000u
#define INTC_MASK  0x1000F010u
#define D_ENABLER  0x1000F520u
#define D_ENABLEW  0x1000F590u

#define GS_PMODE    0x12000000u
#define GS_SMODE1   0x12000010u
#define GS_SMODE2   0x12000020u
#define GS_DISPFB1  0x12000070u
#define GS_DISPLAY1 0x12000080u
#define GS_DISPFB2  0x12000090u
#define GS_DISPLAY2 0x120000A0u
#define GS_BGCOLOR  0x120000E0u
#define GS_CSR      0x12001000u
#define GS_IMR      0x12001010u
#define GS_BUSDIR   0x12001040u
#define GS_SIGLBLID 0x12001080u

#define HSYNC_NS_DIV 63556u

extern uint32_t g_vu_mem[0x4000];   /* host-order words; bytes 0x0000 VU0 micro, 0x4000 VU0 data, 0x8000 VU1 micro, 0xC000 VU1 data */

uint32_t hw_reg(uint32_t a);
void hw_set_reg(uint32_t a, uint32_t v);
uint64_t gs_priv_reg(uint32_t a);
void gs_set_csr_bits(uint32_t bits);

/* DMA consumers (data is in guest memory; transfers complete synchronously) */
int dma_gif(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr);   /* 0: deferred */
void hw_dma_complete(int ch);
void dma_vif1(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr);
void dma_vif0(uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr);
void dma_spr(int ch, uint32_t chcr, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t sadr);
void gif_fifo_write(u128 v);
void vif1_fifo_write(u128 v);
void vif0_fifo_write(u128 v);

/* GS */
void gs_init(void);
void gs_reset(void);
void gs_vsync(void);
extern uint32_t g_gs_flips;     /* DISPFB changes: game frames shown */
extern uint32_t g_gs_upload_bytes, g_gs_uploads;  /* host->GS image transfers */
extern uint32_t g_vif1_chains;  /* VIF1 DMA chains started: one per Burnout 3 frame */
void vif_init(void);
void vif_mpg_flush(void);       /* VU1 microprograms noted by the VIF, not copied yet: write them out */

#endif
