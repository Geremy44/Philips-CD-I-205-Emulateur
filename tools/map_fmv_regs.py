#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""map_fmv_regs.py — repère les accès registres FMV ($E0xxxx) dans un module"""
import sys, re

def u32(d,o): return (d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]
def u16(d,o): return (d[o]<<8)|d[o+1]

def main(path):
    with open(path,"rb") as f: d=f.read()
    print(f"=== Accès registres FMV dans {path} ({len(d)} octets) ===\n")
    hits=[]
    for i in range(0, len(d)-4, 2):
        val=u32(d,i)
        # adresses dans la plage MMIO FMV $E00000-$E0FFFF
        if 0x00E00000 <= val <= 0x00E0FFFF:
            # contexte : opcode précédent (2EBC=MOVE.L imm, 2D7C=MOVE.L imm to (An))
            op=u16(d,i-2) if i>=2 else 0
            mnem={0x2EBC:"MOVE.L #imm,(A7)",0x2D7C:"MOVE.L #imm,d(A6)",
                  0x23FC:"MOVE.L #imm,abs",0x21FC:"MOVE.L #imm,abs.w"}.get(op,"?")
            hits.append((i,val,op,mnem))
    # tri par adresse registre
    for off,val,op,mnem in sorted(hits,key=lambda x:x[1]):
        print(f"  @0x{off:05X}  reg=${val:06X}  (op {op:04X} {mnem})")
    print(f"\nTotal: {len(hits)} référence(s). Registres uniques:")
    for r in sorted(set(h[1] for h in hits)):
        print(f"    ${r:06X}")

if __name__=="__main__":
    main(sys.argv[1])
