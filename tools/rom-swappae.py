#!/usr/bin/env python3
import struct

def hex_to_bin_swap(hexfile, binfile):
    # Décodage basique du Intel HEX
    data = {}
    seg = 0
    with open(hexfile) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != ':':
                # skip entête/corruption non-standard (ex: tes 12 octets initiaux)
                continue
            count = int(line[1:3], 16)
            addr  = int(line[3:7], 16) + seg
            rtype = int(line[7:9], 16)
            payload = bytes.fromhex(line[9:9+count*2])
            if rtype == 0:
                for i, b in enumerate(payload):
                    data[addr + i] = b
            elif rtype == 4:
                seg = struct.unpack('>H', payload)[0] << 16
            # ignorer EOF etc.

    # Reconstruction linéaire
    if not data:
        return
    max_addr = max(data.keys())
    raw = bytearray(max_addr + 1)
    for k, v in data.items():
        raw[k] = v

    # Dé-swap : inverser chaque paire d'octets (U7/U6 swap)
    native = bytearray(len(raw))
    for i in range(0, len(raw) - 1, 2):
        native[i]   = raw[i+1]
        native[i+1] = raw[i]

    with open(binfile, 'wb') as f:
        f.write(native)

hex_to_bin_swap("D:\EMU CD-I\Philips-CD-I-205-Emulateur-main\CDI-205\ROMs\CDI205-00_PS-7211_REL.2.1.hex", "D:\EMU CD-I\Philips-CD-I-205-Emulateur-main\CDI-205\ROMs\CDI205-00_PS-7211_REL.2.1_swapped.bin")
