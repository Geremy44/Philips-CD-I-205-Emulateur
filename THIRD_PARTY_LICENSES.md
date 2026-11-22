# Licences des composants tiers

L'émulateur contient ou s'inspire des travaux suivants. Leurs mentions de
copyright et leurs licences sont reproduites ici, comme ces licences l'exigent
pour la distribution du code source et des exécutables.

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

## ROM

Aucune ROM n'est fournie avec ce dépôt. Les ROM du lecteur CD-i et de la
cartouche FMV doivent provenir du matériel de l'utilisateur.
