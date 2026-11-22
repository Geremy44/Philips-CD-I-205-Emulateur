# Philips CD-i 205 — Émulateur

> ⚠️ **Avertissement légal** — Ce projet ne fournit aucune ROM, BIOS ni logiciel sous copyright. Philips conserve tous les droits sur ses œuvres. Les fichiers `.bin`, `.rom` et `.iso` **ne doivent jamais** être committés (voir `.gitignore`). Ce dépôt ne contient que le code source de l'émulateur et de la documentation de reverse-engineering, à des fins éducatives.

---

## 🎯 Objectif

Émulateur fonctionnel du **Philips CD-i 205**, ordinateur de salon basé sur un Motorola 68070, exécutant l'OS **CD-RTOS** (dérivé d'OS-9 68k), avec support de la **Digital Video Cartridge** (cartouche FMV) pour la lecture de vidéo MPEG-1.

**Plateforme cible** : Windows / Linux / macOS (C99 + SDL2 / SDL2_ttf)

---

## 📍 Où en est l'émulateur

Avec la ROM `CDI205-00_PS-7211_REL.2.1`, l'émulateur démarre CD-RTOS jusqu'au
**menu principal du CD-i, en couleurs** (fond, logo PHILIPS, boutons INFO /
SETTINGS / DIM / MEMORY / OPEN, curseur matériel), lance les disques CD-i,
et **fait tourner des jeux** : *Astérix : Le Défi de César* est jouable
(intro, dessins animés, menus, plateaux, voix, musique et bruitages ; une
partie complète reste à tester), voir [Jeux testés](#jeux-testés) :

![Menu du CD-i 205 rendu par l'émulateur](src/cdi_emu/menu_cdi_boot.png)

Ce qui fonctionne :

- **CPU 68070** : interpréteur 68000 + trame d'exception courte du 68070
  (SR, PC, format/vecteur), USP/SSP, `TRAP`, `RTE`, `STOP`, interruptions
  par niveau, ADDX/SUBX, DIVU/DIVS, CHK, TRAPV, EXT, MOVE SR/CCR, accès
  lecture-modification-écriture corrects. **Durée de chaque instruction**
  d'après les tables de la fiche technique SCC68070 (tables 7 à 19 :
  modes d'adressage, MOVE, MOVEM, décalages, branchements, exceptions),
  soit 16 à 21 cycles par instruction en moyenne (~0,8 MIPS à 15 MHz).
- **Périphériques internes du 68070** : timer T0/T1/T2 (tick OS-9 à 100 Hz),
  registres de priorité PICR1/PICR2, UART (console SDL).
- **68HC05 (HLE)** : réponses aux requêtes de CD-RTOS, IRQ niveau 2,
  pointeur absolu (souris / clavier).
- **NVRAM + horloge M48T08** : sauvegarde automatique, slots F5/F9.
- **CDIC (lecteur CD)** : commandes du pilote `cdapdriv` (reset, TOC,
  lecture mode 1 / mode 2 avec filtres fichier / canal, seek, update —
  qui ne change que les filtres, la lecture en cours continue),
  secteurs livrés à 75 par seconde dans les tampons de la RAM du CDIC
  avec leur sous-code Q, interruption niveau 4. Images `.cue`/`.bin`
  (2352 octets) et `.iso` (2048 octets). Le 68HC05 signale le disque à
  CD-RTOS (`B0 00 02 15`). Les secteurs passent en mémoire par le **DMA
  du 68070** (canal 1, `$80004000`), comme sur la vraie machine.
- **Lancement d'un disque** : PLAY CD-I → `launcher` lit le label, le
  répertoire et l'application, qui démarre. Testé avec *Astérix : Le Défi
  de César* : intro Philips, logos Philips Interactive Media / Pathé /
  Infogrames, dessin animé d'introduction (avec musique) et **écran titre**
  du jeu (JOUER / REJOUER / RÈGLE / FIN) ; JOUER lance la partie.
- **Vitesse réelle** : le temps émulé (cycles à 15 MHz) suit l'horloge du
  PC ; trames vidéo à 50 Hz (lignes de 64 µs).
- **Audio ADPCM CD-i** : les secteurs audio du canal choisi vont dans les
  tampons audio du CDIC (`$2800`/`$3200`), sont décodés (niveaux A/B/C,
  mono/stéréo, 37,8/18,9 kHz) et joués par SDL. La musique de l'intro
  d'Astérix sort correctement.
- **Interruption vidéo** : l'instruction INTERRUPT des programmes ICA, ou
  des DCA d'une ligne (signal « à la ligne N » de CD-RTOS), lève IT1
  (niveau 5, autovecteur 61) — c'est le « top » dont les jeux se servent
  pour se synchroniser.
- **Reset par le 68HC05** (`$8A` sur `$200005`) : redémarrage à chaud après
  un `F$Panic` / « Fatal System Error », comme le vrai lecteur — c'est aussi
  ce qui ramène au shell quand on quitte un logiciel. Le V.S.D. est remis
  à zéro et chaque SCC66470 garde son câblage (programme ICA de l'esclave
  en `$80400`), sinon le shell restait sur un écran cyan puis délavé.
- **Vidéo** : les deux SCC66470 d'après leur datasheet (maître = plan A en
  DRAM banque 1, esclave = plan B en banque 2) — programmes ICA/DCA,
  fichiers bitmap / run-length / mosaïque en 4 et 8 bits/pixel, bordure,
  résolutions — puis le V.S.D. (palette CLUT 256 couleurs en 4 banques,
  codage CLUT7/CLUT8, clé de transparence par plan, ordre des plans,
  superposition, curseur) et décodage **DYUV** des images naturelles
  (deltas quantifiés du Green Book, valeur de départ CA/CB, YUV → RGB),
  facteurs de contribution DB/DC et mixage des plans (fondus). Les
  commandes émises par les DCA s'appliquent à partir de leur ligne.
- **Sons en mémoire** (« ap ») : tampons `$2800`/`$3200` remplis par DMA,
  lecture via AUDCTL/ABUF avec interruption (voix et bruitages des jeux).
- **CD audio (CD-DA)** : le 68HC05 annonce le type de disque (`B0 00 01 15`
  pour un CD audio, `02 15` pour un CD-i) ; le shell affiche alors son
  lecteur de CD audio (pistes, temps, lecture, pause, choix de piste). La
  commande CDIC `$28` envoie le PCM 44,1 kHz vers la sortie son
  (rééchantillonné à 37,8 kHz) et ne donne au pilote que le sous-code Q
  (piste, temps) — tampons vides, sinon le shell y lit des « graphismes »
  CD+G.
- **Pointeur relatif** : le 68HC05 applique les repositionnements du pilote
  (`C0..` sur `$200001`) ; tant que le jeu recentre le curseur, la souris
  donne des déplacements, sinon le curseur CD-i suit la souris du PC
  (recalage mesuré sur le curseur du shell : Y × 240/560). Pendant que le
  jeu recentre le curseur, la souris du PC est capturée dans la fenêtre
  (elle n'en sort plus) et ses déplacements partent en impulsions ; elle
  est relâchée 2 s après, sous le curseur CD-i (F8 ou Alt+Tab la libèrent
  aussi). Les flèches envoient des impulsions (jeux qui lisent le pointeur
  comme une manette).
- **Bus 24 bits** : comme le 68070, l'octet de poids fort des adresses est
  ignoré (hors périphériques internes `$80000000+`) ; certains jeux rangent
  des drapeaux dans les pointeurs.
- **Cadence** : image, entrées et synchronisation au temps réel une fois par
  trame vidéo (50 Hz), avec une horloge haute précision.
- **Fenêtre** : surface 4:3 redimensionnable, plein écran avec bandes
  noires, barre de menus (Windows), souris du PC masquée sur l'image,
  console série masquable, fermeture par la fenêtre vidéo.

Pas encore émulé : graphismes CD+G, drapeaux de région (matte) et facteurs par région,
codage RGB555 et son bit de transparence, valeurs de départ DYUV rechargées
ligne par ligne, interruptions UART/I²C, afficheur de façade, décodage de
la cartouche FMV (MPEG, nécessaire par exemple pour *The 7th Guest*).

**Cartouche vidéo numérique (FMV)** — menu **Matériel > Cartouche vidéo
numérique (MPEG)**, désactivée par défaut, mémorisée dans `options.ini`
(`EMU_FMV=1` / `0` la force) ; le changement provoque un reset, CD-RTOS ne
cherchant la cartouche qu'au démarrage. Il faut sa ROM dans
`ROM/FMV_combined_7308hi_7307lo.bin` (EPROM P7307 + P7308 R4.1, non fournie).
Émulées : la ROM en `$E40000`, le 1 Mo de mémoire en `$D00000`, la DRAM vidéo
en `$E80000`, la puce vidéo MPEG-1 (`$E04000`) et la puce audio (`$E03000`)
avec leurs registres, leurs interruptions (niveau 4, vectorisées) et leurs
transferts par le canal DMA 2 du 68070. L'image MPEG remplace la couleur de
fond derrière les plans du CD-i ; le son MPEG part vers la sortie audio.
Le comportement des puces suit l'émulation de MAME (demande de fusion
[#16181](https://github.com/mamedev/mame/pull/16181), BSD-3, d'après le cœur
[CDi_MiSTer](https://github.com/MiSTer-devel/CDi_MiSTer)) ; le décodage
MPEG-1 utilise [pl_mpeg](https://github.com/phoboslab/pl_mpeg) (MIT).
Détails : `docs/fmv/FMV_EMULATION_NOTES.md`.

### Jeux testés

| Jeu | État | Remarques |
|-----|------|-----------|
| *Astérix : Le Défi de César* | 🟡 Lancement OK, pas testé complètement | Intro, dessins animés, menus, plateaux, voix, musique, bruitages ; une partie de 30 min sans souci, jouée depuis le vrai CD (lecteur du PC) ; partie complète pas encore vérifiée |
| *Inca* | 🟡 Lancement OK, pas testé complètement | Image, son et curseur ; demandait l'INTERRUPT des DCA et les facteurs DB/DC changés en cours d'image |
| *CDI Pinball Volume 1* | 🟡 Lancement OK, pas testé complètement | Les deux flippers répondent (boutons 1 / 2) ; un peu de latence des commandes dans l'émulateur |
| *The 7th Guest* (disque 1) | 🟡 Lancement OK, pas testé complètement | Avec la cartouche FMV : introduction et cinématiques MPEG avec image et son, jusqu'à l'escalier ; quelques artefacts d'image par moments |
| *The 7th Guest* (disque 2, CD audio) | ✅ Fonctionne | Lecteur de CD audio du shell : 7 pistes, lecture, pause, choix de piste, temps écoulé |
| *CD Shoot* | 🟡 Lancement OK, pas testé complètement | Jouable ; la souris reste capturée dans la fenêtre pendant la visée |
| *Secret Mission* | 🟡 Lancement OK, pas testé complètement | Jeu et cinématiques avec image et son (le CDIC garde ses filtres pendant une mise à jour `$2E`) |
| *Kether* | 🟡 Lancement OK, pas testé complètement | Semble bien fonctionner (image, son, sons en mémoire) |
| *Harvest of the Sun* (visite interactive sur Van Gogh) | 🟡 Lancement OK, pas testé complètement | Images propres, fondus et son OK |
| *L'Open de Palm Springs* | 🟡 Lancement OK, pas testé complètement | Fonctionne bien |
| *Journal interactif 94* (TF1, disque 2) | 🟡 Lancement OK, pas testé complètement | Avec la cartouche FMV : vidéo MPEG à 25 images/s et son MPEG (tests sans fenêtre) ; sans elle, « Votre carte numérique n'est pas installée », comme un vrai 205 |

### Charger un disque

```bash
./cdi205 "chemin/vers/jeu.cue"
```

ou `EMU_DISC=chemin/vers/jeu.cue`, ou en cours de route : bouton **OPEN** du
menu CD-i (le 68HC05 reçoit `$89` / `$92` pour ouvrir / fermer le tiroir ;
l'explorateur s'ouvre et le tiroir se referme avec l'image choisie), ou
menu Fichier > Ouvrir un disque (Ctrl+O) / Éjecter. Formats : `.cue` + `.bin`
(un fichier BIN pour tout le disque ou un par piste, pistes
MODE1/MODE2/AUDIO en 2352 octets), `.bin`/`.img` brut (une piste CD-i ou
un CD audio), `.iso` (secteurs de 2048 octets). Une piste déclarée
« données » mais sans en-tête de synchronisation CD-ROM est jouée comme
de l'audio (CD audio mal décrit par son `.cue`).

**Disque physique (lecteur CD / DVD / BD du PC, Windows)** : le menu Fichier
liste chaque lecteur optique de la machine :

- **Jouer le disque du lecteur X:** — l'émulateur lit directement le disque
  (`cdrom:X` en argument ou dans `EMU_DISC` marche aussi) ;
- **Extraire le disque du lecteur X: en BIN/CUE…** — copie le disque entier
  en secteurs bruts de 2352 octets (données CD-i et pistes audio) dans
  `CDI-205/Games/<nom>/<nom>.bin` + `.cue` (dossier trouvé au-dessus de
  l'exécutable, sinon `Games` à côté de lui). Le nom est le volume du
  disque CD-i ; un dossier existant n'est jamais écrasé (`_2`, `_3`…). La
  progression s'affiche dans le titre de la fenêtre, le résultat dans une
  boîte de dialogue ; l'émulateur continue de tourner pendant ce temps.
  Secteurs illisibles : 3 essais, puis remplis de zéros (signalé).

Lecture brute par la commande SCSI READ CD (`$BE`, SCSI pass-through),
`IOCTL_CDROM_RAW_READ` en secours.

### Contrôles

La fenêtre vidéo a une barre de menus (Fichier / Affichage / Contrôles) ;
la fermer quitte l'émulateur. La console série est masquée par défaut
(menu Affichage, ou `EMU_CONSOLE=1`).

| Action | Touche / souris (fenêtre vidéo) |
|---|---|
| Déplacer le curseur CD-i | souris (le curseur CD-i reste sous la souris), clavier ou manette |
| Directions (clavier / manette) | flèches, pavé 8-2-4-6, croix et stick gauche ; un appui = une impulsion, maintenu = déplacement continu ; Maj / RB / RT = ×3 |
| Boutons du CD-i | clics gauche / droit (inversés par défaut comme sur la manette, **F7** pour échanger) ; Espace-Entrée / Retour arrière-Ctrl droit ; manette A-X / B-Y |
| Assigner les touches et la manette | menu **Contrôles > Assigner les touches…** : clic sur une case puis touche, bouton ou direction de stick ; clic droit efface ; enregistré dans `controles.ini` à côté de l'exécutable |
| Couper la souris (clavier / manette seuls) | menu **Contrôles > Souris active** (aussi dans le panneau d'assignation) |
| Brancher / retirer la cartouche vidéo numérique (MPEG) | menu **Matériel** (le CD-i redémarre) |
| Réinitialiser le CD-i (bouton reset, le disque reste) | menu **Fichier > Réinitialiser le CD-i** ou **Ctrl+R** |
| Ouvrir un disque (image) | menu **Fichier > Ouvrir un disque…** ou **Ctrl+O**, ou le bouton OPEN du CD-i |
| Plein écran 4:3 (bandes noires) / fenêtre | **F11** ou **Alt+Entrée** |
| Afficher / masquer la souris du PC sur l'image | **F8** |
| Inverser clic gauche / droit | **F7** |
| Sauvegarder NVRAM + horloge dans le slot courant | **F5** |
| Rappeler le slot courant | **F9** |
| Slot suivant (1 → 9) | **F6** |

### Build portable (Windows)

`src/cdi_emu/dist/cdi205-portable/` (ignoré par git) : `cdi205.exe`, DLL
SDL2 et dépendances UCRT64, `ROM/`, `lancer.bat` (glisser un `.cue` dessus)
et `enregistrer.bat` (session enregistrée sur une NVRAM vierge, `partie.rec`
à renvoyer pour un rejeu identique d'un bug).

### Sauvegardes de jeu et horloge

- La NVRAM (où les jeux CD-i enregistrent leurs parties) est écrite dans
  `nvram.bin` **automatiquement 2 s après chaque modification**, et à la
  fermeture ; l'écriture est atomique (fichier temporaire puis renommage).
- L'horloge M48T08 (`$3FBFF0`) suit l'heure du PC ; si on la règle depuis le
  CD-i, seul le décalage est mémorisé dans `nvram.rtc`, et elle continue
  donc de tourner entre deux sessions.
- `EMU_NVRAM=chemin/nom` utilise `nom.bin` / `nom.rtc` à la place (une
  NVRAM par joueur ou par jeu, par exemple).
- F5 / F9 : instantanés complets NVRAM + horloge dans `saves/slotN.sav`.

### Options de debug (variables d'environnement)

| Variable | Effet |
|---|---|
| `EMU_VIDEO_LOG=1` | journalise l'état vidéo (DCR, ICA, commandes BEP) |
| `EMU_VIDEO_DUMP=frame.bmp` | enregistre l'image affichée à chaque changement |
| `EMU_TRACE=1`, `EMU_TRACE_MAX=n` | trace instruction par instruction |
| `EMU_PC_POLL=n` | affiche le PC toutes les `n` instructions |
| `EMU_SLAVE_LOG=1` | journalise le dialogue avec le 68HC05 |
| `EMU_CDIC_LOG=1` | journalise les commandes et les secteurs du CDIC (`2` : chaque interruption) |
| `EMU_DISC=jeu.cue` | image disque à charger (sinon 1er argument) |
| `EMU_OS9_LOG=1` | trace les appels système OS-9 en erreur (`2` : tous) |
| `EMU_EXC_LOG=1` | signale les exceptions CPU (erreur d'adresse, instruction illégale…) |
| `EMU_UART_LOG=1` | recopie la console UART sur stderr |
| `EMU_BREAK=1A96D0,1A9706` | à ces PC : 64 derniers PC, registres, pile, code |
| `EMU_DUMP=adresse,taille` | (hexa) affiche aussi cette zone mémoire à chaque `EMU_BREAK` |
| `EMU_PLANE_LOG=1` | toutes les 50 trames : pixels les plus fréquents de chaque plan et registres de mixage |
| `EMU_SPEED=100` | vitesse en % du vrai CD-i (`0` = sans limite) |
| `EMU_AUDIO=0` / `EMU_AUDIO_DUMP=son.wav` | coupe le son / enregistre le son en WAV |
| `EMU_WATCH=E5100-E5200`, `EMU_WATCH_AFTER=n` | signale les écritures dans cette plage (et si c'est le DMA) |
| `EMU_DMA_LOG=1` | journalise chaque transfert DMA CDIC ↔ mémoire |
| `EMU_INPUT_REC=f.rec` / `EMU_INPUT_PLAY=f.rec` | enregistre / rejoue souris, clics, disque choisi (OPEN) et commandes Ouvrir / Éjecter au pas près (tests reproductibles) |
| `EMU_INPUT_LOG=1` | pendant un rejeu, affiche chaque changement des boutons |
| `EMU_AUTOCLICK=x,y,pas` | clic automatique du bouton 1 (répété 8 fois) |
| `EMU_NVRAM=nom` | fichiers NVRAM/horloge `nom.bin` / `nom.rtc` |
| `EMU_CONSOLE=1` | affiche la console série au démarrage |
| `EMU_SWAP_BUTTONS=0` | n'inverse pas les boutons de la souris |
| `EMU_PACE_LOG=1` | histogramme des intervalles entre trames (régularité) |
| `EMU_CPI=n` | durée fixe de n cycles par instruction au lieu des tables du SCC68070 (ancien comportement : 8) |
| `EMU_CPU_STATS=1` | cycles moyens par instruction, tous les 50 millions d'instructions |
| `EMU_HEADLESS=1` | fenêtre vidéo cachée (tests automatiques) |
| `EMU_UI_SHOT=f.bmp` | ouvre le panneau d'assignation et enregistre une capture de la fenêtre |
| `EMU_FMV=1` / `0` | cartouche vidéo numérique branchée / retirée (remplace le choix du menu) |
| `EMU_FMV_LOG=1` / `2` | journal de la cartouche (commandes, images décodées, niveau audio ; 2 : tous les registres) |
| `EMU_FMV_DUMP=f.mpg` / `EMU_FMA_DUMP=f.mp2` | enregistre le flux MPEG envoyé aux puces / l'audio reçu (tests) |
| `EMU_FORCE_RESET=n` | redémarrage à chaud au pas n (tests) |
| `EMU_ROM_DUMP=f.bin` | enregistre la ROM telle que chargée |

---

## 📂 Arborescence du code de l'émulateur

```
src/cdi_emu/
├── main.c                    ← boucle principale (CPU, timer, IRQ, vidéo, SDL)
├── build-project.bat         ← compilation Windows (gcc)
├── build-linux.sh            ← compilation Linux (gcc + pkg-config)
├── ROM/                      ← ROM système (non fournie, voir avertissement)
└── core/
    ├── bus.c/h               ← carte mémoire et routage MMIO
    ├── rom_loader.c/h        ← chargeur Intel HEX
    ├── rtc_m48t08.c/h        ← horloge M48T08 (RTC) de la NVRAM
    ├── CPU/
    │   ├── m68k*.c/h         ← CPU 68070 (décodage, modes d'adressage, exceptions)
    │   ├── onchip68070.c/h   ← timer + PICR du 68070
    │   ├── uart.c/h          ← UART du 68070
    │   └── uart_console.c/h  ← terminal UART (SDL2 + SDL2_ttf)
    ├── GPU/
    │   ├── scc66470.c/h      ← contrôleur vidéo SCC66470 ×2 (registres, ICA/DCA, rendu)
    │   ├── vsd.c/h           ← V.S.D. : palette, codage, transparence, plans, curseur
    │   └── video_display.c/h ← fenêtre vidéo SDL2
    ├── SLAVE/                ← co-processeur 68HC05 (HLE : requêtes, pointeur)
    └── CDIC/                 ← contrôleur CD (cdic.c) + images disque (disc.c)
```

---

## 🔬 Deux axes de recherche

Ce projet sépare rigoureusement l'analyse en **deux axes indépendants** :

### Axe A — CONSOLE (CD-i 205)
> Le matériel de base, commun à tous les modèles CD-i.

- **CPU** : Motorola 68070 @ 15 MHz (68000 + périphériques intégrés)
- **Vidéo** : 2 × SCC66470 (VSC maître + esclave, un plan chacun) → V.S.D. (palette / mixage) → DAC ADV7120
- **Audio** : ADPCM PCM/FSCM, DAC stéréo
- **Système** : CD-RTOS (OS-9/68000), NVRAM, lecteur CD + contrôleur CDIC
- **Fichiers** : `docs/console/`

### Axe B — CARTOUCHE FMV (Digital Video Cartridge)
> L'accessoire enfichable optionnel, décodant le MPEG-1.

- **Modules OS-9** : `fmvdrv`, `fmvll`, `csd_fmvvm`…
- **Décodeur** : chip MPEG-1 (SGS-Thomson ?), MMIO dédié `$E80000–$EFFFFF`
- **Handshake** : IRQ dédiée, registre de contrôle `$C0`
- **Fichiers** : `docs/fmv/`

> ⚠️ **Règle de délimitation** : les adresses `$E80000–$EFFFFF` sont **réservées à la FMV**. La console ne doit jamais y accéder.

---

## 📊 Tableau d'avancement

### Axe Console — implémentation en cours
| Tâche | État |
|-------|------|
| Extraction ROM CD-RTOS | ✅ Terminé |
| Cartographie mémoire complète | ✅ Terminé |
| Boot CD-RTOS jusqu'au menu principal | ✅ Terminé |
| Exceptions / interruptions 68070 | ✅ Terminé |
| Implémentation timers 68070 | ✅ Terminé |
| Implémentation UART (sans interruptions) | ✅ Terminé |
| SCC66470 : ICA/DCA, bitmap, run-length, mosaïque | ✅ Terminé (plan A) |
| SCC66470 esclave (plan B) | ✅ Terminé |
| V.S.D. : palette, transparence, ordre des plans, curseur | ✅ Terminé (superposition) |
| V.S.D. : DYUV | ✅ Terminé |
| V.S.D. : facteurs DB/DC, mixage (fondus) | ✅ Terminé |
| V.S.D. : RGB555, régions (matte) | 🔲 À démarrer |
| CDIC : commandes, TOC, lecture de secteurs | ✅ Terminé (Astérix jouable) |
| CDIC : audio ADPCM (lecture disque) | ✅ Terminé |
| CDIC : sons en mémoire (« ap ») | ✅ Terminé |
| CDIC : audio CD-DA (CD audio) | ✅ Terminé (lecteur de CD audio du shell) |
| Bouton OPEN : changer de disque | ✅ Terminé |
| Émulation NVRAM + horloge (sauvegardes) | ✅ Terminé |
| Entrées : pointeur via 68HC05 (HLE, absolu + relatif) | ✅ Terminé |
| Vitesse réelle, plein écran 4:3, menus | ✅ Terminé |
| Afficheur de façade | 🔲 À démarrer |
| DMA 68070 ↔ CDIC (données et audio) | ✅ Terminé |

### Axe FMV — implémentation en cours
| Tâche | État |
|-------|------|
| Dump & analyse désentrelacé | ✅ Terminé |
| Extraction des 15 modules OS-9 | ✅ Terminé |
| Analyse fmvll : table d'init @ $1EE0 | ✅ Terminé |
| Analyse fmvdrv : dispatcher + codes | ✅ Terminé |
| Cartographie MMIO $E80000–$EFFFFF | ✅ Terminé |
| Routine IRQ @ $022E | ✅ Terminé |
| Chaîne d'appel complète | ✅ Terminé |
| Décodage des 24 codes SetStat | ✅ Terminé |
| Identification chip MPEG | ✅ Terminé |
| Reverse-engineering csd_fmvvm | ⚠️ En cours |
| Émulation buffer vidéo MPEG | ⚠️ En cours |
| Synchronisation audio/vidéo | 🔲 À démarrer |

---

## 🚀 Pour commencer

1. **Cloner le dépôt**
   ```bash
   git clone https://github.com/Geremy44/Philips-CD-I-205-Emulateur.git
   cd Philips-CD-I-205-Emulateur
   ```

2. **Compiler l'émulateur** : voir [Compilation](#️-compilation) ci-dessous.

3. **Explorer la documentation**
   - Console : `docs/console/CONSOLE_OVERVIEW.md`
   - FMV : `docs/fmv/FMV_EMULATION_NOTES.md`, `docs/fmv/FMV_OS9_FINDINGS.md`

4. **Exécuter l'émulateur** (depuis `src/cdi_emu`, la ROM est lue dans `ROM/`)
   ```bash
   cd src/cdi_emu && bin/cdi205 "chemin/vers/jeu.cue"
   ```
   Sous Windows : `bin\cdi_emu.exe`. Le menu du CD-i apparaît après une
   trentaine de secondes.

---

## 🛠️ Compilation

Les ROM ne sont pas fournies. Placer la ROM système
`CDI205-00_PS-7211_REL.2.1.hex` dans `src/cdi_emu/ROM/` et, pour la
cartouche FMV, `FMV_combined_7308hi_7307lo.bin` dans le même dossier.

### Windows (MSYS2 UCRT64)

1. Installer [MSYS2](https://www.msys2.org/), puis dans le terminal
   **MSYS2 UCRT64** :
   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-SDL2_ttf
   ```
2. Compiler depuis `cmd`, avec `C:\msys64\ucrt64\bin` dans le `PATH` :
   ```cmd
   cd src\cdi_emu
   build-project.bat
   ```
   L'exécutable est `bin\cdi_emu.exe`.
3. Pour le lancer hors de MSYS2, copier à côté de l'exe les DLL de
   `C:\msys64\ucrt64\bin` dont il dépend (`SDL2.dll`, `SDL2_ttf.dll`,
   `libfreetype-6.dll`, `libharfbuzz-0.dll`, `libpng16-16.dll`, `zlib1.dll`…).
   Dans le terminal UCRT64, `ldd bin/cdi_emu.exe | grep ucrt64` en donne la
   liste. La version portable des releases contient déjà ces DLL.

### Linux

> ⚠️ La compilation Linux n'a pas encore été testée sur une vraie machine.
> Le code isole les parties propres à Windows (lecteur CD physique, boîte
> de dialogue d'ouverture), qui sont simplement absentes sous Linux.

1. Installer les dépendances :
   - Debian / Ubuntu : `sudo apt install build-essential pkg-config libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core`
   - Fedora : `sudo dnf install gcc make pkgconf-pkg-config SDL2-devel SDL2_ttf-devel dejavu-sans-mono-fonts`
   - Arch : `sudo pacman -S base-devel sdl2 sdl2_ttf ttf-dejavu`
2. Compiler :
   ```bash
   cd src/cdi_emu
   sh build-linux.sh
   ```
   L'exécutable est `bin/cdi205`.
3. Lancer : `bin/cdi205 "chemin/vers/jeu.cue"`.

---

## 📄 Licence

Documentation de reverse-engineering à fins éducatives uniquement. Voir avertissement légal en tête de fichier.

Les mentions de copyright et textes de licence des composants tiers sont
dans [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) et le dossier
[`licenses/`](licenses/).

## 🙏 Crédits

- **pl_mpeg** — Dominic Szablewski (MIT) : décodage MPEG-1 vidéo et audio
  de la cartouche FMV.
- **MAME**, demande de fusion [#16181](https://github.com/mamedev/mame/pull/16181)
  — Alexandre Derumier (BSD-3-Clause) : comportement des puces de la
  cartouche Digital Video, porté en C.
- **CDi_MiSTer** — Andre Zeps : le cœur FPGA dont ce travail de MAME est issu.
- **SDL2 / SDL2_ttf** — Sam Lantinga et les contributeurs SDL (zlib) :
  fenêtre, son, entrées et texte.
- **FreeType**, **HarfBuzz**, **GLib**, **gettext**, **libiconv**, **Graphite**,
  **PCRE2**, **libpng**, **zlib**, **bzip2**, **Brotli**, **GCC**, **mingw-w64**
  : bibliothèques livrées avec la version Windows (paquets MSYS2). Portions of
  this software are copyright © 2023 The FreeType Project (www.freetype.org).
  All rights reserved.

Philips et CD-i sont des marques de leurs propriétaires respectifs. Ce
projet n'est ni affilié à Philips ni approuvé par Philips ; aucune ROM ni
image de disque n'est fournie.

---

*Dernière mise à jour : 22 novembre 2026*
