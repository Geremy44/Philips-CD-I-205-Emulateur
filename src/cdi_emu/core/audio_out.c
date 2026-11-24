/*
 * audio_out.c - host sound output (see audio_out.h)
 *
 * Each source (CDIC audio, Digital Video Cartridge MPEG audio) has its own
 * queue; the SDL callback mixes them, as the player's audio mixer does.
 * Putting both in one queue would chop each of them when they play at the
 * same time (Journal interactif menus).
 */

#include "audio_out.h"
#include "CDIC/adpcm.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_QUEUED_SECONDS 1
#define RING_FRAMES (ADPCM_RATE * 2)          /* 2 s per source */

static SDL_AudioDeviceID dev = 0;
static int audio_log = 0;

typedef struct {
    int16_t  buf[RING_FRAMES * 2];
    int      head, count;                      /* in stereo frames */
    unsigned long pushed, dropped, starved;    /* statistics (EMU_AUDIO_LOG) */
    int      primed, waited;           /* playing / frames waited while filling */
    int      gain[4];                  /* LL, LR, RR, RL in 1/4096 (4096 = 0 dB) */
    uint8_t  atten[4];                 /* as set: dB, bit 7 = off */
} source_t;

static source_t src[AUDIO_SOURCES] = {
    { .gain = { 4096, 0, 4096, 0 }, .atten = { 0, 0x80, 0, 0x80 } },
    { .gain = { 4096, 0, 4096, 0 }, .atten = { 0, 0x80, 0, 0x80 } },
};

/* CD-i attenuators (Green Book): 4 values, left->left, left->right,
 * right->right, right->left, in 1 dB steps; bit 7 switches the path off */
void audio_out_set_atten(int s, const uint8_t att[4])
{
    if (s < 0 || s >= AUDIO_SOURCES) return;
    int g[4];
    for (int k = 0; k < 4; k++) {
        g[k] = (att[k] & 0x80) ? 0 : (int)(4096.0 * pow(10.0, -(double)(att[k] & 0x7F) / 20.0) + 0.5);
    }
    /* both paths into one output (e.g. $C0: mono mix): keep the sum at
     * 0 dB at most, so a mono source is not doubled and does not clip */
    for (int side = 0; side < 2; side++) {
        int *direct = &g[side ? 2 : 0], *cross = &g[side ? 1 : 3];
        int sum = *direct + *cross;
        if (sum > 4096) { *direct = *direct * 4096 / sum; *cross = *cross * 4096 / sum; }
    }
    if (dev) SDL_LockAudioDevice(dev);
    memcpy(src[s].gain, g, sizeof(g));
    memcpy(src[s].atten, att, 4);
    if (dev) SDL_UnlockAudioDevice(dev);
    if (audio_log)
        fprintf(stderr, "[AUDIO] source %d attenuation LL %02X LR %02X RR %02X RL %02X\n", s, att[0], att[1], att[2], att[3]);
}

void audio_out_get_atten(int s, uint8_t att[4])
{
    if (s < 0 || s >= AUDIO_SOURCES) s = 0;
    memcpy(att, src[s].atten, 4);
}

/* EMU_AUDIO_DUMP=file.wav: also write everything to a WAV file (header
 * sizes are refreshed after each block, so a killed run stays readable) */
static FILE *wav = NULL;
static uint32_t wav_bytes = 0;

static void put32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((int)(v >> (8 * i)) & 0xFF, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }

static void wav_header(void)
{
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); put32(wav, 36 + wav_bytes);
    fwrite("WAVEfmt ", 1, 8, wav); put32(wav, 16); put16(wav, 1); put16(wav, 2);
    put32(wav, ADPCM_RATE); put32(wav, ADPCM_RATE * 4); put16(wav, 4); put16(wav, 16);
    fwrite("data", 1, 4, wav); put32(wav, wav_bytes);
    fseek(wav, 0, SEEK_END);
}

/* SDL audio thread: mix the sources that have data */
static void fill(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    /* Pre-buffer: a source (re)starts once ~150 ms are queued, or 150 ms
     * after its first sample (short sound effects). The disc delivers the
     * sound in 53 ms sectors and the device takes 54 ms blocks: started
     * from an empty queue, the music ran dry dozens of times in its first
     * seconds (Inca's pan flute stuttered). */
    const int prime = ADPCM_RATE * 15 / 100;
    for (int s = 0; s < AUDIO_SOURCES; s++) {
        source_t *q = &src[s];
        if (!q->count) { q->primed = 0; q->waited = 0; continue; }
        if (!q->primed) {
            if (q->count >= prime || q->waited >= prime) q->primed = 1;
            else q->waited += frames;
        }
    }
    for (int i = 0; i < frames; i++) {
        int l = 0, r = 0;
        for (int s = 0; s < AUDIO_SOURCES; s++) {
            source_t *q = &src[s];
            if (!q->count || !q->primed) continue;
            int il = q->buf[2 * q->head], ir = q->buf[2 * q->head + 1];
            /* attenuation matrix, factors in 1/4096 */
            l += (il * q->gain[0] + ir * q->gain[3]) >> 12;
            r += (ir * q->gain[2] + il * q->gain[1]) >> 12;
            q->head = (q->head + 1) % RING_FRAMES;
            q->count--;
            if (!q->count) { q->starved++; q->primed = 0; q->waited = 0; }
        }
        out[2 * i]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

int audio_out_init(void)
{
    audio_log = getenv("EMU_AUDIO_LOG") != NULL;
    const char *dump = getenv("EMU_AUDIO_DUMP");
    if (dump && *dump && (wav = fopen(dump, "wb")) != NULL) {
        wav_header();
        fprintf(stderr, "[AUDIO] also writing '%s'\n", dump);
    }
    const char *e = getenv("EMU_AUDIO");
    if (e && *e == '0') { fprintf(stderr, "[AUDIO] disabled (EMU_AUDIO=0)\n"); return -1; }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[AUDIO] SDL audio init failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = ADPCM_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    want.callback = fill;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);   /* SDL converts the rate */
    if (!dev) {
        fprintf(stderr, "[AUDIO] cannot open an audio device: %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "[AUDIO] output open (%d Hz stereo, converted by SDL, %d mixed sources)\n",
            ADPCM_RATE, AUDIO_SOURCES);
    return 0;
}

void audio_out_push(void *ctx, const int16_t *stereo, int frames)
{
    int s = (int)(intptr_t)ctx;
    if (s < 0 || s >= AUDIO_SOURCES) s = 0;
    if (wav && frames > 0) {
        for (int i = 0; i < 2 * frames; i++) put16(wav, (uint16_t)stereo[i]);   /* little-endian */
        wav_bytes += (uint32_t)frames * 4u;
        wav_header();
        fflush(wav);
    }
    if (frames <= 0) return;
    source_t *q = &src[s];
    q->pushed += (unsigned long)frames;
    if (audio_log) {
        static unsigned long next[AUDIO_SOURCES];
        if (q->pushed >= next[s]) {
            next[s] = q->pushed + ADPCM_RATE * 5;
            fprintf(stderr, "[AUDIO] source %d: %lu frames pushed, %lu dropped, %lu underruns, %d queued\n",
                    s, q->pushed, q->dropped, q->starved, q->count);
        }
    }
    if (!dev) return;
    SDL_LockAudioDevice(dev);
    for (int i = 0; i < frames; i++) {
        if (q->count >= ADPCM_RATE * MAX_QUEUED_SECONDS) { q->dropped += (unsigned long)(frames - i); break; }   /* ahead of real time */
        int p = (q->head + q->count) % RING_FRAMES;
        q->buf[2 * p] = stereo[2 * i];
        q->buf[2 * p + 1] = stereo[2 * i + 1];
        q->count++;
    }
    SDL_UnlockAudioDevice(dev);
}

void audio_out_pause(int on)
{
    if (dev) SDL_PauseAudioDevice(dev, on ? 1 : 0);
}

void audio_out_close(void)
{
    if (wav) { wav_header(); fclose(wav); wav = NULL; }
    if (dev) SDL_CloseAudioDevice(dev);
    dev = 0;
}

void audio_out_stats(int s, int *queued_ms, unsigned long *underruns, unsigned long *dropped)
{
    if (s < 0 || s >= AUDIO_SOURCES) s = 0;
    *queued_ms = dev ? src[s].count * 1000 / ADPCM_RATE : -1;
    *underruns = src[s].starved;
    *dropped = src[s].dropped;
}
