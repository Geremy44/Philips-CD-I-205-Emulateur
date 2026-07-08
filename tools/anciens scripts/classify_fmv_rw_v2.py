# classify_fmv_rw_v2.py — Classifie les registres FMV via indirection An
# Détecte MOVEA.L #$00E0xxxx,An puis suit les accès (d16,An)
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: python classify_fmv_rw_v2.py <module.bin>")
        sys.exit(1)

    path = sys.argv[1]
    data = open(path, "rb").read()
    print(f"=== Classification R/W v2 (indirection An) — {path} ({len(data)} octets) ===\n")

    regs = {}  # addr -> {'R':n,'W':n,'ctx':[...]}

    def note(addr, mode, off, label):
        e = regs.setdefault(addr, {'R': 0, 'W': 0, 'ctx': []})
        e[mode] += 1
        if len(e['ctx']) < 5:
            e['ctx'].append((off, label))

    # État : base FMV courante par registre An (None si inconnu)
    an_base = [None] * 8     # A0..A7

    size_map = {1: '.B', 3: '.W', 2: '.L'}

    i = 0
    n = len(data)
    while i < n - 1:
        op = (data[i] << 8) | data[i+1]

        # --- MOVEA.L #imm32, An : opcode 0x2[An]7C ---
        # format : 0010 rrr 001 111 100 -> 0x2_7C avec rrr = An
        if (op & 0xF1FF) == 0x207C:
            an = (op >> 9) & 0x7
            if i + 6 <= n:
                imm = (data[i+2] << 24) | (data[i+3] << 16) | (data[i+4] << 8) | data[i+5]
                if 0x00E00000 <= imm <= 0x00E0FFFF:
                    an_base[an] = imm
                else:
                    an_base[an] = None
            i += 6
            continue

        # --- LEA $00E0xxxx, An : opcode 0x4[An]F9 (abs long) ---
        if (op & 0xF1FF) == 0x41F9:
            an = (op >> 9) & 0x7
            if i + 6 <= n:
                imm = (data[i+2] << 24) | (data[i+3] << 16) | (data[i+4] << 8) | data[i+5]
                if 0x00E00000 <= imm <= 0x00E0FFFF:
                    an_base[an] = imm
                else:
                    an_base[an] = None
            i += 6
            continue

        # --- Accès (d16,An) : mode=5, reg=An ---
        # Cherche dans MOVE / TST / CLR
        consumed = try_access(op, data, i, an_base, note, size_map)
        if consumed:
            i += consumed
            continue

        # Toute modif directe d'un An invalide la base (approx prudente)
        # MOVEA / ADDA / SUBA sur An -> on reset
        if (op & 0xF0C0) == 0x2040 or (op & 0xF0C0) == 0x3040:
            an = (op >> 9) & 0x7
            an_base[an] = None

        i += 2

    # Rapport
    if not regs:
        print("  Aucun accès (d16,An) vers $00E0xxxx détecté.\n")
        return

    print(f"  {'ADRESSE':<12} {'R':>3} {'W':>3}  SENS      INSTRUCTION(S)")
    print("  " + "-" * 64)
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

    print(f"\n  {len(regs)} registres classés (via indirection An).")


def try_access(op, data, i, an_base, note, size_map):
    """Tente de décoder un accès (d16,An) -> registre FMV.
    Renvoie le nb d'octets consommés (>0) ou 0."""

    dst_mode = (op >> 6) & 0x7
    dst_reg  = (op >> 9) & 0x7
    src_mode = (op >> 3) & 0x7
    src_reg  = op & 0x7
    top4     = (op >> 12) & 0xF
    size_bits = (op >> 12) & 0x3

    # mode 5 = (d16,An)
    # --- MOVE.x avec source (d16,An) -> READ ---
    if top4 in (0x1, 0x2, 0x3):
        sz = size_map.get(size_bits, '')
        # source (d16,An)
        if src_mode == 5 and an_base[src_reg] is not None:
            d16 = read_d16(data, i + 2)
            addr = (an_base[src_reg] + d16) & 0xFFFFFFFF
            note(addr, 'R', i, f"MOVE{sz}<-(d16,A{src_reg})")
            return 4
        # dest (d16,An)
        if dst_mode == 5 and an_base[dst_reg] is not None:
            d16 = read_d16(data, i + 2)
            addr = (an_base[dst_reg] + d16) & 0xFFFFFFFF
            note(addr, 'W', i, f"MOVE{sz}->(d16,A{dst_reg})")
            return 4

    # --- TST.x (d16,An) -> READ : opcode 0x4A.. mode5 ---
    if (op & 0xFF00) == 0x4A00 and src_mode == 5 and an_base[src_reg] is not None:
        d16 = read_d16(data, i + 2)
        addr = (an_base[src_reg] + d16) & 0xFFFFFFFF
        sz = size_map.get((op >> 6) & 0x3, '')
        note(addr, 'R', i, f"TST{sz} (d16,A{src_reg})")
        return 4

    # --- CLR.x (d16,An) -> WRITE : opcode 0x42.. mode5 ---
    if (op & 0xFF00) == 0x4200 and src_mode == 5 and an_base[src_reg] is not None:
        d16 = read_d16(data, i + 2)
        addr = (an_base[src_reg] + d16) & 0xFFFFFFFF
        sz = size_map.get((op >> 6) & 0x3, '')
        note(addr, 'W', i, f"CLR{sz} (d16,A{src_reg})")
        return 4

    # --- BTST/BSET/BCLR #n,(d16,An) -> RW ---
    if (op & 0xFF00) == 0x0800 and src_mode == 5 and an_base[src_reg] is not None:
        # opcode statique bit : suit un mot immédiat puis le d16
        bitnum = (data[i+2] << 8) | data[i+3]
        d16 = read_d16(data, i + 4)
        addr = (an_base[src_reg] + d16) & 0xFFFFFFFF
        note(addr, 'R', i, f"BTST(d16,A{src_reg})")
        return 6

    return 0


def read_d16(data, off):
    """Lit un déplacement 16 bits signé."""
    if off + 1 >= len(data):
        return 0
    v = (data[off] << 8) | data[off+1]
    if v & 0x8000:
        v -= 0x10000
    return v


if __name__ == "__main__":
    main()
