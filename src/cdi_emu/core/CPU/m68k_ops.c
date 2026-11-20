/*
 * m68k_ops.c - M68000 instruction decoder and executor
 *
 * This is the main opcode dispatch for the custom M68000 interpreter.
 * Instructions are decoded by the top nibble (bits 15-12) into groups,
 * then further sub-decoded by their specific bit fields.
 *
 * Implemented groups:
 *   0x0 - Immediate operations (ORI, ANDI, SUBI, ADDI, EORI, CMPI)
 *         and bit operations (BTST, BCHG, BCLR, BSET)
 *   0x1/0x2/0x3 - MOVE / MOVEA
 *   0x4 - Miscellaneous (NEG, NOT, CLR, TST, JMP, JSR, RTS, RTE,
 *         RTR, LEA, PEA, SWAP, MOVEM, MOVE USP/An, NOP)
 *   0x5 - ADDQ, SUBQ, Scc, DBcc
 *   0x6 - Bcc, BRA, BSR
 *   0x7 - MOVEQ
 *   0x8 - OR
 *   0x9 - SUB, SUBA
 *   0xB - CMP, CMPA, CMPM, EOR
 *   0xC - AND, MULU, MULS, EXG (ABCD stub)
 *   0xD - ADD, ADDA
 *   0xE - Shift/rotate on data registers
 */

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
 * Flag helpers
 * ================================================================ */

/* Set N and Z flags for an 8-bit result */
static inline void set_nz_flags8(m68k_t *cpu, uint8_t val) {
    cpu->sr &= ~(SR_N | SR_Z);
    if (val == 0)     cpu->sr |= SR_Z;
    if (val & 0x80)  cpu->sr |= SR_N;
}

/* Set N and Z flags for a 16-bit result */
static inline void set_nz_flags16(m68k_t *cpu, uint16_t val) {
    cpu->sr &= ~(SR_N | SR_Z);
    if (val == 0)      cpu->sr |= SR_Z;
    if (val & 0x8000)  cpu->sr |= SR_N;
}

/* Set N and Z flags for a 32-bit result */
static inline void set_nz_flags32(m68k_t *cpu, uint32_t val) {
    cpu->sr &= ~(SR_N | SR_Z);
    if (val == 0)           cpu->sr |= SR_Z;
    if (val & 0x80000000U)  cpu->sr |= SR_N;
}

/*
 * Set N and Z flags, clear V and C flags.
 * Used by logical operations and MOVE.
 */
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

/* Return the bit mask for the given operand size */
static inline uint32_t mask_of(int size) {
    return (size == 8) ? 0xFFu
         : (size == 16) ? 0xFFFFu
         : 0xFFFFFFFFu;
}

/* Read and write the X (extend) flag */
static inline int  get_x(m68k_t *cpu)      { return (cpu->sr >> 4) & 1; }
static inline void set_x(m68k_t *cpu, int b){ if (b) cpu->sr |= SR_X; else cpu->sr &= ~SR_X; }


/* ================================================================
 * Condition code evaluation
 *
 * Each Bcc/Scc/DBcc instruction encodes a 4-bit condition code (cc).
 * This function evaluates whether the condition is true given the
 * current SR flags.
 * ================================================================ */

static int cc_true(uint16_t sr, int cc) {
    int C = (sr >> 0) & 1;
    int V = (sr >> 1) & 1;
    int Z = (sr >> 2) & 1;
    int N = (sr >> 3) & 1;
    switch (cc) {
        case 0x0: return 1;             /* T  - True            */
        case 0x1: return 0;             /* F  - False           */
        case 0x2: return !C && !Z;       /* HI - Higher          */
        case 0x3: return C || Z;         /* LS - Lower or Same   */
        case 0x4: return !C;             /* CC - Carry Clear     */
        case 0x5: return C;              /* CS - Carry Set       */
        case 0x6: return !Z;             /* NE - Not Equal       */
        case 0x7: return Z;              /* EQ - Equal           */
        case 0x8: return !V;             /* VC - Overflow Clear  */
        case 0x9: return V;              /* VS - Overflow Set    */
        case 0xA: return !N;            /* PL - Plus            */
        case 0xB: return N;              /* MI - Minus           */
        case 0xC: return (N == V);       /* GE - Greater or Equal */
        case 0xD: return (N != V);       /* LT - Less Than       */
        case 0xE: return !Z && (N == V); /* GT - Greater Than    */
        case 0xF: return Z || (N != V);  /* LE - Less or Equal   */
    }
    return 0;
}

/* ================================================================
 * SUB flag calculation
 *
 * Computes N, Z, V, C flags for a subtraction (d - s = r).
 * Used by CMP, SUB, and other subtract-like operations.
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

/* SUB/SUBQ/SUBX: same as flag_sub, and X = C */
static void flag_sub_x(uint16_t *sr, uint32_t d, uint32_t s, uint32_t r, int size) {
    flag_sub(sr, d, s, r, size);
    if (*sr & SR_C) *sr |= SR_X; else *sr &= ~SR_X;
}

/* ADD/ADDQ: computes X, N, Z, V, C for d + s = r */
static void flag_add_x(uint16_t *sr, uint32_t d, uint32_t s, uint32_t r, int size) {
    uint32_t msb  = (size == 8) ? 0x80u : (size == 16) ? 0x8000u : 0x80000000u;
    uint32_t mask = mask_of(size);
    r &= mask; d &= mask; s &= mask;

    *sr &= ~(SR_X | SR_N | SR_Z | SR_V | SR_C);
    if (r == 0)                      *sr |= SR_Z;
    if (r & msb)                     *sr |= SR_N;
    if ((~(d ^ s) & (d ^ r)) & msb)  *sr |= SR_V;
    if (r < d)                       *sr |= SR_C | SR_X;  /* carry out */
}

/* ================================================================
 * Stack helpers
 * ================================================================ */

/* Push a 32-bit value onto the supervisor stack (A7) */
static void push32(m68k_t *cpu, uint32_t val)
{
    cpu->a[7] -= 4;
    bus_write32(cpu->bus, cpu->a[7], val);
}

/* ================================================================
 * Status register / exceptions (SCC68070 datasheet, "Exception
 * processing", "Stack format", Fig.14/Fig.16)
 * ================================================================ */

/* ADDX / SUBX: Dy,Dx or -(Ay),-(Ax). Z is only cleared (never set). */
static void op_addx_subx(m68k_t *cpu, uint16_t insn, int is_sub)
{
    int size = ((insn >> 6) & 3) == 0 ? 8 : ((insn >> 6) & 3) == 1 ? 16 : 32;
    int rx = (insn >> 9) & 7, ry = insn & 7;
    int mem = (insn >> 3) & 1;
    uint32_t mask = mask_of(size);
    uint32_t x = (cpu->sr & SR_X) ? 1u : 0u;
    uint32_t src, dst, addr = 0;

    if (mem) {
        src  = ea_read(cpu, 4, (uint8_t)ry, size);          /* -(Ay) */
        addr = ea_calc(cpu, 4, (uint8_t)rx, size);          /* -(Ax) */
        dst  = (size == 8) ? m68k_read8(cpu, addr)
             : (size == 16) ? m68k_read16(cpu, addr) : m68k_read32(cpu, addr);
    } else {
        src = cpu->d[ry] & mask;
        dst = cpu->d[rx] & mask;
    }

    uint32_t res = (is_sub ? (dst - src - x) : (dst + src + x)) & mask;
    uint32_t msb = (size == 8) ? 0x80u : (size == 16) ? 0x8000u : 0x80000000u;
    uint16_t z_keep = cpu->sr & SR_Z;
    int carry;
    cpu->sr &= ~(SR_X | SR_N | SR_Z | SR_V | SR_C);
    if (is_sub) {
        carry = ((uint64_t)src + x) > dst;
        if (((src ^ dst) & (dst ^ res)) & msb) cpu->sr |= SR_V;
    } else {
        carry = ((uint64_t)dst + src + x) > mask;
        if ((~(dst ^ src) & (dst ^ res)) & msb) cpu->sr |= SR_V;
    }
    if (carry)     cpu->sr |= SR_C | SR_X;
    if (res & msb) cpu->sr |= SR_N;
    if (res == 0)  cpu->sr |= z_keep;          /* Z unchanged when result is 0 */

    if (mem) {
        if (size == 8)       bus_write8 (cpu->bus, addr, (uint8_t)res);
        else if (size == 16) bus_write16(cpu->bus, addr, (uint16_t)res);
        else                 bus_write32(cpu->bus, addr, res);
    } else if (size == 8)  cpu->d[rx] = (cpu->d[rx] & 0xFFFFFF00u) | res;
    else if (size == 16)   cpu->d[rx] = (cpu->d[rx] & 0xFFFF0000u) | res;
    else                   cpu->d[rx] = res;
}

void m68k_set_sr(m68k_t *cpu, uint16_t new_sr)
{
    new_sr &= 0xA71F;                     /* T, S, I2..I0, X N Z V C */
    int was_s = (cpu->sr & 0x2000) != 0;
    int is_s  = (new_sr  & 0x2000) != 0;
    if (was_s && !is_s) {                 /* supervisor -> user */
        cpu->ssp  = cpu->a[7];
        cpu->a[7] = cpu->usp;
    } else if (!was_s && is_s) {          /* user -> supervisor */
        cpu->usp  = cpu->a[7];
        cpu->a[7] = cpu->ssp;
    }
    cpu->sr = new_sr;
}

void m68k_exception(m68k_t *cpu, int vector)
{
    uint16_t old_sr = cpu->sr;

    /* Supervisor state, trace off */
    m68k_set_sr(cpu, (uint16_t)((old_sr | 0x2000) & ~0x8000));

    /* Short stack frame (4 words): SR at the lowest address, then PC,
     * then the format/vector word (format 0000, vector number << 2). */
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7],
                                (uint16_t)((M68K_FRAME_SHORT << 12) | ((vector & 0xFF) << 2)));
    push32(cpu, cpu->pc);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], old_sr);

    /* Vector table at $000000 (no VBR on the 68070) */
    cpu->pc = m68k_read32(cpu, (uint32_t)(vector & 0xFF) * 4u);
    cpu->stopped = 0;
}

/* ================================================================
 * Address Error exception (vector 3)
 *
 * Triggered when the CPU attempts a word or longword access on an
 * odd address. Pushes a 14-byte exception frame (7 words) and
 * loads the handler address from the vector table.
 * ================================================================ */

void m68k_address_error(m68k_t *cpu, uint32_t fault_addr, uint32_t ir)
{
    uint16_t old_sr = cpu->sr;
    uint32_t old_pc = cpu->pc;

    /* Enter supervisor mode (with stack swap), clear trace */
    m68k_set_sr(cpu, (uint16_t)((old_sr | 0x2000) & ~0x8000));

    /* Push 7-word exception frame (68000 layout).
     * TODO: the SCC68070 uses its 17-word long format (format code $F)
     * for address/bus errors; RTE below skips it when it sees format $F. */
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], old_pc & 0xFFFF);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], old_pc >> 16);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], old_sr);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], ir & 0xFFFF);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], fault_addr & 0xFFFF);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], fault_addr >> 16);
    cpu->a[7] -= 2; bus_write16(cpu->bus, cpu->a[7], 0x0009);  /* SSW */

    /* Load handler from vector 3 (offset 0x0C in the table at $180000) */
    cpu->pc = m68k_read32(cpu, 0x00000Cu);   /* vector 3, table at $000000 */
}


/* ================================================================
 * Shift/Rotate on data register
 *
 * Instruction format: 1110 ccc d ss i tt rrr
 *   ccc = shift count (immediate if i=0, or register Dn if i=1)
 *   d   = direction (0=right, 1=left)
 *   ss  = size (00=byte, 01=word, 10=long)
 *   i   = count source (0=immediate, 1=register)
 *   tt  = operation type (00=AS, 01=LS, 10=ROX, 11=RO)
 *   rrr = target data register
 * ================================================================ */

void op_shift_reg(m68k_t *cpu, uint16_t op) {
    int count_field = (op >> 9) & 7;
    int dir         = (op >> 8) & 1;      /* 1 = left */
    int size        = (op >> 6) & 3;      /* 0=byte, 1=word, 2=long */
    int ir          = (op >> 5) & 1;      /* 0=immediate count, 1=register count */
    int type        = (op >> 3) & 3;      /* 0=AS, 1=LS, 2=ROX, 3=RO */
    int reg         = op & 7;

    uint32_t count;
    if (ir)
        count = cpu->d[count_field] & 63;
    else
        count = (count_field == 0) ? 8 : count_field;

    uint32_t mask; int bits;
    switch (size) {
        case 0: mask = 0x000000FF; bits = 8;  break;
        case 1: mask = 0x0000FFFF; bits = 16; break;
        default:mask = 0xFFFFFFFF; bits = 32; break;
    }

    uint32_t res = cpu->d[reg] & mask;
    int carry = 0, overflow = 0;
    int xflag = get_x(cpu);

    for (uint32_t i = 0; i < count; i++) {
        if (dir == 0) {  /* Right shift/rotate */
            switch (type) {
                case 0: /* ASR - Arithmetic shift right (preserves sign) */
                    carry = res & 1;
                    res = (res >> 1) | (res & (1u << (bits - 1)));
                    break;
                case 1: /* LSR - Logical shift right */
                    carry = res & 1;
                    res >>= 1;
                    break;
                case 2: /* ROXR - Rotate right through extend */
                    {
                        int newc = res & 1;
                        res = (res >> 1) | ((uint32_t)xflag << (bits - 1));
                        carry = newc; xflag = newc;
                    }
                    break;
                case 3: /* ROR - Rotate right */
                    carry = res & 1;
                    res = (res >> 1) | ((uint32_t)carry << (bits - 1));
                    break;
            }
        } else {         /* Left shift/rotate */
            uint32_t msb = res & (1u << (bits - 1));
            switch (type) {
                case 0: /* ASL - Arithmetic shift left */
                    carry = msb ? 1 : 0;
                    res <<= 1;
                    if ((res & (1u << (bits-1))) != msb) overflow = 1;
                    break;
                case 1: /* LSL - Logical shift left */
                    carry = msb ? 1 : 0;
                    res <<= 1;
                    break;
                case 2: /* ROXL - Rotate left through extend */
                    {
                        int newc = msb ? 1 : 0;
                        res = (res << 1) | (xflag & 1);
                        carry = newc; xflag = newc;
                    }
                    break;
                case 3: /* ROL - Rotate left */
                    carry = msb ? 1 : 0;
                    res = (res << 1) | (msb ? 1 : 0);
                    break;
            }
        }
        res &= mask;
    }

    res &= mask;
    cpu->d[reg] = (cpu->d[reg] & ~mask) | res;

    int N = (res & (1u << (bits - 1))) ? 1 : 0;
    int Z = (res == 0) ? 1 : 0;
    int V = (type == 0 && dir == 1) ? overflow : 0;

    if (count == 0) {
        /* Zero count: C unaffected (ROX uses X) */
        carry = (type == 2) ? get_x(cpu) : 0;
    }

    /* Update flags: X N Z V C */
    cpu->sr &= ~0x1F;
    if (carry) cpu->sr |= SR_C;
    if (V)     cpu->sr |= SR_V;
    if (Z)     cpu->sr |= SR_Z;
    if (N)     cpu->sr |= SR_N;

    /* RO (type 3) does not affect X; other types update X */
    if (type != 3) {
        set_x(cpu, (type == 2) ? xflag : carry);
    } else {
        set_x(cpu, get_x(cpu));  /* X unchanged */
    }
}

/* ================================================================
 * Main instruction decoder
 *
 * Fetches the opcode word from the current PC, decodes the top
 * nibble to determine the instruction group, and dispatches
 * to the appropriate handler logic.
 * ================================================================ */

void m68k_step_ops(m68k_t *cpu)
{
    /* Check for odd PC (address error) */
    if (cpu->pc & 1) {
        m68k_address_error(cpu, cpu->pc, 0x0000);
        return;
    }

    uint16_t insn = m68k_read16(cpu, cpu->pc);
    cpu->pc += 2;

    /* Extract common fields from the instruction word */
    uint8_t nib0    = (uint8_t)(insn >> 12);       /* Top nibble: group selector */
    uint8_t ea_mode = (uint8_t)((insn >> 3) & 0x07); /* EA mode field */
    uint8_t ea_reg  = (uint8_t)(insn & 0x07);        /* EA register field */
    int     cc      = (insn >> 8) & 0xF;             /* Condition code */
    uint32_t base   = cpu->pc;                       /* Base for branch offsets */

    switch (nib0) {

    /* ================================================================
     * Group 0x0: Immediate operations + bit operations
     * ================================================================ */
    case 0x0: {

        /* --- Special forms on CCR/SR --- */

        if (insn == 0x003C) {                 /* ORI #imm8, CCR */
            uint16_t imm = m68k_read16(cpu, cpu->pc) & 0xFF;
            cpu->pc += 2;
            cpu->sr = (cpu->sr & ~0x001F) | ((cpu->sr | imm) & 0x001F);
            return;
        }
        if (insn == 0x007C) {                 /* ORI #imm16, SR */
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            m68k_set_sr(cpu, cpu->sr | imm);
            return;
        }
        if (insn == 0x023C) {                 /* ANDI #imm8, CCR */
            uint16_t imm = m68k_read16(cpu, cpu->pc) & 0xFF;
            cpu->pc += 2;
            cpu->sr = (cpu->sr & ~0x001F) | ((cpu->sr & (imm | ~0x1F)) & 0x001F);
            return;
        }
        if (insn == 0x027C) {                 /* ANDI #imm16, SR */
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            m68k_set_sr(cpu, cpu->sr & imm);
            return;
        }
        if (insn == 0x0A3C) {                 /* EORI #imm8, CCR */
            uint16_t imm = m68k_read16(cpu, cpu->pc) & 0xFF;
            cpu->pc += 2;
            cpu->sr = (cpu->sr & ~0x001F) | (((cpu->sr ^ imm)) & 0x001F);
            return;
        }
        if (insn == 0x0A7C) {                 /* EORI #imm16, SR */
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            m68k_set_sr(cpu, cpu->sr ^ imm);
            return;
        }

        /* --- Bit operations using register Dn as bit number --- */
        /* Format: 0000 nnn 1 tt mmmrrr (bit 8 = 1) */
        if ((insn & 0x0100) && ((insn >> 12) & 0xF) == 0x0) {
            int dn  = (insn >> 9) & 7;
            int op  = (insn >> 6) & 3;
            int size = (ea_mode == 0) ? 32 : 8;   /* Dn: 32-bit, memory: 8-bit */
            int bit  = cpu->d[dn] & ((size == 32) ? 31 : 7);
            uint32_t mask = 1u << bit;

            uint32_t val = ea_read_rmw(cpu, ea_mode, ea_reg, size);

            cpu->sr &= ~SR_Z;
            if (!(val & mask)) cpu->sr |= SR_Z;

            switch (op) {
                case 0: /* BTST - test only, no write */              break;
                case 1: val ^=  mask; ea_write_rmw(cpu, ea_mode, ea_reg, size, val); break; /* BCHG */
                case 2: val &= ~mask; ea_write_rmw(cpu, ea_mode, ea_reg, size, val); break; /* BCLR */
                case 3: val |=  mask; ea_write_rmw(cpu, ea_mode, ea_reg, size, val); break; /* BSET */
            }
            return;
        }

        /* --- Bit operations with immediate bit number --- */
        /* Format: 0000 100 tt mmmrrr (bits 11-8 = 1000) */
        if ((insn & 0x0F00) == 0x0800) {
            uint16_t bitsel = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            int op   = (insn >> 6) & 3;
            int size = (ea_mode == 0) ? 32 : 8;
            int bit  = bitsel & ((size == 32) ? 31 : 7);
            uint32_t mask = 1u << bit;

            uint32_t val = ea_read_rmw(cpu, ea_mode, ea_reg, size);

            cpu->sr &= ~SR_Z;
            if (!(val & mask)) cpu->sr |= SR_Z;

            switch (op) {
                case 0: /* BTST */                                      break;
                case 1: val ^=  mask; ea_write_rmw(cpu, ea_mode, ea_reg, size, val); break; /* BCHG */
                case 2: val &= ~mask; ea_write_rmw(cpu, ea_mode, ea_reg, size, val); break; /* BCLR */
                case 3: val |=  mask; ea_write_rmw(cpu, ea_mode, ea_reg, size, val); break; /* BSET */
            }
            return;
        }

        /* --- Immediate arithmetic/logic operations --- */
        /* Format: 0000 ttt s mmmrrr
         *   ttt: 000=ORI 001=ANDI 010=SUBI 011=ADDI 101=EORI 110=CMPI
         *   s:   00=byte 01=word 10=long */
        {
            int subop = (insn >> 9) & 7;
            int size  = (insn >> 6) & 3;

            if (subop != 4 && subop != 7 && size != 3) {
                int bits = (size == 0) ? 8 : (size == 1) ? 16 : 32;

                /* Read the immediate operand */
                uint32_t imm;
                if (bits == 32) {
                    imm = ((uint32_t)m68k_read16(cpu, cpu->pc) << 16)
                        |  (uint32_t)m68k_read16(cpu, cpu->pc + 2);
                    cpu->pc += 4;
                } else {
                    imm = m68k_read16(cpu, cpu->pc);
                    imm &= (bits == 8) ? 0xFF : 0xFFFF;
                    cpu->pc += 2;
                }

                uint32_t dst  = ea_read_rmw(cpu, ea_mode, ea_reg, bits);
                uint32_t mask = (bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1);
                uint32_t msb  = 1u << (bits - 1);
                uint32_t res;

                switch (subop) {
                    case 0: res = (dst | imm) & mask; break;   /* ORI  */
                    case 1: res = (dst & imm) & mask; break;   /* ANDI */
                    case 2: res = (dst - imm) & mask; break;   /* SUBI */
                    case 3: res = (dst + imm) & mask; break;   /* ADDI */
                    case 5: res = (dst ^ imm) & mask; break;   /* EORI */
                    case 6: res = (dst - imm) & mask; break;   /* CMPI */
                    default: res = dst & mask; break;
                }

                /* --- Flag updates --- */
                if (subop == 0 || subop == 1 || subop == 5) {
                    /* Logical ops: set N,Z; clear V,C; leave X unchanged */
                    cpu->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
                    if (res & msb)     cpu->sr |= SR_N;
                    if (res == 0)      cpu->sr |= SR_Z;
                } else {
                    /* Arithmetic ops: set N,Z,V,C; X follows C for SUBI/ADDI */
                    uint32_t a = dst & mask, b = imm & mask, r = res & mask;
                    cpu->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
                    if (r & msb)   cpu->sr |= SR_N;
                    if (r == 0)    cpu->sr |= SR_Z;

                    if (subop == 3) {          /* ADDI: a + b */
                        if (((a & b & ~r) | (~a & ~b & r)) & msb) cpu->sr |= SR_V;
                        if (((a & b) | (b & ~r) | (a & ~r)) & msb) cpu->sr |= SR_C;
                    } else {                   /* SUBI / CMPI: a - b */
                        if (((~a & b & r) | (a & ~b & ~r)) & msb) cpu->sr |= SR_V;
                        if (((~a & b) | (b & r) | (~a & r)) & msb) cpu->sr |= SR_C;
                    }

                    /* X = C only for SUBI and ADDI (not CMPI) */
                    if (subop == 2 || subop == 3) {
                        if (cpu->sr & SR_C) cpu->sr |= SR_X;
                        else                cpu->sr &= ~SR_X;
                    }
                }

                /* CMPI never writes back the result */
                if (subop != 6)
                    ea_write_rmw(cpu, ea_mode, ea_reg, bits, res);

                return;
            }
        }

        /* --- Unhandled opcode in group 0x0 --- */
        fprintf(stderr, "[M68K] OPCODE NON IMPLEMENTE: 0x%04X @ PC=0x%08X (case=0x0)\n",
                insn, cpu->pc - 2);
        cpu->halted = 1;
        return;
    }

    /* ================================================================
     * Groups 0x1/0x2/0x3: MOVE / MOVEA
     *   0x1 = byte, 0x2 = long, 0x3 = word
     * ================================================================ */
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

    /* ================================================================
     * Group 0x4: Miscellaneous control and logic operations
     * ================================================================ */
    case 0x4: {

        /* NEGX <ea> - Negate with extend */
        if ((insn & 0xFF00) == 0x4000 && ((insn >> 6) & 3) != 3) {
            int size = ((insn >> 6) & 3) == 0 ? 8 : ((insn >> 6) & 3) == 1 ? 16 : 32;
            uint32_t v = ea_read_rmw(cpu, ea_mode, ea_reg, size);
            uint32_t mask = mask_of(size);
            uint32_t x = (cpu->sr & SR_X) ? 1 : 0;
            uint32_t res = (0 - (v & mask) - x) & mask;
            flag_sub(&cpu->sr, 0, (v & mask) + x, res, size);
            /* NEGX: Z is only cleared when result is non-zero */
            if (res != 0) cpu->sr &= ~SR_Z;
            if (cpu->sr & SR_C) cpu->sr |= SR_X; else cpu->sr &= ~SR_X;
            ea_write_rmw(cpu, ea_mode, ea_reg, size, res);
            return;
        }

        /* MOVE SR,<ea> (68070: not privileged) */
        if ((insn & 0xFFC0) == 0x40C0) {
            ea_write(cpu, ea_mode, ea_reg, 16, cpu->sr);
            return;
        }

        /* MOVE <ea>,CCR - only the low byte is affected */
        if ((insn & 0xFFC0) == 0x44C0) {
            uint16_t v = (uint16_t)ea_read(cpu, ea_mode, ea_reg, 16);
            cpu->sr = (cpu->sr & 0xFF00) | (v & 0x001F);
            return;
        }

        /* MOVE <ea>,SR - privileged */
        if ((insn & 0xFFC0) == 0x46C0) {
            uint16_t v = (uint16_t)ea_read(cpu, ea_mode, ea_reg, 16);
            m68k_set_sr(cpu, v);
            return;
        }

        /* NEG <ea> - Negate */
        if ((insn & 0xFF00) == 0x4400 && ((insn >> 6) & 3) != 3) {
            int size = ((insn >> 6) & 3) == 0 ? 8 : ((insn >> 6) & 3) == 1 ? 16 : 32;
            uint32_t v = ea_read_rmw(cpu, ea_mode, ea_reg, size);
            uint32_t mask = mask_of(size);
            uint32_t res = (0 - (v & mask)) & mask;
            flag_sub(&cpu->sr, 0, v & mask, res, size);
            if (cpu->sr & SR_C) cpu->sr |= SR_X; else cpu->sr &= ~SR_X;
            ea_write_rmw(cpu, ea_mode, ea_reg, size, res);
            return;
        }

        /* NOT <ea> - Logical complement */
        if ((insn & 0xFF00) == 0x4600 && ((insn >> 6) & 3) != 3) {
            int size = ((insn >> 6) & 3) == 0 ? 8 : ((insn >> 6) & 3) == 1 ? 16 : 32;
            uint32_t v = ea_read_rmw(cpu, ea_mode, ea_reg, size);
            uint32_t mask = mask_of(size);
            uint32_t res = (~v) & mask;
            flag_NZ_clearVC(&cpu->sr, res, size);
            ea_write_rmw(cpu, ea_mode, ea_reg, size, res);
            return;
        }

        /* RTS - Return from subroutine */
        if (insn == 0x4E75) {
            cpu->pc   = m68k_read32(cpu, cpu->a[7]);
            cpu->a[7] += 4;
            return;
        }
        /* NOP - No operation */
        if (insn == 0x4E71) {
            return;
        }
        /* LINK An, #<disp> */
        if ((insn & 0xFFF8) == 0x4E50) {
            int reg = insn & 7;
            int16_t disp = (int16_t)m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            push32(cpu, cpu->a[reg]);
            cpu->a[reg] = cpu->a[7];
            cpu->a[7]  += (uint32_t)(int32_t)disp;
            return;
        }
        /* UNLK An */
        if ((insn & 0xFFF8) == 0x4E58) {
            int reg = insn & 7;
            cpu->a[7]   = cpu->a[reg];
            cpu->a[reg] = m68k_read32(cpu, cpu->a[7]);
            cpu->a[7]  += 4;
            return;
        }
        /* TRAPV - trap (vector 7) if V is set */
        if (insn == 0x4E76) {
            if (cpu->sr & SR_V) m68k_exception(cpu, 7);
            return;
        }
        /* CHK.W <ea>,Dn - trap (vector 6) if Dn < 0 or Dn > <ea> */
        if ((insn & 0xF1C0) == 0x4180) {
            int16_t bound = (int16_t)ea_read(cpu, ea_mode, ea_reg, 16);
            int16_t v     = (int16_t)cpu->d[(insn >> 9) & 7];
            if (v < 0)          { cpu->sr |= SR_N;  m68k_exception(cpu, 6); }
            else if (v > bound) { cpu->sr &= ~SR_N; m68k_exception(cpu, 6); }
            return;
        }
        /* TRAP #n - vectors 32..47 */
        if ((insn & 0xFFF0) == 0x4E40) {
            m68k_exception(cpu, 32 + (insn & 0x0F));
            return;
        }
        /* STOP #imm - load SR and wait for an interrupt (see m68k_step) */
        if (insn == 0x4E72) {
            uint16_t imm = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            m68k_set_sr(cpu, imm);
            cpu->stopped = 1;
            return;
        }
        /* RESET */
        if (insn == 0x4E70) {
            return;
        }
        /* RTR - Return and restore CCR */
        if (insn == 0x4E77) {
            uint16_t ccr = m68k_read16(cpu, cpu->a[7]);
            cpu->a[7] += 2;
            cpu->sr = (cpu->sr & 0xFF00) | (ccr & 0x00FF);
            cpu->pc   = m68k_read32(cpu, cpu->a[7]);
            cpu->a[7] += 4;
            return;
        }
        /* RTE - Return from exception */
        if (insn == 0x4E73) {
            uint16_t new_sr = m68k_read16(cpu, cpu->a[7]);
            uint32_t new_pc = m68k_read32(cpu, cpu->a[7] + 2);
            uint16_t fmt    = m68k_read16(cpu, cpu->a[7] + 6);
            switch (fmt >> 12) {
            case M68K_FRAME_SHORT: cpu->a[7] += 8;  break;    /* 4 words  */
            case M68K_FRAME_LONG:  cpu->a[7] += 34; break;    /* 17 words */
            default:
                /* Format error (vector 14): frame left in place */
                m68k_exception(cpu, 14);
                return;
            }
            cpu->pc = new_pc;
            m68k_set_sr(cpu, new_sr);                         /* may switch to USP */
            return;
        }
        /* MOVE An, USP */
        if ((insn & 0xFFF8) == 0x4E60) {
            cpu->usp = cpu->a[insn & 7];
            return;
        }
        /* MOVE USP, An */
        if ((insn & 0xFFF8) == 0x4E68) {
            cpu->a[insn & 7] = cpu->usp;
            return;
        }
        /* JMP <ea> */
        if ((insn & 0xFFC0) == 0x4EC0) {
            uint32_t target = ea_calc(cpu, ea_mode, ea_reg, 32);
            if (target & 1) {
                m68k_address_error(cpu, target, insn);
                return;
            }
            cpu->pc = target;
            return;
        }
        /* JSR <ea> */
        if ((insn & 0xFFC0) == 0x4E80) {
            uint32_t target = ea_calc(cpu, ea_mode, ea_reg, 32);
            push32(cpu, cpu->pc);
            cpu->pc = target;
            return;
        }
        /* LEA <ea>, An */
        if ((insn & 0xF1C0) == 0x41C0) {
            cpu->a[(insn >> 9) & 7] = ea_calc(cpu, ea_mode, ea_reg, 32);
            return;
        }
        /* CLR <ea> - Clear (set to zero) */
        if ((insn & 0xFF00) == 0x4200) {
            int size = ((insn >> 6) & 3) == 0 ? 8
                     : ((insn >> 6) & 3) == 1 ? 16 : 32;
            ea_write(cpu, ea_mode, ea_reg, size, 0);
            cpu->sr = (cpu->sr & ~(SR_N|SR_V|SR_C)) | SR_Z;
            return;
        }
        /* TST <ea> - Test (set N,Z flags) */
        if ((insn & 0xFF00) == 0x4A00) {
            int size = ((insn >> 6) & 3) == 0 ? 8
                     : ((insn >> 6) & 3) == 1 ? 16 : 32;
            uint32_t v = ea_read(cpu, ea_mode, ea_reg, size);
            flag_NZ_clearVC(&cpu->sr, v, size);
            return;
        }
        /* SWAP Dn - Swap upper and lower 16-bit halves */
        if ((insn & 0xFFF8) == 0x4840) {
            uint32_t v = cpu->d[insn & 7];
            cpu->d[insn & 7] = (v >> 16) | (v << 16);
            flag_NZ_clearVC(&cpu->sr, cpu->d[insn & 7], 32);
            return;
        }
        /* PEA <ea> - Push effective address */
        if ((insn & 0xFFC0) == 0x4840) {
            push32(cpu, ea_calc(cpu, ea_mode, ea_reg, 32));
            return;
        }
        /* EXT.W / EXT.L Dn - must be tested before MOVEM (same opcode
         * pattern with ea_mode = 0, which is not a valid MOVEM mode) */
        if ((insn & 0xFFF8) == 0x4880) {              /* EXT.W: byte -> word */
            uint32_t *r = &cpu->d[insn & 7];
            *r = (*r & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)(*r & 0xFF);
            flag_NZ_clearVC(&cpu->sr, *r, 16);
            return;
        }
        if ((insn & 0xFFF8) == 0x48C0) {              /* EXT.L: word -> long */
            uint32_t *r = &cpu->d[insn & 7];
            *r = (uint32_t)(int32_t)(int16_t)(*r & 0xFFFF);
            flag_NZ_clearVC(&cpu->sr, *r, 32);
            return;
        }
        /* MOVEM - Move multiple registers */
        if ((insn & 0xFB80) == 0x4880) {
            uint16_t mask = m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
            int is_long = (insn >> 6) & 1;
            int to_mem  = !((insn >> 10) & 1);
            int sz      = is_long ? 4 : 2;

            if (to_mem) {
                /* Register-to-memory mode */
                if (ea_mode == 4) {                    /* -(An) predecrement */
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
                } else {                               /* Other modes: ascending order */
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
                /* Memory-to-register mode */
                if (ea_mode == 3) {                    /* (An)+ postincrement */
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
        fprintf(stderr, "[M68K] OPCODE NON IMPLEMENTE: 0x%04X @ PC=0x%08X (case=0x4 misc-jmp-jsr)", insn, cpu->pc - 2);
        cpu->halted = 1;
        return;
    }

    /* ================================================================
     * Group 0x5: ADDQ / SUBQ / Scc / DBcc
     * ================================================================ */
    case 0x5: {
        int size_field = (insn >> 6) & 3;

        if (size_field == 3) {
            /* Scc or DBcc (size field = 3 selects conditional forms) */
            if (ea_mode == 1) {                       /* DBcc Dn, disp16 */
                uint32_t insn_pc = cpu->pc - 2;
                int16_t disp = (int16_t)m68k_read16(cpu, cpu->pc);
                uint32_t disp_base = cpu->pc;
                cpu->pc += 2;
                uint32_t cible = disp_base + (uint32_t)(int32_t)disp;

                if (!cc_true(cpu->sr, cc)) {
                    /* Condition false: decrement counter and branch if not expired */
                    if (cible == insn_pc) {
                        /* Self-loop short-circuit: set counter to -1 and exit */
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
            /* ADDQ / SUBQ */
            int size = (size_field == 0) ? 8 : (size_field == 1) ? 16 : 32;
            uint8_t data = (insn >> 9) & 7;
            if (data == 0) data = 8;    /* Encoding: 0 means 8 */
            int is_sub = (insn >> 8) & 1;

            if (ea_mode == 1) {                       /* ADDQ/SUBQ to An: 32-bit, no flags */
                cpu->a[ea_reg] = is_sub ? (cpu->a[ea_reg] - data)
                                        : (cpu->a[ea_reg] + data);
                return;
            }
            uint32_t val = ea_read_rmw(cpu, ea_mode, ea_reg, size);
            uint32_t res = is_sub ? (val - data) : (val + data);
            ea_write_rmw(cpu, ea_mode, ea_reg, size, res);
            if (is_sub) flag_sub_x(&cpu->sr, val, data, res, size);
            else        flag_add_x(&cpu->sr, val, data, res, size);
            return;
        }
        fprintf(stderr, "[M68K] OPCODE NON IMPLEMENTE: 0x%04X @ PC=0x%08X (case=0x5 addq-subq-scc)", insn, cpu->pc - 2);
        cpu->halted = 1;
        return;
    }

    /* ================================================================
     * Group 0x6: Bcc / BRA / BSR (conditional/unconditional branches)
     * ================================================================ */
    case 0x6: {
        int8_t disp8 = (int8_t)(insn & 0xFF);
        int32_t disp;
        if (disp8 == 0) {
            /* 16-bit displacement (from next extension word) */
            disp = (int16_t)m68k_read16(cpu, cpu->pc);
            cpu->pc += 2;
        } else {
            /* 8-bit displacement */
            disp = disp8;
        }
        if (cc == 1) {                                /* BSR - Branch to subroutine */
            push32(cpu, cpu->pc);
            cpu->pc = base + (uint32_t)disp;
        } else if (cc_true(cpu->sr, cc)) {            /* Bcc / BRA */
            cpu->pc = base + (uint32_t)disp;
        }
        return;
    }

    /* ================================================================
     * Group 0x7: MOVEQ - Move quick (sign-extended 8-bit immediate to Dn)
     * ================================================================ */
    case 0x7:
        cpu->d[(insn >> 9) & 7] = (uint32_t)(int32_t)(int8_t)(insn & 0xFF);
        flag_NZ_clearVC(&cpu->sr, cpu->d[(insn >> 9) & 7], 32);
        return;

    /* ================================================================
     * Group 0x8: OR / DIVU / DIVS / SBCD
     * ================================================================ */
    case 0x8: {
        int dn     = (insn >> 9) & 7;
        int opmode = (insn >> 6) & 7;

        if (opmode == 3 || opmode == 7) {         /* DIVU / DIVS <ea>,Dn (32/16) */
            uint16_t src = (uint16_t)ea_read(cpu, ea_mode, ea_reg, 16);
            if (src == 0) {                       /* zero divide: vector 5 */
                m68k_exception(cpu, 5);
                return;
            }
            uint32_t dividend = cpu->d[dn];
            cpu->sr &= ~(SR_N | SR_Z | SR_V | SR_C);
            if (opmode == 3) {
                uint32_t q = dividend / src, r = dividend % src;
                if (q > 0xFFFF) { cpu->sr |= SR_V; return; }   /* overflow: Dn unchanged */
                cpu->d[dn] = (r << 16) | q;
                if (q == 0) cpu->sr |= SR_Z;
                if (q & 0x8000) cpu->sr |= SR_N;
            } else {
                int32_t a = (int32_t)dividend, b = (int16_t)src;
                if (a == INT32_MIN && b == -1) { cpu->sr |= SR_V; return; }
                int32_t q = a / b, r = a % b;
                if (q > 32767 || q < -32768) { cpu->sr |= SR_V; return; }
                cpu->d[dn] = ((uint32_t)(uint16_t)r << 16) | (uint16_t)q;
                if (q == 0) cpu->sr |= SR_Z;
                if (q < 0)  cpu->sr |= SR_N;
            }
            return;
        }

        if (opmode == 4 && ea_mode <= 1) {        /* SBCD */
            fprintf(stderr, "[M68K] OPCODE NON IMPLEMENTE: 0x%04X @ PC=0x%08X (case=0x8 sbcd)\n",
                    insn, cpu->pc - 2);
            cpu->halted = 1;
            return;
        }

        int size = (opmode & 3) == 0 ? 8 : (opmode & 3) == 1 ? 16 : 32;
        if (opmode & 4) {                         /* OR Dn,<ea> */
            uint32_t v   = ea_read_rmw(cpu, ea_mode, ea_reg, size);
            uint32_t res = (v | cpu->d[dn]) & mask_of(size);
            ea_write_rmw(cpu, ea_mode, ea_reg, size, res);
            flag_NZ_clearVC(&cpu->sr, res, size);
        } else {                                  /* OR <ea>,Dn */
            uint32_t v   = ea_read(cpu, ea_mode, ea_reg, size);
            uint32_t res = (cpu->d[dn] | v) & mask_of(size);
            ea_write(cpu, 0, (uint8_t)dn, size, res);
            flag_NZ_clearVC(&cpu->sr, res, size);
        }
        return;
    }

    /* ================================================================
     * Group 0x9: SUB / SUBA
     * ================================================================ */
    case 0x9: {
        uint8_t dn     = (insn >> 9) & 7;
        int     opmode = (insn >> 6) & 7;

        /* SUBA - word source is sign-extended, no flags */
        if (opmode == 3 || opmode == 7) {
            int size = (opmode == 7) ? 32 : 16;
            uint32_t src = ea_read(cpu, ea_mode, ea_reg, size);
            if (size == 16) src = (uint32_t)(int32_t)(int16_t)src;
            cpu->a[dn] -= src;
            return;
        }

        int size = (opmode & 3) == 0 ? 8 : (opmode & 3) == 1 ? 16 : 32;

        if ((opmode & 4) && ea_mode >= 2) {       /* <ea> = <ea> - Dn */
            uint32_t ea = ea_calc(cpu, ea_mode, ea_reg, size);
            uint32_t d;
            if      (size == 8)  d = m68k_read8 (cpu, ea);
            else if (size == 16) d = m68k_read16(cpu, ea);
            else                 d = m68k_read32(cpu, ea);
            uint32_t s   = cpu->d[dn];
            uint32_t res = d - s;
            if      (size == 8)  bus_write8 (cpu->bus, ea, res & 0xFF);
            else if (size == 16) bus_write16(cpu->bus, ea, res & 0xFFFF);
            else                 bus_write32(cpu->bus, ea, res);
            flag_sub_x(&cpu->sr, d, s, res, size);
        } else if (!(opmode & 4)) {               /* Dn = Dn - <ea> */
            uint32_t s   = ea_read(cpu, ea_mode, ea_reg, size);
            uint32_t d   = cpu->d[dn];
            uint32_t res = d - s;
            if      (size == 8)  cpu->d[dn] = (d & 0xFFFFFF00) | (res & 0xFF);
            else if (size == 16) cpu->d[dn] = (d & 0xFFFF0000) | (res & 0xFFFF);
            else                 cpu->d[dn] = res;
            flag_sub_x(&cpu->sr, d, s, res, size);
        } else {
            op_addx_subx(cpu, insn, 1);           /* SUBX Dy,Dx / -(Ay),-(Ax) */
        }
        return;
    }

    /* ================================================================
     * Group 0xB: CMP / CMPA / CMPM / EOR
     * ================================================================ */
    case 0xB: {
        uint8_t dn     = (insn >> 9) & 7;
        uint8_t opmode = (insn >> 6) & 7;

        /* CMPA - Compare address (word or long) */
        if (opmode == 0x3 || opmode == 0x7) {
            int size = (opmode == 0x7) ? 32 : 16;
            uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
            if (size == 16) val = (uint32_t)(int32_t)(int16_t)val;
            uint32_t res = cpu->a[dn] - val;
            flag_sub(&cpu->sr, cpu->a[dn], val, res, 32);
            return;
        }

        int size = (opmode & 3) == 0 ? 8 : (opmode & 3) == 1 ? 16 : 32;

        if (opmode & 0x4) {
            /* opmode 4/5/6: CMPM if mode=001, otherwise EOR */
            if (ea_mode == 0x1) {
                /* CMPM (Ay)+, (Ax)+ */
                uint32_t src = ea_read(cpu, 0x3, ea_reg, size);
                uint32_t dst = ea_read(cpu, 0x3, dn,     size);
                uint32_t res = dst - src;
                flag_sub(&cpu->sr, dst, src, res, size);
                return;
            } else {
                /* EOR Dn, <ea> */
                uint32_t val = ea_read_rmw(cpu, ea_mode, ea_reg, size);
                uint32_t res = val ^ (cpu->d[dn] & mask_of(size));
                ea_write_rmw(cpu, ea_mode, ea_reg, size, res);
                flag_NZ_clearVC(&cpu->sr, res, size);
                return;
            }
        } else {
            /* CMP <ea>, Dn */
            uint32_t val = ea_read(cpu, ea_mode, ea_reg, size);
            uint32_t d   = cpu->d[dn];
            uint32_t res = d - val;
            flag_sub(&cpu->sr, d, val, res, size);
            return;
        }
    }

    /* ================================================================
     * Group 0xC: AND / MULU / MULS / EXG (ABCD stub)
     * ================================================================ */
    case 0xC: {
        int dn     = (insn >> 9) & 7;
        int opmode = (insn >> 6) & 7;
        int mode   = (insn >> 3) & 7;
        int reg    =  insn & 7;

        /* MULU - Unsigned multiply (word * word = long) */
        if (opmode == 3) {
            uint16_t src = (uint16_t)ea_read(cpu, mode, reg, 16);
            cpu->d[dn] = (cpu->d[dn] & 0xFFFF) * src;
            set_nz_flags32(cpu, cpu->d[dn]);
            cpu->sr &= ~(SR_V | SR_C);
            return;
        }
        /* MULS - Signed multiply (word * word = long) */
        if (opmode == 7) {
            int16_t src = (int16_t)ea_read(cpu, mode, reg, 16);
            int32_t res = (int16_t)(cpu->d[dn] & 0xFFFF) * src;
            cpu->d[dn] = (uint32_t)res;
            set_nz_flags32(cpu, (uint32_t)res);
            cpu->sr &= ~(SR_V | SR_C);
            return;
        }
        /* ABCD - Add binary coded decimal (not implemented) */
        if ((insn & 0x01F0) == 0x0100) {
            fprintf(stderr, "[M68K] OPCODE NON IMPLEMENTE: 0x%04X @ PC=0x%08X (case=0xC abcd)\n", insn, cpu->pc - 2);
            cpu->halted = 1;
            return;
        }
        /* EXG - Exchange two registers */
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
        /* AND - Logical AND */
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
                uint32_t src    = ea_read_rmw(cpu, mode, reg, size);
                uint32_t dn_val = cpu->d[dn];
                uint32_t res = src & dn_val;
                if (size == 8)      set_nz_flags8 (cpu, res & 0xFF);
                else if (size == 16) set_nz_flags16(cpu, res & 0xFFFF);
                else                set_nz_flags32(cpu, res);
                ea_write_rmw(cpu, mode, reg, size, res);
            }
            cpu->sr &= ~(SR_V | SR_C);
            return;
        }
    }

    /* ================================================================
     * Group 0xD: ADD / ADDA
     * ================================================================ */
    case 0xD: {
        int dn     = (insn >> 9) & 7;
        int opmode = (insn >> 6) & 7;
        int mode   = (insn >> 3) & 7;
        int reg    =  insn & 7;

        /* ADDA - Add address (word sign-extended or long) */
        if (opmode == 3 || opmode == 7) {
            int size = (opmode == 7) ? 32 : 16;
            uint32_t src = ea_read(cpu, mode, reg, size);
            if (size == 16) src = (uint32_t)(int32_t)(int16_t)src;
            cpu->a[dn] += src;
            return;
        }

        int size = (opmode & 3) == 0 ? 8 : (opmode & 3) == 1 ? 16 : 32;

        if ((opmode & 4) && mode <= 1) {          /* ADDX Dy,Dx / -(Ay),-(Ax) */
            op_addx_subx(cpu, insn, 0);
            return;
        }
        if (opmode & 4) {                         /* <ea> = <ea> + Dn */
            if (mode == 0) {                       /* (unreachable: ADDX above) */
                uint32_t s   = cpu->d[reg];
                uint32_t res = s + cpu->d[dn];
                if      (size == 8)  cpu->d[reg] = (cpu->d[reg] & 0xFFFFFF00) | (res & 0xFF);
                else if (size == 16) cpu->d[reg] = (cpu->d[reg] & 0xFFFF0000) | (res & 0xFFFF);
                else                 cpu->d[reg] = res;
                flag_add_x(&cpu->sr, s, cpu->d[dn], res, size);
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
                flag_add_x(&cpu->sr, s, cpu->d[dn], res, size);
            }
        } else {                                  /* Dn = Dn + <ea> */
            uint32_t s   = ea_read(cpu, mode, reg, size);
            uint32_t d   = cpu->d[dn];
            uint32_t res = d + s;
            if      (size == 8)  cpu->d[dn] = (d & 0xFFFFFF00) | (res & 0xFF);
            else if (size == 16) cpu->d[dn] = (d & 0xFFFF0000) | (res & 0xFFFF);
            else                 cpu->d[dn] = res;
            flag_add_x(&cpu->sr, d, s, res, size);
        }
        return;
    }

    /* ================================================================
     * Group 0xE: Shift / Rotate
     * ================================================================ */
    case 0xE:
        if (((insn >> 6) & 3) == 3) {
            /* Memory shift form (shift 1 bit on EA): not implemented */
            fprintf(stderr, "[M68K] OPCODE NON IMPLEMENTE: 0x%04X @ PC=0x%08X (case=0xE shift-mem)",
                    insn, cpu->pc - 2);
            cpu->halted = 1;
        } else {
            /* Register shift form */
            op_shift_reg(cpu, insn);
        }
        return;

    /* ================================================================
     * Default: unhandled opcode
     * ================================================================ */
    default:
        fprintf(stderr, "[M68K] OPCODE NON GERE: 0x%04X @ PC=0x%08X\n",
            insn, cpu->pc);
        cpu->halted = 1;
        return;
    }
}
