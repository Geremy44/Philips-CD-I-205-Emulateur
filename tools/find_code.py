#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""find_code.py — localise les zones de vrai code 68000 vs données"""
import sys, os
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

def main():
    path = sys.argv[1]
    with open(path,"rb") as f: d = f.read()
    n = len(d)
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)

    # 1) Localiser tous les 0x4AFC (les marqueurs)
    print("=== Emplacements 0x4AFC (marqueurs) ===")
    offs = [i for i in range(0,n-1,2) if d[i]==0x4A and d[i+1]==0xFC]
    for o in offs: print(f"  0x{o:06X}")

    # 2) Localiser les RTS (4E75) — fins de fonctions = zones de code
    print(f"\n=== Densité de code (fenêtres de 4Ko) ===")
    win = 4096
    for base in range(0, n, win):
        chunk = d[base:base+win]
        rts = chunk.count(b'\x4e\x75')
        bsr = sum(1 for i in range(len(chunk)-1) if chunk[i] in (0x60,0x61))
        tag = ""
        if rts > 5: tag = "  ← CODE probable"
        print(f"  0x{base:06X}-0x{base+win:06X}: RTS={rts:3d} BRA/BSR~={bsr:3d}{tag}")

    # 3) Trouver la 1ère zone dense et désassembler
    best = max(range(0,n,win), key=lambda b: d[b:b+win].count(b'\x4e\x75'))
    print(f"\n=== Désassemblage zone la plus dense @ 0x{best:06X} ===")
    cnt=0
    for insn in md.disasm(d[best:best+0x100], best):
        print(f"  0x{insn.address:06X}: {insn.bytes.hex():12s} {insn.mnemonic}\t{insn.op_str}")
        cnt+=1
        if cnt>=40: break

if __name__=="__main__": main()
