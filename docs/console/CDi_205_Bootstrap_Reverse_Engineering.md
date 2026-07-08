# CD-i 205 — Bootstrap & CDIC Protocol Reverse Engineering
## Firmware PS-7211 REL.2.1 — Analyse de Rétro-Ingénierie

**Document version :** 1.0  
**Cible :** Philips CD-i 205 (PS-7211 REL.2.1)  
**Architecture :** Motorola 68000 @ 11.3 MHz + ASIC CD-i + CDIC  
**Objectifs :** Décompilation du bootstrap primaire, émulation hardware, reconstruction du protocole de boot CDIC  
**Date :** 23 juin 2026  

---

## Table des matières (TOC)

1. [Introduction et Contexte](#1-introduction--contexte)
2. [Carte Mémoire et Zones Critiques](#2-carte-mémoire-et-zones-critiques)
3. [Diagnostic Matériel Post-Reset — FUN_00180E62](#3-diagnostic-matériel-post-reset--fun_00180e62)
4. [Configuration du Mode de Transfert — FUN_00180B44](#4-configuration-du-mode-de-transfert--fun_00180b44)
5. [Machine à États du Bootstrap Primaire — FUN_00180E9E](#5-machine-à-états-du-bootstrap-primaire--fun_00180e9e)
6. [Protocole de Paquet CDIC Détaillé](#6-protocole-de-paquet-cdic-détaillé)
7. [Scanner de Kernel OS-9 — FUN_0018109C](#7-scanner-de-kernel-os-9--fun_0018109c)
8. [Snippets Assembleur Clés](#8-snippets-assembleur-clés)
9. [Conseils pour Stubs Émulateur](#9-conseils-pour-stubs-émulateur)
10. [Synthèse — Graph de Boot ASCII](#10-synthèse--graph-de-boot-ascii)

---

## 1. Introduction & Contexte

### 1.1 Le lecteur CD-i 205

Le Philips CD-i 205 (PS-7211 REL.2.1) est un lecteur de CD interactif basé sur un processeur **Motorola 68000** cadencé à **11.3 MHz**. L'architecture comprend :

| Composant | Rôle |
|---|---|
| **68000 CPU** | Traitement principal, exécution du firmware |
| **ASIC CD-i** | Contrôleur vidéo, audio, interface utilisateur |
| **CDIC** | Contrôleur d'interface CD — gestion du média optique |
| **ROM Interne** | Firmware de bootstrap (zone $0000_0000–$000F_FFFF) |
| **RAM Système** | Zone $0008_0000–$00FF_FFFF (16 Mo adressables) |

### 1.2 Objectifs de cette analyse

L'objectif est de comprendre le **bootstrap primaire** — la séquence qui s'exécute après un reset matériel et qui :

1. Initialise le hardware (ASIC, CDIC)
2. Charge un module d'amorce depuis le média CD
3. Localise et exécute le kernel OS-9

Ces informations sont nécessaires pour :
- **Émulation accurate** du hardware CD-i 205
- **Décompilation** du firmware de bootstrap
- **Injection de code** personnalisée (homebrew, debugging)

### 1.3 Zone d'adresse du bootstrap

Le firmware de bootstrap réside en ROM dans la zone basses adresses :

```
0x00180000 – 0x00181FFF  :  Code du bootstrap primaire (8 Ko)
0x00180460 – 0x0018047F  :  Données initiales (constantes hardcodées)
```

---

## 2. Carte Mémoire et Zones Critiques

### 2.1 Structure des registres ASIC

L'ASIC CD-i est mappé en mémoire à partir de la base **`$8000_2011`**. Chaque registre est accessible via un **offset** depuis cette base.

> ⚠️ **Piège critique** : Toutes les adresses de registre mentionnées dans le firmware sont des **offsets depuis `$80002011`**. Par exemple, `$80002013` = base + `$0x02` = registre de statut.

| Offset depuis $80002011 | Adresse effective | Nom logique | Usage |
|:---:|:---|:---|:---|
| `+$00` | `$80002011` | `ASIC_BASE` / DATA | Registre de données CDIC |
| `+$02` | `$80002013` | `ASIC_STATUS` | Statut du CDIC — bit 3 = `CD_READY` |
| `+$04` | `$80002015` | `ASIC_CTRL` | Contrôle — bit 7 = `TRANSFER_ENABLE` |
| `+$06` | `$80002017` | `ASIC_CMD` | Commande — sélectionne le registre actif |

**Registre ASIC_CMD (offset +$06) — Sélecteur de registre interne :**

| Valeur | Mode du registre ASIC_CTRL |
|:---|:---|
| `$0A` | **Mode Transfert** — ASIC_CTRL devient registre de mode de transfert |
| `$05` | **Mode Normal** — ASIC_CTRL redevient registre de commande standard |
| `$06` | **Mode Statut** — lecture du flags de contrôleur |

### 2.2 Valeurs fixes en données ROM

Ces constantes sont utilisées tout au long du bootstrap :

| Adresse ROM | Valeur | Usage |
|:---|:---|:---|
| `DAT_00180460` | `$0000_0500` | Taille de région / alignement |
| `DAT_00180464` | `$0008_0000` | Adresse de base RAM |
| `DAT_00180468` | `$0050_0000` | Adresse de région mémoire |
| `DAT_0018046C` | `$0100_0000` | Borne mémoire haute |
| `DAT_001804A0` | `$80002011` | Base des registres ASIC (chargée dans A3) |

### 2.3 Résumé mappage mémoire

```
Zone ROM (bootstrap) :  0x00180000 – 0x00181FFF
  └── 0x00180E62 : Point d'entrée FUN_00180E62 (diagnostic post-reset)
  └── 0x00180AEE : Fonction de polling ASIC / lecture CDIC
  └── 0x00180B44 : Configuration du mode de transfert
  └── 0x00180FD0 : Lecture entier big-endian depuis ASIC
  └── 0x0018109C : Scanner de kernel OS-9

Registres ASIC :         $80002011 – $80002017
  └── +$00 (DATA)        Lecture/écriture de données CDIC
  └── +$02 (STATUS)      Statut — bit 3 = CD_READY
  └── +$04 (CTRL)        Contrôle — bit 7 = enable transfert
  └── +$06 (CMD)         Sélection du registre interne

RAM Système :            $00080000 – $00FFFFFF
  └── Zone de chargement du bootstrap : configurable par paquet CDIC
```

---

## 3. Diagnostic Matériel Post-Reset — FUN_00180E62

### 3.1 Point d'entrée `0x00180E62`

Après un reset matériel, le premier code exécuté est **`FUN_00180E62`**. Son rôle est de vérifier que le hardware (ASIC + CDIC) est opérationnel avant de tenter tout accès au média.

```
00180E62  48 E7 3C 40        movem.l  {A2-A4 D3-D1},-(SP)    ; Sauvegarde contexte
00180E66  26 7C 0008 0000    movea.l #$00080000,A3            ; Base RAM
00180E6C  43 F9 0018 04A0    lea     DAT_001804A0(PC),A1       ; A1 = $80002011 (base ASIC)
00180E72  4E 71              nop                             ; Délai硬件
00180E74  60 00 00 3A        bra.w   FUN_00180AEE              ; → Diagnostic ASIC
```

### 3.2 FONCTION `FUN_00180AEE` — Polling de l'ASIC

**Prototype :** `uint8_t FUN_00180AEE(void)`  
**Retourne :** Un octet lu depuis le registre `$8000201B` (registre de statut du contrôleur)

> ⚠️ **Piège critique — Interprétation de l'adresse** :  
> L'instruction `lea (0x6A,PC)=>DAT_00180A7A, A2` charge l'adresse effective `$8000201B`. Cela correspond à l'offset **+$0A** depuis la base ASIC `$80002011`. Il ne s'agit pas d'un registre distinct mais d'une zone de statut étendue du CDIC.

**Comportement du polling :**

1. Lecture de `$8000201B`
2. Si `D0 == -1` → timeout, retour immédiat
3. Si `D0 >= 0` → retour de la valeur

### 3.3 Condition de boot — `LAB_00180E82`

La condition de succès est **exigeante** :

```asm
00180E82  B0 3C 00 06        cmp.b   #$06,D0b         ; D0 doit valoir EXACTEMENT 6
00180E86  67 00 00 06        beq.w   LAB_00180E8E      ; OK → boot normal
00180E8A  60 00 F8 00        bra.w   FUN_0018068C      ; Échec → fallback memory probe
```

| Valeur de retour D0 | Destination | Signification |
|:---:|:---|:---|
| `-1` (`$FF`) | Boucle de retry (`dbf D0,LAB_180E7C`) | Timeout — réessai |
| `$06` | `LAB_00180E8E` — Bootstrap normal | **DISC_READY / SYSTEM_OK** |
| Autre (`$05`, `$0A`, `$F6`…) | `FUN_0018068C` | Échec hardware — fallback menu test |

> ⚠️ **Règle d'or pour l'émulateur** : La fonction `read_asic($8000201B)` **doit retourner exactement `$06`** pour que le boot continue. Retourner `$F6` ou toute autre valeur positive provoque un branchement vers le fallback (memory probe + test du menu).

### 3.4 Préparation du bootstrap — `LAB_00180E8E`

```asm
00180E8E  26 3C 00 00 4B 00  move.l  #$4B00,D3        ; D3 = taille par défaut = 19200 octets
00180E94  61 00 FC AE        bsr.w   FUN_00180B44      ; Configure mode de transfert
00180E98  28 7C FF FF FF FF  movea.l #-1,A4           ; A4 = -1 ( adresse destination par défaut = RAM non initialisée)
```

Le bootstrap commence avec :
- **Taille par défaut** : `$4B00` = 19200 octets (taille du module boot classique OS-9)
- **Adresse destination par défaut** : `-1` (non initialisée, sera écrasée par le premier paquet `ADR`)
- **Mode de transfert** : configuré par `FUN_00180B44`

---

## 4. Configuration du Mode de Transfert — FUN_00180B44

### 4.1 Point d'entrée

```asm
00180B44  48 E7 C0 10        movem.l  {A3 D1 D0},-(SP)   ; Push contexte
00180B48  26 7A F9 56        movea.l  (-$6AA,PC)=>DAT_001804A0,A3 ; A3 = $80002011 (base ASIC)
00180B4C  61 00 FF D4        bsr.w    FUN_00180B22       ; Lecture statut CDIC → D0
00180B50  65 00 00 2C        bcs.w    LAB_00180B7E       ; Erreur → sortie
```

### 4.2 Séquence de configuration (mode transfert)

```asm
00180B5A  26 3C 00 0A        move.l  #$0A,D3              ; D3 = $0A (code "Mode Transfert")
00180B60  36 39 00 06        move.w  D3w,($06,A3)         ; ASIC_CMD = $0A → sélectionne registre mode
00180B64  28 39 00 02        movea.l ($02,A3),A4          ; Lecture ASIC_STATUS → A4 (bit 3 = CD_READY)
00180B6A  C0 04              lsr.b   #0x4,D0b             ; D0 >>= 4 (extraction flag)
00180B6C  63 00 00 1A        bls.w   LAB_00180B88         ; D0 == 0 → skip écriture
00180B70  12 00              move.b  D0b,D1b              ; D1 = mode (1, 2, 4, ou 8)
00180B72  E9 09              lsl.b   #0x4,D1b             ; D1 <<= 4 (poids fort)
00180B74  D0 01              add.b   D1b,D0b              ; D0 = D0 | (D0<<4) → double-nibble
00180B76  00 C0 00 80        or.b    #$80,D0b             ; bit 7 = enable (START TRANSFER)
00180B78  13 C0 00 04        move.b  D0b,($04,A3)         ; ASIC_CTRL = mode validé
00180B7C  60 00 00 0A        bra.w   LAB_00180B88         ; Sortie

00180B80  13 FC 00 00 00 06  move.b  #$05,($06,A3)        ; ASIC_CMD = $05 → retour mode normal
```

### 4.3 Explication du Double-Nibble

La construction **`D0 = (D0 << 4) | D0`** est une **signature de validation matérielle** :

```
Mode 1 (binary 0001) → $11
Mode 2 (binary 0010) → $22
Mode 4 (binary 0100) → $44
Mode 5 (binary 0101) → $55
Mode 8 (binary 1000) → $88
```

Cette double-nibble garantit que :
1. Une écriture accidentelle (bruit, glitch) ne peut pas activer un mode car le bit bas et le bit haut doivent être identiques
2. Le hardware ASIC peut valider la cohérence du mode avant activation
3. Le bit 7 (`$80`) est ensuite OR-ed pour **valider正式启动 le transfert**

### 4.4 Récapitulatif séquence ASIC

| Étape | Registre | Valeur | Effet |
|:---:|:---|:---|:---|
| 1 | `ASIC_CMD` (`$80002017`) | `$0A` | Sélectionne le registre de mode de transfert |
| 2 | `ASIC_STATUS` (`$80002013`) | lecture | Attend bit 3 = `CD_READY` |
| 3 | `ASIC_CTRL` (`$80002015`) | `double_nibble | $80` | Active le mode avec validation |
| 4 | `ASIC_CMD` (`$80002017`) | `$05` | Retour au mode registre standard |

---

## 5. Machine à États du Bootstrap Primaire — FUN_00180E9E

### 5.1 Structure principale

```asm
00180E9E  43 FA FF 5F        lea     (-$A1,PC),A1      ; A1 → zone données
00180EA2  26 3C 00 00 00 00  move.l  #$00000000,D3     ; D3 = 0 (checksum XOR cumulatif)
00180EA8  26 7C FF FF FF FF  movea.l #-1,A4            ; A4 = -1 (dest par défaut)
00180EAE  41 F9 00 08 04 60  lea     DAT_00180460(PC),A0 ; A0 → données fixes
```

### 5.2 Variables d'état initiales

| Variable | Adresse (A1 offset) | Valeur initiale | Rôle |
|:---|:---|:---|:---|
| D2 | — | `$00000000` | **Checksum XOR-8 cumulatif** |
| D3 | — | `$00000000` | Accumulator pour lectures big-endian |
| A4 | — | `-1` ($FFFFFFFF) | **Pointeur destination** des données |
| A0 | — | `DAT_00180460` | Accès aux constantes ROM |

### 5.3 Boucle principale — `LAB_00180EB8`

```asm
00180EB8  61 00 FF 32        bsr.w    FUN_00180AEE      ; Lecture statut / data
00180EBC  B0 3C 00 01        cmp.b    #$01,D0b          ; Octet de préambule ?
00180EC0  66 00 FF F6        bne.w    LAB_00180EB8       ; Non → re-lecture
00180EC4  61 00 FF 26        bsr.w    FUN_00180AEE      ; Lecture commande
00180EC8  12 00              move.b   D0b,D1b           ; Sauvegarde commande
00180ECA  E1 08              lsr.b    #0x1,D0b          ; D0 >>= 1 (bit 0 ?)
00180ECC  63 00 00 10        bls.w    LAB_00180EDE       ; D0 == 0 → commande $00 (fin/erreur)
00180ECE  4E 71              nop                            ; Délai
00180ED0  61 00 FF FE        bsr.w    FUN_00180FD0      ; Lecture header big-endian
00180ED4  61 00 FF 1A        bsr.w    FUN_00180FF0      ; Validation checksum
00180ED8  65 00 01 C2        bcs.w    LAB_00180E9C       ; Checksum échoué → reset state machine
00180EDC  60 00 00 0C        bra.w    LAB_00180EEA       ; Checksum OK → traiter commande
```

### 5.4 Logique de commande — `LAB_00180EEA`

```asm
00180EEA  12 01              move.b   D1b,D0b           ; Restaure commande
00180EEC  4E 90              jsr      (A0)              ; Appel indirect → traitement commande
00180EEE  4E 71              nop                            ; Délai inter-commande
00180EF0  60 00 FF C6        bra.w    LAB_00180EB8       ; Boucle → prochaine commande
```

Les valeurs de commande (`$01`, `$02`, `$04`, `$08`) sont **décodées par table de dispatch** via l'appel indirect `jsr (A0)`. La table est located à `DAT_00180460`.

---

## 6. Protocole de Paquet CDIC Détaillé

### 6.1 Format général d'un paquet

```
┌─────────────┬─────────────────┬───────────────┬──────────────┐
│ Préambule   │ Commande        │ Header        │ Data         │
│ 1 octet ($01)│ 1 octet         │ N octets      │ Payload      │
│             │ ($01/$02/$04/$08)│ (FUN_180FD0)  │              │
└─────────────┴─────────────────┴───────────────┴──────────────┘
                                          └── checksum XOR-8
```

### 6.2 FONCTION `FUN_00180FD0` — Lecture entier big-endian

**Prototype :** `void FUN_00180FD0(int D1)` — D1 = nombre d'octets à lire

```asm
00180FD0  53 41              subq.w  #0x1,D1w      ; D1-- (compteur d'octets)
00180FD2  26 3C 00 00 00 00  move.l #$00000000,D3 ; D3 = 0 (reset accumulateur)

          LAB_00180FD8 (boucle de lecture)
00180FD8  61 00 FF 12        bsr.w   FUN_00180AEE  ; Lecture d'un octet
00180FDC  B0 3C FF           cmp.b   #$FF,D0b      ; Timeout (-1) ?
00180FE0  67 00 FF F6        beq.w   LAB_00180FD8  ; Retry si timeout
00180FE4  E5 08              rol.l   #0x8,D3       ; D3 <<= 8 (make room poids faible)
00180FE6  C0 00              add.b   D0b,D3        ; D3 |= D0 (injecte octet poids faible)
00180FE8  51 C9 FF EE        dbf     D1w,LAB_180FD8 ; D1--, boucle si D1 >= 0

00180FEE  12 00              move.b  D0b,D1b      ; Sauvegarde dernier octet
00180FF0  B0 01              cmp.b   D1b,D0b      ; Compare avec checksum ?
00180FF2  67 00 00 0A        beq.w   LAB_00180FFE  ; Égal → checksum OK
00180FF4  12 00              move.b  D1b,D0b      ; Restore D0 (dernier data)
00180FF6  12 02              move.b  D2b,D0b      ; D0 = checksum calculé
00180FF8  60 00 00 0C        bra.w   LAB_00181006  ; Retour (carry selon checksum)
```

**Sémantique :**
- Les `D1` octets sont lus **big-endian** (premier octet → poids fort de D3)
- Chaque octet est XOR-é dans `D2` pour former le checksum cumulatif
- Le dernier octet lu est **susceptible d'être le checksum** (testé en `00180FF0`)

### 6.3 FONCTION `FUN_00180FF0` — Validation checksum

```asm
00180FF0  61 00 FC D4        bsr.w   FUN_00180AEE  ; Lecture checksum final
00180FF4  B0 3C FF           cmp.b   #$FF,D0b      ; Timeout ?
00180FF6  67 00 FF F6        beq.w   LAB_00180FEE  ; Retry
00180FF8  B0 02              cmp.b   D2b,D0b       ; D0 (checksum lu) == D2 (checksum calculé) ?
00180FFA  66 00 00 0A        bne.w   LAB_00181006  ; Différent → carry = 1 (échec)
00180FFC  12 06              move.b  D6b,D0b       ; D0 = ACK $06
00180FFE  61 00 FF A6        bsr.w   FUN_00180AA8  ; Envoie ACK
00181002  48 63              moveq   #$63,D1        ; D1 = $63 (delay count)
00181004  51 C9 FF BB        dbf     D1w,LAB_180FD8 ; Délai de $63+1 = 100 itérations

          LAB_00181006
00181006  4E 75              rts                           ; Carry = 1 → échec, Carry = 0 → OK
```

**Résultat :**
- `Carry = 0` : Checksum valide → ACK `$06` envoyé
- `Carry = 1` : Checksum invalide → NAK (implicitement, via `rts` sans envoi ACK)

### 6.4 Table des 4 commandes

| Code commande | Nom | Header lu (FUN_180FD0) | Effet sur D3 | Effet sur A4 | Action |
|:---:|:---|:---:|:---:|:---:|:---|
| `$01` | **DATA** | 2 octets (big-endian) = taille N | D3 = N (nouvelle taille) | inchangé | Boucle `dbf D3` lecture octets → `(A4)+` |
| `$02` | **ADR** | 4 octets (big-endian) = adresse | D3 (adresse) | A4 = D3 (nouvelle destination) | Prépare chargement à cette adresse |
| `$04` | **EXEC** | 4 octets (big-endian) = adresse | D3 (adresse) | A4 inchangé | `jsr (A0)` avec A0 = adresse d'exécution |
| `$08` | **SECT** | Aucun (utilise valeur héritée) | inchangé | testé | Appelle `FUN_180B44`, accède `$000003E8`, fallback `FUN_18068C` |

### 6.5 Traitement commande $01 (DATA) — `LAB_00180EEA`

```asm
00180EEA  12 01              move.b   D1b,D0b       ; Restore commande
00180EEC  4E 90              jsr      (A0)          ; Dispatch → lire taille 2 octets
00180EEE  4E 71              nop                    ; Délai
00180EF0  60 00 FF C6        bra.w    LAB_00180EB8   ; Boucle principale

          LAB_00180F08 (traitement DATA)
00180F08  26 3C              move.l   D3,D3          ; D3 = taille (depuis FUN_180FD0 avec D1=2)
00180F0A  4E 71              nop
          LAB_00180F0C (boucle de réception DATA)
00180F0C  61 00 FD DE        bsr.w    FUN_00180AEE  ; Lecture octet data
00180F10  B0 3C FF           cmp.b    #$FF,D0b      ; Timeout ?
00180F12  67 00 FF F8        beq.w    LAB_00180F0C  ; Retry
00180F16  26 41              move.b   D1b,(A4)+     ; Store à destination, advance A4
00180F18  53 43              subq.w   #0x1,D3w      ; D3-- (compteur)
00180F1A  64 00 FF F0        bcc.w    LAB_00180F0C  ; Continue si D3 >= 0
00180F1C  60 00 FC B2        bra.w    LAB_00180ED0  ; Retour → checksum + next
```

### 6.6 Traitement commande $04 (EXEC) — `LAB_00180F58`

```asm
00180F58  41 FA 00 2C        lea      LAB_00180F86(PC),A0 ; A0 = adresse de retour après jsr
00180F5C  61 00 FC 6A        bsr.w    LAB_00180DC8  ; Validation checksum final
00180F60  65 00 03 1A        bcs.w    LAB_0018137C  ; Échec → message d'erreur
00180F64  12 06              move.b   D6b,D0b       ; D0 = $06 (ACK)
00180F66  61 00 FF 3E        bsr.w    FUN_00180AA8  ; Envoie ACK
00180F6A  4E 71              nop                    ; Délai
00180F6C  36 3C 75 30        move.w   #$7530,D3     ; D3 = $7530 (délai ~30000 cycles)
          LAB_00180F72
00180F72  53 C3              subq.w   #0x1,D3w      ; D3--
00180F74  64 00 FF FC        bcc.w    LAB_00180F72  ; Boucle délai

00180F78  41 F9 00 00 03 E8  lea      $000003E8,A0  ; A0 = $000003E8 (adresse de vecteur ?)
00180F7E  4E 90              jsr      (A0)          ; Exécute le code chargé
          ; ← retour ici après le jsr (A0) si le code fait rts
00180F80  60 00 02 FA        bra.w    LAB_0018137C  ; Fin / affichage message
```

> ⚠️ **Détail critique** : Le `jsr (A0)` en `00180F7E` saute vers **`$000003E8`** — une adresse en RAM qui pointe vers le code du bootstrap chargé. Après le `jsr`, le code chargé doit exécuter un `rts` pour revenir ici. Le `bra LAB_0018137C` est atteint après le retour.

---

## 7. Scanner de Kernel OS-9 — FUN_0018109C

### 7.1 Point d'entrée

```asm
0018109C  48 E7 3C 40        movem.l {A2-A4 D3-D1},-(SP)  ; Sauvegarde contexte
001810A0  42 68 00 0C        lea     $0C(SP),A3       ; A3 → liste de descripteurs sur pile
001810A4  2E 6B 00 08        movea.l $08(SP),A7       ; A7 = borne supérieure (fin pile)
```

**Interface :**
- `A3` pointe sur une **liste de descripteurs** sur la pile : `(adresse:4 octets, taille:4 octets)` par région
- `A7` est la borne supérieure (fin de la zone mémoire à scanner)

### 7.2 Boucle de scan — `LAB_001810AA`

```asm
          LAB_001810AA
001810AA  24 13              movea.l (A3),A2          ; A2 = adresse de la région
001810AC  B4 2E              cmpa.l  A7,A2             ; A2 > A7 (fin de liste) ?
001810AE  62 00 00 10        bhi.w   LAB_001810C0     ; Oui → pas de kernel trouvé
001810B2  26 13              movea.l (A3),A3          ; A3 = descripteur courant
001810B4  26 3C              move.l  (A3)+,D3         ; D3 = adresse, A3 advance
001810B6  36 13              move.w  (A3)+,D3w        ; D3 = taille (16 bits), A3 advance
001810B8  61 00 FF E0        bsr.w   LAB_0018109A     ; Vérifie si region contient "Kernel"
001810BA  24 03              movea.l D3,A2            ; A2 = résultat (pointeur ou NULL)
001810BC  66 00 FF EC        bne.w   LAB_001810AA     ; Non trouvé → prochaine région
001810C0  60 00 00 3E        bra.w   LAB_001810FE     ; Fin → affichage "NO OS-9 kernel found"
```

### 7.3 Vérification module OS-9 — `LAB_0018109A`

```asm
0018109A  48 E7 38 20        movem.l {A0/A1/A2 D0/D1/D2},-(SP)

          LAB_0018109E
0018109E  3E 13              move.w  (A3)+,D7w        ; D7 = taille de la région
001810A0  64 00 00 08        bcc.w   LAB_001810AA     ; Taille = 0 → skip

001810A2  22 13              movea.l (A3)+,A1         ; A1 = adresse du module
001810A4  30 3C 4A FC        move.w  #$4AFC,D0w       ; D0 = magic OS-9 ($4AFC)
001810A8  B0 89              cmp.w   (A1),D0          ; Magic word à l'offset $00 du module ?
001810AA  66 00 00 2A        bne.w   LAB_001810D6     ; Non → skip cette région

001810AC  7C 4A              moveq   #$4A,D6b         ; D6 = $4A
001810AE  E5 C6              asr.w   #0x1,D6w         ; D6 = 37 (0x4A >> 1 = 37)
001810B0  70 3C FF FF        moveq   #$FF,D0          ; D0 = -1 ($FFFF)
          LAB_001810B4 (boucle checksum)
001810B4  B0 81              cmp.w   (A1)+,D0         ; XOR-16 sur les mots du module
001810B6  66 00 00 1E        bne.w   LAB_001810D6     ; Checksum != 0 → reject
001810B8  51 C6 FF FA        dbf     D6w,LAB_1810B4   ; D6--, boucle (37+1 = 38 itérations)

001810BC  3F 04              move.w  D4,-(SP)        ; Push checksum final (doit être 0)
001810BE  2A 14              movea.l (A2)+,A1         ; A1 = chaîne "Kernel"
001810C0  20 04              movea.l D4,A0            ; A0 = base du module
001810C2  32 54              movea.w $0C(A1),A1        ; A1 = offset du nom dans le module
001810C4  D3 C4              adda.l  A0,A1             ; A1 = adresse absolue du nom
          LAB_001810C6 (boucle comparaison nom)
001810C6  12 11              move.b  (A1)+,D1b        ; Lecture caractère du nom module
001810C8  13 02 00 09        move.b  D2b,$09(A2)       ; Caractère à comparer
001810CC  C2 89              addq.b  #0x1,A2          ; Advance pointeur comparé
001810CE  E2 08              lsr.b   #0x1,D2b         ; D2 >>= 1
001810D0  64 00 FF F4        bcc.w   LAB_001810C6     ; Continue si pas à la fin
001810D4  20 0C              movea.l A0,A2            ; A2 = pointeur sur le module Kernel

          LAB_001810D6
001810D6  4F EF 00 08        lea     $08(SP),A7        ; Cleanup pile
001810DA  4E 5E              unmovem ...               ; Restore contexte
001810DC  4E 75              rts                          ; Retour (A0 = kernel ou NULL)
```

### 7.4 Algorithme de checksum OS-9

```asm
moveq  #$4A,D6b    ; D6 = 74 (0x4A)
asr.w  #1,D6w      ; D6 = 37 (74 / 2)
moveq  #$FF,D0     ; D0 = -1 ($FFFF) — accumulateur XOR-16

; Boucle sur 37+1 = 38 mots (76 octets)
; Chaque mot est XOR-é avec D0 (initialisé à $FFFF)
; Résultat final doit être $0000 pour un module valide
```

> ⚠️ **Pour créer un faux module Kernel en RAM** :  
> 1. Écrire le **magic `$4AFC`** à l'offset `$00`  
> 2. Calculer le **checksum XOR-16 cumulatif** sur les 38 premiers mots (offset $02–$4C) en initialisant l'accumulateur à `$FFFF` → le résultat de ce checksum **devra être `$0000` après XOR avec lui-même**  
> 3. Le nom "Kernel" doit commencer à l'offset défini par le champ **offset nom** du module (généralement $0C)  
> 4. Les comparaisons sont **insensibles à la casse** (bit 5 libéré via `andi.b #$DF`)

### 7.5 Message d'erreur — `LAB_001810FE`

```asm
001810FE  41 FA 02 27        lea     (-$D9,PC)=>DAT_00181329,A0  ; "NO OS-9 kernel found.\r"
00181102  ...                ; (suite affichage via lea + bra FUN_00181D02)
```

L'affichage utilise le même mécanisme de string printing que le reste du bootstrap (`lea` + `bra FUN_00181D02`).

---

## 8. Snippets Assembleur Clés

### 8.1 Diagnostic post-reset (FUN_00180E62)

```asm
; ============================================================
; FUN_00180E62 — Point d'entrée post-reset
; ============================================================
00180E62  48 E7 3C 40     movem.l  {A2-A4 D3-D1},-(SP)  ; Sauvegarde 7 registres
00180E66  26 7C 00080000  movea.l  #$00080000,A3          ; A3 = base RAM
00180E6C  43 F9 001804A0 lea      DAT_001804A0(PC),A1     ; A1 = $80002011 (ASIC)
00180E72  4E 71           nop                            ; Délai hardware
00180E74  60 00 003A      bra.w    FUN_00180AEE           ; → polling ASIC

; ============================================================
; LAB_00180E82 — Condition de diagnostic (appelé depuis FUN_00180AEE)
; ============================================================
00180E82  B0 3C 00 06     cmp.b    #$06,D0b               ; D0 == $06 (DISC_READY) ?
00180E86  67 00 0006      beq.w    LAB_00180E8E            ; OK → bootstrap normal
00180E8A  60 00 F800      bra.w    FUN_0018068C            ; Échec → fallback memory probe

; ============================================================
; LAB_00180E8E — Préparation bootstrap
; ============================================================
00180E8E  26 3C 00004B00  move.l   #$4B00,D3              ; D3 = 19200 (taille boot par défaut)
00180E94  61 00 FCAE      bsr.w    FUN_00180B44            ; Configure mode de transfert
00180E98  28 7C FFFFFFFF  movea.l  #-1,A4                  ; A4 = -1 (dest non définie)
```

### 8.2 Configuration mode de transfert (FUN_00180B44)

```asm
; ============================================================
; FUN_00180B44 — Configuration du mode de transfert CDIC
; ============================================================
00180B44  48 E7 C010      movem.l  {A3 D1 D0},-(SP)       ; Push contexte

00180B48  26 7A F956      movea.l  (-$6AA,PC)=>DAT_001804A0,A3 ; A3 = $80002011

00180B4C  61 00 FFD4      bsr.w    FUN_00180B22            ; Lecture statut CDIC → D0
00180B50  65 00 002C      bcs.w    LAB_00180B7E            ; Erreur → sortie

; --- Séquence de configuration du mode ---
00180B5A  26 3C 000A      move.l   #$0A,D3                ; D3 = $0A
00180B60  36 39 0006      move.w   D3w,($06,A3)           ; ASIC_CMD = $0A → mode transfert
00180B64  28 39 0002      movea.l  ($02,A3),A4            ; Lecture ASIC_STATUS → A4
00180B6A  C0 04           lsr.b    #0x4,D0b               ; D0 >>= 4
00180B6C  63 00 001A      bls.w    LAB_00180B88            ; D0 == 0 → skip
00180B70  12 00           move.b   D0b,D1b                ; D1 = mode (1,2,4,8)
00180B72  E9 09           lsl.b    #0x4,D1b               ; D1 <<= 4
00180B74  D0 01           add.b    D1b,D0b                ; D0 = D0 | (D0<<4) → DN
00180B76  00C0 0080       or.b     #$80,D0b               ; bit 7 = enable
00180B78  13 C0 0004      move.b   D0b,($04,A3)           ; ASIC_CTRL = mode validé

00180B7C  60 00 000A      bra.w    LAB_00180B88
00180B80  13 FC 0000 0006 move.b   #$05,($06,A3)          ; ASIC_CMD = $05 → mode normal

          LAB_00180B88
00180B88  4E 5E           unmem.l  {A3 D1 D0},(SP)+
00180B8C  4E 75            rts
```

### 8.3 Lecture big-endian + checksum (FUN_00180FD0)

```asm
; ============================================================
; FUN_00180FD0 — Lecture entier big-endian depuis ASIC
; Entrée: D1.w = nombre d'octets à lire
; Sortie: D3.l = entier assemblé big-endian
; Modifié: D2.b = checksum XOR cumulatif
; ============================================================
00180FD0  53 41           subq.w   #0x1,D1w              ; D1-- (prématureur car dbf incrémente)
00180FD2  26 3C 00000000  move.l   #$00000000,D3          ; D3 = 0 (reset accumulateur)

          LAB_00180FD8                           ; ← boucle de lecture
00180FD8  61 00 FF12      bsr.w    FUN_00180AEE           ; Lecture un octet
00180FDC  B0 3C FF        cmp.b    #$FF,D0b               ; Timeout (-1) ?
00180FE0  67 00 FFF6      beq.w    LAB_00180FD8           ; Retry si timeout
00180FE4  E5 08           rol.l    #0x8,D3                ; D3 <<= 8 (make room)
00180FE6  C0 00           add.b    D0b,D3                 ; D3 |= D0 (injecte poids faible)
00180FE8  51 C9 FFEE      dbf      D1w,LAB_00180FD8       ; D1--, boucle si >= 0

00180FEE  12 00           move.b   D0b,D1b                ; Sauvegarde dernier octet
00180FF0  B0 01           cmp.b    D1b,D0b                ; Comparaison checksum
00180FF2  67 00 000A      beq.w    LAB_00180FFE           ; Égal → checksum OK
00180FF4  12 00           move.b   D1b,D0b                ; Restore D0
00180FF6  12 02           move.b   D2b,D0b                ; D0 = checksum calculé

          LAB_00181006
00181006  4E 75           rts                                 ; carry = résultat comparaison
```

### 8.4 Validation checksum (FUN_00180FF0)

```asm
; ============================================================
; FUN_00180FF0 — Validation checksum XOR-8
; Modifié: D2.b = checksum cumulatif
; Retour: Carry clear = checksum valide (ACK $06 envoyé)
;         Carry set   = checksum invalide (NAK implicite)
; ============================================================
00180FF0  61 00 FCD4      bsr.w    FUN_00180AEE           ; Lecture checksum final
00180FF4  B0 3C FF        cmp.b    #$FF,D0b               ; Timeout ?
00180FF6  67 00 FFF6      beq.w    LAB_00180FEE           ; Retry
00180FF8  B0 02           cmp.b    D2b,D0b                ; Lu == calculé ?
00180FFA  66 00 000A      bne.w    LAB_00181006            ; Différent → carry=1 (échec)
00180FFC  12 06           move.b   D6b,D0b                ; D0 = ACK $06
00180FFE  61 00 FFA6      bsr.w    FUN_00180AA8           ; Envoie ACK
00181002  48 63           moveq    #$63,D1                 ; D1 = 99 (delay count)
00181004  51 C9 FFBB      dbf      D1w,LAB_00180FD8        ; Délai de 100 itérations

          LAB_00181006
00181006  4E 75           rts                                 ; carry depuis cmp.b D2,D0
```

### 8.5 Exécution kernel (FUN_00180F58)

```asm
; ============================================================
; LAB_00180F58 — Traitement commande EXEC ($04)
; ============================================================
00180F58  41 FA 002C      lea      LAB_00180F86(PC),A0   ; A0 = adresse de retour
00180F5C  61 00 FC6A      bsr.w    LAB_00180DC8          ; Validation checksum
00180F60  65 00 031A      bcs.w    LAB_0018137C           ; Échec → erreur
00180F64  12 06           move.b   D6b,D0b               ; D0 = $06 (ACK)
00180F66  61 00 FF3E      bsr.w    FUN_00180AA8          ; Envoie ACK
00180F6A  4E 71           nop                             ; Délai
00180F6C  36 3C 7530      move.w   #$7530,D3             ; D3 = $7530 (delay ~30K cycles)

          LAB_00180F72                           ; ← boucle de délai
00180F72  53 C3           subq.w   #0x1,D3w
00180F74  64 00 FFFC      bcc.w    LAB_00180F72

00180F78  41 F9 000003E8 lea      $000003E8,A0           ; A0 = adresse vecteur/kernel
00180F7E  4E 90           jsr      (A0)                   ; Exécute le code chargé
00180F80  60 00 02FA      bra.w    LAB_0018137C           ; Fin après retour
```

### 8.6 Scanner kernel OS-9 (FUN_0018109C)

```asm
; ============================================================
; FUN_0018109C — Scanner de module kernel OS-9
; Entrée: pile = liste de descripteurs (addr:4, taille:4)
; Sortie: A0 = pointeur sur module "Kernel" ou 0 si non trouvé
; ============================================================
0018109C  48 E7 3C40      movem.l  {A2-A4 D3-D1},-(SP)  ; Sauvegarde contexte
001810A0  42 68 000C      lea      $0C(SP),A3            ; A3 → liste descripteurs
001810A4  2E 6B 0008      movea.l  $08(SP),A7            ; A7 = borne supérieure

          LAB_001810AA                           ; ← boucle principale scan
001810AA  24 13           movea.l (A3),A2            ; A2 = adresse région
001810AC  B4 2E           cmpa.l  A7,A2               ; A2 > A7 (fin) ?
001810AE  62 00 0010      bhi.w   LAB_001810C0        ; Oui → pas trouvé
001810B2  26 13           movea.l (A3),A3            ; A3 = descripteur
001810B4  26 3C           move.l  (A3)+,D3            ; D3 = adresse, A3 += 4
001810B6  36 13           move.w  (A3)+,D3w           ; D3 = taille (16 bits), A3 += 2
001810B8  61 00 FFE0      bsr.w   LAB_0018109A         ; Vérifie "Kernel"
001810BA  24 03           movea.l D3,A2               ; A2 = résultat
001810BC  66 00 FFEC      bne.w   LAB_001810AA        ; Non trouvé → next

          LAB_001810C0                           ; ← pas de kernel
001810C0  41 FA 0227      lea      (-$D9,PC)=>DAT_00181329,A0 ; "NO OS-9 kernel found.\r"
00181102  ...             bra.w    FUN_00181D02          ; Affichage message

; ============================================================
; LAB_0018109A — Vérification d'un module OS-9
; ============================================================
0018109A  48 E7 3820      movem.l  {A0-A2 D0-D2},-(SP)

          LAB_0018109E
0018109E  3E 13           move.w  (A3)+,D7w            ; D7 = taille région
001810A0  64 00 0008      bcc.w   LAB_001810AA          ; Taille 0 → skip

001810A2  22 13           movea.l (A3)+,A1             ; A1 = adresse module
001810A4  30 3C 4AFC      move.w  #$4AFC,D0w           ; D0 = magic OS-9
001810A8  B0 89           cmp.w   (A1),D0               ; Magic $4AFC à offset $00 ?
001810AA  66 00 002A      bne.w   LAB_001810D6         ; Non → skip

001810AC  7C 4A           moveq   #$4A,D6b              ; D6 = $4A (74)
001810AE  E5 C6           asr.w   #0x1,D6w              ; D6 = 37
001810B0  70 3C FFFF      moveq   #$FF,D0               ; D0 = -1 ($FFFF) accumulateur

          LAB_001810B4                           ; ← boucle checksum XOR-16
001810B4  B0 81           cmp.w   (A1)+,D0              ; XOR-16 sur mot
001810B6  66 00 001E      bne.w   LAB_001810D6          ; Checksum != 0 → reject
001810B8  51 C6 FFFA      dbf     D6w,LAB_001810B4      ; 37+1 = 38 itérations (76 octets)

001810BC  3F 04           move.w  D4,-(SP)              ; Push checksum (doit être 0)
001810BE  2A 14           movea.l (A2)+,A1              ; A1 = "Kernel"
001810C0  20 04           movea.l D4,A0                 ; A0 = base module
001810C2  32 54           movea.w $0C(A1),A1            ; A1 = offset nom dans module
001810C4  D3 C4           adda.l  A0,A1                  ; A1 = adresse absolue nom

          LAB_001810C6                           ; ← boucle comparaison nom
001810C6  12 11           move.b  (A1)+,D1b              ; Caractère nom module
001810C8  13 02 0009      move.b  D2b,$09(A2)            ; Copie pour cmp
001810CC  C2 89           addq.b  #0x1,A2                ; Advance pointeur
001810CE  E2 08           lsr.b   #0x1,D2b              ; Shift pour test fin
001810D0  64 00 FFF4      bcc.w   LAB_001810C6          ; Continue si pas fin
001810D4  20 0C           movea.l A0,A2                 ; A2 = pointeur kernel trouvé

          LAB_001810D6
001810D6  4F EF 0008      lea     $08(SP),A7            ; Cleanup pile
001810DA  4E 5E           unmovem ...                   ; Restore
001810DC  4E 75           rts                             ; A0 = kernel ou NULL
```

---

## 9. Conseils pour Stubs Émulateur

### 9.1 Stub pour `FUN_00180AEE` — Lecture ASIC

```c
// Stub pour FUN_00180AEE — lecture depuis $8000201B (ASIC extended status)
// Comportement doit simuler le polling CDIC
uint8_t emulate_read_asic_status(void) {
    // Contexte : appelé depuis FUN_00180E62 après reset → DOIT retourner $06
    // Contexte : appelé dans boucle principale → données/commandes du flux série
    // Contexte : appelé pour checksum → octet de checksum

    switch (current_boot_phase) {
        case PHASE_DIAGNOSTIC:
            return 0x06;        // ← OBLIGATOIRE : DISC_READY / SYSTEM_OK
        case PHASE_PREAMBLE:
            return 0x01;        // Préambule du paquet
        case PHASE_COMMAND:
            return next_command; // $01/$02/$04/$08
        case PHASE_DATA:
            return next_data_byte;
        case PHASE_CHECKSUM:
            return computed_checksum; // XOR-8 cumulatif
        default:
            return 0xF6;        // Identité CDIC (fallback)
    }
}
```

> ⚠️ **Piège** : Le premier appel de `FUN_00180AEE` après reset **doit retourner `$06`** impérativement. Retourner une autre valeur positive (comme `$F6` ou `$05`) déclencherait le fallback `FUN_0018068C` (memory probe + menu test) au lieu du bootstrap normal.

### 9.2 Séquence exacte d'écriture ASIC

```c
// Séquence pour configurer un mode de transfert
void asic_configure_transfer_mode(uint8_t mode) {
    // mode : 1, 2, 4, ou 8

    // Étape 1 : sélectionner registre mode de transfert
    write_byte(0x80002017, 0x0A);  // ASIC_CMD = $0A

    // Étape 2 : lire statut (attendre bit 3 = CD_READY)
    uint8_t status = read_byte(0x80002013);
    if (!(status & 0x08)) {
        // Hardware non prêt — retourner erreur
    }

    // Étape 3 : calculer double-nibble et enable bit
    uint8_t ctrl_val = mode | (mode << 4) | 0x80;  // ex. mode 5 → $55 | $80 = $D5
    write_byte(0x80002015, ctrl_val);              // ASIC_CTRL

    // Étape 4 : retour au mode registre normal
    write_byte(0x80002017, 0x05);  // ASIC_CMD = $05
}
```

### 9.3 Injection de bootstrap paquet par paquet

Pour injecter un faux module de bootstrap en mémoire via l'émulateur :

```python
# Protocole d'injection bootstrap
def inject_boot_packet(command, header, data=None):
    """
    command : $01 (DATA), $02 (ADR), $04 (EXEC), $08 (SECT)
    header  : valeur header (taille ou adresse)
    data    : octets pour DATA
    """
    checksums = []

    # 1. Préambule
    emulate_read_asic_status_preamble()  # doit retourner $01

    # 2. Commande
    emulate_read_asic_status_command(command)  # retourne la commande

    # 3. Header (via FUN_180FD0, 2 ou 4 octets big-endian)
    for byte in big_endian_bytes(header):
        emulate_read_asic_status_data(byte)  # retourne les octets
        checksum ^= byte

    # 4. Données (pour commande $01 DATA)
    if data:
        for byte in data:
            emulate_read_asic_status_data(byte)
            checksum ^= byte

    # 5. Checksum final
    emulate_read_asic_status_checksum(checksum)  # doit retourner checksum

    # 6. ACK (émulateur répond $06 après checksum valide)
    # FUN_180AA8 envoie $06 → emulate ACK response

# Exemple : charger un kernel à $00080000 et l'exécuter
def inject_kernel():
    # Paquet ADR : destination = $00080000
    inject_boot_packet(0x02, 0x00080000)

    # Paquet DATA : charger le code du kernel (ex. 19200 octets)
    inject_boot_packet(0x01, 19200, kernel_code)

    # Paquet EXEC : exécuter le kernel
    inject_boot_packet(0x04, 0x00080000)
```

### 9.4 Création d'un faux module OS-9 "Kernel"

```python
import struct

def create_fake_kernel(name="Kernel", module_size=19200, exec_offset=0x100):
    """
    Crée un faux module OS-9 valide pour le scanner FUN_0018109C.
    Le module sera reconnu par le bootstrap si :
    1. Magic $4AFC à offset $00
    2. Checksum XOR-16 sur 38 mots (offset $02–$4D) = $0000 (après XOR avec accumulateur $FFFF)
    3. Nom = "Kernel" (insensible à la casse)
    """
    module = bytearray(module_size)

    # 1. Magic word
    struct.pack_into('>H', module, 0x00, 0x4AFC)

    # 2. Calculer checksum XOR-16 sur 38 mots (76 octets)
    # Accumulateur initialisé à $FFFF
    checksum_acc = 0xFFFF
    for i in range(38):  # 38 mots
        word = struct.unpack_from('>H', module, 0x02 + i * 2)[0]
        checksum_acc ^= word

    # Écrire un mot de compensation pour que le résultat final soit 0
    # En XOR-ant le checksum_acc lui-même, on obtient 0
    struct.pack_into('>H', module, 0x02, checksum_acc ^ 0xFFFF)

    # 3. Nom du module (insensible à la casse via andi.b #$DF)
    # offset $0C du module = position du nom
    name_bytes = name.encode('ascii')
    for i, c in enumerate(name_bytes):
        module[0x0C + i] = c

    # 4. Offset d'exécution (à l'offset standard OS-9)
    struct.pack_into('>I', module, 0x10, exec_offset)

    return bytes(module)

# Utilisation
kernel = create_fake_kernel("Kernel", module_size=19200, exec_offset=0x100)
# Écrire à l'adresse $00080000 en RAM
write_ram(0x00080000, kernel)
```

### 9.5 Points d'arrêt recommandés pour debugging émulateur

| Adresse | Fonction | Raison |
|:---|:---|:---|
| `0x00180E82` | `LAB_00180E82` | Contrôle de la condition de boot — vérifie D0 == $06 |
| `0x00180B5A` | `FUN_00180B44` | Début de la séquence de configuration ASIC |
| `0x00180FE4` | `FUN_00180FD0` | Rotation de l'accumulateur D3 — vérifie le big-endian assembly |
| `0x00180FF8` | `FUN_00180FF0` | Comparaison de checksum — point critique de validation |
| `0x00180F7E` | `LAB_00180F58` | `jsr (A0)` — moment où le contrôle passe au code chargé |
| `0x0018109C` | `FUN_0018109C` | Entrée du scanner kernel — début de la recherche OS-9 |
| `0x001810A8` | `LAB_0018109A` | Test du magic `$4AFC` — validation de structure module |
| `0x001810C6` | `LAB_001810C6` | Comparaison de nom — debug de la reconnaissance "Kernel" |

---

## 10. Synthèse — Graph de Boot ASCII

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         CD-i 205 — BOOT SEQUENCE                            │
│                     Firmware PS-7211 REL.2.1 — 68000                        │
└─────────────────────────────────────────────────────────────────────────────┘

  RESET MATÉRIEL
       │
       ▼
  ┌─────────────────────────────────────────────────────────────┐
  │ FUN_00180E62  — Point d'entrée post-reset                   │
  │   • Initialise base RAM ($0008_0000)                         │
  │   • Charge base ASIC ($8000_2011) dans A1                   │
  └────────────┬────────────────────────────────────────────────┘
               │ bsr.w FUN_00180AEE
               ▼
  ┌─────────────────────────────────────────────────────────────┐
  │ FUN_00180AEE  — Polling ASIC ($8000201B)                    │
  │   • Lecture registre de statut étendu                       │
  │   • Retourne -1 (retry), $06 (OK), ou autre (fallback)     │
  └────────────┬────────────────────────────────────────────────┘
               │
     ┌─────────┴─────────┐
     │                   │
     ▼                   ▼
  ┌────────────┐   ┌─────────────────────────────────────────────────────────┐
  │ D0 == $06  │   │ D0 != $06                                                │
  └─────┬──────┘   └──────────────────┬────────────────────────────────────────┘
        │                             │
        ▼                             ▼
  ┌─────────────────────────┐   ┌──────────────────────────────────────────────┐
  │ LAB_00180E8E             │   │ FUN_0018068C  — Fallback                     │
  │ • D3 = $4B00 (19200)     │   │   • Memory probe                            │
  │ • A4 = -1 (dest)         │   │   • Menu test / diagnostic hardware         │
  └────────────┬────────────┘   └──────────────────────────────────────────────┘
               │
               │ bsr.w FUN_00180B44
               ▼
  ┌─────────────────────────────────────────────────────────────┐
  │ FUN_00180B44  — Configuration mode de transfert             │
  │   • ASIC_CMD = $0A (mode transfert)                         │
  │   • Attente CD_READY (bit 3 de ASIC_STATUS)                 │
  │   • ASIC_CTRL = double_nibble | $80 (validation + enable)  │
  │   • ASIC_CMD = $05 (retour mode normal)                     │
  └────────────┬────────────────────────────────────────────────┘
               │
               ▼
  ┌─────────────────────────────────────────────────────────────┐
  │ FUN_00180E9E  — Machine à états du bootstrap primaire       │
  │   • D2 = 0 (checksum XOR cumulatif)                         │
  │   • D3 = $4B00 (taille par défaut)                          │
  │   • A4 = -1 (adresse destination par défaut)                │
  │                                                             │
  │   BOUCLE PRINCIPALE:                                        │
  │     1. Lecture préambule $01 (retry jusqu'à $01)            │
  │     2. Lecture commande ($01/$02/$04/$08)                   │
  │     3. Lecture header big-endian (FUN_180FD0)               │
  │     4. Validation checksum (FUN_180FF0)                     │
  │     5. ACK $06 ou NAK → reprise boucle                     │
  └────┬────────┬────────┬────────┬─────────────────────────────┘
       │        │        │        │
       ▼        ▼        ▼        ▼
  ┌─────────┐┌─────────┐┌─────────┐┌─────────┐
  │ CMD $01 ││ CMD $02 ││ CMD $04 ││ CMD $08 │
  │ DATA    ││ ADR     ││ EXEC    ││ SECT    │
  └────┬────┘└────┬────┘└────┬────┘└────┬────┘
       │        │        │        │
       ▼        │        │        │
  ┌────────────────┐     │        │
  │ Lecture header │     │        │
  │ 2 octets (BE)  │     │        │
  │ → D3 = taille  │     │        │
  └────┬───────────┘     │        │
       │                 │        │
       ▼                 │        │
  ┌──────────────────────┐         │
  │ Boucle dbf D3        │         │
  │ Lecture octets       │         │
  │ Store → (A4)+        │         │
  └──────────────────────┘         │
                                   │
                                   ▼
                        ┌────────────────┐
                        │ Lecture header │
                        │ 4 octets (BE)  │
                        │ → A4 = adresse │
                        └────────────────┘
                                          │
                                          ▼
                           ┌─────────────────────────────────────┐
                           │ • ACK $06 via FUN_00180AA8          │
                           │ • Délai $7530 cycles                │
                           │ • jsr (A0) → $000003E8              │
                           │   (saute vers code chargé)           │
                           │ • Retour → FIN ou message erreur    │
                           └─────────────┬───────────────────────┘
                                         │
                                         ▼
                    ┌──────────────────────────────────────────────┐
                    │ FUN_0018109C  — Scanner de Kernel OS-9      │
                    │   • Parcours régions mémoire (pile)         │
                    │   • Recherche magic $4AFC                   │
                    │   • Validation checksum 38 mots (XOR-16)   │
                    │   • Comparaison nom "Kernel" (insens. cas) │
                    │   • A0 = pointeur sur module Kernel         │
                    │   • OU affichage "NO OS-9 kernel found."   │
                    └──────────────────────┬───────────────────────┘
                                           │
                                           ▼
                              ┌────────────────────────┐
                              │ KERNEL OS-9 ACTIVÉ     │
                              │ Exécution du système   │
                              └────────────────────────┘
```

### Flux résumé

```
Reset → FUN_180E62 → FUN_180AEE (poll $06?)
  └── OK → FUN_180B44 → FUN_180E9E (boucle principale)
              ├── CMD $01 (DATA) → stocke octets en RAM via A4
              ├── CMD $02 (ADR)  → change A4 (destination)
              ├── CMD $04 (EXEC) → jsr vers code chargé
              └── CMD $08 (SECT) → fallback

      Après EXEC → FUN_18109C (scan kernel OS-9)
              └── A0 = "Kernel" (magic $4AFC, checksum 38 mots, nom)
                  └── Kernel OS-9 en cours d'exécution
```

---

*Document généré le 23 juin 2026 — Rétro-ingénierie CD-i 205 PS-7211 REL.2.1*
*Toutes les adresses sont en hexadécimal, big-endian pour les multi-octets*