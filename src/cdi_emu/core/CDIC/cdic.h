/*
 * cdic.h - CD Interface Controller (CDIC, IMS66490) of the CD-i 205
 *
 * Model built from the ROM driver "cdapdriv" (descriptors "cd" and "ap":
 * port $300000, vector 128, level 4). 16 KB at $300000:
 *
 *   $0000-$3BFF  RAM: sector buffers, buffer N at (N & 5) * $A00:
 *                +$000 header (MM SS FF mode), +$004 subheader x2,
 *                +$00C data ... (sector bytes 12..2351, 2340 bytes)
 *                +$924 subcode Q, 12 words (value in the low byte)
 *   $3C00  command     $23/$24 reset mode 1/2, $27 read TOC, $28 play CD-DA,
 *                      $29 read mode 1, $2A read mode 2, $2B stop?,
 *                      $2C seek, $2E update (new filters on the fly)
 *   $3C02  time        32 bits, MM SS FF 00 in BCD (absolute time)
 *   $3C06  file        file number in the high byte (mode 2 filter)
 *   $3C08  channel     32-bit channel mask (mode 2 data)
 *   $3C0C  audio chan  audio channel mask
 *   $3FF4  ABUF        bit 15 = audio buffer ready (cleared by a read)
 *   $3FF6  XBUF        bit 15 = data buffer ready  (cleared by a read)
 *   $3FF8  DMA control
 *   $3FFA  AUDCTL      bit 13 = audio interrupt
 *   $3FFC  IVEC        interrupt vector (low byte)
 *   $3FFE  DBUF        write: bit 15 = start command, bit 14 = interrupt enable
 *                      read:  bits 0/2 = buffer holding the last sector,
 *                             bit 15 cleared once the command runs,
 *                             bits 6/8 = errors, bit 7 = subcode Q invalid
 */

#ifndef CDIC_H
#define CDIC_H

#include <stdint.h>
#include "disc.h"
#include "adpcm.h"

/* Decoded audio goes to this callback (stereo, 37.8 kHz) */
typedef void (*cdic_audio_cb)(void *ctx, const int16_t *stereo, int frames);

#define CDIC_RAM_BASE  0x00300000
#define CDIC_RAM_SIZE  0x00004000   /* 16 KB, registers from $3C00 */
#define CDIC_IRQ_LEVEL 4

typedef struct cdic_s {
    uint8_t  ram[CDIC_RAM_SIZE];

    /* registers */
    uint16_t command, file, audio_channel;
    uint16_t run_cmd;          /* read in progress (27..2C); $2E only updates filters */
    uint32_t time, channel;
    uint16_t abuf, xbuf, dmactl, audctl, ivec, dbuf;

    /* drive */
    disc_t   disc;
    int      busy;          /* a read / TOC / play command is delivering sectors */
    int      lba;           /* next sector */
    int      buffer;        /* next data buffer (0/1) */
    int      abuffer;       /* next audio buffer (4/5) */
    adpcm_state_t adpcm;
    cdic_audio_cb audio_cb;
    void    *audio_ctx;
    long     audio_sectors;

    /* sound maps played from memory ("ap": AUDCTL = $2800) */
    int      mp_active;     /* playback running */
    int      mp_playing;    /* a buffer is being played */
    int      mp_buf;        /* 0 = $2800, 1 = $3200 */
    int      mp_countdown;  /* cycles left in the current buffer */
    adpcm_state_t mp_adpcm;
    long     mp_buffers;
    int      toc_pos;
    int      countdown;     /* cycles until the next sector / completion */
    int      oneshot;       /* a completion interrupt is pending */
    int      log;           /* EMU_CDIC_LOG */
    long     delivered;
} cdic_t;

void cdic_init(cdic_t *c);

/* Disc image: 0 on success. Eject closes the image. */
int  cdic_insert(cdic_t *c, const char *path);
void cdic_eject(cdic_t *c);
int  cdic_has_disc(const cdic_t *c);
/* 0 = no disc, 1 = CD-i, 2 = audio CD (for the 68HC05 disc status) */
int  cdic_disc_kind(const cdic_t *c);

/* Where the decoded ADPCM audio goes (NULL = nowhere) */
void cdic_set_audio(cdic_t *c, cdic_audio_cb cb, void *ctx);

int      cdic_is_ram(uint32_t addr);       /* whole $300000-$303FFF window */
uint8_t  cdic_read8 (cdic_t *c, uint32_t addr);
void     cdic_write8(cdic_t *c, uint32_t addr, uint8_t v);
uint16_t cdic_read16(cdic_t *c, uint32_t addr);
void     cdic_write16(cdic_t *c, uint32_t addr, uint16_t v);

void cdic_tick(cdic_t *c, int cycles);

/* Interrupt request: returns the level (0 = none) and sets *vector */
int  cdic_irq(cdic_t *c, int *vector);

#endif /* CDIC_H */
