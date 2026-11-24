/*
 * mpeg1_video.h - MPEG-1 video decoder (ISO/IEC 11172-2), written for this
 * emulator from the standard
 *
 * The elementary stream is pushed in as it arrives (mpv_write). A picture
 * is decoded only once it is whole: the start code of the next picture is
 * in, or the caller says no more data will come (flush). Pictures come out
 * in coded order; the caller reorders them with mpv_frame():
 *   I or P decoded -> MPV_REF_OLD is the previous reference, now displayable
 *   B decoded      -> MPV_LAST_B is displayable at once
 *   end of stream  -> MPV_REF_NEW, the last reference
 * Planes are macroblock-aligned: y.width is the row pitch, chroma is 4:2:0.
 */

#ifndef MPEG1_VIDEO_H
#define MPEG1_VIDEO_H

#include <stddef.h>
#include <stdint.h>

typedef struct { int width, height; uint8_t *data; } mpv_plane_t;
typedef struct { int width, height; mpv_plane_t y, cb, cr; } mpv_frame_t;
typedef struct mpv mpv_t;

enum { MPV_REF_OLD = 0, MPV_REF_NEW = 1, MPV_LAST_B = 2 };

/* mpv_decode() results besides the picture type (1 I, 2 P, 3 B) */
enum { MPV_WAIT = 0, MPV_SKIPPED = -1, MPV_NO_PICTURE = -2 };

mpv_t  *mpv_create(void);
void    mpv_destroy(mpv_t *d);
void    mpv_write(mpv_t *d, const uint8_t *data, size_t n);
size_t  mpv_buffered(const mpv_t *d);          /* bytes not decoded yet */
int     mpv_has_header(mpv_t *d);              /* a sequence header was read */
int     mpv_width(const mpv_t *d);
int     mpv_height(const mpv_t *d);
/* decode the next picture: its type, MPV_WAIT (not whole yet),
 * MPV_SKIPPED (D picture or broken data) or MPV_NO_PICTURE (no picture
 * start code in the buffer) */
int     mpv_decode(mpv_t *d, int flush);
const mpv_frame_t *mpv_frame(const mpv_t *d, int which);

#endif
