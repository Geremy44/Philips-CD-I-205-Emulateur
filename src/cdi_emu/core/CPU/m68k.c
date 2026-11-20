/*
 * m68k.c - Motorola 68070 (68000-compatible) CPU core
 *
 * Provides CPU initialization, reset (vector loading), and the
 * single-instruction execution wrapper. The actual instruction
 * decoding is in m68k_ops.c, and effective address calculation
 * is in m68k_ea.c.
 */

#include "m68k.h"
#include "m68k_ops.h"
#include "../bus.h"
#include <string.h>
#include <stdio.h>

/* Initialize the CPU structure to a clean zeroed state */
void m68k_init(m68k_t *cpu) {
    memset(cpu, 0, sizeof(*cpu));
}

/* Associate a bus instance with the CPU for memory access */
void m68k_set_bus(m68k_t *cpu, bus_t *bus) {
    cpu->bus = bus;
}

/*
 * Perform a CPU reset.
 * The M68000 loads the initial Supervisor Stack Pointer (SSP) from
 * the first 4 bytes of the vector table and the Program Counter (PC)
 * from the next 4 bytes. In the CD-i 205, the vector table is at the
 * start of the ROM ($180000).
 */
void m68k_reset(m68k_t *cpu) {
    bus_t *saved = cpu->bus;
    memset(cpu, 0, sizeof(*cpu));
    cpu->bus = saved;
    cpu->sr  = 0x2700;  /* Supervisor mode, interrupt mask = 7 */

    /* Load reset vectors from ROM at $180000 (mapped from $180000) */
    cpu->a[7] = ((uint32_t)bus_read16(cpu->bus, 0x180000) << 16)
              |  (uint32_t)bus_read16(cpu->bus, 0x180002);
    cpu->pc   = ((uint32_t)bus_read16(cpu->bus, 0x180004) << 16)
              |  (uint32_t)bus_read16(cpu->bus, 0x180006);
    cpu->stopped = 0;
}

/* Memory read helpers - route through the bus */
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

/*
 * Execute one CPU instruction.
 * Returns an approximate cycle count (~8 cycles per instruction).
 * The actual cycle count varies by instruction but this flat
 * estimate is sufficient for basic timing.
 */
int m68k_step(m68k_t *cpu) {
    if (cpu->halted) return 0;

    /* Interrupts are sampled between instructions. A request is accepted
     * when its level is above the SR mask (level 7 is non-maskable). */
    if (cpu->irq_level > 0) {
        int mask = (cpu->sr >> 8) & 7;
        if (cpu->irq_level > mask || cpu->irq_level == 7) {
            int level = cpu->irq_level;
            m68k_exception(cpu, cpu->irq_vector);
            cpu->sr = (uint16_t)((cpu->sr & ~0x0700) | (level << 8));
            return 44;                      /* approx. interrupt latency */
        }
    }

    /* STOP: time passes (peripherals keep running) until an interrupt */
    if (cpu->stopped) return 8;

    m68k_step_ops(cpu);
    return 8;
}

void m68k_set_irq(m68k_t *cpu, int level, int vector) {
    cpu->irq_level  = level & 7;
    cpu->irq_vector = vector;
}
