/*
 * cdic.c - CD Interface Controller (CDIC) emulation
 *
 * The CDIC manages CD-ROM data access, sector buffering, and
 * audio CD playback. This is a minimal stub providing RAM storage
 * and a single register value needed to pass an early boot check.
 */

#include "cdic.h"
#include <string.h>

/* CDIC register that returns a reset value during boot diagnostics */
#define CDIC_REG2_ADDR      0x303FFA
#define CDIC_REG2_RESET     0xD7FE

/* Initialize the CDIC to empty state */
void cdic_init(cdic_t *c) {
    memset(c->ram, 0, sizeof(c->ram));
}

/* Check if an address falls within CDIC RAM ($300000-$303FFF) */
int cdic_is_ram(uint32_t addr) {
    return (addr >= CDIC_RAM_BASE && addr < CDIC_RAM_BASE + CDIC_RAM_SIZE);
}

/* Read a byte from CDIC RAM */
uint8_t cdic_ram_read8(cdic_t *c, uint32_t addr) {
    return c->ram[addr - CDIC_RAM_BASE];
}

/* Write a byte to CDIC RAM */
void cdic_ram_write8(cdic_t *c, uint32_t addr, uint8_t v) {
    c->ram[addr - CDIC_RAM_BASE] = v;
}

/*
 * 16-bit register read.
 * Currently only handles the reset detection register at $303FFA,
 * which returns a fixed value during the boot diagnostic sequence.
 */
uint16_t cdic_read16(uint32_t addr) {
    switch (addr) {
        case CDIC_REG2_ADDR:
            return CDIC_REG2_RESET;   /* Passes the boot diagnostic check */
        default:
            return 0x0000;
    }
}
