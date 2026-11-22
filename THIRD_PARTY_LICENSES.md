# Licences des composants tiers

L'émulateur contient ou s'inspire des travaux suivants. Leurs mentions de
copyright et leurs licences sont reproduites ici, comme ces licences l'exigent
pour la distribution du code source et des exécutables. Les textes complets
des licences des bibliothèques livrées avec la version Windows sont dans le
dossier [`licenses/`](licenses/).

---

## pl_mpeg

- **Fichier :** `src/cdi_emu/core/FMV/pl_mpeg.h`, inclus avec une seule
  modification, signalée dans le fichier (contrôle d'en-tête audio assoupli :
  stéréo et stéréo jointe peuvent alterner)
- **Rôle :** décodage des images MPEG-1 et du son MPEG-1 Layer II de la
  cartouche FMV
- **Auteur :** Dominic Szablewski — <https://github.com/phoboslab/pl_mpeg>
- **Licence :** MIT

```
The MIT License (MIT)

Copyright (c) 2019 Dominic Szablewski

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## MAME — émulation de la cartouche Digital Video (FMV)

- **Fichiers dérivés :** `src/cdi_emu/core/FMV/fmv.c`, `fmv_video.c`,
  `fmv_audio.c`, `mpeg_demux.c`, `mpeg_demux.h`
- **Origine :** demande de fusion MAME
  [#16181](https://github.com/mamedev/mame/pull/16181), périphériques
  `cdidvc`, `mcd251`, `gsc38gg307` et `mpeg_demux`. Leur comportement a
  lui-même été établi à partir du cœur FPGA CDi_MiSTer d'Andre Zeps.
- **Nature :** portage en C du fonctionnement de ces périphériques (carte
  des registres, bits d'état et d'interruption, règles de démarrage, de
  manque de données et de cadence, automate du démultiplexeur MPEG)
- **Licence :** BSD-3-Clause

```
Copyright (c) Alexandre Derumier

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## Bibliothèques livrées avec la version Windows

Le build portable (releases) contient, sans modification, les DLL suivantes
issues des paquets [MSYS2](https://www.msys2.org/) UCRT64. L'émulateur les
charge dynamiquement ; chacune peut être remplacée par une autre version
compatible. Les textes complets de leurs licences sont dans le dossier
[`licenses/`](licenses/), joint aussi au zip des releases.

| DLL | Projet (version) | Licence | Texte |
|-----|------------------|---------|-------|
| `SDL2.dll` | [SDL](https://www.libsdl.org/) 2.28.5 — Sam Lantinga | zlib | `licenses/SDL2/` |
| `SDL2_ttf.dll` | [SDL_ttf](https://github.com/libsdl-org/SDL_ttf) 2.20.2 — Sam Lantinga | zlib | `licenses/SDL2_ttf/` |
| `libfreetype-6.dll` | [FreeType](https://freetype.org/) 2.13.2 | FreeType License (FTL) | `licenses/freetype/` |
| `libharfbuzz-0.dll` | [HarfBuzz](https://harfbuzz.github.io/) 8.3.0 | « Old MIT » | `licenses/harfbuzz/` |
| `libgraphite2.dll` | [Graphite](https://github.com/silnrsi/graphite) 1.3.14 — SIL International | LGPL 2.1 (ou MPL / GPL) | `licenses/graphite2/` |
| `libglib-2.0-0.dll` | [GLib](https://gitlab.gnome.org/GNOME/glib) 2.78.3 | LGPL 2.1 ou ultérieure | `licenses/glib2/` |
| `libintl-8.dll` | [GNU gettext](https://www.gnu.org/software/gettext/) 0.22.4 (libintl) | LGPL 2.1 ou ultérieure | `licenses/gettext-intl/` |
| `libiconv-2.dll` | [GNU libiconv](https://www.gnu.org/software/libiconv/) 1.17 | LGPL 2.1 ou ultérieure | `licenses/libiconv/` |
| `libpcre2-8-0.dll` | [PCRE2](https://github.com/PCRE2Project/pcre2) 10.42 — University of Cambridge | BSD-3-Clause | `licenses/pcre2/` |
| `libpng16-16.dll` | [libpng](http://www.libpng.org/pub/png/libpng.html) 1.6.40 | PNG Reference Library License v2 | `licenses/libpng/` |
| `zlib1.dll` | [zlib](https://zlib.net/) 1.3 — Jean-loup Gailly, Mark Adler | zlib | `licenses/zlib/` |
| `libbz2-1.dll` | [bzip2](https://sourceware.org/bzip2/) 1.0.8 — Julian Seward | BSD (bzip2) | `licenses/bzip2/` |
| `libbrotlicommon.dll`, `libbrotlidec.dll` | [Brotli](https://github.com/google/brotli) 1.1.0 — Google | MIT | `licenses/brotli/` |
| `libgcc_s_seh-1.dll`, `libstdc++-6.dll` | [GCC](https://gcc.gnu.org/) 13.2.0, bibliothèques d'exécution | GPL 3 avec exception GCC Runtime Library | `licenses/gcc-libs/` |
| `libwinpthread-1.dll` | [mingw-w64 winpthreads](https://www.mingw-w64.org/) 11.0.0 | MIT / BSD | `licenses/winpthreads/` |

**FreeType** — Portions of this software are copyright © 2023 The FreeType
Project (www.freetype.org). All rights reserved.

**Bibliothèques sous LGPL** (GLib, gettext, libiconv, Graphite) : elles sont
utilisées sans modification et liées dynamiquement ; on peut remplacer la DLL
par sa propre version. Leur code source correspondant est disponible auprès
des projets ci-dessus et dans les recettes de paquets MSYS2
(<https://github.com/msys2/MINGW-packages>), aux versions indiquées.

Aucune police n'est fournie : l'émulateur utilise celles du système
(Consolas, Segoe UI ou Arial sous Windows, DejaVu sous Linux).

---

## ROM

Aucune ROM n'est fournie avec ce dépôt. Les ROM du lecteur CD-i et de la
cartouche FMV doivent provenir du matériel de l'utilisateur.
