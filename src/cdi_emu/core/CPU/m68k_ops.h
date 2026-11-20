/*
 * m68k_ops.h - M68000 instruction decoder and executor
 *
 * Declares the main instruction dispatch function and status
 * register flag bit masks used throughout the opcode implementations.
 */

#ifndef M68K_OPS_H
#define M68K_OPS_H

#include "m68k.h"

/* Status Register (SR) flag bit masks - lower byte (CCR) */
#define SR_C   0x0001   /* Carry    */
#define SR_V   0x0002   /* Overflow */
#define SR_Z   0x0004   /* Zero     */
#define SR_N   0x0008   /* Negative */
#define SR_X   0x0010   /* Extend  */

/* Status Register (SR) system byte flags */
#define SR_S   0x2000   /* Supervisor mode */
#define SR_T   0x8000   /* Trace mode      */

/* Initial SR value after reset: supervisor mode, interrupt mask = 7 */
#define SR_IMP 0x2700

/* Main opcode decoder - called by m68k_step() for each instruction */
void m68k_step_ops(m68k_t *cpu);

#endif /* M68K_OPS_H */
