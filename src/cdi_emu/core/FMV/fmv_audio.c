/*
 * fmv_audio.c - MPEG-1 audio chip of the Digital Video Cartridge (FMA,
 * host side of the DSP56001, registers at $E03000)
 *
 * Register map, status bits and start/underflow rules follow MAME's
 * gsc38gg307 device (pull request #16181, BSD-3-Clause, Alexandre Derumier,
 * from the CDi_MiSTer core). C port; layer II frames decoded by pl_mpeg
 * (MIT) and sent to the host output resampled to 37.8 kHz.
 *
 * Portions derived from MAME: Copyright (c) Alexandre Derumier,
 * BSD-3-Clause; full notice in THIRD_PARTY_LICENSES.md at the repository root.
 */

#include "fmv_internal.h"
#include "mpeg_demux.h"
#include "../audio_out.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PLM_NO_STDIO
#include "pl_mpeg.h"

#define DCLK_HZ    45000
#define OUT_RATE   37800
#define RING       (44100 * 2)             /* stereo frames queued, 2 s */
#define MP2_FRAME_BYTES 1024

enum { FMA_EOI = 1 << 0, FMA_CSU = 1 << 1, FMA_UPD = 1 << 2, FMA_UNF = 1 << 3,
       FMA_DEC = 1 << 4, FMA_POLL = 1 << 8 };

static uint16_t cmd, status, isr, ier, ivec, stream_nr, dclkl_latch;
static uint32_t audio_header, hdr_shift;
static int      dsp_enable, pending_stream_change;
static uint64_t dclk_origin, tick_at = FMV_NEVER;
static int      start_valid;
static int64_t  start_time;
static mpeg_demux_t demux;
static plm_buffer_t *es;
static plm_audio_t  *dec;
static int16_t  ring[RING * 2];
static int      r_head, r_count, src_rate = 44100;
static double   phase;
static unsigned long st_in, st_dec, st_out;   /* statistics (log) */                      /* resampler position, source frames */

static uint32_t fma_dclk(void) { return (uint32_t)((fmv_now - dclk_origin) * DCLK_HZ / FMV_CPU_HZ); }

static void decoder_new(void)
{
    if (dec) plm_audio_destroy(dec);
    es = plm_buffer_create_with_capacity(64 * 1024);
    dec = plm_audio_create_with_buffer(es, 1);
}

static void audio_clear(void) { r_head = r_count = 0; phase = 0; }

void fmva_reset(void)
{
    cmd = status = isr = ier = ivec = stream_nr = dclkl_latch = 0;
    audio_header = hdr_shift = 0;
    dsp_enable = pending_stream_change = 0;
    dclk_origin = fmv_now;
    mpeg_demux_reset(&demux);
    start_valid = 0; start_time = 0;
    decoder_new();
    audio_clear();
    tick_at = fmv_now + FMV_CPU_HZ / 100;
}

int fmva_irq_pending(void) { return (isr & ier) != 0; }
uint8_t fmva_vector(void) { return (uint8_t)ivec; }
void fmva_poll(void) { isr |= FMA_POLL; }

static void decode_pending(void)
{
    while (r_count < 44100 / 4 && r_count + 1152 <= RING) {
        plm_samples_t *s = plm_audio_decode(dec);
        if (!s) break;
        src_rate = plm_audio_get_samplerate(dec);
        st_dec += s->count;
        for (unsigned i = 0; i < s->count; i++) {
            int p = (r_head + r_count) % RING;
            float l = s->interleaved[2 * i], r = s->interleaved[2 * i + 1];
            l = l > 1.f ? 1.f : l < -1.f ? -1.f : l;
            r = r > 1.f ? 1.f : r < -1.f ? -1.f : r;
            ring[2 * p] = (int16_t)(l * 32767.f);
            ring[2 * p + 1] = (int16_t)(r * 32767.f);
            r_count++;
        }
        if (!(status & FMA_DEC)) { status |= FMA_DEC; isr |= FMA_DEC; }   /* decoding started */
        status |= FMA_UPD; isr |= FMA_UPD;
        if (pending_stream_change) { isr |= FMA_CSU; pending_stream_change = 0; }
    }
}

void fmva_data(uint8_t b)
{
    int body = demux.packet_body;
    mpeg_demux_byte(&demux, b, (uint8_t)(0xC0 | stream_nr));
    if (body) {
        if (plm_buffer_get_remaining(es) < 64 * 1024) plm_buffer_write(es, &b, 1);   /* full FIFO drops */
        st_in++;
        {   /* EMU_FMA_DUMP=file: the audio elementary stream received (tests) */
            static FILE *f = NULL; static int tried = 0;
            if (!tried) { tried = 1; if (getenv("EMU_FMA_DUMP")) f = fopen(getenv("EMU_FMA_DUMP"), "wb"); }
            if (f) fputc(b, f);
        }
        hdr_shift = (hdr_shift << 8) | b;
        if ((hdr_shift & 0xfff00000u) == 0xfff00000u) audio_header = hdr_shift;
    }
    if (demux.pts_updated && !start_valid) {   /* the first PTS sets the start */
        start_valid = 1;
        start_time = (int64_t)fma_dclk() + (mpeg_ts_diff(demux.pts, demux.scr) >> 1);
    }
    if (demux.program_end) { isr |= FMA_EOI; status |= FMA_EOI; }
}

void fmva_end_of_transfer(int dma)
{
    if (dma) cmd &= (uint16_t)~0x8000;
    decode_pending();
}

/* 10 ms: start / underflow, and send 10 ms of samples to the host */
static void audio_tick(void)
{
    if (start_valid && (int64_t)fma_dclk() >= start_time) {
        start_valid = 0;
        if (!dsp_enable && fmv_log) fprintf(stderr, "[FMV] audio playback starts\n");
        dsp_enable = 1;
    }
    decode_pending();
    if (!dsp_enable) return;
    if (r_count == 0 && plm_buffer_get_remaining(es) < MP2_FRAME_BYTES) {
        status |= FMA_UNF; isr |= FMA_UNF;
        status &= (uint16_t)~FMA_DEC;
        dsp_enable = 0; start_valid = 0;
        if (fmv_log) fprintf(stderr, "[FMV] audio underflow at %.2f s: %lu bytes in, %lu samples decoded, %lu sent, %u left in ES\n",
                             (double)fmv_now / FMV_CPU_HZ, st_in, st_dec, st_out, (unsigned)plm_buffer_get_remaining(es));
        return;
    }
    /* 378 output frames, linear interpolation from the source rate */
    static int16_t out[OUT_RATE / 100 * 2];
    double step = (double)src_rate / OUT_RATE;
    int n = 0;
    while (n < OUT_RATE / 100) {
        int i0 = (int)phase;
        if (i0 + 1 >= r_count) break;
        double f = phase - i0;
        int a = (r_head + i0) % RING, b2 = (r_head + i0 + 1) % RING;
        out[2 * n]     = (int16_t)(ring[2 * a] + (ring[2 * b2] - ring[2 * a]) * f);
        out[2 * n + 1] = (int16_t)(ring[2 * a + 1] + (ring[2 * b2 + 1] - ring[2 * a + 1]) * f);
        n++;
        phase += step;
    }
    int used = (int)phase;
    r_head = (r_head + used) % RING; r_count -= used; phase -= used;
    st_out += (unsigned long)used;
    if (n) audio_out_push(NULL, out, n);
}

void fmva_tick(void)
{
    static int n = 0;
    while (fmv_now >= tick_at) {
        tick_at += FMV_CPU_HZ / 100; audio_tick();
        if (fmv_log && st_in && ++n % 500 == 0)
            fprintf(stderr, "[FMV] audio at %.1f s: %lu bytes in, %lu decoded, %lu sent, %d queued, %u in ES, run %d\n",
                    (double)fmv_now / FMV_CPU_HZ, st_in, st_dec, st_out, r_count, (unsigned)plm_buffer_get_remaining(es), dsp_enable);
    }
}

uint16_t fmva_read(int reg)
{
    uint16_t d = 0;
    switch (reg) {
    case 0x00: d = cmd; break;
    case 0x01: d = 0x0200 | status; break;
    case 0x02: d = 0x0007; break;
    case 0x03: d = 0x0900; break;
    case 0x04: case 0x05: d = stream_nr; break;
    case 0x06: d = ivec; break;
    case 0x07: d = 0x0042; break;
    case 0x08: d = (uint16_t)(fma_dclk() >> 16); dclkl_latch = (uint16_t)fma_dclk(); break;
    case 0x09: d = dclkl_latch; break;
    case 0x0a: d = (uint16_t)(audio_header >> 16); break;
    case 0x0b: d = (uint16_t)audio_header; break;
    case 0x0c: d = dsp_enable ? 1 : 0; break;
    case 0x0d: d = isr; isr = 0; break;          /* read clears */
    case 0x0e: d = ier; break;
    case 0x12: d = 0x0004; break;                /* DSP56001 HF2 */
    default: break;
    }
    if (fmv_log >= 2 && reg != 0x0d && reg != 0x08 && reg != 0x09)
        fprintf(stderr, "[FMV] A R %02X -> %04X\n", reg * 2, d);
    return d;
}

void fmva_write(int reg, uint16_t v)
{
    if (fmv_log >= 2 || (fmv_log && reg != 0x12 && reg != 0x11))
        fprintf(stderr, "[FMV] A W %02X = %04X\n", reg * 2, v);
    switch (reg) {
    case 0x00:
        cmd = v;
        if (v & 0x8000) { status &= (uint16_t)~FMA_UNF; fmv_board_drq(1); }
        if (v & 1) {                                 /* stop */
            dsp_enable = 0; status = 0;
            mpeg_demux_reset(&demux);
            start_valid = 0;
            decoder_new();
            audio_clear();
        }
        break;
    case 0x04: stream_nr = v & 0x0f; pending_stream_change = 1; break;
    case 0x06: ivec = v; break;
    case 0x0e: ier = v; break;
    default: break;
    }
}
