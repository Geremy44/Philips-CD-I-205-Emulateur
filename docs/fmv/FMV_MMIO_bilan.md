# Bilan rétro-ingénierie — FMV (Digital Video Cartridge) CD-i 205

## 1. Contexte

- **ROM combinée** : `FMV_combined_7308hi_7307lo.bin`, taille **131 072 octets** (0x20000)
- **Checksums observés** :
  - 8-bit : `0x82`
  - 16-bit big-endian : `0x24A9`
  - 16-bit little-endian : `0xA8D9`
  - **attendu : `0x4B82` → DIFFÉRENT** *(réalignement entrelacement à revoir)*
- **Vecteurs 68000** :
  - SSP (0x00) = `0x4AFC0001`
  - PC (0x04) = `0x000001E6`
- **Modules analysés** :
  - `fmvdrv.bin` — 12 756 octets *(driver / interface OS-9 + glue)*
  - `fmvll.bin` — 23 042 octets *(pilote bas niveau)*

---

## 2. Architecture en 2 couches

| Module | Rôle | Registres utilisés |
|---|---|---|
| `fmvll.bin` (low-level) | Pilote matériel direct — cœur vidéo/MPEG + audio | `$E01000`, `$E030xx`, `$E040xx`, `$E041FE`, `$E06100` |
| `fmvdrv.bin` (driver) | Interface OS-9 + glue logic / contrôleur cartouche | `$E000CA`, `$E00680`, `$E00C01`, `$E06600`, `$E06700`, `$E06710` |

---

## 3. Mécanisme d'indirection (fmvll.bin @0x034E)

Le low-level construit une **structure de contexte** pointée par le registre A6, puis accède aux registres hardware via **indirection mémoire**. Ce mécanisme masquait les vraies adresses lors des recherches de motif `2039xxxx`.

Bloc assembleur :

```
0x034E:  2D7C 00E03022 8026   MOVE.L #$00E03022, $26(A6)   ; ptr registre FMV #1
0x0356:  2D7C 00E03024 802A   MOVE.L #$00E03024, $2A(A6)   ; ptr registre FMV #2
0x035E:  6100 35C0            BSR     $00003C20             ; appel sous-routine
0x0362:  7201                 MOVEQ   #1, D1                 ; charge 1
0x0364:  B280                 CMP.L   D0, D1                ; compare
0x0366:  6600 0056            BNE     $00003CBE             ; branchement conditionnel
...
0x038C:  2D40 8022            MOVE.L  D0, $22(A6)            ; stocke résultat/status
```

**Structure de contexte (A6) :**

| Offset (A6) | Contenu | Sens |
|---|---|---|
| `$22(A6)` | résultat / status | data retour |
| `$26(A6)` | `$00E03022` | pointeur registre FMV #1 |
| `$2A(A6)` | `$00E03024` | pointeur registre FMV #2 |

**Insight** : Le pattern `MOVE.L #$00E0xxxx, addr(A6)` charge l'adresse hardware du registre DANS le contexte, puis le code fait `MOVE (A6+offset)` → A_x → accède `(A_x)` = accès réel au registre `$E03022`. Double-indirection.

---

## 4. Registres trouvés — fmvll.bin

*Scan de toute occurrence `$00E0xxxx` en immédiat 32 bits (pattern `2039` + `13C0`/`33C0` absolu)*

| Adresse | Occurrences | Notes |
|---|---|---|
| `$00E01000` | ×6 | **Hot path** — base très sollicitée, probable cœur décodeur MPEG |
| `$00E03000` | ×1 | Bloc contrôle vidéo/MPEG |
| `$00E03004` | ×1 | Status / data |
| `$00E03010` | ×1 | Contrôle complémentaire |
| `$00E03018` | ×1 | Contrôle complémentaire |
| `$00E03022` | ×2 | Registre contexte A6 (ptr #1) |
| `$00E03024` | ×3 | Registre contexte A6 (ptr #2) — le plus accédé |
| `$00E04000` | ×5 | Bloc audio/système — très actif |
| `$00E04064` | ×1 | Registre audio secondaire |
| `$00E040A6` | ×1 | Registre audio tertiaire |
| `$00E041FE` | ×1 | Fin de bloc B (audio ?) |
| `$00E06100` | ×1 | Pont vers couche driver (bus interface) |

**Total : 12 adresses uniques**

---

## 5. Registres trouvés — fmvdrv.bin

*Scan `$00E0xxxx` en immédiat (tous patterns)*

| Adresse | Occurrences | Notes |
|---|---|---|
| `$00E000CA` | ×1 | Registre carte (initialisation) |
| `$00E00680` | ×1 | Registre glue logic |
| `$00E00C01` | ×1 | Flag/octet — adresse impaire (bit control) |
| `$00E06600` | ×1 | Contrôleur cartouche |
| `$00E06700` | ×2 | **Interface bus** — le plus accédé de ce module |
| `$00E06710` | ×1 | Registre interface bus secondaire |

**Total : 6 adresses uniques**

---

## 6. Régions MMIO consolidées

| Région | Plage | Rôle probable | Couche |
|---|---|---|---|
| E000–E00C | `$E00000`–`$E00FFF` | Registres carte / initialisation | `drv` |
| E010 | `$E01000`+ | **Cœur décodeur MPEG** (chemin chaud) | `ll` |
| E030–E041 | `$E03000`–`$E041FF` | Vidéo MPEG + audio | `ll` |
| E060–E067 | `$E06000`–`$E067FF` | Interface bus / contrôleur cartouche | `drv` + `ll` |

---

## 7. Squelette handler MMIO (C)

```c
#include <stdint.h>

/* FMV Digital Video Cartridge — MMIO Handler */

/* Registres low-level (fmvll.bin) */

static inline uint32_t fmv_decoder_read(uint8_t reg) {
    switch (reg) {
        case 0x00: return 0;  // TODO
        case 0x18: return 0;  // TODO
        default: return 0;
    }
}

static inline uint32_t fmv_video_read(uint8_t reg) {
    switch (reg) {
        case 0x00: return 0;  // MPEG video control
        case 0x04: return 0;  // status / data
        case 0x10: return 0;  // TODO
        default: return 0;
    }
}

static inline uint32_t fmv_audio_read(uint8_t reg) {
    switch (reg) {
        case 0x00: return 0;  // audio / system
        case 0x64: return 0;  // TODO
        case 0xA6: return 0;  // TODO
        case 0xFE: return 0;  // end of block B
        default: return 0;
    }
}

/* Registres driver (fmvdrv.bin) */

static inline uint32_t fmv_card_read(uint16_t reg) {
    switch (reg) {
        case 0x0CA: return 0;  // card register
        case 0x680: return 0;  // glue logic
        case 0xC01: return 0;  // flag (odd addr)
        default: return 0;
    }
}

static inline uint32_t fmv_bus_read(uint16_t reg) {
    switch (reg) {
        case 0x600: return 0;  // cartridge controller
        case 0x700: return 0;  // bus interface
        case 0x710: return 0;  // bus secondary
        default: return 0;
    }
}

/* Dispatcher principal */

uint32_t fmv_mmio_read(uint32_t addr) {
    switch (addr & 0xFFFF00) {
        case 0xE01000: return fmv_decoder_read(addr & 0xFF);   // hot path
        case 0xE03000: return fmv_video_read(addr & 0xFF);
        case 0xE04000: return fmv_audio_read(addr & 0xFF);
        case 0xE06000:
        case 0xE06100:
        case 0xE06600:
        case 0xE06700: return fmv_bus_read(addr & 0xFFF);
        case 0xE00000: return fmv_card_read(addr & 0xFFF);
        default:       return 0;                              // unmapped
    }
}

void fmv_mmio_write(uint32_t addr, uint32_t val) {
    // Miroir du reader pour chaque région
    // TODO: implémenter WRITE pour chaque registre
}
```

---

## 8. Prochaines étapes

1. **Classifier chaque registre** en `READ` / `WRITE` / `RW` — analyser le contexte de chaque instruction `MOVE` :
   - `MOVE Dn, addr` → **WRITE** (écriture vers le registre)
   - `MOVE addr, Dn` → **READ** (lecture du registre)
   - `MOVE addr1, addr2` → possibly **R/W** (transfert inter-reg)

2. **Revoir le réalignement d'entrelacement hi/lo** — checksum 16-bit big-endian `0x24A9` ne correspond pas à l'attendu `0x4B82`. Vérifier l'ordre de fusion des blocs hi/lo dans la ROM combinée.

3. **Implémenter les 4 régions MMIO** dans l'émulateur (cadre C ci-dessus) et commencer par le **hot path `$E01000`** (le plus sollicité, ×6 occurrences).

4. **Dump dynamique** : ajouter des traces d'accès dans l'émulateur pour capturer les valeurs concrètes écrites/lues pendant l'exécution d'un vrai CD-i FMV.

---

*Bilan généré depuis les scans successifs de `fmvll.bin` et `fmvdrv.bin` via `extract_fmv_regs.py` et `extract_fmv_any.py`.*
