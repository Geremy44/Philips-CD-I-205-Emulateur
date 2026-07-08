#ifndef M68K_H
#define M68K_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration — bus_t est défini dans bus.h */
typedef struct bus bus_t;

typedef struct m68k {
    uint32_t pc;           /* Program counter */
    uint32_t sp;           /* Stack pointer (A7) */
    uint32_t d[8];         /* Data registers */
    uint32_t a[8];         /* Address registers */
    uint16_t sr;           /* Status register */
    uint32_t usp;          /* User stack pointer */

    int stopped;           /* CPU stopped flag */
    int halted;            /* CPU halted flag */

    bus_t *bus;            /* Bus reference */
} m68k_t;


void m68k_init(m68k_t *cpu);
void m68k_set_bus(m68k_t *cpu, bus_t *bus);
void m68k_reset(m68k_t *cpu);

uint8_t  m68k_read8 (m68k_t *cpu, uint32_t addr);
uint16_t m68k_read16(m68k_t *cpu, uint32_t addr);
uint32_t m68k_read32(m68k_t *cpu, uint32_t addr);

int m68k_step(m68k_t *cpu);

/* ---- Exception handling ---- */
void m68k_address_error(m68k_t *cpu, uint32_t fault_addr, uint32_t ir);

#endif
