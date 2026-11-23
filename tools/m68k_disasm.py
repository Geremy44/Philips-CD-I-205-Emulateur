#!/usr/bin/env python3
"""
m68k_disasm.py - small 68000/68070 disassembler for the CD-i 205 ROM

Usage:
  python m68k_disasm.py ROM.bin START [END|+LEN] [--base 0x180000] [--swapped]

  ROM.bin    raw ROM image (the 205 dump in ROM/ is byte-swapped: use --swapped)
  START/END  CPU addresses (hex), e.g. 1B09E4 +200

Covers the 68000 integer instruction set (no FPU/MMU). Unknown words are
printed as dc.w. OS-9 system calls (TRAP #0 + function word) are shown as
"os9 F$xxx". Output: address, hex words, mnemonic.
"""

import sys

OS9_CALLS = {
    0x00: "F$Link", 0x01: "F$Load", 0x02: "F$UnLink", 0x03: "F$Fork", 0x04: "F$Wait",
    0x05: "F$Chain", 0x06: "F$Exit", 0x07: "F$Mem", 0x08: "F$Send", 0x09: "F$Icpt",
    0x0A: "F$Sleep", 0x0B: "F$SSpd", 0x0C: "F$ID", 0x0D: "F$SPrior", 0x0E: "F$STrap",
    0x0F: "F$PErr", 0x10: "F$PrsNam", 0x11: "F$CmpNam", 0x12: "F$SchBit", 0x13: "F$AllBit",
    0x14: "F$DelBit", 0x15: "F$Time", 0x16: "F$STime", 0x17: "F$CRC", 0x18: "F$GPrDsc",
    0x19: "F$GBlkMp", 0x1A: "F$GModDr", 0x1B: "F$CpyMem", 0x1C: "F$SUser", 0x1D: "F$UnLoad",
    0x1E: "F$RTE", 0x1F: "F$GPrDBT", 0x20: "F$Julian", 0x21: "F$TLink", 0x22: "F$DFork",
    0x23: "F$DExec", 0x24: "F$DExit", 0x25: "F$DatMod", 0x26: "F$SetCRC", 0x27: "F$SetSys",
    0x28: "F$SRqMem", 0x29: "F$SRtMem", 0x2A: "F$IRQ", 0x2B: "F$IOQu", 0x2C: "F$AProc",
    0x2D: "F$NProc", 0x2E: "F$VModul", 0x2F: "F$FindPD", 0x30: "F$AllPD", 0x31: "F$RetPD",
    0x32: "F$SSvc", 0x33: "F$IODel", 0x37: "F$GProcP", 0x38: "F$Move", 0x39: "F$AllRAM",
    0x3A: "F$Permit", 0x3B: "F$Protect", 0x3F: "F$AllTsk", 0x40: "F$DelTsk", 0x4B: "F$AllPrc",
    0x4C: "F$DelPrc", 0x4E: "F$FModul", 0x52: "F$SysDbg", 0x53: "F$Event", 0x54: "F$Gregor",
    0x55: "F$SysID", 0x56: "F$Alarm", 0x57: "F$SigMask", 0x58: "F$ChkMem", 0x59: "F$UAcct",
    0x5A: "F$CCtl", 0x5B: "F$GSPUMp", 0x5C: "F$SRqCMem", 0x5D: "F$POSK", 0x5E: "F$Panic",
    0x5F: "F$MBuf", 0x60: "F$Trans",
    0x80: "I$Attach", 0x81: "I$Detach", 0x82: "I$Dup", 0x83: "I$Create", 0x84: "I$Open",
    0x85: "I$MakDir", 0x86: "I$ChgDir", 0x87: "I$Delete", 0x88: "I$Seek", 0x89: "I$Read",
    0x8A: "I$Write", 0x8B: "I$ReadLn", 0x8C: "I$WritLn", 0x8D: "I$GetStt", 0x8E: "I$SetStt",
    0x8F: "I$Close", 0x92: "I$SGetSt",
}

CC = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]
SZ = {0: ".b", 1: ".w", 2: ".l"}


class Mem:
    def __init__(self, data, base):
        self.d, self.base = data, base

    def w(self, a):
        o = a - self.base
        if o < 0 or o + 1 >= len(self.d):
            raise IndexError
        return (self.d[o] << 8) | self.d[o + 1]

    def l(self, a):
        return (self.w(a) << 16) | self.w(a + 2)


def sx8(v):  return v - 0x100 if v & 0x80 else v
def sx16(v): return v - 0x10000 if v & 0x8000 else v


class Dis:
    def __init__(self, mem, pc):
        self.m, self.start, self.pc = mem, pc, pc

    def fetch(self):
        v = self.m.w(self.pc); self.pc += 2; return v

    def fetchl(self):
        v = self.m.l(self.pc); self.pc += 4; return v

    def ea(self, mode, reg, size):
        if mode == 0: return f"d{reg}"
        if mode == 1: return f"a{reg}"
        if mode == 2: return f"(a{reg})"
        if mode == 3: return f"(a{reg})+"
        if mode == 4: return f"-(a{reg})"
        if mode == 5: return f"{sx16(self.fetch())}(a{reg})"
        if mode == 6: return self.index(f"a{reg}", None)
        if reg == 0: return f"(${self.fetch():04x}).w"
        if reg == 1: return f"(${self.fetchl():08x}).l"
        if reg == 2:
            base = self.pc; d = sx16(self.fetch())
            return f"${(base + d) & 0xFFFFFFFF:06x}(pc)"
        if reg == 3:
            return self.index("pc", self.pc)
        if reg == 4:
            if size == 2: return f"#${self.fetchl():x}"
            v = self.fetch()
            return f"#${(v & 0xFF) if size == 0 else v:x}"
        return "?"

    def index(self, base_name, pc_base):
        ext = self.fetch()
        r = f"{'a' if ext & 0x8000 else 'd'}{(ext >> 12) & 7}.{'l' if ext & 0x800 else 'w'}"
        d = sx8(ext & 0xFF)
        if pc_base is not None:
            return f"${(pc_base + d) & 0xFFFFFFFF:06x}(pc,{r})"
        return f"{d}({base_name},{r})"

    def reglist(self, mask, predec):
        names = [f"d{i}" for i in range(8)] + [f"a{i}" for i in range(8)]
        if predec: names = names[::-1]
        return "/".join(n for i, n in enumerate(names) if mask & (1 << i)) or "none"

    def one(self):
        op = self.fetch()
        hi = op >> 12
        mode, reg = (op >> 3) & 7, op & 7
        rx = (op >> 9) & 7
        sz = (op >> 6) & 3

        if hi == 0:
            if op & 0x100 and mode != 1:
                n = ["btst", "bchg", "bclr", "bset"][sz]
                return f"{n} d{rx},{self.ea(mode, reg, 0)}"
            if op & 0x100 and mode == 1:
                return f"movep d{rx}/(a{reg})"
            if (op & 0xFF00) == 0x0800:
                n = ["btst", "bchg", "bclr", "bset"][sz]
                b = self.fetch() & 0xFF
                return f"{n} #{b},{self.ea(mode, reg, 0)}"
            names = {0: "ori", 2: "andi", 4: "subi", 6: "addi", 0xA: "eori", 0xC: "cmpi"}
            n = names.get((op >> 8) & 0xF)
            if n is None or sz == 3: return None
            if op & 0x3F == 0x3C:
                imm = self.fetch()
                return f"{n} #${imm:x},{'ccr' if sz == 0 else 'sr'}"
            imm = self.fetchl() if sz == 2 else self.fetch() & (0xFF if sz == 0 else 0xFFFF)
            return f"{n}{SZ[sz]} #${imm:x},{self.ea(mode, reg, sz)}"

        if hi in (1, 2, 3):
            s = {1: 0, 3: 1, 2: 2}[hi]
            src = self.ea(mode, reg, s)
            dm = (op >> 6) & 7
            if dm == 1: return f"movea{SZ[s]} {src},a{rx}"
            return f"move{SZ[s]} {src},{self.ea(dm, rx, s)}"

        if hi == 4:
            if op == 0x4E71: return "nop"
            if op == 0x4E75: return "rts"
            if op == 0x4E73: return "rte"
            if op == 0x4E77: return "rtr"
            if op == 0x4E76: return "trapv"
            if op == 0x4E70: return "reset"
            if op == 0x4E72: return f"stop #${self.fetch():04x}"
            if (op & 0xFFF0) == 0x4E40:
                if op & 0xF == 0:
                    code = self.fetch()
                    return f"os9 {OS9_CALLS.get(code, f'${code:02x}')}"
                return f"trap #{op & 0xF}"
            if (op & 0xFFF8) == 0x4E50: return f"link a{reg},#{sx16(self.fetch())}"
            if (op & 0xFFF8) == 0x4E58: return f"unlk a{reg}"
            if (op & 0xFFF8) == 0x4E60: return f"move a{reg},usp"
            if (op & 0xFFF8) == 0x4E68: return f"move usp,a{reg}"
            if (op & 0xFFC0) == 0x4E80: return f"jsr {self.ea(mode, reg, 2)}"
            if (op & 0xFFC0) == 0x4EC0: return f"jmp {self.ea(mode, reg, 2)}"
            if (op & 0xFFF8) == 0x4840: return f"swap d{reg}"
            if (op & 0xFFC0) == 0x4840: return f"pea {self.ea(mode, reg, 2)}"
            if (op & 0xFFF8) == 0x4880: return f"ext.w d{reg}"
            if (op & 0xFFF8) == 0x48C0: return f"ext.l d{reg}"
            if (op & 0xFB80) == 0x4880:
                mask = self.fetch()
                s = ".l" if op & 0x40 else ".w"
                e = self.ea(mode, reg, 2)
                if op & 0x400: return f"movem{s} {e},{self.reglist(mask, False)}"
                return f"movem{s} {self.reglist(mask, mode == 4)},{e}"
            if (op & 0xF1C0) == 0x41C0: return f"lea {self.ea(mode, reg, 2)},a{rx}"
            if (op & 0xF1C0) == 0x4180: return f"chk.w {self.ea(mode, reg, 1)},d{rx}"
            if (op & 0xFFC0) == 0x40C0: return f"move sr,{self.ea(mode, reg, 1)}"
            if (op & 0xFFC0) == 0x44C0: return f"move {self.ea(mode, reg, 1)},ccr"
            if (op & 0xFFC0) == 0x46C0: return f"move {self.ea(mode, reg, 1)},sr"
            if (op & 0xFFC0) == 0x4AC0: return f"tas {self.ea(mode, reg, 0)}"
            if (op & 0xFFC0) == 0x4800: return f"nbcd {self.ea(mode, reg, 0)}"
            names = {0x40: "negx", 0x42: "clr", 0x44: "neg", 0x46: "not", 0x4A: "tst"}
            n = names.get(op >> 8)
            if n and sz != 3: return f"{n}{SZ[sz]} {self.ea(mode, reg, sz)}"
            return None

        if hi == 5:
            if sz == 3:
                c = CC[(op >> 8) & 0xF]
                if mode == 1:
                    base = self.pc; d = sx16(self.fetch())
                    return f"db{c} d{reg},${(base + d) & 0xFFFFFFFF:06x}"
                return f"s{c} {self.ea(mode, reg, 0)}"
            q = rx or 8
            return f"{'subq' if op & 0x100 else 'addq'}{SZ[sz]} #{q},{self.ea(mode, reg, sz)}"

        if hi == 6:
            c = (op >> 8) & 0xF
            n = "bra" if c == 0 else "bsr" if c == 1 else "b" + CC[c]
            base = self.pc
            d = sx8(op & 0xFF)
            if d == 0: d = sx16(self.fetch()); n += ".w"
            else: n += ".s"
            return f"{n} ${(base + d) & 0xFFFFFFFF:06x}"

        if hi == 7:
            return f"moveq #{sx8(op & 0xFF)},d{rx}"

        if hi in (8, 9, 0xB, 0xC, 0xD):
            opm = (op >> 6) & 7
            base = {8: "or", 9: "sub", 0xB: "cmp", 0xC: "and", 0xD: "add"}[hi]
            if hi == 8 and opm in (3, 7): return f"div{'s' if opm == 7 else 'u'}.w {self.ea(mode, reg, 1)},d{rx}"
            if hi == 0xC and opm in (3, 7): return f"mul{'s' if opm == 7 else 'u'}.w {self.ea(mode, reg, 1)},d{rx}"
            if hi in (9, 0xB, 0xD) and opm in (3, 7):
                s = 2 if opm == 7 else 1
                return f"{base}a{SZ[s]} {self.ea(mode, reg, s)},a{rx}"
            s = opm & 3
            if opm & 4:
                if hi in (9, 0xD) and mode <= 1:
                    x = "subx" if hi == 9 else "addx"
                    return f"{x}{SZ[s]} {'-(a%d),-(a%d)' % (reg, rx) if mode else 'd%d,d%d' % (reg, rx)}"
                if hi == 0xB:
                    if mode == 1: return f"cmpm{SZ[s]} (a{reg})+,(a{rx})+"
                    return f"eor{SZ[s]} d{rx},{self.ea(mode, reg, s)}"
                if hi == 0xC and mode <= 1:
                    if opm == 5: return f"exg {'a' if mode else 'd'}{rx},{'a' if mode else 'd'}{reg}"
                    if opm == 6: return f"exg d{rx},a{reg}"
                    return f"abcd ..."
                if hi == 8 and mode <= 1: return "sbcd ..."
                return f"{base}{SZ[s]} d{rx},{self.ea(mode, reg, s)}"
            return f"{base}{SZ[s]} {self.ea(mode, reg, s)},d{rx}"

        if hi == 0xE:
            kinds = ["as", "ls", "rox", "ro"]
            d = "l" if op & 0x100 else "r"
            if sz == 3:
                return f"{kinds[(op >> 9) & 3]}{d}.w {self.ea(mode, reg, 1)}"
            k = kinds[(op >> 3) & 3]
            cnt = f"d{rx}" if op & 0x20 else f"#{rx or 8}"
            return f"{k}{d}{SZ[sz]} {cnt},d{reg}"

        if hi == 0xA: return f"line-a ${op:04x}"
        if hi == 0xF: return f"line-f ${op:04x}"
        return None

    def step(self):
        start = self.pc
        try:
            txt = self.one()
        except IndexError:
            txt = None
        if txt is None:
            self.pc = start + 2
            txt = f"dc.w ${self.m.w(start):04x}"
        words = [self.m.w(a) for a in range(start, self.pc, 2)]
        return start, words, txt


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = sys.argv[1:]
    if len(args) < 2:
        print(__doc__); return 1
    base = 0x180000
    if "--base" in opts:
        base = int(opts[opts.index("--base") + 1], 16)
        args = [a for a in args if a != opts[opts.index("--base") + 1]]
    data = bytearray(open(args[0], "rb").read())
    if "--swapped" in opts:
        for i in range(0, len(data) - 1, 2):
            data[i], data[i + 1] = data[i + 1], data[i]
    start = int(args[1], 16)
    end = start + 0x40
    if len(args) > 2:
        end = start + int(args[2][1:], 16) if args[2].startswith("+") else int(args[2], 16)
    dis = Dis(Mem(data, base), start)
    while dis.pc < end:
        a, words, txt = dis.step()
        hexw = " ".join(f"{x:04x}" for x in words)
        print(f"{a:06x}  {hexw:<20s} {txt}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
