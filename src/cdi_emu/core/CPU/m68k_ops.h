#ifndef M68K_OPS_H
#define M68K_OPS_H

#include "m68k.h"

/* SR flag bit masks */
#define SR_C   0x0001   /* Carry */
#define SR_V   0x0002   /* Overflow */
#define SR_Z   0x0004   /* Zero */
#define SR_N   0x0008   /* Negative */
#define SR_X   0x0010   /* Extend */
#define SR_S   0x2000   /* Supervisor */
#define SR_T   0x8000   /* Trace */
#define SR_IMP 0x2700   /* Initial SR mask */

/* Opcode decoder / executor */
void m68k_step_ops(m68k_t *cpu);

#endif /* M68K_OPS_H */
