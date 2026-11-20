/*
 * cdic.h - CD Interface Controller (CDIC) emulation
 *
 * The CDIC manages CD-ROM data access, sector buffering,
 * and audio CD playback. This is a minimal stub providing
 * RAM storage and one register needed to pass early boot checks.
 */

#ifndef CDIC_H
#define CDIC_H
#include <stdint.h>

/* CDIC RAM region in the CPU address space */
#define CDIC_RAM_BASE  0x00300000
#define CDIC_RAM_SIZE  0x00004000   /* 16 KB */

/* CDIC state (currently only RAM; registers to be added) */
typedef struct {
    uint8_t ram[CDIC_RAM_SIZE];
    /* TODO: registers, DMA state, audio queue, etc. */
} cdic_t;

/* Initialize the CDIC to empty state */
void cdic_init(cdic_t *c);

/* Check if an address falls within CDIC RAM */
int cdic_is_ram(uint32_t addr);

/* CDIC RAM byte-level access */
uint8_t cdic_ram_read8 (cdic_t *c, uint32_t addr);
void    cdic_ram_write8(cdic_t *c, uint32_t addr, uint8_t v);

/* 16-bit register read (currently only returns a reset value) */
uint16_t cdic_read16(uint32_t addr);

#endif /* CDIC_H */
