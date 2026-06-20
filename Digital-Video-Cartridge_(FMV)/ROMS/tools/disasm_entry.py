#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""disasm_entry.py — désassemble le firmware 68000 à partir du reset PC"""
import sys, os
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

def u32(b,o): return (b[o]<<24)|(b[o+1]<<16)|(b[o+2]<<8)|b[o+3]

def main():
    path = sys.argv[1]
    with open(path,"rb") as f: d = f.read()

    reset_pc = u32(d,4)
    print(f"📄 {os.path.basename(path)}")
    print(f"🎯 Reset PC = 0x{reset_pc:06X}\n")

    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    md.detail = False

    # Désassemble 80 instructions depuis le point d'entrée
    code = d[reset_pc:reset_pc+0x200]
    print(f"=== Désassemblage @ 0x{reset_pc:06X} ===")
    count = 0
    for insn in md.disasm(code, reset_pc):
        print(f"  0x{insn.address:06X}:  {insn.bytes.hex():12s}  "
              f"{insn.mnemonic}\t{insn.op_str}")
        count += 1
        if count >= 80: break

if __name__=="__main__": main()
