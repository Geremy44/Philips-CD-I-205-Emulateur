#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""disasm_module.py - Desassemble un module OS-9/CD-i (Motorola 68000) en fichier texte.

Usage:
  python disasm_module.py <fichier.bin_ou_.mod> [offset_hex] [taille_hex]

Si offset/taille omis et que le fichier commence par 0x4AFC, on lit l'en-tete.
Sinon on desassemble tout le fichier depuis le debut.
"""
import sys, os

try:
    from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000
except ImportError:
    print("!! Capstone manquant : pip install capstone")
    sys.exit(1)

def u16(d,o): return (d[o]<<8)|d[o+1]
def u32(d,o): return (d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]

def read_name(d, base, name_off):
    no = base + name_off; name=""
    while no < len(d) and len(name) < 32:
        c=d[no]; ch=c&0x7F
        if ch<0x20 or ch>=0x7F: break
        name+=chr(ch)
        if c&0x80: break
        no+=1
    return name

def main():
    if len(sys.argv) < 2:
        print("Usage: python disasm_module.py <fichier> [offset_hex] [taille_hex]")
        return

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    # Determination offset/taille
    base = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    name = "module"
    exec_off = None

    if u16(data, base) == 0x4AFC:
        size     = u32(data, base + 0x04)
        name_off = u32(data, base + 0x0C)
        mtype    = data[base + 0x12]
        exec_off = u16(data, base + 0x2A)  # M$Exec (16 bits) pour modules executables
        name     = read_name(data, base, name_off) or "module"
        print(f"En-tete OS-9 detecte : nom='{name}' size=0x{size:X} type=0x{mtype:02X} exec=0x{exec_off:X}")
    else:
        size = len(data) - base

    if len(sys.argv) > 3:
        size = int(sys.argv[3], 16)

    code = data[base:base+size]

    # Adresse virtuelle = 0 (offsets relatifs au module, comme dans l'en-tete)
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    md.skipdata = True   # ne s'arrete pas sur octets invalides

    outdir = os.path.join(os.path.dirname(os.path.abspath(path)), "disasm")
    os.makedirs(outdir, exist_ok=True)
    safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in name)
    outpath = os.path.join(outdir, f"{safe}.asm")

    lines = []
    lines.append(f"; ====================================================")
    lines.append(f";  Desassemblage : {name}")
    lines.append(f";  Source        : {os.path.basename(path)}")
    lines.append(f";  Offset base   : 0x{base:06X}   Taille : 0x{size:X} ({size} o)")
    if exec_off is not None:
        lines.append(f";  Point entree  : 0x{exec_off:06X} (M$Exec)")
    lines.append(f"; ====================================================")
    lines.append("")

    count = 0
    for insn in md.disasm(code, 0x0000):
        raw = " ".join("%02X" % b for b in insn.bytes)
        marker = ""
        if exec_off is not None and insn.address == exec_off:
            marker = "   <=== M$Exec (point d'entree)"
            lines.append("")
            lines.append("; --- POINT D'ENTREE ---")
        lines.append("%06X:  %-20s  %-8s %s%s" % (
            insn.address, raw, insn.mnemonic, insn.op_str, marker))
        count += 1

    lines.append("")
    lines.append(f"; === {count} instructions desassemblees ===")

    with open(outpath, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    print(f"\nOK -> {outpath}")
    print(f"   {count} instructions ecrites.")
    print(f"\nApercu (30 premieres lignes) :")
    print("-"*60)
    for l in lines[6:6+30]:
        print(l)

if __name__ == "__main__":
    main()
