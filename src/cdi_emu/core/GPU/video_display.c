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
#include "../FMV/fmv.h"
#include "input_ui.h"
#include "../options.h"
#include "../input_map.h"
#include "../CDIC/cdrom_win.h"
#include <wchar.h>
#include "../bus.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
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
static int log_w = 768;                /* 1024 in 16:9 */
static int aspect_wide = 0;            /* menu Affichage / F2, options.ini */
static int video_filter = 0;           /* menu Affichage / F4, options.ini */
static int overscan_mask = 1;          /* edges hidden like a TV (see OVERSCAN_PX) */
#define WIN_W   log_w
#define WIN_H   576

int g_video_window_closed = 0;
static void create_menu(void);
static void update_view_menu(void);

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

    aspect_wide = option_get("aspect", 0) ? 1 : 0;
    log_w = aspect_wide ? 1024 : 768;
    video_filter = option_get("filter", 0);
    if (video_filter < 0 || video_filter >= VIDEO_FILTERS) video_filter = 0;
    overscan_mask = option_get("overscan", 1) ? 1 : 0;
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
    SDL_RenderSetLogicalSize(ren, WIN_W, WIN_H);       /* 4:3 or 16:9, black borders */

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
    input_map_init();                          /* keys / controllers, controles.ini */
    video_display_menu_check(VM_MOUSE, input_map_mouse_enabled());
    video_display_menu_check(VM_PADS, input_map_pads_enabled());
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
static int ptr_x = 384, ptr_y = 240, ptr_buttons = 0;   /* ptr_buttons: mouse */
static int pad_buttons = 0;          /* buttons held on the keyboard / controllers */
static int swap_mouse = 0;           /* left click = CD-i button 2 (F7) */

void video_display_set_swap(int swap) { swap_mouse = swap != 0; }

/* Hardware reset of the V.S.D. (warm reboot: quitting a disc). Without it
 * the shell inherited the game's last state - e.g. contribution factors
 * left at 0 by a fade to black - and only the backdrop colour showed. */
void video_display_reset_vsd(void) { vsd_reset(&vsd); }

void video_display_set_title(const char *title)
{
    if (win) SDL_SetWindowTitle(win, title ? title : "CD-i 205");
}

/* Name of the loaded disc in the title ("" or NULL = no disc) */
void video_display_set_disc_title(const char *disc)
{
    char t[300];
    if (disc && *disc) snprintf(t, sizeof(t), "CD-i 205 - %s", disc);
    else snprintf(t, sizeof(t), "CD-i 205 - pas de disque");
    video_display_set_title(t);
}

void video_display_message(const char *title, const char *text)
{
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP))
        video_display_toggle_fullscreen();       /* the box must be visible */
    SDL_ShowCursor(SDL_ENABLE);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, text, win);
}
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
    if (pointer_cb) pointer_cb(pointer_ctx, ptr_x, ptr_y, ptr_buttons | pad_buttons);
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
    HMENU hw = CreatePopupMenu();
    AppendMenuW(hw, MF_STRING, VM_FASTBOOT, L"D\u00e9marrage rapide (jusqu\u2019au menu du CD-i)");
    AppendMenuW(hw, MF_STRING, VM_FMV, L"Cartouche vidéo numérique (MPEG)");
    AppendMenuW(file, MF_STRING, VM_OPEN_DISC, L"Ouvrir un disque...\tCtrl+O");
    AppendMenuW(file, MF_STRING, VM_EJECT,     L"\u00c9jecter le disque");
    AppendMenuW(file, MF_STRING, VM_PAUSE,     L"Pause\tPause / Ctrl+P");
    AppendMenuW(file, MF_STRING, VM_RESET,     L"R\u00e9initialiser le CD-i\tCtrl+R");
    AppendMenuW(file, MF_STRING, VM_IMPORT_ROM, L"Importer des ROM...");
    AppendMenuW(file, MF_STRING, VM_CONVERT_CHD, L"Convertir une image CHD en BIN/CUE...");
    {   /* every CD / DVD / BD drive of the PC */
        char drives[26];
        int nd = cdrom_list_drives(drives, 26);
        if (nd) AppendMenuW(file, MF_SEPARATOR, 0, NULL);
        for (int i = 0; i < nd; i++) {
            wchar_t label[96];
            swprintf(label, 96, L"Jouer le disque du lecteur %c:", drives[i]);
            AppendMenuW(file, MF_STRING, VM_DRIVE_PLAY + (drives[i] - 'A'), label);
            swprintf(label, 96, L"Extraire le disque du lecteur %c: en BIN/CUE...", drives[i]);
            AppendMenuW(file, MF_STRING, VM_DRIVE_RIP + (drives[i] - 'A'), label);
            /* burning in its own submenu, simulation first and far from
             * the real burn, which cannot be undone */
            HMENU burn = CreatePopupMenu();
            AppendMenuW(burn, MF_STRING, VM_DRIVE_BURN_TEST + (drives[i] - 'A'),
                        L"Simulation (laser éteint, le disque reste vierge)...");
            AppendMenuW(burn, MF_SEPARATOR, 0, NULL);
            AppendMenuW(burn, MF_SEPARATOR, 0, NULL);
            AppendMenuW(burn, MF_STRING, VM_DRIVE_BURN + (drives[i] - 'A'),
                        L"GRAVER réellement sur un CD-R vierge (définitif)...");
            swprintf(label, 96, L"Graver une image (lecteur %c:)", drives[i]);
            AppendMenuW(file, MF_POPUP, (UINT_PTR)burn, label);
        }
    }
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, VM_SAVE,      L"Sauvegarder l'\u00e9tat\tF5");
    AppendMenuW(file, MF_STRING, VM_LOAD,      L"Rappeler l'\u00e9tat\tF9");
    AppendMenuW(file, MF_STRING, VM_NEXT_SLOT, L"Slot suivant\tF6");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, VM_QUIT,      L"Quitter");
    AppendMenuW(view, MF_STRING, VM_FULLSCREEN, L"Plein \u00e9cran\tF11");
    AppendMenuW(view, MF_STRING, VM_HOST_CURSOR, L"Souris du PC visible\tF8");
    AppendMenuW(view, MF_STRING, VM_CONSOLE,   L"Console s\u00e9rie (UART)");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, VM_ASPECT_43,  L"Format 4:3\tF2");
    AppendMenuW(view, MF_STRING, VM_ASPECT_169, L"Format 16:9 (\u00e9tir\u00e9)\tF2");
    AppendMenuW(view, MF_STRING, VM_OVERSCAN,   L"Bords masqu\u00e9s (comme sur une t\u00e9l\u00e9)");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, VM_FILTER0 + 0, L"Filtre : aucun (pixels nets)\tF4");
    AppendMenuW(view, MF_STRING, VM_FILTER0 + 1, L"Filtre : liss\u00e9 (flou)");
    AppendMenuW(view, MF_STRING, VM_FILTER0 + 4, L"Filtre : liss\u00e9 net");
    AppendMenuW(view, MF_STRING, VM_FILTER0 + 5, L"Filtre : liss\u00e9 contours");
    AppendMenuW(view, MF_STRING, VM_FILTER0 + 3, L"Filtre : \u00e9cran cathodique");
    AppendMenuW(view, MF_STRING, VM_FILTER0 + 2, L"Filtre : lignes de balayage");
    AppendMenuW(ctl,  MF_STRING, VM_INPUT_CONFIG, L"Assigner les touches...");
    AppendMenuW(ctl,  MF_STRING, VM_MOUSE,     L"Souris active");
    AppendMenuW(ctl,  MF_STRING, VM_PADS,      L"Manettes actives (décoché : ignorées dès le démarrage)");
    AppendMenuW(ctl,  MF_SEPARATOR, 0, NULL);
    AppendMenuW(ctl,  MF_STRING, VM_SWAP,      L"Inverser clic gauche / droit\tF7");
    menu_bar = CreateMenu();
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)file, L"Fichier");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)view, L"Affichage");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)ctl,  L"Contr\u00f4les");
    AppendMenuW(menu_bar, MF_POPUP, (UINT_PTR)hw,   L"Mat\u00e9riel");
    SetMenu(hwnd, menu_bar);
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
    SDL_SetWindowSize(win, WIN_W, WIN_H);        /* keep the 4:3 / 16:9 client area */
    update_view_menu();
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
 * window. While the 68HC05 is in that mode the moves are sent as
 * impulses, and the (hidden) PC mouse is put back in the middle when it
 * nears the left / right / bottom edge, so it stays in the window. Not
 * the top edge: pushing the mouse up there releases it, to reach the menu
 * bar; a click in the picture captures it again. Also released ~2 s after
 * the game stops recentring (menus, shell), with the PC mouse put back
 * under the CD-i cursor; F8 or leaving the window (Alt+Tab) release it. */
static int capture = 0, capture_idle = 0, cap_acc_x = 0, cap_acc_y = 0;
static int capture_suspended = 0;    /* left by the top edge, until a click */
static int warp_pending = 0;         /* skip the motion caused by our warp */

#define CAP_MARGIN 48                /* logical pixels from an edge */

static void warp_logical(float lx, float ly)
{
    int wx, wy;
    SDL_RenderLogicalToWindow(ren, lx, ly, &wx, &wy);
    SDL_WarpMouseInWindow(win, wx, wy);
}

static void capture_set(int on, int cdi_x, int cdi_y)
{
    if (on == capture) return;
    capture = on;
    cap_acc_x = cap_acc_y = 0;
    warp_pending = 0;
    if (!on && win && ren) {                     /* PC mouse under the CD-i cursor */
        warp_logical((float)cdi_x * WIN_W / (frame_w ? frame_w : WIN_W), (float)cdi_y * WIN_H / 560.0f);
    }
    apply_host_cursor();
}

void video_display_pointer_mode(int relative, int cdi_x, int cdi_y)
{
    static int headless = -1;
    if (headless < 0) headless = getenv("EMU_HEADLESS") != NULL;
    int focused = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS);
    if (headless || !focused || !hide_host_cursor || input_ui_is_open() || !input_map_mouse_enabled()) {
        capture_set(0, cdi_x, cdi_y);
        return;
    }
    if (relative) {
        capture_idle = 0;
        if (mouse_inside && !capture_suspended) capture_set(1, cdi_x, cdi_y);
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

/* ---- progress window (disc ripping / burning) ----
 * A small window owned by the video window, without close button: a line
 * of text and a bar drawn here. Its messages are dispatched by SDL's own
 * event pump (same thread), so it needs no loop of its own. */
#ifdef _WIN32
static HWND prog_wnd = NULL;
static wchar_t prog_text[256];
static int prog_pct = 0;

static LRESULT CALLBACK prog_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT old = (HFONT)SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        RECT tr = { 16, 12, rc.right - 16, 34 };
        DrawTextW(dc, prog_text, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT bar = { 16, 40, rc.right - 16, 62 };            /* frame, then fill */
        FillRect(dc, &bar, GetSysColorBrush(COLOR_WINDOW));
        FrameRect(dc, &bar, GetSysColorBrush(COLOR_BTNSHADOW));
        RECT fill = { bar.left + 2, bar.top + 2, bar.left + 2 + (bar.right - bar.left - 4) * prog_pct / 100, bar.bottom - 2 };
        if (fill.right > fill.left) FillRect(dc, &fill, GetSysColorBrush(COLOR_HIGHLIGHT));
        wchar_t pct[16];
        swprintf(pct, 16, L"%d %%", prog_pct);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        RECT pr = { 16, 68, rc.right - 16, 88 };
        DrawTextW(dc, pct, -1, &pr, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_CLOSE) return 0;                            /* ends by itself */
    return DefWindowProcW(h, m, w, l);
}
#endif

void video_display_progress(const char *title, const char *text, int pct)
{
#ifdef _WIN32
    if (pct < 0) {
        if (prog_wnd) DestroyWindow(prog_wnd);
        prog_wnd = NULL;
        return;
    }
    if (pct > 100) pct = 100;
    wchar_t wt[256];
    MultiByteToWideChar(CP_UTF8, 0, text ? text : "", -1, wt, 256);
    if (!prog_wnd) {
        static int registered = 0;
        HINSTANCE inst = GetModuleHandleW(NULL);
        if (!registered) {
            WNDCLASSW wc;
            ZeroMemory(&wc, sizeof(wc));
            wc.lpfnWndProc = prog_proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursor(NULL, IDC_ARROW);
            wc.lpszClassName = L"CDi205Progress";
            RegisterClassW(&wc);
            registered = 1;
        }
        if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP))
            video_display_toggle_fullscreen();          /* the window must be visible */
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        HWND owner = (win && SDL_GetWindowWMInfo(win, &info)) ? info.info.win.window : NULL;
        wchar_t wtitle[128];
        MultiByteToWideChar(CP_UTF8, 0, title ? title : "CD-i 205", -1, wtitle, 128);
        DWORD style = WS_POPUP | WS_CAPTION;
        RECT r = { 0, 0, 440, 96 };
        AdjustWindowRect(&r, style, FALSE);
        int w = r.right - r.left, h = r.bottom - r.top, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        RECT o;
        if (owner && GetWindowRect(owner, &o)) {           /* centred on the video */
            x = (o.left + o.right - w) / 2;
            y = (o.top + o.bottom - h) / 2;
        }
        prog_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"CDi205Progress", wtitle, style,
                                   x, y, w, h, owner, NULL, inst, NULL);
        if (!prog_wnd) return;
        prog_text[0] = 0;
        prog_pct = -1;
        ShowWindow(prog_wnd, SW_SHOWNOACTIVATE);
    }
    if (pct != prog_pct || wcscmp(wt, prog_text)) {
        prog_pct = pct;
        wcscpy(prog_text, wt);
        InvalidateRect(prog_wnd, NULL, FALSE);
    }
#else
    (void)title; (void)text; (void)pct;
#endif
}

/* File dialog to choose a disc image. Returns 1 and a UTF-8 path in out. */
int video_display_choose_disc(char *out, int out_size)
{
    return video_display_choose_image(NULL, out, out_size);
}

int video_display_confirm(const char *title, const char *text, const char *yes)
{
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP))
        video_display_toggle_fullscreen();       /* the box must be visible */
    SDL_ShowCursor(SDL_ENABLE);
    const SDL_MessageBoxButtonData buttons[2] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Annuler" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes },
    };
    const SDL_MessageBoxData box = { SDL_MESSAGEBOX_WARNING, win, title, text, 2, buttons, NULL };
    int id = 0;
    if (SDL_ShowMessageBox(&box, &id) != 0) id = 0;
    apply_host_cursor();
    return id == 1;
}

int video_display_choose_image(const char *title, char *out, int out_size)
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
    ofn.lpstrFilter = L"Images de disque CD-i (*.cue;*.bin;*.chd;*.iso;*.img)\0*.cue;*.bin;*.chd;*.iso;*.img\0"
                      L"Images CHD (*.chd)\0*.chd\0Tous les fichiers\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = 1024;
    wchar_t wtitle[256] = L"Ins\u00e9rer un disque dans le CD-i";
    if (title) MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    int ok = GetOpenFileNameW(&ofn);
    apply_host_cursor();
    if (!ok) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, file, -1, out, out_size, NULL, NULL) > 0;
#else
    (void)title; (void)out; (void)out_size;
    return 0;
#endif
}

int video_display_choose_files(const char *title, char ***out)
{
    *out = NULL;
#ifdef _WIN32
    if (win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP))
        video_display_toggle_fullscreen();          /* the dialog must be visible */
    SDL_ShowCursor(SDL_ENABLE);
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    HWND owner = (win && SDL_GetWindowWMInfo(win, &info)) ? info.info.win.window : NULL;
    static wchar_t buf[16384];
    wchar_t wtitle[256];
    buf[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Dumps de ROM (*.bin;*.hex;*.rom)\0*.bin;*.hex;*.rom\0Tous les fichiers\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 16384;
    ofn.lpstrTitle = wtitle;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
              | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    int ok = GetOpenFileNameW(&ofn);
    apply_host_cursor();
    if (!ok) return 0;
    /* one file: its full path; several: the folder, then the names */
    const wchar_t *dir = buf, *p = buf + wcslen(buf) + 1;
    char **list = calloc(64, sizeof(char *));
    int n = 0;
    if (!list) return 0;
    if (!*p) {
        if ((list[0] = malloc(4096)) != NULL) {
            WideCharToMultiByte(CP_UTF8, 0, dir, -1, list[0], 4096, NULL, NULL);
            n = 1;
        }
    } else {
        for (; *p && n < 64; p += wcslen(p) + 1) {
            wchar_t full[4096];
            _snwprintf(full, 4096, L"%ls\\%ls", dir, p);
            full[4095] = 0;
            if ((list[n] = malloc(4096)) == NULL) break;
            WideCharToMultiByte(CP_UTF8, 0, full, -1, list[n], 4096, NULL, NULL);
            n++;
        }
    }
    *out = list;
    return n;
#else
    (void)title;
    return 0;
#endif
}

/* ---- Picture format (4:3 / 16:9) and video filters ----
 * 16:9 stretches the CD-i picture horizontally (logical surface 1024x576),
 * for discs made for wide screens (The 7th Guest). Filters are done with
 * the renderer (no shader support in SDL2's 2D API): smooth = bilinear
 * scaling; scanlines = a dark band under each CD-i line; CRT = smooth +
 * scanlines + RGB aperture mask + vignette. */
static SDL_Texture *mask_tex = NULL, *vign_tex = NULL;

static const char *const FILTER_NAMES[VIDEO_FILTERS] = {
    "aucun", "liss\xC3\xA9 (flou)", "lignes de balayage", "\xC3\xA9" "cran cathodique",
    "liss\xC3\xA9 net", "liss\xC3\xA9 contours"
};
static const int FILTER_ORDER[VIDEO_FILTERS] = { 0, 1, 4, 5, 3, 2 };   /* menu / F4 order */

void video_display_next_filter(void)
{
    int k = 0;
    while (k < VIDEO_FILTERS && FILTER_ORDER[k] != video_filter) k++;
    video_display_set_filter(FILTER_ORDER[(k + 1) % VIDEO_FILTERS]);
}

/* ---- Smoothing (CPU upscaling, then bilinear to the window) ----
 * Plain bilinear from 240 lines to a 1080-line window blurs over 4 screen
 * pixels. "Sharp": pixel replication by an integer factor first, so the
 * final bilinear only softens a one-pixel seam. "Edges": EPX / Scale2x
 * applied twice on the CD-i's own resolution (a 768-wide frame whose pixel
 * pairs are all equal is 384 wide underneath): staircases and diagonals
 * get rounded, flat areas and photos stay as they are. */
#define HI_W (SCC_OUT_MAX_W * 2)
#define HI_H (SCC_OUT_MAX_H * 4)
static uint32_t *hi_buf = NULL, *hi_tmp = NULL;
static SDL_Texture *hi_tex = NULL;

/* EPX / Scale2x: src w x h -> dst 2w x 2h */
static void scale2x(const uint32_t *src, int sp, int w, int h, uint32_t *dst, int dp)
{
    for (int y = 0; y < h; y++) {
        const uint32_t *row = src + y * sp;
        const uint32_t *up = y > 0 ? row - sp : row, *dn = y < h - 1 ? row + sp : row;
        uint32_t *o0 = dst + (2 * y) * dp, *o1 = o0 + dp;
        for (int x = 0; x < w; x++) {
            uint32_t P = row[x], A = up[x], D = dn[x];
            uint32_t C = x > 0 ? row[x - 1] : P, B = x < w - 1 ? row[x + 1] : P;
            uint32_t e0 = P, e1 = P, e2 = P, e3 = P;
            if (A != D && C != B) {
                if (C == A) e0 = A;
                if (A == B) e1 = B;
                if (D == C) e2 = C;
                if (B == D) e3 = D;
            }
            o0[2 * x] = e0; o0[2 * x + 1] = e1;
            o1[2 * x] = e2; o1[2 * x + 1] = e3;
        }
    }
}

/* the frame as a hi-res picture for filters 4 and 5; returns 0 if not used */
static int upscale_frame(const uint32_t *pix, int w, int h, int *ow, int *oh)
{
    if (video_filter != 4 && video_filter != 5) return 0;
    if (!hi_buf) {
        hi_buf = malloc((size_t)HI_W * HI_H * 4);
        hi_tmp = malloc((size_t)HI_W * HI_H * 4);
        if (!hi_buf || !hi_tmp) return 0;
    }
    if (!hi_tex) {
        hi_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, HI_W, HI_H);
        if (!hi_tex) return 0;
        SDL_SetTextureScaleMode(hi_tex, SDL_ScaleModeLinear);
    }
    if (w > SCC_OUT_MAX_W || h > SCC_OUT_MAX_H) return 0;
    if (video_filter == 4) {                     /* x2 across, x4 down, replicated */
        for (int y = 0; y < h; y++) {
            const uint32_t *s = pix + y * SCC_OUT_MAX_W;
            uint32_t *d = hi_buf + (4 * y) * HI_W;
            for (int x = 0; x < w; x++) d[2 * x] = d[2 * x + 1] = s[x];
            for (int k = 1; k < 4; k++) memcpy(d + k * HI_W, d, (size_t)w * 2 * 4);
        }
        *ow = 2 * w; *oh = 4 * h;
        return 1;
    }
    /* edges: is the frame 384 pixels doubled? The CD-i cursor (16 x 16) can
     * sit on an odd pixel and break a few pairs: that alone must not switch
     * the whole picture to the 768 path (the picture changed with every
     * mouse move). */
    int odd = (w % 2) ? w * h : 0;
    for (int y = 0; y < h && odd <= 256; y++) {
        const uint32_t *s = pix + y * SCC_OUT_MAX_W;
        for (int x = 0; x + 1 < w; x += 2) if (s[x] != s[x + 1]) odd++;
    }
    int pairs = odd <= 256;
    int nw = pairs ? w / 2 : w;
    static uint32_t native[SCC_OUT_MAX_W * SCC_OUT_MAX_H];
    for (int y = 0; y < h; y++) {
        const uint32_t *s = pix + y * SCC_OUT_MAX_W;
        uint32_t *d = native + y * SCC_OUT_MAX_W;
        if (pairs) for (int x = 0; x < nw; x++) d[x] = s[2 * x];
        else memcpy(d, s, (size_t)w * 4);
    }
    scale2x(native, SCC_OUT_MAX_W, nw, h, hi_tmp, HI_W);              /* 2nw x 2h */
    if (pairs)
        scale2x(hi_tmp, HI_W, 2 * nw, 2 * h, hi_buf, HI_W);           /* 4nw x 4h = 2w x 4h */
    else {
        /* 768-wide content: once is enough across (twice would be 4 x 768,
         * wider than the buffer: each line ran over the next one and the
         * picture showed two half-width copies); lines doubled down */
        for (int y = 0; y < 2 * h; y++) {
            const uint32_t *s = hi_tmp + y * HI_W;
            memcpy(hi_buf + (2 * y) * HI_W, s, (size_t)w * 2 * 4);
            memcpy(hi_buf + (2 * y + 1) * HI_W, s, (size_t)w * 2 * 4);
        }
    }
    *ow = 2 * w; *oh = 4 * h;
    return 1;
}

/* Left / right edges hidden like a TV's overscan (menu Affichage,
 * options.ini overscan=0|1, on by default). The 768-pixel modes fetch 384
 * bytes per line whatever the game stores there: Inca's flight scenes keep
 * 348-byte lines, so the last 20 pixels showed the start of the next line.
 * On a TV that part is off screen. */
#define OVERSCAN_PX 24                 /* in 768-pixel units, each side */

static void update_view_menu(void)
{
    video_display_menu_check(VM_OVERSCAN, overscan_mask);
    video_display_menu_check(VM_ASPECT_43, !aspect_wide);
    video_display_menu_check(VM_ASPECT_169, aspect_wide);
    for (int i = 0; i < VIDEO_FILTERS; i++) video_display_menu_check(VM_FILTER0 + i, video_filter == i);
}

void video_display_set_aspect(int wide)
{
    aspect_wide = wide ? 1 : 0;
    log_w = aspect_wide ? 1024 : 768;
    option_set("aspect", aspect_wide);
    if (win && !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
        int w, h;
        SDL_GetWindowSize(win, &w, &h);
        SDL_SetWindowSize(win, h * log_w / WIN_H, h);   /* keep the height */
    }
    update_view_menu();
    fprintf(stderr, "[video_display] format %s\n", aspect_wide ? "16:9" : "4:3");
}
int  video_display_aspect_wide(void) { return aspect_wide; }

void video_display_set_filter(int f)
{
    video_filter = (f % VIDEO_FILTERS + VIDEO_FILTERS) % VIDEO_FILTERS;
    option_set("filter", video_filter);
    update_view_menu();
    fprintf(stderr, "[video_display] video filter: %s\n", FILTER_NAMES[video_filter]);
}
int  video_display_filter(void) { return video_filter; }
const char *video_display_filter_name(void) { return FILTER_NAMES[video_filter]; }

static void make_filter_textures(void)
{
    if (!mask_tex) {        /* aperture mask: R, G, B columns, multiplied */
        uint32_t px[3] = { 0xFFFFC8C8u, 0xFFC8FFC8u, 0xFFC8C8FFu };
        mask_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 3, 1);
        if (mask_tex) {
            SDL_UpdateTexture(mask_tex, NULL, px, sizeof(px));
            SDL_SetTextureBlendMode(mask_tex, SDL_BLENDMODE_MOD);
        }
    }
    if (!vign_tex) {        /* vignette: darker corners, multiplied, smoothed */
        enum { VW = 64, VH = 48 };
        static uint32_t px[VW * VH];
        for (int y = 0; y < VH; y++)
            for (int x = 0; x < VW; x++) {
                float dx = (x + 0.5f) / VW - 0.5f, dy = (y + 0.5f) / VH - 0.5f;
                float d = (dx * dx + dy * dy) * 4.0f;                /* 0 centre .. 2 corner */
                int v = (int)(255.0f * (1.0f - 0.22f * d * d));
                if (v < 0) v = 0;
                px[y * VW + x] = 0xFF000000u | (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)v;
            }
        vign_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, VW, VH);
        if (vign_tex) {
            SDL_UpdateTexture(vign_tex, NULL, px, VW * 4);
            SDL_SetTextureBlendMode(vign_tex, SDL_BLENDMODE_MOD);
            SDL_SetTextureScaleMode(vign_tex, SDL_ScaleModeLinear);
        }
    }
}

/* after the picture is copied: lines (h = CD-i lines shown) and mask */
static void apply_filter(int h)
{
    if ((video_filter == 2 || video_filter == 3) && h > 0) {           /* scanlines: lower 40 % of each line */
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, video_filter == 3 ? 110 : 90);
        for (int y = 0; y < h; y++) {
            int y0 = y * WIN_H / h, y1 = (y + 1) * WIN_H / h;
            int band = (y1 - y0) * 2 / 5;
            if (band < 1) band = 1;
            SDL_Rect r = { 0, y1 - band, log_w, band };
            SDL_RenderFillRect(ren, &r);
        }
    }
    if (video_filter == 3) {
        make_filter_textures();
        if (mask_tex) {
            for (int x = 0; x < log_w; x += 3) {            /* 1 logical pixel per column */
                SDL_Rect r = { x, 0, 3, WIN_H };
                SDL_RenderCopy(ren, mask_tex, NULL, &r);
            }
        }
        if (vign_tex) SDL_RenderCopy(ren, vign_tex, NULL, NULL);
    }
}

/* ---- F3 statistics overlay ---- */
static int  stats_on = 0;
static char stats_text[2048];
static int  host_frames = 0;
static double host_fps = 0;
static Uint64 host_fps_t0 = 0;

void video_display_toggle_stats(void)
{
    stats_on = !stats_on;
    fprintf(stderr, "[video_display] statistics %s (F3)\n", stats_on ? "shown" : "hidden");
}
int  video_display_stats_on(void) { return stats_on; }
void video_display_set_stats(const char *text)
{
    snprintf(stats_text, sizeof(stats_text), "%s", text ? text : "");
}

/* frames presented per real second */
static void stats_count_frame(void)
{
    Uint64 now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    if (!host_fps_t0) host_fps_t0 = now;
    host_frames++;
    if (now - host_fps_t0 >= f) {
        host_fps = host_frames * (double)f / (double)(now - host_fps_t0);
        host_frames = 0;
        host_fps_t0 = now;
    }
}

static void stats_render(SDL_Renderer *r)
{
    if (!stats_on) return;
    static TTF_Font *font = NULL;
    static int tried = 0;
    if (!tried) { tried = 1; font = input_ui_font(15); }
    if (!font) return;
    char all[2300];
    snprintf(all, sizeof(all), "Affichage : %.1f images/s (image CD-i %dx%d)\n%s",
             host_fps, frame_w, frame_h, stats_text);
    /* one texture per line, on a translucent box (logical 768x576) */
    SDL_Surface *lines[32];
    int n = 0, wmax = 0, htot = 0;
    SDL_Color fg = { 255, 255, 160, 255 };
    for (char *p = all; *p && n < 32; ) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[300];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len); line[len] = 0;
        lines[n] = TTF_RenderUTF8_Blended(font, *line ? line : " ", fg);
        if (lines[n]) { if (lines[n]->w > wmax) wmax = lines[n]->w; htot += lines[n]->h; n++; }
        if (!e) break;
        p = e + 1;
    }
    SDL_Rect bg = { 6, 6, wmax + 12, htot + 10 };
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_RenderFillRect(r, &bg);
    int y = 11;
    for (int i = 0; i < n; i++) {
        SDL_Texture *t = SDL_CreateTextureFromSurface(r, lines[i]);
        SDL_Rect d = { 12, y, lines[i]->w, lines[i]->h };
        if (t) { SDL_RenderCopy(r, t, NULL, &d); SDL_DestroyTexture(t); }
        y += lines[i]->h;
        SDL_FreeSurface(lines[i]);
    }
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
            int id = (int)LOWORD(m->msg.win.wParam);
            if (id == VM_INPUT_CONFIG) { input_ui_open(); return 1; }
            if (id == VM_MOUSE) {
                input_map_set_mouse(!input_map_mouse_enabled());
                video_display_menu_check(VM_MOUSE, input_map_mouse_enabled());
                return 1;
            }
            if (id == VM_OVERSCAN) {
                overscan_mask = !overscan_mask;
                option_set("overscan", overscan_mask);
                update_view_menu();
                return 1;
            }
            if (id == VM_PADS) {
                input_map_set_pads(!input_map_pads_enabled());
                video_display_menu_check(VM_PADS, input_map_pads_enabled());
                return 1;
            }
            menu_cb(id);
            return 1;
        }
        return 0;
    }
#endif
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION:
    case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED:          /* plain joysticks too */
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: case SDL_JOYAXISMOTION: case SDL_JOYHATMOTION:
    case SDL_JOYBALLMOTION:
        if (input_ui_is_open()) return input_ui_event(e);
        input_map_event(e, 0);          /* hot-plug; held state is read each frame */
        return 1;
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
        if (input_ui_is_open()) return input_ui_event(e);
        if (!input_map_mouse_enabled()) return 1;     /* mouse switched off */
        if (capture) {                          /* moves -> impulses (pointer units) */
            int mx = e->motion.x, my = e->motion.y;     /* logical 768x576 */
            if (warp_pending) {                 /* the jump of our own warp */
                if ((mx > 384 - 24 && mx < 384 + 24 && my > 288 - 24 && my < 288 + 24) || --warp_pending == 0)
                    warp_pending = 0;
                return 1;
            }
            if (my <= 2 && e->motion.yrel < 0) {   /* pushed against the top: free */
                capture_suspended = 1;
                capture = 0;                    /* no warp: stay at the menu bar */
                cap_acc_x = cap_acc_y = 0;
                SDL_ShowCursor(SDL_ENABLE);
                return 1;
            }
            if (mx < CAP_MARGIN || mx > WIN_W - CAP_MARGIN || my > WIN_H - CAP_MARGIN) {
                warp_logical(384.0f, 288.0f);   /* keep the hidden mouse inside */
                warp_pending = 8;
            }
            cap_acc_x += e->motion.xrel * frame_w;
            cap_acc_y += e->motion.yrel * 560;
            int dx = cap_acc_x / WIN_W, dy = cap_acc_y / WIN_H;
            cap_acc_x -= dx * WIN_W;
            cap_acc_y -= dy * WIN_H;
            if ((dx || dy) && pointer_cb)
                pointer_cb(pointer_ctx, dx, dy, ptr_buttons | pad_buttons | VIDEO_PTR_IMPULSE);
            return 1;
        }
        window_to_pointer(e->motion.x, e->motion.y);
        pointer_report();
        return 1;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e->button.windowID != window_id) return 0;
        if (input_ui_is_open()) return input_ui_event(e);
        if (!input_map_mouse_enabled()) return 1;     /* mouse switched off */
        if (capture_suspended && e->type == SDL_MOUSEBUTTONDOWN) {
            capture_suspended = 0;              /* click in the picture: capture again */
            apply_host_cursor();
        }
        int bit = (e->button.button == SDL_BUTTON_LEFT)  ? (swap_mouse ? 2 : 1) :
                  (e->button.button == SDL_BUTTON_RIGHT) ? (swap_mouse ? 1 : 2) : 0;
        if (!bit) return 1;
        if (e->type == SDL_MOUSEBUTTONDOWN) ptr_buttons |= bit; else ptr_buttons &= ~bit;
        if (capture) {                          /* no move, only the buttons */
            if (pointer_cb) pointer_cb(pointer_ctx, 0, 0, ptr_buttons | pad_buttons | VIDEO_PTR_IMPULSE);
            return 1;
        }
        window_to_pointer(e->button.x, e->button.y);
        pointer_report();
        return 1;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (e->key.windowID != window_id) return 0;
        if (input_ui_is_open()) return input_ui_event(e);
        int down = (e->type == SDL_KEYDOWN);
        if (down && !e->key.repeat && e->key.keysym.sym == SDLK_o && (SDL_GetModState() & KMOD_CTRL)) {
            if (menu_cb) menu_cb(VM_OPEN_DISC);
            return 1;
        }
        if (down && !e->key.repeat && (e->key.keysym.sym == SDLK_PAUSE ||
            (e->key.keysym.sym == SDLK_p && (SDL_GetModState() & KMOD_CTRL)))) {
            if (menu_cb) menu_cb(VM_PAUSE);
            return 1;
        }
        if (down && !e->key.repeat && e->key.keysym.sym == SDLK_r && (SDL_GetModState() & KMOD_CTRL)) {
            if (menu_cb) menu_cb(VM_RESET);
            return 1;
        }
        if (down && !e->key.repeat && (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER)
            && (SDL_GetModState() & KMOD_ALT)) {
            video_display_toggle_fullscreen();
            return 1;
        }
        /* Other keys: read each frame through their assignment
         * (input_map_frame, Controles > Assigner les touches) */
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

static int video_log_frame = 0;       /* frames shown, for the logs */

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
        fprintf(stderr, "[VIDEO %c] f%d DE=%d DCR=%04X DCR2=%04X CSR=%04X ICA=%d insns "
                        "-> start=%05X %dx%d\n",
                plane, video_log_frame, f->enabled, v->dcr, v->dcr2, v->csr_control, f->ica_count,
                f->vsr_start, f->width, f->height);
        last_dcr = v->dcr; last_dcr2 = v->dcr2; last_vsr = f->vsr_start;
    }
    if (f->bep_count != last_bep_count ||
        memcmp(last_bep, f->bep, (size_t)f->bep_count * sizeof(uint32_t)) != 0) {
        fprintf(stderr, "[VIDEO %c] f%d BEP CONTROL x%d:", plane, video_log_frame, f->bep_count);
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
    if (hash == last_hash && !getenv("EMU_VIDEO_DUMP_EVERY")) return;
    last_hash = hash;

    /* a "%d" in the name numbers the frames (f%04d.bmp -> f0000.bmp, ...);
     * EMU_VIDEO_DUMP_EVERY=n: one picture every n frames, numbered by frame
     * (timelines compared with a video of the real console) */
    static int frame_no = 0, every = -1;
    if (every < 0) every = getenv("EMU_VIDEO_DUMP_EVERY") ? atoi(getenv("EMU_VIDEO_DUMP_EVERY")) : 0;
    char name[600];
    if (every > 0) {
        if (video_log_frame % every) return;
        snprintf(name, sizeof(name), path, video_log_frame);
    } else if (strstr(path, "%")) snprintf(name, sizeof(name), path, frame_no++);
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

/* Hardware cursor shown: the shell (or a game) is up - ends the fast boot */
int video_display_cursor_on(void) { return (vsd.cursor_ctrl & 0x800000u) != 0; }

/* Last picture shown, for the pause screen (redrawn without emulating) */
static int last_w = 0, last_h = 0, last_hi = 0, last_hw = 0, last_hh = 0;

/* Pause (Fichier > Pause, key Pause): the last picture, darkened, with
 * "PAUSE" on it; the F3 overlay and the menus stay usable. */
void video_display_redraw_paused(void)
{
    if (!initialized || !last_w) return;
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (last_hi) {
        SDL_Rect hr = { 0, 0, last_hw, last_hh };
        SDL_RenderCopy(ren, hi_tex, &hr, NULL);
    } else {
        SDL_Rect sr = { 0, 0, last_w, last_h };
        SDL_RenderCopy(ren, tex, &sr, NULL);
    }
    apply_filter(last_h);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 110);
    SDL_RenderFillRect(ren, NULL);
    static TTF_Font *font = NULL;
    static int tried = 0;
    if (!tried) { tried = 1; font = input_ui_font(48); }
    if (font) {
        SDL_Color fg = { 255, 255, 255, 255 };
        SDL_Surface *sf = TTF_RenderUTF8_Blended(font, "PAUSE", fg);
        if (sf) {
            int lw, lh;
            SDL_RenderGetLogicalSize(ren, &lw, &lh);
            if (!lw) { lw = WIN_W; lh = WIN_H; }
            SDL_Texture *t = SDL_CreateTextureFromSurface(ren, sf);
            SDL_Rect d = { (lw - sf->w) / 2, (lh - sf->h) / 2, sf->w, sf->h };
            if (t) { SDL_RenderCopy(ren, t, NULL, &d); SDL_DestroyTexture(t); }
            SDL_FreeSurface(sf);
        }
    }
    stats_render(ren);
    SDL_RenderPresent(ren);
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
    video_log_frame++;
    {   /* EMU_EXIT_FRAME=n: quit after n frames (regression bench) */
        static int exit_at = -1;
        if (exit_at < 0) exit_at = getenv("EMU_EXIT_FRAME") ? atoi(getenv("EMU_EXIT_FRAME")) : 0;
        if (exit_at > 0 && video_log_frame >= exit_at) { g_video_window_closed = 1; return 0; }
    }
    {   /* EMU_CLICKS=frame:x,y[,buttons];...: clicks at given frames (pointer
         * units, like EMU_AUTOCLICK) - regression bench. Moved to (x,y) at
         * the frame (gliding over the 16 frames before), pressed 2 frames
         * later, released 20 frames after that. */
        static int n = -1, fr[32], cx[32], cy[32], cb[32];
        if (n < 0) {
            n = 0;
            const char *e = getenv("EMU_CLICKS");
            while (e && *e && n < 32) {
                cb[n] = 1;
                if (sscanf(e, "%d:%d,%d,%d", &fr[n], &cx[n], &cy[n], &cb[n]) >= 3) n++;
                e = strchr(e, ';');
                if (e) e++;
            }
        }
        for (int k = 0; k < n && pointer_cb; k++) {
            int d = video_log_frame - fr[k];
            if (d >= -16 && d < 0) {          /* glide there over 16 frames, like a mouse
                                               * (some discs ignore a jump to PLAY) */
                int t = d + 16;
                pointer_cb(pointer_ctx, 200 + (cx[k] - 200) * t / 15, 300 + (cy[k] - 300) * t / 15, 0);
            }
            if (d == 0 || d == 22) pointer_cb(pointer_ctx, cx[k], cy[k], 0);   /* held 20 frames */
            else if (d == 2) pointer_cb(pointer_ctx, cx[k], cy[k], cb[k]);
        }
    }
    {   /* EMU_IMPULSES=frame:dx,dy,buttons[,count];...: relative moves (games
         * that recentre the cursor, e.g. Asterix), one per frame from that
         * frame, count times (tests) */
        static int n = -1, fr[256], dx[256], dy[256], bt[256], ct[256];
        if (n < 0) {
            n = 0;
            const char *e = getenv("EMU_IMPULSES");
            while (e && *e && n < 256) {
                ct[n] = 1;
                if (sscanf(e, "%d:%d,%d,%d,%d", &fr[n], &dx[n], &dy[n], &bt[n], &ct[n]) >= 4) n++;
                e = strchr(e, ';');
                if (e) e++;
            }
        }
        for (int k = 0; k < n && pointer_cb; k++) {
            int d = video_log_frame - fr[k];
            if (d >= 0 && d < ct[k])
                pointer_cb(pointer_ctx, dx[k], dy[k], bt[k] | VIDEO_PTR_IMPULSE);
        }
    }
    {   /* EMU_RAM_DUMP=prefix,f1,f2,...: write the whole DRAM to prefix_<frame>.bin
         * at those frames (tests: find a game's variables) */
        static int nd = -1, fr[32];
        static char pre[512];
        if (nd < 0) {
            nd = 0;
            const char *e = getenv("EMU_RAM_DUMP");
            if (e) {
                const char *c = strchr(e, ',');
                if (c && c - e < (int)sizeof(pre)) {
                    memcpy(pre, e, (size_t)(c - e)); pre[c - e] = 0;
                    while (c && nd < 32) { fr[nd++] = atoi(c + 1); c = strchr(c + 1, ','); }
                }
            }
        }
        for (int k = 0; k < nd; k++)
            if (fr[k] == video_log_frame) {
                char name[600];
                snprintf(name, sizeof(name), "%s_%05d.bin", pre, video_log_frame);
                FILE *fp = fopen(name, "wb");
                /* $000000 1 MB DRAM, then the $500000 bank (512 KB) */
                if (fp) { fwrite(b->dram, 1, sizeof(b->dram), fp); fwrite(b->ram2, 1, sizeof(b->ram2), fp); fclose(fp); }
            }
    }
    log_frame_changes('A', v,  frame);
    log_frame_changes('B', vb, frame_b);
    if (video_log_on()) {      /* where each plane's picture comes from (line 20) */
        static uint32_t last_l[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        for (int pl = 0; pl < 2; pl++) {
            uint32_t l = (pl ? frame_b : frame)->line_addr[20];
            if (l != last_l[pl]) {
                fprintf(stderr, "[VIDEO %c] f%d line20 @%05X\n", pl ? 'B' : 'A', video_log_frame, (unsigned)l);
                last_l[pl] = l;
            }
        }
    }
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
#define BEP_UPTO(row) do {         while (bep_ia < frame->bep_count && frame->bep_line[bep_ia] <= (row))             vsd_command_plane(&vsd, frame->bep[bep_ia++], 0);         while (bep_ib < frame_b->bep_count && frame_b->bep_line[bep_ib] <= (row))             vsd_command_plane(&vsd, frame_b->bep[bep_ib++], 1);     } while (0)
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
            int ext = fmv_video_showing();
            vsd_ext_on = 0;
            {   /* EMU_REGION_DUMP=n: at frame n, print the per-line region
                 * registers, coding and transparency (tests) */
                static int target = -2, nfr = 0;
                if (target == -2) target = getenv("EMU_REGION_DUMP") ? atoi(getenv("EMU_REGION_DUMP")) : -1;
                if (y == 0) nfr++;
                if (target > 0 && nfr == target && y < 60)
                    fprintf(stderr, "[LINE] y%3d A %05X B %05X\n", y,
                            (unsigned)frame->line_addr[y], (unsigned)frame_b->line_addr[y]);
                if (target > 0 && nfr == target && getenv("EMU_BLINE") && y == atoi(getenv("EMU_BLINE"))) {
                    const bus_t *bb = b;                      /* raw bytes of that line, plane B */
                    uint32_t a0 = frame_b->line_addr[y];
                    fprintf(stderr, "[BLINE] y%d @%05X:", y, (unsigned)a0);
                    for (int k = 0; k < 96; k++) fprintf(stderr, " %02X", bb->dram[(a0 + k) % sizeof(bb->dram)]);
                    fprintf(stderr, "\n");
                }
                if (target > 0 && nfr == target) {
                    static uint32_t prev[16];
                    uint32_t cur[16];
                    memcpy(cur, vsd.region, sizeof(vsd.region));
                    cur[8] = vsd.icm; cur[9] = vsd.tcr; cur[10] = vsd.plane_order; cur[11] = vsd.transp_a;
                    cur[12] = vsd.transp_b; cur[13] = vsd.weight_a; cur[14] = vsd.weight_b; cur[15] = vsd.mask_a;
                    if (y == 0 || memcmp(cur, prev, sizeof(cur))) {
                        fprintf(stderr, "[REG] y%3d icm %06X tcr %06X ord %X tA %06X tB %06X wA %X wB %X mA %06X R:", y, (unsigned)vsd.icm, (unsigned)vsd.tcr, (unsigned)vsd.plane_order, (unsigned)vsd.transp_a, (unsigned)vsd.transp_b, (unsigned)vsd.weight_a, (unsigned)vsd.weight_b, (unsigned)vsd.mask_a);
                        for (int k = 0; k < 8; k++) fprintf(stderr, " %06X", (unsigned)vsd.region[k]);
                        fprintf(stderr, "\n");
                        memcpy(prev, cur, sizeof(cur));
                    }
                }
            }
            for (int x = 0; x < w; x++) {
                if (ext) vsd_ext_on = fmv_video_pixel(x * 384 / w, y, &vsd_ext_rgb);
                vsd_region_flags(&vsd, x * 768 / w, vsd_rflag);
                int b_on = frame_b->enabled && x < wb && y < hb;
                static int hide = -1;        /* EMU_HIDE_PLANE=A or B (tests) */
                if (hide < 0) hide = getenv("EMU_HIDE_PLANE") ? *getenv("EMU_HIDE_PLANE") : 0;
                if (hide == 'B') b_on = 0;
                dst[x] = vsd_mix_rgb(&vsd, src_a[x], frame->enabled && hide != 'A', dyuv_a ? &line_a[x] : NULL,
                                     b_on ? src_b[x] : 0, b_on, (dyuv_b && b_on) ? &line_b[x] : NULL);
            }
        }
        if (overscan_mask) {                          /* before the cursor: it stays visible */
            int m = OVERSCAN_PX * w / 768;
            for (int y = 0; y < h; y++) {
                uint32_t *row = &pixels[y * SCC_OUT_MAX_W];
                for (int x = 0; x < m; x++) { row[x] = 0xFF000000u; row[w - 1 - x] = 0xFF000000u; }
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
    {   /* format: the assignment panel is laid out for 4:3 */
        static int cur_w = 0;
        int want = input_ui_is_open() ? 768 : log_w;
        if (want != cur_w) { cur_w = want; SDL_RenderSetLogicalSize(ren, want, WIN_H); }
    }
    int hw, hh;
    last_w = w; last_h = h; last_hi = 0;
    if (upscale_frame(pixels, w, h, &hw, &hh)) {  /* filters 4 / 5: CPU-upscaled first */
        SDL_Rect hr = { 0, 0, hw, hh };
        SDL_UpdateTexture(hi_tex, &hr, hi_buf, HI_W * (int)sizeof(uint32_t));
        SDL_RenderCopy(ren, hi_tex, &hr, NULL);
        last_hi = 1; last_hw = hw; last_hh = hh;
    } else {
        SDL_SetTextureScaleMode(tex, video_filter == 0 || video_filter == 2 ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        SDL_RenderCopy(ren, tex, &src_rect, NULL);    /* stretched to the 4:3 / 16:9 surface */
    }
    apply_filter(h);
    input_ui_render(ren);                         /* assignment panel, if open */
    stats_count_frame();
    stats_render(ren);                            /* F3 statistics */
    {   /* EMU_STATS_SHOT=file.bmp,frame: F3 on from the start, window saved at
         * that frame (tests) */
        static int init = 0, at = -1, nf = 0;
        static char path[600];
        if (!init) {
            init = 1;
            const char *e = getenv("EMU_STATS_SHOT");
            if (e && sscanf(e, "%590[^,],%d", path, &at) == 2) stats_on = !getenv("EMU_SHOT_NOSTATS");
            const char *ws = getenv("EMU_WIN_SIZE");      /* WxH (tests) */
            int ww, wh;
            if (ws && sscanf(ws, "%dx%d", &ww, &wh) == 2) SDL_SetWindowSize(win, ww, wh);
        }
        if (at >= 0 && ++nf == at) {
            int ow, oh;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (s && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
                SDL_SaveBMP(s, path);
            if (s) SDL_FreeSurface(s);
        }
    }
    {   /* EMU_UI_SHOT=file.bmp: open the panel and save the window (tests) */
        static int shot = 0;
        const char *p = getenv("EMU_UI_SHOT");
        if (p && shot < 3 && ++shot == 2) input_ui_open();
        if (p && shot == 3) {
            int ow, oh;
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (s && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
                SDL_SaveBMP(s, p);
            if (s) SDL_FreeSurface(s);
        }
        if (p && shot == 2) shot = 3;
    }
    SDL_RenderPresent(ren);

    /* Keyboard / controllers -> CD-i pointer, through their assignment */
    if (!input_ui_is_open() && pointer_cb) {
        int dx, dy, b;
        input_map_frame(SDL_GetKeyboardFocus() == win, &dx, &dy, &b);
        if (dx || dy || b != pad_buttons) {
            pad_buttons = b;
            pointer_cb(pointer_ctx, dx, dy, ptr_buttons | pad_buttons | VIDEO_PTR_IMPULSE);
        }
    }
    return 1;
}
