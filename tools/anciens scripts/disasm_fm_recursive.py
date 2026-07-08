#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""disasm_fm_recursive.py - Desassembleur recursif File Manager OS-9/CD-i (68000).

Usage: python disasm_fm_recursive.py <fichier.os9> [base_hex]
"""
import sys, os
from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000

FM_ROUTINES = ["Create","Open","MakDir","ChgDir","Delete","Seek",
               "Read","Write","ReadLn","WriteLn","GetStat","SetStat","Close"]

# Types de module OS-9
MOD_TYPES = {0x01:"Prog",0x02:"Subroutine",0x03:"Multi",0x04:"Data",
             0x05:"CSDdata",0x0C:"SysModule",0x0D:"FileMgr",
             0x0E:"DeviceDrv",0x0F:"DeviceDesc"}

def u16(d,o): return (d[o]<<8)|d[o+1]
def u32(d,o): return (d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]
def s16(v):   return v-0x10000 if v&0x8000 else v
def s8(v):    return v-0x100 if v&0x80 else v

def read_name(d, abs_off):
    no=abs_off; name=""
    while no<len(d) and len(name)<40:
        c=d[no]; ch=c&0x7F
        if ch<0x20 or ch>=0x7F: break
        name+=chr(ch)
        if c&0x80: break
        no+=1
    return name

def decode_header(d, base):
    """Decode l'en-tete OS-9/68k complet."""
    h={}
    h['sync']     = u16(d,base+0x00)
    h['sysrev']   = u16(d,base+0x02)
    h['size']     = u32(d,base+0x04)
    h['owner']    = u32(d,base+0x08)
    h['name_off'] = u32(d,base+0x0C)
    h['accs']     = u16(d,base+0x10)
    h['type']     = d[base+0x12]
    h['lang']     = d[base+0x13]
    h['attr']     = d[base+0x14]
    h['revis']    = d[base+0x15]
    h['edit']     = u16(d,base+0x16)
    h['usage']    = u32(d,base+0x18)
    h['symbol']   = u32(d,base+0x1C)
    # 0x20-0x2F : reserve / specifique
    h['name']     = read_name(d, base + h['name_off'])
    return h

def main():
    if len(sys.argv)<2:
        print("Usage: python disasm_fm_recursive.py <fichier.os9> [base_hex]"); return
    path=sys.argv[1]
    d=open(path,"rb").read()
    base=int(sys.argv[2],16) if len(sys.argv)>2 else 0

    H=decode_header(d,base)
    size=H['size']
    mod=d[base:base+size]          # le module isole, adresses relatives 0..size
    N=len(mod)

    # --- Pointeur vers la table FM : champ a l'offset 0x32 de l'en-tete ---
    fm_tbl_off = u16(mod, 0x32)          # = 0x0062 pour cdfm
    print(f"Table FM @ 0x{fm_tbl_off:X}")

    fm_table=[]
    for i,rn in enumerate(FM_ROUTINES):
        off=u16(mod, fm_tbl_off + i*2)
        fm_table.append((rn,off))

    # ---- Initialisation du desassembleur Capstone ----
    md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
    md.detail = False

    # ---- Moteur recursif ----
    CODE=bytearray(N)        # 1 = octet de code
    insn_at={}               # addr -> (mnemonic, op_str, size, bytes)
    labels=set()             # cibles de saut -> labels
    calls=set()              # cibles de bsr/jsr -> sous-routines
    visited=set()

    # seeds = entrees FM valides
    queue=[]
    for rn,off in fm_table:
        if 0 < off < N and off != 0x4E75:
            queue.append(off); calls.add(off)

    def branch_target(insn):
        """Retourne (cible, type) ou None. type: 'jmp','call','cond'."""
        m=insn.mnemonic
        op=insn.op_str.strip()
        tgt=None
        if op.startswith("$"):
            try: tgt=int(op[1:],16)
            except: tgt=None
        if m in ("bra","jmp"):              return (tgt,'jmp')
        if m in ("bsr","jsr"):              return (tgt,'call')
        if m=="rts" or m=="rte" or m=="rtr":return (None,'stop')
        if m.startswith("b") and len(m)>=3: return (tgt,'cond')   # bcc,bne,beq...
        if m.startswith("db"):              return (tgt,'cond')   # dbra,dbne...
        return (None,'seq')

    while queue:
        addr=queue.pop()
        if addr in visited: continue
        # desassemble en sequence depuis addr
        a=addr
        while 0<=a<N:
            if a in visited: break
            chunk=bytes(mod[a:a+10])
            gen=md.disasm(chunk,a,count=1)
            insn=next(gen,None)
            if insn is None:
                CODE[a]=1; visited.add(a); break   # octet indecodable
            visited.add(a)
            for b in range(insn.address, insn.address+insn.size):
                if b<N: CODE[b]=1
            insn_at[a]=(insn.mnemonic,insn.op_str,insn.size,insn.bytes)
            tgt,kind=branch_target(insn)
            if tgt is not None and 0<=tgt<N:
                if kind=='call':
                    calls.add(tgt);  queue.append(tgt)
                else:
                    labels.add(tgt); queue.append(tgt)
            a_next=insn.address+insn.size
            if kind in ('jmp','stop'):
                break          # fin de sequence lineaire
            a=a_next

    # ---- Generation du listing ----
    L=[]
    ap=L.append
    ap("; ====================================================")
    ap(";  DESASSEMBLAGE RECURSIF - File Manager OS-9 / CD-i")
    ap("; ====================================================")
    ap(";  EN-TETE MODULE OS-9/68k")
    ap("; ----------------------------------------------------")
    ap(f";   Nom module   : {H['name']}")
    ap(f";   Sync ID      : 0x{H['sync']:04X}  {'OK' if H['sync']==0x4AFC else 'INVALIDE'}")
    ap(f";   SysRev       : 0x{H['sysrev']:04X}")
    ap(f";   Taille       : 0x{H['size']:X} ({H['size']} octets)")
    ap(f";   Owner        : 0x{H['owner']:08X}")
    ap(f";   Offset nom   : 0x{H['name_off']:X}")
    ap(f";   Acces        : 0x{H['accs']:04X}")
    ap(f";   Type         : 0x{H['type']:02X} ({MOD_TYPES.get(H['type'],'?')})")
    ap(f";   Langage      : 0x{H['lang']:02X}")
    ap(f";   Attr/Revis   : 0x{H['attr']:02X} / 0x{H['revis']:02X}")
    ap(f";   Edition      : {H['edit']}")
    ap(f";   Usage        : 0x{H['usage']:08X}")
    ap(f";   Symbol       : 0x{H['symbol']:08X}")
    ap("; ----------------------------------------------------")
    ap(";  TABLE D'ENTREE FILE MANAGER (13 routines)")
    for rn,off in fm_table:
        flag="" if 0<off<N else "  (!! hors module)"
        ap(f";    %-8s -> 0x%04X%s"%(rn,off,flag))
    ap("; ====================================================")
    ap("")

    # map offset routine FM -> nom
    fm_at={off:rn for rn,off in fm_table if 0<off<N}

    stats_code=sum(CODE); stats_data=N-stats_code

    i=0
    while i<N:
        if CODE[i] and i in insn_at:
            mn,ops,sz,bts=insn_at[i]
            # label / entete routine
            if i in fm_at:
                ap("")
                ap(f"; ========== FM ROUTINE : {fm_at[i]}  (0x{i:04X}) ==========")
            elif i in calls:
                ap("")
                ap(f"sub_%04X:"%i)
            elif i in labels:
                ap(f"lab_%04X:"%i)
            raw=" ".join("%02X"%b for b in bts)
            note=""
            if mn=="trap" and "#$0" in ops:
                note="   ; <- OS-9 system call (I$/F$)"
            # reecrire cibles en labels lisibles
            disp=ops
            if ops.strip().startswith("$"):
                try:
                    t=int(ops.strip()[1:],16)
                    if t in fm_at:       disp="FM_%s"%fm_at[t]
                    elif t in calls:     disp="sub_%04X"%t
                    elif t in labels:    disp="lab_%04X"%t
                except: pass
            ap("%06X:  %-22s  %-8s %s%s"%(i,raw,mn,disp,note))
            i+=sz
        else:
            # zone de donnees : regrouper en lignes de 8 octets
            start=i
            run=bytearray()
            while i<N and not (CODE[i] and i in insn_at):
                run.append(mod[i]); i+=1
                if len(run)>=8: break
            hexb=" ".join("%02X"%b for b in run)
            asc="".join(chr(b) if 0x20<=b<0x7F else "." for b in run)
            ap("%06X:  dc.b   %-24s ; %s"%(start,hexb,asc))

    ap("")
    ap("; ====================================================")
    ap(f";  Couverture : {stats_code} octets CODE / {stats_data} octets DATA")
    ap(f";  Total {N} octets  |  {len(insn_at)} instructions")
    ap(f";  Sous-routines detectees : {len(calls)}  |  Labels : {len(labels)}")
    ap("; ====================================================")

    outdir=os.path.join(os.path.dirname(os.path.abspath(path)),"disasm")
    os.makedirs(outdir,exist_ok=True)
    safe="".join(c if c.isalnum() or c in "._-" else "_" for c in H['name'])
    outpath=os.path.join(outdir,f"{safe}_recursive.asm")
    open(outpath,"w",encoding="utf-8").write("\n".join(L))

    print(f"OK -> {outpath}")
    print(f"   CODE={stats_code}o  DATA={stats_data}o  ({100*stats_code//N}% code)")
    print(f"   {len(insn_at)} instructions, {len(calls)} sous-routines, {len(labels)} labels")

if __name__=="__main__":
    main()
