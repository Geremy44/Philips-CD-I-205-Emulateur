/*
 * adpcm.h - CD-i ADPCM audio sector decoder (Green Book levels A, B, C)
 *
 * A form 2 audio sector carries 18 sound groups of 128 bytes (sector
 * bytes 24..2327). The coding byte (4th subheader byte) gives:
 *   bit 0     stereo
 *   bits 3-2  sampling: 0 = 37.8 kHz, 1 = 18.9 kHz
 *   bits 5-4  bits per sample: 0 = 4 bits (levels B/C), 1 = 8 bits (level A)
 *   bit 6     emphasis (ignored)
 * Each sound group: 16 bytes of sound parameters (filter in the high nibble,
 * range in the low one, one per sound unit), then 28 words of 4 bytes.
 * Prediction: s = data * 2^(12 - range) + (s1 * K0 + s2 * K1) / 64.
 */

#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

typedef struct {
    int32_t s1[2], s2[2];   /* previous samples, per channel */
} adpcm_state_t;

void adpcm_reset(adpcm_state_t *st);

/* Decode one audio sector (raw 2352 bytes). Output is always stereo at
 * 37800 Hz (18.9 kHz is doubled, mono is copied to both channels).
 * Returns the number of stereo frames written to out (max 8064). */
int  adpcm_decode_sector(adpcm_state_t *st, const uint8_t *raw, int16_t *out);

#define ADPCM_MAX_FRAMES 8064
#define ADPCM_RATE       37800

#endif /* ADPCM_H */
