/*
 * video_display.c - CD-i video output display window
 *
 * The SCC66470 model (scc66470_render) produces, for each frame, the 8-bit
 * values the VSC drives on its V(7:0) video port: it runs the ICA/DCA
 * control programs and decodes bit-map, run-length and mosaic files in
 * 4 or 8 bits/pixel. This file only turns those values into colours and
 * presents them in an SDL2 window.
 *
 * Colours: the V(7:0) port feeds the V.S.D. back-end processor (vsd.c),
 * which receives the BEP CONTROL words emitted by the ICA/DCA programs
 * (CLUT loads, coding method, transparency, cursor) and maps each pixel
 * value to RGB. With EMU_VIDEO_LOG=1 the BEP words are logged.
 */

#include "video_display.h"
#include "scc66470.h"
#include "vsd.h"
#include "../bus.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W   SCC_OUT_MAX_W          /* 768: one pixel per 4-bit pixel   */
#define WIN_H   (SCC_OUT_MAX_H * 2)    /* 560: lines doubled for the aspect */

int g_video_window_closed = 0;

static SDL_Window   *win = NULL;
static SDL_Renderer *ren = NULL;
static SDL_Texture  *tex = NULL;
static uint32_t     *pixels = NULL;    /* ARGB8888, SCC_OUT_MAX_W x SCC_OUT_MAX_H */
static scc_frame_t  *frame  = NULL;   /* plane A: master VSC */
static scc_frame_t  *frame_b = NULL;  /* plane B: slave VSC */
static vsd_t         vsd;
static int           initialized = 0;
static uint32_t      window_id = 0;

static int video_log_on(void)
{
    static int state = -1;
    if (state < 0) {
        const char *s = getenv("EMU_VIDEO_LOG");
        state = (s && *s && strcmp(s, "0") != 0) ? 1 : 0;
    }
    return state;
}

void video_display_init(void)
{
    if (initialized) return;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[video_display] SDL_Init error: %s\n", SDL_GetError());
        return;
    }

    win = SDL_CreateWindow("CD-i 205 - Video Output",
                           SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                           WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "[video_display] SDL_CreateWindow error: %s\n", SDL_GetError());
        return;
    }

    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        fprintf(stderr, "[video_display] SDL_CreateRenderer error: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); win = NULL;
        return;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                            SCC_OUT_MAX_W, SCC_OUT_MAX_H);
    pixels = calloc((size_t)SCC_OUT_MAX_W * SCC_OUT_MAX_H, sizeof(uint32_t));
    frame   = calloc(1, sizeof(*frame));
    frame_b = calloc(1, sizeof(*frame_b));
    if (!tex || !pixels || !frame || !frame_b) {
        fprintf(stderr, "[video_display] texture/buffer allocation failed: %s\n", SDL_GetError());
        free(pixels); free(frame); free(frame_b); pixels = NULL; frame = NULL; frame_b = NULL;
        if (tex) SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
        tex = NULL; ren = NULL; win = NULL;
        return;
    }

    vsd_reset(&vsd);
    window_id = SDL_GetWindowID(win);
    initialized = 1;
    fprintf(stderr, "[video_display] Window created (ID=%u, %dx%d)\n", window_id, WIN_W, WIN_H);
}

uint32_t video_display_get_window_id(void)
{
    return window_id;
}

/* ===== Pointer input ===== */

static video_pointer_cb pointer_cb = NULL;
static void            *pointer_ctx = NULL;
static int ptr_x = 384, ptr_y = 240, ptr_buttons = 0;
static int frame_w = SCC_OUT_MAX_W, frame_h = 240;   /* last displayed geometry */

void video_display_set_pointer_cb(video_pointer_cb cb, void *ctx)
{
    pointer_cb = cb;
    pointer_ctx = ctx;
}

static void pointer_report(void)
{
    if (ptr_x < 0) ptr_x = 0;
    if (ptr_y < 0) ptr_y = 0;
    if (ptr_x > 1023) ptr_x = 1023;
    if (ptr_y > 1023) ptr_y = 1023;
    if (pointer_cb) pointer_cb(pointer_ctx, ptr_x, ptr_y, ptr_buttons);
}

/* Window pixel -> pointer units, as measured against the shell's cursor:
 * X in 4-bit-pixel columns (0..width-1), Y in half display lines
 * (0..2*lines-1, the shell halves it). */
static void window_to_pointer(int wx, int wy)
{
    int ww = WIN_W, wh = WIN_H;
    if (win) SDL_GetWindowSize(win, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    ptr_x = wx * frame_w / ww;
    ptr_y = wy * frame_h * 2 / wh;
}

int video_display_handle_event(const void *ev)
{
    const SDL_Event *e = (const SDL_Event *)ev;
    if (!initialized) return 0;

    switch (e->type) {
    case SDL_MOUSEMOTION:
        if (e->motion.windowID != window_id) return 0;
        window_to_pointer(e->motion.x, e->motion.y);
        pointer_report();
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e->button.windowID != window_id) return 0;
        int bit = (e->button.button == SDL_BUTTON_LEFT)  ? 1 :
                  (e->button.button == SDL_BUTTON_RIGHT) ? 2 : 0;
        if (!bit) return 1;
        window_to_pointer(e->button.x, e->button.y);
        if (e->type == SDL_MOUSEBUTTONDOWN) ptr_buttons |= bit; else ptr_buttons &= ~bit;
        pointer_report();
        return 1;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (e->key.windowID != window_id) return 0;
        int down = (e->type == SDL_KEYDOWN);
        /* Arrows: one impulse per key press / auto-repeat (relative move,
         * as a CD-i thumbstick): EMU_IMPULSE units, x3 with Shift */
        static int impulse = 0;
        if (!impulse) { const char *e2 = getenv("EMU_IMPULSE"); impulse = (e2 && atoi(e2) > 0) ? atoi(e2) : 24; }
        int step = (SDL_GetModState() & KMOD_SHIFT) ? impulse * 3 : impulse;
        int dx = 0, dy = 0;
        switch (e->key.keysym.sym) {
        case SDLK_LEFT:  dx = -step; break;
        case SDLK_RIGHT: dx =  step; break;
        case SDLK_UP:    dy = -step; break;
        case SDLK_DOWN:  dy =  step; break;
        case SDLK_SPACE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (down) ptr_buttons |= 1; else ptr_buttons &= ~1;
            break;
        case SDLK_BACKSPACE:
            if (down) ptr_buttons |= 2; else ptr_buttons &= ~2;
            break;
        default:
            return 1;
        }
        if (dx || dy) {
            if (down && pointer_cb) pointer_cb(pointer_ctx, dx, dy, ptr_buttons | VIDEO_PTR_IMPULSE);
            return 1;
        }
        pointer_report();
        return 1;
    }
    case SDL_TEXTINPUT:
        /* typed text belongs to the UART terminal, not the video window */
        return e->text.windowID == window_id;
    }
    return 0;
}

/* Log display state changes (EMU_VIDEO_LOG=1) */
typedef struct {
    uint16_t dcr, dcr2;
    uint32_t vsr;
    int      bep_count;
    uint32_t bep[SCC_BEP_MAX];
} log_state_t;

static void log_frame_changes(char plane, const scc66470_t *v, const scc_frame_t *f)
{
    static log_state_t st[2] = { { 0xFFFF, 0xFFFF, 0xFFFFFFFFu, -1, {0} },
                                 { 0xFFFF, 0xFFFF, 0xFFFFFFFFu, -1, {0} } };
    log_state_t *l = &st[plane == 'B'];
    #define last_dcr       l->dcr
    #define last_dcr2      l->dcr2
    #define last_vsr       l->vsr
    #define last_bep_count l->bep_count
    #define last_bep       l->bep

    if (!video_log_on()) return;

    if (v->dcr != last_dcr || v->dcr2 != last_dcr2 || f->vsr_start != last_vsr) {
        fprintf(stderr, "[VIDEO %c] DE=%d DCR=%04X DCR2=%04X CSR=%04X ICA=%d insns "
                        "-> start=%05X %dx%d\n",
                plane, f->enabled, v->dcr, v->dcr2, v->csr_control, f->ica_count,
                f->vsr_start, f->width, f->height);
        last_dcr = v->dcr; last_dcr2 = v->dcr2; last_vsr = f->vsr_start;
    }
    if (f->bep_count != last_bep_count ||
        memcmp(last_bep, f->bep, (size_t)f->bep_count * sizeof(uint32_t)) != 0) {
        fprintf(stderr, "[VIDEO %c] BEP CONTROL x%d:", plane, f->bep_count);
        for (int i = 0; i < f->bep_count; i++) fprintf(stderr, " %08X", f->bep[i]);
        fprintf(stderr, "\n");
        last_bep_count = f->bep_count;
        memcpy(last_bep, f->bep, (size_t)f->bep_count * sizeof(uint32_t));
    }
    #undef last_dcr
    #undef last_dcr2
    #undef last_vsr
    #undef last_bep_count
    #undef last_bep
}

/* EMU_VIDEO_DUMP=<file.bmp>: save the displayed frame whenever it changes */
static void dump_frame_if_changed(int w, int h)
{
    static const char *path = NULL;
    static int checked = 0;
    static uint32_t last_hash = 0;
    if (!checked) { path = getenv("EMU_VIDEO_DUMP"); checked = 1; }
    if (!path || !*path) return;

    uint32_t hash = 2166136261u ^ (uint32_t)(w * 7919 + h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            hash = (hash ^ pixels[y * SCC_OUT_MAX_W + x]) * 16777619u;
    if (hash == last_hash) return;
    last_hash = hash;

    /* a "%d" in the name numbers the frames (f%04d.bmp -> f0000.bmp, ...) */
    static int frame_no = 0;
    char name[600];
    if (strstr(path, "%")) snprintf(name, sizeof(name), path, frame_no++);
    else                   snprintf(name, sizeof(name), "%s", path);
    FILE *fp = fopen(name, "wb");
    if (!fp) return;
    int rowb = w * 3, pad = (4 - rowb % 4) % 4;
    uint32_t size = 54u + (uint32_t)(rowb + pad) * (uint32_t)h;
    uint8_t hd[54] = { 'B', 'M' };
    #define PUT32(o, v) do { uint32_t _v = (uint32_t)(v); hd[o] = _v; hd[o+1] = _v >> 8; hd[o+2] = _v >> 16; hd[o+3] = _v >> 24; } while (0)
    PUT32(2, size); PUT32(10, 54); PUT32(14, 40); PUT32(18, w); PUT32(22, -h);
    hd[26] = 1; hd[28] = 24;
    #undef PUT32
    fwrite(hd, 1, sizeof(hd), fp);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t c = pixels[y * SCC_OUT_MAX_W + x];
            uint8_t px[3] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16) };
            fwrite(px, 1, 3, fp);
        }
        fwrite("\0\0\0", 1, (size_t)pad, fp);
    }
    fclose(fp);
    if (video_log_on()) fprintf(stderr, "[VIDEO] frame saved to %s\n", name);
}

int video_display_update(void *scc66470, void *bus)
{
    if (!initialized) return 1;
    if (g_video_window_closed) return 0;

    scc66470_t *v = (scc66470_t *)scc66470;
    bus_t *b = (bus_t *)bus;

    scc66470_t *vb = bus_get_video_slave(b);

    /* Both VSCs share the 1 MB of DRAM (bank 1 / bank 2) and the timing */
    scc66470_render(v,  b->dram, (uint32_t)sizeof(b->dram), frame);
    scc66470_render(vb, b->dram, (uint32_t)sizeof(b->dram), frame_b);
    log_frame_changes('A', v,  frame);
    log_frame_changes('B', vb, frame_b);
    /* EMU_VIDEO_LOG=2: also dump the ICA programs (non-BEP words) when the
     * display start changes */
    {
        static uint32_t last_a = 0xFFFFFFFFu, last_b = 0xFFFFFFFFu;
        const char *e = getenv("EMU_VIDEO_LOG");
        if (e && atoi(e) >= 2) {
            for (int pl = 0; pl < 2; pl++) {
                const scc_frame_t *f = pl ? frame_b : frame;
                uint32_t *last = pl ? &last_b : &last_a;
                if (f->vsr_start == *last) continue;
                *last = f->vsr_start;
                uint32_t ica = pl ? 0x80400u : 0x400u;
                fprintf(stderr, "[VIDEO %c] ICA @%05X:", pl ? 'B' : 'A', ica);
                uint32_t ptr = ica;
                for (int k = 0, n = 0; k < 400 && n < 48; k++) {
                    const uint8_t *q = &b->dram[ptr & 0xFFFFCu];
                    uint32_t w = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
                    ptr += 4;
                    if (w & 0x80000000u) continue;          /* skip BEP words */
                    fprintf(stderr, " %08X", w);
                    n++;
                    if ((w >> 28) == 4) { ptr = w & 0xFFFFFu; fprintf(stderr, "->"); }   /* fetch follows VSR */
                    if ((w >> 28) == 0 || (w >> 28) == 3 || (w >> 28) == 5) break;   /* STOP */
                }
                fprintf(stderr, "\n");
            }
        }
    }

    /* Back-end processor: apply this frame's BEP CONTROL words in order
     * (both VSCs drive the V.S.D. command port) */
    for (int i = 0; i < frame->bep_count; i++)
        vsd_command(&vsd, frame->bep[i]);
    for (int i = 0; i < frame_b->bep_count; i++)
        vsd_command(&vsd, frame_b->bep[i]);

    int w = frame->width, h = frame->height;
    frame_w = w; frame_h = h;
    if (!frame->enabled && !frame_b->enabled) {
        /* DE = 0 on both: no picture */
        memset(pixels, 0, (size_t)SCC_OUT_MAX_W * SCC_OUT_MAX_H * sizeof(uint32_t));
    } else {
        int wb = frame_b->width, hb = frame_b->height;
        int dyuv_a = vsd_plane_dyuv(&vsd, 0), dyuv_b = vsd_plane_dyuv(&vsd, 1);
        static uint32_t line_a[SCC_OUT_MAX_W], line_b[SCC_OUT_MAX_W];
        for (int y = 0; y < h; y++) {
            const uint8_t *src_a = &frame->pix[y * SCC_OUT_MAX_W];
            const uint8_t *src_b = &frame_b->pix[y * SCC_OUT_MAX_W];
            uint32_t *dst = &pixels[y * SCC_OUT_MAX_W];
            /* DYUV planes are decoded a whole line at a time (delta coding) */
            if (dyuv_a) vsd_dyuv_line(&vsd, 0, src_a, w, line_a);
            if (dyuv_b && y < hb) vsd_dyuv_line(&vsd, 1, src_b, wb < w ? wb : w, line_b);
            for (int x = 0; x < w; x++) {
                int b_on = frame_b->enabled && x < wb && y < hb;
                dst[x] = vsd_mix_rgb(&vsd, src_a[x], frame->enabled, dyuv_a ? &line_a[x] : NULL,
                                     b_on ? src_b[x] : 0, b_on, (dyuv_b && b_on) ? &line_b[x] : NULL);
            }
        }
        vsd_draw_cursor(&vsd, pixels, w, h, SCC_OUT_MAX_W);
    }

    dump_frame_if_changed(w, h);

    SDL_Rect src_rect = { 0, 0, w, h };
    SDL_UpdateTexture(tex, NULL, pixels, SCC_OUT_MAX_W * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, &src_rect, NULL);
    SDL_RenderPresent(ren);
    return 1;
}
