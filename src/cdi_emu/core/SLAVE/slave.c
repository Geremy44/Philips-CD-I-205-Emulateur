#include "slave.h"
#include <stddef.h>

void slave_attach(slave_t *s, slave_backend_t *be)
{
    s->be = be;
    s->irq_line = 0;
}

void slave_reset(slave_t *s)
{
    if (s->be)
        s->be->reset(s->be->ctx);
    s->irq_line = 0;
}

uint8_t slave_read8(slave_t *s, uint32_t addr)
{
    return s->be ? s->be->read8(s->be->ctx, addr) : 0xFF;
}

void slave_write8(slave_t *s, uint32_t addr, uint8_t val)
{
    if (s->be)
        s->be->write8(s->be->ctx, addr, val);
}

void slave_tick(slave_t *s, int master_cycles)
{
    if (s->be && s->be->tick)
        s->irq_line = s->be->tick(s->be->ctx, master_cycles);
}

void slave_push_input(slave_t *s, const uint8_t *report, int len)
{
    if (s->be && s->be->push_input)
        s->be->push_input(s->be->ctx, report, len);
}
