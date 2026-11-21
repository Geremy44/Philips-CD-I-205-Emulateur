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
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#include <commdlg.h>
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The picture is drawn on a 4:3 logical surface; SDL letterboxes it in any
 * window size or in full screen, and converts mouse coordinates to it. */
#define WIN_W   768
#define WIN_H   576

int g_video_window_closed = 0;
static void create_menu(void);

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
                           WIN_W, WIN_H,
                           (getenv("EMU_HEADLESS") ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN) | SDL_WINDOW_RESIZABLE);   /* EMU_HEADLESS=1: tests */
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
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);       /* 4:3, black borders */

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
    create_menu();
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

/* Logical (4:3) coordinates -> pointer units, measured against the CD-i
 * shell cursor (pointer driver scaling, $1B081A): the cursor column equals
 * the pointer X (4-bit-pixel columns of the frame), and the cursor line is
 * pointer Y * 240 / 560 -- the Y range 0..559 spans the whole picture. So
 * the CD-i cursor's top-left corner lands under the PC mouse. */
static void window_to_pointer(int lx, int ly)
{
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= WIN_W) lx = WIN_W - 1;
    if (ly >= WIN_H) ly = WIN_H - 1;
    ptr_x = lx * frame_w / WIN_W;
    ptr_y = ly * 560 / WIN_H;
}

/* ===== Menu bar (Windows: native menu on the video window) ===== */
static video_menu_cb menu_cb = NULL;
#ifdef _WIN32
static HMENU menu_bar = NULL;
#endif

void video_display_set_menu_cb(video_menu_cb cb) { menu_cb = cb; }

void video_display_menu_check(int id, int on)
{
#ifdef _WIN32
    if (menu_bar) CheckMenuItem(menu_bar, (UINT)id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
#else
    (void)id; (void)on;
#endif
}

static void create_menu(void)
{
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
    HWND hwnd = info.info.win.window;
    HMENU file = CreatePopupMenu(), view = CreatePopupMenu(), ctl = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, VM_OPEN_DISC, L"Ouvrir un disque...\tCtrl+O");
    AppendMenuW(file, MF_STRING, VM_EJECT,     L"\u00c9jecter le disque");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, VM_SAVE,      L"Sauvegarder l'\u00e9tat\tF5");
    AppendMenuW(file, MF_STRING, VM_LOAD,      L"Rappeler l'\u00e9tat\tF9");
    AppendMenuW(file, MF_STRING, VM_NEXT_SLOT, L"Slot suivant\tF6");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, VM_QUIT,      L"Quitter");
    AppendMenuW(view, MF_STRING, VM_FULLSCREEN, L"Plein \u00e9cran\tF11");
    AppendMenuW(view, MF_STRING, VM_HOST_CURSOR, L"Souris du PC visible\tF8");
    AppendMenuW(view, MF_STRING, VM_CONSOLE,   L"Console s\u00e9rie (UART)");
    AppendMenuW(ctl,  MF_STRING, VM_SWAP,      L"Inverser clic gauche / droit\tF7");
    menu_bar = CreateMenu();
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)file, L"Fichier");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)view, L"Affichage");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)ctl,  L"Contr\u00f4les");
    SetMenu(hwnd, menu_bar);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    SDL_SetWindowSize(win, WIN_W, WIN_H);        /* keep the 4:3 client area */
#endif
}

/* PC mouse cursor hidden while over the video window (only the CD-i
 * cursor is shown); F8 toggles it, e.g. to check that both coincide. */
static int hide_host_cursor = 1, mouse_inside = 0;

static void apply_host_cursor(void)
{
    SDL_ShowCursor((hide_host_cursor && mouse_inside) ? SDL_DISABLE : SDL_ENABLE);
}

/* Always give the PC cursor back (called when the emulator exits) */
void video_display_restore_host_cursor(void)
{
    mouse_inside = 0;
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);
}

/* ===== Mouse capture while the game recentres the CD-i cursor =====
 * Games that read the pointer as relative movement put the CD-i cursor
 * back in place all the time; the PC mouse then wandered out of the
 * window. While the 68HC05 is in that mode, SDL's relative mouse mode
 * keeps the PC mouse in the window and its moves are sent as impulses.
 * Released ~2 s after the game stops recentring (menus, shell), with the
 * PC mouse put back under the CD-i cursor; F8 or leaving the window
 * (Alt+Tab) also releases it. */
static int capture = 0, capture_idle = 0, cap_acc_x = 0, cap_acc_y = 0;

static void capture_set(int on, int cdi_x, int cdi_y)
{
    if (on == capture) return;
    capture = on;
    cap_acc_x = cap_acc_y = 0;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    if (!on && win && ren) {                     /* PC mouse under the CD-i cursor */
        float lx = (float)cdi_x * WIN_W / (frame_w ? frame_w : WIN_W);
        float ly = (float)cdi_y * WIN_H / 560.0f;
        int wx, wy;
        SDL_RenderLogicalToWindow(ren, lx, ly, &wx, &wy);
        SDL_WarpMouseInWindow(win, wx, wy);
    }
    apply_host_cursor();
}

void video_display_pointer_mode(int relative, int cdi_x, int cdi_y)
{
    static int headless = -1;
    if (headless < 0) headless = getenv("EMU_HEADLESS") != NULL;
    int focused = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
    if (headless || !focused || !hide_host_cursor) { capture_set(0, cdi_x, cdi_y); return; }
    if (relative) {
        capture_idle = 0;
        if (mouse_inside) capture_set(1, cdi_x, cdi_y);
    } else if (capture && ++capture_idle >= 100) {   /* 2 s at 50 Hz */
        capture_set(0, cdi_x, cdi_y);
    }
}

void video_display_toggle_host_cursor(void)
{
    hide_host_cursor = !hide_host_cursor;
    if (!hide_host_cursor) capture_set(0, ptr_x, ptr_y);
    apply_host_cursor();
    video_display_menu_check(VM_HOST_CURSOR, !hide_host_cursor);
    fprintf(stderr, "[video_display] PC mouse cursor %s over the video\n",
            hide_host_cursor ? "hidden" : "shown");
}

/* File dialog to choose a disc image. Returns 1 and a UTF-8 path in out. */
int video_display_choose_disc(char *out, int out_size)
{
#ifdef _WIN32
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP))
        video_display_toggle_fullscreen();          /* the dialog must be visible */
    SDL_ShowCursor(SDL_ENABLE);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    HWND owner = (win && SDL_GetWindowWMInfo(win, &info)) ? info.info.win.window : NULL;
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Images de disque CD-i (*.cue;*.bin;*.iso;*.img)\0*.cue;*.bin;*.iso;*.img\0Tous les fichiers\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    ofn.lpstrTitle = L"Ins\u00e9rer un disque dans le CD-i";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    int ok = GetOpenFileNameW(&ofn);
    apply_host_cursor();
    if (!ok) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, file, -1, out, out_size, NULL, NULL) > 0;
#else
    (void)out; (void)out_size;
    return 0;
#endif
}

/* F11 / Alt+Enter: window <-> full screen (desktop resolution, 4:3 kept) */
void video_display_toggle_fullscreen(void)
{
    if (!win) return;
    int fs = (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    video_display_menu_check(VM_FULLSCREEN, !fs);
    fprintf(stderr, "[video_display] %s\n", fs ? "windowed" : "full screen");
}

int video_display_handle_event(const void *ev)
{
    const SDL_Event *e = (const SDL_Event *)ev;
    if (!initialized) return 0;

    switch (e->type) {
#ifdef _WIN32
    case SDL_SYSWMEVENT: {
        const SDL_SysWMmsg *m = e->syswm.msg;
        if (m && m->subsystem == SDL_SYSWM_WINDOWS && m->msg.win.msg == WM_COMMAND &&
            HIWORD(m->msg.win.wParam) == 0 && menu_cb) {
            menu_cb((int)LOWORD(m->msg.win.wParam));
            return 1;
        }
        return 0;
    }
#endif
    case SDL_WINDOWEVENT:
        if (e->window.windowID != window_id) return 0;
        if (e->window.event == SDL_WINDOWEVENT_ENTER) { mouse_inside = 1; apply_host_cursor(); }
        if (e->window.event == SDL_WINDOWEVENT_LEAVE || e->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
            e->window.event == SDL_WINDOWEVENT_HIDDEN || e->window.event == SDL_WINDOWEVENT_CLOSE) {
            mouse_inside = 0;           /* never leave the PC cursor hidden elsewhere */
            apply_host_cursor();
        }
        return 0;                       /* let the console handle close, etc. */
    case SDL_MOUSEMOTION:
        if (e->motion.windowID != window_id) return 0;
        if (capture) {                          /* moves -> impulses (pointer units) */
            cap_acc_x += e->motion.xrel * frame_w;
            cap_acc_y += e->motion.yrel * 560;
            int dx = cap_acc_x / WIN_W, dy = cap_acc_y / WIN_H;
            cap_acc_x -= dx * WIN_W;
            cap_acc_y -= dy * WIN_H;
            if ((dx || dy) && pointer_cb)
                pointer_cb(pointer_ctx, dx, dy, ptr_buttons | VIDEO_PTR_IMPULSE);
            return 1;
        }
        window_to_pointer(e->motion.x, e->motion.y);
        pointer_report();
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e->button.windowID != window_id) return 0;
        int bit = (e->button.button == SDL_BUTTON_LEFT)  ? 1 :
                  (e->button.button == SDL_BUTTON_RIGHT) ? 2 : 0;
        if (!bit) return 1;
        if (e->type == SDL_MOUSEBUTTONDOWN) ptr_buttons |= bit; else ptr_buttons &= ~bit;
        if (capture) {                          /* no move, only the buttons */
            if (pointer_cb) pointer_cb(pointer_ctx, 0, 0, ptr_buttons | VIDEO_PTR_IMPULSE);
            return 1;
        }
        window_to_pointer(e->button.x, e->button.y);
        pointer_report();
        return 1;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (e->key.windowID != window_id) return 0;
        int down = (e->type == SDL_KEYDOWN);
        if (down && !e->key.repeat && e->key.keysym.sym == SDLK_o && (SDL_GetModState() & KMOD_CTRL)) {
            if (menu_cb) menu_cb(VM_OPEN_DISC);
            return 1;
        }
        if (down && !e->key.repeat && (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER)
            && (SDL_GetModState() & KMOD_ALT)) {
            video_display_toggle_fullscreen();
            return 1;
        }
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

    /* Back-end processor: both VSCs drive the V.S.D. command port. The
     * ICA words apply before the frame, the DCA words from their line on
     * (games change the image contribution factors, CLUT, ... mid-screen). */
    int bep_ia = 0, bep_ib = 0;
#define BEP_UPTO(row) do {         while (bep_ia < frame->bep_count && frame->bep_line[bep_ia] <= (row))             vsd_command(&vsd, frame->bep[bep_ia++]);         while (bep_ib < frame_b->bep_count && frame_b->bep_line[bep_ib] <= (row))             vsd_command(&vsd, frame_b->bep[bep_ib++]);     } while (0)
    BEP_UPTO(-1);

    /* EMU_PLANE_LOG=1: every 50 frames, the most used pixel values of each
     * plane and the mixing registers (black picture hunting) */
    if (getenv("EMU_PLANE_LOG")) {
        static int nf = 0;
        if (++nf % 50 == 0) {
            for (int pl = 0; pl < 2; pl++) {
                const scc_frame_t *f = pl ? frame_b : frame;
                static int hist[256];
                memset(hist, 0, sizeof(hist));
                for (int y = 0; y < f->height; y++)
                    for (int x = 0; x < f->width; x++) hist[f->pix[y * SCC_OUT_MAX_W + x]]++;
                fprintf(stderr, "[PLANE %c] f%d en=%d %dx%d:", pl ? 'B' : 'A', nf, f->enabled, f->width, f->height);
                for (int k = 0; k < 5; k++) {
                    int best = 0;
                    for (int i = 1; i < 256; i++) if (hist[i] > hist[best]) best = i;
                    if (!hist[best]) break;
                    fprintf(stderr, " %02X:%d(%06X)", best, hist[best], (unsigned)vsd.clut[best]);
                    hist[best] = 0;
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "[PLANE] icm=%06X tcr=%06X order=%X transp=%06X/%06X mask=%06X/%06X w=%X/%X\n",
                    (unsigned)vsd.icm, (unsigned)vsd.tcr, (unsigned)vsd.plane_order,
                    (unsigned)vsd.transp_a, (unsigned)vsd.transp_b, (unsigned)vsd.mask_a,
                    (unsigned)vsd.mask_b, (unsigned)vsd.weight_a, (unsigned)vsd.weight_b);
        }
    }

    int w = frame->width, h = frame->height;
    frame_w = w; frame_h = h;
    if (!frame->enabled && !frame_b->enabled) {
        /* DE = 0 on both: no picture */
        memset(pixels, 0, (size_t)SCC_OUT_MAX_W * SCC_OUT_MAX_H * sizeof(uint32_t));
    } else {
        int wb = frame_b->width, hb = frame_b->height;
        static uint32_t line_a[SCC_OUT_MAX_W], line_b[SCC_OUT_MAX_W];
        for (int y = 0; y < h; y++) {
            BEP_UPTO(y);
            int dyuv_a = vsd_plane_dyuv(&vsd, 0), dyuv_b = vsd_plane_dyuv(&vsd, 1);
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
    BEP_UPTO(0x7FFF);                                 /* words past the last line */
#undef BEP_UPTO

    dump_frame_if_changed(w, h);

    SDL_Rect src_rect = { 0, 0, w, h };
    SDL_UpdateTexture(tex, NULL, pixels, SCC_OUT_MAX_W * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, &src_rect, NULL);    /* stretched to the 4:3 surface */
    SDL_RenderPresent(ren);
    return 1;
}
