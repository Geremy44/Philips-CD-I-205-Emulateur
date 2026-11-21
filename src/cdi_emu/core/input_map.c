/*
 * input_map.c - keyboard / game controller bindings (see input_map.h)
 */

#include "input_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int binding[ACT_COUNT][IM_SLOTS];
static int mouse_on = 1;
static char ini_path[1100];

#define MAX_PADS 4
static SDL_GameController *pads[MAX_PADS];

static const char *ACT_NAME[ACT_COUNT] = {
    "Haut", "Bas", "Gauche", "Droite", "Bouton 1", "Bouton 2", "Rapide"
};
static const char *ACT_KEY[ACT_COUNT] = {   /* names in controles.ini */
    "haut", "bas", "gauche", "droite", "bouton1", "bouton2", "rapide"
};

const char *input_map_action_name(int act) { return (act >= 0 && act < ACT_COUNT) ? ACT_NAME[act] : "?"; }
int  input_map_get(int act, int slot)      { return binding[act][slot]; }
int  input_map_mouse_enabled(void)         { return mouse_on; }

static void defaults(void)
{
    #define B(x) (IM_PAD_BUTTON + SDL_CONTROLLER_BUTTON_##x)
    #define AX(x, pos) (IM_PAD_AXIS + 2 * SDL_CONTROLLER_AXIS_##x + (pos))
    static const int def[ACT_COUNT][IM_SLOTS] = {
        { SDL_SCANCODE_UP,        SDL_SCANCODE_KP_8,   B(DPAD_UP),    AX(LEFTY, 0) },
        { SDL_SCANCODE_DOWN,      SDL_SCANCODE_KP_2,   B(DPAD_DOWN),  AX(LEFTY, 1) },
        { SDL_SCANCODE_LEFT,      SDL_SCANCODE_KP_4,   B(DPAD_LEFT),  AX(LEFTX, 0) },
        { SDL_SCANCODE_RIGHT,     SDL_SCANCODE_KP_6,   B(DPAD_RIGHT), AX(LEFTX, 1) },
        { SDL_SCANCODE_SPACE,     SDL_SCANCODE_RETURN, B(A),          B(X) },
        { SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RCTRL,  B(B),          B(Y) },
        { SDL_SCANCODE_LSHIFT,    SDL_SCANCODE_RSHIFT, B(RIGHTSHOULDER), AX(TRIGGERRIGHT, 1) },
    };
    #undef B
    #undef AX
    memcpy(binding, def, sizeof(binding));
}

/* ---- code <-> text ---- */

static const char *axis_name(int axis)
{
    const char *s = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)axis);
    return s ? s : "axe";
}

/* Readable names of controller inputs */
static const char *pad_button_name(int b)
{
    static const char *n[] = { "A", "B", "X", "Y", "Back", "Guide", "Start",
        "Stick G clic", "Stick D clic", "LB", "RB",
        "Croix haut", "Croix bas", "Croix gauche", "Croix droite" };
    if (b >= 0 && b < (int)(sizeof(n) / sizeof(n[0]))) return n[b];
    const char *s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
    return s ? s : "?";
}

static const char *pad_axis_name(int a, int pos)
{
    switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX:  return pos ? "Stick G droite" : "Stick G gauche";
    case SDL_CONTROLLER_AXIS_LEFTY:  return pos ? "Stick G bas" : "Stick G haut";
    case SDL_CONTROLLER_AXIS_RIGHTX: return pos ? "Stick D droite" : "Stick D gauche";
    case SDL_CONTROLLER_AXIS_RIGHTY: return pos ? "Stick D bas" : "Stick D haut";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return "LT";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "RT";
    }
    return "axe";
}

void input_map_code_name(int code, char *buf, int n)
{
    if (code <= 0) { snprintf(buf, n, "-"); return; }
    if (code < IM_PAD_BUTTON) {
        const char *s = SDL_GetScancodeName((SDL_Scancode)code);
        snprintf(buf, n, "%s", (s && *s) ? s : "?");
    } else if (code < IM_PAD_AXIS) {
        snprintf(buf, n, "%s", pad_button_name(code - IM_PAD_BUTTON));
    } else {
        int a = (code - IM_PAD_AXIS) / 2, pos = (code - IM_PAD_AXIS) & 1;
        snprintf(buf, n, "%s", pad_axis_name(a, pos));
    }
}

/* ini values: "key:Up", "pad:a", "axis:leftx-", "-" */
static void code_to_ini(int code, char *buf, int n)
{
    if (code <= 0) snprintf(buf, n, "-");
    else if (code < IM_PAD_BUTTON) snprintf(buf, n, "key:%s", SDL_GetScancodeName((SDL_Scancode)code));
    else if (code < IM_PAD_AXIS) {
        const char *s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)(code - IM_PAD_BUTTON));
        snprintf(buf, n, "pad:%s", s ? s : "?");
    } else {
        int a = (code - IM_PAD_AXIS) / 2, pos = (code - IM_PAD_AXIS) & 1;
        snprintf(buf, n, "axis:%s%c", axis_name(a), pos ? '+' : '-');
    }
}

static int ini_to_code(const char *v)
{
    if (!strncmp(v, "key:", 4)) {
        SDL_Scancode s = SDL_GetScancodeFromName(v + 4);
        return s != SDL_SCANCODE_UNKNOWN ? (int)s : 0;
    }
    if (!strncmp(v, "pad:", 4)) {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(v + 4);
        return b != SDL_CONTROLLER_BUTTON_INVALID ? IM_PAD_BUTTON + (int)b : 0;
    }
    if (!strncmp(v, "axis:", 5)) {
        char name[64];
        snprintf(name, sizeof(name), "%s", v + 5);
        size_t l = strlen(name);
        if (l < 2) return 0;
        int pos = name[l - 1] == '+';
        name[l - 1] = 0;
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name);
        return a != SDL_CONTROLLER_AXIS_INVALID ? IM_PAD_AXIS + 2 * (int)a + pos : 0;
    }
    return 0;
}

/* ---- controles.ini ---- */

static void save(void)
{
    if (!ini_path[0]) return;
    FILE *f = fopen(ini_path, "w");
    if (!f) return;
    fprintf(f, "; CD-i 205 - assignation des touches (menu Controles)\n");
    fprintf(f, "; valeurs : key:<touche SDL>, pad:<bouton>, axis:<axe>+/-, ou -\n");
    fprintf(f, "souris=%d\n", mouse_on);
    for (int a = 0; a < ACT_COUNT; a++) {
        fprintf(f, "%s=", ACT_KEY[a]);
        for (int s = 0; s < IM_SLOTS; s++) {
            char v[96];
            code_to_ini(binding[a][s], v, sizeof(v));
            fprintf(f, "%s%s", s ? "," : "", v);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

static void load(void)
{
    FILE *f = fopen(ini_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == ';' || !eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "souris")) { mouse_on = atoi(v) != 0; continue; }
        for (int a = 0; a < ACT_COUNT; a++) {
            if (strcmp(k, ACT_KEY[a])) continue;
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", v);
            int s = 0;
            for (char *tok = strtok(tmp, ","); tok && s < IM_SLOTS; tok = strtok(NULL, ","), s++)
                binding[a][s] = ini_to_code(tok);
        }
    }
    fclose(f);
}

void input_map_set(int act, int slot, int code)
{
    if (act < 0 || act >= ACT_COUNT || slot < 0 || slot >= IM_SLOTS) return;
    /* one input drives one action: remove it elsewhere */
    if (code > 0)
        for (int a = 0; a < ACT_COUNT; a++)
            for (int s = 0; s < IM_SLOTS; s++)
                if (binding[a][s] == code) binding[a][s] = 0;
    binding[act][slot] = code;
    save();
}

void input_map_reset(void)       { defaults(); save(); }
void input_map_set_mouse(int on) { mouse_on = on != 0; save(); }

/* ---- controllers ---- */

static void open_pad(int index)
{
    if (!SDL_IsGameController(index)) return;
    for (int i = 0; i < MAX_PADS; i++)
        if (!pads[i]) {
            pads[i] = SDL_GameControllerOpen(index);
            if (pads[i]) fprintf(stderr, "[INPUT] controller: %s\n", SDL_GameControllerName(pads[i]));
            return;
        }
}

void input_map_init(void)
{
    defaults();
    char *base = SDL_GetBasePath();
    snprintf(ini_path, sizeof(ini_path), "%scontroles.ini", base ? base : "");
    SDL_free(base);
    load();
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0)
        for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
}

#define AXIS_PRESS 16000             /* a stick / trigger counts as pressed */

int input_map_event(const SDL_Event *e, int capture)
{
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED:
        open_pad(e->cdevice.which);
        return 0;
    case SDL_CONTROLLERDEVICEREMOVED:
        for (int i = 0; i < MAX_PADS; i++)
            if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e->cdevice.which) {
                SDL_GameControllerClose(pads[i]);
                pads[i] = NULL;
            }
        return 0;
    case SDL_KEYDOWN:
        return (capture && !e->key.repeat) ? (int)e->key.keysym.scancode : 0;
    case SDL_CONTROLLERBUTTONDOWN:
        return capture ? IM_PAD_BUTTON + e->cbutton.button : 0;
    case SDL_CONTROLLERAXISMOTION:
        if (capture && (e->caxis.value > AXIS_PRESS || e->caxis.value < -AXIS_PRESS))
            return IM_PAD_AXIS + 2 * e->caxis.axis + (e->caxis.value > 0);
        return 0;
    }
    return 0;
}

/* How much an input is pressed: 0..1000 (analog for sticks) */
static int input_level(int code, const Uint8 *keys)
{
    if (code <= 0) return 0;
    if (code < IM_PAD_BUTTON) return (keys && code < 512 && keys[code]) ? 1000 : 0;
    int best = 0;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) continue;
        int v = 0;
        if (code < IM_PAD_AXIS) {
            v = SDL_GameControllerGetButton(pads[i], (SDL_GameControllerButton)(code - IM_PAD_BUTTON)) ? 1000 : 0;
        } else {
            int a = (code - IM_PAD_AXIS) / 2, pos = (code - IM_PAD_AXIS) & 1;
            int raw = SDL_GameControllerGetAxis(pads[i], (SDL_GameControllerAxis)a);
            if (!pos) raw = -raw;
            const int dead = 8000;                /* stick dead zone */
            v = raw > dead ? (int)((long)(raw - dead) * 1000 / (32767 - dead)) : 0;
        }
        if (v > best) best = v;
    }
    return best;
}

static int action_level(int act, const Uint8 *keys)
{
    int best = 0;
    for (int s = 0; s < IM_SLOTS; s++) {
        int v = input_level(binding[act][s], keys);
        if (v > best) best = v;
    }
    return best;
}

/* Movement: a press gives one impulse at once (games that read the
 * pointer as a joystick need it), holding it moves on after 12 frames;
 * analog sticks move in proportion to the deflection. */
#define IMPULSE      24
#define HOLD_FRAMES  12
#define HOLD_SPEED   10              /* units per frame while held */

void input_map_frame(int keyboard_focus, int *dx, int *dy, int *buttons)
{
    static int held[4], acc[2];
    const Uint8 *keys = keyboard_focus ? SDL_GetKeyboardState(NULL) : NULL;
    int fast = action_level(ACT_FAST, keys) > 500 ? 3 : 1;
    int mv[2] = { 0, 0 };
    for (int d = 0; d < 4; d++) {                 /* up, down, left, right */
        int lvl = action_level(ACT_UP + d, keys);
        int axis = d < 2 ? 1 : 0, sign = (d & 1) ? 1 : -1;
        if (lvl <= 0) { held[d] = 0; continue; }
        if (lvl >= 1000) {                        /* digital (or stick at full tilt) */
            if (held[d] == 0) mv[axis] += sign * IMPULSE * fast;
            else if (held[d] >= HOLD_FRAMES) mv[axis] += sign * HOLD_SPEED * fast;
        } else {                                  /* analog */
            acc[axis] += sign * lvl * HOLD_SPEED * fast;
        }
        held[d]++;
    }
    for (int a = 0; a < 2; a++) {                 /* analog remainder, in 1/1000 units */
        mv[a] += acc[a] / 1000;
        acc[a] %= 1000;
    }
    *dx = mv[0];
    *dy = mv[1];
    *buttons = (action_level(ACT_B1, keys) > 500 ? 1 : 0) | (action_level(ACT_B2, keys) > 500 ? 2 : 0);
}
