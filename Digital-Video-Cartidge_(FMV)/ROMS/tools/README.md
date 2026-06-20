# 🛠️ Outils d'Analyse FMV

Ensemble de scripts Python pour analyser, vérifier et reconstruire les ROMs de la carte FMV.

---

## 📂 Contenu

```
tools/
├── README.md                                 # Ce fichier
├── check_FMV_entrelacement.py                # 🔑 Détection automatique de l'ordre et extraction des strings
├── check_FMV_checksum.py                        # Vérification d'intégrité
```

---

## 🚀 Utilisation Rapide

```bash
# Lancement principal (détection + entrelacement + sauvegarde)
python check_FMV_entrelacement.py <ROM_A> <ROM_B>

# Vérification d'intégrité
python check_FMV_checksum.py
```

---

## 🔑 Outil Principal : `check_FMV_entrelacement.py`

### Résumé
Script de détection automatique de l'ordre d'entrelacement des deux EPROMs de la carte FMV.

### Usage
```bash
python check_FMV_entrelacement.py "ROM_P7307.bin" "ROM_P7308.bin"
```

### Entrées
| Paramètre | Description |
|-----------|-------------|
| `ROM_A` | Chemin vers la ROM P7307 (checksum 0x4BA9) |
| `ROM_B` | Chemin vers la ROM P7308 (checksum 0xFFD9) |

### Fonctionnement

1. **Lecture** des deux fichiers ROM (32 Ko chacun)
2. **Calcul** des checksums 16-bit à l'offset 0x7FBE
3. **Test** des deux ordres d'entrelacement possibles :
   - Ordre 1 : `A(P7307)=MSB / B(P7308)=LSB`
   - Ordre 2 : `B(P7308)=MSB / A(P7307)=LSB`
4. **Extraction** des chaînes ASCII de chaque version
5. **Scoring** selon les mots-clés FMV/MPEG/Audio/Video
6. **Sélection** automatique du meilleur ordre
7. **Sauvegarde** du fichier entrelacé correct

### Sortie
```
======================================================================
  ENTRELACEMENT FMV - DÉTECTION DU BON ORDRE D'OCTETS
======================================================================
📄 ROM A : FMV 4BA9 P7307 R4.1 VMPEG.BIN (65536 octets)
📄 ROM B : FMV FFD9 P7308 R4.1 VMPEG.BIN (65536 octets)
🔐 Checksum A : 0x4BA9
🔐 Checksum B : 0xFFD9

── Ordre 1 : A(MSB), B(LSB) ──
   Score mots-clés : 0

── Ordre 2 : B(MSB), A(LSB) ──
   Score mots-clés : 89
   Mots-clés : MPEG×2, FMV×42, ERROR×15, ...

✅ BON ORDRE : B(P7308)=MSB / A(P7307)=LSB
💾 Fichier sauvegardé : FMV_combined_7308hi_7307lo.bin
```

### Fichiers générés
| Fichier | Contenu |
|---------|---------|
| `FMV_combined_*.bin` | ROM entrelacée complète (64 Ko) |
| `FMV_strings_*.txt` | Toutes les chaînes ASCII extraites |

---

## 📋 `check_FMV_checksum.py`

### Résumé
Vérifie les checksums des EPROMs.

### Usage
```bash
python check_FMV_checksum.py <ROM_A> <ROM_B>
```

### Contrôles
- ✅ Checksum P7307 = `0x4BA9`
- ✅ Checksum P7308 = `0xFFD9`

### Sortie
```
P7307 checksum : 0x4BA9 ✅
P7308 checksum : 0xFFD9 ✅
```

---

## 📌 Notes

- Tous les scripts supportent les chemins **Windows** (`\`) et **Unix** (`/`)
- Les scripts sont compatibles **Python 3.8+**
- Aucune modification des ROMs originales — seul le fichier combiné est créé

Pour plus de détails, voir la documentation dans `../docs/`.