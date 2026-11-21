/*
 * scc68070_timing.h - SCC68070 instruction durations (datasheet tables 7-19)
 */
#ifndef SCC68070_TIMING_H
#define SCC68070_TIMING_H

#include "m68k.h"

/* Clock periods (15 MHz) of the instruction at cpu->pc, computed before
 * it executes. */
int scc68070_cycles(const m68k_t *cpu);

/* Interrupt acknowledge + exception processing (table 19) */
int scc68070_interrupt_cycles(void);

#endif
