#include "uart_console.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ================================================================
 * Configuration
 * ================================================================ */
#define UART_COLS      80
#define UART_ROWS      25
#define CHAR_W         9
#define CHAR_H         16
#define MAX_ESC_PARAMS 8
#define RX_BUF_SIZE    256

/* Chemin de la police - adaptez selon votre système.
 * Linux : /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf
 * Vous pouvez aussi embarquer une police .ttf dans le repo. */
#ifndef UART_FONT_PATH
#define UART_FONT_PATH "DejaVuSansMono.ttf"
#endif

/* ================================================================
 * Etat interne SDL
 * ================================================================ */
static SDL_Window   *win  = NULL;
static SDL_Renderer *ren  = NULL;
static TTF_Font     *font = NULL;
static int initialized = 0;

/* ================================================================
 * Grille d'affichage (texte + couleurs)
 * ================================================================ */
static char      screen[UART_ROWS][UART_COLS + 1];
static SDL_Color fg_grid[UART_ROWS][UART_COLS];
static SDL_Color bg_grid[UART_ROWS][UART_COLS];
static int cursor_x = 0, cursor_y = 0;

static const SDL_Color default_fg = {200, 200, 200, 255};
static const SDL_Color default_bg = {0,   0,   0,   255};
static SDL_Color current_fg;
static SDL_Color current_bg;
static int reverse_mode = 0;

static const SDL_Color ansi_colors[8] = {
    {0,   0,   0,   255}, /* 0 black   */
    {205, 0,   0,   255}, /* 1 red     */
    {0,   205, 0,   255}, /* 2 green   */
    {205, 205, 0,   255}, /* 3 yellow  */
    {0,   0,   238, 255}, /* 4 blue    */
    {205, 0,   205, 255}, /* 5 magenta */
    {0,   205, 205, 255}, /* 6 cyan    */
    {229, 229, 229, 255}  /* 7 white   */
};

/* ================================================================
 * Parseur ANSI - état
 * ================================================================ */
typedef enum { ST_NORMAL, ST_ESC, ST_CSI } parser_state_t;
static parser_state_t state = ST_NORMAL;
static int esc_params[MAX_ESC_PARAMS];
static int esc_param_count = 0;
static int esc_cur_param = 0;

/* ================================================================
 * Buffer RX (clavier -> firmware)
 * ================================================================ */
static uint8_t rx_buf[RX_BUF_SIZE];
static int rx_head = 0, rx_tail = 0;


static TTF_Font *try_open_font(void) {
    const char *paths[] = {
        UART_FONT_PATH,
        "DejaVuSansMono.ttf",
        "C:/Windows/Fonts/consola.ttf",   /* Consolas - toujours présent */
        "C:/Windows/Fonts/cour.ttf",      /* Courier New */
        "/ucrt64/share/fonts/TTF/DejaVuSansMono.ttf",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(paths[i], 14);
        if (f) {
            fprintf(stderr, "[uart_console] Police chargee: %s\n", paths[i]);
            return f;
        }
    }
    return NULL;
}

static void rx_push(uint8_t c) {
    int next = (rx_head + 1) % RX_BUF_SIZE;
    if (next != rx_tail) {
        rx_buf[rx_head] = c;
        rx_head = next;
    }
}

int uart_console_has_input(void) {
    return rx_head != rx_tail;
}

uint8_t uart_console_get_input(void) {
    uint8_t c = 0;
    if (rx_head != rx_tail) {
        c = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    }
    return c;
}

/* ================================================================
 * Gestion de l'écran texte
 * ================================================================ */
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

static void clear_line_from_cursor(void) {
    for (int x = cursor_x; x < UART_COLS; x++) {
        screen[cursor_y][x] = ' ';
        fg_grid[cursor_y][x] = default_fg;
        bg_grid[cursor_y][x] = default_bg;
    }
}

static void newline(void) {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= UART_ROWS) {
        scroll_up();
        cursor_y = UART_ROWS - 1;
    }
}

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

/* ================================================================
 * Traitement des séquences CSI (ESC [ ... lettre)
 * ================================================================ */
static void handle_csi(char final) {
    if (esc_param_count < MAX_ESC_PARAMS) {
        esc_params[esc_param_count++] = esc_cur_param;
    }

    switch (final) {
        case 'H':
        case 'f': {
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

        case 'J':
            if (esc_param_count == 0 || esc_params[0] == 2 || esc_params[0] == 0) {
                clear_screen();
            }
            break;

        case 'K':
            clear_line_from_cursor();
            break;

        case 'A':
            cursor_y -= (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_y < 0) cursor_y = 0;
            break;

        case 'B':
            cursor_y += (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_y >= UART_ROWS) cursor_y = UART_ROWS - 1;
            break;

        case 'C':
            cursor_x += (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_x >= UART_COLS) cursor_x = UART_COLS - 1;
            break;

        case 'D':
            cursor_x -= (esc_param_count && esc_params[0] > 0) ? esc_params[0] : 1;
            if (cursor_x < 0) cursor_x = 0;
            break;

        case 'm':
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

/* ================================================================
 * API publique : réception d'un caractère depuis uart_write8()
 * ================================================================ */
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

/* ================================================================
 * Initialisation SDL2
 * ================================================================ */
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
                            SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "[uart_console] SDL_CreateWindow error: %s\n", SDL_GetError());
        return;
    }

    // ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    // if (!ren) {
    //     ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    // }

    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }

    font = try_open_font();
    if (!font) {
        fprintf(stderr, "[uart_console] AUCUNE police trouvee: %s\n", TTF_GetError());
    }

    current_fg = default_fg;
    current_bg = default_bg;
    reverse_mode = 0;
    clear_screen();

    SDL_StartTextInput();

    initialized = 1;
}

/* ================================================================
 * Boucle de mise à jour : rendu + input clavier
 * Retourne 0 si fermeture demandée, 1 sinon.
 * ================================================================ */
int uart_console_update(void) {
    if (!initialized) return 1;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            uart_console_close();
            exit(0);        /* sortie immediate si la boucle principale l'ignore */
            return 0;

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
                    /* Raccourcis Ctrl+lettre -> code de contrôle ASCII */
                    if (mod & KMOD_CTRL) {
                        SDL_Keycode k = e.key.keysym.sym;
                        if (k >= SDLK_a && k <= SDLK_z) {
                            rx_push((uint8_t)(k - SDLK_a + 1)); /* ^A=1 .. ^Z=26 */
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

    SDL_SetRenderDrawColor(ren, default_bg.r, default_bg.g, default_bg.b, 255);
    SDL_RenderClear(ren);

    /* Fonds de cellule non-défaut */
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

    /* Texte */
    if (font) {
        for (int y = 0; y < UART_ROWS; y++) {
            /* Rendu ligne par ligne pour limiter le nombre d'appels TTF */
            char line[UART_COLS + 1];
            memcpy(line, screen[y], UART_COLS + 1);

            /* Rendu caractère par caractère pour supporter les couleurs
             * multiples par ligne (moins rapide mais simple et robuste) */
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

    /* Curseur clignotant */
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

/* ================================================================
 * Fermeture propre
 * ================================================================ */
void uart_console_close(void) {
    if (font) { TTF_CloseFont(font); font = NULL; }
    if (ren)  { SDL_DestroyRenderer(ren); ren = NULL; }
    if (win)  { SDL_DestroyWindow(win); win = NULL; }
    if (TTF_WasInit()) TTF_Quit();
    initialized = 0;
    /* Ne fait pas SDL_Quit() global si votre émulateur gère déjà
     * une fenêtre vidéo principale avec SDL_INIT_VIDEO partagé. */
}