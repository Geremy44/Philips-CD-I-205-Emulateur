#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diag_parity.py — teste la vraie parité d'en-tête OS-9"""
import sys, os

def u16(b, o): return (b[o] << 8) | b[o+1]

def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())
    base = 0x1E6

    print("Dump en-tête (24 octets) :")
    for k in range(0, 24, 2):
        print(f"  +0x{k:02X}: 0x{u16(data, base+k):04X}")

    # OS-9 header parity : XOR de tous les mots de 0x00 à 0x12 inclus
    # (10 mots = 20 octets), résultat doit = 0xFFFF
    for nwords in (9, 10, 12):
        chk = 0
        for w in range(nwords):
            chk ^= u16(data, base + w*2)
        status = "✅ 0xFFFF !" if chk == 0xFFFF else f"= 0x{chk:04X}"
        print(f"  Parité sur {nwords} mots : {status}")

if __name__ == "__main__":
    main()
