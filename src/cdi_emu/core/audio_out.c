/*
 * audio_out.c - host sound output (see audio_out.h)
 */

#include "audio_out.h"
#include "CDIC/adpcm.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUEUED_SECONDS 1

static SDL_AudioDeviceID dev = 0;

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

int audio_out_init(void)
{
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
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);   /* SDL converts the rate */
    if (!dev) {
        fprintf(stderr, "[AUDIO] cannot open an audio device: %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "[AUDIO] output open (%d Hz stereo, converted by SDL)\n", ADPCM_RATE);
    return 0;
}

void audio_out_push(void *ctx, const int16_t *stereo, int frames)
{
    (void)ctx;
    if (wav && frames > 0) {
        for (int i = 0; i < 2 * frames; i++) put16(wav, (uint16_t)stereo[i]);   /* little-endian */
        wav_bytes += (uint32_t)frames * 4u;
        wav_header();
        fflush(wav);
    }
    if (!dev || frames <= 0) return;
    Uint32 queued = SDL_GetQueuedAudioSize(dev);
    if (queued > (Uint32)(ADPCM_RATE * 4 * MAX_QUEUED_SECONDS)) return;   /* ahead of real time */
    SDL_QueueAudio(dev, stereo, (Uint32)frames * 4u);
}

void audio_out_close(void)
{
    if (wav) { wav_header(); fclose(wav); wav = NULL; }
    if (dev) SDL_CloseAudioDevice(dev);
    dev = 0;
}
