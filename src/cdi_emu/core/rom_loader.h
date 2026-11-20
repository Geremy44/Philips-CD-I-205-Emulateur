/*
 * rom_loader.h - ROM image loader
 *
 * Supports loading ROM dumps in Intel HEX format (used for the
 * CD-i 205 firmware EEPROM dumps) and raw binary format.
 */

#ifndef ROM_LOADER_H
#define ROM_LOADER_H

#include <stdint.h>
#include <stddef.h>

/* Load an Intel HEX file; returns malloc'd buffer or NULL on failure */
uint8_t *load_rom_hex(const char *path, size_t *size);

/*
 * Load a raw binary ROM file.
 * byteswap_words = 1: swap each 16-bit word (big-endian <-> little-endian)
 * byteswap_words = 0: load as-is
 */
uint8_t *load_rom_bin(const char *path, size_t *size, int byteswap_words);

/* Auto-detect format by file extension (.hex or .bin) */
uint8_t *load_rom(const char *path, size_t *size);

#endif /* ROM_LOADER_H */
