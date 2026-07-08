#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""detect_sync.py - Compte les occurrences du sync OS-9 dans les 2 ordres d'octets."""
import sys

def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    n_normal = 0   # 4A FC  (ordre normal)
    n_swap   = 0   # FC 4A  (byte-swappe)
    off_normal = []
    off_swap = []

    for i in range(len(data) - 1):
        a, b = data[i], data[i+1]
        if a == 0x4A and b == 0xFC:
            n_normal += 1
            if len(off_normal) < 10:
                off_normal.append(i)
        if a == 0xFC and b == 0x4A:
            n_swap += 1
            if len(off_swap) < 10:
                off_swap.append(i)

    print("=== DETECTION SYNC OS-9 ===")
    print("Sync NORMAL (4A FC)     : %d occurrences" % n_normal)
    print("   premiers offsets:", [hex(x) for x in off_normal])
    print("Sync BYTE-SWAP (FC 4A)  : %d occurrences" % n_swap)
    print("   premiers offsets:", [hex(x) for x in off_swap])
    print()

    # Test aligne sur offsets PAIRS seulement (les modules sont word-aligned)
    n_normal_even = sum(1 for i in off_normal if i % 2 == 0)
    n_swap_even   = sum(1 for i in off_swap   if i % 2 == 0)
    print("Normal sur offset pair  :", n_normal_even, "/ 10 premiers")
    print("Swap sur offset pair    :", n_swap_even,   "/ 10 premiers")
    print()

    if n_swap > n_normal * 3:
        print(">>> VERDICT : firmware probablement BYTE-SWAPPE")
        print(">>> Je vais te preparer un de-swap + re-parsing")
    elif n_normal > n_swap * 3:
        print(">>> VERDICT : ordre d'octets NORMAL")
        print(">>> Le probleme vient d'ailleurs (alignement / validation header)")
    else:
        print(">>> VERDICT : ambigu, on regardera les offsets en detail")

if __name__ == "__main__":
    main()
