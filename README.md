# Philips CD-i 205 Émulateur

![Status](https://img.shields.io/badge/status-recherche_active-yellow)
![Lang](https://img.shields.io/badge/lang-Python%20%7C%20C%2FC%2B%2B-blue)

Émulateur complet pour la console **Philips CD-i 205**, développé à partir de matériel authentique.

> **⚠️ État actuel :** Ce projet est en phase de **rétro-ingénierie** et d'analyse des ROMs.  
> Le cœur d'émulation en C/C++ n'est pas encore commencé.

---

## 🎯 Objectif

Émuler fidèlement la **Philips CD-i 205** que je possède, ainsi que son extension **Full Motion Video (FMV)** (*Digital Video Cartidge*).

Le firmware BIOS de la console principale n'étant pas encore dumpé, la recherche débute par l'étude de la **carte FMV**, qui dispose de sa propre ROM et d'une architecture indépendante.

---

## 🗂️ Structure du dépôt
```text
Philips-CD-I-205-Emulateur/
├── Digital-Video-Cartidge_(FMV)/   # Recherches, dumps et notes sur la carte FMV
├── tools/                           # 🐍 Scripts Python d'analyse de ROMs (en cours)
├── src/                             # ⏳ (à venir) Cœur d'émulation C/C++
└── docs/                            # Documentation technique et découvertes
```

---

## 🛠️ Stack technique

| Phase | Langage | Outils | Statut |
|-------|---------|--------|--------|
| Analyse ROMs | Python 3 | Scripts maison | 🟡 En cours |
| Reverse Engineering | - | Ghidra | 🟡 En cours |
| Cœur émulation | C / C++ | CMake (prévu) | 🔴 Non commencé |

---

## 🚀 Feuille de route

- [x] Acquisition et documentation du matériel (CD-i 205 + carte FMV)
- [ ] Analyse approfondie de la ROM de la carte FMV
- [ ] Dump du BIOS de la console principale
- [ ] Développement du CPU core (Motorola 68000)
- [ ] Implémentation des sous-systèmes audio / vidéo / CD
- [ ] Émulation de la carte FMV (décodage MPEG)
- [ ] Interface utilisateur (GUI)

---

## ⚠️ Informations légales

Ce dépôt ne contient **aucun BIOS, firmware ou ROM sous copyright**.  
Chaque utilisateur devra utiliser ses propres dumps extraits de son matériel original.

---

## 🙏 Remerciements

- Merci à **Claude code**.