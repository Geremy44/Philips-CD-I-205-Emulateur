# tools/checksum.py
import sys

def analyze_checksum(path):
    with open(path, 'rb') as f:
        data = f.read()
    
    print(f"\n📄 {path}")
    
    # 1. Somme simple de tous les octets (8/16/32 bits)
    total = sum(data)
    print(f"   Somme totale 8-bit  : 0x{total & 0xFF:02X}")
    print(f"   Somme totale 16-bit : 0x{total & 0xFFFF:04X}")
    print(f"   Somme totale 32-bit : 0x{total & 0xFFFFFFFF:08X}")
    
    # 2. Si checksum à la fin : la somme de TOUT doit donner 0x00 ou 0xFFFF
    print(f"   → Si somme==0, checksum en fin probable")
    
    # 3. Regarde les derniers 2 et 4 octets (checksum stocké ?)
    last2 = data[-2:]
    last4 = data[-4:]
    print(f"   Derniers 2 octets : {last2.hex().upper()} = 0x{int.from_bytes(last2,'big'):04X}")
    print(f"   Derniers 4 octets : {last4.hex().upper()}")
    
    # 4. Test : somme sauf les 2 derniers octets
    body_sum = sum(data[:-2]) & 0xFFFF
    stored = int.from_bytes(data[-2:], 'big')
    print(f"   Somme corps (sans 2 derniers) : 0x{body_sum:04X}")
    print(f"   Complément : 0x{(-body_sum) & 0xFFFF:04X}")
    print(f"   Stocké : 0x{stored:04X}")
    if (body_sum + stored) & 0xFFFF == 0:
        print(f"   ✅ CHECKSUM 16-bit VALIDÉ (somme+stocké=0) !")
    elif body_sum == stored:
        print(f"   ✅ CHECKSUM = somme directe !")

if __name__ == '__main__':
    for path in sys.argv[1:]:
        analyze_checksum(path)
