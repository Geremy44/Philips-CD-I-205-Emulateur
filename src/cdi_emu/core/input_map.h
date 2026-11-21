/*
 * input_map.h - keyboard / game controller bindings for the CD-i pointer
 *
 * Each CD-i action (pointer up/down/left/right, buttons 1 and 2, fast
 * move) has up to two keys and one game controller input (button, or a
 * stick / trigger direction). Bindings and the "mouse enabled" option are
 * kept in controles.ini next to the executable.
 */

#ifndef INPUT_MAP_H
#define INPUT_MAP_H

#include <SDL2/SDL.h>

enum {
    ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT,
    ACT_B1, ACT_B2, ACT_FAST,
    ACT_COUNT
};

#define IM_SLOTS 4                 /* key 1, key 2, controller 1, controller 2 */

/* Binding codes: 0 = none, 1..511 = SDL scancode,
 * 1000 + b = controller button b, 2000 + 2*axis + (positive) = axis */
#define IM_PAD_BUTTON 1000
#define IM_PAD_AXIS   2000

void        input_map_init(void);                 /* defaults, controles.ini, controllers */
const char *input_map_action_name(int act);       /* French label */
int         input_map_get(int act, int slot);
void        input_map_set(int act, int slot, int code);   /* also saves */
void        input_map_reset(void);                /* defaults, saved */
void        input_map_code_name(int code, char *buf, int n);

int         input_map_mouse_enabled(void);
void        input_map_set_mouse(int on);          /* saved */

/* SDL event: opens / closes controllers on hot-plug. While capturing
 * (assignment panel), returns the code of the key / button / axis just
 * pressed, else 0. */
int         input_map_event(const SDL_Event *e, int capture);

/* Once per video frame: pointer move (impulse units) and buttons
 * (bit 0 = CD-i button 1, bit 1 = button 2) from keys and controllers.
 * keyboard_focus = the video window has the keyboard. */
void        input_map_frame(int keyboard_focus, int *dx, int *dy, int *buttons);

#endif
