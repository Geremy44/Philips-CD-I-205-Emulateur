#!/usr/bin/env python3
"""
disasm_fmv_init.py — Désassemble la routine d'init FMV autour de $1EE0
pour extraire les écritures vers les registres MMIO ($E8xxxx).
Nécessite : pip install capstone
"""
import sys
from capstone import Cs, CS_ARCH_M68K, CS_MODE_BIG_ENDIAN

START = 0x1E00
END   = 0x2000

def main():
    with open(sys.argv[1], "rb") as f:
        data = f.read()

    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN)
    code = data[START:END]

    print(f"=== Désassemblage $1E00–$2000 ===\n")
    writes = []
    for insn in md.disasm(code, START):
        line = f"  ${insn.address:04X}: {insn.mnemonic:8} {insn.op_str}"
        print(line)
        # repère les écritures vers MMIO ($E8xxxx) ou immédiats vers registre
        op = insn.op_str.lower()
        if "move" in insn.mnemonic.lower() and "#" in op:
            writes.append((insn.address, insn.mnemonic, insn.op_str))

    print(f"\n=== Écritures immédiates détectées ({len(writes)}) ===")
    for addr, mn, ops in writes:
        print(f"  ${addr:04X}: {mn} {ops}")

if __name__ == "__main__":
    main()
