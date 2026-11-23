# tools/

Outils d'analyse de la ROM du CD-i 205. **À conserver : ne pas supprimer ce
dossier lors d'un ménage** (il sert à chaque enquête sur un pilote OS-9).

- `inca_codes.py` : codes de reprise d'Inca (CD-i), calculés avec l'algorithme
  du jeu (`cdi_menu.mdo`, routine `$16D2`). `python tools/inca_codes.py`
  liste un code par niveau ; `--vies N`, `--tous NIVEAU`, `--decode CODE`.
- `m68k_disasm.py` : petit désassembleur 68000/68070 (appels OS-9 nommés).
  Exemple : `python tools/m68k_disasm.py CDI-205/ROMs/CDI205-00_PS-7211_REL.2.1.bin 1B09E4 +200 --base 0x180000 --swapped`
