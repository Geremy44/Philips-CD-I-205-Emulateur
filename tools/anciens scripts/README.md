# tools/ — Scripts d'analyse pour l'émulateur CD-i 205

> Ces outils servent les **deux axes de recherche** : CONSOLE et CARTOUCHE FMV.

---

## Outils disponibles

| Script | Axe | Description |
|--------|-----|-------------|
| `check_FMV_entrelacement.py` | FMV | Désentrelace le dump brut de la cartouche FMV |
| `os9_module_parser.py` | CONSOLE + FMV | Parse les en-têtes de modules OS-9/68000 dans un dump binaire |

---

## 1. `check_FMV_entrelacement.py`

### Problème

Le dump brut de la cartouche FMV obtenu via le logicpack est **entrelacé** : les lignes paires et impaires du signal analogique sont inversées. Le fichier brut est donc inexploitable tel quel.

### Principe

Le script **désentrelace** le dump en reconstruisant l'image correcte à partir des deux moitiés entrelacées. Il détecte aussi les **inversions** (certains blocs sont inversés verticalement) et corrige automatiquement.

### Usage

```bash
# Utilisation basique
python tools/check_FMV_entrelacement.py --input dump_brut.bin --output dump_desentrelace.bin

# Mode verbose (affiche les détails du traitement)
python tools/check_FMV_entrelacement.py --input dump_brut.bin --output dump_desentrelace.bin -v

# Détection automatique de l'inversion (par défaut)
python tools/check_FMV_entrelacement.py --input dump_brut.bin --output dump_desentrelace.bin --detect-invert

# Forcer une inversion (si la détection échoue)
python tools/check_FMV_entrelacement.py --input dump_brut.bin --output dump_desentrelace.bin --force-invert

# Spécifier les dimensions (si différent du défaut)
python tools/check_FMV_entrelacement.py --input dump_brut.bin --output dump_desentrelace.bin --width 640 --height 480
```

### Options

| Option | Description | Défaut |
|--------|-------------|--------|
| `--input`, `-i` | Fichier dump brut d'entrée | *requis* |
| `--output`, `-o` | Fichier désentrelacé de sortie | *requis* |
| `-v`, `--verbose` | Sortie détaillée | `False` |
| `--detect-invert` | Détection automatique inversion | `True` |
| `--force-invert` | Forcer l'inversion | `False` |
| `--width` | Largeur en pixels | `640` |
| `--height` | Hauteur en pixels | `480` |
| `--offset` | Offset de début dans le dump | `0` |

### Exemple de sortie

```
$ python tools/check_FMV_entrelacement.py -i fmv_dump.bin -o fmv_dump_fixed.bin -v

[check_FMV] Entrelacement détecté : lignes paires/impaires inversées
[check_FMV] Inversion detectée : bloc 0 inversé (OK)
[check_FMV] Désentrelacement terminé.
[check_FMV] Bloc 0 : OK  (1280000 octets traités)
[check_FMV] Fichier de sortie : fmv_dump_fixed.bin (2560000 octets)
```

> 💡 **Tip** : après désentrelacement, utilisez le résultat avec `os9_module_parser.py` pour extraire les modules OS-9 de la cartouche FMV.

---

## 2. `os9_module_parser.py`

### Problème

Les ROMs CD-RTOS et les dumps de cartouche FMV contiennent des **modules OS-9/68000** compilés. Sans knowing their format, ils sont illisibles. Ce script parse automatiquement les en-têtes OS-9 dans un fichier binaire.

### Principe

Le parseur scanne le fichier à la recherche de **sync words** OS-9 (`$4AFC53C3`) puis décode l'en-tête de chaque module trouvé :

- Nom du module (encodé, terminé par `0x80`)
- Type (Module, Device, Driver, System)
- Langage (C, Pascal, BASIC…)
- Adresse d'initialisation (`init`)
- Adresse de terminaison (`term`)
- Taille totale

### Usage

```bash
# Parsing basique d'un dump
python tools/os9_module_parser.py --input mon_dump.bin

# Sortie dans un fichier (JSON + texte)
python tools/os9_module_parser.py --input mon_dump.bin --output resultat_parse.txt --format both

# Format JSON (pour processing automatisé)
python tools/os9_module_parser.py --input mon_dump.bin --output resultat.json --format json

# Mode verbose (affiche aussi le code hexadécimal des en-têtes)
python tools/os9_module_parser.py --input mon_dump.bin -v

# Scan d'une plage d'offset précise (gros fichiers)
python tools/os9_module_parser.py --input mon_dump.bin --offset 0x80000 --length 0x10000

# Spécifier la taille du fichier (si header non fiable)
python tools/os9_module_parser.py --input mon_dump.bin --file-size 2097152

# Extraire les modules individuellement (fichiers séparés)
python tools/os9_module_parser.py --input mon_dump.bin --extract-dir ./modules_extraits/
```

### Options

| Option | Description | Défaut |
|--------|-------------|--------|
| `--input`, `-i` | Fichier binaire d'entrée | *requis* |
| `--output`, `-o` | Fichier de sortie | stdout |
| `--format` | Format de sortie : `text`, `json`, `both` | `text` |
| `-v`, `--verbose` | Sortie détaillée | `False` |
| `--offset` | Offset de départ (hex ou décimal) | `0` |
| `--length` | Nombre d'octets à scanner | `taille fichier` |
| `--file-size` | Taille totale du fichier | *détectée* |
| `--extract-dir` | Dossier d'extraction des modules | *aucune* |

### Exemple de sortie (texte)

```
$ python tools/os9_module_parser.py --input fmv_dump_fixed.bin

=== OS-9 Module Parser ===
Fichier : fmv_dump_fixed.bin (2097152 octets)
Sync word : 0x4AFC53C3 (OS-9/68000)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Module #1
  Nom         : fmvll
  Type        : Device Driver (0x06)
  Langage     : C (0x01)
  Attributs   : ReEntrant (0x01)
  Taille      : 1234 octets
  Init @      : $0D2A
  Term @      : $0000
  Édition     : 1
  CRC         : 0xABCD
  Offset dump : $0001_0000
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Module #2
  Nom         : fmvdrv
  Type        : Device Driver (0x06)
  Langage     : C (0x01)
  Attributs   : Global (0x80)
  Taille      : 5678 octets
  Init @      : $0CA8
  Term @      : $0FFF
  Édition     : 1
  CRC         : 0x1234
  Offset dump : $0001_2000
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  ... (total : 15 modules trouvés)
```

### Exemple de sortie (JSON)

```json
{
  "filename": "fmv_dump_fixed.bin",
  "filesize": 2097152,
  "sync_word": "0x4AFC53C3",
  "modules": [
    {
      "index": 1,
      "name": "fmvll",
      "type": "Device",
      "language": "C",
      "attributes": ["ReEntrant"],
      "size": 1234,
      "init_offset": "0x0D2A",
      "term_offset": "0x0000",
      "edition": 1,
      "crc": "0xABCD",
      "header_offset": 65536,
      "body_offset": 65568
    }
  ]
}
```

### Format d'en-tête OS-9 reconnu

```
Offset  Contenu              Type
────────────────────────────────────
$00     $4AFC53C3           magic sync
$04     [name]              ASCII + terminateur 0x80
$xx     header body
  $00   type                1 octet
  $01   language            1 octet
  $02   attributes          1 octet
  $03   revision           1 octet
  $04   parity              3 octets
  $07   size                3 octets
  $0A   init                3 octets
  $0D   term                3 octets
  $10   memtype             3 octets
  $13   edition             1 octet
```

---

## 3. Installation & Prérequis

```bash
# Python 3.8+
python --version   # >= 3.8

# Dépendances (Aucune — scripts standards)
# uniquement la bibliothèque standard Python

# Vérification
cd tools
python os9_module_parser.py --help
python check_FMV_entrelacement.py --help
```

---

## 4. Flux de travail recommandé

```
1. Dump brut de la cartouche FMV
          │
          ▼
2. [check_FMV_entrelacement.py] — désentrelacement
          │
          ▼
3. Dump désentrelacé de la cartouche FMV
          │
          ▼
4. [os9_module_parser.py] — extraction des modules OS-9
          │
          ▼
5. Analyse manuelle des modules (fmvll, fmvdrv…)
          │
          ▼
6. Rédaction FMV_OS9_FINDINGS.md + FMV_MMIO_MAP.md
          │
          ▼
7. Développement stub émulation FMV (src/)
```

---

*Dernière mise à jour : 21 juin 2026*
