/*
 * adpcm.c - CD-i ADPCM audio sector decoder (see adpcm.h)
 */

#include "adpcm.h"
#include <string.h>

static const int K0[4] = { 0, 60, 115, 98 };
static const int K1[4] = { 0, 0, -52, -55 };

void adpcm_reset(adpcm_state_t *st) { memset(st, 0, sizeof(*st)); }

static int16_t predict(adpcm_state_t *st, int ch, int filter, int32_t sample)
{
    int32_t s = sample + ((st->s1[ch] * K0[filter & 3] + st->s2[ch] * K1[filter & 3]) >> 6);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    st->s2[ch] = st->s1[ch];
    st->s1[ch] = s;
    return (int16_t)s;
}

int adpcm_decode_sector(adpcm_state_t *st, const uint8_t *raw, int16_t *out)
{
    uint8_t coding = raw[19];
    int stereo = coding & 1;
    int half   = ((coding >> 2) & 3) == 1;        /* 18.9 kHz */
    int bits8  = ((coding >> 4) & 3) == 1;        /* level A */
    const uint8_t *g = raw + 24;

    /* decode to a mono/stereo sample list first */
    static int16_t l[4032], r[4032];
    int nl = 0, nr = 0;

    for (int grp = 0; grp < 18; grp++, g += 128) {
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            /* parameters of sound unit u at byte 4 + u (bytes 4..7 repeat
             * 0..3, so this also holds for the 4 units of level A) */
            uint8_t p = g[4 + u];
            int range = p & 0x0F, filter = (p >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;
            for (int i = 0; i < 28; i++) {
                const uint8_t *w = g + 16 + i * 4;
                int32_t smp;
                if (bits8) {
                    smp = (int32_t)(int8_t)w[u] * 256;
                } else {
                    int nib = (w[u >> 1] >> ((u & 1) * 4)) & 0x0F;
                    smp = (int32_t)(int16_t)(uint16_t)(nib << 12);
                }
                smp >>= (range > 12 ? 12 : range);
                int16_t v = predict(st, ch, filter, smp);
                if (ch == 0) { if (nl < 4032) l[nl++] = v; }
                else         { if (nr < 4032) r[nr++] = v; }
            }
        }
    }

    /* to stereo 37.8 kHz */
    int n = stereo ? (nl < nr ? nl : nr) : nl;
    int rep = half ? 2 : 1;
    int frames = 0;
    for (int i = 0; i < n; i++) {
        int16_t a = l[i], b = stereo ? r[i] : l[i];
        for (int k = 0; k < rep && frames < ADPCM_MAX_FRAMES; k++) {
            out[2 * frames] = a;
            out[2 * frames + 1] = b;
            frames++;
        }
    }
    return frames;
}
