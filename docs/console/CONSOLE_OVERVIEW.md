# CONSOLE_OVERVIEW — Philips CD-i 205 : Vue d'ensemble matériel

> 📂 Ce document fait partie de `docs/console/` — axe de recherche CONSOLE.
> Pour la cartouche FMV, voir [`docs/fmv/FMV_OS9_FINDINGS.md`](../fmv/FMV_OS9_FINDINGS.md).

---

## 1. Architecture générale

Le Philips CD-i 205 est un **ordinateur de salon** compact basé sur un SOC Motorola. Il ne contient **aucun disque dur** : toutes les données sont lues depuis le **CD-ROM intégré** via le lecteur laser.

```
┌─────────────────────────────────────────────┐
│              PHILIPS CD-i 205               │
│                                             │
│  ┌──────────┐    ┌──────────────────────┐  │
│  │ CD-ROM   │───▶│  CDIC (CD Interface  │  │
│  │  (MPR)   │    │   Controller)        │  │
│  └──────────┘    └──────────┬───────────┘  │
│                            │ audio CD      │
│  ┌──────────┐    ┌──────────▼───────────┐  │
│  │  NVRAM   │───▶│   SLAVE processor   │  │
│  │ (clock)  │    │  (co-processeur)    │  │
│  └──────────┘    └──────────┬───────────┘  │
│                             │               │
│  ┌──────────┐    ┌──────────▼───────────┐  │
│  │   MCD212 │◀───│    68070 (main CPU) │  │
│  │  (VSD)   │    │  @ 15 MHz           │  │
│  └────┬─────┘    └──────────┬───────────┘  │
│       │                     │               │
│       │ video               │ bus système   │
│       ▼                     ▼               │
│  ┌──────────┐    ┌──────────────────────┐  │
│  │  sortie  │    │   ROM CD-RTOS        │  │
│  │ composite│    │   RAM utilisateur     │  │
│  │  + RGB   │    │   RAM système        │  │
│  └──────────┘    └──────────────────────┘  │
└─────────────────────────────────────────────┘
```

---

## 2. Cartographie mémoire du CD-i 205

| Plage d'adresses | Taille | Type | Description |
|-----------------|--------|------|-------------|
| `$000000–$1FFFFF` | 2 Mo | ROM | CD-RTOS (système d'exploitation) + modules OS-9 |
| `$200000–$3FFFFF` | 2 Mo | — | *réservé (espace carte d'extension ?)* |
| `$400000–$7FFFFF` | 4 Mo | — | *réservé expansion* |
| `$800000–$9FFFFF` | 2 Mo | — | *espace mémoire additionnelle ?* |
| `$A00000–$CFFFFF` | 3 Mo | RAM | RAM utilisateur + mémoire vidéo |
| `$D00000–$D7FFFF` | 512 Ko | RAM | RAM système / framebuffer secondaire |
| `$D80000–$DFFFFF` | 512 Ko | — | *réservé* |
| `$E00000–$E7FFFF` | 512 Ko | I/O | **MMIO console** (68070 internes, MCD212, CDIC, UART…) |
| `$E80000–$EFFFFF` | 512 Ko | I/O | **MMIO CARTOUCHE FMV** (voir `docs/fmv/FMV_MMIO_MAP.md`) |

> ⚠️ **Règle de délimitation FMV** : `$E80000–$EFFFFF` est reservé à la Digital Video Cartridge et **n'est jamais accédé par la console seule**. Ne pas confondre avec le MMIO console (`$E00000–$E7FFFF`).

### RAM détaillée

| Plage | Description |
|-------|-------------|
| `$A00000–$AFFFFF` | Zone utilisateur / programme |
| `$B00000–$BFFFFF` | Framebuffer principal (MCD212, 1 plane) |
| `$C00000–$CFFFFF` | Framebuffer secondaire / sprites (MCD212, 1 plane) |
| `$D00000–$D3FFFF` | RAM système (NVRAM shadow, buffers CD) |

---

## 3. Composants matériels détaillés

### 3.1 CPU — Motorola 68070

Le 68070 est un **SOC 32 bits** intégrant un cœur 68000-compatible avec des périphériques sur puce :

| Périphérique intégré | Adresse relative | Notes |
|---------------------|----------------|-------|
| Timers (2×) | `$E00000` | Timer A, Timer B |
| UART | `$E10000` | Port série / console debug |
| I²C controller | `$E20000` | Communication NVRAM, tuner |
| DMA controller | `$E30000` | Transferts audio/mémoire |
| Interrupt controller | interne | IRQ maskable / non-maskable |

### 3.2 Vidéo — MCD212 (Dual Plane Display Controller)

Produit par **Philips**, le MCD212 est le cœur graphique du CD-i :

| Caractéristique | Valeur |
|----------------|--------|
| Résolutions | 384×280, 512×384, 768×576 (entrelaçé) |
| Plans graphiques | 2 plans (foreground + background) |
| Couleurs | 16, 256 ou 32 768 couleurs (selon mode) |
| Sprites | Matrice de sprites hardware |
| Sortie | RGB analogique + Composite |

> 🔲 **À extraire** : la plage d'adresses exactes des registres MCD212 dans l'espace `$E00000–$E7FFFF`.

### 3.3 Audio — PCM / FSCM

| Caractéristique | Détail |
|----------------|--------|
| Format | ADPCM adaptatif (4-bit) |
| Canaux | Stéréo |
| Fréquence | 16 niveaux de compression |
| Codec | FSCM (Full Scalable CODEC Module) |
| Sortie | DAC stéréo |

> 🔲 **À confirmer** : adresse des registres audio dans le MMIO console.

### 3.4 SLAVE Processor

Un **second processeur** (type 68HC05 ou equivalent) gère :
- La lecture du CD-ROM (servo-mécanique)
- La correction d'erreurs
- Le transfert audio CD directement vers le DAC

### 3.5 NVRAM / Horloge

| Caractéristique | Détail |
|----------------|--------|
| Capacité | 2 Ko ( RTC + config ) |
| Contenu | Date/heure, paramètres régionaux, calibration vidéo |
| Communication | Bus I²C (via contrôleur interne 68070) |

### 3.6 CD-ROM + CDIC

| Élément | Description |
|---------|-------------|
| Format | CD-ROM XA, Mode 2 Form 1 & 2 |
| Média | CD-i Digital Audio Disc |
| Contrôleur | CDIC (CD Interface Controller) |
| Débit | ~150 Ko/s (1× CD-ROM) |
| Temps d'accès | ~300–500 ms |

---

## 4. CD-RTOS — Système d'exploitation

### 4.1 Qu'est-ce que CD-RTOS ?

**CD-RTOS** (CD-ROM Real-Time Operating System) est le SE du CD-i. C'est un **dérivé direct d'OS-9/68000** de Microware, adapté au temps réel et à l'embarqué :

| Élément | Lien OS-9 |
|---------|-----------|
| Noyau | OS-9 style : tâches, sémaphores, messages |
| Système de fichiers | ISO9660 + extensions CD-i |
| Gestion mémoire | Segmentation / pagination |
| Appels système | I$Read, I$Write, I$Seek, P$Start… |
| Device drivers | Format OS-9 standard (device descriptors) |
| Modules OS-9 | Chargeables dynamiquement |

### 4.2 Modules OS-9 attendus dans la ROM système

> 🔲 **À extraire** : les modules OS-9 de la ROM CD-RTOS doivent être dumpés et parsés avec `tools/os9_module_parser.py`.

| Module | Rôle |
|--------|------|
| `kernel` | Noyau temps réel, ordonnancement |
| `scf` | Character I/O device manager |
| `rbf` | Random Block File manager (CD-ROM) |
| `ioman` | I/O Manager |
| `datetime` | Gestion date/heure |
| `cmds` | Interpréteur de commandes (?) |
| `ddinit` | Driver descriptor initialization |
| `dddef` | Default device descriptor (?) |

---

## 5. Tableau de statut — Console

| Tâche | Priorité | État | Notes |
|-------|----------|------|-------|
| Extraction ROM CD-RTOS | ⭐⭐⭐ | 🔲 À démarrer | dump mémoire ou fichier .bin à acquérir |
| Cartographie mémoire complète | ⭐⭐⭐ | 🔲 À démarrer | cross-ref doc + dump |
| Identification modules OS-9 ROM | ⭐⭐ | 🔲 À démarrer | utiliser `os9_module_parser.py` |
| Reverse-engineering MCD212 | ⭐⭐⭐ | 🔲 À démarrer | docs externes + tests |
| Analyse CDIC | ⭐⭐ | 🔲 À démarrer | contrôleur CD-ROM |
| Implémentation timers 68070 | ⭐ | 🔲 À démarrer | 2 timers internes |
| Implémentation UART | ⭐ | 🔲 À démarrer | debug console |
| Émulation NVRAM | ⭐⭐ | 🔲 À démarrer | I²C |
| Émulation DMA audio | ⭐⭐ | 🔲 À démarrer | |
| Émulation SLAVE processor | ⭐ | 🔲 À démarrer | co-processeur CD |

---

## 6. Ressources externes

- [CD-i Technical Reference Manual (PDF)](http://www.cdi-player.com/CDI/tekniikka/cdi_tech.pdf) — documentation officielle Philips
- [MAME — driver CD-i 205](https://github.com/mamedev/mame/blob/master/src/mess/machine/cdi200.cpp) — référence d'émulation
- [OS-9/68000 Programmer's Reference](http://www.discover-music.net/OS-9/OS-9_68000_Programmers_Reference.pdf) — API OS-9
- [Philips MCD212 datasheet](https://archive.org/details/bitsavers_philipsMCD2DisplayControllerDataSheet_109615) — contrôleur vidéo

---

*Document axe CONSOLE — dernière mise à jour : 21 juin 2026*
