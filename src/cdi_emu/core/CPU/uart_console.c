/*
 * uart_console.c - SDL2-based UART terminal window
 *
 * Implements an 80x25 character terminal using SDL2 and SDL_ttf.
 * Characters written to the emulated UART TX register are rendered
 * here, and keyboard input is captured into a ring buffer that the
 * UART RX register reads from.
 *
 * Supports basic ANSI escape sequences for cursor movement, screen
 * clearing, and color (used by the CD-i firmware console output).
 */

#include "uart_console.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ===== Terminal configuration ===== */
#define UART_COLS      80
#define UART_ROWS      25
#define CHAR_W         9
#define CHAR_H         16
#define MAX_ESC_PARAMS 8
#define RX_BUF_SIZE    256

/*
 * Font search paths. The terminal tries each path in order and uses
 * the first font that loads successfully.
 */
#ifndef UART_FONT_PATH
#define UART_FONT_PATH "DejaVuSansMono.ttf"
#endif

/* ===== SDL2 state ===== */
static SDL_Window   *win  = NULL;
static int console_wanted(void);
static SDL_Renderer *ren  = NULL;
static TTF_Font     *font = NULL;
static int initialized = 0;

/* ===== Text screen grid (character + per-cell colors) ===== */
static char      screen[UART_ROWS][UART_COLS + 1];
static SDL_Color fg_grid[UART_ROWS][UART_COLS];
static SDL_Color bg_grid[UART_ROWS][UART_COLS];
static int cursor_x = 0, cursor_y = 0;

/* Default and current colors */
static const SDL_Color default_fg = {200, 200, 200, 255};
static const SDL_Color default_bg = {0,   0,   0,   255};
static SDL_Color current_fg;
static SDL_Color current_bg;
static int reverse_mode = 0;

/* ANSI standard 8-color palette */
static const SDL_Color ansi_colors[8] = {
    {0,   0,   0,   255}, /* 0: black   */
    {205, 0,   0,   255}, /* 1: red     */
    {0,   205, 0,   255}, /* 2: green   */
    {205, 205, 0,   255}, /* 3: yellow  */
    {0,   0,   238, 255}, /* 4: blue    */
    {205, 0,   205, 255}, /* 5: magenta */
    {0,   205, 205, 255}, /* 6: cyan    */
    {229, 229, 229, 255}  /* 7: white   */
};

/* ===== ANSI escape sequence parser state ===== */
typedef enum { ST_NORMAL, ST_ESC, ST_CSI } parser_state_t;
static parser_state_t state = ST_NORMAL;
static int esc_params[MAX_ESC_PARAMS];
static int esc_param_count = 0;
static int esc_cur_param = 0;

/* ===== RX ring buffer (keyboard input for the emulated UART) ===== */
static uint8_t rx_buf[RX_BUF_SIZE];
static int rx_head = 0, rx_tail = 0;

/* Try opening a TrueType font from a list of candidate paths */
static TTF_Font *try_open_font(void) {
    const char *paths[] = {
        UART_FONT_PATH,
        "DejaVuSansMono.ttf",
        "C:/Windows/Fonts/consola.ttf",   /* Consolas */
        "C:/Windows/Fonts/cour.ttf",      /* Courier New */
        "/ucrt64/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",   /* Debian / Ubuntu */
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",               /* Arch */
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf", /* Fedora */
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(paths[i], 14);
        if (f) {
            fprintf(stderr, "[uart_console] Font loaded: %s\n", paths[i]);
            return f;
        }
    }
    return NULL;
}

/* Push a character into the RX ring buffer */
static void rx_push(uint8_t c) {
    int next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_buf[rx_head] = c;
        rx_head = next;
    }
}

/* Check if keyboard input is available in the RX buffer */
int uart_console_has_input(void) {
    return rx_head != rx_tail;
}

/* Retrieve the next character from the RX buffer */
uint8_t uart_console_get_input(void) {
    uint8_t c = 0;
    if (rx_head != rx_tail) {
        c = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    }
    return c;
}

/* ===== Screen manipulation ===== */

/* Clear the entire screen to default colors */
static void clear_screen(void) {
    memset(screen, ' ', sizeof(screen));
    for (int y = 0; y < UART_ROWS; y++) {
        screen[y][UART_COLS] = '\0';
        for (int x = 0; x < UART_COLS; x++) {
            fg_grid[y][x] = default_fg;
            bg_grid[y][x] = default_bg;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
}

/* Scroll the screen up by one line (bottom line cleared) */
static void scroll_up(void) {
    memmove(screen[0], screen[1], (UART_ROWS - 1) * (UART_COLS + 1));
    memmove(fg_grid[0], fg_grid[1], (UART_ROWS - 1) * UART_COLS * sizeof(SDL_Color));
    memmove(bg_grid[0], bg_grid[1], (UART_ROWS - 1) * UART_COLS * sizeof(SDL_Color));

    memset(screen[UART_ROWS - 1], ' ', UART_COLS);
    screen[UART_ROWS - 1][UART_COLS] = '\0';
    for (int x = 0; x < UART_COLS; x++) {
        fg_grid[UART_ROWS - 1][x] = default_fg;
        bg_grid[UART_ROWS - 1][x] = default_bg;
    }
}

/* Clear from cursor position to end of line */
static void clear_line_from_cursor(void) {
    for (int x = cursor_x; x < UART_COLS; x++) {
        screen[cursor_y][x] = ' ';
        fg_grid[cursor_y][x] = default_fg;
        bg_grid[cursor_y][x] = default_bg;
    }
}

/* Move cursor to next line, scrolling if needed */
static void newline(void) {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= UART_ROWS) {
        scroll_up();
        cursor_y = UART_ROWS - 1;
    }
}

/* Place a character at the cursor position and advance cursor */
static void putchar_screen(char c) {
    if (cursor_x >= UART_COLS) newline();

    screen[cursor_y][cursor_x] = c;
    if (reverse_mode) {
        fg_grid[cursor_y][cursor_x] = current_bg;
        bg_grid[cursor_y][cursor_x] = current_fg;
    } else {
        fg_grid[cursor_y][cursor_x] = current_fg;
        bg_grid[cursor_y][cursor_x] = current_bg;
    }
    cursor_x++;
}

/* ===== ANSI CSI sequence handler ===== */

/*
 * Handle a completed CSI (ESC [ ... <final>) escape sequence.
 * Supports cursor positioning, screen/line clearing, cursor movement,
 * and SGR (color/attribute) control.
 */
static void handle_csi(char final) {
    if (esc_param_count < MAX_ESC_PARAMS) {
        esc_params[esc_param_count++] = esc_cur_param;
    }

    switch (final) {
        case 'H':
        case 'f': {  /* Set cursor position (row;col, 1-based) */
            int row = (esc_param_count >= 1 && esc_params[0] > 0) ? esc_params[0] : 1;
            int col = (esc_param_count >= 2 && esc_params[1] > 0) ? esc_params[1] : 1;
            cursor_y = row - 1;
            cursor_x = col - 1;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_x < 0) cursor_x = 0;
            if (cursor_y >= UART_ROWS) cursor_y = UART_ROWS - 1;
            if (cursor_x >= UART_COLS) cursor_x = UART_COLS - 1;
            break;
        }

        case 'J':  /* Erase display */
            if (esc_param_count == 0 || esc_params[0] == 2 || esc_params[0] == 0) {
                clear_screen();
            }
            break;

        case 'K':  /* Erase line from cursor */
            clear_line_from_cursor();
            break;

        case 'A':  /* Cursor up */
            cursor_y -= (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_y < 0) cursor_y = 0;
            break;

        case 'B':  /* Cursor down */
            cursor_y += (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_y >= UART_ROWS) cursor_y = UART_ROWS - 1;
            break;

        case 'C':  /* Cursor right */
            cursor_x += (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_x >= UART_COLS) cursor_x = UART_COLS - 1;
            break;

        case 'D':  /* Cursor left */
            cursor_x -= (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_x < 0) cursor_x = 0;
            break;

        case 'm':  /* SGR - Set graphic rendition (colors/attributes) */
            for (int i = 0; i < esc_param_count; i++) {
                int p = esc_params[i];
                if (p == 0) {
                    current_fg = default_fg;
                    current_bg = default_bg;
                    reverse_mode = 0;
                } else if (p == 7) {
                    reverse_mode = 1;
                } else if (p == 27) {
                    reverse_mode = 0;
                } else if (p >= 30 && p <= 37) {
                    current_fg = ansi_colors[p - 30];
                } else if (p >= 40 && p <= 47) {
                    current_bg = ansi_colors[p - 40];
                } else if (p >= 90 && p <= 97) {
                    current_fg = ansi_colors[p - 90];
                } else if (p >= 100 && p <= 107) {
                    current_bg = ansi_colors[p - 100];
                }
            }
            break;

        default:
            break;
    }
}

/* ===== Public API ===== */

/*
 * Process a character from the emulated UART TX output.
 * Handles control characters (CR, LF, BS, TAB) and ANSI escape
 * sequences (ESC [ ...).
 */
void uart_console_putchar(char c) {
    switch (state) {
    case ST_NORMAL:
        if (c == '\x1b') {
            state = ST_ESC;
            break;
        }
        if (c == '\r') { cursor_x = 0; break; }
        if (c == '\n') { newline(); break; }
        if (c == '\b') { if (cursor_x > 0) cursor_x--; break; }
        if (c == '\t') {
            cursor_x = (cursor_x / 8 + 1) * 8;
            if (cursor_x >= UART_COLS) newline();
            break;
        }
        putchar_screen(c);
        break;

    case ST_ESC:
        if (c == '[') {
            state = ST_CSI;
            esc_param_count = 0;
            esc_cur_param = 0;
            memset(esc_params, 0, sizeof(esc_params));
        } else {
            state = ST_NORMAL;
        }
        break;

    case ST_CSI:
        if (isdigit((unsigned char)c)) {
            esc_cur_param = esc_cur_param * 10 + (c - '0');
        } else if (c == ';') {
            if (esc_param_count < MAX_ESC_PARAMS) {
                esc_params[esc_param_count++] = esc_cur_param;
            }
            esc_cur_param = 0;
        } else {
            handle_csi(c);
            state = ST_NORMAL;
        }
        break;
    }
}

/*
 * Initialize the SDL2 terminal window.
 * Creates the window, renderer, and loads a TrueType font.
 * Call once at emulator startup.
 */
void uart_console_init(void) {
    if (initialized) return;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "[uart_console] SDL_Init error: %s\n", SDL_GetError());
            return;
        }
    }

    if (TTF_WasInit() == 0) {
        if (TTF_Init() != 0) {
            fprintf(stderr, "[uart_console] TTF_Init error: %s\n", TTF_GetError());
            return;
        }
    }

    win = SDL_CreateWindow("UART Terminal - CD-I",
                            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                            UART_COLS * CHAR_W, UART_ROWS * CHAR_H,
                            console_wanted() ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN);
    if (!win) {
        fprintf(stderr, "[uart_console] SDL_CreateWindow error: %s\n", SDL_GetError());
        return;
    }

    /* Try accelerated renderer, fall back to software */
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }

    font = try_open_font();
    if (!font) {
        fprintf(stderr, "[uart_console] No font found: %s\n", TTF_GetError());
    }

    current_fg = default_fg;
    current_bg = default_bg;
    reverse_mode = 0;
    clear_screen();

    SDL_StartTextInput();

    initialized = 1;
}

/*
 * Process SDL events and render the terminal.
 * Captures keyboard input into the RX buffer and renders the
 * current screen state with the TrueType font.
 * Returns 0 if the user closed the window, 1 otherwise.
 */
static uart_hotkey_cb hotkey_cb = NULL;

/* The serial console starts hidden (EMU_CONSOLE=1 shows it); the video
 * window's menu shows / hides it. */
static int console_wanted(void)
{
    const char *e = getenv("EMU_CONSOLE");
    return e && *e == '1';
}

int uart_console_is_visible(void)
{
    return win && (SDL_GetWindowFlags(win) & SDL_WINDOW_SHOWN) && !(SDL_GetWindowFlags(win) & SDL_WINDOW_HIDDEN);
}

void uart_console_set_visible(int on)
{
    if (!win) return;
    if (on) { SDL_ShowWindow(win); SDL_RaiseWindow(win); }
    else    SDL_HideWindow(win);
    extern void video_display_menu_check(int id, int on);
    video_display_menu_check(1103 /* VM_CONSOLE */, on);
}
void uart_console_set_hotkey_cb(uart_hotkey_cb cb) { hotkey_cb = cb; }

int uart_console_update(void) {
    if (!initialized) return 1;

    /* --- Handle SDL events ---
     * Events for all SDL2 windows are processed here. Window close
     * events (SDL_WINDOWEVENT_CLOSE) are dispatched by window ID:
     * the UART window close stops the emulator, the video display
     * window close sets a flag that video_display_update checks. */
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        /* Mouse / keys aimed at the video window drive the CD-i pointer */
        extern int video_display_handle_event(const void *sdl_event);
        if (e.type == SDL_KEYDOWN && !e.key.repeat && hotkey_cb &&
            e.key.keysym.sym >= SDLK_F1 && e.key.keysym.sym <= SDLK_F12 &&
            hotkey_cb((int)(e.key.keysym.sym - SDLK_F1) + 1))
            continue;
        if (video_display_handle_event(&e)) continue;
        switch (e.type) {
        case SDL_QUIT:
            return 0;

        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                /* Check which window was closed by its ID */
                extern int g_video_window_closed;
                extern uint32_t video_display_get_window_id(void);
                if (e.window.windowID == SDL_GetWindowID(win)) {
                    /* UART terminal closed: only hide it (menu Affichage) */
                    uart_console_set_visible(0);
                    continue;
                }
                if (e.window.windowID == video_display_get_window_id()) {
                    /* Video display window closed - set flag */
                    g_video_window_closed = 1;
                }
            }
            break;

        case SDL_TEXTINPUT:
            for (int i = 0; e.text.text[i] != '\0'; i++) {
                rx_push((uint8_t)e.text.text[i]);
            }
            break;

        case SDL_KEYDOWN: {
            SDL_Keymod mod = SDL_GetModState();
            switch (e.key.keysym.sym) {
                case SDLK_RETURN:
                case SDLK_KP_ENTER:
                    rx_push('\r');
                    break;
                case SDLK_BACKSPACE:
                    rx_push('\b');
                    break;
                case SDLK_TAB:
                    rx_push('\t');
                    break;
                case SDLK_ESCAPE:
                    rx_push('\x1b');
                    break;
                default:
                    /* Ctrl+letter -> ASCII control code (^A=1 .. ^Z=26) */
                    if (mod & KMOD_CTRL) {
                        SDL_Keycode k = e.key.keysym.sym;
                        if (k >= SDLK_a && k <= SDLK_z) {
                            rx_push((uint8_t)(k - SDLK_a + 1));
                        }
                    }
                    break;
            }
            break;
        }

        default:
            break;
        }
    }

    if (!ren) return 1;

    /* --- Render the terminal --- */

    /* Clear background */
    SDL_SetRenderDrawColor(ren, default_bg.r, default_bg.g, default_bg.b, 255);
    SDL_RenderClear(ren);

    /* Draw non-default cell backgrounds */
    for (int y = 0; y < UART_ROWS; y++) {
        for (int x = 0; x < UART_COLS; x++) {
            SDL_Color bg = bg_grid[y][x];
            if (bg.r != default_bg.r || bg.g != default_bg.g || bg.b != default_bg.b) {
                SDL_SetRenderDrawColor(ren, bg.r, bg.g, bg.b, 255);
                SDL_Rect cell = { x * CHAR_W, y * CHAR_H, CHAR_W, CHAR_H };
                SDL_RenderFillRect(ren, &cell);
            }
        }
    }

    /* Render text characters (one at a time for per-cell color support) */
    if (font) {
        for (int y = 0; y < UART_ROWS; y++) {
            for (int x = 0; x < UART_COLS; x++) {
                char ch = screen[y][x];
                if (ch == ' ' || ch == '\0') continue;
                char buf[2] = { ch, '\0' };
                SDL_Surface *surf = TTF_RenderText_Blended(font, buf, fg_grid[y][x]);
                if (surf) {
                    SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
                    SDL_Rect dst = { x * CHAR_W, y * CHAR_H, surf->w, surf->h };
                    SDL_RenderCopy(ren, tex, NULL, &dst);
                    SDL_DestroyTexture(tex);
                    SDL_FreeSurface(surf);
                }
            }
        }
    }

    /* Draw blinking cursor */
    static int blink_counter = 0;
    blink_counter = (blink_counter + 1) % 60;
    if (blink_counter < 30) {
        SDL_SetRenderDrawColor(ren, 200, 200, 200, 150);
        SDL_Rect cur = { cursor_x * CHAR_W, cursor_y * CHAR_H, CHAR_W, CHAR_H };
        SDL_RenderFillRect(ren, &cur);
    }

    SDL_RenderPresent(ren);
    return 1;
}
