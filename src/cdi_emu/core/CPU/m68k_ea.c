/* m68k_ea.c */
#include "m68k_ea.h"
#include "../bus.h"
#include <stdio.h>


// uint32_t ea_read(m68k_t *cpu, uint8_t mode, uint8_t reg, int size)
// {
//     uint32_t raw = ea_calc(cpu, mode, reg, size);
//     if ((raw & 0xF0000000u) == 0xF0000000u) return cpu->d[raw & 7];
//     if ((raw & 0xF0000000u) == 0xE0000000u) return cpu->a[raw & 7];
//     if (size == 8)  return m68k_read8 (cpu, raw);
//     if (size == 16) return m68k_read16(cpu, raw);
//     return m68k_read32(cpu, raw);
// }


// void ea_write(m68k_t *cpu, uint8_t mode, uint8_t reg, int size, uint32_t val)
// {
//     uint32_t raw = ea_calc(cpu, mode, reg, size);
//     if ((raw & 0xF0000000u) == 0xF0000000u) { cpu->d[raw & 7] = val; return; }
//     if ((raw & 0xF0000000u) == 0xE0000000u) { cpu->a[raw & 7] = val; return; }
//     if (size == 8)       bus_write8 (cpu->bus, raw, (uint8_t)val);
//     else if (size == 16) bus_write16(cpu->bus, raw, (uint16_t)val);
//     else                 bus_write32(cpu->bus, raw, val);
// }

void ea_write(m68k_t *cpu, uint8_t mode, uint8_t reg, int size, uint32_t val)
{
    uint32_t raw = ea_calc(cpu, mode, reg, size);

    /* Dn : préserver les bits de poids fort selon la taille */
    if ((raw & 0xF0000000u) == 0xF0000000u) {
        int r = raw & 7;
        if (size == 8)       cpu->d[r] = (cpu->d[r] & ~0x000000FFu) | (val & 0x000000FFu);
        else if (size == 16) cpu->d[r] = (cpu->d[r] & ~0x0000FFFFu) | (val & 0x0000FFFFu);
        else                 cpu->d[r] = val;
        return;
    }

    /* An : toujours écrit 32 bits (le sign-extend .w est géré par MOVEA/ADDA...) */
    if ((raw & 0xF0000000u) == 0xE0000000u) {
        cpu->a[raw & 7] = val;
        return;
    }

    if (size == 8)       bus_write8 (cpu->bus, raw, (uint8_t)val);
    else if (size == 16) bus_write16(cpu->bus, raw, (uint16_t)val);
    else                 bus_write32(cpu->bus, raw, val);
}

uint32_t ea_read(m68k_t *cpu, uint8_t mode, uint8_t reg, int size)
{
    uint32_t raw = ea_calc(cpu, mode, reg, size);

    /* Dn : ne retourner que la partie utile selon la taille */
    if ((raw & 0xF0000000u) == 0xF0000000u) {
        uint32_t v = cpu->d[raw & 7];
        if (size == 8)  return v & 0x000000FFu;
        if (size == 16) return v & 0x0000FFFFu;
        return v;
    }

    /* An : idem (rare en lecture .w mais on masque par cohérence) */
    if ((raw & 0xF0000000u) == 0xE0000000u) {
        uint32_t v = cpu->a[raw & 7];
        if (size == 16) return v & 0x0000FFFFu;
        return v;
    }

    if (size == 8)  return m68k_read8 (cpu, raw);
    if (size == 16) return m68k_read16(cpu, raw);
    return m68k_read32(cpu, raw);
}

uint32_t ea_calc(m68k_t *cpu, uint8_t mode, uint8_t reg, int size)
{
    switch (mode) {
        case 0: /* Dn */
            return 0xF0000000u | reg;

        case 1: /* An */
            return 0xE0000000u | reg;

        case 2: /* (An) */
            return cpu->a[reg];

        case 3: /* (An)+ */
        {
            uint32_t addr = cpu->a[reg];
            int inc = (size == 8) ? 1 : (size == 16) ? 2 : 4;
            if (reg == 7 && size == 8) inc = 2; /* alignement mot pour la pile */
            cpu->a[reg] += inc;
            return addr;
        }

        case 4: /* -(An) */
        {
            int inc = (size == 8) ? 1 : (size == 16) ? 2 : 4;
            if (reg == 7 && size == 8) inc = 2;
            cpu->a[reg] -= inc;
            return cpu->a[reg];
        }

        case 5: /* d16(An) */
        {
            int16_t disp = (int16_t)m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            return cpu->a[reg] + disp;
        }

        case 6: /* d8(An, Xn) */
        {
            uint16_t ext = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            int8_t d8 = (int8_t)(ext & 0xFF);
            int xn = (ext >> 12) & 0xF;
            uint32_t xv = (xn & 8) ? cpu->a[xn & 7] : cpu->d[xn & 7];
            if (!(ext & 0x0800)) xv = (uint32_t)(int16_t)xv; /* registre index .W */
            return cpu->a[reg] + d8 + xv;
        }

        case 7:
            switch (reg) {
                case 0: { /* abs.W */
                    int16_t a = (int16_t)m68k_read16(cpu, cpu->pc);
                    cpu->pc += 2;
                    return (uint32_t)a;
                }
                case 1: { /* abs.L */
                    uint32_t a = m68k_read32(cpu, cpu->pc);
                    cpu->pc += 4;
                    return a;
                }
                case 2: { /* d16(PC) */
                    uint32_t base = cpu->pc;                 /* base = PC du mot d'extension */
                    int16_t d16 = (int16_t)m68k_read16(cpu, cpu->pc);
                    cpu->pc += 2;
                    return base + (uint32_t)(int32_t)d16;
                }
                case 3: { /* d8(PC, Xn) */
                    uint32_t base = cpu->pc;
                    uint16_t ext = m68k_read16(cpu, cpu->pc);
                    cpu->pc += 2;
                    int8_t d8 = (int8_t)(ext & 0xFF);
                    int xn = (ext >> 12) & 0xF;
                    uint32_t xv = (xn & 8) ? cpu->a[xn & 7] : cpu->d[xn & 7];
                    if (!(ext & 0x0800)) xv = (uint32_t)(int16_t)xv; /* index .W */
                    return base + (uint32_t)(int32_t)d8 + xv;

                }

                case 4: { /* immédiat */
                    uint32_t saved_pc = cpu->pc;
                    cpu->pc += (size == 32) ? 4 : 2;
                    return (size == 8) ? saved_pc + 1 : saved_pc;   /* .b → octet haut */
                }

                default:
                    return 0;
            }

        default:
            return 0;
    }
}
