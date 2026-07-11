#include "m68k.h"
#include "m68k_ops.h"
#include "../bus.h"
#include <string.h>
#include <stdio.h>

void m68k_init(m68k_t *cpu) {
    memset(cpu, 0, sizeof(*cpu));
}

void m68k_set_bus(m68k_t *cpu, bus_t *bus) {
    cpu->bus = bus;
}

void m68k_reset(m68k_t *cpu) {
    bus_t *saved = cpu->bus;
    memset(cpu, 0, sizeof(*cpu));
    cpu->bus = saved;
    cpu->sr  = 0x2700;                 /* superviseur, IRQ masquées */

    /* ✅ vrais vecteurs reset lus en ROM (mappée à 0x180000) */
    cpu->a[7] = ((uint32_t)bus_read16(cpu->bus, 0x180000) << 16)
              |  (uint32_t)bus_read16(cpu->bus, 0x180002);
    cpu->pc   = ((uint32_t)bus_read16(cpu->bus, 0x180004) << 16)
              |  (uint32_t)bus_read16(cpu->bus, 0x180006);
    cpu->stopped = 0;
}

uint8_t m68k_read8(m68k_t *cpu, uint32_t addr) {
    return bus_read8(cpu->bus, addr);
}

uint16_t m68k_read16(m68k_t *cpu, uint32_t addr) {
    return bus_read16(cpu->bus, addr);
}

uint32_t m68k_read32(m68k_t *cpu, uint32_t addr) {
    uint32_t hi = bus_read16(cpu->bus, addr);
    uint32_t lo = bus_read16(cpu->bus, addr + 2);
    return (hi << 16) | lo;
}

int m68k_step(m68k_t *cpu) {
    if (cpu->stopped || cpu->halted) return 0;
    m68k_step_ops(cpu);
    return 8;               /* ~8 cycles forfaitaires par instruction */
}
