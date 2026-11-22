/*
 * fmv.h - Digital Video Cartridge (DVC / "FMV", 22ER9141) of the CD-i 205
 *
 * Hardware (service manual, block diagram 4.4): FMA host chip + DSP56001
 * (MPEG audio), FMV chip + 512 KB DRAM (MPEG-1 video), 1 MB of extra DRAM
 * with its controller, EPROMs P7307 / P7308 (OS-9 modules: sysgo, fmvll,
 * madriv / ma at $E03000, fmvdrv / mv at $E04000, dspcode, MoviMan...).
 *
 * Address map used here (from the drivers' own range checks):
 *   $E40000-$E5FFFF  cartridge ROM (128 KB)
 *   $E80000-$EFFFFF  video DRAM (512 KB)
 *   $E00000-$E3FFFF  chip registers (logged, being reverse-engineered)
 *
 * Optional: off unless enabled (menu Materiel, saved in options.ini, or
 * EMU_FMV=1 / 0 which overrides it).
 */

#ifndef FMV_H
#define FMV_H

#include <stdint.h>

/* Load the cartridge ROM (does not insert it). Returns 0 if found. */
int  fmv_init(const char *rom_path);
int  fmv_rom_loaded(void);
/* Insert / remove the cartridge (takes effect at the next CD-i reset:
 * CD-RTOS looks for its modules at boot) */
void fmv_set_enabled(int on);
int  fmv_enabled(void);

/* Saved choice (options.ini next to the executable): -1 = not set */
int  fmv_option_load(void);
void fmv_option_save(int on);

int      fmv_handles(uint32_t addr);       /* address inside the cartridge */
uint8_t  fmv_read8(uint32_t addr);
void     fmv_write8(uint32_t addr, uint8_t v);

/* 16-bit accesses (the chips have word registers; reading a status
 * register clears it, so a word must not be split in two byte reads) */
uint16_t fmv_read16(uint32_t addr);
void     fmv_write16(uint32_t addr, uint16_t v);

/* CD-i reset: chips back to their power-on state */
void fmv_reset(void);
/* time: CPU clock periods elapsed; fmv_frame() at each vertical retrace */
void fmv_tick(int cycles);
void fmv_frame(void);
/* the 68070 DMA channel 2 has been started (bus.c) */
void fmv_dma_started(void);
/* interrupt request: level (4) or 0, and the vector */
int  fmv_irq(int *vector);
/* MPEG picture: chip pixel coordinates (384 across the active line) */
int  fmv_video_pixel(int x, int y, uint32_t *rgb);
int  fmv_video_showing(void);

#endif
