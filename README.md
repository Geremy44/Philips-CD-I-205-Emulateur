# 🧠⚡ PHILIPS CD-i 205 EMULATOR
### _[ rétro-ingénierie de la machine // reconstruction du passé ]_

<p align="center">
  <img src="https://img.shields.io/badge/statut-reverse%20engineering-ff00ff?style=for-the-badge" />
  <img src="https://img.shields.io/badge/coeur-non%20implémenté-ff0033?style=for-the-badge" />
  <img src="https://img.shields.io/badge/cible-CD--i205-00ffff?style=for-the-badge" />
</p>

---

<p align="center">
  <img src="https://media.giphy.com/media/xT9IgzoKnwFNmISR8I/giphy.gif" width="500"/>
</p>

---

## 🧬 APERÇU DU SYSTÈME

```
> séquence de boot inconnue
> matériel partiellement cartographié
> pipeline vidéo fragmenté
> cartouche fmv en cours d’analyse
```

ce projet vise à reconstruire une **Philips CD-i 205** à partir de données brutes  
aucun sdk. aucune doc exploitable. aucun raccourci.

seulement :
- des ROMs dumpées
- du désassemblage
- du comportement hardware

---

## ⚡ MISSION

```
[1] extraire du signal à partir du silence
[2] décoder une logique propriétaire
[3] émuler l’inémulé
```

---

## 🧠 ÉTAT ACTUEL

```
analyse ROM        ████████░░░░░░░░░░
reverse engineering ██████░░░░░░░░░░░░
cartographie hw    ███░░░░░░░░░░░░░░░
coeur émulation    ░░░░░░░░░░░░░░░░░░
```

---

## 📼 CARTOUCHE FMV — CIBLE PRINCIPALE

```
> hardware vidéo dédié
> décodage mpeg (comportement non standard)
> agit comme coprocesseur
> encore partiellement inconnu
```

ce composant est le **principal verrou technique**

---

## 🗂 STRUCTURE

```
.
├── Digital-Video-Cartridge_(FMV)/
├── tools/
├── docs/
└── README.md
```

---

## 🛠 OUTILS

```
RÉTRO-INGÉNIERIE → GHIDRA
SCRIPTS          → PYTHON 3
FUTUR COEUR      → C / C++
```

---

## 🧪 EXÉCUTION

```
python tools/analyze_rom.py rom.bin
```

---

## ⚠️ AVERTISSEMENT

```
aucun émulateur fonctionnel
aucun boot
aucune sortie vidéo
```

phase actuelle : **recherche pure**

---

## 🕶 PHILOSOPHIE

```
ne pas imiter
ne pas approximer
tout comprendre
```

---

## 📡 JOURNAL

```
[2026-06-18] début extraction ROM
[2026-06-19] premières passes de désassemblage
[2026-06-20] identification routines vidéo
```

---

## ⚖️ LÉGAL

```
aucun bios fourni
aucun contenu propriétaire inclus
dump matériel personnel requis
```

---

## 🧠 FAITS CONNUS

```
> cpu: motorola 68000
> support: cd interactif
> vidéo: mpeg assisté matériel
> documentation: quasi inexistante
```

---

<p align="center">
  <img src="https://media.giphy.com/media/26tn33aiTi1jkl6H6/giphy.gif" width="400"/>
</p>

---

## 🧩 STATUT FINAL

```
[ MODE RECHERCHE ACTIF ]
[ SYSTÈME NON MAÎTRISÉ ]
[ RECONSTRUCTION EN COURS ]
```

---

### // EOF
