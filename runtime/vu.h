/* Vector units in micro mode. */
#ifndef VU_H
#define VU_H

#include "rt.h"

/* run unit 0/1 from addr (bytes into micro memory), or continue at TPC when addr = ~0 */
void vu_run(int unit, uint32_t addr);
uint32_t vu_vi(int unit, int k);        /* an integer register, as it is now */
const float *vu_vf(int unit, int k);    /* a float register */
void vu_micro_written(int unit);
/* the same for bytes [off, off + n) of the unit's micro memory alone (the lookups of programs that do not lie there
 * stand) */
void vu_micro_written_at(int unit, uint32_t off, uint32_t n);

uint32_t vif_top(int unit);
uint32_t vif_itop(int unit);

#endif
