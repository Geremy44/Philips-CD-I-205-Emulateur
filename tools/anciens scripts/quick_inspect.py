# quick_inspect.py — Analyse les premiers blocs
data = open(r"D:\EMU CD-I\Philips-CD-I-205-Emulateur-main\CDI-205\ROMs\CDI205-00_PS-7211_REL.2.1.bin", "rb").read()

print("=== PREMIERS 512 OCTETS ===")
for off in range(0, 512, 16):
    hex_str = ' '.join(f'{b:02X}' for b in data[off:off+16])
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[off:off+16])
    print(f"{off:04X}  {hex_str:<48}  {ascii_str}")

print("\n=== RECHERCHE DE VECTEURS M68K ===")
# Cherche un pattern d'exception handlers
for off in range(0, min(0x400, len(data)), 4):
    v1 = int.from_bytes(data[off:off+4], 'big')
    v2 = int.from_bytes(data[off+4:off+8], 'big') if off+8 <= len(data) else 0
    if 0x00000100 <= v1 <= 0x00100000 or 0x00000100 <= v2 <= 0x00100000:
        print(f"  @{off:04X}: {v1:08X} {v2:08X}")

print("\n=== MIDDLE (0x40000) ===")
for off in range(0x40000, 0x40000 + 128, 16):
    hex_str = ' '.join(f'{b:02X}' for b in data[off:off+16])
    ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in data[off:off+16])
    print(f"{off:06X}  {hex_str:<48}  {ascii_str}")
