# bin_to_hex.py — Convertit bin en Intel HEX avec offset EEPROM
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: python bin_to_hex.py <input.bin> [output.hex] [base_addr]")
        print("  base_addr : adresse de départ (défaut: 0x0000)")
        sys.exit(1)

    infile = sys.argv[1]
    outfile = sys.argv[2] if len(sys.argv) > 2 else infile.replace('.bin', '.hex')
    base_addr = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x0000

    data = open(infile, "rb").read()
    print(f"📖 Lecture : {infile} ({len(data):,} octets)")
    print(f"📝 Sortie  : {outfile}")
    print(f"🎯 Adresse de base : 0x{base_addr:08x}")
    print()

    # Intel HEX : 16 octets/ligne
    lines = []
    current_hi = None

    for offset in range(0, len(data), 16):
        chunk = data[offset:offset+16]
        addr = base_addr + offset

        # Extended Linear Address Record si Hi byte change
        hi = (addr >> 16) & 0xFFFF
        if hi != current_hi:
            current_hi = hi
            # :02000004<HHLL>CC
            checksum = (0x02 + 0x00 + 0x00 + 0x04 + (hi >> 8) + (hi & 0xFF)) & 0xFF
            checksum = (0x100 - checksum) & 0xFF
            lines.append(f":02000004{hi:04X}{checksum:02X}")

        # Data Record
        lo = addr & 0xFFFF
        bytecount = len(chunk)
        hexdata = ''.join(f'{b:02X}' for b in chunk)
        
        # Checksum = not(count + addr_hi + addr_lo + rectype + data) + 1
        cs = bytecount + (lo >> 8) + (lo & 0xFF) + 0x00
        for b in chunk:
            cs += b
        checksum = (0x100 - (cs & 0xFF)) & 0xFF

        lines.append(f":{bytecount:02X}{lo:04X}00{hexdata}{checksum:02X}")

    # End of File
    lines.append(":00000001FF")

    # Écrit
    with open(outfile, "w") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"✅ {len(lines)} lignes écrites")
    print(f"   Couverture : 0x{base_addr:08x} → 0x{base_addr + len(data):08x}")
    print(f"\n📤 Prêt à envoyer : {outfile}")


if __name__ == "__main__":
    main()
