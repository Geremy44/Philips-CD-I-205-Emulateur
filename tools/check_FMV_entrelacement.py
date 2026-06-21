# check_FMV_entrelacement.py
"""
Entrelace deux EPROM 16-bit, teste les deux ordres d'octets,
détermine le bon via détection de chaînes lisibles,
et sauvegarde le fichier entrelacé correct.

Usage:
    python check_FMV_entrelacement.py "chemin/P7307.bin" "chemin/P7308.bin"
"""
import sys
import os
import re

# Mots-clés attendus dans le firmware FMV (CD-i / MPEG)
KEYWORDS = [
    b'dummy', b'MPEG', b'mpeg', b'R4.1', b'R4', b'Philips', b'PHILIPS',
    b'Error', b'error', b'ERROR', b'Version', b'version', b'VERSION',
    b'FMV', b'Audio', b'audio', b'Video', b'video', b'Settings',
    b'CD-i', b'CDI', b'OS9', b'OS-9', b'Microware', b'init', b'Init',
    b'buffer', b'Buffer', b'decode', b'Decode', b'stream', b'Stream',
    b'sync', b'Sync', b'reset', b'Reset', b'status', b'Status',
]

MIN_STR_LEN = 4  # longueur mini d'une chaîne ASCII


def interleave(hi, lo):
    """Entrelace : hi fournit l'octet de poids fort, lo le poids faible."""
    out = bytearray(len(hi) + len(lo))
    out[0::2] = hi
    out[1::2] = lo
    return bytes(out)


def extract_strings(data, min_len=MIN_STR_LEN):
    """Extrait les chaînes ASCII imprimables."""
    pattern = re.compile(rb'[\x20-\x7E]{%d,}' % min_len)
    return pattern.findall(data)


def score_strings(strings):
    """Compte les occurrences de mots-clés connus."""
    score = 0
    found = []
    blob = b'\n'.join(strings)
    for kw in KEYWORDS:
        count = blob.count(kw)
        if count > 0:
            score += count
            found.append((kw.decode('latin1'), count))
    return score, found


def analyze_order(name, hi, lo):
    """Entrelace selon un ordre et évalue."""
    data = interleave(hi, lo)
    strings = extract_strings(data)
    score, found = score_strings(strings)
    return data, strings, score, found


def main():
    if len(sys.argv) != 3:
        print("Usage: python check_FMV_entrelacement.py <P7307.bin> <P7308.bin>")
        sys.exit(1)

    path_a, path_b = sys.argv[1], sys.argv[2]

    with open(path_a, 'rb') as f:
        rom_a = f.read()  # P7307 (4BA9)
    with open(path_b, 'rb') as f:
        rom_b = f.read()  # P7308 (FFD9)

    print("=" * 70)
    print("  ENTRELACEMENT FMV - DÉTECTION DU BON ORDRE D'OCTETS")
    print("=" * 70)
    print(f"\n📄 ROM A : {os.path.basename(path_a)} ({len(rom_a)} octets)")
    print(f"📄 ROM B : {os.path.basename(path_b)} ({len(rom_b)} octets)")

    # Vérif checksums
    sum_a = sum(rom_a) & 0xFFFF
    sum_b = sum(rom_b) & 0xFFFF
    print(f"\n🔐 Checksum A : 0x{sum_a:04X}")
    print(f"🔐 Checksum B : 0x{sum_b:04X}")

    if len(rom_a) != len(rom_b):
        print("\n⚠️  ATTENTION : tailles différentes ! L'entrelacement sera tronqué.")
        n = min(len(rom_a), len(rom_b))
        rom_a, rom_b = rom_a[:n], rom_b[:n]

    # Test des deux ordres
    print("\n" + "=" * 70)
    print("  TEST DES DEUX ORDRES")
    print("=" * 70)

    # Ordre 1 : A=poids fort (MSB), B=poids faible (LSB)
    data1, str1, score1, found1 = analyze_order("A=MSB / B=LSB", rom_a, rom_b)
    # Ordre 2 : B=poids fort (MSB), A=poids faible (LSB)
    data2, str2, score2, found2 = analyze_order("B=MSB / A=LSB", rom_b, rom_a)

    print(f"\n── Ordre 1 : A(P7307)=MSB, B(P7308)=LSB ──")
    print(f"   Chaînes trouvées : {len(str1)}")
    print(f"   Score mots-clés  : {score1}")
    print(f"   Mots-clés : {', '.join(f'{k}×{c}' for k,c in found1[:15])}")

    print(f"\n── Ordre 2 : B(P7308)=MSB, A(P7307)=LSB ──")
    print(f"   Chaînes trouvées : {len(str2)}")
    print(f"   Score mots-clés  : {score2}")
    print(f"   Mots-clés : {', '.join(f'{k}×{c}' for k,c in found2[:15])}")

    # Décision
    print("\n" + "=" * 70)
    print("  RÉSULTAT")
    print("=" * 70)

    if score1 >= score2:
        best_data, best_str, best_found = data1, str1, found1
        order = "A=MSB (P7307 poids fort) / B=LSB (P7308 poids faible)"
        suffix = "7307hi_7308lo"
    else:
        best_data, best_str, best_found = data2, str2, found2
        order = "B=MSB (P7308 poids fort) / A=LSB (P7307 poids faible)"
        suffix = "7308hi_7307lo"

    print(f"\n✅ BON ORDRE : {order}")
    print(f"   Score : {max(score1, score2)} mots-clés détectés")

    # Sauvegarde
    out_dir = os.path.dirname(path_a)
    out_path = os.path.join(out_dir, f"FMV_combined_{suffix}.bin")
    with open(out_path, 'wb') as f:
        f.write(best_data)
    print(f"\n💾 Fichier entrelacé sauvegardé :")
    print(f"   {out_path}")
    print(f"   Taille : {len(best_data)} octets ({len(best_data)//1024} Ko)")
    print(f"   Checksum 16-bit : 0x{sum(best_data) & 0xFFFF:04X}")

    # Sauvegarde des chaînes
    str_path = os.path.join(out_dir, f"FMV_strings_{suffix}.txt")
    with open(str_path, 'wb') as f:
        for s in best_str:
            f.write(s + b'\n')
    print(f"\n📝 Chaînes extraites sauvegardées :")
    print(f"   {str_path}")
    print(f"   Total : {len(best_str)} chaînes")

    # Aperçu des chaînes les plus intéressantes
    print("\n" + "=" * 70)
    print("  APERÇU DES CHAÎNES LISIBLES (les plus longues)")
    print("=" * 70)
    interesting = sorted(set(best_str), key=len, reverse=True)[:40]
    for s in interesting:
        try:
            txt = s.decode('ascii')
            print(f"   {txt}")
        except:
            pass

    print("\n✅ Terminé ! Envoie-moi le contenu de :")
    print(f"   {str_path}")
    print("=" * 70)


if __name__ == '__main__':
    main()
