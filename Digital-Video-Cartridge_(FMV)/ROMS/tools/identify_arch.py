#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""identify_arch.py — détecte l'archi et le point d'entrée d'un firmware brut"""
import sys, os

def u32(b,o): return (b[o]<<24)|(b[o+1]<<16)|(b[o+2]<<8)|b[o+3]
def u16(b,o): return (b[o]<<8)|b[o+1]

def main():
    path = sys.argv[1]
    with open(path,"rb") as f: d = f.read()
    n = len(d)
    print(f"📄 {os.path.basename(path)} ({n} octets)\n")

    # --- Hypothèse 68000 big-endian ---
    ssp = u32(d,0)
    pc  = u32(d,4)
    print("=== Hypothèse 68000 (big-endian) ===")
    print(f"  SSP initial (0x00) = 0x{ssp:08X}")
    print(f"  Reset PC    (0x04) = 0x{pc:08X}")
    plausible = pc < n or (0xF00000 <= pc <= 0xFFFFFF)
    print(f"  PC plausible ? {'✅' if pc < n else '⚠️ hors fichier (ROM mappée haute?)'}")

    print("\n=== Premiers 32 octets (vecteurs) ===")
    for i in range(0,32,4):
        print(f"  +0x{i:02X}: 0x{u32(d,i):08X}")

    # --- Recherche de signatures connues ---
    print("\n=== Signatures notables ===")
    sigs = {
        b'CL45':'C-Cube CL450/CL480 MPEG', b'\x4E\x71':'68000 NOP',
        b'\x4E\x75':'68000 RTS', b'CD-RTOS':'CD-RTOS', b'OS-9':'OS-9',
        b'Philips':'Philips', b'MPEG':'MPEG', b'VMPEG':'VMPEG',
        b'\x60':'BRA possible',
    }
    for sig,desc in sigs.items():
        c = d.count(sig)
        if c: print(f"  '{sig}' ({desc}) : {c}×")

    # --- Distribution des opcodes pour deviner l'archi ---
    print("\n=== Indices 68000 ===")
    for op,name in [(b'\x4E\x75','RTS'),(b'\x4E\x71','NOP'),
                    (b'\x4E\x56','LINK A6'),(b'\x4E\x5E','UNLK A6'),
                    (b'\x4E\xB9','JSR abs'),(b'\x4E\xF9','JMP abs')]:
        print(f"  {name:10s} {op.hex()} : {d.count(op)}×")

if __name__=="__main__": main()
