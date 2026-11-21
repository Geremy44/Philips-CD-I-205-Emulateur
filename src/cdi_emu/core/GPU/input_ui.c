/*
 * input_ui.c - key / controller assignment panel (see input_ui.h)
 */

#include "input_ui.h"
#include "../input_map.h"
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <string.h>

static int open_ = 0;
static int cap_act = -1, cap_slot = -1;      /* cell waiting for an input */
static TTF_Font *font = NULL, *font_big = NULL;

/* Layout, logical 768x576 */
#define PX   48
#define PY   40
#define PW   672
#define PH   496
#define ROW0 132
#define ROWH 38
#define COLX0 70                              /* action names */
static const int COLX[IM_SLOTS] = { 176, 306, 436, 566 };
#define COLW 124
#define CELLH 30
#define BTN_Y 410
#define MOUSE_X 70
#define MOUSE_W 260
#define RESET_X 400
#define CLOSE_X 560
#define BTN_W 140

void input_ui_open(void)  { open_ = 1; cap_act = cap_slot = -1; }
void input_ui_close(void) { open_ = 0; cap_act = cap_slot = -1; }
int  input_ui_is_open(void) { return open_; }

static TTF_Font *load_font(int size)
{
    static const char *paths[] = {
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/consola.ttf", "DejaVuSans.ttf", NULL
    };
    if (!TTF_WasInit() && TTF_Init() != 0) return NULL;
    for (int i = 0; paths[i]; i++) {
        TTF_Font *f = TTF_OpenFont(paths[i], size);
        if (f) return f;
    }
    return NULL;
}

static int inside(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int input_ui_event(const SDL_Event *e)
{
    if (!open_) return 0;
    /* waiting for an input: keys, controller buttons, stick directions */
    if (cap_act >= 0) {
        if (e->type == SDL_KEYDOWN && e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            cap_act = cap_slot = -1;              /* cancel */
            return 1;
        }
        int code = input_map_event(e, 1);
        if (code) {
            /* the key / controller columns accept both kinds of input */
            input_map_set(cap_act, cap_slot, code);
            cap_act = cap_slot = -1;
            return 1;
        }
        if (e->type == SDL_MOUSEBUTTONDOWN) { cap_act = cap_slot = -1; return 1; }
        return e->type == SDL_KEYDOWN || e->type == SDL_KEYUP || e->type == SDL_MOUSEMOTION ||
               e->type == SDL_MOUSEBUTTONUP || e->type == SDL_CONTROLLERBUTTONDOWN ||
               e->type == SDL_CONTROLLERBUTTONUP || e->type == SDL_CONTROLLERAXISMOTION;
    }
    input_map_event(e, 0);                        /* controller hot-plug */
    switch (e->type) {
    case SDL_KEYDOWN:
        if (e->key.keysym.scancode == SDL_SCANCODE_ESCAPE) input_ui_close();
        return 1;
    case SDL_KEYUP: case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONUP:
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION:
        return 1;
    case SDL_MOUSEBUTTONDOWN: {
        int x = e->button.x, y = e->button.y;
        for (int a = 0; a < ACT_COUNT; a++)
            for (int s = 0; s < IM_SLOTS; s++)
                if (inside(x, y, COLX[s], ROW0 + a * ROWH, COLW, CELLH)) {
                    if (e->button.button == SDL_BUTTON_RIGHT) input_map_set(a, s, 0);
                    else { cap_act = a; cap_slot = s; }
                    return 1;
                }
        if (inside(x, y, MOUSE_X, BTN_Y, MOUSE_W, CELLH)) input_map_set_mouse(!input_map_mouse_enabled());
        else if (inside(x, y, RESET_X, BTN_Y, BTN_W, CELLH)) input_map_reset();
        else if (inside(x, y, CLOSE_X, BTN_Y, BTN_W, CELLH)) input_ui_close();
        return 1;
    }
    }
    return 0;
}

static void text(SDL_Renderer *ren, TTF_Font *f, const char *s, int x, int y, SDL_Color c, int center_w)
{
    if (!f || !s || !*s) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, surf);
    SDL_Rect r = { x, y, surf->w, surf->h };
    if (center_w > 0) {
        if (r.w > center_w - 6) r.w = center_w - 6;     /* clip long names */
        r.x = x + (center_w - r.w) / 2;
    }
    SDL_Rect src = { 0, 0, r.w, surf->h };
    if (t) { SDL_RenderCopy(ren, t, &src, &r); SDL_DestroyTexture(t); }
    SDL_FreeSurface(surf);
}

static void box(SDL_Renderer *ren, int x, int y, int w, int h, SDL_Color fill, SDL_Color line)
{
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawColor(ren, fill.r, fill.g, fill.b, fill.a);
    SDL_RenderFillRect(ren, &r);
    SDL_SetRenderDrawColor(ren, line.r, line.g, line.b, line.a);
    SDL_RenderDrawRect(ren, &r);
}

void input_ui_render(SDL_Renderer *ren)
{
    if (!open_) return;
    if (!font) font = load_font(17);
    if (!font_big) font_big = load_font(24);
    const SDL_Color white = { 235, 235, 240, 255 }, grey = { 160, 165, 180, 255 },
                    gold = { 250, 210, 90, 255 }, panel = { 18, 22, 40, 235 },
                    edge = { 90, 110, 170, 255 }, cell = { 40, 48, 80, 255 },
                    active = { 120, 90, 20, 255 };
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect dim = { 0, 0, 768, 576 };
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 120);
    SDL_RenderFillRect(ren, &dim);
    box(ren, PX, PY, PW, PH, panel, edge);

    text(ren, font_big, "Assignation des touches", PX + 22, PY + 14, white, 0);
    text(ren, font, "Action", COLX0, ROW0 - 32, grey, 0);
    text(ren, font, "Touche 1", COLX[0], ROW0 - 32, grey, COLW);
    text(ren, font, "Touche 2", COLX[1], ROW0 - 32, grey, COLW);
    text(ren, font, "Manette 1", COLX[2], ROW0 - 32, grey, COLW);
    text(ren, font, "Manette 2", COLX[3], ROW0 - 32, grey, COLW);

    for (int a = 0; a < ACT_COUNT; a++) {
        int y = ROW0 + a * ROWH;
        text(ren, font, input_map_action_name(a), COLX0, y + 4, white, 0);
        for (int s = 0; s < IM_SLOTS; s++) {
            int waiting = (a == cap_act && s == cap_slot);
            box(ren, COLX[s], y, COLW, CELLH, waiting ? active : cell, edge);
            char name[96];
            if (waiting) snprintf(name, sizeof(name), "Appuyez...");
            else input_map_code_name(input_map_get(a, s), name, sizeof(name));
            text(ren, font, name, COLX[s], y + 4, waiting ? gold : white, COLW);
        }
    }

    char mouse[64];
    snprintf(mouse, sizeof(mouse), "Souris : %s", input_map_mouse_enabled() ? "active" : "d\xC3\xA9sactiv\xC3\xA9" "e");
    box(ren, MOUSE_X, BTN_Y, MOUSE_W, CELLH, cell, edge);
    text(ren, font, mouse, MOUSE_X, BTN_Y + 4, input_map_mouse_enabled() ? white : gold, MOUSE_W);
    box(ren, RESET_X, BTN_Y, BTN_W, CELLH, cell, edge);
    text(ren, font, "Par d\xC3\xA9" "faut", RESET_X, BTN_Y + 4, white, BTN_W);
    box(ren, CLOSE_X, BTN_Y, BTN_W, CELLH, cell, edge);
    text(ren, font, "Fermer", CLOSE_X, BTN_Y + 4, white, BTN_W);

    text(ren, font, cap_act >= 0
         ? "Touche, bouton ou stick de la manette ; \xC3\x89" "chap annule."
         : "Clic : assigner   -   clic droit : effacer   -   \xC3\x89" "chap : fermer",
         PX + 22, BTN_Y + 50, grey, 0);
}
