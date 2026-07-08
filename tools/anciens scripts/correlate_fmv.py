#!/usr/bin/env python3
"""
correlate_fmv.py - Analyse correlation pour firmware Philips CD-i 205
TOUT le résultat est écrit dans un fichier .txt (pas de sortie console)
"""
import sys
import os


def main():
    if len(sys.argv) < 2:
        print("Usage: python correlate_fmv.py <rom.bin>")
        sys.exit(1)

    bin_path = sys.argv[1]
    if not os.path.isfile(bin_path):
        print(f"Erreur: fichier introuvable: {bin_path}")
        sys.exit(1)

    # Output file : même répertoire, même nom + _correlate.txt
    base, ext = os.path.splitext(bin_path)
    out_path = base + "_correlate.txt"

    with open(bin_path, "rb") as f:
        data = f.read()

    with open(out_path, "w", encoding="utf-8") as fout:
        fout.write(f"=== ANALYSE CORRELATION FMV CD-i 205 ===\n")
        fout.write(f"Fichier source : {bin_path}\n")
        fout.write(f"Taille        : {len(data):,} octets (0x{len(data):X})\n")
        fout.write("\n")

        # ============================================================
        # SECTION 1 - References Extension Space (0x400000+)
        # ============================================================
        fout.write("=== RECHERCHE REFERENCES EXTENSION SPACE (0x400000+) ===\n")
        count_ext = 0
        for off in range(len(data) - 3):
            val = (data[off] << 24) | (data[off + 1] << 16) | (data[off + 2] << 8) | data[off + 3]
            if 0x00400000 <= val <= 0x00FFFFFF:
                ctx = data[max(0, off - 4): off + 8]
                ctx_hex = " ".join(f"{b:02X}" for b in ctx)
                fout.write(f"  @0x{off:05X}: -> 0x{val:08X}  ctx: {ctx_hex}\n")
                count_ext += 1
        fout.write(f"  Total : {count_ext} references trouvees\n\n")

        # ============================================================
        # SECTION 2 - Modules OS-9 (magic 0x4AFC)
        # ============================================================
        fout.write("=== RECHERCHE MODULES OS-9 (magic 0x4AFC) ===\n")
        count_os9 = 0
        idx = 0
        while idx < len(data) - 3:
            idx = data.find(b"\x4A\xFC", idx)
            if idx == -1:
                break
            # Taille déclarée big-endian à idx+2
            if idx + 5 < len(data):
                size = (data[idx + 2] << 24) | (data[idx + 3] << 16) | (data[idx + 4] << 8) | data[idx + 5]
            else:
                size = 0
            fout.write(f"  @0x{idx:05X}: module OS-9, taille declaree=0x{size:X}\n")
            count_os9 += 1
            idx += 2
        fout.write(f"  Total : {count_os9} modules trouves\n\n")

        # ============================================================
        # SECTION 3 - Strings MPEG/VMPEG/FMV/VSD/video/Video
        # ============================================================
        fout.write("=== RECHERCHE STRINGS 'MPEG/VMPEG/FMV/VSD/video/Video' ===\n")
        keywords = [b"MPEG", b"VMPEG", b"FMV", b"VSD", b"video", b"Video"]
        for kw in keywords:
            count_kw = 0
            idx = 0
            while idx < len(data):
                idx = data.find(kw, idx)
                if idx == -1:
                    break
                ctx = data[idx: min(idx + 24, len(data))]
                ascii_str = "".join(chr(b) if 32 <= b <= 126 else "." for b in ctx)
                fout.write(f"  @0x{idx:05X}: '{kw.decode()}' -> {ascii_str}\n")
                count_kw += 1
                idx += 1
            fout.write(f"  Total '{kw.decode()}' : {count_kw} occurrences\n\n")

    # UNIQUE sortie console
    print(f"Rapport ecrit : {out_path}")


if __name__ == "__main__":
    main()
