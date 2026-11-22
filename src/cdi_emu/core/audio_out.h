/*
 * audio_out.h - host sound output (SDL2), stereo 16-bit at 37.8 kHz
 *
 * The CDIC pushes decoded ADPCM frames as they arrive from the disc; SDL
 * converts to the host rate. If the emulation runs faster than real time
 * the queue is capped (oldest audio kept, newest dropped) so the sound
 * never lags far behind the picture. EMU_AUDIO=0 disables the output.
 */

#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>

/* Sources, mixed together; ctx of audio_out_push = (void *)source */
enum { AUDIO_SRC_CDIC = 0, AUDIO_SRC_FMV = 1, AUDIO_SOURCES = 2 };

int  audio_out_init(void);                       /* 0 = ok */
void audio_out_push(void *ctx, const int16_t *stereo, int frames);
void audio_out_close(void);

/* Attenuation of a source (CD-i attenuators): left->left, left->right,
 * right->right, right->left, 1 dB steps, bit 7 = path off */
void audio_out_set_atten(int source, const uint8_t att[4]);
void audio_out_get_atten(int source, uint8_t att[4]);

/* For the F3 overlay: queued sound (ms, -1 without output device), counts
 * of underruns and of frames dropped for being ahead of real time */
void audio_out_stats(int source, int *queued_ms, unsigned long *underruns, unsigned long *dropped);

#endif /* AUDIO_OUT_H */
