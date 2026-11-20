/*
 * onchip68070.h - SCC68070 on-chip timer and interrupt priority registers
 *
 * Datasheet SCC68070 (Nov. 1990):
 *   - Table 4  : on-chip address map
 *   - Table 24 : timer registers at $80002020..$80002029
 *   - Fig.42/43: TCR / TSR
 *   - PICR1 ($80002045): bits 2-0 = timer interrupt level
 *   - PICR2 ($80002047): bits 6-4 = UART RX level, bits 2-0 = UART TX level
 *   - Table 1  : on-chip interrupts use autovectors 57..63 (level 1..7)
 *
 * The reference timer T0 increments every 96 CPU clocks; on overflow
 * ($FFFF -> 0) it is reloaded from RR and TSR.OV is set. TSR bits stay set
 * until software writes a 1 to them, and request an interrupt at the
 * PICR1 level while set.
 */

#ifndef ONCHIP68070_H
#define ONCHIP68070_H

#include <stdint.h>

#define ONCHIP_TIMER_BASE   0x80002020u
#define ONCHIP_TIMER_END    0x80002029u
#define ONCHIP_PICR1        0x80002045u
#define ONCHIP_PICR2        0x80002047u

#define TIMER_PRESCALE      96          /* CPU clocks per T0 increment */

/* TSR bits */
#define TSR_T0_OV   0x80
#define TSR_T1_MA   0x40
#define TSR_T1_CAP  0x20
#define TSR_T1_OV   0x10
#define TSR_T2_MA   0x08
#define TSR_T2_CAP  0x04
#define TSR_T2_OV   0x02

typedef struct onchip68070 {
    uint8_t  tsr, tcr;
    uint16_t rr, t0, t1, t2;
    uint32_t prescale;     /* CPU clocks accumulated toward the next T0 tick */
    uint8_t  picr1, picr2;
} onchip68070_t;

void    onchip68070_reset(onchip68070_t *o);
int     onchip68070_handles(uint32_t addr);
uint8_t onchip68070_read8 (onchip68070_t *o, uint32_t addr);
void    onchip68070_write8(onchip68070_t *o, uint32_t addr, uint8_t v);
void    onchip68070_tick  (onchip68070_t *o, int cpu_cycles);

/* Highest pending on-chip interrupt: returns its level (0 = none) and
 * stores the autovector number (56 + level) in *vector. */
int     onchip68070_irq(const onchip68070_t *o, int *vector);

#endif /* ONCHIP68070_H */
