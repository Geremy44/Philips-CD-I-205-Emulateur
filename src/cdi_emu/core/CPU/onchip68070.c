/*
 * onchip68070.c - SCC68070 on-chip timer and interrupt priority registers
 * (see onchip68070.h for the datasheet references)
 */

#include "onchip68070.h"
#include <string.h>

void onchip68070_reset(onchip68070_t *o)
{
    memset(o, 0, sizeof(*o));      /* PICR/LIR and timer are cleared on reset */
}

int onchip68070_handles(uint32_t addr)
{
    return (addr >= ONCHIP_TIMER_BASE && addr <= ONCHIP_TIMER_END) ||
           addr == ONCHIP_PICR1 || addr == ONCHIP_PICR2;
}

uint8_t onchip68070_read8(onchip68070_t *o, uint32_t addr)
{
    switch (addr) {
    case ONCHIP_PICR1: return o->picr1;
    case ONCHIP_PICR2: return o->picr2;
    }
    switch (addr - ONCHIP_TIMER_BASE) {
    case 0x0: return o->tsr;
    case 0x1: return o->tcr;
    case 0x2: return (uint8_t)(o->rr >> 8);
    case 0x3: return (uint8_t)o->rr;
    case 0x4: return (uint8_t)(o->t0 >> 8);
    case 0x5: return (uint8_t)o->t0;
    case 0x6: return (uint8_t)(o->t1 >> 8);
    case 0x7: return (uint8_t)o->t1;
    case 0x8: return (uint8_t)(o->t2 >> 8);
    case 0x9: return (uint8_t)o->t2;
    }
    return 0xFF;
}

static inline void set_hi(uint16_t *r, uint8_t v) { *r = (uint16_t)((*r & 0x00FF) | (v << 8)); }
static inline void set_lo(uint16_t *r, uint8_t v) { *r = (uint16_t)((*r & 0xFF00) | v); }

void onchip68070_write8(onchip68070_t *o, uint32_t addr, uint8_t v)
{
    switch (addr) {
    case ONCHIP_PICR1: o->picr1 = v; return;
    case ONCHIP_PICR2: o->picr2 = v; return;
    }
    switch (addr - ONCHIP_TIMER_BASE) {
    case 0x0: o->tsr &= (uint8_t)~v; break;      /* write 1 to clear */
    case 0x1: o->tcr = v;            break;
    case 0x2: set_hi(&o->rr, v);     break;
    case 0x3: set_lo(&o->rr, v);     break;
    case 0x4: set_hi(&o->t0, v);     break;
    case 0x5: set_lo(&o->t0, v);     break;
    case 0x6: set_hi(&o->t1, v);     break;
    case 0x7: set_lo(&o->t1, v);     break;
    case 0x8: set_hi(&o->t2, v);     break;
    case 0x9: set_lo(&o->t2, v);     break;
    }
}

void onchip68070_tick(onchip68070_t *o, int cpu_cycles)
{
    if (cpu_cycles <= 0) return;
    o->prescale += (uint32_t)cpu_cycles;

    while (o->prescale >= TIMER_PRESCALE) {
        o->prescale -= TIMER_PRESCALE;

        if (o->t0 == 0xFFFF) {                    /* overflow: reload */
            o->t0 = o->rr;
            o->tsr |= TSR_T0_OV;
        } else {
            o->t0++;
        }

        /* Match mode (TCR mode = 01): T0 == T1/T2 sets MA */
        if (((o->tcr >> 4) & 3) == 1 && o->t0 == o->t1) o->tsr |= TSR_T1_MA;
        if (( o->tcr       & 3) == 1 && o->t0 == o->t2) o->tsr |= TSR_T2_MA;
    }
}

int onchip68070_irq(const onchip68070_t *o, int *vector)
{
    int level = 0;
    if (o->tsr & 0xFE) {
        int tl = o->picr1 & 7;                    /* timer priority level */
        if (tl > level) level = tl;
    }
    /* TODO: UART RX/TX (PICR2) and I2C interrupts */
    if (vector) *vector = 56 + level;             /* on-chip autovector 57..63 */
    return level;
}
