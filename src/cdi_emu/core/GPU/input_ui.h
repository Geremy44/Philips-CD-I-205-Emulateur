/*
 * input_ui.h - key / controller assignment panel, drawn over the video
 *
 * Opened from the menu (Controles > Assigner les touches...). Click a cell
 * then press a key or a controller button / stick direction; right click
 * clears it; Escape closes. The CD-i gets no input while it is open.
 */

#ifndef INPUT_UI_H
#define INPUT_UI_H

#include <SDL2/SDL.h>

void input_ui_open(void);
void input_ui_close(void);
int  input_ui_is_open(void);

/* Event while the panel is open (coordinates in the 768x576 logical
 * surface). Returns 1 when consumed. */
int  input_ui_event(const SDL_Event *e);

/* Draw the panel (logical 768x576 coordinates) */
void input_ui_render(SDL_Renderer *ren);

/* A TrueType font of that size (system fonts), NULL if none */
struct _TTF_Font;
struct _TTF_Font *input_ui_font(int size);

#endif
