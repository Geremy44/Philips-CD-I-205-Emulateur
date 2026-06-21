#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
diag_os9_header.py
Teste les 4 variantes d'octets sur l'en-tête à 0x1E6 (module connu).
Usage:
    python diag_os9_header.py "chemin\\FMV_combined_7308hi_7307lo.bin"
"""
import sys, os

def u16(b, o): return (b[o] << 8) | b[o+1]

def test_header(data, base, label):
    print(f"\n{'='*60}\n  {label}\n{'='*60}")
    if base + 0x10 > len(data):
        print("  hors limites"); return
    sync = u16(data, base)
    print(f"  sync      = 0x{sync:04X}  {'✅' if sync==0x4AFC else '❌'}")
    print(f"  size      = 0x{(data[base+2]<<24)|(data[base+3]<<16)|(data[base+4]<<8)|data[base+5]:08X}")
    print(f"  nameoff   = 0x{u16(data, base+6):04X}")
    print(f"  type/lang = 0x{u16(data, base+8):04X}")
    # parité : XOR des 6 premiers mots doit = 0xFFFF
    chk = 0
    for w in range(6):
        chk ^= u16(data, base + w*2)
    print(f"  parité    = 0x{chk:04X}  {'✅ VALIDE' if chk==0xFFFF else '❌'}")
    # dump brut
    raw = ' '.join(f'{data[base+k]:02X}' for k in range(16))
    print(f"  raw[16]   = {raw}")

def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())

    print(f"📄 {os.path.basename(path)} ({len(data)} octets)")

    # Variante 1 : tel quel
    test_header(data, 0x1E6, "VARIANTE 1 : fichier tel quel @ 0x1E6")

    # Variante 2 : byte-swap global (échange chaque paire)
    swapped = bytearray(len(data))
    for k in range(0, len(data)-1, 2):
        swapped[k]   = data[k+1]
        swapped[k+1] = data[k]
    # après swap, le sync 4AFC peut être ailleurs ; on cherche
    pos = swapped.find(b'\x4A\xFC')
    print(f"\n[byte-swap] premier 4AFC @ 0x{pos:X}" if pos>=0 else "\n[byte-swap] pas de 4AFC")
    if pos >= 0:
        test_header(swapped, pos, f"VARIANTE 2 : byte-swap @ 0x{pos:X}")

    # Variante 3 : décalage -1 (impair) tel quel
    test_header(data, 0x1E7, "VARIANTE 3 : tel quel @ 0x1E7 (décalage)")

if __name__ == "__main__":
    main()
