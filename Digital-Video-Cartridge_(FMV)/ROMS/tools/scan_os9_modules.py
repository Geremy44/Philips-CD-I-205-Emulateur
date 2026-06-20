#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scan_os9_modules.py  (v2 — validation stricte par parité)
Usage:
    python scan_os9_modules.py "chemin\\ROM.bin"
"""
import sys, os

def u16(b, o): return (b[o] << 8) | b[o+1]
def u32(b, o): return (b[o]<<24)|(b[o+1]<<16)|(b[o+2]<<8)|b[o+3]

MODULE_TYPES = {0x1:"Program",0x2:"Subroutine",0x3:"Multi-module",
    0x4:"Data",0x5:"Config",0xC:"FileManager",0xD:"DeviceDriver",
    0xE:"DeviceDescriptor",0xF:"TrapHandler"}

def header_parity(b, base):
    """XOR des mots 0x00..0x12 inclus → doit valoir 0xFFFF."""
    chk = 0
    for w in range(10):           # 10 mots = offsets 0x00 à 0x12
        chk ^= u16(b, base + w*2)
    return chk

def read_name(b, base, nameoff):
    """Nom OS-9 : dernier caractère a le bit 7 à 1."""
    o = base + nameoff
    name = []
    for k in range(64):
        if o+k >= len(b): break
        c = b[o+k]
        name.append(chr(c & 0x7F))
        if c & 0x80: break
    return ''.join(name)

def main():
    path = sys.argv[1]
    with open(path,"rb") as f:
        data = bytearray(f.read())
    print(f"📄 {os.path.basename(path)} ({len(data)} octets)\n")

    real, fake = [], 0
    o = 0
    while o < len(data)-20:
        if data[o]==0x4A and data[o+1]==0xFC:
            par = header_parity(data, o)
            typ = (u16(data,o+8) >> 8) & 0x0F
            if par == 0xFFFF and typ in MODULE_TYPES:
                size = u32(data,o+2)
                nameoff = u16(data,o+6)
                name = read_name(data,o,nameoff)
                real.append((o,size,typ,name))
                print(f"  ✅ 0x{o:06X} size=0x{size:06X} "
                      f"type={MODULE_TYPES[typ]:16s} name='{name}'")
                o += 2
            else:
                fake += 1
                o += 2
        else:
            o += 1

    print(f"\n{'='*60}\n  Modules VALIDES : {len(real)}  |  Faux 4AFC : {fake}")

    out = os.path.join(os.path.dirname(path),"os9_modules_report.txt")
    with open(out,"w",encoding="utf-8") as f:
        for o2,s,t,n in real:
            f.write(f"0x{o2:06X}  size=0x{s:06X}  type={MODULE_TYPES[t]}  name='{n}'\n")
    print(f"📝 Rapport : {out}")

if __name__=="__main__":
    main()
