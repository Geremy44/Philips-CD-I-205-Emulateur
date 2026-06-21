#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract_module.py — extrait un module OS-9 par son nom"""
import sys, os

def u32(d,o): return (d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]

def read_name(d,i,off):
    no=i+off; s=""
    while no<len(d) and len(s)<32:
        c=d[no]; ch=c&0x7F
        if ch<0x20 or ch>=0x7F: break
        s+=chr(ch)
        if c&0x80: break
        no+=1
    return s

def main(path, target):
    with open(path,"rb") as f: d=f.read()
    outdir=os.path.join(os.path.dirname(path),"modules")
    os.makedirs(outdir,exist_ok=True)
    for i in range(0,len(d)-0x18,2):
        if d[i]==0x4A and d[i+1]==0xFC:
            size=u32(d,i+4); noff=u32(d,i+0x0C)
            if size==0 or size>len(d) or i+size>len(d): continue
            name=read_name(d,i,noff) if 0<noff<len(d) else ""
            if name.lower()==target.lower():
                out=os.path.join(outdir,f"{name}.bin")
                with open(out,"wb") as g: g.write(d[i:i+size])
                print(f"✅ Extrait '{name}' @0x{i:06X} ({size} octets)")
                print(f"   → {out}")
                # exec offset (M$Exec @ 0x16, 4 octets pour prog/driver)
                if i+0x1A<=len(d):
                    mexec=u32(d,i+0x16)
                    print(f"   M$Exec (entrée) = 0x{mexec:06X} (relatif au module)")
                return
    print(f"❌ Module '{target}' introuvable")

if __name__=="__main__":
    main(sys.argv[1], sys.argv[2])
