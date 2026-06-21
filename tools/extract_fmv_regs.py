# extract_fmv_regs.py
import sys
data = open(sys.argv[1],"rb").read()
regs = {}
for i in range(len(data)-6):
    # MOVE.L #imm,xxx : opcode haute nibble 2, basse = 7C
    if data[i]&0xF0==0x20 and data[i+1]==0x7C:
        if data[i+2]==0x00 and data[i+3]==0xE0:
            addr = (data[i+2]<<24)|(data[i+3]<<16)|(data[i+4]<<8)|data[i+5]
            regs[addr] = regs.get(addr,0)+1
for a in sorted(regs):
    print(f"  $%08X   x%d" % (a, regs[a]))
print(f"\n  {len(regs)} registres FMV uniques")
