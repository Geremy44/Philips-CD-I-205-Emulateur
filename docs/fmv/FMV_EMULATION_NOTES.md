# Cartouche FMV (Digital Video Cartridge) — notes d'émulation vérifiées

> Faits établis en novembre 2026 en faisant tourner la vraie ROM de la
> cartouche dans l'émulateur. Ils remplacent les hypothèses des autres
> documents de ce dossier (STi 5500, adresses `$E80000` pour les registres,
> table de codes SetStat), qui ne se sont pas vérifiées.

## Matériel (manuel de service 22ER9141, schéma 4.4)

| Bloc | Composants |
|---|---|
| Audio | FMA 7105 (hôte) + SRAM, **DSP56001** 7115 + DSPRAM, DAC CS4328 |
| Vidéo | **FMV 7201** (décodeur MPEG-1) + DRAM 512 Ko (HM514260), VideoDAC MC44200 (RGB renvoyé au CD-i et mélangé avec ses plans) |
| Mémoire | 1 Mo de DRAM (2 × HM514260) + contrôleur DRC 7400 |
| ROM | EPROM P7307 (octets bas) / P7308 (octets hauts) R4.1, 2 × 64 Ko |

Le PDF du manuel est chiffré (RC4, mot de passe utilisateur vide) : les pages
s'extraient avec un déchiffrement standard.

## Modules de la ROM (`FMV_combined_7308hi_7307lo.bin`, 128 Ko)

`sysgo`, `csd_fmvvm`, `fmvconf`, `vmpeg`, `vcd`, `fmvll`, `dspcode` (programme
du DSP56001, 16 Ko), `MoviMan` (gestionnaire de fichiers), `ma` (descripteur,
port **$E03000**), `madriv`, `mv` (descripteur, port **$E04000**), `fmvdrv`,
`fmvvolset`, `ramtest4`, `dummy`.

## Plan d'adresses

| Zone | Contenu | Vérifié par |
|---|---|---|
| `$E40000`-`$E5FFFF` | ROM | CD-RTOS trouve et lance ses modules ; `fmvll` teste ce domaine |
| `$E03000`… | puce audio FMA | descripteur `ma`, `fmvconf`, `madriv` |
| `$E04000`… | puce vidéo FMV | descripteur `mv`, `fmvdrv` |
| `$E01000`, `$E00C00` | registres utilisés par `fmvll` / `madriv` | traces |
| `$E80000`-`$EFFFFF` | DRAM de la puce vidéo (512 Ko) | `fmvconf` la déclare au noyau comme **mémoire colorée de type `$90`** (code en `$E40BF2`) ; `MoviMan` y réserve ensuite 512 Ko |
| `$D00000`-`$DFFFFF` | **1 Mo de DRAM supplémentaire** (mémoire système, trouvée par le sondage « Extension 1 ») | sans elle, le Journal échoue à réserver 328 Ko (erreur #8019) |

**Piège** : le module `init` du 205 déclare une zone « Extension 1 »
(`$500000`-`$FFFFFF`) que le noyau sonde par blocs de 32 Ko (motifs `5A A5`,
accès octet). La cartouche ne doit pas y répondre comme de la RAM, sinon le
noyau y place de la mémoire système, que les pilotes écrasent ensuite (boucle
de reset après `fmvconf`). Seules les deux routines de sondage de la ROM du
205 (`$1806F0`-`$180790` et `$184820`-`$184870`) voient un bus vide dans
`$E00000`-`$EFFFFF` ; le noyau doit, lui, accéder à la DRAM vidéo une fois
déclarée en type `$90`. Le 1 Mo en `$D00000`, lui, répond au sondage.

## Déroulé observé (cartouche branchée)

1. Le `sysgo` de la cartouche lance `fmvconf` : test `$E03018` (déjà
   initialisé ?), `F$Link dspcode`, envoi du programme DSP (≈ 36 000 écritures
   en `$E030xx`), initialisation de la puce vidéo (blocs en `$E048xx`-`$E05Cxx`),
   puis `$E03018 = 1`.
2. `sysgo` lance `vcd` (lecteur Vidéo CD), qui se termine.
3. Les pilotes s'installent : `F$IRQ` vecteur **`$5A`** (fmvdrv) et **`$7B`**
   (madriv), priorité `$80` ; module de données `AVSYNC00` créé par `F$DatMod`.
4. Le *Journal interactif 94* (disque 2, programme `cdi_TF194_JEU`) : ses
   codes d'erreur sont décimaux (« Erreur de lecture(#%d) »).
   - #8019 = échec de réservation de 328 Ko (`$188B0`) → manque le 1 Mo en
     `$D00000` ;
   - #8009 = `MoviMan` n'obtient pas 512 Ko de type `$90` (`F$SRqCMem`
     #171) → le noyau ne pouvait pas accéder à `$E80000` ;
   - ensuite : lecture MPEG lancée (CDIC `$2A`, LBA 212302, voies
     `$80000001`) puis interrompue (`$24`, `$23`) faute de décodeur ; écran
     cyan (fond laissé visible pour la vidéo MPEG).

## Chemin des données (vérifié)

- Le CDIC dépose les secteurs MPEG dans le 1 Mo en `$D00000` (DMA canal 1).
- `fmvdrv` / `madriv` écrivent à la main les en-têtes de paquet dans le port
  `$E040DE`, puis envoient la charge utile par le **canal DMA 2 du 68070**
  (`$80004040` : CSR=$FF, MAR, MTC, OCR $30, CCR $80) et lèvent la demande
  de transfert de la puce (bit 15 de SYSCMD `$E040C0` pour la vidéo, de CMD
  `$E03000` pour l'audio).
- Les deux puces partagent l'interruption **niveau 4** (descripteurs `mv` /
  `ma`), vectorisée : vidéo `IVEC($E040DC) >> 3` = `$5A`, audio `$7B` ;
  l'audio est prioritaire. Lire l'ISR (`$E04062`, `$E0301A`) l'efface.
- Registres, bits d'état (RFB, PIC, VSYNC, TIM à 100,45 Hz...) et rythme
  d'affichage : repris de MAME, PR #16181 (mcd251, gsc38gg307, cdidvc,
  mpeg_demux), même ROM R4.1. Chaque puce ne garde que son flux ($E0+n
  vidéo, $C0+n audio).

## Implémentation

`src/cdi_emu/core/FMV/` : `fmv.c` (carte : adresses, DMA, interruption,
temps), `fmv_video.c` (puce vidéo, décodage pl_mpeg, ordre d'affichage),
`fmv_audio.c` (puce audio, MP2 via pl_mpeg, rééchantillonné à 37,8 kHz),
`mpeg_demux.c`, `pl_mpeg.h`. L'image est mélangée dans `vsd.c` à la place
du fond (`vsd_ext_on`).

État au 21/11/2026 : *Journal interactif* disque 2 → vidéo décodée et
affichée à 25 images/s, son MPEG produit au bon débit (tests sans fenêtre).

22/11/2026 : testé en vrai sur le Journal (fonctionne, quelques bugs).
*The 7th Guest* disque 1 restait figé sur le logo Philips, sans son. Deux
corrections :
- Son : la piste alterne stéréo et stéréo jointe d'une trame à l'autre ;
  pl_mpeg prenait ça pour une perte de synchro et ne décodait qu'une trame.
  Le contrôle de `pl_mpeg.h` ne compare plus que mono / stéréo (modification
  signalée dans le fichier).
- Vidéo : le compteur d'images en attente n'était décrémenté qu'à la mise en
  file. Les images sautées ou retenues pour le réordonnancement n'étaient
  jamais décomptées, donc la fin des données (EOD) n'était jamais signalée.
  Il est maintenant décrémenté au décodage, et la référence retenue compte
  pour EOD, NDAT et le registre $A4.
Le jeu enchaîne maintenant le logo et les séquences suivantes tout seul.

Ensuite, à la fin des cinématiques (face à l'escalier), le jeu se figeait.
La dernière image de référence d'un clip restait retenue pour le
réordonnancement, faute d'image suivante : elle n'était jamais affichée, et
EOD n'était jamais signalée. Quand la file d'affichage est vide, la
référence retenue est maintenant affichée (c'est la suivante dans l'ordre).
Reproduit et vérifié par rejeu de la partie enregistrée (`partie.rec`).

## Prochaines étapes

- *The 7th Guest* : vérifier en vrai au-delà de l'escalier ; quelques
  artefacts d'image par moments.
- Journal interactif, bouton 2 (menu) puis bouton 1 au centre (le jeu) :
  le menu s'affiche, mais les 2×4 photos (centrées à gauche) et la ligne de
  4 images avec silhouette noire (en haut à droite) ne sont pas dessinées
  par-dessus.
- Position / taille de l'image selon les registres de fenêtre ; synchro
  son / image.
- Le pilote audio envoie parfois plus vite que la lecture (surplus écarté) :
  modéliser la demande de transfert de la puce audio selon sa file.
