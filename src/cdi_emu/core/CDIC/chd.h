/*
 * chd.h - reader for MAME "CHD" CD images (version 5)
 *
 * Written for this emulator from the published formats (no MAME code):
 *   - CHD v5 header, metadata (CHT2 / CHTR track list) and hunk map,
 *     compressed map included (Huffman + run lengths);
 *   - the CD codecs: "cdlz" (LZMA), "cdzl" (deflate), "cdfl" (FLAC for
 *     audio), each with deflate-compressed subcode; uncompressed hunks;
 *   - CD-ROM sync / ECC regenerated where the compressor removed them.
 * Not handled: parent (differential) CHDs, zstd ("cdzs").
 *
 * A CD frame in a CHD is 2448 bytes: 2352 bytes of sector + 96 of
 * subcode. Audio samples are stored big-endian (chd_read_frame returns them
 * as stored; disc.c swaps them).
 */

#ifndef CHD_H
#define CHD_H

#include <stdint.h>

#define CHD_FRAME     2448
#define CHD_MAX_TRACKS 99

enum { CHD_AUDIO, CHD_MODE1_RAW, CHD_MODE2_RAW, CHD_MODE1, CHD_MODE2_FORM1,
       CHD_MODE2_FORM2, CHD_MODE2 };

typedef struct {
    int type;          /* CHD_* */
    int datasize;      /* bytes of sector data in a frame (2352, 2048, 2336...) */
    int frames;        /* frames of the track in the CHD (pregap included if stored) */
    int pregap;        /* pregap frames (INDEX 00 -> INDEX 01) */
    int pregap_stored; /* 1 = the pregap frames are in the CHD ("V" type) */
    int postgap;
    int first_frame;   /* index of the track's first frame in the CHD */
} chd_track_t;

typedef struct chd chd_t;

/* Open; NULL + reason (French, UTF-8) in err on failure */
chd_t *chd_open(const char *path, char *err, int n);
void   chd_close(chd_t *c);
int    chd_ntracks(const chd_t *c);
const chd_track_t *chd_track(const chd_t *c, int i);
/* One raw frame (2448 bytes). 0 = ok, -1 = read / decode error */
int    chd_read_frame(chd_t *c, int frame, uint8_t *out);

#endif /* CHD_H */
