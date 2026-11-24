# Cartouche Digital Video : comportement émulé

Description, en faits matériels, de ce que l'émulateur reproduit pour la
cartouche (22ER9141). Elle sert de cahier des charges au code de
`src/cdi_emu/core/FMV/`. Ses sources sont :
- le manuel de service (`Digital-Video-Cartridge-SM.pdf`) ;
- l'observation des pilotes de la ROM de la cartouche (`fmvdrv`, `madriv`,
  `fmvll`) ;
- les essais sur les jeux (`FMV_EMULATION_NOTES.md`).

Les durées sont comptées en périodes de l'horloge du 68070 (15 MHz).
L'horloge des puces (DCLK) est à 45 kHz.

## 1. Carte

| Zone | Contenu |
|---|---|
| `$D00000`-`$DFFFFF` | 1 Mo de RAM (mémoire système ajoutée) |
| `$E00000`-`$E3FFFF` | registres des puces, mot par mot, répétés tous les 64 Ko |
| `$E40000`-`$E7FFFF` | ROM de 128 Ko, répétée |
| `$E80000`-`$EFFFFF` | DRAM vidéo (512 Ko), déclarée par `fmvconf` |

- **Fenêtre des registres** (index = (adresse / 2) & `$7FFF`) :
  - `$18xx` : puce audio (FMA), registre `xx` ;
  - `$20xx` : puce vidéo (FMV), registre `xx` ;
  - `$08xx` : choix de l'horloge pixel (`$E01000`), écritures sans effet ;
  - `$206F` (`$E040DE`) : port de données. Un mot écrit à la main est envoyé
    à la puce de la dernière transmission, suivi d'une fin de transfert « non
    DMA ».
- **Accès octet aux registres.**
  - En lecture, l'octet pair lit le mot entier, et l'octet impair qui suit
    reçoit l'octet bas déjà lu.
  - En écriture, l'octet pair est retenu, et l'écriture du mot se fait avec
    l'octet impair.
- **Sondage mémoire du noyau.** Les deux routines de la ROM 205 qui
  cherchent de la mémoire (`$1806F0`-`$18078F`, `$184820`-`$18486F`) ne
  doivent trouver ni registres ni DRAM vidéo : elles lisent `$FF` et leurs
  écritures sont perdues.
- **Transmissions.** Une puce qui demande des données arme une demande. La
  transmission se fait par le canal DMA 2 du 68070 dès qu'il est lancé, d'un
  bloc. Chaque mot donne deux octets à la puce, octet fort d'abord. À la fin
  d'un bloc DMA, la puce reçoit « fin de transfert ». Tant que la puce audio
  attend, la demande est relancée à chaque pas de temps.
- **Interruptions.** Niveau 4, vectorisées. La puce audio passe avant la
  puce vidéo.
- **Retour de trame.** À chaque retour de trame du CD-i, la puce vidéo voit le
  début du blanking. La fin du blanking arrive 1,6 ms plus tard.

## 2. Flux système MPEG (commun aux deux puces)

Chaque puce lit tout le flux système, octet par octet, et ne garde que les
paquets de son propre flux : `$C0` + n pour l'audio, `$E0` + n pour la
vidéo, n étant le registre « numéro de flux ».

- **Codes de départ.** Un code `00 00 01 xx` n'est cherché qu'en dehors du
  contenu d'un paquet gardé. Des zéros en plus avant le `01` sont admis. Les
  paquets des autres flux ne sont pas sautés : leur contenu est relu comme
  du flux système.
- **`xx` = `$BA` (pack).**
  - Les 5 octets suivants forment le SCR. Ce sont les champs 3+15+15 bits de
    la norme, sans les bits de repère, et ils ne sont pas vérifiés.
  - Le SCR devient valable en lisant l'octet d'après (le 6e).
- **`xx` = `$B9`.** Fin de programme : une impulsion.
- **`xx` = flux gardé.**
  - Les 2 octets suivants donnent la longueur du paquet. Le décompte commence
    à l'octet qui les suit. Quand il arrive à 1 au début d'un octet, cet
    octet est le dernier du contenu.
  - **En-tête du paquet :**
    - octets `$FF` de bourrage ;
    - en option, la taille du tampon STD, sur 2 octets dont le premier vaut
      `01xxxxxx` ;
    - puis l'un des trois cas suivants :
      - `0010xxx1` + 4 octets : PTS seul ;
      - `0011xxx1` + 4 octets, puis `0001xxx1` + 4 octets : PTS puis DTS ;
      - `$0F` : aucune date.
  - **Contrôle des dates.**
    - Le premier octet d'une date doit avoir son bit 0 à 1, et pour le DTS
      sa forme exacte est vérifiée.
    - Les octets 3 et 5 doivent aussi avoir leur bit 0 à 1.
    - Un défaut abandonne l'en-tête : le contenu du paquet n'est pas gardé.
  - **Contenu du paquet.** Il commence à l'octet qui suit la dernière date,
    ou le `$0F`. En lisant le premier octet du contenu, les dates deviennent
    valables. Sans DTS, la DTS prend la valeur de la PTS.
- **Autres codes.** Ils sont ignorés.

## 3. Puce vidéo (MCD251, `$E04000`)

### Registres (index de mot)

| Index | Lecture | Écriture |
|---|---|---|
| `$01`/`$02`/`$03` | largeur / hauteur / code de cadence de l'image | idem |
| `$04`/`$05` | timecode de la dernière image décodée (fort / faible) | |
| `$29`-`$2D` | largeur, hauteur, cadence, timecode de l'image affichée | |
| `$2E` | état de l'image affichée : type << 6, référence temporelle | |
| `$2F` | bit 13 : demande de données (flux en attente < 32 Ko ou décodeur à sec) | |
| `$30` | masque d'interruptions | idem |
| `$31` | interruptions en attente, remises à zéro par la lecture | |
| `$32` | période du minuteur | relance le minuteur |
| `$36`-`$3F` | fenêtre : Yo Xo Ya Xa Yd Xd Hf Lf Yf Xf | idem |
| `$44` | commande décodeur | si l'octet fort vaut `$22` hors pause : image suivante |
| `$46` | commande VDI (bit 14 levé à chaque DTS reçue) | idem |
| `$4C` | SCR des puces : (DCLK >> 6) & `$7FFF` | recale cette horloge |
| `$4F` | toujours 1 | |
| `$50` | dernière DTS reçue >> 7, sur 15 bits | |
| `$52` | en lecture : images en réserve, ou dates en attente à l'arrêt | |
| `$54` | durée d'une image en 90 kHz | |
| `$55` | `$0708` (cadence PAL) | |
| `$56` | cadence demandée | idem |
| `$57` | | relance le minuteur |
| `$60` | commande système | voir plus bas |
| `$61` | commande d'affichage | voir plus bas |
| `$62` | numéro de flux (4 bits) | idem |
| `$63` | | réglage SCR (bit 2 : synchroniser sur les DTS) |
| `$6E` | vecteur (le 68070 reçoit la valeur >> 3) | idem |

### Interruptions (bits de `$30`/`$31`)

| Bit | Signification |
|---|---|
| 0 | début de séquence |
| 1 | début de groupe d'images |
| 2 | nouvelle image affichée |
| 3 | fin des données |
| 4 | (inutilisé) |
| 5 | plus de données |
| 6 | (inutilisé) |
| 7 | fenêtre mise à jour |
| 8 | minuteur |
| 9 | fin de séquence |
| 10 | fin de programme (ISO 11172) |
| 11 | début du blanking |
| 12 | pause atteinte |
| 13 | fenêtre mise à jour |

### Commande système (`$60`, bits pris dans cet ordre)

- **Bit 3, lecture.**
  - Le départ a lieu 1 000 DCLK plus tard.
  - La taille et la cadence affichées reprennent celles de la dernière image
    décodée.
  - Les bits 6 et 1 de la commande décodeur sont levés.
  - Le ralenti prend la valeur des bits 0-2.
- **Bit 4, pause.** La lecture s'arrête. L'interruption de pause arrive
  100 DCLK plus tard.
- **Bit 5, reprise.** Le départ a lieu 1 000 DCLK plus tard, et le ralenti
  prend la valeur des bits 0-2.
- **Bit 6, image par image.** L'image suivante sera affichée, et un départ
  prévu est annulé.
- **Bit 7, arrêt.**
- **Bit 8, vider.**
  - La lecture s'arrête et le décodeur est vidé : flux, images, dates.
  - Le démultiplexeur repart de zéro.
  - Le décodeur est mis en marche.
- **Bit 12, décodeur en marche.**
- **Bit 13, décodeur arrêté.**
  - La lecture s'arrête.
  - Les registres d'image et le timecode sont remis à zéro.
  - Plus rien n'est affiché, et le décodeur est vidé.
- **Bit 15.** Demande de transmission (DMA).

### Commande d'affichage (`$61`)

- **Bit 3.** La fenêtre (`$36`-`$3F`) sera recopiée :
  - au prochain début de blanking si le bit 2 (défilement) est levé ;
  - sinon, à la prochaine image affichée.

  La recopie lève les interruptions 7 et 13.
- **Bit 8.** Masque la vidéo.
- **Bit 9 ou 10.** Montre la vidéo.

### Décodage et ordre d'affichage

- **Suivi du flux.** Le flux vidéo reçu est suivi au fil de l'eau :
  - en-tête de séquence `$B3` : cadence ;
  - groupe d'images `$B8` : timecode ;
  - image `$00` : type et référence temporelle ;
  - fin de séquence `$B7`.

  Chaque image notée retient la dernière DTS reçue avant elle.
- **Quand décoder une image.** Une image n'est décodée qu'une fois entière :
  le code de départ suivant est arrivé, plus 8 octets. La fin d'une séquence
  ou d'un programme compte aussi.
- **Ordre d'affichage.**
  - Une image B est mise en file d'affichage tout de suite.
  - Une image I ou P est retenue. La référence retenue avant elle part alors
    dans la file.
  - La dernière référence part à la fin du programme, ou quand la file est
    vide pendant la lecture.
- **Taille de la file.** La file compte 8 images au plus.
- **Indicateurs.** La première image I après un en-tête de séquence ou de
  groupe porte les indicateurs « début de séquence » et « début de groupe ».

### Cadence d'affichage

- **Durée d'une image.** Elle vient du code de cadence, sur une horloge à
  30 MHz :

  | Codes | Durée (périodes de 30 MHz) |
  |---|---|
  | 0, 3 et 9 à 15 | 1 200 000 |
  | 1 | 1 251 251 |
  | 2 | 1 250 000 |
  | 4 | 1 001 001 |
  | 5 | 1 000 000 |
  | 6 | 600 000 |
  | 7 | 500 500 |
  | 8 | 500 000 |

  Le même tableau existe en 90 kHz : 3 600, 3 754, 3 750, 3 600, 3 003,
  3 000, 1 800, 1 502, 1 500, puis 3 600 pour la suite.
- **Le minuteur d'image.**
  - Chaque tick compare l'horloge des puces à la DTS de la prochaine image,
    en unités de 128 périodes de 90 kHz, avec un écart signé sur 15 bits.
  - Si la synchronisation est demandée, l'image attend que l'écart dépasse
    −30.
  - L'écart corrige la période suivante de 2 048 périodes par unité. Il ne
    le fait que si l'horloge a été réglée, que la DTS est valable et que le
    SCR n'est pas à plus de 15 000 en avance sur elle.
  - En ralenti (n > 1), la période est multipliée par n.
- **Au début du blanking qui suit un tick :**
  - la fenêtre est recopiée si elle attendait ;
  - si la file est vide pendant la lecture, la référence retenue est
    libérée ;
  - la première image de la file est affichée. Si c'était la dernière, sans
    rien en attente, l'interruption « plus de données » est levée.
- **À la fin de ce blanking :**
  - l'interruption « nouvelle image » est levée, ainsi que « début de
    groupe » et « début de séquence » si l'image les porte ;
  - « fin des données » est levée si tout est épuisé ;
  - le décodeur est relancé.
- **Minuteur général.**
  - Il tombe toutes les (période + 1) × 8 DCLK. Au départ, la période vaut
    55, soit 100,4 Hz.
  - À chaque tick, il lève l'interruption 8 et signale un tick à la puce
    audio.

## 4. Puce audio (FMA, `$E03000`)

### Registres

| Index | Lecture | Écriture |
|---|---|---|
| `$00` | commande | bit 15 : demande de données (efface le manque) ; bit 0 : arrêt et vidage |
| `$01` | `$0200` | état |
| `$02` | `$0007` | |
| `$03` | `$0900` | |
| `$04` | numéro de flux | numéro de flux, signale le changement au prochain décodage |
| `$05` | numéro de flux | |
| `$06` | vecteur | vecteur |
| `$07` | `$0042` | |
| `$08` | DCLK >> 16 ; garde les 16 bits faibles | |
| `$09` | les 16 bits faibles gardés | |
| `$0A`/`$0B` | dernier en-tête de trame MP2 reçu | |
| `$0C` | 1 pendant la lecture | |
| `$0D` | interruptions en attente, remises à zéro par la lecture | |
| `$0E` | masque d'interruptions | idem |
| `$11` | | registre de l'interface hôte du DSP56001 |
| `$12` | `$0004` (HF2) | données vers le DSP |

### Interruptions et état

| Bit | Signification |
|---|---|
| 0 | fin de programme |
| 1 | changement de flux pris en compte |
| 2 | nouvelles trames décodées |
| 3 | manque de données |
| 4 | décodage commencé |
| 8 | tick du minuteur vidéo |

### Fonctionnement

- **Départ de la lecture.** La première PTS reçue fixe l'instant de départ :
  DCLK au moment de la réception + (PTS − SCR) / 2.
- **Toutes les 10 ms :**
  - départ si l'instant est passé ;
  - décodage des trames en attente, en gardant au plus 1/4 s d'échantillons
    décodés ;
  - envoi de 10 ms de son, soit 378 échantillons à 37,8 kHz, interpolés
    linéairement depuis la fréquence de la trame.
- **Manque de données.** Il est déclaré pendant la lecture quand il reste
  moins de 1 024 octets de flux et :
  - qu'il ne reste aucun échantillon décodé ;
  - ou qu'il n'en reste qu'un, sans nouvelles données depuis 0,4 s.

  La lecture s'arrête alors et l'échantillon restant est jeté.
- **Surplus.** Après une fin de programme, si plus de 16 Ko attendent déjà, le
  programme suivant est jeté en entier. Le flux en attente est limité à
  64 Ko.
- **Atténuateurs.** La commande hôte `$13` du DSP est suivie de 4 octets :
  gauche→gauche, gauche→droite, droite→gauche, droite→droite, en dB, `$80`
  pour coupé. Au départ : stéréo sans atténuation.
