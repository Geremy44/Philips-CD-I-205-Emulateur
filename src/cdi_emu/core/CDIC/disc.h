/*
 * disc.h - CD image reader for the CDIC
 *
 * Accepted images:
 *   .cue + .bin  raw 2352-byte sectors (MODE1/2352, MODE2/2352, AUDIO),
 *                tracks in one BIN file or one BIN per track (audio CDs);
 *                a "data" track without the CD-ROM sync pattern is in fact
 *                CD-DA (badly ripped audio CDs) and is played as audio
 *   .bin / .img  raw 2352-byte sectors, a single CD-i (mode 2) track
 *   .iso         cooked 2048-byte mode 1 sectors (header synthesized)
 *   cdrom:X      the disc in the PC's drive X: (raw sectors, cdrom_win.c)
 *
 * Addresses are LBAs: LBA 0 = absolute time 00:02:00.
 */

#ifndef DISC_H
#define DISC_H

#include <stdint.h>
#include <stdio.h>

#define DISC_MAX_TRACKS 99
#define DISC_RAW        2352

typedef struct {
    int     number;        /* 1..99 */
    int     audio;         /* 1 = CD-DA track */
    int     start;         /* LBA of INDEX 01 */
    int     file;          /* index in disc_t.files */
    int64_t file_offset;   /* byte offset of that sector in its file */
} disc_track_t;

typedef struct {
    FILE        *f;              /* first image file (NULL = no disc) */
    FILE        *files[DISC_MAX_TRACKS];
    int          nfiles;
    int          sector_size;    /* 2352 (raw) or 2048 (iso) */
    int          ntracks;
    disc_track_t track[DISC_MAX_TRACKS];
    int          leadout;        /* LBA of the lead-out */
    char         path[1100];
    struct cdrom *drive;         /* physical drive ("cdrom:F"), else NULL */
    uint8_t     *cache;          /* drive read-ahead */
    int          cache_lba, cache_n;
} disc_t;

/* Open an image (.cue, .bin, .img, .iso). Returns 0 on success. */
int  disc_open(disc_t *d, const char *path);
void disc_close(disc_t *d);
int  disc_present(const disc_t *d);

/* Read one raw 2352-byte sector. Outside the image: an empty mode 2
 * sector with a valid header. Returns the track index (0-based) or -1. */
int  disc_read(disc_t *d, int lba, uint8_t out[DISC_RAW]);

/* Track index containing lba (-1 before track 1) */
int  disc_track_of(const disc_t *d, int lba);

/* BCD helpers and LBA <-> absolute MSF (LBA 0 = 00:02:00) */
uint8_t disc_bcd(int v);
int     disc_unbcd(uint8_t v);
void    disc_lba_to_msf(int lba, uint8_t msf[3]);   /* BCD */
int     disc_msf_to_lba(uint8_t m, uint8_t s, uint8_t f);

#endif /* DISC_H */
