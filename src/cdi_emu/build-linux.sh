#!/bin/sh
# CD-i 205 emulator - build for Linux (gcc + SDL2 + SDL2_ttf)
#   Debian / Ubuntu : sudo apt install build-essential pkg-config libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core
#   Fedora          : sudo dnf install gcc make pkgconf-pkg-config SDL2-devel SDL2_ttf-devel dejavu-sans-mono-fonts
#   Arch            : sudo pacman -S base-devel sdl2 sdl2_ttf ttf-dejavu
# Usage: ./build-linux.sh   (from src/cdi_emu) -> bin/cdi205
set -e
cd "$(dirname "$0")"
mkdir -p bin
CFLAGS="-O2 -std=c99 -D_DEFAULT_SOURCE -I. -Icore -Icore/CPU -Icore/GPU -Icore/SLAVE -Icore/CDIC $(pkg-config --cflags sdl2 SDL2_ttf)"
LIBS="$(pkg-config --libs sdl2 SDL2_ttf) -lm"
# same source list as build-project.bat
SRCS=$(grep -oE '^[[:space:]]+[A-Za-z0-9_/.]+\.c' build-project.bat | tr -d ' \r')
for c in $SRCS; do
    echo "  $c"
    gcc -c $CFLAGS "$c" -o "bin/$(basename "${c%.c}").o"
done
gcc -o bin/cdi205 bin/*.o $LIBS
echo "OK: bin/cdi205"
