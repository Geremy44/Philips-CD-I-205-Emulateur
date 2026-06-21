#!/usr/bin/env python3
import sys

def main():
    if len(sys.argv) < 3:
        print("Usage: python find_pattern.py <fichier.bin> <pattern_hex> [contexte]")
        print("  ex: python find_pattern.py fmvdrv.bin 247C")
        print("  ex: python find_pattern.py fmvdrv.bin 247C00E0")
        return

    path = sys.argv[1]
    pat_hex = sys.argv[2].replace(" ", "").replace("0x", "")
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 8

    if len(pat_hex) % 2 != 0:
        print("⚠️  Pattern hex doit avoir un nombre pair de chiffres")
        return

    try:
        pattern = bytes.fromhex(pat_hex)
    except ValueError:
        print("⚠️  Pattern hex invalide")
        return

    with open(path, "rb") as f:
        data = f.read()

    print("=" * 60)
    print(f"  RECHERCHE pattern {pat_hex.upper()} dans {path}")
    print(f"  ({len(data)} octets)")
    print("=" * 60)

    found = 0
    start = 0
    while True:
        idx = data.find(pattern, start)
        if idx < 0:
            break
        found += 1

        # contexte avant/après
        lo = max(0, idx - ctx)
        hi = min(len(data), idx + len(pattern) + ctx)
        chunk = data[lo:hi]
        hexstr = " ".join(f"{b:02X}" for b in chunk)

        print(f"\n  @0x{idx:05X}  (match)")
        print(f"    {hexstr}")

        # si pattern type MOVEA.L #imm,An (247C/267C/...) on décode l'immédiat
        if len(pattern) >= 2 and idx + 6 <= len(data):
            op = data[idx:idx+2]
            # 2x7C = MOVEA.L #imm,Ax  (24=A2,26=A3,28=A4...)
            if op[1] == 0x7C and (op[0] & 0xF1) == 0x21:
                an = (op[0] >> 1) & 0x07
                imm = int.from_bytes(data[idx+2:idx+6], "big")
                print(f"    → MOVEA.L #${imm:06X}, A{an}")

        start = idx + 1

    print(f"\n  ✅ {found} occurrence(s) trouvée(s)")

if __name__ == "__main__":
    main()
