/*
 * cdburn.h - write a disc image (.cue / .bin) to a blank CD-R / CD-RW
 *
 * Disc-at-once (MMC "session at once") through SCSI pass-through, every
 * sector sent raw (2352 bytes: sync, header, subheader, data, EDC/ECC as
 * they are in the image), so CD-i mode 2 form 1 / form 2 sectors and CD-DA
 * tracks are copied exactly. The lead-in says "CD-i disc" when the image is
 * one. Windows only; the burn runs in its own thread.
 */

#ifndef CDBURN_H
#define CDBURN_H

/* Check an image and describe it for the confirmation box (UTF-8).
 * Returns 0 if it can be burned, else -1 with the reason in out. */
int cdburn_describe(const char *image, char *out, int n);

/* Burn image to the blank disc in drive `letter`. simulate = 1: test
 * write, laser off (the drive goes through every step, the disc stays
 * blank). Returns 0 if started, -1 if a burn is already running. */
int cdburn_start(char letter, const char *image, int simulate);

/* -1 = idle; else 0..100 for the current stage, named in stage (UTF-8).
 * *done = 1 once finished; msg then holds the result. */
int cdburn_progress(int *done, char *stage, int sn, char *msg, int n);

#endif
