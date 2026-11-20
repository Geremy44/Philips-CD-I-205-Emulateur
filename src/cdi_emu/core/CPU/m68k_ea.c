/*
 * m68k_ea.c - M68000 Effective Address calculation and access
 *
 * The 68000 encodes operand locations using a 6-bit field in each
 * instruction word: a 3-bit mode (m) and a 3-bit register (r).
 * This module decodes that field, computes the target address
 * (or identifies a register), and provides read/write primitives.
 *
 * Addressing modes:
 *   0  Dn        - Data register direct
 *   1  An        - Address register direct
 *   2  (An)      - Address register indirect
 *   3  (An)+     - Postincrement
 *   4  -(An)     - Predecrement
 *   5  d16(An)   - Displacement
 *   6  d8(An,Xn) - Indexed
 *   7  Special   - See sub-modes below
 */

#include "m68k_ea.h"
#include "../bus.h"
#include <stdio.h>

/*
 * Write to an effective address.
 * For register destinations, preserves the upper bits according to size.
 * For memory destinations, performs a bus write of the appropriate width.
 */
void ea_write(m68k_t *cpu, uint8_t mode, uint8_t reg, int size, uint32_t val)
{
    /* Register modes are decided by the mode field, never by the value
     * returned by ea_calc: a real address such as 0xE5FFFFFF must not be
     * mistaken for a register. */
    if (mode == 0) {
        int r = reg & 7;
        if (size == 8)       cpu->d[r] = (cpu->d[r] & ~0x000000FFu) | (val & 0x000000FFu);
        else if (size == 16) cpu->d[r] = (cpu->d[r] & ~0x0000FFFFu) | (val & 0x0000FFFFu);
        else                 cpu->d[r] = val;
        return;
    }

    /* Address register: always written as 32-bit */
    if (mode == 1) {
        cpu->a[reg & 7] = val;
        return;
    }

    uint32_t raw = ea_calc(cpu, mode, reg, size);

    /* Memory access */
    if (size == 8)       bus_write8 (cpu->bus, raw, (uint8_t)val);
    else if (size == 16) bus_write16(cpu->bus, raw, (uint16_t)val);
    else                 bus_write32(cpu->bus, raw, val);
}

/*
 * Read from an effective address.
 * For register sources, returns only the relevant lower bits per size.
 * For memory sources, performs a bus read of the appropriate width.
 */
uint32_t ea_read(m68k_t *cpu, uint8_t mode, uint8_t reg, int size)
{
    /* Register modes: decided by the mode field (see ea_write) */
    if (mode == 0) {
        uint32_t v = cpu->d[reg & 7];
        if (size == 8)  return v & 0x000000FFu;
        if (size == 16) return v & 0x0000FFFFu;
        return v;
    }

    /* Address register */
    if (mode == 1) {
        uint32_t v = cpu->a[reg & 7];
        if (size == 16) return v & 0x0000FFFFu;
        return v;
    }

    uint32_t raw = ea_calc(cpu, mode, reg, size);

    /* Memory access */
    if (size == 8)  return m68k_read8 (cpu, raw);
    if (size == 16) return m68k_read16(cpu, raw);
    return m68k_read32(cpu, raw);
}

/*
 * Calculate the effective address for a given mode/register.
 *
 * Returns either a memory address or a sentinel:
 *   0xF0000000 | reg  -> Data register Dn
 *   0xE0000000 | reg  -> Address register An
 *
 * For modes that consume extension words (displacement, indexed,
 * absolute, immediate), this function advances the PC accordingly.
 */
uint32_t ea_calc(m68k_t *cpu, uint8_t mode, uint8_t reg, int size)
{
    switch (mode) {
        case 0: /* Dn - Data register direct */
            return 0xF0000000u | reg;

        case 1: /* An - Address register direct */
            return 0xE0000000u | reg;

        case 2: /* (An) - Address register indirect */
            return cpu->a[reg];

        case 3: /* (An)+ - Postincrement */
        {
            uint32_t addr = cpu->a[reg];
            int inc = (size == 8) ? 1 : (size == 16) ? 2 : 4;
            /* A7 (stack pointer) always stays word-aligned */
            if (reg == 7 && size == 8) inc = 2;
            cpu->a[reg] += inc;
            return addr;
        }

        case 4: /* -(An) - Predecrement */
        {
            int inc = (size == 8) ? 1 : (size == 16) ? 2 : 4;
            if (reg == 7 && size == 8) inc = 2;
            cpu->a[reg] -= inc;
            return cpu->a[reg];
        }

        case 5: /* d16(An) - Displacement */
        {
            int16_t disp = (int16_t)m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            return cpu->a[reg] + disp;
        }

        case 6: /* d8(An, Xn) - Indexed with displacement */
        {
            uint16_t ext = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            int8_t d8 = (int8_t)(ext & 0xFF);
            int xn = (ext >> 12) & 0xF;
            uint32_t xv = (xn & 8) ? cpu->a[xn & 7] : cpu->d[xn & 7];
            if (!(ext & 0x0800)) xv = (uint32_t)(int16_t)xv;  /* Word-sized index */
            return cpu->a[reg] + d8 + xv;
        }

        case 7: /* Special addressing modes */
            switch (reg) {
                case 0: { /* abs.W - Absolute short (sign-extended) */
                    int16_t a = (int16_t)m68k_read16(cpu, cpu->pc);
                    cpu->pc += 2;
                    return (uint32_t)a;
                }
                case 1: { /* abs.L - Absolute long */
                    uint32_t a = m68k_read32(cpu, cpu->pc);
                    cpu->pc += 4;
                    return a;
                }
                case 2: { /* d16(PC) - PC-relative with displacement */
                    uint32_t base = cpu->pc;
                    int16_t d16 = (int16_t)m68k_read16(cpu, cpu->pc);
                    cpu->pc += 2;
                    return base + (uint32_t)(int32_t)d16;
                }
                case 3: { /* d8(PC, Xn) - PC-relative indexed */
                    uint32_t base = cpu->pc;
                    uint16_t ext = m68k_read16(cpu, cpu->pc);
                    cpu->pc += 2;
                    int8_t d8 = (int8_t)(ext & 0xFF);
                    int xn = (ext >> 12) & 0xF;
                    uint32_t xv = (xn & 8) ? cpu->a[xn & 7] : cpu->d[xn & 7];
                    if (!(ext & 0x0800)) xv = (uint32_t)(int16_t)xv;  /* Word-sized index */
                    return base + (uint32_t)(int32_t)d8 + xv;
                }
                case 4: { /* #imm - Immediate data */
                    uint32_t saved_pc = cpu->pc;
                    cpu->pc += (size == 32) ? 4 : 2;
                    /* Byte-sized immediates are stored in the high byte of the word */
                    return (size == 8) ? saved_pc + 1 : saved_pc;
                }
                default:
                    return 0;
            }

        default:
            return 0;
    }
}

/* ---- Read-modify-write: resolve the effective address only once ---- */

uint32_t ea_read_rmw(m68k_t *cpu, uint8_t mode, uint8_t reg, int size)
{
    if (mode == 0 || mode == 1)
        return ea_read(cpu, mode, reg, size);

    uint32_t addr = ea_calc(cpu, mode, reg, size);
    cpu->ea_rmw_addr = addr;
    if (size == 8)  return m68k_read8 (cpu, addr);
    if (size == 16) return m68k_read16(cpu, addr);
    return m68k_read32(cpu, addr);
}

void ea_write_rmw(m68k_t *cpu, uint8_t mode, uint8_t reg, int size, uint32_t val)
{
    if (mode == 0 || mode == 1) {
        ea_write(cpu, mode, reg, size, val);
        return;
    }
    uint32_t addr = cpu->ea_rmw_addr;
    if (size == 8)       bus_write8 (cpu->bus, addr, (uint8_t)val);
    else if (size == 16) bus_write16(cpu->bus, addr, (uint16_t)val);
    else                 bus_write32(cpu->bus, addr, val);
}
