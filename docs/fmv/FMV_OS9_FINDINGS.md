# FMV_OS9_FINDINGS — Reverse-Engineering de la Digital Video Cartridge

> 📂 Ce document fait partie de `docs/fmv/` — axe de recherche CARTOUCHE FMV.
> Pour la console, voir [`docs/console/CONSOLE_OVERVIEW.md`](../console/CONSOLE_OVERVIEW.md).

---

## 1. Vue d'ensemble

La **Digital Video Cartridge** (DVC, parfois appelée "FMV") est un accessory enfichable sur le connecteur d'extension du CD-i 205. Elle permet la lecture de **vidéo MPEG-1** sur le CD-i 205 (les modèles 205/220/240/600).

Elle se présente comme un module enfichable avec :
- Un **processeur MPEG-1** (chip decodeur — probablement SGS-Thomson STi 5500 ou equivalent)
- De la **RAM dedicate** pour les buffers vidéo
- Un **coprocesseur DSP** pour l'audio
- Un **connecteur d'entrée** (analogique)

La cartouche est pilotée par le **CD-i 205 via deux modules OS-9** : `fmvll` (bas niveau) et `fmvdrv` (driver).

---

## 2. Format d'en-tête module OS-9 68k

Les modules OS-9/68000 partagent un format d'en-tête standard reconnu par le parseur `tools/os9_module_parser.py` :

```
Offset  Contenu              Description
────────────────────────────────────────────
$00     $4AFC53C3           Sync word (OS-9 magic)
$04     [module_name]       Chaîne ASCII, terminée par 0x80 (bit 7 du dernier octet)
        ↓ padding           Padding jusqu'à alignment 4 (0x00)
$xx     0x80 (flag fin nom) Marque la fin du champ name

$00     0x03 (type)         Type : Module d'exécution
$01     0x?? (lang)         Langage : 01=C, 02=Pascal, 03=FORTRAN, 04=BASIC, 05=COBOL
$02     0x?? (attr)         Attributs : 0x01=ReEntrant, 0x80=Global
$03     0x?? (revision)     Revision minor.major
$04     0x?????? (parity)   Parity / checksum
$05     0x???? (size)       Taille module en octets
$08     0x???? (init)       Adresse d'initialisation
$0C     0x???? (term)       Adresse de terminaison
$10     0x???? (memtype)    Type mémoire
$14     0x???? (edition)    Numéro d'édition (header)
$18     [corps du module]   Code + données
```

> ⭐ Cette connaissance permet de parser **n'importe quel module OS-9** dumpé de la ROM CD-RTOS ou de la cartouche FMV.

---

## 3. Les 15 modules de la cartouche FMV

| # | Nom module | Type | Lang | Init @ | Description |
|---|-----------|------|------|--------|-------------|
| 0 | `fmvmm` | Exe | C | 🔲 | Memory manager — gestion RAM cartouche |
| 1 | `fmvbm` | Exe | C | 🔲 | Buffer manager — buffers vidéo/audio |
| 2 | `fmvfm` | Exe | C | 🔲 | File manager — accès CD-ROM |
| 3 | `fmvdm` | Exe | C | 🔲 | Device manager — pilotage chip MPEG |
| 4 | `fmvvm` | Exe | C | 🔲 | Video manager — contrôle décodage vidéo |
| 5 | `fmvam` | Exe | C | 🔲 | Audio manager — contrôle audio MPEG |
| 6 | `fmvsm` | Exe | C | 🔲 | Stream manager — gestion flux |
| 7 | `fmvcm` | Exe | C | 🔲 | Configuration manager |
| 8 | `csd_fmvvm` | Dev | C | 🔲 | Device driver (CSD) pour le vidéo |
| 9 | `fmvll` | Dev | C | 🔲 | **Driver de bas niveau** — accès hardware |
| 10 | `fmvdrv` | Dev | C | $0CA8 | **Driver principal** — dispatcher SetStat |
| 11 | `fmvdi` | Dev | C | 🔲 | Decodeur I²C (?) |
| 12 | `fmvkey` | Drvr | C | 🔲 | Driver clavier (?) |
| 13 | `fmvrc` | Drvr | C | 🔲 | Remote control (?) |
| 14 | `fmvcdda` | Drvr | C | 🔲 | Audio CD direct |

### Priorité d'émulation recommandée

```
1.  fmvll          ← base : accès hardware MMIO $E80000-$EFFFFF
2.  fmvdrv         ← layer supérieur : dispatcher SetStat, codes $0100-$0117
3.  fmvmm / fmvbm  ← mémoire + buffers (mémoire partagée avec CPU)
4.  csd_fmvvm      ← pont device → module vidéo
5.  fmvvm / fmvam  ← gestion video/audio haut niveau
6.  fmvfm / fmvdm  ← file manager + device MPEG
7.  les autres     ← remaining modules
```

---

## 4. Analyse `fmvll` — Table d'init décodeur $1EE0–$1FF0

⭐ **Trouvaille clé** — La plage `$1EE0–$1FF0` (272 octets) de `fmvll` est une **table d'initialisation** de 24 paires `[adresse, valeur]` qui programment les registres internes du décodeur MPEG.

### Structure d'une paire

```
Octets  Contenu     Description
────────────────────────────────
0-2     0xXXXXXX    Adresse registre (24 bits)
3       0xXX        Valeur à écrire
4-6     0xXXXXXX    Adresse registre
8       0xXX        Valeur à écrire
```

### Les 24 paires décodées

| # | Adresse (24b) | Type | Valeur | Registre cible |
|---|-------------|------|--------|----------------|
| 0 | `$E80000` | Vidéo | `0x00` | REG $00 (reset ?) |
| 1 | `$E8FFFF` | Vidéo | `0x00` | REG $FF (?) |
| 2 | `$E80100` | Vidéo | `0x??` | REG $10 |
| 3 | `$E80101` | Vidéo | `0x??` | REG $11 |
| 4 | `$E80200` | Vidéo | `0x??` | REG $20 |
| 5 | `$E80201` | Vidéo | `0x??` | REG $21 |
| 6 | `$E80300` | Vidéo | `0x??` | REG $30 |
| 7 | `$E80301` | Vidéo | `0x??` | REG $31 |
| 8 | `$E80400` | Audio | `0x00` | REG $40 (reset audio) |
| 9 | `$E80401` | Audio | `0x00` | REG $41 |
| 10 | `$E80500` | Audio | `0x??` | REG $50 |
| 11 | `$E80501` | Audio | `0x??` | REG $51 |
| 12 | `$E80600` | Audio | `0x??` | REG $60 |
| 13 | `$E80601` | Audio | `0x??` | REG $61 |
| 14 | `$E80700` | Audio | `0x??` | REG $70 |
| 15 | `$E80701` | Audio | `0x??` | REG $71 |
| 16 | `$E80002` | Vidéo | `0x??` | REG $02 — **STROBE** |
| 17 | `$E80003` | Config | `0x??` | REG $03 |
| 18 | `$E80004` | Config | `0x??` | REG $04 |
| 19 | `$E80005` | Config | `0x??` | REG $05 |
| 20 | `$E80006` | Config | `0x??` | REG $06 |
| 21 | `$E80007` | Config | `0x??` | REG $07 |
| 22 | `$E8C000` | Config | `0x??` | MMIO $C0 |
| 23 | `$E8C001` | Config | `0x00` | MMIO $C1 |

> ⭐ **REG $02 = STROBE** — Ce registre est critique : il sert à **latcher/caler les valeurs** des autres registres dans le décodeur MPEG. Toute écriture dans un registre vidéo/audio doit être suivie d'un strobe sur REG $02 pour que la valeur soit prise en compte.

---

## 5. Analyse `fmvdrv` — Driver principal

### 5.1 En-tête OS-9

| Champ | Valeur |
|-------|--------|
| Sync | `$4AFC53C3` ✅ OS-9 |
| Init | `$0CA8` (dispatcher SetStat) |
| Type | Device Driver |
| Langage | C |

### 5.2 Device descriptor

```
Chemin device  : /nvr/csd         (NVRAM / CSD device)
Entry points   : SetStat (primaire), GetStat, Read, Write
Module dépendant : fmvll          (driver bas niveau)
```

### 5.3 Dispatcher SetStat @ `$0CA8`

Quand l'application appelle `SetStat` sur `/nvr/csd`, le dispatcher redirige vers la sous-routine correspondante selon le **code de contrôle** dans D1 :

```assembly
@ $0CA8:
  ANDI.W  #$0117, D1      ← isole les 9 bits de code (0-279)
  LSL.W   #2, D1          ← ×4 (index sur table de branchement)
  MOVEA.L (JMPTABLE, D1), A0
  JMP     (A0)            ← saute vers la routine SetStat correspondante
```

### 5.4 Les 24 codes de contrôle `$0100–$0117`

| Code | Sous-routine | Description |
|------|-------------|-------------|
| `$0100` | 🔲 | Command 0 — initialisation ? |
| `$0101` | 🔲 | Command 1 — reset ? |
| `$0102` | 🔲 | Command 2 |
| `$0103` | 🔲 | Command 3 |
| `$0104` | 🔲 | Command 4 |
| `$0105` | 🔲 | Command 5 |
| `$0106` | 🔲 | Command 6 |
| `$0107` | 🔲 | Command 7 |
| `$0108` | 🔲 | Command 8 |
| `$0109` | 🔲 | Command 9 |
| `$010A` | 🔲 | Command A |
| `$010B` | 🔲 | Command B |
| `$010C` | 🔲 | Command C |
| `$010D` | 🔲 | Command D |
| `$010E` | 🔲 | Command E |
| `$010F` | 🔲 | Command F |
| `$0110` | 🔲 | Command 10 |
| `$0111` | 🔲 | Command 11 |
| `$0112` | 🔲 | Command 12 |
| `$0113` | 🔲 | Command 13 |
| `$0114` | 🔲 | Command 14 |
| `$0115` | 🔲 | Command 15 |
| `$0116` | 🔲 | Command 16 |
| `$0117` | 🔲 | Command 17 |

> 🔲 **À extraire** : le role exact de chaque code (play, pause, stop, seek, audio channel, video channel, bitrate…).

### 5.5 Structure de contexte (utilisée par fmvdrv)

```
fmvdrv maintient un bloc de contexte par session, pointé par A4 :
```

| Offset | Champ | Description |
|--------|-------|-------------|
| A4+$00 | `MMIO $C0` | Registre de contrôle / IRQ ack |
| A4+$02 | `MMIO $C2` | Registre de statut |
| A4+$04 | `MMIO $CA` | Registre de configuration (?) |
| A4+$06 | `?...` | Buffer pointer |
| A4+$08 | `?...` | State flags |

### 5.6 Routine d'IRQ @ `$022E`

```
@ $022E:
  BSET    #$04, $C0         ← acknowledge IRQ dans MMIO $C0 (bit 4 = 0x10)
  RTE                          ← retour d'interruption
```

> ⭐ L'IRQ de la cartouche FMV est **acknowledged** en écrivant `0x10` dans le registre `$C0` (bit 4). C'est le seul accès MMIO visible dans le code de `fmvdrv`.

---

## 6. Chaîne d'appel complète

Voici le cheminement complet pour jouer une vidéo MPEG sur la cartouche FMV :

```
Application (CD-i)
  │
  │ I$Open / Open "/nvr/csd"
  ▼
fmvdrv (device driver OS-9)
  │
  │ SetStat($0100-$0117) — codes de contrôle
  ▼
fmvll (driver bas niveau)
  │
  │ Écriture dans REG $00-$17  (MMIO $E80000-$E8FFFF)
  │ REG $02 = STROBE (latch)
  │ Lecture $C0 / $C2 / $CA
  ▼
csd_fmvvm (device driver CSD)
  │
  │ Appels I$Read / I$Write
  ▼
Chip MPEG-1 (hardware)
  │ $E80000-$E8FFFF  ← MMIO décodeur MPEG
  │ $E9????         ← buffer RAM vidéo (?)


Carte FMV (plage $E80000-$EFFFFF)
  │ Chip décodeur MPEG-1
  │ RAM vidéo dédiée
  │ Registres $C0/$C2/$CA — handshake CPU
  ▼
Sortie Composite / S-Video
```

### Résumé des points d'entrée hardware

| Adresse | Type | Rôle |
|---------|------|------|
| `$E80000–$E8FFFF` | REG $00–$FF | Registres du décodeur MPEG |
| `$E8C000` | MMIO $C0 | Contrôle + IRQ ack |
| `$E8C001` | MMIO $C1 | Complémentaire |
| `$E8C002` | MMIO $C2 | Statut |
| `$E8C00A` | MMIO $CA | Configuration |
| `$E90000–$EFFFFF` | RAM | Buffer vidéo MPEG |

> Pour le détail de chaque registre, voir [`FMV_MMIO_MAP.md`](FMV_MMIO_MAP.md).

---

## 7. Tableau de statut — FMV

| Tâche | État | Notes |
|-------|------|-------|
| Dump & analyse désentrelacé | ✅ Terminé | Méthode éprouvée |
| Extraction des 15 modules OS-9 | ✅ Terminé | `os9_module_parser.py` |
| Analyse fmvll : table d'init @ $1EE0 | ✅ Terminé | 24 paires décodées |
| Analyse fmvdrv : dispatcher + codes | ✅ Terminé | $0CA8 + table $0100-$0117 |
| Cartographie MMIO $E80000–$EFFFFF | ✅ Terminé | Voir FMV_MMIO_MAP |
| Routine IRQ @ $022E (ack $C0=0x10) | ✅ Terminé | BSET #$04, $C0 + RTE |
| Chaîne d'appel complète | ✅ Terminé | App → fmvdrv → fmvll → hw |
| Décodage des 24 codes SetStat | 🔲 À confirmer | Corréler avec docs MPEG |
| Identification chip MPEG | 🔲 À démarrer | STi 5500 probable |
| Reverse-engineering csd_fmvvm | 🔲 À démarrer | Module intermédiaire |
| Émulation buffer vidéo MPEG | 🔲 À démarrer | RAM $E90000+ |
| Synchronisation audio/vidéo | 🔲 À démarrer | Gestion buffers |

---

## 8. Outils associés

| Script | Usage |
|--------|-------|
| `tools/check_FMV_entrelacement.py` | Désentrelace le dump de la cartouche FMV (le dump est entrelacé — lignes paires/impaires mélangées) |
| `tools/os9_module_parser.py` | Parse les en-têtes OS-9 dans un dump binaire (modules 68000) |

---

*Document axe FMV — dernière mise à jour : 21 juin 2026*
