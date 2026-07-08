#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract_module.py - Extrait + decode l'en-tete d'un module OS-9 CD-i (format 32 bits correct)."""
import sys, os, struct

MODULE_TYPES = {0x01:"Prog",0x02:"Subroutine",0x03:"Multi-module",0x04:"Data",
    0x05:"CSDdata",0x0B:"Trap",0x0C:"System",0x0D:"FileMgr",0x0E:"Driver",0x0F:"Descriptor"}
LANGUAGES = {0x00:"Data",0x01:"68000",0x02:"BASIC",0x03:"Pascal",0x04:"C",0x06:"Fortran"}

def u16(d,o): return (d[o]<<8)|d[o+1]
def u32(d,o): return (d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]

def read_name(d, i, name_off):
    no = i + name_off; name=""
    while no < len(d) and len(name) < 32:
        c=d[no]; ch=c&0x7F
        if ch<0x20 or ch>=0x7F: break
        name+=chr(ch)
        if c&0x80: break
        no+=1
    return name

def decode_header(d, off):
    h={}
    h["sync"]    = u16(d,off+0x00)
    h["sysrev"]  = u16(d,off+0x02)
    h["size"]    = u32(d,off+0x04)   # M$Size  32 bits
    h["owner"]   = u32(d,off+0x08)   # M$Owner 32 bits
    h["name_off"]= u32(d,off+0x0C)   # M$Name  32 bits
    h["accs"]    = u16(d,off+0x10)   # M$Accs
    h["type"]    = d[off+0x12]       # M$Type  @ 0x12
    h["lang"]    = d[off+0x13]       # M$Lang  @ 0x13
    h["attr"]    = d[off+0x14]       # M$Attr
    h["revs"]    = d[off+0x15]       # M$Revs
    h["edition"] = u16(d,off+0x16)   # M$Edit
    # Champs executables (modules OS-9/68k) commencent vers 0x2C
    h["exec"]    = u32(d,off+0x2C)   # M$Exec
    h["excpt"]   = u32(d,off+0x30)   # M$Excpt
    h["mem"]     = u32(d,off+0x34)   # M$Mem
    h["stack"]   = u32(d,off+0x38)   # M$Stack
    h["idata"]   = u32(d,off+0x3C)   # M$IData
    h["irefs"]   = u32(d,off+0x40)   # M$IRefs
    h["init"]    = u32(d,off+0x44)   # M$Init
    h["term"]    = u32(d,off+0x48)   # M$Term
    h["name"]    = read_name(d, off, h["name_off"])
    return h

def main():
    if len(sys.argv)<3:
        print("Usage: python extract_module.py <deswap.bin> <offset_hex> [taille_hex]"); return
    path=sys.argv[1]; off=int(sys.argv[2],16)
    with open(path,"rb") as f: data=f.read()
    h=decode_header(data,off)
    size=int(sys.argv[3],16) if len(sys.argv)>3 else h["size"]
    print("="*60)
    print("  MODULE @ 0x%06X" % off)
    print("="*60)
    print("  Nom        : %s" % h["name"])
    print("  Sync       : 0x%04X %s" % (h["sync"],"OK" if h["sync"]==0x4AFC else "!!!"))
    print("  Taille     : 0x%X (%d octets)" % (h["size"],h["size"]))
    print("  Type       : %s" % MODULE_TYPES.get(h["type"],"0x%02X"%h["type"]))
    print("  Langage    : %s" % LANGUAGES.get(h["lang"],"0x%02X"%h["lang"]))
    print("  Edition    : %d" % h["edition"])
    print("  Nom @abs   : 0x%06X" % (off+h["name_off"]))
    if h["type"] in (0x01,0x0B,0x0C,0x0D,0x0E):
        print("  --- Offsets executables (relatifs au module) ---")
        for k in ("exec","excpt","mem","stack","idata","irefs","init","term"):
            print("  %-6s: 0x%06X" % (k,h[k]))
    outdir=os.path.join(os.path.dirname(path),"modules")
    os.makedirs(outdir,exist_ok=True)
    safe="".join(c if c.isalnum() or c in "._-" else "_" for c in h["name"]) or "module_%06X"%off
    outpath=os.path.join(outdir,"%s.os9"%safe)
    with open(outpath,"wb") as f: f.write(data[off:off+size])
    print("\n  Extrait -> %s (%d octets)" % (outpath,size))
    if h["type"] in (0x01,0x0B,0x0C,0x0D,0x0E):
        print("  exec absolu dans le fichier = 0x%06X" % (off+h["exec"]))

if __name__=="__main__":
    main()
