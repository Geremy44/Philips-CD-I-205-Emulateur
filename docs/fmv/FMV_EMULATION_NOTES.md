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
  d'affichage : décrits dans `FMV_COMPORTEMENT.md` (le travail de MAME,
  PR #16181, sur la même ROM R4.1, a servi de référence). Chaque puce ne garde que son flux ($E0+n
  vidéo, $C0+n audio).

## Implémentation

`src/cdi_emu/core/FMV/` : `fmv.c` (carte : adresses, DMA, interruption,
temps), `mcd251.c` (puce vidéo, décodage pl_mpeg, ordre d'affichage),
`fma.c` (puce audio, MP2 via pl_mpeg, rééchantillonné à 37,8 kHz),
`mpeg_system.c` (lecture du flux système), `pl_mpeg.h`. Comportement
attendu : `FMV_COMPORTEMENT.md`. L'image est mélangée dans `vsd.c` à la place
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

Journal interactif (22/11/2026, rejeu d'une partie enregistrée) :
- Son haché : dans le menu, le pilote renvoie sa boucle musicale de 2 s
  toutes les 0,5 s environ (ce qui le cadence sur le vrai matériel n'est pas
  identifié : ni l'EOI, ni le bit 9 d'état). Couper des octets quand la file
  déborde cassait des trames MP2 (son haché). Retenir le DMA du pilote était
  pire : il l'attend en `$E50EB0` avec les interruptions masquées (SR $2700),
  le CDIC perdait alors des secteurs et les images chargées étaient
  décalées (photos des rédacteurs mêlées de morceaux d'autres écrans après
  une sélection). Solution retenue : le DMA n'est jamais retenu, et quand la
  file contient déjà plus de 16 Ko au début d'un nouveau programme, ce
  programme entier (jusqu'à son code de fin) est écarté. Les boucles étant
  identiques, la musique reste continue (44 100 échantillons/s, aucune
  sous-alimentation). `EMU_FMA_HOLD` réactive la retenue pour les tests.
- Photos absentes du menu « Choisissez un rédacteur en chef » : le plan B a
  la transparence 0xB (« transparent si drapeau de région 0 faux »). Les
  registres de région D0-D7 n'étaient pas émulés. Valeurs écrites par le
  jeu ligne par ligne : 90FC6E / 90FD36 (mettre le drapeau 0 à 1 en X = 110
  ou 310) et 80FEB4 / 80FC00 (le remettre à 0 en X = 692 ou 0). Format
  retenu : bit 23 = change un drapeau, bit 20 = nouvelle valeur, bit 16 =
  drapeau 0 / 1, bits 9-0 = X en pixels haute résolution, drapeaux à 0 en
  début de ligne. Les changements de poids par région ne sont pas émulés.
- Son encore haché dans le menu (test réel) : à l'arrivée sur le menu, le
  CDIC joue aussi du son ADPCM (environ 5 s) pendant l'audio MPEG. La sortie
  son n'avait qu'une file : les deux flux s'y succédaient par morceaux.
  `audio_out.c` a maintenant une file par source (CDIC, cartouche) mélangées
  dans le rappel SDL, comme le mélangeur audio du lecteur.
- Artefacts vidéo (blocs décalés, images fantômes) : l'émulateur décodait
  parfois une image dont la fin n'était pas encore arrivée par DMA (un
  compteur d'« images complètes » en avance d'une image). Les images
  prédites à partir de cette image tronquée étaient fausses jusqu'à l'image I
  suivante. Le décodage suit maintenant la règle de pl_mpeg : une image
  n'est décodée que lorsque le code de début de l'image suivante est dans
  le tampon, ou en fin de séquence / de programme. Vérifié en comparant
  l'empreinte de chaque image décodée avec un décodage hors émulateur du
  même flux : 109 images différentes sur 295 avant, aucune après, à part
  celles qui suivent un vidage de file demandé par le jeu (le décodage hors
  émulateur ne le reproduit pas).
- En-têtes d'image : `sc_kind` valait 0 pour un en-tête d'image, ce qui
  désactivait sa lecture. Type, référence temporelle, code temporel du GOP
  et indicateurs début de GOP / séquence sont maintenant lus (registres
  d'état, interruptions GOP / SEQ).
- Fin de flux audio (UNF) : signalée dès que la puce n'a plus rien de
  décodé et moins d'une trame MP2 en entrée. Le *Journal interactif* lance
  l'animation de son menu sur cet UNF (fin du clip d'introduction, 61 s).
  Cas particulier : quand il ne reste qu'un échantillon, le rééchantillonneur
  ne peut plus avancer ; cela compte comme vide seulement après 400 ms sans
  données (*The 7th Guest* attend cet UNF pour relancer sa boucle d'ambiance,
  alors que la musique du menu du Journal, une boucle de 2 s renvoyée par le
  jeu, reprend après une pause de ~210 ms et ne doit pas s'arrêter). Un délai
  de 400 ms appliqué à tous les cas empêchait l'UNF de 61 s : cadre du menu
  figé et musique en boucle mal enchaînée. `EMU_FMA_UNF_MS` change ce délai
  (tests).
- Volumes (atténuateurs du Green Book : gauche→gauche, gauche→droite,
  droite→droite, droite→gauche, pas de 1 dB, bit 7 = voie coupée), appliqués
  à chaque source dans le mélangeur (`audio_out_set_atten`) :
  - son CD-i : commande `$Cx` + 4 octets sur le canal 2 du 68HC05 ; les bits
    1, 0, 3, 2 de `x` coupent LL, LR, RR, RL (`$C5` = stéréo, `$CA` =
    inversé, mesures du cœur CDi_MiSTer, doc/dac.md) ;
  - cartouche : le pilote charge un programme dans le DSP56001, puis envoie
    la commande hôte `$13` (CVR = `$93`) suivie de 4 octets sur TXL
    (registre `$E03024` après sélection par `$E03022`) : LL, LR, RL, RR.
  Menu du Journal : voix CD-i à −4 dB, musique MPEG à −24 dB.
  Quand les deux voies d'une même sortie sont ouvertes (`$C0`, mélange
  mono), leur somme est ramenée à 0 dB pour ne pas doubler ni saturer.

## Prochaines étapes

- *The 7th Guest* : vérifier en vrai le reste du jeu.
- Vérifier en vrai que les artefacts d'image ont disparu (corrigés en test).
- Position / taille de l'image selon les registres de fenêtre ; synchro
  son / image.
