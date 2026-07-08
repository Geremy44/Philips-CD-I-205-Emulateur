#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
parse_os9.py - Parsing des modules OS-9 68000 dans le firmware CD-i 205
Scanne le sync 0x4AFC, parse chaque en-tete de module et liste les noms.

Structure en-tete module OS-9 68000 (big-endian) :
  +0x00  word  M$Sync   = 0x4AFC  (marqueur de synchronisation)
  +0x02  word  M$SysRev (revision systeme)
  +0x04  long  M$Size   (taille totale du module en octets)
  +0x08  long  M$Owner  (proprietaire)
  +0x0C  long  M$Name   (offset RELATIF au debut du module -> nom ASCII)
  +0x10  word  M$Accs   (permissions)
  +0x12  word  M$Type   (type | langage)
  +0x14  word  M$Edit   (edition)
  +0x16  long  M$Usage  (offset table usage)
  +0x1A  long  M$Symbol (offset table symboles)
  ...
  +0x30  long  M$Exec   (offset RELATIF -> point d'entree execution)
  +0x34  long  M$Excpt  (offset gestion exceptions)
"""

import sys
import os
import struct

SYNC = 0x4AFC

# Types de module OS-9 (octet de poids fort de M$Type)
MODULE_TYPES = {
    0x01: "Prog (programme)",
    0x02: "Subroutine",
    0x03: "Multi-module",
    0x04: "Data",
    0x05: "CSDdata (config)",
    0x0B: "Trap handler",
    0x0C: "System (systeme)",
    0x0D: "FileMgr (gestionnaire fichier)",
    0x0E: "Driver (pilote)",
    0x0F: "Descriptor (descripteur)",
}

LANGUAGES = {
    0x00: "Data",
    0x01: "68000 obj",
    0x02: "BASIC I-code",
    0x03: "Pascal P-code",
    0x04: "C I-code",
    0x05: "Cobol I-code",
    0x06: "Fortran",
}


def read_cstring(data, offset, maxlen=64):
    """Lit une chaine OS-9 : ASCII dont le dernier octet a le bit 7 a 1."""
    chars = []
    for i in range(maxlen):
        if offset + i >= len(data):
            break
        b = data[offset + i]
        c = b & 0x7F
        if 32 <= c < 127:
            chars.append(chr(c))
        if b & 0x80:   # bit 7 = fin de chaine OS-9
            break
        if b == 0:
            break
    return "".join(chars)


def checksum_ok(data, mod_off, size):
    """Verifie le header parity OS-9 (les 3 octets du header CRC ne sont
    pas trivialement verifiables ici, on valide juste la coherence taille)."""
    if size < 0x30 or size > 0x100000:
        return False
    if mod_off + size > len(data):
        return False
    return True


def parse_modules(path):
    with open(path, "rb") as f:
        data = f.read()

    out = []
    out.append("=== MODULES OS-9 DANS LE FIRMWARE CD-i 205 ===")
    out.append("Fichier : %s" % path)
    out.append("Taille  : %d octets (0x%X)" % (len(data), len(data)))
    out.append("")

    modules = []
    i = 0
    while i < len(data) - 1:
        word = (data[i] << 8) | data[i + 1]
        if word == SYNC:
            # Candidat module : on tente de parser l'en-tete
            if i + 0x34 <= len(data):
                size   = struct.unpack_from(">I", data, i + 0x04)[0]
                name_o = struct.unpack_from(">I", data, i + 0x0C)[0]
                mtype  = struct.unpack_from(">H", data, i + 0x12)[0]
                exec_o = struct.unpack_from(">I", data, i + 0x30)[0]

                if checksum_ok(data, i, size):
                    type_hi = (mtype >> 8) & 0xFF
                    lang    = mtype & 0xFF
                    name    = read_cstring(data, i + name_o) if name_o < size else "?"

                    modules.append({
                        "file_off": i,
                        "size":     size,
                        "name":     name,
                        "type":     type_hi,
                        "lang":     lang,
                        "exec":     exec_o,
                    })
                    # On saute a la fin du module pour eviter les faux positifs
                    i += max(size, 2)
                    continue
        i += 2

    out.append("Total modules trouves : %d" % len(modules))
    out.append("")
    out.append("%-4s %-10s %-9s %-22s %-18s %s" %
               ("#", "FileOff", "Taille", "Type", "Langage", "Nom"))
    out.append("-" * 90)

    for n, m in enumerate(modules):
        type_str = MODULE_TYPES.get(m["type"], "0x%02X ?" % m["type"])
        lang_str = LANGUAGES.get(m["lang"], "0x%02X ?" % m["lang"])
        out.append("%-4d 0x%06X   0x%06X  %-22s %-18s %s" %
                   (n, m["file_off"], m["size"], type_str, lang_str, m["name"]))

    # Mise en evidence des modules potentiellement FMV / video
    out.append("")
    out.append("=== MODULES SUSPECTS FMV / VIDEO / MPEG ===")
    keywords = ["fmv", "mpeg", "video", "vid", "csd", "mcd", "play",
                "movie", "audio", "dvc", "ucm", "vsd", "sector"]
    found = False
    for n, m in enumerate(modules):
        low = m["name"].lower()
        if any(k in low for k in keywords):
            found = True
            out.append("  [#%d] %-16s  file 0x%06X  exec rel 0x%06X  (exec abs file 0x%06X)"
                       % (n, m["name"], m["file_off"], m["exec"],
                          m["file_off"] + m["exec"]))
    if not found:
        out.append("  (aucun nom evident - on inspectera tous les drivers/descriptors)")

    base, _ = os.path.splitext(path)
    out_path = base + "_modules.txt"
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    print("\n".join(out))
    print("\nRapport ecrit :", out_path)


def main():
    if len(sys.argv) < 2:
        print("Usage: python parse_os9.py <firmware.bin>")
        sys.exit(1)
    parse_modules(sys.argv[1])


if __name__ == "__main__":
    main()
