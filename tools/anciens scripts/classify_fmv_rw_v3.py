# classify_fmv_rw_v3.py — Indirection A6 (base FMV chargée depuis un champ contexte)
# Repère MOVEA.L (d16,A6),An puis classe R/W des accès (d16,An) qui suivent.
# Les registres sont nommés [A6+ctxOff]+regOff  (relatif, car base inconnue à froid)
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: python classify_fmv_rw_v3.py <module.bin>")
        sys.exit(1)

    path = sys.argv[1]
    data = open(path, "rb").read()
    print(f"=== Classification R/W v3 (indirection A6) — {path} ({len(data)} octets) ===\n")

    # An -> ('A6', ctx_off) si chargé depuis (ctx_off,A6) ; sinon None
    an_src = [None] * 8
    # regs[(ctx_off, reg_off)] = {'R','W','ctx'}
    regs = {}

    def note(key, mode, off, label):
        e = regs.setdefault(key, {'R': 0, 'W': 0, 'ctx': []})
        e[mode] += 1
        if len(e['ctx']) < 5:
            e['ctx'].append((off, label))

    size_map = {1: '.B', 3: '.W', 2: '.L'}

    i = 0
    n = len(data)
    while i < n - 1:
        op = (data[i] << 8) | data[i+1]
        dst_mode = (op >> 6) & 0x7
        dst_reg  = (op >> 9) & 0x7
        src_mode = (op >> 3) & 0x7
        src_reg  = op & 0x7
        top4     = (op >> 12) & 0xF

        # --- MOVEA.L (d16,A6),An : op 0x2[An]6E (.L, src mode=5 reg=6) ---
        # 0010 rrr 001 101 110 = 0x2_6E
        if (op & 0xF1FF) == 0x206E:
            an = (op >> 9) & 0x7
            d16 = read_d16(data, i + 2)
            an_src[an] = d16
            i += 4
            continue

        # --- MOVEA.L (An)+ ou autre vers An : invalide ---
        if (op & 0xF1C0) == 0x2040 and dst_mode == 1:
            # MOVEA.L vers An sans (d16,A6) explicite -> invalide cette piste
            an = (op >> 9) & 0x7
            an_src[an] = None
            # ne pas consommer plus, fallthrough géré ci-dessous

        # --- Accès (d16,An) avec An tracé ---
        consumed = 0
        # MOVE.x
        if top4 in (0x1, 0x2, 0x3):
            sz = size_map.get((op >> 12) & 0x3, '')
            if src_mode == 5 and an_src[src_reg] is not None:
                d16 = read_d16(data, i + 2)
                key = (an_src[src_reg], d16)
                note(key, 'R', i, f"MOVE{sz}<-(d16,A{src_reg})")
                consumed = 4
            elif dst_mode == 5 and an_src[dst_reg] is not None:
                d16 = read_d16(data, i + 2)
                key = (an_src[dst_reg], d16)
                note(key, 'W', i, f"MOVE{sz}->(d16,A{dst_reg})")
                consumed = 4

        # TST.x (d16,An)
        if not consumed and (op & 0xFF00) == 0x4A00 and src_mode == 5 and an_src[src_reg] is not None:
            d16 = read_d16(data, i + 2)
            key = (an_src[src_reg], d16)
            sz = size_map.get((op >> 6) & 0x3, '')
            note(key, 'R', i, f"TST{sz}(d16,A{src_reg})")
            consumed = 4

        # CLR.x (d16,An)
        if not consumed and (op & 0xFF00) == 0x4200 and src_mode == 5 and an_src[src_reg] is not None:
            d16 = read_d16(data, i + 2)
            key = (an_src[src_reg], d16)
            sz = size_map.get((op >> 6) & 0x3, '')
            note(key, 'W', i, f"CLR{sz}(d16,A{src_reg})")
            consumed = 4

        # BTST/BSET/BCLR #n,(d16,An)
        if not consumed and (op & 0xFF00) == 0x0800 and src_mode == 5 and an_src[src_reg] is not None:
            d16 = read_d16(data, i + 4)
            key = (an_src[src_reg], d16)
            note(key, 'R', i, f"BIT(d16,A{src_reg})")
            consumed = 6

        if consumed:
            i += consumed
            continue

        i += 2

    # Rapport
    if not regs:
        print("  Aucun accès via (d16,A6)->An->(d16,An) détecté.")
        print("  La base passe peut-être par 2 niveaux d'indirection, ou par A5/registre data.\n")
        return

    print(f"  {'REGISTRE RELATIF':<22} {'R':>3} {'W':>3}  SENS      INSTRUCTION(S)")
    print("  " + "-" * 70)
    for key in sorted(regs):
        ctx_off, reg_off = key
        e = regs[key]
        if e['R'] and e['W']: sens = "RW"
        elif e['W']: sens = "WRITE"
        elif e['R']: sens = "READ"
        else: sens = "?"
        name = f"[A6+{ctx_off:#x}]+{reg_off:#x}"
        ctxs = ", ".join(f"{c[1]}@{c[0]:#06x}" for c in e['ctx'][:3])
        print(f"  {name:<22} {e['R']:>3} {e['W']:>3}  {sens:<8}  {ctxs}")

    print(f"\n  {len(regs)} registres relatifs classés.")
    print("  ⚠️ Adresse absolue = (valeur de [A6+ctxOff] à l'exécution) + regOff")


def read_d16(data, off):
    if off + 1 >= len(data):
        return 0
    v = (data[off] << 8) | data[off+1]
    if v & 0x8000:
        v -= 0x10000
    return v


if __name__ == "__main__":
    main()
