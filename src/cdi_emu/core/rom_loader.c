/*
 * rom_loader.c - ROM image loader for Intel HEX format
 *
 * Parses Intel HEX records and builds a 512 KB ROM image.
 * Performs byte-swapping to convert from the file's little-endian
 * byte order to the big-endian order expected by the M68000 bus.
 */

#include "rom_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_char(char c);

/*
 * Load an Intel HEX file and return a 512 KB ROM buffer.
 * The caller is responsible for freeing the returned buffer.
 * Returns NULL on failure.
 */
uint8_t *load_rom_hex(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("fopen");
        return NULL;
    }

    /* Allocate 512 KB for the ROM image */
    uint8_t *rom = malloc(0x80000);
    if (!rom) {
        fclose(f);
        return NULL;
    }
    memset(rom, 0xFF, 0x80000);  /* Fill with virgin EPROM state */

    char line[1024];
    uint16_t extended_addr = 0;  /* Upper 16 bits from type-04 records */

    while (fgets(line, sizeof(line), f)) {
        if (line[0] != ':') continue;

        /* Parse the IHEX record header */
        int byte_count  = (hex_char(line[1]) << 4) | hex_char(line[2]);
        int addr        = (hex_char(line[3]) << 12) | (hex_char(line[4]) << 8)
                       | (hex_char(line[5]) << 4)  | hex_char(line[6]);
        int record_type = (hex_char(line[7]) << 4) | hex_char(line[8]);

        switch (record_type) {
            case 0x00:  /* Data record */
            {
                uint32_t full_addr = (extended_addr << 16) | addr;

                for (int i = 0; i < byte_count; i++) {
                    int byte_hex = (hex_char(line[9 + i*2]) << 4)
                                 | hex_char(line[9 + i*2 + 1]);
                    uint8_t byte_val = (uint8_t)byte_hex;

                    uint32_t rom_offset = full_addr + i;

                    /*
                     * Byte-swap adjacent pairs to convert from the file's
                     * byte order to big-endian (the M68000 reads high byte
                     * first at even addresses).
                     */
                    if (rom_offset < 0x80000) {
                        if (rom_offset & 1) {
                            /* Odd offset (low byte) -> store at preceding even address */
                            rom[rom_offset - 1] = byte_val;
                        } else {
                            /* Even offset (high byte) -> store at following odd address */
                            rom[rom_offset + 1] = byte_val;
                        }
                    }
                }
                break;
            }

            case 0x01:  /* End Of File record */
                fclose(f);
                *out_size = 0x80000;
                return rom;

            case 0x04:  /* Extended Linear Address record */
            {
                extended_addr = (hex_char(line[9])  << 12)
                              | (hex_char(line[10]) << 8)
                              | (hex_char(line[11]) << 4)
                              | hex_char(line[12]);
                break;
            }

            case 0x05:  /* Start Linear Address (ignored - we use ROM vectors) */
                break;

            default:
                fprintf(stderr, "Unknown record type: %02X\n", record_type);
                break;
        }
    }

    fclose(f);
    *out_size = 0x80000;
    return rom;
}

/* Convert a single hex character to its numeric value (0-15) */
static int hex_char(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}
