#include <stdio.h>
#include "m68k_ops.h"
#include "m68k_ea.h"
#include "../bus.h"

/* SR flag masks (also declared in m68k_ops.h for external use) */
#ifndef SR_N
#define SR_N 0x0008
#define SR_Z 0x0004
#define SR_V 0x0002
#define SR_C 0x0001
#define SR_X 0x0010
#endif

/* ================================================================
 *  Flag helpers
 * ================================================================ */

static inline void set_nz_flags8(m68k_t *cpu, uint8_t val) {
    cpu->sr &= ~(SR_N | SR_Z);
    if (val == 0)     cpu->sr |= SR_Z;
    if (val & 0x80)  cpu->sr |= SR_N;
}

static inline void set_nz_flags16(m68k_t *cpu, uint16_t val) {
    cpu->sr &= ~(SR_N | SR_Z);
    if (val == 0)      cpu->sr |= SR_Z;
    if (val & 0x8000)  cpu->sr |= SR_N;
}

static inline void set_nz_flags32(m68k_t *cpu, uint32_t val) {
    cpu->sr &= ~(SR_N | SR_Z);
    if (val == 0)           cpu->sr |= SR_Z;
    if (val & 0x80000000U)  cpu->sr |= SR_N;
}

static inline void flag_NZ_clearVC(uint16_t *sr, uint32_t r, int size)
{
    *sr &= ~(SR_N | SR_Z | SR_V | SR_C);
    if (size == 8) {
        if ((uint8_t)r == 0)  *sr |= SR_Z;
        if (r & 0x80)          *sr |= SR_N;
    } else if (size == 16) {
        if ((uint16_t)r == 0)  *sr |= SR_Z;
        if (r & 0x8000)        *sr |= SR_N;
    } else {
        if (r == 0)            *sr |= SR_Z;
        if (r & 0x80000000U)   *sr |= SR_N;
    }
}

static inline uint32_t mask_of(int size) {
    return (size == 8) ? 0xFFu
         : (size == 16) ? 0xFFFFu
         : 0xFFFFFFFFu;
}

/* ================================================================
 *  Condition codes
 * ================================================================ */

static int cc_true(uint16_t sr, int cc) {
    int C = (sr >> 0) & 1;
    int V = (sr >> 1) & 1;
    int Z = (sr >> 2) & 1;
    int N = (sr >> 3) & 1;
    switch (cc) {
        case 0x0: return 1;             /* T  */
        case 0x1: return 0;             /* F  */
        case 0x2: return !C && !Z;       /* HI */
        case 0x3: return C || Z;         /* LS */
        case 0x4: return !C;             /* CC */
        case 0x5: return C;              /* CS */
        case 0x6: return !Z;             /* NE */
        case 0x7: return Z;              /* EQ */
        case 0x8: return !V;             /* VC */
        case 0x9: return V;              /* VS */
        case 0xA: return !N;            /* PL */
        case 0xB: return N;              /* MI */
        case 0xC: return (N == V);       /* GE */
        case 0xD: return (N != V);       /* LT */
        case 0xE: return !Z && (N == V); /* GT */
        case 0xF: return Z || (N != V);  /* LE */
    }
    return 0;
}

/* ================================================================
 *  SUB flag calculation (for CMP, etc.)
 * ================================================================ */

void flag_sub(uint16_t *sr, uint32_t d, uint32_t s, uint32_t r, int size) {
    uint32_t msb = (size == 8) ? 0x80u : (size == 16) ? 0x8000u : 0x80000000u;
    uint32_t mask = mask_of(size);
    r &= mask; d &= mask; s &= mask;

    *sr &= ~0x0F;
    if (r == 0)                       *sr |= 0x04;  /* Z */
    if (r & msb)                      *sr |= 0x08;  /* N */
    if (((s ^ d) & (d ^ r)) & msb)   *sr |= 0x02;  /* V */
    if (s > d)                        *sr |= 0x01;  /* C (borrow) */
}

/* ================================================================
 *  Stack helpers
 * ================================================================ */

static void push32(m68k_t *cpu, uint32_t val)
{
    cpu->a[7] -= 4;
    bus_write32(cpu->bus, cpu->a[7], val);
}

/* ================================================================
 * 68000 Address Error (vecteur 3) — frame 14 octets (7 mots)
 * ================================================================ */

void m68k_address_error(m68k_t *cpu, uint32_t fault_addr, uint32_t ir)
{
    uint16_t old_sr = cpu->sr;
    uint32_t old_pc = cpu->pc;

    cpu->sr |=  0x2000;    /* superviseur */
    cpu->sr &= ~0x8000;    /* clear trace */

    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], old_pc & 0xFFFF);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], old_pc >> 16);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], old_sr);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], ir & 0xFFFF);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], fault_addr & 0xFFFF);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], fault_addr >> 16);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], 0x0009);  /* SSW */

    /* handler = vecteur 3 (table à 0x180000, offset 0x0C) */
    cpu->pc = ((uint32_t)m68k_read16(cpu, 0x18000C) << 16)
            |  (uint32_t)m68k_read16(cpu, 0x18000E);
}


void op_shift_reg(m68k_t *cpu, uint16_t op) {
    int count_field = (op >> 9) & 7;   /* count immediat OU num de registre */
    int dir         = (op >> 8) & 1;   /* 1 = left, 0 = right */
    int size        = (op >> 6) & 3;   /* 00=byte 01=word 10=long */
    int ir          = (op >> 5) & 1;   /* 0 = count immediat, 1 = par registre */
    int type        = (op >> 3) & 3;   /* 00=AS 01=LS 10=ROX 11=RO */
    int reg         = op & 7;          /* registre de donnees cible */

    //int X = (cpu->sr >> 4) & 1;

    /* Nombre de decalages */
    uint32_t count;
    if (ir == 0) {
        count = (count_field == 0) ? 8 : count_field;  /* 0 => 8 */
    } else {
        count = cpu->d[count_field] & 63;              /* modulo 64 */
    }

    /* Masque et nombre de bits selon la taille */
    uint32_t mask;
    int bits;
    switch (size) {
        case 0: mask = 0x000000FF; bits = 8;  break;
        case 1: mask = 0x0000FFFF; bits = 16; break;
        case 2: mask = 0xFFFFFFFF; bits = 32; break;
        default: return;
    }

    uint32_t val = cpu->d[reg] & mask;
    int carry = 0;
    int overflow = 0;

    if (type == 0) {
        /* ===== ASL / ASR (arithmetique) ===== */
        if (dir) {
            /* ASL */
            for (uint32_t i = 0; i < count; i++) {
                uint32_t msb_before = (val >> (bits - 1)) & 1;
                carry = msb_before;
                val = (val << 1) & mask;
                uint32_t msb_after = (val >> (bits - 1)) & 1;
                if (msb_before != msb_after) overflow = 1;
            }
        } else {
            /* ASR : conserve le bit de signe */
            uint32_t sign = (val >> (bits - 1)) & 1;
            for (uint32_t i = 0; i < count; i++) {
                carry = val & 1;
                val >>= 1;
                if (sign) val |= (1u << (bits - 1));
            }
        }
    } else if (type == 1) {
        /* ===== LSL / LSR (logique) ===== */
        if (dir) {
            for (uint32_t i = 0; i < count; i++) {
                carry = (val >> (bits - 1)) & 1;
                val = (val << 1) & mask;
            }
        } else {
            for (uint32_t i = 0; i < count; i++) {
                carry = val & 1;
                val >>= 1;
            }
        }
    } else if (type == 2) {
        /* ===== ROXL / ROXR (rotation via X) ===== */
        int x = (cpu->sr >> 4) & 1;   /* bit X du SR */
        if (dir) {
            for (uint32_t i = 0; i < count; i++) {
                int msb = (val >> (bits - 1)) & 1;
                val = ((val << 1) | x) & mask;
                x = msb;
            }
        } else {
            for (uint32_t i = 0; i < count; i++) {
                int lsb = val & 1;
                val = (val >> 1) | ((uint32_t)x << (bits - 1));
                x = lsb;
            }
        }
        carry = x;
    } else {
        /* ===== ROL / ROR (rotation simple) ===== */
        if (dir) {
            /* ROL */
            for (uint32_t i = 0; i < count; i++) {
                int msb = (val >> (bits - 1)) & 1;
                val = ((val << 1) | msb) & mask;
                carry = msb;
            }
        } else {
            /* ROR : notre cas 0xE898 */
            for (uint32_t i = 0; i < count; i++) {
                int lsb = val & 1;
                val = (val >> 1) | ((uint32_t)lsb << (bits - 1));
                carry = lsb;
            }
        }
    }

    /* Ecriture du resultat dans le registre (preserve les bits hauts non concernes) */
    cpu->d[reg] = (cpu->d[reg] & ~mask) | (val & mask);

    /* ===== Mise a jour des flags ===== */
    /* N : bit de signe du resultat */
    int N = (val >> (bits - 1)) & 1;
    /* Z : resultat nul */
    int Z = ((val & mask) == 0) ? 1 : 0;

    /* Reconstruire le SR : bits CCR = X N Z V C (positions 4 3 2 1 0) */
    uint16_t sr = cpu->sr & ~0x1F;  /* efface X N Z V C */

    if (N) sr |= (1 << 3);
    if (Z) sr |= (1 << 2);
    if (overflow) sr |= (1 << 1);   /* V : seulement pour ASL, sinon 0 */
    if (count != 0) {
        if (carry) sr |= (1 << 0);  /* C */
        /* X = C pour AS/LS/ROX (pas pour RO !) */
        if (type != 3) {
            if (carry) sr |= (1 << 4);  /* X */
            else       sr &= ~(1 << 4);
        } else {
            /* RO : X inchange -> on le restaure */
            sr |= (cpu->sr & (1 << 4));
        }
    } else {
        /* count == 0 : C = 0 (sauf ROX ou C=X), X inchange */
        if (type == 2) { if ((cpu->sr >> 4) & 1) sr |= 1; }  /* ROX: C=X */
        sr |= (cpu->sr & (1 << 4));  /* X inchange */
    }

    cpu->sr = sr;
}

/* ================================================================
 *  Décodeur principal
 * ================================================================ */

void m68k_step_ops(m68k_t *cpu)
{
    if (cpu->pc & 1) {
        m68k_address_error(cpu, cpu->pc, 0x0000);
        return;
    }

    uint16_t insn = m68k_read16(cpu, cpu->pc);
    cpu->pc += 2;

    uint8_t nib0    = (uint8_t)(insn >> 12);
    uint8_t ea_mode = (uint8_t)((insn >> 3) & 0x07);
    uint8_t ea_reg  = (uint8_t)(insn & 0x07);
    int     cc      = (insn >> 8) & 0xF;
    uint32_t base   = cpu->pc;

    switch (nib0) {

    /* ---------- 0x0 : ORI/ANDI/EORI CCR-SR + Bit ops ---------- */
    case 0x0: {
        if (insn == 0x003C) {                        /* ORI #imm,CCR */
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            cpu->sr = (cpu->sr & ~0x001F) | ((cpu->sr | imm) & 0x001F);
            return;
        }
        if (insn == 0x007C) {                        /* ORI #imm,SR */
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            cpu->sr |= imm;
            return;
        }
        if (insn == 0x027C) {                        /* ANDI #imm,SR */
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            cpu->sr &= imm;
            return;
        }
        if (insn == 0x0A7C) {                        /* EORI #imm,SR */
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            cpu->sr ^= imm;
            return;
        }
        if ((insn & 0xFF00) == 0x0800) {             /* BTST/BCHG/BCLR/BSET #imm */
            uint16_t bitsel = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            int size = (ea_mode == 0) ? 32 : 8;
            uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
            int op  = (insn >> 6) & 3;
            int bit = bitsel & ((size == 32) ? 31 : 7);
            uint32_t mask = 1u << bit;
            cpu->sr &= ~SR_Z;
            if (!(val & mask)) cpu->sr |= SR_Z;
            if (op == 1)      val ^= mask;
            else if (op == 2)  val &= ~mask;
            else if (op == 3)  val |= mask;
            if (op != 0) ea_write(cpu, ea_mode, ea_reg, size, val);
            return;
        }
        /* ---------- Immediat arithmetique/logique : ORI/ANDI/SUBI/ADDI/EORI/CMPI ---------- */
        /* Format : 0000 tttt ss mmm rrr   (tttt pair, bit8=0 => pas bit-ops) */
        /* ---------- Immediat arithmetique/logique : ORI/ANDI/SUBI/ADDI/EORI/CMPI ----------
        * Format : 0000 tttt ss mmm rrr
        *   tttt = 0x0 ORI  | 0x2 ANDI | 0x4 SUBI | 0x6 ADDI | 0xA EORI | 0xC CMPI
        *   ss   = 00 byte  | 01 word  | 10 long  | 11 INVALIDE
        * On utilise 4 bits (tttt), PAS 3, sinon ADDI(6) et CMPI(C) sont confondus !
        */
        if ((insn & 0xF000) == 0x0000 && (insn & 0x0100) == 0) {
            int tttt = (insn >> 8) & 0xF;   /* 4 BITS : discriminant complet */

            /* Ne traiter que les vrais immediats arithmetiques/logiques */
            if (tttt == 0x0 || tttt == 0x2 || tttt == 0x4 ||
                tttt == 0x6 || tttt == 0xA || tttt == 0xC) {

                int size = (insn >> 6) & 3;   /* 0=byte 1=word 2=long */

                if (size == 3) {
                    fprintf(stderr, "[M68K] imm size=3 invalide: 0x%04X @ 0x%08X\n",
                            insn, cpu->pc - 2);
                    cpu->halted = 1;
                    return;
                }

                int bits = (size == 0) ? 8 : (size == 1) ? 16 : 32;

                fprintf(stderr, "[CASE0-IMM] insn=0x%04X tttt=0x%X size=%d PC=0x%08X\n",
                        insn, tttt, size, cpu->pc - 2);

                /* Lecture de l'immediat */
                uint32_t imm;
                if (size == 2) {
                    imm = ((uint32_t)m68k_read16(cpu, cpu->pc) << 16)
                        |  (uint32_t)m68k_read16(cpu, cpu->pc + 2);
                    cpu->pc += 4;
                } else {
                    imm = m68k_read16(cpu, cpu->pc);
                    cpu->pc += 2;
                    if (size == 0) imm &= 0xFF;
                }

                /* Lecture de l'operande (EA) */
                uint32_t val = ea_read(cpu, ea_mode, ea_reg, bits);

                uint32_t mask = (bits == 8) ? 0xFF : (bits == 16) ? 0xFFFF : 0xFFFFFFFF;
                uint32_t msb  = 1u << (bits - 1);
                uint32_t res;
                int writeback = 1;

                switch (tttt) {
                    case 0x0: res = val | imm; break;   /* ORI  */
                    case 0x2: res = val & imm; break;   /* ANDI */
                    case 0xA: res = val ^ imm; break;   /* EORI */

                    case 0x4:   /* SUBI : val - imm */
                    case 0xC: { /* CMPI : val - imm, PAS de writeback */
                        uint64_t full = (uint64_t)(val & mask) - (uint64_t)(imm & mask);
                        res = (uint32_t)full & mask;
                        cpu->sr &= ~0x0F;                                     /* N Z V C (pas X) */
                        if (res & msb)                     cpu->sr |= (1 << 3); /* N */
                        if ((res & mask) == 0)             cpu->sr |= (1 << 2); /* Z */
                        if (full & ((uint64_t)1 << bits))  cpu->sr |= (1 << 0); /* C = emprunt */
                        if (((val ^ imm) & (val ^ res)) & msb) cpu->sr |= (1 << 1); /* V */

                        if (tttt == 0xC) {
                            writeback = 0;                  /* CMPI n'ecrit jamais */
                        } else {
                            /* SUBI met X = C */
                            if (cpu->sr & 1) cpu->sr |= (1 << 4);
                            else             cpu->sr &= ~(1 << 4);
                        }
                        goto imm_done;
                    }

                    case 0x6: { /* ADDI : val + imm */
                        uint64_t full = (uint64_t)(val & mask) + (uint64_t)(imm & mask);
                        res = (uint32_t)full & mask;
                        cpu->sr &= ~0x0F;
                        if (res & msb)                     cpu->sr |= (1 << 3); /* N */
                        if ((res & mask) == 0)             cpu->sr |= (1 << 2); /* Z */
                        if (full & ((uint64_t)1 << bits))  cpu->sr |= (1 << 0); /* C */
                        if ((~(val ^ imm) & (val ^ res)) & msb) cpu->sr |= (1 << 1); /* V */
                        if (cpu->sr & 1) cpu->sr |= (1 << 4);  /* X = C */
                        else             cpu->sr &= ~(1 << 4);
                        goto imm_done;
                    }

                    default:
                        fprintf(stderr, "[M68K] immediat tttt inconnu 0x%X @ 0x%08X\n",
                                tttt, cpu->pc);
                        cpu->halted = 1;
                        return;
                }

                /* Flags logiques (ORI/ANDI/EORI) : N,Z ; V=C=0 ; X inchange */
                cpu->sr &= ~0x0F;
                if (res & msb)         cpu->sr |= (1 << 3);   /* N */
                if ((res & mask) == 0) cpu->sr |= (1 << 2);   /* Z */

            imm_done:
                if (writeback)
                    ea_write(cpu, ea_mode, ea_reg, bits, res);
                return;
            }
            /* sinon : pas un immediat (ex: MOVEP, bit-ops dynamiques...) -> continue */
        }
        return;
    }

    /* ---------- 0x1 / 0x2 / 0x3 : MOVE ---------- */
    case 0x1: case 0x2: case 0x3: {
        int size = (nib0 == 0x1) ? 8 : (nib0 == 0x2) ? 32 : 16;
        uint8_t src_mode = ea_mode, src_reg = ea_reg;
        uint8_t dst_reg  = (insn >> 9) & 7;
        uint8_t dst_mode = (insn >> 6) & 7;

        uint32_t val = ea_read(cpu, src_mode, src_reg, size);

        if (dst_mode == 1) {                          /* MOVEA */
            if (size == 16) val = (uint32_t)(int32_t)(int16_t)val;
            cpu->a[dst_reg] = val;
        } else {
            ea_write(cpu, dst_mode, dst_reg, size, val);
            flag_NZ_clearVC(&cpu->sr, val, size);
        }
        return;
    }

    /* ---------- 0x4 : JMP/JSR/RTS/RTE/RTR/LEA/NOP/CLR/TST/SWAP/PEA/MOVEM/USP ---------- */
    case 0x4: {
        if (insn == 0x4E75) {                        /* RTS */
            cpu->pc   = m68k_read32(cpu, cpu->a[7]);
            cpu->a[7] += 4;
            return;
        }
        if (insn == 0x4E71) {                        /* NOP */
            return;
        }
        if (insn == 0x4E77) {                        /* RTR */
            uint16_t ccr = m68k_read16(cpu, cpu->a[7]);
            cpu->a[7] += 2;
            cpu->sr = (cpu->sr & 0xFF00) | (ccr & 0x00FF);
            cpu->pc   = m68k_read32(cpu, cpu->a[7]);
            cpu->a[7] += 4;
            return;
        }
        if (insn == 0x4E73) {                        /* RTE */
            cpu->sr     = m68k_read16(cpu, cpu->a[7]);
            cpu->a[7]  += 2;
            cpu->pc     = m68k_read32(cpu, cpu->a[7]);
            cpu->a[7]  += 4;
            return;
        }
        if ((insn & 0xFFF8) == 0x4E60) {             /* MOVE An,USP */
            cpu->usp = cpu->a[insn & 7];
            return;
        }
        if ((insn & 0xFFF8) == 0x4E68) {             /* MOVE USP,An */
            cpu->a[insn & 7] = cpu->usp;
            return;
        }
        if ((insn & 0xFFC0) == 0x4EC0) {             /* JMP <ea> */
            uint32_t target = ea_calc(cpu, ea_mode, ea_reg, 32);
            if (target & 1) {
                m68k_address_error(cpu, target, insn);
                return;
            }
            cpu->pc = target;
            return;
        }
        if ((insn & 0xFFC0) == 0x4E80) {             /* JSR <ea> */
            uint32_t target = ea_calc(cpu, ea_mode, ea_reg, 32);
            push32(cpu, cpu->pc);
            cpu->pc = target;
            return;
        }
        if ((insn & 0xF1C0) == 0x41C0) {             /* LEA <ea>,An */
            cpu->a[(insn >> 9) & 7] = ea_calc(cpu, ea_mode, ea_reg, 32);
            return;
        }
        if ((insn & 0xFF00) == 0x4200) {              /* CLR <ea> */
            int size = ((insn >> 6) & 3) == 0 ? 8
                     : ((insn >> 6) & 3) == 1 ? 16 : 32;
            ea_write(cpu, ea_mode, ea_reg, size, 0);
            cpu->sr = (cpu->sr & ~(SR_N|SR_V|SR_C)) | SR_Z;
            return;
        }
        if ((insn & 0xFF00) == 0x4A00) {             /* TST <ea> */
            int size = ((insn >> 6) & 3) == 0 ? 8
                     : ((insn >> 6) & 3) == 1 ? 16 : 32;
            uint32_t v = ea_read(cpu, ea_mode, ea_reg, size);
            flag_NZ_clearVC(&cpu->sr, v, size);
            return;
        }
        if ((insn & 0xFFF8) == 0x4840) {             /* SWAP Dn */
            uint32_t v = cpu->d[insn & 7];
            cpu->d[insn & 7] = (v >> 16) | (v << 16);
            flag_NZ_clearVC(&cpu->sr, cpu->d[insn & 7], 32);
            return;
        }
        if ((insn & 0xFFC0) == 0x4840) {              /* PEA <ea> */
            push32(cpu, ea_calc(cpu, ea_mode, ea_reg, 32));
            return;
        }
        if ((insn & 0xFB80) == 0x4880) {              /* MOVEM */
            uint16_t mask = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            int is_long = (insn >> 6) & 1;
            int to_mem  = !((insn >> 10) & 1);
            int sz      = is_long ? 4 : 2;

            if (to_mem) {
                if (ea_mode == 4) {                    /* -(An) prédécrément */
                    uint32_t addr = cpu->a[ea_reg];
                    for (int i = 0; i < 16; i++) {
                        if (mask & (1 << i)) {
                            uint32_t val = (i < 8) ? cpu->a[7 - i] : cpu->d[15 - i];
                            addr -= sz;
                            if (is_long) bus_write32(cpu->bus, addr, val);
                            else         bus_write16(cpu->bus, addr, val & 0xFFFF);
                        }
                    }
                    cpu->a[ea_reg] = addr;
                } else {                               /* autre mode : ordre normal */
                    uint32_t addr = ea_calc(cpu, ea_mode, ea_reg, 32);
                    for (int i = 0; i < 16; i++) {
                        if (mask & (1 << i)) {
                            uint32_t val = (i < 8) ? cpu->d[i] : cpu->a[i - 8];
                            if (is_long) bus_write32(cpu->bus, addr, val);
                            else         bus_write16(cpu->bus, addr, val & 0xFFFF);
                            addr += sz;
                        }
                    }
                }
            } else {
                if (ea_mode == 3) {                    /* (An)+ postincrément */
                    uint32_t addr = cpu->a[ea_reg];
                    for (int i = 0; i < 16; i++) {
                        if (mask & (1 << i)) {
                            uint32_t val = is_long ? m68k_read32(cpu, addr)
                                                : (uint32_t)(int32_t)(int16_t)m68k_read16(cpu, addr);
                            if (i < 8) cpu->d[i]     = val;
                            else       cpu->a[i - 8] = val;
                            addr += sz;
                        }
                    }
                    cpu->a[ea_reg] = addr;
                } else {
                    uint32_t addr = ea_calc(cpu, ea_mode, ea_reg, 32);
                    for (int i = 0; i < 16; i++) {
                        if (mask & (1 << i)) {
                            uint32_t val = is_long ? m68k_read32(cpu, addr)
                                                : (uint32_t)(int32_t)(int16_t)m68k_read16(cpu, addr);
                            if (i < 8) cpu->d[i]     = val;
                            else       cpu->a[i - 8] = val;
                            addr += sz;
                        }
                    }
                }
            }
            return;
        }
        return;  /* 0x4xxx non implémenté → silently continue (was: stopped=1) */
    }

    /* ---------- 0x5 : ADDQ / SUBQ / Scc / DBcc ---------- */
    case 0x5: {
        int size_field = (insn >> 6) & 3;

        if (size_field == 3) {
            if (ea_mode == 1) {                       /* DBcc Dn, disp16 */
                uint32_t insn_pc = cpu->pc - 2;
                int16_t disp = (int16_t)m68k_read16(cpu, cpu->pc);
                uint32_t disp_base = cpu->pc;
                cpu->pc += 2;
                uint32_t cible = disp_base + (uint32_t)(int32_t)disp;

                if (!cc_true(cpu->sr, cc)) {
                    /* empty-loop short-circuit: target == DBcc itself */
                    if (cible == insn_pc) {
                        cpu->d[ea_reg] = (cpu->d[ea_reg] & 0xFFFF0000) | 0xFFFF;
                        return;
                    }
                    uint16_t counter = (uint16_t)(cpu->d[ea_reg] & 0xFFFF) - 1;
                    cpu->d[ea_reg] = (cpu->d[ea_reg] & 0xFFFF0000) | counter;
                    if (counter != 0xFFFF)
                        cpu->pc = cible;
                }
                return;
            } else {                                  /* Scc <ea> */
                uint8_t val = cc_true(cpu->sr, cc) ? 0xFF : 0x00;
                ea_write(cpu, ea_mode, ea_reg, 8, val);
                return;
            }
        } else {
            int size = (size_field == 0) ? 8 : (size_field == 1) ? 16 : 32;
            uint8_t data = (insn >> 9) & 7;
            if (data == 0) data = 8;
            int is_sub = (insn >> 8) & 1;

            if (ea_mode == 1) {                       /* ADDQ/SUBQ An : 32-bit, no flags */
                cpu->a[ea_reg] = is_sub ? (cpu->a[ea_reg] - data)
                                        : (cpu->a[ea_reg] + data);
                return;
            }
            uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
            uint32_t res = is_sub ? (val - data) : (val + data);
            ea_write(cpu, ea_mode, ea_reg, size, res);
            flag_NZ_clearVC(&cpu->sr, res, size);
            return;
        }
    }

    /* ---------- 0x6 : Bcc / BRA / BSR ---------- */
    case 0x6: {
        int8_t disp8 = (int8_t)(insn & 0xFF);
        int32_t disp;
        if (disp8 == 0) {
            disp = (int16_t)m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
        } else {
            disp = disp8;
        }
        if (cc == 1) {                                /* BSR */
            push32(cpu, cpu->pc);
            cpu->pc = base + (uint32_t)disp;
        } else if (cc_true(cpu->sr, cc)) {            /* Bcc / BRA */
            cpu->pc = base + (uint32_t)disp;
        }
        return;
    }

    /* ---------- 0x7 : MOVEQ ---------- */
    case 0x7:
        cpu->d[(insn >> 9) & 7] = (uint32_t)(int32_t)(int8_t)(insn & 0xFF);
        flag_NZ_clearVC(&cpu->sr, cpu->d[(insn >> 9) & 7], 32);
        return;

    /* ---------- 0x8 : OR ---------- */
    case 0x8: {
        uint32_t val = ea_read(cpu, ea_mode, ea_reg, 16);
        cpu->d[(insn >> 9) & 7] |= (val & 0xFFFF);
        flag_NZ_clearVC(&cpu->sr, cpu->d[(insn >> 9) & 7], 16);
        return;
    }

    /* ---------- 0x9 : SUB / SUBA ---------- */
    case 0x9: {
        int size = ((insn & 0x00C0) == 0x00C0) ? 32 : 16;
        uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
        uint8_t dn  = (insn >> 9) & 7;
        if ((insn & 0x00C0) == 0x00C0 && ((insn >> 8) & 1) == 0) { /* SUBA */
            cpu->a[dn] -= val;
        } else {
            uint32_t res = cpu->d[dn] - val;
            cpu->d[dn] = res;
            flag_NZ_clearVC(&cpu->sr, res, size);
        }
        return;
    }

    /* ---------- 0xB : CMP / CMPA / EOR ---------- */
    case 0xB: {
        uint8_t dn     = (insn >> 9) & 7;
        uint8_t opmode = (insn >> 6) & 7;

        /* CMPA */
        if (opmode == 0x3 || opmode == 0x7) {
            int size = (opmode == 0x7) ? 32 : 16;
            uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
            if (size == 16) val = (uint32_t)(int32_t)(int16_t)val;
            uint32_t res = cpu->a[dn] - val;
            flag_sub(&cpu->sr, cpu->a[dn], val, res, 32);
            return;
        }

        int size = (opmode & 3) == 0 ? 8 : (opmode & 3) == 1 ? 16 : 32;

        if (opmode & 0x4) {                            /* EOR Dn, <ea> */
            uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
            uint32_t res = val ^ (cpu->d[dn] & mask_of(size));
            ea_write(cpu, ea_mode, ea_reg, size, res);
            flag_NZ_clearVC(&cpu->sr, res, size);
        } else {                                        /* CMP <ea>, Dn */
            uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
            uint32_t d   = cpu->d[dn];
            uint32_t res = d - val;
            flag_sub(&cpu->sr, d, val, res, size);
        }
        return;
    }

    /* ---------- 0xC : AND / MULU / MULS / ABCD / EXG ---------- */
    case 0xC: {
        int dn     = (insn >> 9) & 7;
        int opmode = (insn >> 6) & 7;
        int mode   = (insn >> 3) & 7;
        int reg    =  insn & 7;

        /* MULU : opmode=3 */
        if (opmode == 3) {
            uint16_t src = (uint16_t)ea_read(cpu, mode, reg, 16);
            cpu->d[dn] = (cpu->d[dn] & 0xFFFF) * src;
            set_nz_flags32(cpu, cpu->d[dn]);
            cpu->sr &= ~(SR_V | SR_C);
            return;
        }
        /* MULS : opmode=7 */
        if (opmode == 7) {
            int16_t src = (int16_t)ea_read(cpu, mode, reg, 16);
            int32_t res = (int16_t)(cpu->d[dn] & 0xFFFF) * src;
            cpu->d[dn] = (uint32_t)res;
            set_nz_flags32(cpu, (uint32_t)res);
            cpu->sr &= ~(SR_V | SR_C);
            return;
        }
        /* ABCD */
        if ((insn & 0x01F0) == 0x0100) {
            /* non implémenté — silently skip */
            return;
        }
        /* EXG */
        if ((insn & 0x0100) &&
            (((insn >> 3) & 0x1F) == 0x08 ||
             ((insn >> 3) & 0x1F) == 0x09 ||
             ((insn >> 3) & 0x1F) == 0x11)) {
            int exmode = (insn >> 3) & 0x1F;
            int ry    = reg;
            uint32_t tmp;
            if (exmode == 0x08) {              /* Dx <-> Dy */
                tmp = cpu->d[dn]; cpu->d[dn] = cpu->d[ry]; cpu->d[ry] = tmp;
            } else if (exmode == 0x09) {        /* Ax <-> Ay */
                tmp = cpu->a[dn]; cpu->a[dn] = cpu->a[ry]; cpu->a[ry] = tmp;
            } else {                           /* Dx <-> Ay */
                tmp = cpu->d[dn]; cpu->d[dn] = cpu->a[ry]; cpu->a[ry] = tmp;
            }
            return;
        }
        /* AND */
        {
            int size = (opmode & 3) == 0 ? 8 : (opmode & 3) == 1 ? 16 : 32;
            int dir  = (opmode >> 2) & 1;

            if (dir == 0) {                     /* Dn = Dn AND <ea> */
                uint32_t src = ea_read(cpu, mode, reg, size);
                uint32_t dst = cpu->d[dn];
                uint32_t res;
                if (size == 8) {
                    res = (dst & ~0xFFu) | ((dst & src) & 0xFF);
                    set_nz_flags8(cpu, res & 0xFF);
                } else if (size == 16) {
                    res = (dst & ~0xFFFFu) | ((dst & src) & 0xFFFF);
                    set_nz_flags16(cpu, res & 0xFFFF);
                } else {
                    res = dst & src;
                    set_nz_flags32(cpu, res);
                }
                cpu->d[dn] = res;
            } else {                            /* <ea> = <ea> AND Dn */
                uint32_t src    = ea_read(cpu, mode, reg, size);
                uint32_t dn_val = cpu->d[dn];
                uint32_t res = src & dn_val;
                if (size == 8)      set_nz_flags8 (cpu, res & 0xFF);
                else if (size == 16) set_nz_flags16(cpu, res & 0xFFFF);
                else                set_nz_flags32(cpu, res);
                ea_write(cpu, mode, reg, size, res);
            }
            cpu->sr &= ~(SR_V | SR_C);
            return;
        }
    }

    /* ---------- 0xD : ADD / ADDA ---------- */
    case 0xD: {
        int dn     = (insn >> 9) & 7;
        int opmode = (insn >> 6) & 7;
        int mode   = (insn >> 3) & 7;
        int reg    =  insn & 7;

        /* ADDA */
        if (opmode == 3 || opmode == 7) {
            int size = (opmode == 7) ? 32 : 16;
            uint32_t src = ea_read(cpu, mode, reg, size);
            if (size == 16) src = (uint32_t)(int32_t)(int16_t)src;
            cpu->a[dn] += src;
            return;
        }

        int size = (opmode & 3) == 0 ? 8 : (opmode & 3) == 1 ? 16 : 32;

        if (opmode & 4) {                         /* <ea> = <ea> + Dn */
            if (mode == 0) {                       /* destination = Dn */
                uint32_t s   = cpu->d[reg];
                uint32_t res = s + cpu->d[dn];
                if      (size == 8)  cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (res & 0xFF);
                else if (size == 16) cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (res & 0xFFFF);
                else                 cpu->d[reg] = res;
                flag_NZ_clearVC(&cpu->sr, res, size);
            } else {
                uint32_t ea = ea_calc(cpu, mode, reg, size);
                uint32_t s;
                if      (size == 8)  s = m68k_read8 (cpu, ea);
                else if (size == 16) s = m68k_read16(cpu, ea);
                else                 s = m68k_read32(cpu, ea);
                uint32_t res = s + cpu->d[dn];
                if      (size == 8)  bus_write8 (cpu->bus, ea, res & 0xFF);
                else if (size == 16) bus_write16(cpu->bus, ea, res & 0xFFFF);
                else                 bus_write32(cpu->bus, ea, res);
                flag_NZ_clearVC(&cpu->sr, res, size);
            }
        } else {                                  /* Dn = Dn + <ea> */
            uint32_t s   = ea_read(cpu, mode, reg, size);
            uint32_t d   = cpu->d[dn];
            uint32_t res = d + s;
            if      (size == 8)  cpu->d[dn] = (d & 0xFFFFFF00) | (res & 0xFF);
            else if (size == 16) cpu->d[dn] = (d & 0xFFFF0000) | (res & 0xFFFF);
            else                 cpu->d[dn] = res;
            flag_NZ_clearVC(&cpu->sr, res, size);
        }
        return;
    }

    case 0xE:   /* 1110 : shift / rotate */
        if (((insn >> 6) & 3) == 3) {
            /* forme memoire : shift 1 bit sur EA - a implementer plus tard */
            fprintf(stderr, "[M68K] shift memoire non gere: 0x%04X\n", insn);
            cpu->halted = 1;
        } else {
            op_shift_reg(cpu, insn);   /* forme registre */
        }
        return;

    /* ---------- default : opcode inconnu ---------- */
    default:
        fprintf(stderr,
            "[M68K] OPCODE NON GERE: 0x%04X @ PC=0x%08X\n",
            insn, cpu->pc - 2);   /* -2 car PC a deja avance apres le fetch */
        cpu->halted = 1;
        return;
    }
}
