#!/usr/bin/env python3
"""
extract_fmv_registers.py
-------------------------
Extrait les 24 paires de registres (REG $00-$17) depuis la table
située à $1EE0 dans la ROM FMV désentrelacée.

Usage:
    python3 extract_fmv_registers.py FMV_combined_7308hi_7307lo.bin
    python3 extract_fmv_registers.py FMV_combined_7308hi_7307lo.bin --md

Sortie:
    - Tableau lisible (stdout)
    - Fragment Markdown prêt pour FMV_MMIO_MAP.md (option --md)
"""

import sys
import argparse

# --- Constantes liées au layout de la ROM FMV ---
TABLE_START = 0x1EE0        # début de la table de registres dans fmvll
TABLE_END   = 0x1FF0        # fin de la zone à scanner
NUM_REGS    = 24            # REG $00 .. $17
EXPECTED_CHECKSUM = 0x4B82  # checksum connu du BIN combiné (128 Ko)


def load_rom(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != 0x20000:
        print(f"⚠️  Taille inattendue : {len(data)} octets "
              f"(attendu 131072). On continue quand même.")
    return data


def simple_checksum(data):
    """Somme 16 bits de tous les mots big-endian (sanity check)."""
    s = 0
    for i in range(0, len(data) - 1, 2):
        s = (s + ((data[i] << 8) | data[i + 1])) & 0xFFFF
    return s


def extract_registers(data):
    """
    Décode NUM_REGS paires (index, valeur) depuis TABLE_START.
    Format supposé : chaque entrée = 2 octets [REG_ID, VALUE].
    """
    regs = []
    offset = TABLE_START
    for i in range(NUM_REGS):
        reg_id = data[offset]
        value  = data[offset + 1]
        regs.append((i, reg_id, value, offset))
        offset += 2
    return regs


def print_table(regs):
    print("\n  IDX | REG  | VALUE | OFFSET")
    print("  ----+------+-------+--------")
    for idx, reg_id, value, offset in regs:
        print(f"  ${idx:02X} | ${reg_id:02X}  |  ${value:02X}  | ${offset:04X}")


def print_markdown(regs, checksum):
    print("\n<!-- ===== Fragment Markdown pour FMV_MMIO_MAP.md ===== -->\n")
    print(f"### Table de registres FMV (extraite de `$1EE0`)\n")
    print(f"> Source : `FMV_combined_7308hi_7307lo.bin` "
          f"(checksum `0x{checksum:04X}`)\n")
    print("| Index | Registre | Valeur | Offset ROM |")
    print("|-------|----------|--------|------------|")
    for idx, reg_id, value, offset in regs:
        print(f"| `${idx:02X}` | `${reg_id:02X}` | `${value:02X}` | `${offset:04X}` |")
    print()


def main():
    ap = argparse.ArgumentParser(description="Extracteur de registres FMV")
    ap.add_argument("rom", help="chemin vers le BIN FMV désentrelacé")
    ap.add_argument("--md", action="store_true",
                    help="émettre aussi le fragment Markdown")
    args = ap.parse_args()

    data = load_rom(args.rom)

    chk = simple_checksum(data)
    status = "✅ OK" if chk == EXPECTED_CHECKSUM else "⚠️  DIFFÉRENT"
    print(f"Checksum calculé : 0x{chk:04X}  "
          f"(attendu 0x{EXPECTED_CHECKSUM:04X}) → {status}")

    regs = extract_registers(data)
    print_table(regs)

    if args.md:
        print_markdown(regs, chk)


if __name__ == "__main__":
    main()
