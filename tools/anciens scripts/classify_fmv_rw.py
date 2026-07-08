# classify_fmv_rw.py — Classifie chaque registre FMV en READ / WRITE / RW
# Analyse les instructions MOVE 68000 qui touchent les adresses $00E0xxxx
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: python classify_fmv_rw.py <module.bin>")
        sys.exit(1)

    path = sys.argv[1]
    data = open(path, "rb").read()
    print(f"=== Classification R/W — {path} ({len(data)} octets) ===\n")

    # regs[addr] = {'R':n, 'W':n, 'ctx':[...]}
    regs = {}

    def note(addr, mode, off, ctx):
        e = regs.setdefault(addr, {'R': 0, 'W': 0, 'ctx': []})
        e[mode] += 1
        if len(e['ctx']) < 4:
            e['ctx'].append((off, ctx))

    i = 0
    n = len(data)
    while i < n - 5:
        # On cherche une adresse absolue longue $00E0xxxx dans le flux
        if data[i] == 0x00 and data[i+1] == 0xE0:
            addr = (data[i] << 24) | (data[i+1] << 16) | (data[i+2] << 8) | data[i+3]
            if 0x00E00000 <= addr <= 0x00E0FFFF:
                # L'opcode précède l'adresse de 2 octets (mode .L absolu)
                if i >= 2:
                    op = (data[i-2] << 8) | data[i-1]
                else:
                    op = 0
                mode, label = classify_opcode(op, addr)
                if mode:
                    note(addr, mode, i-2, label)
                i += 4
                continue
        i += 1

    # Rapport
    if not regs:
        print("  Aucune adresse $00E0xxxx en mode absolu .L trouvée.")
        print("  (Le module utilise peut-être l'indirection par registre A6.)\n")
        return

    print(f"  {'ADRESSE':<12} {'R':>3} {'W':>3}  SENS      INSTRUCTION(S)")
    print("  " + "-" * 60)
    for a in sorted(regs):
        e = regs[a]
        if e['R'] and e['W']:
            sens = "RW"
        elif e['W']:
            sens = "WRITE"
        elif e['R']:
            sens = "READ"
        else:
            sens = "?"
        ctxs = ", ".join(f"{c[1]}@{c[0]:#06x}" for c in e['ctx'])
        print(f"  ${a:08X}  {e['R']:>3} {e['W']:>3}  {sens:<8}  {ctxs}")

    print(f"\n  {len(regs)} registres classés.")


def classify_opcode(op, addr):
    """Détermine si l'opcode MOVE lit (source=abs) ou écrit (dest=abs)
    une adresse absolue longue. Renvoie (mode, label)."""
    top4 = (op >> 12) & 0xF

    # MOVE.x : 00=?, 01=.B, 11=.W, 10=.L (bits 13-12)
    size_bits = (op >> 12) & 0x3
    size_map = {1: '.B', 3: '.W', 2: '.L'}

    # Champs mode/registre 68000
    dst_mode = (op >> 6) & 0x7
    dst_reg  = (op >> 9) & 0x7
    src_mode = (op >> 3) & 0x7
    src_reg  = op & 0x7

    # Mode 7 reg 1 = absolu long (.L)
    is_abs_l = lambda m, r: (m == 7 and r == 1)

    if top4 in (0x1, 0x2, 0x3):  # famille MOVE
        sz = size_map.get(size_bits, '')
        # destination = absolu long  -> WRITE vers le registre MMIO
        if is_abs_l(dst_mode, dst_reg):
            return ('W', f"MOVE{sz}->reg")
        # source = absolu long -> READ depuis le registre MMIO
        if is_abs_l(src_mode, src_reg):
            return ('R', f"MOVE{sz}<-reg")

    # TST.x abs (0x4A..) -> READ
    if (op & 0xFF00) == 0x4A00:
        return ('R', "TST reg")

    # CLR.x abs (0x42..) -> WRITE (écrit 0)
    if (op & 0xFF00) == 0x4200:
        return ('W', "CLR reg")

    # BTST/BCLR/BSET sur abs -> RW (lecture-modif-écriture)
    if (op & 0xF000) == 0x0000 and is_abs_l(src_mode, src_reg):
        return ('R', "BTST/bit reg")

    return (None, None)


if __name__ == "__main__":
    main()
