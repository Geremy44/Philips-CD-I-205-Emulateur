/*
 * fmv_video.c - MPEG-1 video chip of the Digital Video Cartridge (MCD251,
 * "FMV" chip, registers at $E04000)
 *
 * Register map, status bits, interrupt and display pacing follow MAME's
 * mcd251 device (pull request #16181, BSD-3-Clause, Alexandre Derumier),
 * which was worked out from the CDi_MiSTer core by Andre Zeps. This is a C
 * port of that behaviour, with pl_mpeg (MIT, Dominic Szablewski) as the
 * MPEG-1 picture decoder.
 *
 * Coordinates of fmvv_pixel(): the chip's own, 384 pixels across the active
 * line, lines counted from the start of active video.
 *
 * Portions derived from MAME: Copyright (c) Alexandre Derumier,
 * BSD-3-Clause; full notice in THIRD_PARTY_LICENSES.md at the repository root.
 */

#include "fmv_internal.h"
#include "mpeg_demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include "pl_mpeg.h"

#define DCLK_HZ           45000
#define FRAME_CLOCK_HZ    30000000
#define PLAY_DELAY_TICKS  1000
#define PAUSE_DELAY_TICKS 100
#define DISPLAY_RATE_PAL  0x0708
#define MAX_PICTURES      8
#define VIDEO_FIFO_FULL   (32 * 1024)
#define INFO_Q            64

enum {      /* interrupt status / enable bits */
    IRQ_SEQ = 1 << 0, IRQ_GOP = 1 << 1, IRQ_PIC = 1 << 2, IRQ_EOD = 1 << 3,
    IRQ_RFB = 1 << 4, IRQ_NDAT = 1 << 5, IRQ_OVF = 1 << 6, IRQ_DCL = 1 << 7,
    IRQ_TIM = 1 << 8, IRQ_ESI = 1 << 9, IRQ_EII = 1 << 10, IRQ_VSYNC = 1 << 11,
    IRQ_PAI = 1 << 12, IRQ_VCUP = 1 << 13
};
enum {      /* SYSCMD */
    SYS_PLAY = 1 << 3, SYS_PAUSE = 1 << 4, SYS_CONTINUE = 1 << 5, SYS_STEP = 1 << 6,
    SYS_STOP = 1 << 7, SYS_CLEARFIFO = 1 << 8, SYS_DEC_ON = 1 << 12,
    SYS_DEC_OFF = 1 << 13, SYS_DMA = 1 << 15
};
enum {      /* VIDCMD */
    VID_SCROLL = 1 << 2, VID_REGSUPD = 1 << 3, VID_HIDE = 1 << 8, VID_SHOW = 1 << 9,
    VID_SHOWNEXT = 1 << 10
};

static const uint32_t PERIOD_30MHZ[16] = {
    1200000, 1251251, 1250000, 1200000, 1001001, 1000000, 600000, 500500,
    500000, 1200000, 1200000, 1200000, 1200000, 1200000, 1200000, 1200000 };
static const uint16_t PERIOD_90KHZ[16] = {
    3600, 3754, 3750, 3600, 3003, 3000, 1800, 1502,
    1500, 3600, 3600, 3600, 3600, 3600, 3600, 3600 };

typedef struct {
    uint16_t width, height;
    uint8_t  video_status;              /* picture type << 6 | temporal ref */
    uint32_t timecode;
    uint8_t  rate;                      /* picture_rate code */
    uint8_t  first_of_gop, first_of_seq, ends_seq;
} pinfo_t;

typedef struct { uint32_t *rgb; pinfo_t info; } picture_t;

/* ---- registers ---- */
static uint16_t isr, ier, ivec, syscmd, vidcmd, sysscr, dec_cmd, vdi_cmd, frame_rate, tcnt = 55;
static uint8_t  stream_nr, slow_motion;
static int      dsp_enable, playback_active;
static uint16_t image_w, image_h, image_rt;
static uint16_t vctrl[10];                       /* $6C-$7F: Yo Xo Ya Xa Yd Xd Wh Ww Yw Xw */
static uint16_t l_disp_x, l_disp_y, l_win_x, l_win_y, l_win_w, l_win_h;   /* latched */
static int      upd_latch, upd_scroll, show_video;

/* ---- timing (CPU clock periods) ---- */
static uint64_t dclk_origin;
static int32_t  dclk_offset;
static int      syscr_written;
static uint64_t tim_next = FMV_NEVER, start_at = FMV_NEVER, pause_at = FMV_NEVER, frame_at = FMV_NEVER;

/* ---- stream ---- */
static mpeg_demux_t demux;
static int64_t  next_dts, dts_q[INFO_Q];
static int      dts_n;

/* ---- decoder ---- */
static plm_buffer_t *es;                         /* elementary stream not decoded yet */
static plm_video_t  *dec;
static pinfo_t  info_q[INFO_Q];                  /* pictures in coded order, from the input */
static int      info_n;
static int      complete_pics;                   /* pictures whose end has arrived */
static int      pictures_in_input;
static int      needs_data;
static pinfo_t  held_info;
static int      reference_held;
/* scanner of the elementary stream as it arrives */
static uint32_t sc_shift;
static int      sc_kind, sc_pos, seq_seen, pic_open, pend_gop, pend_seq;
static uint8_t  sc_buf[4];
static uint32_t cur_timecode;
static uint8_t  cur_rate = 3;

/* ---- pictures ---- */
static picture_t fifo[MAX_PICTURES];
static int       fifo_n;
static picture_t display;
static int       display_valid;
static pinfo_t   display_info;
static uint16_t  dec_w, dec_h, dec_p90;
static uint32_t  dec_tc;
static uint8_t   dec_rate;
static uint32_t  period_30mhz = 1200000;
static int32_t   dts_desync;
static int       desync_ok, latch_vsync, latch_vblank, step_latch;

static uint32_t raw_dclk(void) { return (uint32_t)((fmv_now - dclk_origin) * DCLK_HZ / FMV_CPU_HZ); }
static uint32_t fmv_dclk(void) { return raw_dclk() + (uint32_t)dclk_offset; }

static void free_pic(picture_t *p) { free(p->rgb); p->rgb = NULL; }

static void decoder_new(void)
{
    if (dec) plm_video_destroy(dec);         /* also frees its buffer */
    es = plm_buffer_create_with_capacity(256 * 1024);
    dec = plm_video_create_with_buffer(es, 1);
    info_n = complete_pics = 0;
    reference_held = 0;
    sc_shift = 0; sc_kind = 0; seq_seen = pic_open = pend_gop = pend_seq = 0;
}

static void clear_fifo(void)
{
    decoder_new();
    needs_data = 0;
    for (int i = 0; i < fifo_n; i++) free_pic(&fifo[i]);
    fifo_n = 0;
    dts_n = 0; next_dts = 0;
    pictures_in_input = 0;
    latch_vsync = latch_vblank = step_latch = 0;
    desync_ok = 0; dts_desync = 0;
}

void fmvv_reset(void)
{
    isr = ier = ivec = syscmd = vidcmd = sysscr = dec_cmd = vdi_cmd = frame_rate = 0;
    tcnt = 55; stream_nr = 0; slow_motion = 0;
    dsp_enable = playback_active = 0;
    image_w = image_h = image_rt = 0;
    memset(vctrl, 0, sizeof(vctrl));
    l_disp_x = l_disp_y = l_win_x = l_win_y = l_win_w = l_win_h = 0;
    upd_latch = upd_scroll = show_video = 0;
    dclk_origin = fmv_now; dclk_offset = 0; syscr_written = 0;
    mpeg_demux_reset(&demux);
    clear_fifo();
    free_pic(&display); display_valid = 0;
    memset(&display_info, 0, sizeof(display_info));
    dec_w = dec_h = dec_p90 = 0; dec_tc = 0; dec_rate = 0;
    start_at = pause_at = frame_at = FMV_NEVER;
    /* TIM: 45000 / ((55 + 1) * 8) = 100.446 Hz, the host's SCR bookkeeping
     * depends on it */
    tim_next = fmv_now + fmv_ticks((uint64_t)(tcnt + 1) * 8, DCLK_HZ);
}

int fmvv_irq_pending(void) { return (isr & ier) != 0; }
uint8_t fmvv_vector(void) { return (uint8_t)(ivec >> 3); }
int fmvv_showing(void) { return show_video && display_valid; }

/* ======================= pictures ======================= */

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void queue_picture(plm_frame_t *f, const pinfo_t *info)
{
    if (fifo_n >= MAX_PICTURES || !f || !f->y.data) return;
    int w = (int)f->width, h = (int)f->height;
    picture_t *p = &fifo[fifo_n];
    p->rgb = malloc((size_t)w * h * 4);
    if (!p->rgb) return;
    p->info = *info;
    p->info.width = (uint16_t)w; p->info.height = (uint16_t)h;
    int lp = (int)f->y.width, cp = (int)f->cb.width;
    for (int y = 0; y < h; y++) {              /* ITU-R BT.601, studio range */
        const uint8_t *yl = f->y.data + (size_t)y * lp;
        const uint8_t *cbl = f->cb.data + (size_t)(y >> 1) * cp;
        const uint8_t *crl = f->cr.data + (size_t)(y >> 1) * cp;
        uint32_t *o = p->rgb + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int cr = crl[x >> 1] - 128, cb = cbl[x >> 1] - 128;
            int r = (cr * 104597) >> 16, g = (cb * 25674 + cr * 53278) >> 16, b = (cb * 132201) >> 16;
            int l = ((yl[x] - 16) * 76309) >> 16;
            o[x] = (uint32_t)clamp8(l + r) << 16 | (uint32_t)clamp8(l - g) << 8 | clamp8(l + b);
        }
    }
    fifo_n++;
}

static void update_irq_log(const char *why)
{
    if (fmv_log >= 2 && (isr & ~(IRQ_PIC | IRQ_TIM | IRQ_VSYNC)))
        fprintf(stderr, "[FMV] video ISR %04X (%s)\n", isr, why);
}

static void release_held(void)
{
    if (reference_held) {
        reference_held = 0;
        queue_picture(&dec->frame_backward, &held_info);    /* the newest reference */
    }
}

static void sequence_end(void)
{
    release_held();
    isr |= IRQ_ESI;
    update_irq_log("sequence end");
}

/* one picture through pl_mpeg; returns its type (1 I, 2 P, 3 B), 0 when the
 * data is not there yet, -1 for a picture the decoder skipped */
static int decode_one(void)
{
    plm_video_t *v = dec;
    if (complete_pics <= 0 || !plm_video_has_header(v)) return 0;
    if (v->start_code != PLM_START_PICTURE) {
        v->start_code = plm_buffer_find_start_code(v->buffer, PLM_START_PICTURE);
        if (v->start_code == -1) { complete_pics = 0; return 0; }
    }
    plm_buffer_discard_read_bytes(v->buffer);
    v->picture_type = 0;
    plm_video_decode_picture(v);
    complete_pics--;
    if (pictures_in_input) pictures_in_input--;   /* consumed, shown or not */
    return (v->picture_type >= 1 && v->picture_type <= 3) ? v->picture_type : -1;
}

static void decode_pending(void)
{
    if (!dsp_enable) return;
    needs_data = 0;
    while (fifo_n < MAX_PICTURES) {
        int t = decode_one();
        if (t == 0) { needs_data = 1; break; }
        pinfo_t info;
        memset(&info, 0, sizeof(info));
        if (info_n) { info = info_q[0]; memmove(info_q, info_q + 1, (size_t)--info_n * sizeof(pinfo_t)); }
        if (t < 0) { if (info.ends_seq) sequence_end(); continue; }
        info.width = (uint16_t)dec->width; info.height = (uint16_t)dec->height;
        dec_w = info.width; dec_h = info.height; dec_tc = info.timecode;
        dec_rate = info.rate & 15; dec_p90 = PERIOD_90KHZ[dec_rate];
        period_30mhz = PERIOD_30MHZ[dec_rate];
        if (playback_active) { image_w = dec_w; image_h = dec_h; image_rt = dec_rate; }
        static unsigned long ndec = 0;
        if (fmv_log >= 3 || (fmv_log && ++ndec % 50 == 1))
            fprintf(stderr, "[FMV] decoded picture %lu at %.2f s: %dx%d type %d tref %d, %d queued\n", ndec,
                    (double)fmv_now / FMV_CPU_HZ, dec_w, dec_h, t, info.video_status & 0x3f, fifo_n);
        if (t == 3) queue_picture(&dec->frame_current, &info);       /* B: shown now */
        else {
            if (reference_held) queue_picture(&dec->frame_forward, &held_info);
            held_info = info;
            reference_held = 1;
        }
        if (info.ends_seq) sequence_end();
    }
}

/* the elementary stream as it enters the chip: counts pictures and reads
 * the headers the status registers report */
static void scan_es(uint8_t b)
{
    if (sc_kind) {
        sc_buf[sc_pos++] = b;
        int need = sc_kind == 0x00 ? 2 : 4;
        if (sc_pos < need) return;
        if (sc_kind == 0xB3) { cur_rate = sc_buf[3] & 15; }
        else if (sc_kind == 0xB8) {
            uint32_t v = (uint32_t)sc_buf[0] << 24 | sc_buf[1] << 16 | sc_buf[2] << 8 | sc_buf[3];
            uint32_t g = v >> 7;                 /* 25-bit time code */
            uint32_t fr = g & 0x3f, s = (g >> 6) & 0x3f, m = (g >> 13) & 0x3f, h = (g >> 19) & 0x1f;
            cur_timecode = (h << 6) | m | (s << 22) | (fr << 16);
        } else if (sc_kind == 0x00 && info_n < INFO_Q) {
            pinfo_t *p = &info_q[info_n++];
            memset(p, 0, sizeof(*p));
            int tref = (sc_buf[0] << 2) | (sc_buf[1] >> 6), type = (sc_buf[1] >> 3) & 7;
            p->video_status = (uint8_t)(((type & 3) << 6) | (tref & 0x3f));
            p->timecode = cur_timecode;
            p->rate = cur_rate;
            if (type == 1) { p->first_of_gop = (uint8_t)pend_gop; p->first_of_seq = (uint8_t)pend_seq; pend_gop = pend_seq = 0; }
        }
        sc_kind = 0;
        return;
    }
    sc_shift = (sc_shift << 8) | b;
    if ((sc_shift & 0xFFFFFF00u) != 0x00000100u) return;
    uint8_t code = (uint8_t)sc_shift;
    if (code >= 0x01 && code <= 0xAF) return;    /* slices */
    if (code == 0xB3) { seq_seen = 1; pend_seq = pend_gop = 1; }
    if (code == 0xB8) pend_gop = 1;
    if (!seq_seen) return;
    if (pic_open) { complete_pics++; pic_open = 0; }
    if (code == 0x00) {
        pic_open = 1;
        pictures_in_input++;
        if (dts_n < INFO_Q) dts_q[dts_n++] = next_dts;
        next_dts = 0;
        sc_kind = 0x00; sc_pos = 0;
    } else if (code == 0xB3 || code == 0xB8) { sc_kind = code; sc_pos = 0; }
    else if (code == 0xB7) {
        if (info_n) info_q[info_n - 1].ends_seq = 1;
        else sequence_end();
    }
    sc_shift = 0;
}

void fmvv_data(uint8_t b)
{
    int body = demux.packet_body;
    mpeg_demux_byte(&demux, b, (uint8_t)(0xE0 | stream_nr));
    if (body) {
        uint8_t c = b;
        if (plm_buffer_get_remaining(es) < 2 * 1024 * 1024) plm_buffer_write(es, &c, 1);
        scan_es(b);
    }
    if (demux.dts_updated) { next_dts = demux.dts; vdi_cmd |= 1 << 14; }
    if (demux.program_end) {
        /* end of the program: the reference held for reordering is shown too */
        if (pic_open) { complete_pics++; pic_open = 0; }
        decode_pending();
        release_held();
        isr |= IRQ_EII;
        if (fmv_log)
            fprintf(stderr, "[FMV] video program end at %.2f s: %d queued, %d in input, %d complete, %u bytes in ES, ier %04X\n",
                    (double)fmv_now / FMV_CPU_HZ, fifo_n, pictures_in_input, complete_pics,
                    (unsigned)plm_buffer_get_remaining(es), ier);
        update_irq_log("program end");
    }
}

void fmvv_end_of_transfer(void) { decode_pending(); }

/* ======================= display pacing ======================= */

static void restart_frame_timer(void)
{
    if (!playback_active) { frame_at = FMV_NEVER; return; }
    int64_t period = ((int64_t)period_30mhz - 1 - (int64_t)dts_desync * 2048) & 0xffffff;
    if (slow_motion > 1) period *= slow_motion;
    if (period < 1200) period = period_30mhz;
    frame_at = fmv_now + fmv_ticks((uint64_t)period, FRAME_CLOCK_HZ);
}

static void frame_tick(void)
{
    if (!playback_active) return;
    int sync = (sysscr >> 2) & 1;
    if (dts_n) {
        int32_t d = (int32_t)((fmv_dclk() >> 6) & 0x7fff) - (int32_t)((dts_q[0] >> 7) & 0x7fff);
        if (d >= 0x4000) d -= 0x8000; else if (d < -0x4000) d += 0x8000;
        if (!sync || d > -30) desync_ok = 1;
        int valid = dts_q[0] != 0;
        int way_out = valid && mpeg_ts_diff(demux.scr, dts_q[0]) > 15000;
        dts_desync = (syscr_written && sync && valid && !way_out) ? d : 0;
    } else { desync_ok = 1; dts_desync = 0; }
    if (!sync || desync_ok) latch_vsync = 1;
    restart_frame_timer();
}

static void latch_display(void)
{
    if (!fifo_n) return;
    free_pic(&display);
    display = fifo[0];
    memmove(fifo, fifo + 1, (size_t)--fifo_n * sizeof(picture_t));
    fifo[fifo_n].rgb = NULL;
    display_valid = 1;
    if (dts_n) memmove(dts_q, dts_q + 1, (size_t)--dts_n * sizeof(int64_t));
    display_info = display.info;
}

static void apply_register_update(void)
{
    isr |= IRQ_VCUP | IRQ_DCL;
    upd_latch = 0;
    l_disp_y = vctrl[4]; l_disp_x = vctrl[5];
    l_win_h = vctrl[6];  l_win_w = vctrl[7];
    l_win_y = vctrl[8];  l_win_x = vctrl[9];
}

/* state 1: vsync (start of vertical blanking), 0: picture starts display */
void fmvv_vblank(int state)
{
    if (state) {
        static int dbg_n = 0;
        if (fmv_log >= 2 && fifo_n && ++dbg_n % 50 == 0)
            fprintf(stderr, "[FMV] vsync state: %d queued, active %d, dsp %d, latch %d, frame_at %s, dts_n %d, desync_ok %d, syscr %04X\n",
                    fifo_n, playback_active, dsp_enable, latch_vsync, frame_at == FMV_NEVER ? "never" : "set",
                    dts_n, desync_ok, sysscr);
        if (latch_vsync) {
            latch_vsync = 0;
            if (upd_latch && !upd_scroll) apply_register_update();
            if (dsp_enable && playback_active && !fifo_n) release_held();   /* ran dry: the held reference is next */
            if (fifo_n) {
                if (playback_active && fifo_n == 1 && pictures_in_input == 0 && !reference_held) isr |= IRQ_NDAT;
                latch_display();
                latch_vblank = 1;
            }
        }
        if (step_latch && fifo_n) { step_latch = 0; latch_display(); latch_vblank = 1; }
        isr |= IRQ_VSYNC;
        if (upd_latch && upd_scroll) apply_register_update();
        update_irq_log("vsync");
    } else {
        if (latch_vblank) {
            latch_vblank = 0;
            isr |= IRQ_PIC;
            if (display_info.first_of_gop) isr |= IRQ_GOP;
            if (display_info.first_of_seq) isr |= IRQ_SEQ;
            if (!fifo_n && pictures_in_input == 0 && !reference_held) isr |= IRQ_EOD;
            update_irq_log("picture");
        }
        decode_pending();                       /* keep the decoder fed */
        static int dbg2 = 0;
        if (fmv_log >= 2 && !fifo_n && (pictures_in_input || reference_held) && ++dbg2 % 50 == 1)
            fprintf(stderr, "[FMV] dry: %d in input, %d complete, held %d, dsp %d, header %d, start %d, %u bytes in ES\n",
                    pictures_in_input, complete_pics, reference_held, dsp_enable, plm_video_has_header(dec),
                    dec->start_code, (unsigned)plm_buffer_get_remaining(es));
    }
}

void fmvv_tick(void)
{
    while (fmv_now >= tim_next) {
        isr |= IRQ_TIM;
        fmva_poll();
        tim_next += fmv_ticks((uint64_t)(tcnt + 1) * 8, DCLK_HZ);
    }
    if (fmv_now >= start_at) { start_at = FMV_NEVER; playback_active = 1; restart_frame_timer(); }
    if (fmv_now >= pause_at) { pause_at = FMV_NEVER; isr |= IRQ_PAI; }
    if (fmv_now >= frame_at) { frame_at = FMV_NEVER; frame_tick(); }
}

int fmvv_pixel(int x, int y, uint32_t *rgb)
{
    if (!show_video || !display_valid || !display.rgb) return 0;
    int sx = x - (int)l_disp_x, sy = y - (int)l_disp_y;
    if (sx < 0 || sy < 0) return 0;
    if (l_win_w && sx >= (int)l_win_w) return 0;
    if (l_win_h && sy >= (int)l_win_h) return 0;
    int px = sx + (int)l_win_x, py = sy + (int)l_win_y;
    if (px >= (int)display.info.width || py >= (int)display.info.height) return 0;
    *rgb = display.rgb[(size_t)py * display.info.width + px];
    return 1;
}

/* ======================= registers ======================= */

uint16_t fmvv_read(int reg)
{
    uint16_t d = 0;
    switch (reg) {
    case 0x01: d = image_w; break;
    case 0x02: d = image_h; break;
    case 0x03: d = image_rt; break;
    case 0x04: d = (uint16_t)(dec_tc >> 16); break;
    case 0x05: d = (uint16_t)dec_tc; break;
    case 0x29: d = display_info.width; break;
    case 0x2a: d = display_info.height; break;
    case 0x2b: d = display_info.rate & 15; break;
    case 0x2c: d = (uint16_t)(display_info.timecode >> 16); break;
    case 0x2d: d = (uint16_t)display_info.timecode; break;
    case 0x2e: d = display_info.video_status; break;
    case 0x2f:  /* SYS_STS bit 13: request for bits, follows the input FIFO */
        d = (plm_buffer_get_remaining(es) < VIDEO_FIFO_FULL || needs_data) ? 0x2000 : 0;
        break;
    case 0x30: d = ier; break;
    case 0x31: d = isr; isr = 0; break;          /* read clears */
    case 0x32: d = tcnt; break;
    case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:
    case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: d = vctrl[reg - 0x36]; break;
    case 0x44: d = dec_cmd; break;
    case 0x46: d = vdi_cmd; break;
    case 0x4c: d = (uint16_t)((fmv_dclk() >> 6) & 0x7fff); break;   /* GEN_SYSCR */
    case 0x4f: d = 1; break;                                       /* GEN_DEC_DELAY */
    case 0x50: d = (uint16_t)((demux.dts >> 7) & 0x7fff); break;  /* GEN_DEC_TIM1 */
    case 0x52: d = playback_active ? (uint16_t)(fifo_n + reference_held + pictures_in_input) : (uint16_t)dts_n; break;
    case 0x54: d = dec_p90; break;
    case 0x55: d = DISPLAY_RATE_PAL; break;
    case 0x56: d = frame_rate; break;
    case 0x60: d = syscmd; break;
    case 0x61: d = vidcmd; break;
    case 0x62: d = stream_nr; break;
    case 0x6e: d = ivec; break;
    default: break;
    }
    if (fmv_log >= 2 && reg != 0x2f && reg != 0x31 && reg != 0x4c)
        fprintf(stderr, "[FMV] V R %02X -> %04X\n", reg * 2, d);
    return d;
}

void fmvv_write(int reg, uint16_t v)
{
    if (fmv_log) fprintf(stderr, "[FMV] V W %02X = %04X\n", reg * 2, v);
    switch (reg) {
    case 0x30: ier = v; break;
    case 0x32: tcnt = v; tim_next = fmv_now + fmv_ticks((uint64_t)(v + 1) * 8, DCLK_HZ); break;
    case 0x57: tim_next = fmv_now + fmv_ticks((uint64_t)(tcnt + 1) * 8, DCLK_HZ); break;
    case 0x60:
        syscmd = v;
        if (v & SYS_PLAY) {
            start_at = fmv_now + fmv_ticks(PLAY_DELAY_TICKS, DCLK_HZ);
            image_w = dec_w; image_h = dec_h; image_rt = dec_rate;
            dec_cmd |= (1 << 6) | (1 << 1);
            slow_motion = v & 7;
        }
        if (v & SYS_PAUSE) {
            playback_active = 0; start_at = frame_at = FMV_NEVER;
            pause_at = fmv_now + fmv_ticks(PAUSE_DELAY_TICKS, DCLK_HZ);
        }
        if (v & SYS_CONTINUE) { start_at = fmv_now + fmv_ticks(PLAY_DELAY_TICKS, DCLK_HZ); slow_motion = v & 7; }
        if (v & SYS_STEP) { step_latch = 1; start_at = FMV_NEVER; }
        if (v & SYS_STOP) { playback_active = 0; start_at = frame_at = FMV_NEVER; }
        if (v & SYS_CLEARFIFO) {
            playback_active = 0; start_at = frame_at = FMV_NEVER;
            clear_fifo();
            mpeg_demux_reset(&demux);
            dsp_enable = 1;
        }
        if (v & SYS_DEC_ON) dsp_enable = 1;
        if (v & SYS_DEC_OFF) {
            dsp_enable = playback_active = 0; start_at = frame_at = FMV_NEVER;
            image_w = image_h = image_rt = 0;
            display_info.video_status = 0; display_info.timecode = 0;
            dec_tc = 0; dec_p90 = 0; dec_rate = 0;
            display_valid = 0;
            clear_fifo();
        }
        if (v & SYS_DMA) fmv_board_drq(0);
        break;
    case 0x61:
        vidcmd = v;
        if (v & VID_REGSUPD) { upd_latch = 1; upd_scroll = (v & VID_SCROLL) != 0; }
        if (v & VID_HIDE) show_video = 0;
        if (v & (VID_SHOW | VID_SHOWNEXT)) show_video = 1;
        break;
    case 0x63: sysscr = v; break;
    case 0x6e: ivec = v; break;
    case 0x36: case 0x37: case 0x38: case 0x39: case 0x3a:
    case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f: vctrl[reg - 0x36] = v; break;
    case 0x44:
        dec_cmd = v;
        if ((v >> 8) == 0x22 && !(syscmd & SYS_PAUSE)) step_latch = 1;
        break;
    case 0x46: vdi_cmd = v; break;
    case 0x4c:
        dclk_offset = (int32_t)((((uint32_t)v & 0x7fff) << 6) - (raw_dclk() & 0x001fffc0));
        syscr_written = 1;
        break;
    case 0x56: frame_rate = v; break;
    case 0x01: image_w = v; break;
    case 0x02: image_h = v; break;
    case 0x03: image_rt = v; break;
    case 0x62: stream_nr = v & 0x0f; break;
    default: break;
    }
}
