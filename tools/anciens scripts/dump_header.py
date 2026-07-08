import sys
with open(sys.argv[1], 'rb') as f:
    data = f.read()
off = 0x11F0E
print(f"Header @ 0x{off:X}:")
for i in range(0, 0x30, 16):
    row = data[off+i:off+i+16]
    hexs = " ".join(f"{b:02X}" for b in row)
    print(f"  +{i:02X}: {hexs}")
