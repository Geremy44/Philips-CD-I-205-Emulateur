#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan_os9_modules.py — parser OS-9 corrigé (champs 32 bits + parity)"""
import sys, os

def u16(d,o): return (d[o]<<8)|d[o+1]
def u32(d,o): return (d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]

def os9_crc(data):
    crc = 0xFFFFFF
    for b in data:
        crc ^= (b << 16)
        for _ in range(8):
            crc = (crc<<1)
            if crc & 0x1000000: crc ^= 0x800063
            crc &= 0xFFFFFF
    return crc

# Le header parity OS-9 : XOR de tous les mots de l'en-tête jusqu'au M$HdrChk
# M$HdrChk est à l'offset 0x18 (24 bits) pour les modules OS-9/68k standard.
# La parité couvre les octets 0..0x17, et M$Parity (offset 0x18, 2 octets)
# fait que le XOR total = 0xFFFF.
def header_parity_ok(d, i):
    # somme XOR des mots 0x00..0x16 doit == complément du mot 0x18? 
    # On teste la convention : XOR words[0..N] == 0xFFFF
    for hdrlen in (0x18, 0x1A, 0x2E, 0x30):
        if i+hdrlen > len(d): continue
        x = 0
        for k in range(0, hdrlen, 2):
            x ^= u16(d, i+k)
        if x == 0xFFFF:
            return hdrlen
    return None

def read_name(d, i, name_off):
    no = i + name_off
    name = ""
    if 0 <= no < len(d):
        # noms OS-9 : dernier octet a bit 7 mis à 1
        while no < len(d) and len(name) < 32:
            c = d[no]
            ch = c & 0x7F
            if ch < 0x20 or ch >= 0x7F: break
            name += chr(ch)
            if c & 0x80: break   # fin de chaîne OS-9
            no += 1
    return name

def parse(d, path):
    print(f"📄 {os.path.basename(path)} ({len(d)} octets)\n")
    n = len(d)
    found = 0
    for i in range(0, n-0x18, 2):
        if d[i]==0x4A and d[i+1]==0xFC:
            size     = u32(d, i+0x04)      # M$Size — 32 bits !
            name_off = u32(d, i+0x0C)      # M$Name — 32 bits !
            mtype    = d[i+0x12] if i+0x12<n else 0
            if size==0 or size>n or (i+size)>n:
                tag="❌"; hp=None
            else:
                hp = header_parity_ok(d, i)
                tag = "✅" if hp else "⚠️"
            name = read_name(d, i, name_off) if 0<name_off<n else ""
            print(f"{tag} @0x{i:06X} size=0x{size:06X} "
                  f"hdrlen={hp if hp else '--':>4} "
                  f"type=0x{mtype:02X} name='{name}'")
            if hp: found+=1
    print(f"\n=== Modules header-valides : {found} ===")

if __name__=="__main__":
    with open(sys.argv[1],"rb") as f: d=f.read()
    parse(d, sys.argv[1])
