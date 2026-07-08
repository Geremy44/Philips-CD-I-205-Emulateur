# CONSOLE_MMIO_MAP — Cartographie MMIO de la console CD-i 205

> 📂 Ce document fait partie de `docs/console/` — axe de recherche CONSOLE.
> ⚠️ **Rappel FMV** : la plage `$E80000–$EFFFFF` est **réservée à la cartouche FMV** et ne fait pas partie de ce MMIO console. Voir [`docs/fmv/FMV_MMIO_MAP.md`](../fmv/FMV_MMIO_MAP.md).

---

## 1. Structure générale du MMIO console

L'espace d'adressage `$E00000–$E7FFFF` (512 Ko) est dédié aux **périphériques intégrés** du 68070 et aux chip externes :

```
$E00000 ┬─ 68070 internes
│        ├─ $E00000 : Timer A
│        ├─ $E02000 : Timer B
│        ├─ $E10000 : UART
│        ├─ $E20000 : Contrôleur I²C
│        └─ $E30000 : Contrôleur DMA
│
├─ $E40000 : MCD212 — contrôleur vidéo
├─ $E50000 : CDIC — contrôleur CD-ROM
├─ $E60000 : SLAVE processor (interne ?)
├─ $E70000 : (?) peripheral inconnu
$E7FFFF ┘
```

> 🔲 = **à confirmer** — les décalages exacts n'ont pas encore été vérifiés par dump ou documentation.

---

## 2. 68070 — Périphériques intégrés

### 2.1 Timer A

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| TMR_A | `$E00000` | 16 | Registre de contrôle timer A |
| TRR_A | `$E00002` | 16 | Registre de référence (reload) |
| TCR_A | `$E00004` | 16 | Compteur timer A |
| TSR_A | `$E00006` | 8 | Registre d'état / flag |

### 2.2 Timer B

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| TMR_B | `$E02000` | 16 | Registre de contrôle timer B |
| TRR_B | `$E02002` | 16 | Registre de référence |
| TCR_B | `$E02004` | 16 | Compteur timer B |
| TSR_B | `$E02006` | 8 | Registre d'état |

### 2.3 UART

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| URCR | `$E10000` | 8 | Registre de contrôle (UART Control Register) |
| URSR | `$E10001` | 8 | Registre d'état (UART Status Register) |
| URDR | `$E10002` | 8 | Registre de données (TX/RX buffer) |
| — | — | — | — |
| — | — | — | — |

### 2.4 Contrôleur I²C

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| I2CR | `$E20000` | 8 | Registre de contrôle I²C |
| I2SR | `$E20001` | 8 | Registre d'état I²C |
| I2DR | `$E20002` | 8 | Registre de données I²C |
| I2AR | `$E20003` | 8 | Registre d'adresse (slave) |

### 2.5 Contrôleur DMA

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| DMACR | `$E30000` | 8 | Registre de contrôle DMA |
| DMAST | `$E30001` | 8 | Registre d'état |
| DMAADL | `$E30002` | 16 | Adresse source/destination (low) |
| DMAADH | `$E30004` | 8 | Adresse (high) |
| DMACNT | `$E30006` | 16 | Compteur de transfert |

---

## 3. MCD212 — Contrôleur vidéo Dual Plane Display

> 🔲 **À extraire** : la plage d'adresses exacte du MCD212 dans `$E40000–$E??????`.

Le MCD212 est controlé via des **registres de configuration** et des **buffers en RAM** :

### 3.1 Registres de configuration (MMIO)

| Registre | Adresse | Description |
|---------|---------|-------------|
| `MCR` (Main Control Register) | 🔲 `$E4????` | Contrôle général |
| `PCR` (Pixel Control Register) | 🔲 `$E4????` | Mode couleur, profondeur |
| `VSR` (Vertical Status Register) | 🔲 `$E4????` | État vidéo vertical |
| `HCFG` (Horizontal Config) | 🔲 `$E4????` | Config horizontale |

### 3.2 BufferRAM vidéo (en RAM système)

| Plage | Type | Description |
|-------|------|-------------|
| `$B00000–$BFFFFF` | 1 Mo | Plan A (foreground) |
| `$C00000–$CFFFFF` | 1 Mo | Plan B (background) |
| `$D00000–$D1FFFF` | 128 Ko | Zone sprites / OAM |

---

## 4. CDIC — Contrôleur CD-ROM Interface

> 🔲 **À extraire** : les registres CDIC exacts.

| Registre | Adresse | Description |
|---------|---------|-------------|
| `CDIC_CMD` | 🔲 `$E5????` | Registre de commande |
| `CDIC_DATA` | 🔲 `$E5????` | Registre de données |
| `CDIC_STATUS` | 🔲 `$E5????` | Registre d'état |
| `CDIC_INT` | 🔲 `$E5????` | Registre d'interruption |

---

## 5. Tableau récapitulatif MMIO console

| Composant | Plage | État | Sources |
|-----------|-------|------|---------|
| Timer A | `$E00000–$E00007` | 🔲 À confirmer | — |
| Timer B | `$E02000–$E02007` | 🔲 À confirmer | — |
| UART | `$E10000–$E10003` | 🔲 À confirmer | — |
| I²C | `$E20000–$E20003` | 🔲 À confirmer | — |
| DMA | `$E30000–$E30007` | 🔲 À confirmer | — |
| MCD212 | `$E40000–$E4????` | 🔲 À extraire | doc externe |
| CDIC | `$E50000–$E5????` | 🔲 À extraire | doc externe |
| SLAVE | `$E60000–$E6????` | 🔲 Inconnu | — |
| Réservé | `$E70000–$E7FFFF` | — | — |
| **FMV CART** | `$E80000–$EFFFFF` | ✅ Doc séparée | `docs/fmv/FMV_MMIO_MAP.md` |

---

## 6. IRQ — Interruptions console

> 🔲 **À extraire** : table de vecteurs IRQ + niveaux de priorité.

| IRQ | Source | Niveau |
|-----|--------|--------|
| 1 | Clavier | — |
| 2 | Timer B | — |
| 3 | UART | — |
| 4 | DMA | — |
| 5 | CDIC (CD-ROM event) | — |
| 6 | MCD212 (vertical blank) | — |
| 7 | SLAVE | — |
| NMI | Reset / error | — |

---

*Document axe CONSOLE — dernière mise à jour : 21 juin 2026*
