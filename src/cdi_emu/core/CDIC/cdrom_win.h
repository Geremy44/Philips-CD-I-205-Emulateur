/*
 * cdrom_win.h - physical CD drive access (Windows)
 *
 * Raw 2352-byte sectors (CD-i mode 2 and CD-DA alike) with the SCSI
 * READ CD command through SCSI pass-through, IOCTL_CDROM_RAW_READ as a
 * fallback; table of contents with IOCTL_CDROM_READ_TOC. Also rips a
 * whole disc to .bin + .cue.
 */

#ifndef CDROM_WIN_H
#define CDROM_WIN_H

#include <stdint.h>

typedef struct cdrom cdrom_t;

#define CDROM_MAX_TRACKS 99

typedef struct {
    int ntracks;
    int number[CDROM_MAX_TRACKS];
    int start[CDROM_MAX_TRACKS];     /* LBA */
    int audio[CDROM_MAX_TRACKS];
    int leadout;                     /* LBA */
} cdrom_toc_t;

/* CD / DVD / BD drive letters ("DF" ...); returns how many */
int      cdrom_list_drives(char *letters, int max);

cdrom_t *cdrom_open(char letter);
void     cdrom_close(cdrom_t *c);
int      cdrom_read_toc(cdrom_t *c, cdrom_toc_t *toc);  /* 0 = ok (disc in) */
/* count raw sectors from lba into out (count * 2352 bytes); 0 = ok */
int      cdrom_read_raw(cdrom_t *c, int lba, int count, int audio, uint8_t *out);
/* Disc name: CD-i volume id, else "CD_audio" / "Disque" */
void     cdrom_disc_name(cdrom_t *c, const cdrom_toc_t *toc, char *out, int n);

/* ---- ripping (runs in its own thread) ---- */

/* Start ripping the disc in drive `letter` into games_dir/<name>/<name>.bin
 * + .cue. Returns 0 if started. */
int  cdrom_rip_start(char letter, const char *games_dir);
/* -1 = idle, 0..100 = running or finished; *done = 1 once finished (then
 * msg holds the result: the .cue path or the error). */
int  cdrom_rip_progress(int *done, char *msg, int n);

#endif
