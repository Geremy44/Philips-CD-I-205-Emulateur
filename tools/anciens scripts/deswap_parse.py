#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""deswap_parse.py - De-swap (FC4A -> 4AFC) + parsing modules OS-9 CD-i."""
import sys, os, struct

SYNC = 0x4AFC

MODULE_TYPES = {
    0x01:"Prog", 0x02:"Subroutine", 0x03:"Multi-module", 0x04:"Data",
    0x05:"CSDdata", 0x0B:"Trap", 0x0C:"System", 0x0D:"FileMgr",
    0x0E:"Driver", 0x0F:"Descriptor",
}
LANGUAGES = {0x00:"Data",0x01:"68000",0x02:"BASIC",0x03:"Pascal",0x04:"C",0x06:"Fortran"}

def deswap(data):
    """Inverse chaque paire d'octets (byte-swap 16 bits)."""
    b = bytearray(data)
    if len(b) % 2:                 # securite si taille impaire
        b.append(0)
    b[0::2], b[1::2] = b[1::2], b[0::2]
    return bytes(b)

def read_cstring(data, off, maxlen=64):
    s = []
    for i in range(maxlen):
        if off+i >= len(data): break
        ch = data[off+i]
        c = ch & 0x7F
        if 32 <= c < 127: s.append(chr(c))
        if ch & 0x80 or ch == 0: break
    return "".join(s)

def os9_crc(data, off, size):
    """CRC 24 bits OS-9 sur tout le module (doit valoir 0x00B70B)."""
    if off+size > len(data): return None
    accum = 0xFFFFFF
    for i in range(size):
        b = data[off+i]
        accum ^= (b << 16)
        for _ in range(8):
            accum <<= 1
            if accum & 0x1000000:
                accum ^= 0x800063
        accum &= 0xFFFFFF
    return accum

def parse(path):
    with open(path, "rb") as f:
        raw = f.read()
    data = deswap(raw)

    # ecrit le binaire de-swappe pour reutilisation
    base, _ = os.path.splitext(path)
    deswap_path = base + "_deswap.bin"
    with open(deswap_path, "wb") as f:
        f.write(data)

    out = []
    out.append("=== MODULES OS-9 (firmware DE-SWAPPE) ===")
    out.append("Source : %s" % path)
    out.append("De-swap ecrit : %s" % deswap_path)
    out.append("")

    modules = []
    i = 0
    while i < len(data) - 0x30:
        if data[i] == 0x4A and data[i+1] == 0xFC:   # sync trouve
            size  = struct.unpack(">I", data[i+4:i+8])[0]
            n_off = struct.unpack(">I", data[i+0x0C:i+0x10])[0]
            typ   = data[i+0x12]
            lang  = data[i+0x13]
            execo = struct.unpack(">I", data[i+0x30:i+0x34])[0] if size >= 0x34 else 0

            # validation : taille plausible + CRC OS-9
            valid = (0x30 <= size <= 0x80000) and (i+size <= len(data))
            crc = os9_crc(data, i, size) if valid else None
            crc_ok = (crc == 0x00B70B)

            if valid:
                name = read_cstring(data, i + n_off) if (i+n_off) < len(data) else "?"
                modules.append({
                    "off":i, "size":size, "type":typ, "lang":lang,
                    "exec":execo, "name":name, "crc_ok":crc_ok
                })
                i += size            # saut au module suivant
                continue
        i += 2                       # avance word-aligned

    out.append("Total modules : %d" % len(modules))
    out.append("")
    out.append("%-4s %-9s %-9s %-12s %-8s %-5s %s" %
               ("#","FileOff","Taille","Type","Lang","CRC","Nom"))
    out.append("-"*90)
    for n,m in enumerate(modules):
        out.append("%-4d 0x%06X  0x%06X  %-12s %-8s %-5s %s" %
                   (n, m["off"], m["size"],
                    MODULE_TYPES.get(m["type"],"0x%02X"%m["type"]),
                    LANGUAGES.get(m["lang"],"0x%02X"%m["lang"]),
                    "OK" if m["crc_ok"] else "--",
                    m["name"]))

    out.append("")
    out.append("=== MODULES FMV / VIDEO / MPEG ===")
    kw = ["fmv","mpeg","video","vid","csd","mcd","play","movie",
          "audio","dvc","ucm","vsd","sector","decod"]
    hit = False
    for n,m in enumerate(modules):
        if any(k in m["name"].lower() for k in kw):
            hit = True
            out.append("  [#%d] %-16s off 0x%06X  exec abs 0x%06X"
                       % (n, m["name"], m["off"], m["off"]+m["exec"]))
    if not hit:
        out.append("  (rien d'evident - on listera tous les Driver/Descriptor)")

    rep = base + "_modules.txt"
    with open(rep, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("\n".join(out))
    print("\nRapport :", rep)

if __name__ == "__main__":
    parse(sys.argv[1])
