#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dump_region.py — hexdump annoté autour d'un offset"""
import sys

def main(path, off, length=64):
    off=int(off,16); length=int(length)
    with open(path,"rb") as f: d=f.read()
    start=max(0,off-16)
    print(f"=== {path} @0x{off:05X} (±) ===\n")
    for i in range(start, min(len(d),off+length), 2):
        w=(d[i]<<8)|d[i+1]
        marker=" <<<" if i==off else ""
        # détecte les imm 32 bits qui suivent un opcode d'écriture
        long=""
        if i+4<=len(d) and w in (0x2EBC,0x2D7C,0x2F7C,0x23FC,0x317C):
            imm=(d[i+2]<<24)|(d[i+3]<<16)|(d[i+4]<<8)|d[i+5] if i+6<=len(d) else 0
            long=f"  imm=${imm:08X}"
        print(f"  0x{i:05X}: {w:04X}{long}{marker}")

if __name__=="__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv)>3 else 64)
