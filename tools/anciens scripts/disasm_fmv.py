#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
disasm_fmv.py - Desassemblage cible M68K du firmware CD-i 205
Analyse 3 zones :
  1. Point d'entree boot (0xB804)
  2. Module reel detecte (0x000EA4)
  3. Scan global -> ne garde que les vrais acces memoire vers >= 0x400000

Usage:
    python disasm_fmv.py "chemin/vers/firmware.bin"
"""

import sys
import os

# ----------------------------------------------------------------------
# Parametres memoire
# ----------------------------------------------------------------------
BASE_ADDR      = 0x180000     # adresse memoire de la System ROM
ENTRY_OFFSET   = 0xB804       # offset fichier du point d'entree boot
MODULE_OFFSET  = 0x000EA4     # offset fichier du module reel detecte
EXT_THRESHOLD  = 0x400000     # seuil Extension Space / hardware FMV
ENTRY_COUNT    = 200          # nb d'instructions a desassembler par zone

# ----------------------------------------------------------------------
# Verification de Capstone + support M68K
# ----------------------------------------------------------------------
try:
    import capstone
except ImportError:
    print("ERREUR : Capstone n'est pas installe.")
    print("  -> pip install capstone")
    sys.exit(1)

if not hasattr(capstone, "CS_ARCH_M68K"):
    print("ERREUR : ta version de Capstone n'a pas le support M68K.")
    print("  Version actuelle :", capstone.__version__)
    print("  -> pip install --upgrade capstone   (il faut >= 4.0)")
    sys.exit(1)


def make_disassembler():
    """Cree un desassembleur M68K (mode 68000)."""
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
    md.detail = True
    return md


def disasm_zone(md, data, file_offset, count, title, out):
    """Desassemble `count` instructions depuis file_offset et ecrit dans out."""
    out.append("")
    out.append("=" * 70)
    out.append(title)
    out.append("=" * 70)

    chunk = data[file_offset: file_offset + count * 8]  # marge large
    mem_addr = BASE_ADDR + file_offset

    n = 0
    for insn in md.disasm(chunk, mem_addr):
        hexbytes = " ".join("%02X" % b for b in insn.bytes)
        out.append("  0x%06X (file 0x%05X):  %-22s  %s %s" % (
            insn.address,
            insn.address - BASE_ADDR,
            hexbytes,
            insn.mnemonic,
            insn.op_str,
        ))
        n += 1
        if n >= count:
            break

    if n == 0:
        out.append("  (aucune instruction decodee - zone probablement data/packee)")


def scan_extension_refs(md, data, out):
    """
    Desassemble tout le binaire et ne garde que les instructions
    dont un operande immediat/adresse pointe vers >= EXT_THRESHOLD.
    """
    out.append("")
    out.append("=" * 70)
    out.append("SCAN GLOBAL - acces memoire reels vers >= 0x%06X" % EXT_THRESHOLD)
    out.append("=" * 70)

    # Mnemoniques qui transportent une adresse/immediat interessant
    INTERESTING = ("lea", "move", "movea", "jsr", "jmp", "pea")

    found = 0
    mem_addr = BASE_ADDR

    for insn in md.disasm(data, mem_addr):
        mnem = insn.mnemonic.lower()
        if not any(mnem.startswith(x) for x in INTERESTING):
            continue

        # Cherche une valeur >= seuil dans les operandes
        target = None
        try:
            for op in insn.operands:
                # immediat
                if op.type == capstone.m68k.M68K_OP_IMM:
                    if op.imm >= EXT_THRESHOLD and op.imm <= 0xFFFFFF:
                        target = op.imm
                        break
                # adresse absolue
                if op.type == capstone.m68k.M68K_OP_MEM:
                    disp = getattr(op.mem, "disp", 0)
                    if disp >= EXT_THRESHOLD and disp <= 0xFFFFFF:
                        target = disp
                        break
        except Exception:
            # fallback : parse textuel du op_str
            pass

        # fallback textuel si pas trouve via operands
        if target is None:
            txt = insn.op_str.replace("$", "").replace("0x", "")
            for token in txt.replace(",", " ").replace("(", " ").replace(")", " ").split():
                try:
                    val = int(token, 16)
                    if EXT_THRESHOLD <= val <= 0xFFFFFF:
                        target = val
                        break
                except ValueError:
                    continue

        if target is not None:
            hexbytes = " ".join("%02X" % b for b in insn.bytes)
            out.append("  0x%06X (file 0x%05X):  %-22s  %s %s   -> 0x%06X" % (
                insn.address,
                insn.address - BASE_ADDR,
                hexbytes,
                insn.mnemonic,
                insn.op_str,
                target,
            ))
            found += 1

    out.append("")
    out.append("  Total references reelles trouvees : %d" % found)


def main():
    if len(sys.argv) < 2:
        print("Usage: python disasm_fmv.py <firmware.bin>")
        sys.exit(1)

    path = sys.argv[1]
    if not os.path.isfile(path):
        print("ERREUR : fichier introuvable :", path)
        sys.exit(1)

    with open(path, "rb") as f:
        data = f.read()

    print("Fichier charge :", path)
    print("Taille         : %d octets (0x%X)" % (len(data), len(data)))

    md = make_disassembler()

    out = []
    out.append("=== DESASSEMBLAGE FMV CD-i 205 (Capstone M68K) ===")
    out.append("Fichier source : %s" % path)
    out.append("Taille         : %d octets (0x%X)" % (len(data), len(data)))
    out.append("Base memoire   : 0x%06X" % BASE_ADDR)

    # Zone 1 : point d'entree boot
    disasm_zone(md, data, ENTRY_OFFSET, ENTRY_COUNT,
                "ZONE 1 - POINT D'ENTREE BOOT (file 0x%05X / mem 0x%06X)"
                % (ENTRY_OFFSET, BASE_ADDR + ENTRY_OFFSET), out)

    # Zone 2 : module reel detecte
    disasm_zone(md, data, MODULE_OFFSET, ENTRY_COUNT,
                "ZONE 2 - MODULE DETECTE (file 0x%05X / mem 0x%06X)"
                % (MODULE_OFFSET, BASE_ADDR + MODULE_OFFSET), out)

    # Zone 3 : scan global des references extension
    scan_extension_refs(md, data, out)

    # Ecriture du rapport
    base, _ = os.path.splitext(path)
    out_path = base + "_disasm.txt"
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    print("Rapport ecrit :", out_path)


if __name__ == "__main__":
    main()
