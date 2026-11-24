/*
 * mp2_audio.h - MPEG-1 audio layer II decoder (ISO/IEC 11172-3), written
 * for this emulator from the standard
 *
 * The elementary stream is pushed in as it arrives (mpa_write); mpa_decode
 * returns one frame (1152 stereo samples, mono duplicated) once it is all
 * in, or 0 when the next frame is not there yet.
 */

#ifndef MP2_AUDIO_H
#define MP2_AUDIO_H

#include <stddef.h>
#include <stdint.h>

typedef struct mpa mpa_t;

mpa_t  *mpa_create(void);
void    mpa_destroy(mpa_t *a);
void    mpa_write(mpa_t *a, const uint8_t *data, size_t n);
size_t  mpa_buffered(const mpa_t *a);          /* bytes not decoded yet */
/* next frame: number of stereo samples (interleaved L R, about -1..1), or 0 */
int     mpa_decode(mpa_t *a, const float **samples);
int     mpa_samplerate(const mpa_t *a);

#endif
