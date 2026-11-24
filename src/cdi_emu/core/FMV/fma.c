/*
 * fma.c - audio side of the Digital Video Cartridge: the FMA host chip and
 * its DSP56001 (MPEG-1 layer II), registers at $E03000
 *
 * Written for this emulator from docs/fmv/FMV_COMPORTEMENT.md (section 4).
 * Frames are decoded by pl_mpeg and played through the host output,
 * resampled to 37.8 kHz.
 */

#include "fmv_internal.h"
#include "mpeg_system.h"
#include "../audio_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLM_NO_STDIO
#include "pl_mpeg.h"

#define TICK          (FMV_CPU_HZ / 100)   /* the chip's 10 ms period */
#define HOST_RATE     37800
#define HOST_PER_TICK (HOST_RATE / 100)
#define PCM_SIZE      (44100 * 2)          /* decoded stereo samples kept, 2 s */
#define PCM_AHEAD     (44100 / 4)          /* decode no further ahead than this */
#define FRAME_SAMPLES 1152
#define LOW_WATER     1024                 /* less input than this: maybe starving */
#define INPUT_MAX     (64 * 1024)
#define DROP_ABOVE    (16 * 1024)
#define ROOM_FOR_DMA  (12 * 1024)          /* EMU_FMA_HOLD only */

/* status and interrupt bits */
#define ST_END_OF_PROGRAM 0x0001
#define ST_STREAM_CHANGED 0x0002
#define ST_FRAMES         0x0004
#define ST_STARVED        0x0008
#define ST_DECODING       0x0010
#define ST_TIMER          0x0100

static struct {
    /* registers */
    uint16_t command, status, pending, mask, vector, stream, clock_low;
    uint32_t frame_header, header_bytes;
    /* DSP56001 host port: attenuation */
    int      dsp_register, dsp_command, dsp_count;
    uint8_t  dsp_bytes[4];
    /* playback */
    int      playing, stream_changed;
    int      start_known;
    int64_t  start_at;                     /* in DCLK */
    uint64_t clock_origin, next_tick;
    uint64_t last_input;                   /* time of the latest stream byte */
    uint64_t starve_wait;                  /* see audio_tick */
    int      program_over, skipping;
    mps_t    sys;
    plm_buffer_t *input;
    plm_audio_t  *mp2;
    /* decoded samples */
    int16_t  pcm[PCM_SIZE * 2];
    int      pcm_first, pcm_count, rate;
    double   position;                     /* resampling position */
    unsigned long bytes_in, decoded, played;
} A;

static uint32_t dclk(void) { return (uint32_t)((fmv_now - A.clock_origin) * 45000 / FMV_CPU_HZ); }
static size_t input_level(void) { return plm_buffer_get_remaining(A.input); }

static void new_decoder(void)
{
    if (A.mp2) plm_audio_destroy(A.mp2);   /* frees its buffer too */
    A.input = plm_buffer_create_with_capacity(INPUT_MAX);
    A.mp2 = plm_audio_create_with_buffer(A.input, 1);
    A.skipping = 0;
}

static void forget_samples(void) { A.pcm_first = A.pcm_count = 0; A.position = 0; }

static void flag_irq(uint16_t bits) { A.pending |= bits; }

void fmva_reset(void)
{
    const char *e = getenv("EMU_FMA_UNF_MS");
    A.starve_wait = e ? (uint64_t)atoi(e) * (FMV_CPU_HZ / 1000) : FMV_CPU_HZ * 4 / 10;
    A.command = A.status = A.pending = A.mask = A.vector = A.stream = A.clock_low = 0;
    A.frame_header = A.header_bytes = 0;
    A.playing = A.stream_changed = 0;
    A.dsp_register = A.dsp_command = A.dsp_count = 0;
    static const uint8_t straight[4] = { 0, 0x80, 0, 0x80 };   /* stereo, no attenuation */
    audio_out_set_atten(AUDIO_SRC_FMV, straight);
    A.clock_origin = fmv_now;
    mps_reset(&A.sys);
    A.start_known = 0; A.start_at = 0;
    new_decoder();
    forget_samples();
    if (!A.rate) A.rate = 44100;
    A.next_tick = fmv_now + TICK;
}

int fmva_irq_pending(void) { return (A.pending & A.mask) != 0; }
uint8_t fmva_vector(void) { return (uint8_t)A.vector; }
void fmva_poll(void) { flag_irq(ST_TIMER); }
int fmva_wants_data(void) { return input_level() < ROOM_FOR_DMA; }

void fmva_info(int *kbps, int *rate, int *playing)
{
    static const int kbit[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
    *kbps = A.frame_header ? kbit[(A.frame_header >> 12) & 15] : 0;
    *rate = A.rate;
    *playing = A.playing;
}

/* decode frames while there is room, up to a quarter second ahead */
static void decode_frames(void)
{
    while (A.pcm_count < PCM_AHEAD && A.pcm_count + FRAME_SAMPLES <= PCM_SIZE) {
        plm_samples_t *f = plm_audio_decode(A.mp2);
        if (!f) break;
        A.rate = plm_audio_get_samplerate(A.mp2);
        A.decoded += f->count;
        for (unsigned i = 0; i < f->count; i++) {
            int at = (A.pcm_first + A.pcm_count++) % PCM_SIZE;
            for (int c = 0; c < 2; c++) {
                float v = f->interleaved[2 * i + c];
                if (v > 1.f) v = 1.f;
                if (v < -1.f) v = -1.f;
                A.pcm[2 * at + c] = (int16_t)(v * 32767.f);
            }
        }
        if (!(A.status & ST_DECODING)) { A.status |= ST_DECODING; flag_irq(ST_DECODING); }
        A.status |= ST_FRAMES; flag_irq(ST_FRAMES);
        if (A.stream_changed) { flag_irq(ST_STREAM_CHANGED); A.stream_changed = 0; }
    }
}

void fmva_data(uint8_t b)
{
    int kept = A.sys.payload;               /* decided by the bytes before */
    mps_byte(&A.sys, b, (uint8_t)(0xC0 | A.stream));
    if (kept) {
        /* The host may send far ahead of playback (the Journal interactif
         * resends its 2 s menu loop every 0.5 s); holding its DMA would stall
         * the CPU with interrupts masked. A whole program is skipped instead,
         * so that no frame is cut. */
        if (A.program_over && !A.skipping && input_level() > DROP_ABOVE) {
            A.skipping = 1;
            if (fmv_log >= 2) fprintf(stderr, "[FMV] audio: input well filled, next program skipped\n");
        }
        if (!A.skipping && input_level() < INPUT_MAX) plm_buffer_write(A.input, &b, 1);
        if (fmv_log && A.bytes_in && fmv_now - A.last_input > FMV_CPU_HZ / 20)
            fprintf(stderr, "[FMV] audio data again after %.0f ms\n", (double)(fmv_now - A.last_input) * 1000.0 / FMV_CPU_HZ);
        A.bytes_in++;
        A.last_input = fmv_now;
        A.program_over = 0;
        {   static FILE *f = NULL; static int tried = 0;   /* EMU_FMA_DUMP=file (tests) */
            if (!tried) { tried = 1; if (getenv("EMU_FMA_DUMP")) f = fopen(getenv("EMU_FMA_DUMP"), "wb"); }
            if (f) fputc(b, f);
        }
        A.header_bytes = (A.header_bytes << 8) | b;
        if ((A.header_bytes >> 20) == 0xFFF) A.frame_header = A.header_bytes;
    }
    if ((A.sys.events & MPS_PTS) && !A.start_known) {
        A.start_known = 1;                  /* the first PTS gives the start */
        A.start_at = (int64_t)dclk() + (mps_ts_diff(A.sys.pts, A.sys.scr) >> 1);
    }
    if (A.sys.events & MPS_END) {
        A.status |= ST_END_OF_PROGRAM; flag_irq(ST_END_OF_PROGRAM);
        A.program_over = 1;
        A.skipping = 0;
        if (fmv_log) fprintf(stderr, "[FMV] audio program end at %.2f s\n", (double)fmv_now / FMV_CPU_HZ);
    }
}

void fmva_end_of_transfer(int dma)
{
    if (dma) A.command &= 0x7FFF;
    decode_frames();
}

static void audio_tick(void)
{
    if (A.start_known && (int64_t)dclk() >= A.start_at) {
        A.start_known = 0;
        if (!A.playing && fmv_log) fprintf(stderr, "[FMV] audio playback starts\n");
        A.playing = 1;
    }
    decode_frames();
    if (!A.playing) return;
    /* Starving: almost no input and nothing decoded. A single sample left
     * cannot be played (interpolation needs two); it counts as nothing once
     * no data has come for a while: The 7th Guest waits for this to loop its
     * ambient sound, the Journal's menu music goes on after a short gap. */
    if (input_level() < LOW_WATER &&
        (A.pcm_count == 0 || (A.pcm_count < 2 && fmv_now - A.last_input > A.starve_wait))) {
        A.status = (uint16_t)((A.status | ST_STARVED) & ~ST_DECODING);
        flag_irq(ST_STARVED);
        A.playing = 0; A.start_known = 0;
        forget_samples();
        if (fmv_log) fprintf(stderr, "[FMV] audio starved at %.2f s (%lu bytes in, %lu decoded, %lu played)\n",
                             (double)fmv_now / FMV_CPU_HZ, A.bytes_in, A.decoded, A.played);
        return;
    }
    /* 10 ms for the host, linear interpolation between decoded samples */
    static int16_t out[HOST_PER_TICK * 2];
    double step = (double)A.rate / HOST_RATE;
    int n = 0;
    for (; n < HOST_PER_TICK; n++) {
        int k = (int)A.position;
        if (k + 1 >= A.pcm_count) break;
        double frac = A.position - k;
        int p0 = (A.pcm_first + k) % PCM_SIZE, p1 = (A.pcm_first + k + 1) % PCM_SIZE;
        for (int c = 0; c < 2; c++)
            out[2 * n + c] = (int16_t)(A.pcm[2 * p0 + c] + (A.pcm[2 * p1 + c] - A.pcm[2 * p0 + c]) * frac);
        A.position += step;
    }
    int used = (int)A.position;
    A.pcm_first = (A.pcm_first + used) % PCM_SIZE;
    A.pcm_count -= used;
    A.position -= used;
    A.played += (unsigned long)used;
    if (n) audio_out_push((void *)(intptr_t)AUDIO_SRC_FMV, out, n);
}

void fmva_tick(void)
{
    while (fmv_now >= A.next_tick) {
        A.next_tick += TICK;
        audio_tick();
    }
}

uint16_t fmva_read(int reg)
{
    uint16_t v = 0;
    switch (reg) {
    case 0x00: v = A.command; break;
    case 0x01: v = (uint16_t)(0x0200 | A.status); break;
    case 0x02: v = 0x0007; break;
    case 0x03: v = 0x0900; break;
    case 0x04: case 0x05: v = A.stream; break;
    case 0x06: v = A.vector; break;
    case 0x07: v = 0x0042; break;
    case 0x08: { uint32_t c = dclk(); v = (uint16_t)(c >> 16); A.clock_low = (uint16_t)c; break; }
    case 0x09: v = A.clock_low; break;
    case 0x0A: v = (uint16_t)(A.frame_header >> 16); break;
    case 0x0B: v = (uint16_t)A.frame_header; break;
    case 0x0C: v = A.playing ? 1 : 0; break;
    case 0x0D: v = A.pending; A.pending = 0; break;
    case 0x0E: v = A.mask; break;
    case 0x12: v = 0x0004; break;           /* DSP56001 flag HF2 */
    }
    if (fmv_log >= 2 && reg != 0x0D && reg != 0x08 && reg != 0x09)
        fprintf(stderr, "[FMV] A R %02X -> %04X\n", reg * 2, v);
    return v;
}

/* DSP56001 host interface: a host command ($80 | n on register 1), then its
 * bytes on register 7. Command $13 sets the four attenuators. */
static void dsp_port(uint16_t v)
{
    if (A.dsp_register == 1 && (v & 0x80)) { A.dsp_command = v & 0x7F; A.dsp_count = 0; return; }
    if (A.dsp_register != 7 || A.dsp_command != 0x13) return;
    A.dsp_bytes[A.dsp_count++] = (uint8_t)v;
    if (A.dsp_count < 4) return;
    /* sent as L->L, L->R, R->L, R->R; the mixer takes L->L, L->R, R->R, R->L */
    uint8_t att[4] = { A.dsp_bytes[0], A.dsp_bytes[1], A.dsp_bytes[3], A.dsp_bytes[2] };
    audio_out_set_atten(AUDIO_SRC_FMV, att);
    if (fmv_log) fprintf(stderr, "[FMV] audio attenuation %02X %02X %02X %02X\n",
                         A.dsp_bytes[0], A.dsp_bytes[1], A.dsp_bytes[2], A.dsp_bytes[3]);
    A.dsp_command = A.dsp_count = 0;
}

void fmva_write(int reg, uint16_t v)
{
    if (fmv_log >= 2 || (fmv_log && reg != 0x11 && reg != 0x12))
        fprintf(stderr, "[FMV] A W %02X = %04X\n", reg * 2, v);
    switch (reg) {
    case 0x00:
        A.command = v;
        if (v & 0x8000) { A.status &= (uint16_t)~ST_STARVED; fmv_board_drq(1); }
        if (v & 0x0001) {                   /* stop: everything flushed */
            A.playing = 0; A.status = 0;
            mps_reset(&A.sys);
            A.start_known = 0;
            new_decoder();
            forget_samples();
        }
        break;
    case 0x04: A.stream = v & 0x0F; A.stream_changed = 1; break;
    case 0x06: A.vector = v; break;
    case 0x0E: A.mask = v; break;
    case 0x11: A.dsp_register = v & 7; break;
    case 0x12: dsp_port(v); break;
    }
}
