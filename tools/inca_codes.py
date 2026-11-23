#!/usr/bin/env python3
"""
inca_codes.py - codes de reprise d'Inca (CD-i, Coktel Vision / Philips 1993)

Algorithme relu dans le jeu lui-meme (module cdi_menu.mdo, routine $16D2,
verification du pave numerique "Resume / Laden") :

  - le code a 6 chiffres de 1 a 9 ; 9 vaut 5 ; chaque chiffre - 1 donne 3 bits
  - chaque position est XORee avec une cle (3 bits) : 0 3 1 5 6 4
  - s4 = s0 ^ s2 ^ 3 et s5 = s1 ^ s3 ^ 7 (deux controles)
  - s3 est un "sel" libre (0..7) : 8 codes differents pour le meme etat
  - s0..s2 ^ s3 donnent : B = 8*s0 + s1 (point de reprise, 0..39) et A = s2
  - G = (A ^ B) & 7 doit etre <= 5 : probablement le nombre de vies
    (les 5 perles du chapeau) ; tous les codes connus ont G = 5
  - B doit designer une entree valide de la table du jeu (11 entrees, une
    par niveau 2..12)

Le code ne contient rien d'autre : l'inventaire n'est pas libre, il depend
du point de reprise (niveau).

Usage :
  python inca_codes.py                   liste un code par niveau (5 vies)
  python inca_codes.py --vies 3          idem avec 3 vies
  python inca_codes.py --tous 7          les 8 variantes du niveau 7
  python inca_codes.py --decode 681224   decode un code
"""

import argparse

KEY = [0, 3, 1, 5, 6, 4]

# point de reprise (B) -> niveau, table du jeu ($503F32 en memoire)
CHECKPOINT_LEVEL = {6: 2, 29: 3, 8: 4, 10: 5, 12: 6, 15: 7, 17: 8, 19: 9, 32: 10, 22: 11, 23: 12}
LEVEL_CHECKPOINT = {lv: b for b, lv in CHECKPOINT_LEVEL.items()}

LEVEL_NAMES = {1: "Titicaca", 2: "Antara", 3: "Paracas", 4: "Atahualpa", 5: "Katchampa",
               6: "Pizarro", 7: "Space gems", 8: "Cuzco", 9: "Paititi", 10: "War 1",
               11: "Take off", 12: "War 2", 13: "Aguirre"}


def encode(level, lives=5, salt=0):
    """Code a 6 chiffres pour un niveau (2..12), des vies (0..5), un sel (0..7)."""
    if level not in LEVEL_CHECKPOINT:
        raise ValueError("niveau %d sans code (codes : niveaux 2 a 12)" % level)
    if not 0 <= lives <= 5:
        raise ValueError("vies : 0 a 5")
    b = LEVEL_CHECKPOINT[level]
    t = [b >> 3, b & 7, (lives ^ b) & 7]
    s = [x ^ salt for x in t] + [salt]
    s.append((s[0] ^ 3 ^ s[2]) & 7)
    s.append((s[1] ^ 7 ^ s[3]) & 7)
    return "".join(str(((s[i] ^ KEY[i]) & 7) + 1) for i in range(6))


def decode(code):
    """(valide, niveau ou None, vies, sel, message) d'apres la routine du jeu."""
    if len(code) != 6 or any(c not in "123456789" for c in code):
        return False, None, None, None, "6 chiffres de 1 a 9"
    s = [((5 if c == "9" else int(c)) - 1 ^ KEY[i]) & 7 for i, c in enumerate(code)]
    if s[4] != (s[0] ^ 3 ^ s[2]) & 7 or s[5] != (s[1] ^ 7 ^ s[3]) & 7:
        return False, None, None, None, "controle faux"
    t = [(s[i] ^ s[3]) & 7 for i in range(3)]
    b = 8 * t[0] + t[1]
    lives = (t[2] ^ b) & 7
    if lives > 5:
        return False, None, lives, s[3], "vies > 5"
    if b not in CHECKPOINT_LEVEL:
        return False, None, lives, s[3], "point de reprise %d inconnu" % b
    return True, CHECKPOINT_LEVEL[b], lives, s[3], "ok"


def name(level):
    # niveau n du jeu = n-ieme nom de la liste (cdi_main.mdo) ; non verifie
    return LEVEL_NAMES.get(level, "?")


def main():
    p = argparse.ArgumentParser(description="Codes de reprise d'Inca (CD-i)")
    p.add_argument("--vies", type=int, default=5, help="nombre de vies 0..5 (defaut 5)")
    p.add_argument("--sel", type=int, default=0, help="variante 0..7 (defaut 0)")
    p.add_argument("--tous", type=int, metavar="NIVEAU", help="les 8 variantes d'un niveau")
    p.add_argument("--decode", metavar="CODE", help="decoder un code")
    a = p.parse_args()
    if a.decode:
        ok, lv, lives, salt, msg = decode(a.decode)
        if ok:
            print("%s : niveau %d (%s), %d vies, variante %d" % (a.decode, lv, name(lv), lives, salt))
        else:
            print("%s : refuse par le jeu (%s)" % (a.decode, msg))
        return
    if a.tous:
        for salt in range(8):
            print("niveau %d, %d vies, variante %d : %s" % (a.tous, a.vies, salt, encode(a.tous, a.vies, salt)))
        return
    for lv in sorted(LEVEL_CHECKPOINT):
        print("niveau %2d  %-11s %d vies : %s" % (lv, name(lv), a.vies, encode(lv, a.vies, a.sel)))


if __name__ == "__main__":
    main()
