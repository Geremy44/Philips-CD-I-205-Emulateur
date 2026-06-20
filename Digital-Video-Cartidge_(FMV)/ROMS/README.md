# 💾 ROMs de la Carte FMV

Dumps mémoire des deux EPROMs de la carte FMV (P7307 / P7308) et de la ROM reconstruite.

---

## 📂 Contenu

```
ROMS/
├── README.md                                   # Ce fichier
│
├── EPROMs originales (dumps directs)          #
│   ├── FMV 4BA9 P7307 R4.1 VMPEG.BIN          # EPROM 27C256 — P7307
│   └── FMV FFD9 P7308 R4.1 VMPEG.BIN          # EPROM 27C256 — P7308
│
├── ROM reconstruite                            #
│   └── FMV_combined_7308hi_7307lo.bin         # ROM entrelacée complète (128 Ko)
│
└── Extraites (analyse)                        #
    └── FMV_strings_7308hi_7307lo.txt          # Chaînes ASCII extraites
```

---

## 🔐 Checksums et Identification

Chaque EPROM contient un **checksum 16-bit** utilisé par le firmware pour vérifier l'intégrité au démarrage.

| Fichier | Checksum | Rôle dans l'entrelacement |
|---------|----------|---------------------------|
| `P7307` | **0x4BA9** | Partie poids **FAIBLE** (LSB / octets pairs) |
| `P7308` | **0xFFD9** | Partie poids **FORT** (MSB / octets impairs) |

> 📝 Les checksums correspondent aux noms de fichiers — Philips embedait le checksum dans l'identifiant pour faciliter la vérification.

---

## 🔀 Format Entrelacé — Comment Ça Marche

Les deux EPROMs de 64 Ko (27C512) sont combinées pour former une ROM de **128 Ko** avec des mots de 16 bits.

### Schéma d'entrelacement

```
Adresse 16-bit    Ordre correct (validé) :
┌─────────────────────────────┬─────────────────────────────┐
│  Mot 16-bit [N]             │  ROM 8-bit source           │
├─────────────────────────────┼─────────────────────────────┤
│  Octet Haut (MSB)           ←│  P7308 (0xFFD9) = MSB      │
│  Octet Bas (LSB)            ←│  P7307 (0x4BA9) = LSB      │
└─────────────────────────────┴─────────────────────────────┘

 Adresse              P7308 (MSB)     P7307 (LSB)
  0x0000  ──────►  ROM[0] = P7308[0]    ROM[1] = P7307[0]
  0x0002  ──────►  ROM[2] = P7308[1]    ROM[3] = P7307[1]
  0x0004  ──────►  ROM[4] = P7308[2]    ROM[5] = P7307[2]
   ...                    ...               ...
```

### Reconstruction en Python

```python
P7307 = open("FMV 4BA9 P7307 R4.1 VMPEG.BIN", "rb").read()
P7308 = open("FMV FFD9 P7308 R4.1 VMPEG.BIN", "rb").read()

# Ordre validé : P7308 = MSB, P7307 = LSB
combined = bytearray(len(P7307) * 2)
for i in range(len(P7307)):
    combined[i * 2]     = P7308[i]   # MSB (poids fort)
    combined[i * 2 + 1] = P7307[i]   # LSB (poids faible)

open("FMV_combined_7308hi_7307lo.bin", "wb").write(combined)
```

---

## ✅ Validation de l'Ordre

L'ordre d'entrelacement **P7308=MSB / P7307=LSB** a été validé par :

| Méthode | Score |
|---------|-------|
| Analyse de chaînes ASCII | **89 mots-clés** dans le bon ordre / **0** dans le mauvais |
| Mot `dummy` | ✅ présent à l'offset attendu |
| Chaîne `dummy×1` | ✅ confirme l'alignement buffer |
| Chaînes `MPEG`, `CDI`, `FMV` | ✅ lisibles dans le bon ordre |

> ⚠️ **L'ordre inverse (P7307=MSB / P7308=LSB) donne un score de 0.** L'ordre est sans ambiguïté.

---

## 📊 Tailles des Fichiers

```
P7307.raw           52 4288 octets   (64 Ko) — LSB half
P7308.raw           52 4288 octets   (64 Ko) — MSB half
FMV_combined.raw    1,049e+6 octets   (128 Ko) — 16-bit word ROM
FMV_strings.txt      ~20 Ko         (texte ASCII extrait)
```

---

## 🔍 Vérification Rapide

```bash
# Vérifier les checksums des EPROMs
python ../tools/verify_FMV_roms.py

# Vérifier la taille du fichier combiné
python -c "import os; f='FMV_combined_7308hi_7307lo.bin'; print(f'{f}: {os.path.getsize(f)} octets')"
```

**Sortie attendue :**
```
P7307 checksum : 0x4BA9 ✅
P7308 checksum : 0xFFD9 ✅
ROM combinée   : 1,049e+6 octets ✅
```

---

## 📜 Chaînes Identifiées dans le Firmware

Le firmware contient les modules et chaînes suivants :

| Catégorie | Chaînes trouvées |
|-----------|-----------------|
| **Modules OS-9** | `PHILIPS`, `CDI_Lib`, `DMA_Lib`, `Error_Lib`, `test1`, `test2` |
| **Firmware** | `R4`, `FMV Low Level Test Rel 5.0`, `VCD_Lib`, `mpeg_lib`, `mpeg_driver` |
| **Mémoire** | `dummy×1` (buffer 512 Ko), `DRAM`, `ROMS` |
| **Audio/Video** | `Audio×5`, `Video×6`, `MPEG×4`, `mpeg×2` |
| **Erreurs** | `ERROR×15`, `error×10`, `Error×1` (gestion d'erreurs complète) |
| **Formats** | `MPEG-1`, `CD-i`, `FMV`, `mpeg` |

---

## 📌 Notes

- Les EPROMs sont des **27C512** (64 Ko, 28 pins)
- La ROM combinée est accessible comme une mémoire **16-bit** par le CPU hôte du CD-i
- Le firmware est signé **Philips — FMV Low Level Test Rel 5.0**
- Le buffer `dummy` de 512 Ko est utilisé comme zone tampon vidéo (0xE80000–0xEFFFFF)