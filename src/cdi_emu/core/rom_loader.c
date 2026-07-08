#include "rom_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ✅ Prototype anticipé (forward declaration) */
static int hex_char(char c);

uint8_t *load_rom_hex(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "r");
    if (!f) {
        perror("fopen");
        return NULL;
    }

    /* Allouer 512 Ko pour la ROM */
    uint8_t *rom = malloc(0x80000);
    if (!rom) {
        fclose(f);
        return NULL;
    }
    memset(rom, 0xFF, 0x80000);  /* EPROM vierge */

    char line[1024];
    uint16_t extended_addr = 0;  /* adresse étendue (type 04) */

    while (fgets(line, sizeof(line), f)) {
        if (line[0] != ':') continue;

        /* Parser l'enregistrement IHEX */
        int byte_count = (hex_char(line[1]) << 4) | hex_char(line[2]);
        int addr = (hex_char(line[3]) << 12) | (hex_char(line[4]) << 8)
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
                    
                    /* ✅ SWAP EN LIVE : inverser pairs/impairs pour big-endian */
                    if (rom_offset < 0x80000) {
                        if (rom_offset & 1) {
                            /* offset impair → c'est l'octet bas → doit aller en pair */
                            rom[rom_offset - 1] = byte_val;
                        } else {
                            /* offset pair → c'est l'octet haut → doit aller en impair */
                            rom[rom_offset + 1] = byte_val;
                        }
                    }
                }
                break;
            }

            case 0x01:  /* End Of File */
                fclose(f);
                *out_size = 0x80000;
                return rom;

            case 0x04:  /* Extended Linear Address */
            {
                extended_addr = (hex_char(line[9]) << 12)
                              | (hex_char(line[10]) << 8)
                              | (hex_char(line[11]) << 4)
                              | hex_char(line[12]);
                break;
            }

            case 0x05:  /* Start Linear Address (ignoré) */
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

/* Helper pour convertir un caractère hex */
static int hex_char(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}
