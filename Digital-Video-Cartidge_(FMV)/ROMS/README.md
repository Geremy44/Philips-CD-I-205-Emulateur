# 💾 ROMs de la Carte FMV

Contient les dumps binaires des deux EPROMs et leur version reconstruite entrelacée.

---

## 📂 Contenu

```
ROMS/
├── FMV FFD9 P7307 R4.1 VMPEG.BIN    # EPROM P7307 (LSB) — 64 Ko
├── FMV 4BA9 P7308 R4.1 VMPEG.BIN    # EPROM P7308 (MSB) — 64 Ko
├── FMV_combined_7308hi_7307lo.bin  # ROM entrelacée reconstruite (128 Ko)
└── tools/                          # Scripts d'analyse
```

---

## 🔐 Checksums et Identification

| Fichier | Checksum | Rôle |
|---------|----------|------|
| `FMV FFD9 P7307 R4.1 VMPEG.BIN` | `0xFFD9` | EPROM basse (LSB) |
| `FMV 4BA9 P7308 R4.1 VMPEG.BIN` | `0x4BA9` | EPROM haute (MSB) |
| `FMV_combined_7308hi_7307lo.bin` | — | ROM entrelacée complète |

---

## 🔀 Format Entrelacé — Comment Ça Marche

### Schéma d'entrelacement

```
Adresse 16-bit    Ordre validé (P7308=MSB / P7307=LSB) :

 Octet bas           Octet haut
 Adresse 0           P7307[0]          P7308[0]
 Adresse 1           P7307[1]          P7308[1]
 Adresse 2           P7307[2]          P7308[2]
    ...                  ...               ...
```

### Reconstruction en Python

```python
with open('FMV 4BA9 P7308 R4.1 VMPEG.BIN', 'rb') as f:
    P7308 = f.read()  # MSB (poids fort)

with open('FMV FFD9 P7307 R4.1 VMPEG.BIN', 'rb') as f:
    P7307 = f.read()  # LSB (poids faible)

# Ordre validé : P7308 = MSB, P7307 = LSB
combined = bytearray(len(P7307) * 2)
for i in range(len(P7307)):
    combined[i * 2]     = P7308[i]  # MSB (poids fort)
    combined[i * 2 + 1] = P7307[i]  # LSB (poids faible)
```

---

## ✅ Validation de l'Ordre

L'ordre d'entrelacement a été validé par :

| Méthode | Détail |
|---------|--------|
| Détection de chaînes lisibles | Score de 89 mots-clés FMV/MPEG/Audio/Video |
| Mot `dummy` présent | À l'offset attendu dans la ROM combinée |

---

## 📊 Tailles des Fichiers

| Fichier | Taille |
|---------|--------|
| `FMV FFD9 P7307 R4.1 VMPEG.BIN` | 65 536 octets (64 Ko) |
| `FMV 4BA9 P7308 R4.1 VMPEG.BIN` | 65 536 octets (64 Ko) |
| `FMV_combined_7308hi_7307lo.bin` | 131 072 octets (128 Ko) |

---

## 🔍 Vérification Rapide

```bash
# Vérifier les checksums des EPROMs
cd tools
python check_FMV_checksum.py ../ROM1 ../ROM2

# Vérifier la taille du fichier combiné
```

**Sortie attendue :**
```
P7307 checksum : 0xFFD9 ✅
P7308 checksum : 0x4BA9 ✅
ROM combinée   : 131072 octets ✅
```

---

## 📜 Chaînes Identifiées dans le Firmware

Le firmware contient les modules et chaînes suivants :

| Catégorie | Chaînes trouvées |
|-----------|-----------------|
| **Formats** | `MPEG-1`, `CD-i`, `FMV`, `mpeg` |
| **Audio/Video** | `AUDIO`, `VIDEO`, `mpeg`, `MPEG` |
| **Erreurs** | `ERROR`, `error` |

---

## 📌 Notes

- Les EPROMs sont des **ST M27C512** (64 Ko, 28 pins)
- La ROM combinée est de **128 Ko** (131 072 octets)
- Chaque EPROM individuelle fait **64 Ko** (65 536 octets)
