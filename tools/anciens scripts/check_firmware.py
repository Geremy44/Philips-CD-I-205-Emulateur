# check_firmware.py — Vérifie l'intégrité du dump EEPROM
import sys
import struct

def main():
    if len(sys.argv) < 2:
        print("Usage: python check_firmware.py <firmware.bin>")
        sys.exit(1)

    path = sys.argv[1]
    data = open(path, "rb").read()
    size = len(data)

    print(f"📦 Firmware : {path}")
    print(f"   Taille réelle : {size:,} octets (0x{size:x})")
    print()

    # Vérifie zones attendues
    checks = [
        (0x00000, 0x20000, "ROM système (128 KB)"),
        (0x20000, 0x40000, "Espace FMV/data (128 KB)"),
    ]

    for start, end, label in checks:
        if size >= end:
            chunk = data[start:end]
            ff_count = chunk.count(b'\xff')
            zero_count = chunk.count(b'\x00')
            entropy = 100 * (len(chunk) - ff_count - zero_count) // len(chunk)
            print(f"  [{start:#08x}..{end:#08x}] {label}")
            print(f"    Contenu : {ff_count:,} 0xFF + {zero_count:,} 0x00 → {entropy}% données")
            if ff_count == len(chunk):
                print(f"    ⚠️  VIDE (tous 0xFF)")
            elif zero_count == len(chunk):
                print(f"    ⚠️  VIDE (tous 0x00)")
        else:
            print(f"  [{start:#08x}..{end:#08x}] {label} — **MANQUANT**")

    print()
    if size == 0x40000:
        print("✅ Firmware complet (512 KB attendus)")
    elif size == 0x80000:
        print("⚠️  Firmware surdimensionné (512 KB théorique, 1 MB dumped)")
        print("   → Probabilités :")
        print("      • Miroir : [0x40000..0x80000] = copie [0x00000..0x40000]")
        print("      • Padding : [0x40000..0x80000] = 0xFF ou 0x00")
        print("   → Vérification...")
        if size >= 0x80000:
            first_half  = data[0x00000:0x40000]
            second_half = data[0x40000:0x80000]
            if first_half == second_half:
                print("      ✅ MIROIR détecté → garde les 512 KB premiers")
            elif second_half == b'\xff' * 0x40000:
                print("      ✅ PADDING 0xFF → garde les 512 KB premiers")
            elif second_half == b'\x00' * 0x40000:
                print("      ✅ PADDING 0x00 → garde les 512 KB premiers")
            else:
                print("      ❓ Deuxième moitié a de vraies données → INSPECT")
    else:
        print(f"❓ Taille inattendue : {size} octets")

    print()
    # Magic bytes
    print("🔍 Signature (premiers octets) :")
    print(f"   {data[:16].hex().upper()}")
    if data[0:4] == b'\x00\x00\x00\x00':
        print("   → Commence par des 0x00 (bootloader ou padding)")
    elif data[0:2] == b'\x4a\xfc':
        print("   → M68K RESET (SSP + PC) détecté ✅")
    else:
        print(f"   → Format inconnu, inspect manuellement")


if __name__ == "__main__":
    main()
