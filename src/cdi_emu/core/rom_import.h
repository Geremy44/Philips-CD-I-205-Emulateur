/*
 * rom_import.h - import of the user's ROM dumps into ROM/
 *
 * Accepts Intel HEX or raw binary files, recognises them by size and
 * checksum, merges split EPROMs and writes the images the emulator loads:
 *   ROM/CDI205-00_PS-7211_REL.2.1.bin   console ROM (512 KB)
 *   ROM/FMV_combined_7308hi_7307lo.bin  Digital Video Cartridge ROM (128 KB)
 */

#ifndef ROM_IMPORT_H
#define ROM_IMPORT_H

#include <stddef.h>

#define ROM_CONSOLE_FILE "CDI205-00_PS-7211_REL.2.1.bin"
#define ROM_FMV_FILE     "FMV_combined_7308hi_7307lo.bin"

/* What an import wrote (bit mask) */
enum { ROM_IMPORTED_CONSOLE = 1, ROM_IMPORTED_FMV = 2 };

/* Import the given files into rom_dir. msg receives a French report, one
 * line per file and per ROM written. Returns the ROM_IMPORTED_* mask
 * (0: nothing usable). */
int rom_import_files(const char *const *paths, int n, const char *rom_dir,
                     char *msg, size_t msg_size);

#endif
