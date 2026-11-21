/*
 * cdic.c - CD Interface Controller (CDIC) emulation (see cdic.h)
 *
 * Sectors arrive at single speed (75 per second). Each delivered sector is
 * copied into a RAM buffer with its subcode Q, then XBUF bit 15 is set and
 * the CDIC interrupts (level 4, vector from IVEC) if DBUF bit 14 is set.
 * Real-time audio: during a mode 2 read, an ADPCM sector whose channel is
 * in the audio mask ($3C0C) goes to audio buffer 4 or 5 ($2800 / $3200,
 * DBUF bit 2) with the same interrupt; cdapdriv then starts the playback
 * (AUDCTL = $800, bit 11 = playing). The sector is decoded and sent to the
 * host sound output here.
 *
 * Sound maps from memory ("ap", cdapdriv $1AA1A8): the driver DMAs 2304
 * bytes of ADPCM into $2800 / $3200 (coding byte at +$0B, data at +$0C)
 * and writes AUDCTL = $2800 (bit 11 play, bit 13 interrupt). Each buffer
 * lasts its audio time; then ABUF bit 15 is set (interrupt) and the other
 * buffer plays while the driver refills this one. A buffer with coding
 * $FF ("empty", written by the stop routine $1A9AA4) ends the playback.
 */

#include "cdic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_BASE        0x3C00
#define CPU_HZ          15000000
#define SECTOR_CYCLES   (CPU_HZ / 75)
#define SEEK_CYCLES     (SECTOR_CYCLES * 2)
#define ONESHOT_CYCLES  2000

#define DBUF_START      0x8000
#define DBUF_IRQ_EN     0x4000

/* subheader submode bits */
#define SM_EOR    0x01
#define SM_VIDEO  0x02
#define SM_AUDIO  0x04
#define SM_DATA   0x08
#define SM_FORM2  0x20

static void cdic_logf(cdic_t *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void cdic_logf(cdic_t *c, const char *fmt, ...)
{
    if (!c->log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[CDIC] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void cdic_init(cdic_t *c)
{
    disc_t keep = c->disc;
    int had = disc_present(&c->disc);
    cdic_audio_cb cb = c->audio_cb;
    void *ctx = c->audio_ctx;
    memset(c, 0, sizeof(*c));
    if (had) c->disc = keep;
    c->audio_cb = cb;
    c->audio_ctx = ctx;
    c->audctl = 0xD7FE;              /* value expected by the boot diagnostic */
    const char *s = getenv("EMU_CDIC_LOG");
    c->log = (s && *s) ? atoi(s) : 0;     /* 1: commands, 2: + every interrupt */
}

int cdic_insert(cdic_t *c, const char *path)
{
    c->busy = 0;
    return disc_open(&c->disc, path);
}

void cdic_eject(cdic_t *c)
{
    c->busy = 0;
    disc_close(&c->disc);
}

int cdic_has_disc(const cdic_t *c) { return disc_present(&c->disc); }

void cdic_set_audio(cdic_t *c, cdic_audio_cb cb, void *ctx)
{
    c->audio_cb = cb;
    c->audio_ctx = ctx;
}

int cdic_is_ram(uint32_t addr)
{
    return addr >= CDIC_RAM_BASE && addr < CDIC_RAM_BASE + CDIC_RAM_SIZE;
}

/* ---- sector delivery ---- */

static void put_q(cdic_t *c, int buf, const uint8_t q[12])
{
    uint8_t *p = c->ram + buf * 0xA00 + 0x924;
    for (int i = 0; i < 12; i++) { p[2 * i] = 0; p[2 * i + 1] = q[i]; }
}

static void raise_data(cdic_t *c, int buf)
{
    c->dbuf = (uint16_t)((c->dbuf & ~(DBUF_START | 0x01C5)) | buf);
    c->xbuf |= 0x8000;
    c->delivered++;
}

/* Q of a program-area sector */
static void q_program(cdic_t *c, int lba, int t, uint8_t q[12])
{
    memset(q, 0, 12);
    const disc_track_t *tr = &c->disc.track[t < 0 ? 0 : t];
    q[0] = (uint8_t)((tr->audio ? 0x00 : 0x40) | 0x01);
    q[1] = disc_bcd(tr->number);
    q[2] = 0x01;
    int rel = lba - tr->start;
    if (rel < 0) rel = -rel;
    q[3] = disc_bcd(rel / (60 * 75));
    q[4] = disc_bcd((rel / 75) % 60);
    q[5] = disc_bcd(rel % 75);
    disc_lba_to_msf(lba, q + 7);
}

/* Lead-in Q entries, cycled: A0, A1, A2, then each track */
static void q_toc(cdic_t *c, int pos, uint8_t q[12])
{
    disc_t *d = &c->disc;
    int n = d->ntracks + 3;
    pos %= n;
    memset(q, 0, 12);
    int first_data = !d->track[0].audio;
    disc_lba_to_msf(pos, q + 3);                  /* running lead-in time */
    if (pos == 0) {
        q[0] = (uint8_t)((first_data ? 0x40 : 0x00) | 1);
        q[2] = 0xA0;
        q[7] = disc_bcd(d->track[0].number);
        q[8] = first_data ? 0x10 : 0x00;          /* disc type: CD-i */
    } else if (pos == 1) {
        q[0] = (uint8_t)((d->track[d->ntracks - 1].audio ? 0x00 : 0x40) | 1);
        q[2] = 0xA1;
        q[7] = disc_bcd(d->track[d->ntracks - 1].number);
    } else if (pos == 2) {
        q[0] = (uint8_t)((first_data ? 0x40 : 0x00) | 1);
        q[2] = 0xA2;
        disc_lba_to_msf(d->leadout, q + 7);
    } else {
        const disc_track_t *t = &d->track[pos - 3];
        q[0] = (uint8_t)((t->audio ? 0x00 : 0x40) | 1);
        q[2] = disc_bcd(t->number);
        disc_lba_to_msf(t->start, q + 7);
    }
}

static void deliver_toc(cdic_t *c)
{
    uint8_t q[12];
    int buf = c->buffer;
    memset(c->ram + buf * 0xA00, 0, 0x924);
    q_toc(c, c->toc_pos++, q);
    put_q(c, buf, q);
    raise_data(c, buf);
    c->buffer ^= 1;
}

/* One sector time of a read / play / seek command */
static void deliver_sector(cdic_t *c)
{
    static uint8_t raw[DISC_RAW];
    int lba = c->lba++;
    int t = disc_read(&c->disc, lba, raw);
    int mode = raw[15];

    if (c->run_cmd == 0x2A && mode == 2) {
        uint8_t file = raw[16], chan = raw[17] & 0x1F, sm = raw[18];
        /* Empty sectors (no data/audio/video bit) are padding, except the
         * ones that close a record or a file (EOR $01, trigger $10, EOF $80):
         * cdapdriv counts EORs to end a play and signal the application
         * (Asterix: $E1 markers after each sequence, e.g. LBA 53085). */
        if ((sm & (SM_DATA | SM_AUDIO | SM_VIDEO)) == 0 && !(sm & 0x91))
            return;
        if (file != (c->file >> 8))
            return;
        /* Audio sector in the audio mask: played (audio buffers 4/5).
         * Otherwise it falls through to the data path below: a channel in
         * the data mask loads ADPCM into memory (sound maps, e.g. Asterix
         * reads its sound effects that way, channel mask $80, audio 0). */
        if ((sm & SM_AUDIO) && (sm & SM_FORM2) && (c->audio_channel & (1u << chan))) {
            int abuf = 4 | c->abuffer;
            memcpy(c->ram + abuf * 0xA00, raw + 12, 0x924);
            uint8_t q[12];
            q_program(c, lba, t, q);
            put_q(c, abuf, q);
            raise_data(c, abuf);
            c->abuffer ^= 1;
            c->audio_sectors++;
            if (c->audio_cb) {
                static int16_t pcm[2 * ADPCM_MAX_FRAMES];
                int n = adpcm_decode_sector(&c->adpcm, raw, pcm);
                c->audio_cb(c->audio_ctx, pcm, n);
            }
            if (c->log && (c->audio_sectors <= 2 || c->audio_sectors % 500 == 0))
                cdic_logf(c, "LBA %d: ADPCM sector (chan %d, coding %02X) -> buffer %d #%ld\n",
                          lba, chan, raw[19], abuf, c->audio_sectors);
            return;
        }
        if (!(c->channel & (1u << chan)))
            return;
    }

    int buf = c->buffer;
    memcpy(c->ram + buf * 0xA00, raw + 12, 0x924);
    uint8_t q[12];
    q_program(c, lba, t, q);
    put_q(c, buf, q);
    raise_data(c, buf);
    c->buffer ^= 1;
    if (c->log && (c->delivered < 8 || (c->delivered % 500) == 0 || c->log >= 3))
        cdic_logf(c, "cmd %02X LBA %d -> buffer %d (mode %d, sub %02X %02X %02X %02X) #%ld\n",
                  c->run_cmd, lba, buf, mode, raw[16], raw[17], raw[18], raw[19], c->delivered);
}

static void start_command(cdic_t *c)
{
    uint8_t m = (uint8_t)(c->time >> 24), s = (uint8_t)(c->time >> 16), f = (uint8_t)((c->time >> 8) & 0x7F);
    cdic_logf(c, "command %02X time %08X (LBA %d) file %04X chan %08X audio %04X%s\n",
              c->command, (unsigned)c->time, disc_msf_to_lba(m, s, f), c->file,
              (unsigned)c->channel, c->audio_channel, cdic_has_disc(c) ? "" : " [no disc]");
    switch (c->command) {
    case 0x27:                                  /* read TOC */
        c->run_cmd = c->command;
        c->busy = cdic_has_disc(c);
        c->toc_pos = 0;
        c->countdown = SEEK_CYCLES;
        break;
    case 0x28: case 0x29: case 0x2A: case 0x2C:  /* play / read / seek */
        adpcm_reset(&c->adpcm);
        c->run_cmd = c->command;
        c->busy = cdic_has_disc(c);
        c->lba = disc_msf_to_lba(m, s, f);
        c->countdown = SEEK_CYCLES;
        break;
    case 0x2E:                                  /* update: filters already live */
        /* Accepted at once: DBUF bit 15 drops even if no sector passes the
         * new filters. cdapdriv's alarm stops the drive (command $24) only
         * once bit 15 is clear; keeping it set made the read run on until
         * a late sector was DMA'd into a buffer the game had freed. */
        if (c->busy) c->dbuf &= (uint16_t)~DBUF_START;
        else { c->oneshot = 1; c->countdown = ONESHOT_CYCLES; }
        break;
    default:                                    /* $23/$24 reset, $2B stop, ... */
        c->busy = 0;
        c->oneshot = 1;
        c->countdown = ONESHOT_CYCLES;
        break;
    }
}

/* Sound map playback (see the header comment) */
static void mp_start_buffer(cdic_t *c)
{
    uint32_t off = c->mp_buf ? 0x3200 : 0x2800;
    uint8_t coding = c->ram[off + 0x0B];
    if (coding == 0xFF) {                         /* empty buffer: stop */
        c->mp_active = 0;
        c->mp_playing = 0;
        cdic_logf(c, "sound map: buffer %04X empty, stop after %ld buffers\n", (unsigned)off, c->mp_buffers);
        return;
    }
    static uint8_t raw[DISC_RAW];
    static int16_t pcm[2 * ADPCM_MAX_FRAMES];
    memset(raw, 0, sizeof(raw));
    raw[19] = coding;
    memcpy(raw + 24, c->ram + off + 0x0C, 0x900);
    int n = adpcm_decode_sector(&c->mp_adpcm, raw, pcm);
    if (c->audio_cb) c->audio_cb(c->audio_ctx, pcm, n);
    if (n < 1) n = 1;
    c->mp_countdown = (int)((int64_t)n * CPU_HZ / ADPCM_RATE);
    c->mp_playing = 1;
    c->mp_buffers++;
    if (c->log >= 2 || c->mp_buffers <= 2)
        cdic_logf(c, "sound map: buffer %04X coding %02X, %d frames\n", (unsigned)off, coding, n);
}

static void mp_tick(cdic_t *c, int cycles)
{
    if (!c->mp_active) return;
    c->mp_countdown -= cycles;
    if (c->mp_countdown > 0) return;
    if (c->mp_playing) {                          /* buffer consumed */
        c->mp_playing = 0;
        c->abuf |= 0x8000;
        c->mp_buf ^= 1;
    }
    mp_start_buffer(c);
}

void cdic_tick(cdic_t *c, int cycles)
{
    mp_tick(c, cycles);
    if (!c->busy && !c->oneshot) return;
    c->countdown -= cycles;
    if (c->countdown > 0) return;

    if (c->oneshot) {
        c->oneshot = 0;
        c->dbuf &= (uint16_t)~DBUF_START;
        c->xbuf |= 0x8000;
        if (!c->busy) return;
    }
    c->countdown += SECTOR_CYCLES;
    if (c->run_cmd == 0x27) deliver_toc(c);
    else                    deliver_sector(c);
}

int cdic_irq(cdic_t *c, int *vector)
{
    int data  = (c->xbuf & 0x8000) && (c->dbuf & DBUF_IRQ_EN);
    int audio = (c->abuf & 0x8000) && (c->audctl & 0x2000);
    if (!data && !audio) return 0;
    *vector = c->ivec & 0xFF;
    return CDIC_IRQ_LEVEL;
}

/* ---- register access ---- */

uint16_t cdic_read16(cdic_t *c, uint32_t addr)
{
    uint32_t o = (addr - CDIC_RAM_BASE) & 0x3FFE;
    uint16_t v;
    switch (o) {
    case 0x3C00: return c->command;
    case 0x3C02: return (uint16_t)(c->time >> 16);
    case 0x3C04: return (uint16_t)c->time;
    case 0x3C06: return c->file;
    case 0x3C08: return (uint16_t)(c->channel >> 16);
    case 0x3C0A: return (uint16_t)c->channel;
    case 0x3C0C: return c->audio_channel;
    case 0x3FF4: v = c->abuf; c->abuf &= 0x7FFF; return v;
    case 0x3FF6:
        v = c->xbuf;
        if ((v & 0x8000) && c->log >= 2) cdic_logf(c, "XBUF read %04X (DBUF %04X): interrupt acknowledged\n", v, c->dbuf);
        c->xbuf &= 0x7FFF;
        return v;
    case 0x3FF8: return c->dmactl;
    case 0x3FFA: return c->audctl;
    case 0x3FFC: return c->ivec;
    case 0x3FFE: return c->dbuf;
    default:     return (uint16_t)((c->ram[o] << 8) | c->ram[o + 1]);
    }
}

void cdic_write16(cdic_t *c, uint32_t addr, uint16_t v)
{
    uint32_t o = (addr - CDIC_RAM_BASE) & 0x3FFE;
    switch (o) {
    case 0x3C00: c->command = v; break;
    case 0x3C02: c->time = (c->time & 0x0000FFFFu) | ((uint32_t)v << 16); break;
    case 0x3C04: c->time = (c->time & 0xFFFF0000u) | v; break;
    case 0x3C06: c->file = v; break;
    case 0x3C08: c->channel = (c->channel & 0x0000FFFFu) | ((uint32_t)v << 16); break;
    case 0x3C0A: c->channel = (c->channel & 0xFFFF0000u) | v; break;
    case 0x3C0C: c->audio_channel = v; break;
    case 0x3FF4: c->abuf = v; break;
    case 0x3FF6: c->xbuf = v; break;
    case 0x3FF8: c->dmactl = v; break;
    case 0x3FFA:
        if ((v & 0x2800) == 0x2800 && (c->audctl & 0x2800) != 0x2800) {
            c->mp_active = 1;                     /* sound map: start at $2800 */
            c->mp_playing = 0;
            c->mp_buf = 0;
            c->mp_countdown = ONESHOT_CYCLES;
            adpcm_reset(&c->mp_adpcm);
        } else if ((v & 0x2800) == 0x2800 && !c->mp_active) {
            c->mp_active = 1;                     /* rewritten while idle: restart */
            c->mp_playing = 0;
            c->mp_buf = 0;
            c->mp_countdown = ONESHOT_CYCLES;
            adpcm_reset(&c->mp_adpcm);
        } else if (!(v & 0x0800)) {
            c->mp_active = 0;                     /* playback off */
            c->mp_playing = 0;
        }
        c->audctl = v;
        break;
    case 0x3FFC: c->ivec = v; break;
    case 0x3FFE:
        c->dbuf = v;
        if (v & DBUF_START)
            start_command(c);
        else if (!(v & DBUF_IRQ_EN)) {
            if (c->busy) cdic_logf(c, "stop (DBUF=%04X) after %ld sectors\n", v, c->delivered);
            c->busy = 0;
            c->oneshot = 0;
        }
        break;
    default:
        c->ram[o] = (uint8_t)(v >> 8);
        c->ram[o + 1] = (uint8_t)v;
        break;
    }
}

uint8_t cdic_read8(cdic_t *c, uint32_t addr)
{
    uint32_t o = addr - CDIC_RAM_BASE;
    if (o < REG_BASE) return c->ram[o];
    uint16_t w = cdic_read16(c, addr & ~1u);
    return (addr & 1) ? (uint8_t)w : (uint8_t)(w >> 8);
}

void cdic_write8(cdic_t *c, uint32_t addr, uint8_t v)
{
    uint32_t o = addr - CDIC_RAM_BASE;
    if (o < REG_BASE) { c->ram[o] = v; return; }
    /* merge into the word without the read side effects */
    uint32_t e = o & 0x3FFE;
    uint16_t w;
    switch (e) {
    case 0x3FF4: w = c->abuf; break;
    case 0x3FF6: w = c->xbuf; break;
    default:     w = cdic_read16(c, addr & ~1u); break;
    }
    w = (addr & 1) ? (uint16_t)((w & 0xFF00) | v) : (uint16_t)((w & 0x00FF) | (v << 8));
    cdic_write16(c, addr & ~1u, w);
}
