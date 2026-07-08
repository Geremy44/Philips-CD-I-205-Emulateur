# Philips CD-i 205 — Émulateur

> ⚠️ **Avertissement légal** — Ce projet ne fournit aucune ROM, BIOS ni logiciel sous copyright. Philips conserve tous les droits sur ses œuvres. Les fichiers `.bin`, `.rom` et `.iso` **ne doivent jamais** être committés (voir `.gitignore`). Ce dépôt ne contient que des outils d'analyse et de documentation de reverse-engineering à des fins éducatives.

---

## 🎯 Objectif

Émulateur fonctionnel du **Philips CD-i 205**, ordinateur de salon basé sur un Motorola 68070, exécutant l'OS **CD-RTOS** (dérivé d'OS-9 68k), avec support de la **Digital Video Cartridge** (cartouche FMV) pour la lecture de vidéo MPEG-1.

**Plateforme cible** : Linux / macOS / Windows (Python + libSDL2)

---

## 📂 Arborescence réelle du dépôt

```
Philips-CD-I-205-Emulateur/
├── README.md                        ← ce fichier
├── .gitignore
│
├── docs/                            ← documentation de recherche
│   ├── console/                    ← AXE A : console CD-i 205
│   │   ├── CONSOLE_OVERVIEW.md     ← vue d'ensemble matériel
│   │   └── CONSOLE_MMIO_MAP.md     ← carte MMIO console
│   └── fmv/                       ← AXE B : cartouche FMV
│       ├── FMV_OS9_FINDINGS.md     ← reverse-engineering modules OS-9
│       └── FMV_MMIO_MAP.md         ← carte MMIO cartouche FMV
│
├── tools/                          ← scripts d'analyse
│   └── README.md
│
└── src/                           ← code source de l'émulateur (WIP)
    └── ...                        ← à venir
```

---

## 🔬 Deux axes de recherche

Ce projet sépare rigoureusement l'analyse en **deux axes indépendants** :

### Axe A — CONSOLE (CD-i 205)
> Le matériel de base, commun à tous les modèles CD-i.

- **CPU** : Motorola 68070 @ 15 MHz (68000 + périphériques intégrés)
- **Vidéo** : contrôleur VSD + MCD212 (Dual Plane Display Controller)
- **Audio** : ADPCM PCM/FSCM, DAC stéréo
- **Système** : CD-RTOS (OS-9/68000), NVRAM, lecteur CD + contrôleur CDIC
- **Fichiers** : `docs/console/`

### Axe B — CARTOUCHE FMV (Digital Video Cartridge)
> L'accessoire enfichable optionnel, décodant le MPEG-1.

- **Modules OS-9** : `fmvdrv`, `fmvll`, `csd_fmvvm`…
- **Décodeur** : chip MPEG-1 (SGS-Thomson ?), MMIO dédié `$E80000–$EFFFFF`
- **Handshake** : IRQ dédiée, registre de contrôle `$C0`
- **Fichiers** : `docs/fmv/`

> ⚠️ **Règle de délimitation** : les地址 `$E80000–$EFFFFF` sont **réservées à la FMV**. La console ne doit jamais y accéder.

---

## 📊 Tableau d'avancement

### Axe Console — recherche à démarrer
| Tâche | État |
|-------|------|
| Extraction ROM CD-RTOS | 🔲 À démarrer |
| Cartographie mémoire complète | 🔲 À démarrer |
| Reverse-engineering MCD212 | 🔲 À démarrer |
| Analyse CDIC | 🔲 À démarrer |
| Implémentation timers 68070 | 🔲 À démarrer |
| Implémentation UART | 🔲 À démarrer |
| Émulation NVRAM | 🔲 À démarrer |
| Émulation DMA audio | 🔲 À démarrer |

### Axe FMV — recherche avancée
| Tâche | État |
|-------|------|
| Dump & analyse désentrelacé | ✅ Terminé |
| Extraction des 15 modules OS-9 | ✅ Terminé |
| Analyse fmvll : table d'init @ $1EE0 | ✅ Terminé |
| Analyse fmvdrv : dispatcher + codes | ✅ Terminé |
| Cartographie MMIO $E80000–$EFFFFF | ✅ Terminé |
| Routine IRQ @ $022E | ✅ Terminé |
| Chaîne d'appel complète | ✅ Terminé |
| Décodage des 24 codes SetStat | 🔲 À confirmer |
| Identification chip MPEG | 🔲 À démarrer |
| Reverse-engineering csd_fmvvm | 🔲 À démarrer |
| Émulation buffer vidéo MPEG | 🔲 À démarrer |
| Synchronisation audio/vidéo | 🔲 À démarrer |

---

## 🚀 Pour commencer

1. **Cloner le dépôt**
   ```bash
   git clone https://github.com/Geremy44/Philips-CD-I-205-Emulateur.git
   cd Philips-CD-I-205-Emulateur
   ```

2. **Explorer la documentation**
   - Console : `docs/console/CONSOLE_OVERVIEW.md`
   - FMV : `docs/fmv/FMV_OS9_FINDINGS.md`

3. **Utiliser les outils d'analyse**
   ```bash
   cd tools && python os9_module_parser.py --help
   ```

---

## 📄 Licence

Documentation de reverse-engineering à fins éducatives uniquement. Voir avertissement légal en tête de fichier.

---

*Dernier mise à jour : 21 juin 2026*
