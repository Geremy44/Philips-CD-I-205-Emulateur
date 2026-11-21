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

int  audio_out_init(void);                       /* 0 = ok */
void audio_out_push(void *ctx, const int16_t *stereo, int frames);
void audio_out_close(void);

#endif /* AUDIO_OUT_H */
