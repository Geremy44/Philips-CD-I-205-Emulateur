/*
 * slave.c - Slave co-processor frontend
 *
 * The slave co-processor (a 68HC05 microcontroller) manages CD-ROM
 * servo control, input devices, and NVRAM in the CD-i. This frontend
 * dispatches all operations to the active backend (HLE or LLE),
 * providing a uniform interface regardless of the emulation approach.
 */

#include "slave.h"
#include <stddef.h>

/* Attach a backend to the slave frontend */
void slave_attach(slave_t *s, slave_backend_t *be)
{
    s->be = be;
    s->irq_line = 0;
}

/* Reset the slave through its backend */
void slave_reset(slave_t *s)
{
    if (s->be)
        s->be->reset(s->be->ctx);
    s->irq_line = 0;
}

/* Read a byte from the slave's register space */
uint8_t slave_read8(slave_t *s, uint32_t addr)
{
    return s->be ? s->be->read8(s->be->ctx, addr) : 0xFF;
}

/* Write a byte to the slave's register space */
void slave_write8(slave_t *s, uint32_t addr, uint8_t val)
{
    if (s->be)
        s->be->write8(s->be->ctx, addr, val);
}

/* Advance slave timing by the given master clock cycles */
void slave_tick(slave_t *s, int master_cycles)
{
    if (s->be && s->be->tick)
        s->irq_line = s->be->tick(s->be->ctx, master_cycles);
}

/* Push input data (e.g. from a controller or keyboard) to the slave */
void slave_push_input(slave_t *s, const uint8_t *report, int len)
{
    if (s->be && s->be->push_input)
        s->be->push_input(s->be->ctx, report, len);
}
