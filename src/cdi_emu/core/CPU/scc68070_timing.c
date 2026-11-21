/*
 * scc68070_timing.c - instruction durations of the SCC68070
 *
 * From the Philips SCC68070 product specification (November 1990),
 * "INSTRUCTION TIMING", tables 7 to 19 (docs/SCC68070.PDF, pages 25-29).
 * The values are internal clock periods (two crystal periods): at the
 * CD-i 205's 15 MHz CPU clock, exactly the emulator's cycle unit.
 *
 * The duration is computed before the instruction runs (it needs the
 * register values for shift counts, DBcc / TRAPV conditions, MOVEM masks).
 * Bus wait states are not counted (the tables assume none).
 */

#include "scc68070_timing.h"
#include "../bus.h"

/* Addressing mode index: 0 Dn, 1 An, 2 (An), 3 (An)+, 4 -(An), 5 d(An),
 * 6 d(An,Xi), 7 abs.W, 8 abs.L, 9 d(PC), 10 d(PC,Xi), 11 #imm */
static int mode_index(int mode, int reg)
{
    if (mode < 7) return mode;
    return reg <= 4 ? 7 + reg : 11;
}

/* Table 7: effective address calculation (.B/.W and .L) */
static const unsigned char EA_BW[12] = { 0, 0, 4, 4, 7, 11, 14, 8, 12, 11, 14, 4 };
static const unsigned char EA_L [12] = { 0, 0, 8, 8, 11, 15, 18, 12, 16, 15, 18, 8 };

static int ea_time(int mi, int lng) { return lng ? EA_L[mi] : EA_BW[mi]; }

/* Tables 8 / 9: MOVE, rows = source (Rn, (An), (An)+, -(An), d(An),
 * d(An,Xi), xxx.S, xxx.L, d(PC), d(PC,Xi), #xxx), columns = destination
 * (Rn, (An), (An)+, -(An), d(An), d(An,Xi), xxx.S, xxx.L) */
static const unsigned char MOVE_BW[11][8] = {
    {  7, 11, 11, 14, 18, 21, 15, 19 }, { 11, 15, 15, 18, 22, 25, 19, 23 },
    { 11, 15, 15, 18, 22, 25, 19, 23 }, { 14, 18, 18, 21, 25, 28, 22, 26 },
    { 18, 22, 22, 25, 29, 32, 26, 30 }, { 21, 25, 25, 28, 32, 35, 29, 33 },
    { 15, 19, 19, 22, 26, 29, 23, 27 }, { 19, 23, 23, 26, 30, 33, 27, 31 },
    { 18, 22, 22, 25, 29, 32, 26, 30 }, { 21, 25, 25, 28, 32, 35, 29, 33 },
    { 11, 15, 15, 18, 22, 25, 19, 23 },
};
static const unsigned char MOVE_L[11][8] = {
    {  7, 15, 15, 18, 22, 25, 19, 23 }, { 15, 23, 23, 26, 30, 33, 27, 31 },
    { 15, 23, 23, 26, 30, 33, 27, 31 }, { 18, 26, 26, 29, 33, 36, 30, 34 },
    { 22, 30, 30, 33, 37, 40, 34, 38 }, { 25, 33, 33, 36, 40, 43, 37, 41 },
    { 19, 27, 27, 30, 34, 37, 31, 35 }, { 23, 31, 31, 34, 38, 41, 35, 39 },
    { 22, 30, 30, 33, 37, 40, 34, 38 }, { 25, 33, 33, 36, 40, 43, 37, 41 },
    { 15, 23, 23, 26, 30, 33, 27, 31 },
};

/* Table 16, columns (An), (An)+, -(An), d(An), d(An,Xi), xxx.S, xxx.L,
 * d(PC), d(PC,Xi) -> index mi - 2 */
static const unsigned char JMP_T[9]   = {  7, 0, 0, 14, 17, 14, 18, 14, 17 };  /* JMP, LEA */
static const unsigned char JSR_T[9]   = { 18, 0, 0, 25, 28, 25, 29, 25, 28 };  /* JSR, PEA */
static const unsigned char MOVEM_MR[9] = { 26, 26, 0, 30, 33, 30, 34, 30, 33 };
static const unsigned char MOVEM_RM[9] = { 23, 0, 23, 27, 30, 27, 31, 0, 0 };

static int col16(const unsigned char *t, int mi)
{
    int v = (mi >= 2 && mi <= 10) ? t[mi - 2] : 0;
    return v ? v : 14;
}

/* Condition codes (Bcc / DBcc / Scc / TRAPV) */
static int cond_true(uint16_t sr, int cc)
{
    int c = sr & 1, v = (sr >> 1) & 1, z = (sr >> 2) & 1, n = (sr >> 3) & 1;
    switch (cc) {
    case 0:  return 1;
    case 1:  return 0;
    case 2:  return !c && !z;
    case 3:  return c || z;
    case 4:  return !c;
    case 5:  return c;
    case 6:  return !z;
    case 7:  return z;
    case 8:  return !v;
    case 9:  return v;
    case 10: return !n;
    case 11: return n;
    case 12: return n == v;
    case 13: return n != v;
    case 14: return !z && n == v;
    default: return z || n != v;
    }
}

static int popcount16(unsigned m)
{
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

/* Table 19 */
int scc68070_interrupt_cycles(void) { return 65; }

int scc68070_cycles(const m68k_t *cpu)
{
    uint16_t op = bus_read16(cpu->bus, cpu->pc);
    int mode = (op >> 3) & 7, reg = op & 7;
    int mi = mode_index(mode, reg);
    int size = (op >> 6) & 3;                       /* 0 B, 1 W, 2 L (most groups) */
    int lng = (size == 2);
    int mem = (mi >= 2);                            /* <ea> is not a register */

    switch (op >> 12) {
    case 0x1: case 0x2: case 0x3: {                 /* MOVE / MOVEA (tables 8, 9) */
        int dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        int di = mode_index(dmode, dreg);
        di = di <= 1 ? 0 : di - 1;                  /* Dn / An (MOVEA) = Rn column */
        if (di > 7) di = 7;
        int si = mi <= 1 ? 0 : mi - 1;              /* Dn / An = Rn row */
        if (si > 10) si = 10;
        return ((op >> 12) == 2) ? MOVE_L[si][di] : MOVE_BW[si][di];
    }

    case 0x0:
        if (op & 0x0100) {                          /* dynamic bit ops / MOVEP */
            if (mode == 1) {                        /* MOVEP (table 18) */
                int l = op & 0x40, w = op & 0x80;   /* w: register to memory */
                return w ? (l ? 39 : 25) : (l ? 36 : 22);
            }
            int t = (op >> 6) & 3;                  /* 0 BTST, 1 BCHG, 2 BCLR, 3 BSET */
            if (!mem) return t ? 10 : 7;
            return (t ? 14 : 7) + ea_time(mi, 0);
        }
        if ((op & 0x0F00) == 0x0800) {              /* static bit ops (#n) */
            int t = (op >> 6) & 3;
            if (!mem) return t ? 17 : 14;
            return (t ? 21 : 14) + ea_time(mi, 0);
        }
        if ((op & 0x00FF) == 0x003C || (op & 0x00FF) == 0x007C)
            return 14;                              /* ORI/ANDI/EORI to CCR / SR */
        {                                           /* ORI ANDI SUBI ADDI EORI CMPI */
            int cmpi = (op & 0x0F00) == 0x0C00;
            if (!mem) return lng ? 18 : 14;
            if (cmpi) return (lng ? 18 : 14) + ea_time(mi, lng);
            return (lng ? 26 : 18) + ea_time(mi, lng);
        }

    case 0x4:
        if (op == 0x4E71) return 7;                 /* NOP */
        if (op == 0x4E70) return 154;               /* RESET */
        if (op == 0x4E72) return 13;                /* STOP */
        if (op == 0x4E73) return 39;                /* RTE, short format */
        if (op == 0x4E75) return 15;                /* RTS */
        if (op == 0x4E76) return (cpu->sr & 2) ? 55 : 10;   /* TRAPV */
        if (op == 0x4E77) return 22;                /* RTR */
        if (op == 0x4AFC) return 55;                /* ILLEGAL */
        if ((op & 0xFFF0) == 0x4E40) return 52;     /* TRAP */
        if ((op & 0xFFF8) == 0x4E50) return 25;     /* LINK */
        if ((op & 0xFFF8) == 0x4E58) return 15;     /* UNLK */
        if ((op & 0xFFF0) == 0x4E60) return 7;      /* MOVE USP */
        if ((op & 0xFFC0) == 0x4E80) return col16(JSR_T, mi);   /* JSR */
        if ((op & 0xFFC0) == 0x4EC0) return col16(JMP_T, mi);   /* JMP */
        if ((op & 0xF1C0) == 0x41C0) return col16(JMP_T, mi);   /* LEA */
        if ((op & 0xF1C0) == 0x4180) return 19 + ea_time(mi, 0); /* CHK (not taken) */
        if ((op & 0xFFC0) == 0x4840)                /* SWAP / PEA */
            return mode == 0 ? 7 : col16(JSR_T, mi);
        if ((op & 0xFB80) == 0x4880) {              /* EXT / MOVEM */
            if (mode == 0) return 7;                /* EXT */
            int l = op & 0x40;
            int n = popcount16(bus_read16(cpu->bus, cpu->pc + 2));
            if (op & 0x0400)                        /* memory to registers */
                return col16(MOVEM_MR, mi) + n * (l ? 11 : 7);
            return col16(MOVEM_RM, mi) + n * (l ? 11 : 7);
        }
        if ((op & 0xFFC0) == 0x4800)                /* NBCD */
            return mem ? 14 + ea_time(mi, 0) : 10;
        if ((op & 0xFFC0) == 0x4AC0)                /* TAS */
            return mem ? 15 + ea_time(mi, 0) - 4 : 10;
        if ((op & 0xFFC0) == 0x40C0)                /* MOVE from SR */
            return mem ? 11 + ea_time(mi, 0) : 7;
        if ((op & 0xFDC0) == 0x44C0)                /* MOVE to CCR / SR */
            return 10 + ea_time(mi, 0);
        if (size != 3) {                            /* NEGX CLR NEG NOT TST */
            int sub = (op >> 8) & 0xF;
            if (sub == 0xA) return 7 + (mem ? ea_time(mi, lng) : 0);   /* TST */
            if (!mem) return 7;
            int t = (lng ? 15 : 11) + ea_time(mi, lng);
            if (sub == 0x2) t -= lng ? 8 : 4;       /* CLR: no read */
            return t;
        }
        return 14;

    case 0x5:
        if (size == 3) {
            if (mode == 1) {                        /* DBcc */
                int cc = (op >> 8) & 0xF;
                return cond_true(cpu->sr, cc) ? 14 : 17;
            }
            return mem ? 17 + ea_time(mi, 0) : 13;  /* Scc */
        }
        if (!mem) return 7;                         /* ADDQ / SUBQ */
        return (lng ? 15 : 11) + ea_time(mi, lng);

    case 0x6: {                                     /* BRA / BSR / Bcc */
        int word = (op & 0xFF) == 0;
        if ((op & 0x0F00) == 0x0100) return word ? 22 : 17;   /* BSR */
        return word ? 14 : 13;
    }

    case 0x7:
        return 7;                                   /* MOVEQ */

    case 0x8: case 0xC:                             /* OR / AND / DIV / MUL / BCD / EXG */
        if ((op & 0xF0C0) == 0x80C0)                /* DIVU / DIVS */
            return ((op & 0x0100) ? 169 : 130) + ea_time(mi, 0);
        if ((op & 0xF0C0) == 0xC0C0)                /* MULU / MULS */
            return 76 + ea_time(mi, 0);
        if ((op & 0xF1F0) == 0x8100 || (op & 0xF1F0) == 0xC100)  /* SBCD / ABCD */
            return (op & 8) ? 31 : 10;
        if ((op & 0xF1F8) == 0xC140 || (op & 0xF1F8) == 0xC148 || (op & 0xF1F8) == 0xC188)
            return 13;                              /* EXG */
        if (op & 0x0100)                            /* Dn,<ea> */
            return (lng ? 15 : 11) + ea_time(mi, lng);
        return 7 + ea_time(mi, lng);                /* <ea>,Dn */

    case 0x9: case 0xD:                             /* SUB / ADD (+A, +X) */
        if (size == 3)                              /* SUBA / ADDA */
            return 7 + ea_time(mi, op & 0x0100);
        if ((op & 0x0130) == 0x0100)                /* SUBX / ADDX */
            return (op & 8) ? (lng ? 40 : 28) : 7;
        if (op & 0x0100)
            return (lng ? 15 : 11) + ea_time(mi, lng);
        return 7 + ea_time(mi, lng);

    case 0xB:                                       /* CMP / CMPA / CMPM / EOR */
        if (size == 3) return 7 + ea_time(mi, op & 0x0100);      /* CMPA */
        if ((op & 0x0138) == 0x0108) return lng ? 26 : 18;       /* CMPM */
        if (op & 0x0100) return mem ? (lng ? 15 : 11) + ea_time(mi, lng) : 7;  /* EOR */
        return 7 + ea_time(mi, lng);                /* CMP */

    case 0xE:
        if (size == 3) return 14 + ea_time(mi, 0);  /* memory shift: one bit */
        {
            int count = (op >> 9) & 7;
            if (op & 0x20) count = (int)(cpu->d[count] & 63);   /* count in Dn */
            else if (count == 0) count = 8;
            return 13 + 3 * count;
        }

    default:                                        /* line A / line F */
        return 55;
    }
}
