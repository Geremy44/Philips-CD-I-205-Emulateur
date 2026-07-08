# FMV_MMIO_MAP — Cartographie MMIO de la Digital Video Cartridge

> 📂 Ce document fait partie de `docs/fmv/` — axe de recherche CARTOUCHE FMV.
> ⚠️ Ce MMIO est **uniquement accessible** quand la cartouche FMV est enfichée. En son absence, `$E80000–$EFFFFF` renvoie probablement à des valeurs non définies ou à un bus flottant.

---

## 1. Vue d'ensemble de l'espace FMV

```
$E80000 ┬─ Registres décodeur MPEG ($E80000–$E8FFFF)
│        │  REG $00–$FF (organisés par fonction)
│        │  Registres vidéo ($00–$1F), audio ($20–$3F), config ($40–$FF)
│        │
│        ├─ MMIO handshake CPU ↔ chip MPEG ($E8C000–$E8C0FF)
│        │  $C0 : contrôle + IRQ ack
│        │  $C2 : statut lecture
│        │  $CA : configuration
│        │
│        └─ Zone registre additionnels ($E8D000–$E8FFFF)
│
$E90000 ┬─ RAM vidéo dédiée (buffer decoded frames)
$EFFFFF ┘
```

> ⚠️ La plage `$E80000–$EFFFFF` est **exclusivement réservée à la cartouche FMV**. La console (sans cartouche) ne doit jamais y accéder.

---

## 2. MMIO Handshake CPU ($E8C000–$E8C0FF)

Registres critiques pour la communication CPU 68070 ↔ chip MPEG-1.

### 2.1 Registres principaux

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| `CTRL_C0` | `$E8C000` | 8 | **Registre de contrôle + IRQ ACK** |
| `CTRL_C1` | `$E8C001` | 8 | Complémentaire du contrôle |
| `STAT_C2` | `$E8C002` | 8 | Registre de statut (lecture) |
| `CTRL_C3` | `$E8C003` | 8 | Contrôle additionnel |
| `CTRL_C4` | `$E8C004` | 8 | Contrôle additionnel |
| `CTRL_C5` | `$E8C005` | 8 | Contrôle additionnel |
| `STAT_C6` | `$E8C006` | 8 | Statut additionnel |
| `STAT_C7` | `$E8C007` | 8 | Statut additionnel |
| `CTRL_C8` | `$E8C008` | 8 | Contrôle additionnel |
| `CTRL_C9` | `$E8C009` | 8 | Contrôle additionnel |
| `CFG_CA` | `$E8C00A` | 8 | **Configuration chip MPEG** |

### 2.2 Signification des bits de $C0

| Bit | Valeur hex | Rôle | Sens |
|-----|-----------|------|------|
| bit 0 | `0x01` | Command bit 0 | W |
| bit 1 | `0x02` | Command bit 1 | W |
| bit 2 | `0x04` | Command bit 2 | W |
| bit 3 | `0x08` | Command bit 3 | W |
| **bit 4** | `**0x10**` | **IRQ acknowledge** | **W** ⭐ |
| bit 5 | `0x20` | Flag | R/W |
| bit 6 | `0x40` | Flag | R/W |
| bit 7 | `0x80` | Enable | R/W |

> ⭐ **Acknowledge IRQ** : l'IRQ de la cartouche FMV est acquittée en écrivant `0x10` dans `$C0` (bit 4 mis). Voir routine `@ $022E` dans `fmvdrv` :
> ```assembly
> BSET  #$04, $C0   ; equivalent à write $C0 | $10
> RTE
> ```

---

## 3. Registres du décodeur MPEG ($E80000–$E8FFFF)

### 3.1 Registres vidéo ($E80000–$E801FF)

REG $00–$1F : configuration du décodeur vidéo (resolution, timing, mode).

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| `REG $00` | `$E80000` | 8 | **Reset décodeur vidéo** (valeur init `0x00`) |
| `REG $02` | `$E80002` | 8 | **STROBE** — calage des valeurs |
| `REG $03` | `$E80003` | 8 | Mode vidéo |
| `REG $04` | `$E80004` | 8 | Configuration |
| `REG $05` | `$E80005` | 8 | Configuration |
| `REG $06` | `$E80006` | 8 | Configuration |
| `REG $07` | `$E80007` | 8 | Configuration |
| `REG $10` | `$E80100` | 8 | Registre vidéo $10 |
| `REG $11` | `$E80101` | 8 | Registre vidéo $11 |
| `REG $20` | `$E80200` | 8 | Registre vidéo $20 |
| `REG $21` | `$E80201` | 8 | Registre vidéo $21 |
| `REG $30` | `$E80300` | 8 | Registre vidéo $30 |
| `REG $31` | `$E80301` | 8 | Registre vidéo $31 |

> ⭐ **STROBE** — Le REG $02 est le **latch / strobe**. Après toute écriture dans un registre vidéo ou audio, une écriture dans REG $02 = `0x??` (valeur strobe) déclenche le **latching** des valeurs dans le décodeur MPEG. Toute implémentation d'émulation doit respecter cette séquence.

### 3.2 Registres audio ($E80400–$E807FF)

REG $20–$3F : configuration du décodeur audio MPEG.

| Registre | Adresse | Bits | Description |
|---------|---------|------|-------------|
| `REG $40` | `$E80400` | 8 | **Reset décodeur audio** (valeur init `0x00`) |
| `REG $41` | `$E80401` | 8 | Registre audio $41 |
| `REG $50` | `$E80500` | 8 | Registre audio $50 |
| `REG $51` | `$E80501` | 8 | Registre audio $51 |
| `REG $60` | `$E80600` | 8 | Registre audio $60 |
| `REG $61` | `$E80601` | 8 | Registre audio $61 |
| `REG $70` | `$E80700` | 8 | Registre audio $70 |
| `REG $71` | `$E80701` | 8 | Registre audio $71 |

> 🔲 **À extraire** : les valeurs exactes de chaque registre audio issues de la table `$1EE0–$1FF0` de `fmvll`.

### 3.3 Registres de configuration ($E80800–$E8FFFF)

| Registre | Adresse | Description |
|---------|---------|-------------|
| `REG $80–$FF` | `$E80800–$E8FFFF` | Configuration additionnelle |

> 🔲 **À extraire** : contenu exact des registres $80–$FF.

---

## 4. Tableau consolidé — tous registres

| Plage | Zone | État |
|-------|------|------|
| `$E80000` | REG $00 (reset vidéo) | ✅ `0x00` (table $1EE0) |
| `$E80002` | REG $02 (STROBE) | ✅ Identifié — latch critique |
| `$E80003–$E80007` | REG $03–$07 | ✅ Dans table init |
| `$E80100–$E80101` | REG $10–$11 | ✅ Dans table init |
| `$E80200–$E80201` | REG $20–$21 | ✅ Dans table init |
| `$E80300–$E80301` | REG $30–$31 | ✅ Dans table init |
| `$E80400–$E80401` | REG $40–$41 (reset audio + $41) | ✅ Dans table init |
| `$E80500–$E80501` | REG $50–$51 | ✅ Dans table init |
| `$E80600–$E80601` | REG $60–$61 | ✅ Dans table init |
| `$E80700–$E80701` | REG $70–$71 | ✅ Dans table init |
| `$E8C000` | MMIO $C0 (contrôle + IRQ ack) | ✅ Bit 4 = ack |
| `$E8C001` | MMIO $C1 | ✅ Dans table init |
| `$E8C002` | MMIO $C2 (statut) | ✅ Identifié |
| `$E8C00A` | MMIO $CA (configuration) | ✅ Identifié |
| `$E80800–$E8FFFF` | REG $80–$FF | 🔲 À extraire |
| `$E90000–$EFFFFF` | RAM vidéo MPEG | 🔲 À cartographier |

---

## 5. Codes de contrôle SetStat (fmvdrv → fmvll)

Ces 24 codes sont appelés via `SetStat` sur `/nvr/csd` :

| Code | Mnémo | Description |
|------|-------|-------------|
| `$0100` | FMV_INIT | Initialisation décodeur MPEG |
| `$0101` | FMV_RESET | Reset du décodeur |
| `$0102` | FMV_PLAY | Démarrage lecture vidéo |
| `$0103` | FMV_STOP | Arrêt lecture |
| `$0104` | FMV_PAUSE | Pause |
| `$0105` | FMV_SEEK | Positionnement |
| `$0106` | FMV_AUDIO_CHANNEL | Sélection canal audio |
| `$0107` | FMV_VIDEO_CHANNEL | Sélection canal vidéo |
| `$0108` | FMV_SET_BRIGHTNESS | Luminosité |
| `$0109` | FMV_SET_CONTRAST | Contraste |
| `$010A` | FMV_SET_VOLUME | Volume audio |
| `$010B` | FMV_SET_BITRATE | Débit MPEG |
| `$010C` | FMV_BUFFER_INFO | Info buffers |
| `$010D` | FMV_SYNC_MODE | Mode synchronisation |
| `$010E` | FMV_STEP | Avance image par image |
| `$010F` | FMV_STILL_FRAME | Affichage image fixe |
| `$0110` | FMV_STREAM_SELECT | Sélection flux |
| `$0111` | FMV_ERROR_STATUS | Statut erreur |
| `$0112` | FMV_EOF_MARKER | Fin de flux |
| `$0113` | FMV_BOF_MARKER | Début de flux |
| `$0114` | FMV_BUFFER_THRESHOLD | Seuil buffer |
| `$0115` | FMV_FRAME_RATE | Cadence images |
| `$0116` | FMV_RESOLUTION | Résolution |
| `$0117` | FMV_??? | Command 23 (inconnu) |

> 🔲 **Correspondance à vérifier** : les mnémoniques ci-dessus sont des propositions basées sur le contexte CD-i MPEG. Une correlates avec les docs STi 5500 ou les sources MAME serait bienvenue.

---

## 6. Flux IRQ FMV

```
Chip MPEG-1 (hardware)
  │
  │ IRQ Hardware (niveau ?)
  ▼
Routine fmvdrv @ $022E
  │
  │ BSET #$04, $C0    ← acknowledge IRQ
  │ RTE
  ▼
CPU 68070 (traitement IRQ)
  │
  │ Traitement haut niveau (dans le contexte OS-9)
  ▼
fmvll / fmvdrv (callbacks)
  │
  │ Réveil thread attente
  ▼
Application (données vidéo disponibles)
```

| Signal | Source | Destination | Acknowledge |
|--------|--------|-------------|-------------|
| IRQ FMV | Chip MPEG | 68070 → OS-9 | Write `$C0 = $10` (bit 4) |

---

## 7. Résumé — Implémentation stub MMIO

```
Structure à implémenter pour émuler la cartouche FMV :

class FMV_Cartridge:
    def __init__(self):
        # Registres décodeur MPEG
        self.regs = bytearray(256)        # REG $00-$FF

        # MMIO handshake
        self.mmio_C0 = 0x00               # contrôle / IRQ ack
        self.mmio_C1 = 0x00
        self.mmio_C2 = 0x00               # statut (lecture)
        self.mmio_CA = 0x00               # configuration

        # Interrupt state
        self.irq_pending = False
        self.irq_vector = None

    def write_byte(self, addr, value):
        if $E80000 <= addr < $E90000:
            self._write_reg(addr, value)    # REG $00-$FF
        elif addr == $E8C000:
            self._write_ctrl_C0(value)      # ack IRQ (bit 4)
        elif addr == $E8C002:
            self.mmio_C2 = value
        elif addr == $E8C00A:
            self.mmio_CA = value

    def _write_reg(self, addr, value):
        reg = addr & 0xFF
        self.regs[reg] = value
        if reg == 0x02:
            self._do_strobe()               # ⭐ STROBE latch

    def _do_strobe(self):
        # Capture les valeurs écrites dans les autres registres
        # et les applique au décodeur MPEG simulé
        pass
```

---

*Document axe FMV — dernière mise à jour : 21 juin 2026*
