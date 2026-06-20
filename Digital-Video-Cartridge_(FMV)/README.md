# 🎬 Carte FMV (Full-Motion Video) — CD-i 205

Projet d'émulation de la carte d'extension Full-Motion Video pour le lecteur CD-i Philips 205.

---

## 📋 Contenu

```
Carte FMV/
├── ROMS/                          # ROMs originales et reconstruites
│   ├── README.md
│   └── tools/                     # Scripts d'analyse
│       ├── check_FMV_checksum.py       # Vérification des checksums
│       └── check_FMV_entrelacement.py  # Détection de l'ordre d'entrelacement
└── (fichiers additionnels)
```

---

## 🏗️ Architecture Matérielle (vue d'ensemble)

La carte FMV repose sur deux EPROMs ST M27C512 entrelacées :

| Composant | Description |
|-----------|-------------|
| **P7307** | EPROM basse (LSB) — 64 Ko — checksum `0xFFD9` |
| **P7308** | EPROM haute (MSB) — 64 Ko — checksum `0x4BA9` |
| **Combinée** | ROM entrelacée — 128 Ko (131 072 octets) |

---

## 🔌 Connectique et Placement

```
CD-i 205 (vue arrière)
│                                 │
│  │      CARTE FMV          │   │
│  │  │  P7307   │ P7308     │   │
│  │  │  (28 pins) (28 pins) │   │
│                                 │
│  [ Slot d'extension FMV ]       │
```

Les deux EPROM sont des **ST M27C512** (64 Ko chacune, 28 pins).

---

## 🚀 Démarrage Rapide

### 1. Vérifier l'ordre d'entrelacement

```bash
cd ROMS/tools
python check_FMV_entrelacement.py \
  "../FMV FFD9 P7307 R4.1 VMPEG.BIN" \
  "../FMV 4BA9 P7308 R4.1 VMPEG.BIN"
```

**Sortie attendue :**
```
✅ BON ORDRE : P7308=MSB / P7307=LSB
   Score : 89 mots-clés détectés
💾 Fichier entrelacé : FMV_combined_7308hi_7307lo.bin
```

### 2. Vérifier l'intégrité des ROMs

```bash
python check_FMV_checksum.py \
  "../FMV FFD9 P7307 R4.1 VMPEG.BIN" \
  "../FMV 4BA9 P7308 R4.1 VMPEG.BIN"
```

**Sortie attendue :**
```
P7307 checksum : 0xFFD9 ✅
P7308 checksum : 0x4BA9 ✅
```

---

## 🔑 Résultats Clés de l'Analyse

- Les deux EPROMs sont des **ST M27C512** (64 Ko, 28 pins)
- La ROM combinée fait **128 Ko** (131 072 octets)
- Ordre d'entrelacement validé : **P7308 = MSB / P7307 = LSB**
- Checksums vérifiés et concordants

---

## 👤 Projet

Émulation matérielle / logicielle de la carte FMV Philips CD-i 205.

_(merci Claude)_
