# fmv_disasm_capstone.py — Désassemblage M68K + map registres FMV
import sys
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

def main():
    if len(sys.argv) < 2:
        print("Usage: python fmv_disasm_capstone.py <module.bin> [base_hex]")
        sys.exit(1)

    path = sys.argv[1]
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    data = open(path, "rb").read()

    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    md.detail = True

    # Suit MOVEA.L (d16,A6),An  -> base contexte par registre
    an_ctx = {}      # 'a0' -> ctx_off
    regs = {}        # (ctx_off, reg_off) -> {'R','W'}

    for insn in md.disasm(data, base):
        m = insn.mnemonic
        ops = insn.op_str

        # 1. Chargement base : movea.l (d16,a6), aN
        if m == 'movea.l' and 'a6)' in ops and ',' in ops:
            # ex: "(0x34,a6), a0"
            try:
                left, right = ops.rsplit(',', 1)
                an = right.strip()
                if '(' in left and 'a6' in left:
                    d = left.strip().strip('()').split(',')[0]
                    an_ctx[an] = int(d, 16)
            except Exception:
                pass
            continue

        # 2. Accès (d16,aN) avec aN tracé
        for an, ctx_off in list(an_ctx.items()):
            tag = f"{an})"
            if tag in ops and '(' in ops:
                # sens : si l'opérande aN est à droite (dest) -> WRITE sinon READ
                is_write = ops.rstrip().endswith(tag)
                key_disp = parse_disp(ops, an)
                if key_disp is None:
                    continue
                key = (ctx_off, key_disp)
                e = regs.setdefault(key, {'R': 0, 'W': 0, 'ex': []})
                e['W' if is_write else 'R'] += 1
                if len(e['ex']) < 3:
                    e['ex'].append(f"{insn.address:#06x} {m} {ops}")

    print_report(regs)


def parse_disp(ops, an):
    """Extrait d16 d'un opérande (d16,aN)."""
    import re
    mch = re.search(r'\(([^,()]+),' + an + r'\)', ops)
    if not mch:
        return None
    try:
        return int(mch.group(1), 16)
    except ValueError:
        return None


def print_report(regs):
    if not regs:
        print("  Aucun accès (d16,A6)->An->(d16,An) détecté.")
        return
    print(f"  {'REGISTRE':<20} {'R':>3} {'W':>3}  SENS")
    print("  " + "-" * 60)
    for key in sorted(regs):
        ctx, off = key
        e = regs[key]
        sens = "RW" if e['R'] and e['W'] else ("WRITE" if e['W'] else "READ")
        print(f"  [A6+{ctx:#x}]+{off:#x}".ljust(22)
              + f"{e['R']:>3} {e['W']:>3}  {sens}")
        for ex in e['ex']:
            print(f"       {ex}")
    print(f"\n  {len(regs)} registres classés (désassemblage réel).")


if __name__ == "__main__":
    main()
