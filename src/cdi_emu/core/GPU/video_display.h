/*
 * video_display.h - CD-i video output display window
 *
 * Creates an SDL2 window that renders the CD-i's video output
 * by reading the framebuffer from emulated DRAM using the
 * SCC66470's Video Start Register (VSR) and display mode bits.
 *
 * The CD-i's normal display resolution is 384x280 (PAL).
 * The window is scaled 2x to 768x560 for visibility.
 */

#ifndef VIDEO_DISPLAY_H
#define VIDEO_DISPLAY_H

#include <stdint.h>

/* Global flag - set to 1 when the video display window is closed.
 * Checked by the main loop and uart_console event handler. */
extern int g_video_window_closed;

/* Initialize the video display window. Call once at startup. */
void video_display_init(void);

/*
 * Render one video frame from the emulated framebuffer and present it.
 *
 * scc66470: pointer to the video controller state (for VSR, CSR, CLUT)
 * bus: pointer to the memory bus (for reading framebuffer data)
 *
 * Returns 0 if the window was closed, 1 if still active.
 */
int video_display_update(void *scc66470, void *bus);

/* Get the SDL2 window ID (used by uart_console to distinguish close events) */
uint32_t video_display_get_window_id(void);

/* Pointer input: the video window turns mouse (and arrow keys + Space /
 * Enter / Backspace) into an absolute pointer position (0..1023) and
 * buttons (bit 0 = button 1, bit 1 = button 2), reported through cb. */
typedef void (*video_pointer_cb)(void *ctx, int x, int y, int buttons);
void video_display_set_pointer_cb(video_pointer_cb cb, void *ctx);

/* Called for every SDL event; returns 1 if the event belongs to the video
 * window and was consumed (opaque pointer to an SDL_Event). */
int video_display_handle_event(const void *sdl_event);

/* Flag in the buttons argument of the pointer callback: (x, y) are a
 * relative impulse (dx, dy) instead of an absolute position */
#define VIDEO_PTR_IMPULSE 0x100

/* Switch between window and full screen (F11, Alt+Enter) */
void video_display_toggle_fullscreen(void);

/* Show / hide the PC mouse cursor over the video window (F8) */
void video_display_toggle_host_cursor(void);

/* Called once per frame: relative = the game recentres the CD-i cursor
 * (68HC05 relative mode); the PC mouse is then captured in the window and
 * sent as impulses. cdi_x / cdi_y = CD-i cursor (pointer units), where
 * the PC mouse is put back when the capture ends. */
void video_display_pointer_mode(int relative, int cdi_x, int cdi_y);

/* Mouse buttons: 1 = left click is CD-i button 2 (F7). Keys and
 * controllers follow their own assignment (Controles menu). */
void video_display_set_swap(int swap);

/* Reset the V.S.D. (palette, coding, weights...) like the hardware reset */
void video_display_reset_vsd(void);

/* Window title (NULL = the normal one) and a message box */
void video_display_set_title(const char *title);
void video_display_message(const char *title, const char *text);
/* Show the PC cursor again (on exit) */
void video_display_restore_host_cursor(void);

/* Menu bar of the video window (native on Windows). The callback gets the
 * command id; video_display_menu_check() sets an item's check mark. */
enum {
    VM_QUIT = 1001, VM_SAVE, VM_LOAD, VM_NEXT_SLOT, VM_OPEN_DISC, VM_EJECT, VM_RESET,
    VM_FULLSCREEN = 1101, VM_HOST_CURSOR, VM_CONSOLE,
    VM_SWAP = 1201, VM_INPUT_CONFIG, VM_MOUSE,
    VM_FMV = 1401,           /* Materiel > Digital Video Cartridge */
    VM_DRIVE_PLAY = 1300,    /* + drive letter - 'A': play the disc in it */
    VM_DRIVE_RIP  = 1330     /* + drive letter - 'A': rip it to .bin / .cue */
};
typedef void (*video_menu_cb)(int id);
void video_display_set_menu_cb(video_menu_cb cb);
void video_display_menu_check(int id, int on);

/* Windows file dialog for a disc image; 1 + UTF-8 path in out if chosen */
int  video_display_choose_disc(char *out, int out_size);

#endif /* VIDEO_DISPLAY_H */
