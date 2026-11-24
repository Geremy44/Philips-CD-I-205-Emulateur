/*
 * mcd251.c - video side of the Digital Video Cartridge: the MCD251 MPEG-1
 * video chip, registers at $E04000
 *
 * Written for this emulator from docs/fmv/FMV_COMPORTEMENT.md (section 3).
 * Pictures are decoded by mpeg1_video.c; this file follows the stream as it
 * arrives, puts the pictures in display order and paces them.
 *
 * fmvv_pixel() takes the chip's own coordinates: 384 pixels across the
 * active line, lines from the start of active video.
 */

#include "fmv_internal.h"
#include "mpeg_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mpeg1_video.h"

#define DCLK_HZ       45000
#define PERIOD_CLOCK  30000000              /* clock of the picture period */
#define START_DELAY   1000                  /* DCLK from play / continue to the first tick */
#define PAUSE_DELAY   100
#define PAL_RATE      0x0708
#define QUEUE_MAX     8                     /* pictures waiting for display */
#define NOTES_MAX     64                    /* pictures seen in the input, not decoded yet */
#define WANTS_BELOW   (32 * 1024)
#define INPUT_MAX     (2 * 1024 * 1024)
#define CLOSE_AFTER   8                     /* bytes after the next start code */

/* interrupts */
#define I_SEQUENCE     0x0001
#define I_GOP          0x0002
#define I_PICTURE      0x0004
#define I_END_OF_DATA  0x0008
#define I_NO_DATA      0x0020
#define I_WINDOW       0x0080
#define I_TIMER        0x0100
#define I_SEQ_END      0x0200
#define I_PROGRAM_END  0x0400
#define I_BLANKING     0x0800
#define I_PAUSED       0x1000
#define I_WINDOW2      0x2000

/* system command ($60) */
#define C_PLAY      0x0008
#define C_PAUSE     0x0010
#define C_CONTINUE  0x0020
#define C_STEP      0x0040
#define C_STOP      0x0080
#define C_FLUSH     0x0100
#define C_DEC_ON    0x1000
#define C_DEC_OFF   0x2000
#define C_TRANSFER  0x8000
/* display command ($61) */
#define D_SCROLL    0x0004
#define D_UPDATE    0x0008
#define D_HIDE      0x0100
#define D_SHOW      0x0200
#define D_SHOW_NEXT 0x0400

/* picture period from the picture_rate code */
static const uint32_t PERIOD_30M[16] = {
    1200000, 1251251, 1250000, 1200000, 1001001, 1000000, 600000, 500500,
    500000, 1200000, 1200000, 1200000, 1200000, 1200000, 1200000, 1200000 };
static const uint16_t PERIOD_90K[16] = {
    3600, 3754, 3750, 3600, 3003, 3000, 1800, 1502,
    1500, 3600, 3600, 3600, 3600, 3600, 3600, 3600 };

typedef struct {
    uint16_t width, height;
    uint8_t  status;                        /* type << 6 | temporal reference */
    uint32_t timecode;
    uint8_t  rate;
    uint8_t  starts_gop, starts_seq, ends_seq;
} note_t;

typedef struct { uint32_t *rgb; note_t note; } frame_t;

static struct {
    /* registers */
    uint16_t pending, mask, vector, sys_cmd, disp_cmd, scr_ctl, dec_cmd, vdi_cmd, rate_req, timer;
    uint8_t  stream, slow;
    uint16_t img_w, img_h, img_rate;
    uint16_t win[10];                       /* $36-$3F */
    uint16_t at_x, at_y, from_x, from_y, cut_w, cut_h;   /* window in use */
    int      update_wanted, update_on_blank, visible;
    int      decoding, running;
    /* clocks (CPU periods) */
    uint64_t clock_origin;
    int32_t  clock_adjust;
    int      clock_set;
    uint64_t timer_at, start_at, pause_at, tick_at;
    /* input */
    mps_t    sys;
    int64_t  last_dts, due[NOTES_MAX];      /* DTS of each picture in the input, then queued */
    int      due_n;
    mpv_t   *dec;                           /* holds the input not decoded yet */
    note_t   notes[NOTES_MAX];
    int      notes_n;
    int      whole;                         /* pictures entirely in the input */
    int      closing;                       /* bytes still to come before the last one is whole */
    int      flushing;                      /* no more data for the open picture */
    int      in_input, starving;
    note_t   held_note;
    int      held;                          /* a reference waits for reordering */
    /* input scanner */
    uint32_t shift;
    int      hdr_kind, hdr_got, seq_seen, pic_open, new_gop, new_seq;
    uint8_t  hdr[4];
    uint32_t timecode;
    uint8_t  rate;
    /* output */
    frame_t  queue[QUEUE_MAX];
    int      queued;
    frame_t  shown;
    int      shown_valid;
    note_t   shown_note;
    uint16_t dec_w, dec_h, dec_p90;
    uint32_t dec_tc;
    uint8_t  dec_rate;
    uint32_t period;
    int32_t  drift;
    int      in_sync, show_on_blank, end_on_unblank, step_next;
} V = { .timer = 55, .rate = 3, .period = 1200000 };

#define HDR_PICTURE 0x100

unsigned long fmvv_shown;

static uint32_t raw_clock(void) { return (uint32_t)((fmv_now - V.clock_origin) * DCLK_HZ / FMV_CPU_HZ); }
static uint32_t chip_clock(void) { return raw_clock() + (uint32_t)V.clock_adjust; }
static uint64_t dclk_ticks(uint64_t n) { return fmv_ticks(n, DCLK_HZ); }

void fmvv_info(int *w, int *h, int *playing) { *w = V.dec_w; *h = V.dec_h; *playing = V.running && V.visible; }
int fmvv_irq_pending(void) { return (V.pending & V.mask) != 0; }
uint8_t fmvv_vector(void) { return (uint8_t)(V.vector >> 3); }
int fmvv_showing(void) { return V.visible && V.shown_valid; }

static void log_irq(const char *why)
{
    if (fmv_log >= 2 && (V.pending & ~(I_PICTURE | I_TIMER | I_BLANKING)))
        fprintf(stderr, "[FMV] video ISR %04X (%s)\n", V.pending, why);
}

/* ======================= input and decoder ======================= */

static void new_decoder(void)
{
    if (V.dec) mpv_destroy(V.dec);
    V.dec = mpv_create();
    V.notes_n = V.whole = 0;
    V.held = 0;
    V.shift = 0; V.hdr_kind = 0; V.seq_seen = V.pic_open = V.new_gop = V.new_seq = 0;
    V.closing = 0;
    V.flushing = 0;
}

static void free_frame(frame_t *f) { free(f->rgb); f->rgb = NULL; }

static void flush_all(void)
{
    new_decoder();
    V.starving = 0;
    for (int i = 0; i < V.queued; i++) free_frame(&V.queue[i]);
    V.queued = 0;
    V.due_n = 0; V.last_dts = 0;
    V.in_input = 0;
    V.in_sync = V.show_on_blank = V.end_on_unblank = 0;
    V.step_next = 0;
    V.drift = 0;
}

void fmvv_reset(void)
{
    V.pending = V.mask = V.vector = V.sys_cmd = V.disp_cmd = V.scr_ctl = V.dec_cmd = V.vdi_cmd = V.rate_req = 0;
    V.timer = 55; V.stream = 0; V.slow = 0;
    V.decoding = V.running = 0;
    V.img_w = V.img_h = V.img_rate = 0;
    memset(V.win, 0, sizeof(V.win));
    V.at_x = V.at_y = V.from_x = V.from_y = V.cut_w = V.cut_h = 0;
    V.update_wanted = V.update_on_blank = V.visible = 0;
    V.clock_origin = fmv_now; V.clock_adjust = 0; V.clock_set = 0;
    mps_reset(&V.sys);
    flush_all();
    free_frame(&V.shown); V.shown_valid = 0;
    memset(&V.shown_note, 0, sizeof(V.shown_note));
    V.dec_w = V.dec_h = V.dec_p90 = 0; V.dec_tc = 0; V.dec_rate = 0;
    V.start_at = V.pause_at = V.tick_at = FMV_NEVER;
    V.timer_at = fmv_now + dclk_ticks((uint64_t)(V.timer + 1) * 8);   /* 100.4 Hz at 55 */
}

/* a decoded picture, converted to RGB (ITU-R BT.601, studio range) */
static void enqueue(const mpv_frame_t *f, const note_t *note)
{
    if (V.queued >= QUEUE_MAX || !f || !f->y.data) return;
    int w = (int)f->width, h = (int)f->height;
    frame_t *q = &V.queue[V.queued];
    q->rgb = malloc((size_t)w * h * 4);
    if (!q->rgb) return;
    q->note = *note;
    q->note.width = (uint16_t)w; q->note.height = (uint16_t)h;
    for (int y = 0; y < h; y++) {
        const uint8_t *py = f->y.data + (size_t)y * f->y.width;
        const uint8_t *pb = f->cb.data + (size_t)(y >> 1) * f->cb.width;
        const uint8_t *pr = f->cr.data + (size_t)(y >> 1) * f->cb.width;
        uint32_t *o = q->rgb + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int cr = pr[x >> 1] - 128, cb = pb[x >> 1] - 128;
            int lum = ((py[x] - 16) * 76309) >> 16;
            int r = lum + ((cr * 104597) >> 16);
            int g = lum - ((cb * 25674 + cr * 53278) >> 16);
            int b = lum + ((cb * 132201) >> 16);
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            o[x] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    }
    V.queued++;
}

static void release_reference(void)
{
    if (!V.held) return;
    V.held = 0;
    enqueue(mpv_frame(V.dec, MPV_REF_NEW), &V.held_note);   /* the newest reference */
}

static void sequence_over(void)
{
    release_reference();
    V.pending |= I_SEQ_END;
    log_irq("sequence end");
}

/* one picture through the decoder: 1 I, 2 P, 3 B; 0 not ready; -1 skipped */
static int decode_picture(void)
{
    if (V.whole <= 0 || !mpv_has_header(V.dec)) return 0;
    /* whole only once the next picture's start code is in (or no more
     * data comes): a truncated picture would spoil every picture predicted
     * from it until the next I picture */
    int t = mpv_decode(V.dec, V.flushing);
    if (t == MPV_NO_PICTURE) { V.whole = 0; return 0; }
    if (t == MPV_WAIT) return 0;
    V.whole--;
    if (V.in_input) V.in_input--;
    return t > 0 ? t : -1;
}

static void decode_ready(void)
{
    if (!V.decoding) return;
    V.starving = 0;
    while (V.queued < QUEUE_MAX) {
        int type = decode_picture();
        if (!type) { V.starving = 1; break; }
        note_t n;
        memset(&n, 0, sizeof(n));
        if (V.notes_n) { n = V.notes[0]; memmove(V.notes, V.notes + 1, (size_t)--V.notes_n * sizeof(note_t)); }
        if (type < 0) { if (n.ends_seq) sequence_over(); continue; }
        n.width = (uint16_t)mpv_width(V.dec); n.height = (uint16_t)mpv_height(V.dec);
        V.dec_w = n.width; V.dec_h = n.height; V.dec_tc = n.timecode;
        V.dec_rate = n.rate & 15;
        V.dec_p90 = PERIOD_90K[V.dec_rate];
        V.period = PERIOD_30M[V.dec_rate];
        if (V.running) { V.img_w = V.dec_w; V.img_h = V.dec_h; V.img_rate = V.dec_rate; }
        if (fmv_log >= 3)
            fprintf(stderr, "[FMV] decoded %dx%d type %d at %.2f s, %d queued\n",
                    V.dec_w, V.dec_h, type, (double)fmv_now / FMV_CPU_HZ, V.queued);
        if (type == 3) enqueue(mpv_frame(V.dec, MPV_LAST_B), &n);          /* B: in display order already */
        else {
            if (V.held) enqueue(mpv_frame(V.dec, MPV_REF_OLD), &V.held_note);   /* the previous reference */
            V.held_note = n;
            V.held = 1;
        }
        if (n.ends_seq) sequence_over();
    }
}

/* header fields read from the input as it arrives */
static void header_done(void)
{
    if (V.hdr_kind == 0xB3) V.rate = V.hdr[3] & 15;
    else if (V.hdr_kind == 0xB8) {
        /* 25-bit time code: drop, hours 5, minutes 6, marker, seconds 6, pictures 6 */
        uint32_t t = ((uint32_t)V.hdr[0] << 24 | V.hdr[1] << 16 | V.hdr[2] << 8 | V.hdr[3]) >> 7;
        uint32_t pic = t & 63, sec = (t >> 6) & 63, min = (t >> 13) & 63, hour = (t >> 19) & 31;
        V.timecode = sec << 22 | pic << 16 | hour << 6 | min;
    } else if (V.hdr_kind == HDR_PICTURE && V.notes_n < NOTES_MAX) {
        note_t *n = &V.notes[V.notes_n++];
        memset(n, 0, sizeof(*n));
        int tref = V.hdr[0] << 2 | V.hdr[1] >> 6, type = (V.hdr[1] >> 3) & 7;
        n->status = (uint8_t)((type & 3) << 6 | (tref & 63));
        n->timecode = V.timecode;
        n->rate = V.rate;
        if (type == 1) { n->starts_gop = (uint8_t)V.new_gop; n->starts_seq = (uint8_t)V.new_seq; V.new_gop = V.new_seq = 0; }
    }
    V.hdr_kind = 0;
}

static void scan_input(uint8_t b)
{
    if (V.closing && --V.closing == 0) V.whole++;
    if (V.hdr_kind) {
        V.hdr[V.hdr_got++] = b;
        if (V.hdr_got == (V.hdr_kind == HDR_PICTURE ? 2 : 4)) header_done();
        return;
    }
    V.shift = V.shift << 8 | b;
    if ((V.shift >> 8) != 0x000001) return;
    uint8_t code = (uint8_t)V.shift;
    if (code >= 0x01 && code <= 0xAF) return;          /* slice */
    if (code == 0xB3) { V.seq_seen = 1; V.new_seq = V.new_gop = 1; }
    if (code == 0xB8) V.new_gop = 1;
    if (!V.seq_seen) return;
    if (V.pic_open) {                                   /* this code ends the open picture */
        if (V.closing) V.whole++;
        V.closing = CLOSE_AFTER;
        V.pic_open = 0;
    }
    switch (code) {
    case 0x00:
        V.pic_open = 1;
        V.flushing = 0;
        V.in_input++;
        if (V.due_n < NOTES_MAX) V.due[V.due_n++] = V.last_dts;
        V.last_dts = 0;
        V.hdr_kind = HDR_PICTURE; V.hdr_got = 0;
        break;
    case 0xB3: case 0xB8:
        V.hdr_kind = code; V.hdr_got = 0;
        break;
    case 0xB7:                                          /* sequence end */
        V.flushing = 1;
        if (V.notes_n) V.notes[V.notes_n - 1].ends_seq = 1;
        else sequence_over();
        break;
    }
    V.shift = 0;
}

void fmvv_data(uint8_t b)
{
    int kept = V.sys.payload;
    mps_byte(&V.sys, b, (uint8_t)(0xE0 | V.stream));
    if (kept) {
        if (mpv_buffered(V.dec) < INPUT_MAX) mpv_write(V.dec, &b, 1);
        {   static FILE *f = NULL; static int tried = 0;   /* EMU_FMV_ES_DUMP=file (tests) */
            if (!tried) { tried = 1; if (getenv("EMU_FMV_ES_DUMP")) f = fopen(getenv("EMU_FMV_ES_DUMP"), "wb"); }
            if (f) fputc(b, f);
        }
        scan_input(b);
    }
    if (V.sys.events & MPS_DTS) { V.last_dts = V.sys.dts; V.vdi_cmd |= 0x4000; }
    if (V.sys.events & MPS_END) {
        /* end of program: whatever is in is whole, and the held reference goes too */
        if (V.closing) { V.whole++; V.closing = 0; }
        V.flushing = 1;
        if (V.pic_open) { V.whole++; V.pic_open = 0; }
        decode_ready();
        release_reference();
        V.pending |= I_PROGRAM_END;
        if (fmv_log)
            fprintf(stderr, "[FMV] video program end at %.2f s: %d queued, %d in input\n",
                    (double)fmv_now / FMV_CPU_HZ, V.queued, V.in_input);
        log_irq("program end");
    }
}

void fmvv_end_of_transfer(void) { decode_ready(); }

/* ======================= display pacing ======================= */

static void arm_period(void)
{
    if (!V.running) { V.tick_at = FMV_NEVER; return; }
    int64_t p = ((int64_t)V.period - 1 - (int64_t)V.drift * 2048) & 0xFFFFFF;
    if (V.slow > 1) p *= V.slow;
    if (p < 1200) p = V.period;
    V.tick_at = fmv_now + fmv_ticks((uint64_t)p, PERIOD_CLOCK);
}

static void period_tick(void)
{
    if (!V.running) return;
    int follow_dts = (V.scr_ctl >> 2) & 1;
    if (V.due_n) {
        /* chip clock vs the next picture's DTS, in units of 128 at 90 kHz */
        int32_t d = (int32_t)((chip_clock() >> 6) & 0x7FFF) - (int32_t)((V.due[0] >> 7) & 0x7FFF);
        if (d >= 0x4000) d -= 0x8000;
        else if (d < -0x4000) d += 0x8000;
        if (!follow_dts || d > -30) V.in_sync = 1;
        int known = V.due[0] != 0;
        int far_ahead = known && mps_ts_diff(V.sys.scr, V.due[0]) > 15000;
        V.drift = (V.clock_set && follow_dts && known && !far_ahead) ? d : 0;
    } else { V.in_sync = 1; V.drift = 0; }
    if (!follow_dts || V.in_sync) V.show_on_blank = 1;
    arm_period();
}

/* EMU_FMV_PICS=dir,from,to (seconds): the pictures shown then, as BMP (tests) */
static void save_shown(void)
{
    static int init = 0, count = 0;
    static char dir[512];
    static double t0, t1;
    if (!init) {
        init = 1;
        const char *e = getenv("EMU_FMV_PICS");
        if (!e || sscanf(e, "%500[^,],%lf,%lf", dir, &t0, &t1) != 3) dir[0] = 0;
        if (e) fprintf(stderr, "[FMV] saving pictures to '%s' from %.1f to %.1f s\n", dir, t0, t1);
    }
    double t = (double)fmv_now / FMV_CPU_HZ;
    if (!dir[0] || t < t0 || t > t1 || count >= 300 || !V.shown.rgb) return;
    char name[600];
    int w = V.shown.note.width, h = V.shown.note.height;
    snprintf(name, sizeof(name), "%s/p%04d_%07.3f_%c%02d.bmp", dir, count++, t,
             "XIPB"[(V.shown.note.status >> 6) & 3], V.shown.note.status & 63);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    uint8_t hd[54] = { 'B', 'M' };
    uint32_t size = 54u + (uint32_t)w * h * 4u, top_down = (uint32_t)-h;
    for (int i = 0; i < 4; i++) { hd[2 + i] = (uint8_t)(size >> 8 * i); hd[22 + i] = (uint8_t)(top_down >> 8 * i); }
    hd[10] = 54; hd[14] = 40; hd[18] = (uint8_t)w; hd[19] = (uint8_t)(w >> 8); hd[26] = 1; hd[28] = 32;
    fwrite(hd, 1, 54, f);
    fwrite(V.shown.rgb, 4, (size_t)w * h, f);
    fclose(f);
}

static void show_next(void)
{
    if (!V.queued) return;
    free_frame(&V.shown);
    V.shown = V.queue[0];
    memmove(V.queue, V.queue + 1, (size_t)--V.queued * sizeof(frame_t));
    V.queue[V.queued].rgb = NULL;
    V.shown_valid = 1;
    fmvv_shown++;
    if (V.due_n) memmove(V.due, V.due + 1, (size_t)--V.due_n * sizeof(int64_t));
    V.shown_note = V.shown.note;
    save_shown();
}

static void take_window(void)
{
    V.pending |= I_WINDOW2 | I_WINDOW;
    V.update_wanted = 0;
    V.at_y = V.win[4]; V.at_x = V.win[5];
    V.cut_h = V.win[6]; V.cut_w = V.win[7];
    V.from_y = V.win[8]; V.from_x = V.win[9];
}

/* blanking starts (1) / the picture starts (0) */
void fmvv_vblank(int blanking)
{
    if (blanking) {
        if (V.show_on_blank) {
            V.show_on_blank = 0;
            if (V.update_wanted && !V.update_on_blank) take_window();
            if (V.decoding && V.running && !V.queued) release_reference();   /* ran dry */
            if (V.queued) {
                if (V.running && V.queued == 1 && V.in_input == 0 && !V.held) V.pending |= I_NO_DATA;
                show_next();
                V.end_on_unblank = 1;
            }
        }
        if (V.step_next && V.queued) { V.step_next = 0; show_next(); V.end_on_unblank = 1; }
        V.pending |= I_BLANKING;
        if (V.update_wanted && V.update_on_blank) take_window();
        log_irq("blanking");
        return;
    }
    if (V.end_on_unblank) {
        V.end_on_unblank = 0;
        V.pending |= I_PICTURE;
        if (V.shown_note.starts_gop) V.pending |= I_GOP;
        if (V.shown_note.starts_seq) V.pending |= I_SEQUENCE;
        if (!V.queued && V.in_input == 0 && !V.held) V.pending |= I_END_OF_DATA;
        log_irq("picture");
    }
    decode_ready();
}

void fmvv_tick(void)
{
    while (fmv_now >= V.timer_at) {
        V.pending |= I_TIMER;
        fmva_poll();
        V.timer_at += dclk_ticks((uint64_t)(V.timer + 1) * 8);
    }
    if (fmv_now >= V.start_at) { V.start_at = FMV_NEVER; V.running = 1; arm_period(); }
    if (fmv_now >= V.pause_at) { V.pause_at = FMV_NEVER; V.pending |= I_PAUSED; }
    if (fmv_now >= V.tick_at) { V.tick_at = FMV_NEVER; period_tick(); }
}

int fmvv_pixel(int x, int y, uint32_t *rgb)
{
    if (!V.visible || !V.shown_valid || !V.shown.rgb) return 0;
    int dx = x - (int)V.at_x, dy = y - (int)V.at_y;
    if (dx < 0 || dy < 0) return 0;
    if (V.cut_w && dx >= (int)V.cut_w) return 0;
    if (V.cut_h && dy >= (int)V.cut_h) return 0;
    int sx = dx + (int)V.from_x, sy = dy + (int)V.from_y;
    if (sx >= (int)V.shown.note.width || sy >= (int)V.shown.note.height) return 0;
    *rgb = V.shown.rgb[(size_t)sy * V.shown.note.width + sx];
    return 1;
}

/* ======================= registers ======================= */

uint16_t fmvv_read(int reg)
{
    uint16_t v = 0;
    switch (reg) {
    case 0x01: v = V.img_w; break;
    case 0x02: v = V.img_h; break;
    case 0x03: v = V.img_rate; break;
    case 0x04: v = (uint16_t)(V.dec_tc >> 16); break;
    case 0x05: v = (uint16_t)V.dec_tc; break;
    case 0x29: v = V.shown_note.width; break;
    case 0x2A: v = V.shown_note.height; break;
    case 0x2B: v = V.shown_note.rate & 15; break;
    case 0x2C: v = (uint16_t)(V.shown_note.timecode >> 16); break;
    case 0x2D: v = (uint16_t)V.shown_note.timecode; break;
    case 0x2E: v = V.shown_note.status; break;
    case 0x2F: v = (mpv_buffered(V.dec) < WANTS_BELOW || V.starving) ? 0x2000 : 0; break;
    case 0x30: v = V.mask; break;
    case 0x31: v = V.pending; V.pending = 0; break;
    case 0x32: v = V.timer; break;
    case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A:
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: v = V.win[reg - 0x36]; break;
    case 0x44: v = V.dec_cmd; break;
    case 0x46: v = V.vdi_cmd; break;
    case 0x4C: v = (uint16_t)((chip_clock() >> 6) & 0x7FFF); break;
    case 0x4F: v = 1; break;
    case 0x50: v = (uint16_t)((V.sys.dts >> 7) & 0x7FFF); break;
    case 0x52: v = V.running ? (uint16_t)(V.queued + V.held + V.in_input) : (uint16_t)V.due_n; break;
    case 0x54: v = V.dec_p90; break;
    case 0x55: v = PAL_RATE; break;
    case 0x56: v = V.rate_req; break;
    case 0x60: v = V.sys_cmd; break;
    case 0x61: v = V.disp_cmd; break;
    case 0x62: v = V.stream; break;
    case 0x6E: v = V.vector; break;
    }
    if (fmv_log >= 2 && reg != 0x2F && reg != 0x31 && reg != 0x4C)
        fprintf(stderr, "[FMV] V R %02X -> %04X\n", reg * 2, v);
    return v;
}

static void stop_running(void) { V.running = 0; V.start_at = V.tick_at = FMV_NEVER; }

static void system_command(uint16_t v)
{
    V.sys_cmd = v;
    if (v & C_PLAY) {
        V.start_at = fmv_now + dclk_ticks(START_DELAY);
        V.img_w = V.dec_w; V.img_h = V.dec_h; V.img_rate = V.dec_rate;
        V.dec_cmd |= 0x0042;
        V.slow = v & 7;
    }
    if (v & C_PAUSE) { stop_running(); V.pause_at = fmv_now + dclk_ticks(PAUSE_DELAY); }
    if (v & C_CONTINUE) { V.start_at = fmv_now + dclk_ticks(START_DELAY); V.slow = v & 7; }
    if (v & C_STEP) { V.step_next = 1; V.start_at = FMV_NEVER; }
    if (v & C_STOP) stop_running();
    if (v & C_FLUSH) { stop_running(); flush_all(); mps_reset(&V.sys); V.decoding = 1; }
    if (v & C_DEC_ON) V.decoding = 1;
    if (v & C_DEC_OFF) {
        V.decoding = 0;
        stop_running();
        V.img_w = V.img_h = V.img_rate = 0;
        V.shown_note.status = 0; V.shown_note.timecode = 0;
        V.dec_tc = 0; V.dec_p90 = 0; V.dec_rate = 0;
        V.shown_valid = 0;
        flush_all();
    }
    if (v & C_TRANSFER) fmv_board_drq(0);
}

void fmvv_write(int reg, uint16_t v)
{
    if (fmv_log) fprintf(stderr, "[FMV] V W %02X = %04X\n", reg * 2, v);
    switch (reg) {
    case 0x01: V.img_w = v; break;
    case 0x02: V.img_h = v; break;
    case 0x03: V.img_rate = v; break;
    case 0x30: V.mask = v; break;
    case 0x32: V.timer = v; V.timer_at = fmv_now + dclk_ticks((uint64_t)(v + 1) * 8); break;
    case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A:
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: V.win[reg - 0x36] = v; break;
    case 0x44:
        V.dec_cmd = v;
        if ((v >> 8) == 0x22 && !(V.sys_cmd & C_PAUSE)) V.step_next = 1;
        break;
    case 0x46: V.vdi_cmd = v; break;
    case 0x4C:                              /* set the chip clock */
        V.clock_adjust = (int32_t)((((uint32_t)v & 0x7FFF) << 6) - (raw_clock() & 0x001FFFC0));
        V.clock_set = 1;
        break;
    case 0x56: V.rate_req = v; break;
    case 0x57: V.timer_at = fmv_now + dclk_ticks((uint64_t)(V.timer + 1) * 8); break;
    case 0x60: system_command(v); break;
    case 0x61:
        V.disp_cmd = v;
        if (v & D_UPDATE) { V.update_wanted = 1; V.update_on_blank = (v & D_SCROLL) != 0; }
        if (v & D_HIDE) V.visible = 0;
        if (v & (D_SHOW | D_SHOW_NEXT)) V.visible = 1;
        break;
    case 0x62: V.stream = v & 0x0F; break;
    case 0x63: V.scr_ctl = v; break;
    case 0x6E: V.vector = v; break;
    }
}
