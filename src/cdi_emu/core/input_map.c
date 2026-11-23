/*
 * input_map.c - keyboard / game controller bindings (see input_map.h)
 */

#include "input_map.h"
#include "options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int binding[ACT_COUNT][IM_SLOTS];
static int mouse_on = 1;
static char ini_path[1100];

#define MAX_PADS 4
static SDL_GameController *pads[MAX_PADS];
static SDL_Joystick *joys[MAX_PADS];          /* joysticks unknown to SDL's controller list */

static const char *HAT_DIR[4] = { "haut", "bas", "gauche", "droite" };

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
    if (code >= IM_JOY_HAT && code < IM_JOY_END) {
        snprintf(buf, n, "Croix %d %s", (code - IM_JOY_HAT) / 4 + 1, HAT_DIR[(code - IM_JOY_HAT) & 3]);
        return;
    }
    if (code >= IM_JOY_AXIS && code < IM_JOY_HAT) {
        snprintf(buf, n, "Axe %d %c", (code - IM_JOY_AXIS) / 2 + 1, ((code - IM_JOY_AXIS) & 1) ? '+' : '-');
        return;
    }
    if (code >= IM_JOY_BUTTON && code < IM_JOY_AXIS) { snprintf(buf, n, "Bouton %d", code - IM_JOY_BUTTON + 1); return; }
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
    else if (code >= IM_JOY_HAT && code < IM_JOY_END)
        snprintf(buf, n, "jhat:%d%s", (code - IM_JOY_HAT) / 4, HAT_DIR[(code - IM_JOY_HAT) & 3]);
    else if (code >= IM_JOY_AXIS && code < IM_JOY_HAT)
        snprintf(buf, n, "jaxis:%d%c", (code - IM_JOY_AXIS) / 2, ((code - IM_JOY_AXIS) & 1) ? '+' : '-');
    else if (code >= IM_JOY_BUTTON && code < IM_JOY_AXIS) snprintf(buf, n, "joy:%d", code - IM_JOY_BUTTON);
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
    if (!strncmp(v, "joy:", 4)) {
        int b = atoi(v + 4);
        return (b >= 0 && b < IM_JOY_AXIS - IM_JOY_BUTTON) ? IM_JOY_BUTTON + b : 0;
    }
    if (!strncmp(v, "jaxis:", 6)) {
        int a = atoi(v + 6);
        size_t l = strlen(v);
        return (a >= 0 && a < 50 && l > 6) ? IM_JOY_AXIS + 2 * a + (v[l - 1] == '+') : 0;
    }
    if (!strncmp(v, "jhat:", 5)) {
        int h = atoi(v + 5);
        for (int d = 0; d < 4; d++)
            if (strstr(v + 5, HAT_DIR[d]) && h >= 0 && h < 25) return IM_JOY_HAT + 4 * h + d;
        return 0;
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
    if (!SDL_IsGameController(index)) {             /* plain joystick */
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
        for (int i = 0; i < MAX_PADS; i++)
            if (joys[i] && SDL_JoystickInstanceID(joys[i]) == id) return;   /* already open */
        for (int i = 0; i < MAX_PADS; i++)
            if (!joys[i]) {
                joys[i] = SDL_JoystickOpen(index);
                if (joys[i]) fprintf(stderr, "[INPUT] joystick: %s (%d axes, %d buttons, %d hats)\n",
                                     SDL_JoystickName(joys[i]), SDL_JoystickNumAxes(joys[i]),
                                     SDL_JoystickNumButtons(joys[i]), SDL_JoystickNumHats(joys[i]));
                return;
            }
        return;
    }
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) return;
    for (int i = 0; i < MAX_PADS; i++)
        if (!pads[i]) {
            pads[i] = SDL_GameControllerOpen(index);
            if (pads[i]) fprintf(stderr, "[INPUT] controller: %s\n", SDL_GameControllerName(pads[i]));
            return;
        }
}

/* Events of a plain joystick (not of a controller, which sends both) */
static int is_plain_joy(SDL_JoystickID id)
{
    for (int i = 0; i < MAX_PADS; i++)
        if (joys[i] && SDL_JoystickInstanceID(joys[i]) == id) return 1;
    return 0;
}

int input_map_is_pad_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
    case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: case SDL_CONTROLLERAXISMOTION:
    case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED:
    case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: case SDL_JOYAXISMOTION: case SDL_JOYHATMOTION:
    case SDL_JOYBALLMOTION:
        return 1;
    }
    return 0;
}

void input_map_init(void)
{
    defaults();
    char *base = SDL_GetBasePath();
    snprintf(ini_path, sizeof(ini_path), "%scontroles.ini", base ? base : "");
    SDL_free(base);
    load();
    if (option_get("pads", 1)) input_map_set_pads(1);
    else fprintf(stderr, "[INPUT] controllers off (Controles menu)\n");
}

/* Controllers on / off (Controles menu, options.ini pads=0|1): off, SDL's
 * controller code is not even started, so a misbehaving pad cannot block */
static int pads_on = 0;
int input_map_pads_enabled(void) { return pads_on; }
void input_map_set_pads(int on)
{
    on = on != 0;
    if (option_get("pads", 1) != on) option_set("pads", on);
    if (on == pads_on) return;
    pads_on = on;
    if (on) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0)
            for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    } else {
        for (int i = 0; i < MAX_PADS; i++) {
            if (pads[i]) { SDL_GameControllerClose(pads[i]); pads[i] = NULL; }
            if (joys[i]) { SDL_JoystickClose(joys[i]); joys[i] = NULL; }
        }
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
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
    case SDL_JOYDEVICEADDED:
        if (pads_on) open_pad(e->jdevice.which);
        return 0;
    case SDL_JOYDEVICEREMOVED:
        for (int i = 0; i < MAX_PADS; i++)
            if (joys[i] && SDL_JoystickInstanceID(joys[i]) == e->jdevice.which) {
                SDL_JoystickClose(joys[i]);
                joys[i] = NULL;
            }
        return 0;
    case SDL_JOYBUTTONDOWN:
        return (capture && is_plain_joy(e->jbutton.which) && e->jbutton.button < IM_JOY_AXIS - IM_JOY_BUTTON)
               ? IM_JOY_BUTTON + e->jbutton.button : 0;
    case SDL_JOYAXISMOTION:
        if (capture && is_plain_joy(e->jaxis.which) && e->jaxis.axis < 50 &&
            (e->jaxis.value > AXIS_PRESS || e->jaxis.value < -AXIS_PRESS))
            return IM_JOY_AXIS + 2 * e->jaxis.axis + (e->jaxis.value > 0);
        return 0;
    case SDL_JOYHATMOTION:
        if (capture && is_plain_joy(e->jhat.which) && e->jhat.hat < 25 && e->jhat.value != SDL_HAT_CENTERED) {
            int d = (e->jhat.value & SDL_HAT_UP) ? 0 : (e->jhat.value & SDL_HAT_DOWN) ? 1 : (e->jhat.value & SDL_HAT_LEFT) ? 2 : 3;
            return IM_JOY_HAT + 4 * e->jhat.hat + d;
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
    const int dead = 8000;                        /* stick dead zone */
    /* plain joysticks: their own codes, and the controller inputs of the
     * default bindings (A/B/X/Y = buttons 1-4, left stick, D-pad = hat 1) */
    int jb = -1, ja = -1, jpos = 0, jh = -1, jd = 0;
    if (code >= IM_JOY_HAT && code < IM_JOY_END) { jh = (code - IM_JOY_HAT) / 4; jd = (code - IM_JOY_HAT) & 3; }
    else if (code >= IM_JOY_AXIS) { ja = (code - IM_JOY_AXIS) / 2; jpos = (code - IM_JOY_AXIS) & 1; }
    else if (code >= IM_JOY_BUTTON) jb = code - IM_JOY_BUTTON;
    else if (code < IM_PAD_AXIS) {
        int b = code - IM_PAD_BUTTON;
        if (b >= SDL_CONTROLLER_BUTTON_A && b <= SDL_CONTROLLER_BUTTON_Y) jb = b - SDL_CONTROLLER_BUTTON_A;
        else if (b >= SDL_CONTROLLER_BUTTON_DPAD_UP && b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT) { jh = 0; jd = b - SDL_CONTROLLER_BUTTON_DPAD_UP; }
    } else {
        int a = (code - IM_PAD_AXIS) / 2;
        if (a == SDL_CONTROLLER_AXIS_LEFTX || a == SDL_CONTROLLER_AXIS_LEFTY) { ja = a; jpos = (code - IM_PAD_AXIS) & 1; }
    }
    for (int i = 0; i < MAX_PADS; i++) {
        if (!joys[i]) continue;
        int v = 0;
        if (jb >= 0 && jb < SDL_JoystickNumButtons(joys[i])) v = SDL_JoystickGetButton(joys[i], jb) ? 1000 : 0;
        else if (ja >= 0 && ja < SDL_JoystickNumAxes(joys[i])) {
            int raw = SDL_JoystickGetAxis(joys[i], ja);
            if (!jpos) raw = -raw;
            v = raw > dead ? (int)((long)(raw - dead) * 1000 / (32767 - dead)) : 0;
        } else if (jh >= 0 && jh < SDL_JoystickNumHats(joys[i])) {
            static const Uint8 MASK[4] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
            v = (SDL_JoystickGetHat(joys[i], jh) & MASK[jd]) ? 1000 : 0;
        }
        if (v > best) best = v;
    }
    if (code >= IM_JOY_BUTTON) return best;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) continue;
        int v = 0;
        if (code < IM_PAD_AXIS) {
            v = SDL_GameControllerGetButton(pads[i], (SDL_GameControllerButton)(code - IM_PAD_BUTTON)) ? 1000 : 0;
        } else {
            int a = (code - IM_PAD_AXIS) / 2, pos = (code - IM_PAD_AXIS) & 1;
            int raw = SDL_GameControllerGetAxis(pads[i], (SDL_GameControllerAxis)a);
            if (!pos) raw = -raw;
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
