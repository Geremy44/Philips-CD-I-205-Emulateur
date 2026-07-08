#ifndef ROM_LOADER_H
#define ROM_LOADER_H

#include <stdint.h>
#include <stddef.h>

/* Chargement Intel HEX (dumps EEPROM / .hex) */
uint8_t *load_rom_hex(const char *path, size_t *size);

/* Chargement binaire pur (.bin)
   byteswap_words = 1 : inverse chaque paire d'octets (swap 16 bits)
   byteswap_words = 0 : laisse le fichier tel quel */
uint8_t *load_rom_bin(const char *path, size_t *size, int byteswap_words);

/* Détecte auto selon l'extension (.hex ou autre) */
uint8_t *load_rom(const char *path, size_t *size);

#endif
