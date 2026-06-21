#!/usr/bin/env python3
"""disasm_simple.py — désassemblage minimal 68000"""
import sys
import capstone as cs

def main(path, start, length):
    start=int(start,16); length=int(length)
    with open(path,"rb") as f:
        f.seek(start)
        code=f.read(length)
    
    md=cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_32)
    md.detail=True
    for instr in md.disasm(code, start):
        print(f"0x{instr.address:05X}  {instr.mnemonic:8} {instr.op_str}")

if __name__=="__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3],16))
