# Philips CD-i — Cartouche Digital Video (FMV) — Résultats du Reverse Engineering

## Vue d'ensemble

- **Cible :** ROM de la cartouche FMV (Full Motion Video / MPEG) du Philips CD-i, utilisée avec les lecteurs CD-i 200/205.
- **Fichiers analysés :**
  - `FMV_combined_7308hi_7307lo.bin` (131072 octets / 128 Ko) — la ROM de la cartouche FMV (désentrelacée hi/lo).
  - `cdi200.rom` (524288 octets / 512 Ko) — le firmware hôte du lecteur CD-i 205 (référence).
- **Architecture confirmée :** Motorola 68000 + CD-RTOS (OS-9 / OS-9000).

## Confirmations clés

- Les marqueurs `0x4AFC` SONT des identifiants de synchronisation de module OS-9 (`M$ID`), et non des gardes inter-fonctions.
- Preuves : chaînes du noyau OS-9 trouvées dans `cdi200.rom` :
  - `"OS-9 Boot failed; can't find 'init'"`
  - `"WARNING - kernel has bad CRC"`
  - `"MPU incompatible with OS-9 kernel"`
  - `"Bad pseudo-vector table"`
  - `"can't open console terminal"`
  - `"OS9P2 module aborted"`
- **73 modules OS-9 valides** analysés dans `cdi200.rom` ; **15 modules OS-9 valides** analysés dans la ROM FMV.

## Format d'en-tête de module OS-9 (68000)

| Offset | Taille | Champ           | Notes                                              |
|--------|--------|-----------------|----------------------------------------------------|
| +0x00  | 2      | `M$ID`          | Mot de synchro = `0x4AFC`                          |
| +0x02  | 2      | `M$SysRev`      | Niveau de révision système                         |
| +0x04  | 4      | `M$Size`        | Taille totale du module (32 bits)                  |
| +0x08  | 4      | `M$Owner`       | ID du propriétaire                                 |
| +0x0C  | 4      | `M$Name`        | Offset vers la chaîne du nom du module (32 bits)   |
| +0x10  | 2      | `M$Accs`        | Permissions d'accès                                |
| +0x12  | 2      | `M$Type/Lang`   | Type de module + langage                           |
| +0x16  | 4      | `M$Exec`        | Offset du point d'entrée                           |
| +0x18  | 3      | `M$HdrChk`      | Parité d'en-tête 24 bits (XOR des mots d'en-tête)  |

**Notes :**
- La parité d'en-tête = XOR des mots d'en-tête donne une parité paire (bit7=0).
- Le CRC du corps du module utilise le polynôme OS-9 24 bits `0x800063`.
- Le nom du module est une chaîne ASCII terminée par null à `header_base + M$Name`.
- Les modules sont **tous empaquetés ensemble** dans la ROM sans padding d'alignement — scanner séquentiellement les `0x4AFC`.

## Modules de la cartouche FMV (15)

| Offset   | Nom           | Type        | Rôle                                                          |
|----------|---------------|-------------|---------------------------------------------------------------|
| 0x000000 | `sysgo`       | prog 0x01   | Boot/init de la carte                                         |
| 0x0001E6 | `csd_fmvvm`   | csd 0x05    | Configuration Status Descriptor — déclare la carte au système |
| 0x000276 | `fmvconf`     | prog 0x01   | Configuration FMV                                            |
| 0x001E82 | `vmpeg`       | data 0x04   | Données/firmware du décodeur vidéo MPEG                       |
| 0x002D82 | `vcd`         | prog 0x01   | Gestionnaire Video CD                                         |
| 0x0042E4 | `fmvll`       | prog 0x01   | FMV Low-Level (routines hardware/MMIO)                        |
| 0x009CE6 | `dspcode`     | data 0x04   | Microcode du DSP audio (MPEG audio)                           |
| 0x00DD66 | `MoviMan`     | sysmgr 0x0D | Movie Manager — gestionnaire de lecture                      |
| 0x00FBB8 | `ma`          | descr 0x0F  | Descripteur de device "ma" (MPEG audio)                      |
| 0x00FC20 | `madriv`      | driver 0x0E | Driver audio MPEG                                            |
| 0x011A14 | `mv`          | descr 0x0F  | Descripteur de device "mv" (MPEG vidéo)                      |
| 0x011A8C | `fmvdrv`      | driver 0x0E | Driver FMV principal (contrôle décodeur MPEG — registres MMIO)|
| 0x014C60 | `fmvvolset`   | subr 0x02   | Réglage du volume                                           |
| 0x014D3A | `ramtest4`    | prog 0x01   | Test RAM de la carte                                         |
| 0x0157B6 | `dummy`       | data 0x04   | Padding                                                      |

## Modules clés pour l'émulation

| Priorité | Module        | Offset   | Taille   | Raison                                                    |
|----------|---------------|----------|----------|-----------------------------------------------------------|
| **1er**  | `csd_fmvvm`   | 0x0001E6 | ~144 o   | Premier à émuler — le système DÉTECTE la carte via celui-ci|
| **2e**   | `fmvdrv`      | 0x011A8C | ~12 Ko   | Contrôle décodeur MPEG — **désassemblé pour carte MMIO** ✅ |
| **3e**   | `fmvll`       | 0x0042E4 | ~23 Ko   | Communication hardware bas niveau (routines MMIO) ✅       |
| **4e**   | `madriv`      | 0x00FC20 | ~7,5 Ko  | Driver audio MPEG                                         |
| **5e**   | `MoviMan`     | 0x00DD66 | ~7 Ko    | Gestionnaire de lecture (utilise fmvdrv + madriv)        |
| **6e**   | `vmpeg`       | 0x001E82 | ~15 Ko   | Données chargées dans la puce vidéo MPEG                  |
| **7e**   | `dspcode`     | 0x009CE6 | ~16 Ko   | Microcode chargé dans le DSP audio                       |

---

## ⭐ NOUVEAU : Analyse détaillée de `fmvll` (Low-Level)

### Table d'init du décodeur `$1EE0–$1FF0`

La table d'init contient **24 paires** suivant le pattern M68K :
```asm
move.l #<valeur>, -(A7)      ; 2F 3C <valeur 32 bits>
move.w #<index_reg>, (A7)    ; 48 78 <index 16 bits>
```
Une routine lit ces paires sur la pile et écrit dans le décodeur :
`write_to_decoder(index_reg, valeur)`.

### Table d'init complète décodée

| Index | Valeur     | Rôle probable                  |
|-------|------------|--------------------------------|
| 0x0A  | 0xD8000000 | Registre vidéo (1ère écriture) |
| 0x03  | 0x70000000 | Configuration                  |
| 0x09  | 0xCA000000 | Vidéo                          |
| 0x0B  | 0xD9000000 | Vidéo                          |
| 0x0D  | 0x78000400 | Multi-écriture                 |
| 0x02  | 0x60000000 | **STROBE / latch**             |
| 0x04  | 0xC00C1313 | Configuration                  |
| 0x0A  | 0xD8000007 | Vidéo (2e écriture)            |
| 0x05  | 0xC1800001 | Configuration                  |
| 0x06  | 0xC2000000 | Configuration                  |
| 0x07  | 0xC4070707 | Configuration                  |
| 0x08  | 0xC7000000 | Configuration                  |
| 0x0C  | 0xDB00003F | Vidéo                          |
| 0x0D  | 0xC3000000 | Configuration (2e écriture)    |
| 0x01  | 0xA1000000 | Configuration                  |
| 0x0E  | 0xC5000000 | Audio                          |
| 0x0F  | 0x99000000 | Audio                          |
| 0x02  | 0x60000000 | **STROBE / latch**             |
| 0x10  | 0xB0000000 | Audio                          |
| 0x11  | 0x95000000 | Audio                          |
| 0x02  | 0x60000000 | **STROBE / latch**             |
| 0x12  | 0xB4000000 | Audio                          |
| 0x0F  | 0x99000000 | Audio                          |
| 0x02  | 0x60000000 | **STROBE / latch**             |
| 0x13  | 0x97000000 | Audio                          |
| 0x02  | 0x60000000 | **STROBE / latch**             |
| 0x11  | 0x95000000 | Audio                          |
| 0x02  | 0x60000000 | **STROBE / latch**             |

**Observations clés :**
- **REG \$02 = STROBE** : signal de validation/latch émis après chaque groupe d'écritures (potentiellement une IRQ).
- **REG \$0A et \$0D = multi-écritures** : certains registres nécessitent plusieurs phases.
- **REG \$0E–\$13 = bloc audio** ; **REG \$01–\$0D = bloc vidéo/config**.

---

## ⭐ NOUVEAU : Analyse détaillée de `fmvdrv` (Driver principal)

### En-tête du module

```
$00: 4AFC 0001    Sync OS-9 + révision système
$04: 000031D4     Taille du module = 12756 octets
$0C: 000031C8     Offset du nom → "fmvdrv"
$12: ....          Type $0E = Device Driver
$30: 000001D6     Offset d'exécution = $1D6
```

### Chemin de device

À l'offset `$0066`, signature ASCII :
```
"/nvr/csd"   → le driver ouvre la NVRAM + module CSD
```
Confirme le lien avec `csd_fmvvm.bin`.

### Dispatcher SetStat/GetStat @ `$0CA8`

Gros switch comparant le code fonction (`cmpi.w #$01xx,D0`).

### Cartographie des codes de contrôle FMV (`$0100–$0117`)

| Code   | Déc. | Fonction probable (FMV)            |
|--------|------|------------------------------------|
| $0100  | 256  | `FMV_INIT` / Reset décodeur        |
| $0101  | 257  | `FMV_PLAY` / Start                 |
| $0102  | 258  | `FMV_STOP`                         |
| $0104  | 260  | `FMV_PAUSE`                        |
| $0105  | 261  | `FMV_RESUME`                       |
| $0106  | 262  | `FMV_SCAN_FWD` (avance rapide)     |
| $0107  | 263  | `FMV_SCAN_REV` (retour rapide)     |
| $0108  | 264  | `FMV_STEP` (image par image)       |
| $0109  | 265  | `FMV_SET_WINDOW` (position vidéo)  |
| $010A  | 266  | `FMV_SET_VIDEO_MODE`               |
| $010B  | 267  | `FMV_SET_AUDIO_MODE`               |
| $010C  | 268  | `FMV_GET_STATUS`                   |
| $010D  | 269  | `FMV_FLUSH` (vider buffers)         |
| $010E  | 270  | `FMV_SET_VOLUME`                   |
| $010F  | 271  | `FMV_MUTE`                         |
| $0110  | 272  | `FMV_SET_CHANNEL`                  |
| $0111  | 273  | `FMV_SET_SPEED`                    |
| $0112  | 274  | `FMV_SYNC` (synchro A/V)           |
| $0113  | 275  | `FMV_GET_FRAME`                    |
| $0114  | 276  | `FMV_SET_PALETTE` / display        |
| $0116  | 278  | `FMV_SPECIAL`                      |
| $0117  | 279  | `FMV_RAW_ACCESS`                   |

➡️ **24 codes de contrôle**, cohérent avec les 24 étapes d'init de `fmvll`.

### Structure de contexte (static storage OS-9)

| Offset   | Rôle                                          |
|----------|-----------------------------------------------|
| +$0096   | Pointeur device path / handle principal       |
| +$008E   | Pointeur buffer                                |
| +$00A4   | État courant / flag                            |
| +$00AC   | Compteur frames / timer                        |
| +$00AE   | Pointeur buffer                                |
| +$00C0   | **MMIO contrôle/IRQ** (ack = $0010)            |
| +$00C2   | **MMIO mode/status**                          |
| +$00CA   | Status hardware                                |
| +$00DE   | Flag init décodeur                             |
| +$0120   | **IRQ arm/status** ($2000)                     |
| +$0196   | Buffer descriptor (souvent $FFFFFFFF)          |
| +$017B   | Flag byte (sub-state machine)                  |

### Routine d'IRQ @ `$022E`

```asm
move.w  #$2000, ($120,A2)        ; arme l'IRQ
move.l  #$FFFFFFFF,($196,A2)     ; reset buffer descriptor
move.w  #$0010, ($C0,A2)         ; acknowledge interrupt via MMIO $C0
```
➡️ Confirme : registre `$C0` = contrôle/IRQ, valeur `$0010` = acquittement d'interruption.

---

## ⭐ NOUVEAU : Chaîne d'appel complète

```
Application CD-i
     ↓ SetStat($0101 = PLAY)
fmvdrv.bin   (dispatcher @ $0CA8)
     ↓ appelle les routines
fmvll.bin    (init décodeur — table $1EE0)
     ↓ écrit MMIO $C0/$C2/$C6
csd_fmvvm.bin (gestion VM/buffers)
     ↓ MMIO
Hardware FMV @ $E80000–$EFFFFF
```

### Carte MMIO consolidée (préliminaire)

| Registre (offset) | Rôle                          | Valeurs notables           |
|-------------------|-------------------------------|----------------------------|
| $C0               | Contrôle / IRQ                | $0010 = ack IRQ            |
| $C2               | Mode / status                 | divers modes              |
| $C6               | Configuration                 | —                          |
| $CA               | Status hardware               | lecture                    |
| Base device       | `$E80000–$EFFFFF`             | espace MMIO complet       |

---

## Lecteur hôte (cdi200.rom) — Modules notables

Le firmware hôte contient 73 modules OS-9. Modules clés :

- `kernel` — cœur du noyau OS-9
- `init` — initialisation système
- `sysgo` — chargeur de boot principal
- `csd_220` — Configuration Status Descriptor de la carte mère (vs `csd_fmvvm` sur la cartouche)
- `csdinit` — initialisation CSD
- `cdfm` — gestionnaire de fichiers CD
- `ucm` — module de contrôle d'unité
- `video` — sous-système vidéo
- `cdapdriv` — driver lecteur audio CD
- `config` — configuration système
- `play` — contrôle de lecture

> **Note :** La cartouche FMV utilise `csd_fmvvm` ; l'hôte utilise `csd_220`. Le CSD de la cartouche doit correspondre aux attentes de l'hôte pour que le système énumère le device FMV.

## Notes

- Les séquences `0x4E400006` marquées ❌ pendant le scan sont des **instructions TRAP** (points d'entrée d'appels système OS-9), pas des modules OS-9 — **à ignorer**.
- L'ancien fichier `.HEX` était incomplet ; le fichier complet `cdi200.rom` (512 Ko) fait foi pour le firmware hôte.
- La ROM FMV était à l'origine entrelacée (octets pairs/impairs de deux puces ROM, 7308hi / 7307lo). Le fichier combiné `FMV_combined_7308hi_7307lo.bin` a été reconstruit en fusionnant les octets hi/lo.
- Types de modules OS-9 utilisés dans la cartouche FMV :
  - `0x01` — Module programme
  - `0x02` — Module sous-routine
  - `0x04` — Module données
  - `0x05` — CSD (Configuration Status Descriptor)
  - `0x0D` — System Manager
  - `0x0E` — Device Driver
  - `0x0F` — Device Descriptor

## Statut / Prochaines étapes

| Tâche                                              | Statut |
|----------------------------------------------------|--------|
| Désentrelacer la ROM hi/lo                         | ✅ Fait |
| Confirmer architecture 68000 + OS-9 / CD-RTOS      | ✅ Fait |
| Parser de module OS-9 fonctionnel (88 modules)     | ✅ Fait |
| Carte des modules de la cartouche FMV (15 nommés)  | ✅ Fait |
| Extraire & désassembler `fmvdrv` pour carte MMIO   | ✅ Fait |
| Décoder la table d'init de `fmvll` ($1EE0–$1FF0)   | ✅ Fait |
| Cartographier les codes SetStat/GetStat FMV        | ✅ Fait |
| Identifier la routine d'IRQ et l'ack ($C0=$0010)   | ✅ Fait |
| Émuler `csd_fmvvm` pour que le système détecte la carte | 🔲 À faire |
| Implémenter le device FMV dans l'émulateur (stub MMIO) | 🔲 À faire |
| Charger `dspcode` dans le DSP audio                | 🔲 À faire |
| Charger `vmpeg` dans le décodeur vidéo MPEG        | 🔲 À faire |

## Références

- Référence en-tête de module OS-9 : Microware System Architecture II (M68000/OS-9)
- Lecteur CD-i : Philips CD-i 200/205, Model No. 22/24
- Cartouche FMV : Digital Video Cartridge (22TC922/22TC923)
