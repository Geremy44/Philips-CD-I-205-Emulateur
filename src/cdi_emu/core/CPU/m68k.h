/*
 * m68k.h - Motorola 68070 (68000-compatible) CPU core
 *
 * Defines the CPU state structure and interface for the
 * custom M68000 interpreter used by the CD-i 205 emulator.
 */

#ifndef M68K_H
#define M68K_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration - bus_t is defined in bus.h */
typedef struct bus bus_t;

/*
 * M68000 CPU state
 * Holds all programmer-visible registers plus internal control flags.
 */
typedef struct m68k {
    uint32_t pc;           /* Program counter */
    uint32_t sp;           /* Stack pointer (mirror of A7, kept for convenience) */
    uint32_t d[8];         /* Data registers D0-D7 */
    uint32_t a[8];         /* Address registers A0-A7 (A7 = supervisor stack) */
    uint16_t sr;           /* Status register (CCR + system byte) */
    uint32_t usp;          /* User stack pointer while S = 1 (A7 is the SSP) */
    uint32_t ssp;          /* Supervisor stack pointer while S = 0 (A7 is the USP) */

    int stopped;           /* STOP instruction active (waits for interrupt) */
    int halted;            /* Unrecoverable halt (unimplemented opcode, etc.) */

    /* Interrupt request presented to the CPU (by main loop / peripherals).
     * irq_level 0 = none; irq_vector = vector number used when accepted. */
    int irq_level;
    int irq_vector;

    bus_t *bus;            /* Pointer to the memory bus for read/write access */

    uint32_t ea_rmw_addr;  /* address resolved by ea_read_rmw(), reused by ea_write_rmw() */
} m68k_t;

/* SCC68070 exception stack frame, short format: SR, PC, format|vector*4 */
#define M68K_FRAME_SHORT 0x0
#define M68K_FRAME_LONG  0xF   /* 17-word SCC68070 format (bus/address error) */

/* Write SR, swapping A7 between USP and SSP when the S bit changes */
void m68k_set_sr(m68k_t *cpu, uint16_t new_sr);

/* Enter exception processing for `vector` (short stack frame) */
void m68k_exception(m68k_t *cpu, int vector);

/* Present an interrupt request: level 1-7 (0 clears), vector number */
void m68k_set_irq(m68k_t *cpu, int level, int vector);


/* Initialize a CPU structure to zeroed state */
void m68k_init(m68k_t *cpu);

/* Associate a bus with the CPU for memory access */
void m68k_set_bus(m68k_t *cpu, bus_t *bus);

/* Perform a CPU reset: load SSP and PC from the reset vector table */
void m68k_reset(m68k_t *cpu);

/* Memory access wrappers that route through the bus */
uint8_t  m68k_read8 (m68k_t *cpu, uint32_t addr);
uint16_t m68k_read16(m68k_t *cpu, uint32_t addr);
uint32_t m68k_read32(m68k_t *cpu, uint32_t addr);

/* Execute one instruction; returns approximate cycle count */
int m68k_step(m68k_t *cpu);

/* Raise an address error exception (vector 3, misaligned access) */
void m68k_address_error(m68k_t *cpu, uint32_t fault_addr, uint32_t ir);

#endif /* M68K_H */
