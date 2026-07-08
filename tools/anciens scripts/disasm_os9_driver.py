#!/usr/bin/env python3
# disasm_os9_driver.py — Désassemble un module driver OS-9/68k
import sys, struct

try:
    from capstone import *
    from capstone.m68k import *
    HAVE_CAPSTONE = True
except ImportError:
    HAVE_CAPSTONE = False

def rd16(d, o): return struct.unpack(">H", d[o:o+2])[0]
def rd32(d, o): return struct.unpack(">I", d[o:o+4])[0]

def parse_header(d):
    """Header OS-9/68k module."""
    assert rd16(d, 0) == 0x4AFC, "Pas un module OS-9 (sync != 0x4AFC)"
    h = {
        'sysrev'   : rd16(d, 0x02),
        'size'     : rd32(d, 0x04),
        'owner'    : rd32(d, 0x08),
        'name_off' : rd32(d, 0x0C),
        'accs'     : rd16(d, 0x10),
        'type'     : d[0x12],
        'lang'     : d[0x13],
        'attr'     : d[0x14],
        'revis'    : d[0x15],
        'edition'  : rd16(d, 0x16),
        # --- header exécutable (type Driver/Program) ---
        'exec_off' : rd32(d, 0x2C),   # M$Exec
        'excpt_off': rd32(d, 0x30),   # M$Excpt
        'mem_size' : rd32(d, 0x34),   # M$Mem
        'stack'    : rd32(d, 0x38),   # M$Stack
        'idata'    : rd32(d, 0x3C),   # M$IData
        'irefs'    : rd32(d, 0x40),   # M$IRefs
    }
    return h

def read_name(d, off):
    s = b""
    while off < len(d) and d[off] != 0:
        s += bytes([d[off] & 0x7F]); off += 1
    return s.decode('latin1', 'replace')

def scan_hw_constants(data):
    """Cherche des longs/words ressemblant à des adresses HW dans tout le module."""
    print(f"\n{'='*64}\n  🔎 SCAN constantes HW (longs 32 bits)\n{'='*64}")
    found = 0
    for o in range(0, len(data)-4, 2):
        val = rd32(data, o)
        # zones registres CD-i typiques
        if (0x00300000 <= val <= 0x00400000) or \
           (0x004FFF00 <= val <= 0x00500100) or \
           (0x00E00000 <= val <= 0x00E10000):
            print(f"   offset 0x{o:04X} : 0x{val:08X}")
            found += 1
    if not found:
        print("   (aucune constante d'adresse HW trouvée)")

def main():
    if len(sys.argv) < 2:
        print("Usage: disasm_os9_driver.py <module.bin> [max_instr] [start_offset_hex]")
        print("  Exemples:")
        print("    disasm_os9_driver.py fmvdrv.bin 60 0       (début module)")
        print("    disasm_os9_driver.py fmvdrv.bin 80 1E8C    (Init driver)")
        print("    disasm_os9_driver.py fmvdrv.bin 80 1EA0    (vrai code Init)")
        return
    path = sys.argv[1]
    max_instr = int(sys.argv[2]) if len(sys.argv) > 2 else 400

    with open(path, "rb") as f:
        data = f.read()

    h = parse_header(data)
    name = read_name(data, h['name_off'])

    print("="*64)
    print(f"  MODULE : {name}")
    print("="*64)
    print(f"  Size       : 0x{h['size']:X} ({h['size']} o)")
    print(f"  Type/Lang  : 0x{h['type']:02X} / 0x{h['lang']:02X}")
    print(f"  Edition    : {h['edition']}")
    print(f"  --- Entry points (offsets module) ---")
    print(f"  M$Exec     : 0x{h['exec_off']:X}")
    print(f"  M$Excpt    : 0x{h['excpt_off']:X}")
    print(f"  M$Mem      : 0x{h['mem_size']:X}")
    print(f"  M$Stack    : 0x{h['stack']:X}")
    print(f"  M$IData    : 0x{h['idata']:X}")

    # --- Table des entry points du DRIVER (à M$Exec) ---
    exe = h['exec_off']
    if 0 < exe < len(data)-2:
        print(f"\n  --- Driver branch table @ 0x{exe:X} ---")
        labels = ["Init","Read","Write","GetStat","SetStat","Term","Trap?"]
        for i,lbl in enumerate(labels):
            o = exe + i*2
            if o+2 > len(data): break
            disp = rd16(data, o)
            if disp & 0x8000: disp -= 0x10000   # signed
            tgt = o + disp
            warn = "  ⚠️ HORS LIMITES" if not (0 <= tgt < len(data)) else ""
            print(f"   {lbl:9s}: +0x{o:04X}  disp={disp:+d}  → 0x{tgt:04X}{warn}")

    # --- Scan constantes HW dans tout le module ---
    scan_hw_constants(data)

    if not HAVE_CAPSTONE:
        print("\n⚠️ Capstone non installé → pip install capstone")
        return

    # --- start_offset configurable (3e argument) ---
    if len(sys.argv) > 3:
        start = int(sys.argv[3], 16)
    else:
        start = exe   # M$Exec par défaut

    if not (0 <= start < len(data)):
        print(f"\n⚠️ start_offset 0x{start:X} hors limites (taille=0x{len(data):X})")
        return

    # --- Désassemblage ---
    md = Cs(CS_ARCH_M68K, CS_MODE_BIG_ENDIAN | CS_MODE_M68K_000)
    md.detail = True

    print(f"\n{'='*64}\n  DISASSEMBLY depuis 0x{start:X}\n{'='*64}")
    code = data[start:]
    count = 0
    io_hits = []
    reg_loads = []
    for insn in md.disasm(code, start):
        line = f"  0x{insn.address:05X}:  {insn.mnemonic:8s} {insn.op_str}"

        # --- détection d'adresses absolues hautes (registres MPEG/HW) ---
        for tok in insn.op_str.replace(',', ' ').replace('(', ' ').replace(')', ' ').split():
            t = tok.strip()
            if t.startswith('$') or t.startswith('0x'):
                try:
                    val = int(t.lstrip('$').replace('0x',''), 16)
                    if (0x300000 <= val <= 0x4FFFFF) or (0xE00000 <= val <= 0xEFFFFF):
                        io_hits.append((insn.address, val, insn.mnemonic, insn.op_str))
                        line += "   ; <-- I/O HW ?"
                        break
                except:
                    pass

        # --- détection chargement registres d'adresse a2/a3/a6 ---
        mn = insn.mnemonic.lower()
        if mn in ("movea.l", "lea", "movea.w", "move.l"):
            ops = insn.op_str.lower()
            for areg in ("a2", "a3", "a6"):
                if ops.endswith(areg) or ops.endswith(areg + ")"):
                    reg_loads.append((insn.address, insn.mnemonic, insn.op_str))
                    line += f"   ; <== load {areg.upper()}"
                    break

        print(line)
        count += 1
        if count >= max_instr:
            print("  ... (limite atteinte)")
            break

    # --- Résumés ---
    if reg_loads:
        print(f"\n{'='*64}\n  📍 CHARGEMENTS a2/a3/a6 ({len(reg_loads)})\n{'='*64}")
        for addr, mn, ops in reg_loads:
            print(f"   @0x{addr:05X}  {mn:8s} {ops}")

    if io_hits:
        print(f"\n{'='*64}\n  🎯 ACCÈS HARDWARE potentiels ({len(io_hits)})\n{'='*64}")
        for addr, val, mn, ops in io_hits:
            print(f"   @0x{addr:05X}  {mn:8s} {ops}   → reg 0x{val:06X}")

if __name__ == "__main__":
    main()
