# tools/

Outils d'analyse de la ROM du CD-i 205. **À conserver : ne pas supprimer ce
dossier lors d'un ménage** (il sert à chaque enquête sur un pilote OS-9).

- `banc_regression.py` : banc de non-régression. Lance chaque jeu de
  `CDI-205/Games` sans fenêtre (NVRAM de départ, horloge figée au
  1er janvier 2026, clic sur PLAY), garde une image
  toutes les 2000 frames et l'empreinte du son, et compare à une référence.
  `python tools/banc_regression.py --reference` après une version validée,
  puis `python tools/banc_regression.py` après chaque modification
  (`--jeux inca,hotel` pour quelques jeux, `-j` pour le parallélisme).
  Références et différences dans `CDI-205/Regression/` (hors git : images
  de jeux). L'émulation est déterministe : une différence vient du code.
  « plantage » : le 68070 s'est arrêté (instruction invalide).
- `inca_codes.py` : codes de reprise d'Inca (CD-i), calculés avec l'algorithme
  du jeu (`cdi_menu.mdo`, routine `$16D2`). `python tools/inca_codes.py`
  liste un code par niveau ; `--vies N`, `--tous NIVEAU`, `--decode CODE`.
- `m68k_disasm.py` : petit désassembleur 68000/68070 (appels OS-9 nommés).
  Exemple : `python tools/m68k_disasm.py CDI-205/ROMs/CDI205-00_PS-7211_REL.2.1.bin 1B09E4 +200 --base 0x180000 --swapped`
