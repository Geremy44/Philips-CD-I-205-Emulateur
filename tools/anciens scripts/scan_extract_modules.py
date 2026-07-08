#!/usr/bin/env python3
"""
ROM MODULE SCANNER & EXTRACTOR - OS-9/68k CD-i
Layout: +00 Sync +02 SysRev +04 Size(4) +08 Owner(4)
        +0C NameOff(4) +10 Access +12 Type +13 Lang
        +14 Attr +15 Rev +16 Edition
"""

import sys, struct, os

SYNC_MAGIC = 0x4AFC
MODULE_MIN_SIZE = 32
MODULE_MAX_SIZE = 0x100000

TYPES = {0x01:"Program",0x02:"Subroutine",0x03:"MultiModule",0x04:"Data",
         0x05:"CSDdata",0x0C:"System",0x0D:"FileMgr",0x0E:"Driver",0x0F:"Descriptor"}
LANGUAGES = {0x00:"Obj/Data",0x01:"M68000",0x80:"ICode"}

def u16(d,o):
    return struct.unpack(">H",d[o:o+2])[0] if o+2<=len(d) else None
def u32(d,o):
    return struct.unpack(">I",d[o:o+4])[0] if o+4<=len(d) else None

def read_os9_string(data, base_offset, max_len=32):
    """Terminée par bit-7 du dernier char OU par 0x00."""
    s = ""
    for i in range(max_len):
        pos = base_offset + i
        if pos >= len(data):
            return s
        byte = data[pos]
        if byte == 0x00:
            return s
        c = byte & 0x7F
        if c < 0x20 or c == 0x7F:
            return s
        s += chr(c)
        if byte & 0x80:
            return s
    return s

def os9_crc24(data):
    crc = 0xFFFFFF
    for byte in data:
        crc ^= (byte << 16)
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x800063
        crc &= 0xFFFFFF
    return crc

CRC_MAGIC = 0x00800FE3

def verify_crc(data, offset, size):
    if offset + size > len(data):
        return False, None
    raw = os9_crc24(data[offset:offset+size])
    return (raw == CRC_MAGIC), raw

def verify_module_header(data, offset):
    if offset + 0x30 > len(data):
        return None
    if u16(data, offset) != SYNC_MAGIC:
        return None
    try:
        sysrev      = u16(data, offset + 0x02)
        module_size = u32(data, offset + 0x04)
        owner       = u32(data, offset + 0x08)
        name_offset = u32(data, offset + 0x0C)
        access      = u16(data, offset + 0x10)
        type_code   = data[offset + 0x12]
        language    = data[offset + 0x13]
        attr        = data[offset + 0x14]
        revision    = data[offset + 0x15]
        edition     = u16(data, offset + 0x16)

        if module_size < MODULE_MIN_SIZE or module_size > MODULE_MAX_SIZE:
            return None
        if offset + module_size > len(data):
            return None
        if name_offset < 0x14 or name_offset >= module_size:
            return None

        name = read_os9_string(data, offset + name_offset)
        if not name:
            return None

        crc_ok, crc_raw = verify_crc(data, offset, module_size)

        return {
            "offset": offset, "size": module_size, "name": name,
            "type_code": type_code,
            "type_name": TYPES.get(type_code, f"Unknown({type_code:02X})"),
            "language": language,
            "language_name": LANGUAGES.get(language, f"?{language:02X}"),
            "revision": revision, "edition": edition,
            "access": access, "attr": attr,
            "crc_ok": crc_ok, "crc_raw": crc_raw
        }
    except Exception:
        return None

def scan_rom(rom_data, output_dir, strict_crc=False):
    print("\n🔍 Scanning for sync markers (0x4AFC)...")
    sync_positions = []
    i = 0
    while i < len(rom_data) - 1:
        if rom_data[i] == 0x4A and rom_data[i+1] == 0xFC:
            sync_positions.append(i)
        i += 1
    print(f"📈 {len(sync_positions)} sync markers trouvés")

    modules = []
    for pos in sync_positions:
        mod = verify_module_header(rom_data, pos)
        if mod:
            modules.append(mod)

    valid_crc = sum(1 for m in modules if m['crc_ok'])
    print(f"   {len(modules)} modules header-valides ({valid_crc} avec CRC OK)\n")

    if not modules:
        print("⚠️  Aucun module trouvé !")
        return []

    print("=" * 70)
    print(f"  {'OFFSET':<10} {'NAME':<14} {'TYPE':<12} {'LANG':<10} {'SIZE':<8} CRC")
    print("=" * 70)
    for m in modules:
        crc = "✓" if m['crc_ok'] else f"✗(0x{m['crc_raw']:06X})"
        print(f"  0x{m['offset']:06X}  {m['name']:<14} {m['type_name']:<12} "
              f"{m['language_name']:<10} 0x{m['size']:04X}   {crc}")
    print("=" * 70)

    os.makedirs(output_dir, exist_ok=True)
    extracted = 0
    for m in modules:
        if strict_crc and not m['crc_ok']:
            continue
        safe_name = "".join(c if c.isalnum() else "_" for c in m['name'])
        fname = f"{m['offset']:06X}_{safe_name}_{m['type_name']}.bin"
        fpath = os.path.join(output_dir, fname)
        with open(fpath, 'wb') as f:
            f.write(rom_data[m['offset']:m['offset']+m['size']])
        extracted += 1
    print(f"\n💾 {extracted} modules extraits dans : {output_dir}")
    return modules

def main():
    if len(sys.argv) < 2:
        print("Usage: python scan_extract_modules.py <rom_file> [output_dir] [--strict-crc]")
        sys.exit(1)

    rom_file = sys.argv[1]
    output_dir = "modules"
    strict = False
    for arg in sys.argv[2:]:
        if arg == "--strict-crc":
            strict = True
        else:
            output_dir = arg

    if not os.path.isfile(rom_file):
        print(f"❌ Fichier introuvable : {rom_file}")
        sys.exit(1)

    print("=" * 70)
    print("  ROM MODULE SCANNER & EXTRACTOR")
    print("=" * 70)

    with open(rom_file, 'rb') as f:
        rom_data = f.read()

    print(f"\n📊 ROM size: {len(rom_data)} octets ({len(rom_data)/1024:.1f} KB)")
    print(f"📁 Output: {os.path.abspath(output_dir)}")

    modules = scan_rom(rom_data, output_dir, strict_crc=strict)

    if modules:
        print("\n🎯 Drivers / FileMgr détectés :")
        for m in modules:
            if m['type_name'] in ['Driver', 'FileMgr']:
                crc = "✓" if m['crc_ok'] else "✗"
                print(f"   [{crc}CRC] 0x{m['offset']:06X}  {m['name']}")
        print()

if __name__ == "__main__":
    main()
