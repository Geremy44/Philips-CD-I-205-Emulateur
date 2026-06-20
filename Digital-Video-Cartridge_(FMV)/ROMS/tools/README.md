# 🛠️ Outils d'Analyse FMV

Scripts Python pour analyser et valider les ROMs de la carte FMV Philips CD-i 205.

---

## 📂 Contenu

```
tools/
├── check_FMV_checksum.py       # Vérification des checksums EPROM
└── check_FMV_entrelacement.py  # Détection de l'ordre d'entrelacement
```

---

## 🚀 Utilisation Rapide

```bash
# Détecter le bon ordre d'entrelacement
python check_FMV_entrelacement.py <ROM_P7307> <ROM_P7308>

# Vérifier l'intégrité (checksums)
python check_FMV_checksum.py <ROM_P7307> <ROM_P7308>
```

---

## 🔑 Outil Principal : `check_FMV_entrelacement.py`

### Résumé
Entrelace les deux EPROMs dans les deux ordres possibles, extrait les chaînes ASCII lisibles et détermine automatiquement le bon ordre via un score de mots-clés.

### Usage
```bash
python check_FMV_entrelacement.py <ROM_A> <ROM_B>
```

| Paramètre | Description |
|-----------|-------------|
| `ROM_A` | Chemin vers la ROM P7307 (checksum `0xFFD9`) |
| `ROM_B` | Chemin vers la ROM P7308 (checksum `0x4BA9`) |

### Fonctionnement

1. **Lecture** des deux fichiers ROM (64 Ko chacun)
2. **Calcul** des checksums 16-bit pour identification
3. **Test** des deux ordres d'entrelacement possibles :
   - Ordre 1 : `A(P7307)=MSB / B(P7308)=LSB`
   - Ordre 2 : `B(P7308)=MSB / A(P7307)=LSB`
4. **Extraction** des chaînes ASCII de chaque version
5. **Scoring** selon les mots-clés FMV/MPEG/Audio/Video
6. **Sélection** automatique du meilleur ordre
7. **Sauvegarde** du fichier entrelacé correct

### Sortie
```
═══════════════════════════════════════════════════════════════
  ENTRELACEMENT FMV - DÉTECTION DU BON ORDRE D'OCTETS
═══════════════════════════════════════════════════════════════

🔐 Checksum A : 0xFFD9
🔐 Checksum B : 0x4BA9

── Ordre 1 : A(MSB), B(LSB) ──
   Chaînes trouvées : X
   Score mots-clés  : 0

── Ordre 2 : B(MSB), A(LSB) ──
   Chaînes trouvées : Y
   Score mots-clés  : 89
   Mots-clés : MPEG×2, FMV×42, ERROR×15, ...

═══════════════════════════════════════════════════════════════
  RÉSULTAT
═══════════════════════════════════════════════════════════════

✅ BON ORDRE : B(P7308)=MSB / A(P7307)=LSB

💾 Fichier entrelacé sauvegardé :
   FMV_combined_7308hi_7307lo.bin
   131072 octets ✅
```

### Fichiers générés
| Fichier | Contenu |
|---------|---------|
| `FMV_combined_*.bin` | ROM entrelacée complète (128 Ko) |
| `FMV_combined_strings.txt` | Liste des chaînes ASCII détectées |

---

## 📋 `check_FMV_checksum.py`

### Résumé
Calcule les checksums des deux EPROMs et les compare aux valeurs de référence (`0xFFD9` et `0x4BA9`).

### Usage
```bash
python check_FMV_checksum.py <ROM_A> <ROM_B>
```

### Contrôles
- ✅ Checksum P7307 = `0xFFD9`
- ✅ Checksum P7308 = `0x4BA9`

### Sortie
```
P7307 checksum : 0xFFD9 ✅
P7308 checksum : 0x4BA9 ✅
```

---

## 📌 Notes

- Les scripts sont compatibles **Python 3.8+**
- Chaque EPROM individuelle fait **64 Ko** (65 536 octets)
- La ROM entrelacée complète fait **128 Ko** (131 072 octets)
