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
static scc_frame_t  *frame  = NULL;
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
    frame  = calloc(1, sizeof(*frame));
    if (!tex || !pixels || !frame) {
        fprintf(stderr, "[video_display] texture/buffer allocation failed: %s\n", SDL_GetError());
        free(pixels); free(frame); pixels = NULL; frame = NULL;
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

/* Log display state changes (EMU_VIDEO_LOG=1) */
static void log_frame_changes(const scc66470_t *v, const scc_frame_t *f)
{
    static uint16_t last_dcr = 0xFFFF, last_dcr2 = 0xFFFF;
    static uint32_t last_vsr = 0xFFFFFFFFu;
    static int      last_bep_count = -1;
    static uint32_t last_bep[SCC_BEP_MAX];

    if (!video_log_on()) return;

    if (v->dcr != last_dcr || v->dcr2 != last_dcr2 || f->vsr_start != last_vsr) {
        fprintf(stderr, "[VIDEO] DE=%d DCR=%04X DCR2=%04X CSR=%04X ICA=%d insns "
                        "-> start=%05X %dx%d\n",
                f->enabled, v->dcr, v->dcr2, v->csr_control, f->ica_count,
                f->vsr_start, f->width, f->height);
        last_dcr = v->dcr; last_dcr2 = v->dcr2; last_vsr = f->vsr_start;
    }
    if (f->bep_count != last_bep_count ||
        memcmp(last_bep, f->bep, (size_t)f->bep_count * sizeof(uint32_t)) != 0) {
        fprintf(stderr, "[VIDEO] BEP CONTROL x%d:", f->bep_count);
        for (int i = 0; i < f->bep_count; i++) fprintf(stderr, " %08X", f->bep[i]);
        fprintf(stderr, "\n");
        last_bep_count = f->bep_count;
        memcpy(last_bep, f->bep, (size_t)f->bep_count * sizeof(uint32_t));
    }
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

    FILE *fp = fopen(path, "wb");
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
    if (video_log_on()) fprintf(stderr, "[VIDEO] frame saved to %s\n", path);
}

int video_display_update(void *scc66470, void *bus)
{
    if (!initialized) return 1;
    if (g_video_window_closed) return 0;

    scc66470_t *v = (scc66470_t *)scc66470;
    bus_t *b = (bus_t *)bus;

    scc66470_render(v, b->dram, (uint32_t)sizeof(b->dram), frame);
    log_frame_changes(v, frame);

    /* Back-end processor: apply this frame's BEP CONTROL words in order */
    for (int i = 0; i < frame->bep_count; i++)
        vsd_command(&vsd, frame->bep[i]);

    int w = frame->width, h = frame->height;
    if (!frame->enabled) {
        /* DE = 0: the VSC drives no picture */
        memset(pixels, 0, (size_t)SCC_OUT_MAX_W * SCC_OUT_MAX_H * sizeof(uint32_t));
    } else {
        for (int y = 0; y < h; y++) {
            const uint8_t *src = &frame->pix[y * SCC_OUT_MAX_W];
            uint32_t *dst = &pixels[y * SCC_OUT_MAX_W];
            for (int x = 0; x < w; x++) dst[x] = vsd_pixel_a(&vsd, src[x]);
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
