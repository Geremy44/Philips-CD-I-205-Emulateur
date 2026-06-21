# extract_fmv_any.py — toute occurrence d'adresse $00E0xxxx
import sys
data = open(sys.argv[1],"rb").read()
regs = {}
for i in range(len(data)-4):
    if data[i]==0x00 and data[i+1]==0xE0:
        addr=(data[i]<<24)|(data[i+1]<<16)|(data[i+2]<<8)|data[i+3]
        # filtre plages plausibles
        if 0x00E00000<=addr<=0x00E0FFFF:
            regs[addr]=regs.get(addr,0)+1
for a in sorted(regs):
    print("  $%08X   x%d"%(a,regs[a]))
print("\n  %d adresses $00E0xxxx"%len(regs))
