/*
 * m68k_ea.h - M68000 Effective Address (EA) calculation
 *
 * The 68000 encodes operand locations in a 6-bit field (mode + register).
 * These functions decode that field into actual addresses or register
 * references, and perform read/write operations through the bus.
 */

#ifndef M68K_EA_H
#define M68K_EA_H

#include "m68k.h"

/*
 * Calculate the effective address for the given mode/register.
 *
 * Returns either a real memory address or a sentinel value:
 *   0xF0xxxxxx - Data register D(x & 7)
 *   0xE0xxxxxx - Address register A(x & 7)
 *
 * size is 8, 16, or 32 - affects auto-increment/pre-decrement step size
 * and immediate operand fetch width.
 */
uint32_t ea_calc(m68k_t *cpu, uint8_t mode, uint8_t reg, int size);

/*
 * Write a value to the effective address.
 * For register destinations, preserves upper bits per operand size.
 */
void ea_write(m68k_t *cpu, uint8_t mode, uint8_t reg, int size, uint32_t val);

/*
 * Read a value from the effective address.
 * For register sources, returns only the relevant lower bits per size.
 */
uint32_t ea_read(m68k_t *cpu, uint8_t mode, uint8_t reg, int size);

/*
 * Read-modify-write pair (ADDQ, SUBQ, NEG, NOT, ORI/ANDI/..., EOR, AND,
 * BCHG/BCLR/BSET, ...). ea_read_rmw() resolves the address once and
 * ea_write_rmw() writes back to that same address, so extension words
 * are fetched once and (An)+ / -(An) are applied once.
 */
uint32_t ea_read_rmw (m68k_t *cpu, uint8_t mode, uint8_t reg, int size);
void     ea_write_rmw(m68k_t *cpu, uint8_t mode, uint8_t reg, int size, uint32_t val);

#endif /* M68K_EA_H */
