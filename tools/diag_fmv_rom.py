#!/usr/bin/env python3
"""
diag_fmv_rom.py — Vérifie l'identité du dump et localise la table de registres.
"""
import sys

def checksums(data):
    s8  = sum(data) & 0xFF
    s16 = 0
    for i in range(0, len(data) - 1, 2):
        s16 = (s16 + ((data[i] << 8) | data[i+1])) & 0xFFFF
    s16le = 0
    for i in range(0, len(data) - 1, 2):
        s16le = (s16le + ((data[i+1] << 8) | data[i])) & 0xFFFF
    return s8, s16, s16le

def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    print(f"Fichier : {path}")
    print(f"Taille  : {len(data)} octets (0x{len(data):X})\n")

    s8, s16be, s16le = checksums(data)
    print("=== Checksums (pour réaligner avec l'outil d'entrelacement) ===")
    print(f"  8-bit            : 0x{s8:02X}")
    print(f"  16-bit big-endian: 0x{s16be:04X}")
    print(f"  16-bit lil-endian: 0x{s16le:04X}")
    print(f"  (on cherchait    : 0x4B82)\n")

    # Affiche les vecteurs 68000 pour confirmer que c'est le bon dump
    print("=== Vecteurs 68000 (sanity) ===")
    def rd32(o): return int.from_bytes(data[o:o+4], "big")
    print(f"  SSP  (0x00) = 0x{rd32(0):08X}")
    print(f"  PC   (0x04) = 0x{rd32(4):08X}")

    # Dump brut autour de $1EE0 pour inspection visuelle
    print("\n=== Hexdump $1ED0–$1F20 ===")
    for base in range(0x1ED0, 0x1F20, 16):
        chunk = data[base:base+16]
        hexa = " ".join(f"{b:02X}" for b in chunk)
        txt  = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  ${base:04X}: {hexa}  {txt}")

if __name__ == "__main__":
    main()
