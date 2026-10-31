/*
 * Clock-read counters for the game's world walker and its callees (autopilot "wstat 9"), built only with -DWTIME
 * (make -f Makefile.wii OUT=build-wiit XDEFS=-DWTIME, the recompiler run with B3_WTIME=1 so that config/burnout3.py
 * overrides the functions with the wrappers of wtime.c). A sampling profile put small functions of the world path at
 * several times what their clock reads say (2026-10-17, f_0026AF50), so the pieces are timed directly.
 *
 * A function's time is its clock reads' difference less the "foreign" time inside it: the DMA kicks of the VIF and the
 * GIF (the frame's chain is drawn inside the texture bind's kick) and the kernel's polls (interrupt handlers, sound,
 * thread switches). What is left is the game's own code and the natives it calls. Rows nest: the walker contains the
 * others.
 */
#ifndef WTIME_H
#define WTIME_H
#include <stdint.h>

#ifdef WTIME
extern uint64_t g_wt_foreign, g_wt_t0;
extern int g_wt_depth;
uint64_t plat_ticks(void);
#define WT_ENTER() do { if (g_wt_depth++ == 0) g_wt_t0 = plat_ticks(); } while (0)
#define WT_LEAVE() do { if (--g_wt_depth == 0) g_wt_foreign += plat_ticks() - g_wt_t0; } while (0)

/* the rows: functions wrapped by config OVERRIDES (hle_wt_XXXXXXXX) and three timed inside existing natives */
#define WT_LIST(X) \
    X(0021C780, walker)    X(0021D080, chunkflags) X(0026B8E0, begin)     X(0026B7B0, end)      X(0026BAA0, viewpkt) \
    X(0026AE30, secrec)    X(0026B2B0, secend)     X(0026B2F0, groupset)  X(0026B1C0, copy)     X(00221A70, texbind) \
    X(001F3080, dmapkt)    X(0026A7E0, props7e0)   X(0026A610, propsvis)  X(0026A1C0, props1c0) X(00332D00, statlists) \
    X(0021D770, statsec)   X(0021D550, d550)       X(0026A240, a240) \
    X(001B8F00, phys)      X(00273BF0, collbf0)    X(00273400, coll400)   X(00274C90, collc90)  X(00274AF0, collaf0) \
    X(003BE9B0, colle9b0)  X(003BF720, collf720)   X(003BEF60, collef60)
enum {
#define X(a, nm) WT_##nm,
    WT_LIST(X)
#undef X
    WT_batch, WT_instview, WT_texsend, WT_vis138, WT_row920, WT_coll070, WT_clut, WT_xfer, WT_lazyfin, WT_N
};
typedef struct { uint64_t t0, f0; } WtMark;
WtMark wt_mark(void);
WtMark wt_mark_in(void);                /* also inside the chain's DMA kick (inclusive: nothing taken off) */
void wt_done_in(int row, WtMark m);
void wt_done(int row, WtMark m);
void wt_report(uint32_t frames);
#define WT_MARK(m_) const WtMark m_ = wt_mark()
#define WT_MARK_IN(m_) const WtMark m_ = wt_mark_in()
#define WT_DONE_IN(row_, m_) wt_done_in(WT_##row_, m_)
#define WT_DONE(row_, m_) wt_done(WT_##row_, m_)
#define WT_REPORT(f_) wt_report(f_)
#else
#define WT_ENTER() ((void)0)
#define WT_LEAVE() ((void)0)
#define WT_MARK(m_) ((void)0)
#define WT_MARK_IN(m_) ((void)0)
#define WT_DONE_IN(row_, m_) ((void)0)
#define WT_DONE(row_, m_) ((void)0)
#define WT_REPORT(f_) ((void)0)
#endif

#endif
