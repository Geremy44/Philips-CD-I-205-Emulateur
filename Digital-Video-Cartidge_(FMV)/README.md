# 🎬 Carte FMV (Full-Motion Video) — CD-i 205

Dossier racine de la carte FMV Philips, contenant la documentation, les ROMs (NON) et les outils d'analyse.

---

## 📋 Contenu

```
Carte FMV/
├── README.md                               # Ce fichier
├── ARCHITECTURE.md                         # Spécifications matérielles détaillées
├── ROMS/                                   # Dumps d'EPROM de la carte
│   ├── README.md
│   ├── FMV_combined_7308hi_7307lo.bin      # ROM entrelacée complète (128 Ko)
│   ├── FMV_strings_7308hi_7307lo.txt       # Chaînes extraites (debug)
│   ├── FMV 4BA9 P7307 R4.1 VMPEG.BIN       # EPROM P7307 (origine)
│   └── FMV FFD9 P7308 R4.1 VMPEG.BIN       # EPROM P7308 (origine)
├── tools/                                  # Outils d'analyse et vérification
│   ├── README.md
│   ├── check_FMV_entrelacement.py          # Script principal de détection d'ordre
│   ├── verify_FMV_roms.py                  # Vérification d'intégrité
│   ├── extract_FMV_strings.py              # Extraction de chaînes ASCII
│   └── analyze_FMV_modules.py              # Analyse des modules OS-9
└── docs/                                   # Documentation technique
    ├── FMV_memory_map.md                   # Carte mémoire FMV
    ├── FMV_registers.md                     # Registres FMA / FMV / VCD
    └── FMV_modules.md                      # Modules OS-9 du firmware
```

---

## 🏗️ Architecture Matérielle (vue d'ensemble)

La carte FMV est une extension plug-in pour le lecteur CD-i 205. Elle décompresse la vidéo MPEG-1 en temps réel sur un flux CD-i.

| Composant | Rôle | Détail |
|-----------|------|--------|
| **Microprocesseur** | Décodeur MPEG | 68HC005 (?) avec ROM 内置 |
| **VCD (Video CD)** | Décompression MPEG-1 | Noyau temps réel |
| **FMA (FMV Audio)** | Gestion audio MPEG | Décodeur audio |
| **DRAM principale** | Tampon image | 512 Ko (0xE80000–0xEFFFFF, word boundary) |
| **DRAM extension** | Tampon optionnel | +1 Mb (optionnel) |
| **ROM FMV** | Firmware | 128 Ko entrelacés (P7307 + P7308) |
| **Registers** | Registres I/O | Espace mémoire mapé |

---

## 🔌 Connectique et Placement

```
CD-i 205 (vue arrière)
┌─────────────────────────────────┐
│                                 │
│  ┌─────────────────────────┐   │
│  │      CARTE FMV          │   │
│  │  ┌──────────┐           │   │
│  │  │  P7307   │ P7308     │   │
│  │  │  (28 pins) (28 pins) │   │
│  │  └──────────┘           │   │
│  └─────────────────────────┘   │
│                                 │
│  [ Slot d'extension FMV ]       │
└─────────────────────────────────┘
```

Les deux UVEPROM sont des ST M27C512

---

## 🚀 Démarrage Rapide

### 1. Vérifier l'ordre d'entrelacement

```bash
cd tools
python check_FMV_entrelacement.py ^
  "../ROMS/FMV 4BA9 P7307 R4.1 VMPEG.BIN" ^
  "../ROMS/FMV FFD9 P7308 R4.1 VMPEG.BIN"
```

**Sortie attendue :**
```
✅ BON ORDRE : P7308=MSB / P7307=LSB
   Score : 89 mots-clés détectés
💾 Fichier entrelacé : FMV_combined_7308hi_7307lo.bin
```

### 2. Vérifier l'intégrité des ROMs

```bash
python verify_FMV_roms.py
```

### 3. Extraire les chaînes

```bash
python extract_FMV_strings.py ../ROMS/FMV_combined_7308hi_7307lo.bin
```

---

## 📖 Documentation Détaillée

| Document | Contenu |
|----------|---------|
| `ARCHITECTURE.md` | Spécifications complètes, schémas,нципы работы |
| `ROMS/README.md` | Guide des ROMs, checksum, format entrelacé |
| `tools/README.md` | Catalogue des outils et modes d'emploi |
| `docs/FMV_memory_map.md` | Cartographie mémoire complète |
| `docs/FMV_registers.md` | Détails de tous les registres |
| `docs/FMV_modules.md` | Analyse des modules OS-9 |

---

## 🔑 Résultats Clés de l'Analyse

- **Ordre d'entrelacement** : P7308 = MSB (poids fort) / P7307 = LSB (poids faible)
- **Firmware identifiable** : Module **FMV Low Level Test Rel 5.0** détecté
- **Librairies présentent** : `MPEG`, `CDI_Lib`, `DMA_Lib`, `Error_Lib`
- **Mots-clés trouvés** : 89 occurrences (MPEG, FMV, Error, Audio, Video…)
- **Vérification dummy** : ✅ Buffer dummy de 512 Ko à 0xE80000–0xEFFFFF

---

## 👤 Projet

Analyse reverse-engineering de la carte FMV Philips CD-i 205.
Membres identifiés : FMV DRAM, VCD (Video CD), FMA (FMV Audio), FMA Registers.
Statut : **Phase d'analyse — ROMs décodées ✅ | Registres en cours 📋**

> ⚠️ Les fichiers ROMs sont des dumps à usage de recherche /backup. Respectez le copyright Philips. (je ne les ai pas publiés)