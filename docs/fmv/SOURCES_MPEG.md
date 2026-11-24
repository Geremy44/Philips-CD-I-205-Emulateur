# Sources des décodeurs MPEG

Les deux décodeurs de la cartouche Digital Video ont été écrits pour
l'émulateur à partir des normes, sans reprendre le code d'un autre décodeur :

- `src/cdi_emu/core/FMV/mpeg1_video.c` : images MPEG-1 ;
- `src/cdi_emu/core/FMV/mp2_audio.c` : son MPEG-1 Layer II (MP2).

Ce document liste les références utilisées.

## Normes

- **ISO/IEC 11172-2 (MPEG-1 vidéo).** Elle donne :
  - la syntaxe des couches séquence, image, tranche, macrobloc et bloc ;
  - les tables de codes de longueur variable (annexe B) ;
  - la déquantification et les vecteurs de mouvement ;
  - la prédiction au demi-pixel.

  La transformée inverse (IDCT) est calculée directement à partir de sa
  définition.
- **ISO/IEC 11172-3 (MPEG-1 audio).** Elle donne :
  - l'en-tête de trame ;
  - les tables d'allocation de bits 3-B.2a à 3-B.2d ;
  - la sélection des facteurs d'échelle ;
  - le groupement des échantillons et la requantification ;
  - le filtre de synthèse polyphase.

  Les facteurs d'échelle (2^(1 − i/3)) et les valeurs requantifiées
  ((2·code − (pas − 1)) / pas) découlent de leur définition et sont calculés.

Les normes ISO sont payantes et ne sont donc pas jointes ici.

## Documents de référence

Ces deux documents ne sont pas de nous. Une copie locale est rangée dans
ce dossier, mais elle est exclue de git (`.gitignore`) et n'est donc pas
diffusée avec le dépôt. Pour les obtenir, utiliser les liens ci-dessous.

### `MPEG-Layer3_Bitstream_Syntax_and_Decoding_Fraunhofer_1997.pdf`

« MPEG-Layer3 Bitstream Syntax and Decoding », Martin Sieler et Ralph
Sperschneider (Fraunhofer IIS), édition 1.2 du 4 mars 1997.
Source : <https://mp3guessenc.sourceforge.io/MPEG%20Layer3%20Bitstream%20Syntax%20and%20Decoding.pdf>

**Sert à :** la fenêtre de synthèse D[0..511] de `mp2_audio.c`. C'est la
table 3-B.3 de la norme ISO 11172-3, reproduite dans ce document
(section 4.3.12, filtre de synthèse). Le filtre est le même en Layer II et
en Layer III.

**Contrôles faits à la recopie :**
- les 512 valeurs sont toutes des multiples exacts de 1/65536, ce qui
  explique pourquoi la table du code est stockée en entiers ;
- elles respectent toutes la symétrie de la norme : D[i] = −D[512 − i]
  quand i n'est pas un multiple de 64, et D[i] = D[512 − i] sinon ;
- passé dans le filtre d'analysis de la norme, puis dans notre synthèse, un
  signal ressort avec un gain de 1,0000 et un écart d'environ −97 dB. Le
  retard est de 481 échantillons, comme prévu.

### `Koornwinder_How_were_the_MP3_filter_coefficients_produced.pdf`

« How were the MP3 filter coefficients produced? », Tom H. Koornwinder
(Institut KdV, université d'Amsterdam), version du 19 février 2007.
Source : <https://staff.fnwi.uva.nl/t.h.koornwinder/art/misc/hemker.pdf>

**Sert à :** les propriétés de la fenêtre. Ce document ne contient pas les
valeurs, mais il établit trois relations :
- la fenêtre de synthèse vaut 32 fois la fenêtre d'analysis (D[i] = 32·C[i]) ;
- elle respecte la symétrie citée plus haut ;
- C[0] = C[512] = 0.

Ces relations ont servi à vérifier la table recopiée et à écrire le test
d'analysis puis synthèse.

## Autres références de la cartouche

- `Digital-Video-Cartridge-SM.pdf` : manuel de service de la cartouche
  (puces, cartes mémoire et d'entrées-sorties).
- `FMV_EMULATION_NOTES.md` : notes d'émulation de la cartouche.
