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

/* F3: statistics overlay. The emulator gives the text (one line per
 * newline); the display adds its own measured frame rate. */
/* Picture format: 0 = 4:3, 1 = 16:9 (stretched); saved in options.ini */
void video_display_set_aspect(int wide);
int  video_display_aspect_wide(void);

/* Video filter, saved in options.ini:
 * 0 none, 1 smooth (bilinear), 2 scanlines, 3 CRT, 4 sharp smooth, 5 edge smooth */
#define VIDEO_FILTERS 6
void video_display_set_filter(int f);
void video_display_next_filter(void);     /* F4: in menu order */
int  video_display_filter(void);
const char *video_display_filter_name(void);

void video_display_toggle_stats(void);
int  video_display_stats_on(void);
void video_display_set_stats(const char *text);

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
/* "CD-i 205 - <name of the loaded disc>" ("" or NULL = no disc) */
void video_display_set_disc_title(const char *disc);
/* Progress window with a bar (ripping, burning): created on the first call,
 * updated by the next ones, closed with pct < 0. Text in UTF-8. */
void video_display_progress(const char *title, const char *text, int pct);
void video_display_message(const char *title, const char *text);
/* Multi-selection file dialog (ROM dumps). Returns the number of files and
 * a malloc'd array of malloc'd UTF-8 paths in *out (0 if cancelled or not
 * available on this system). */
int  video_display_choose_files(const char *title, char ***out);
/* Show the PC cursor again (on exit) */
void video_display_restore_host_cursor(void);
/* pause screen: last picture darkened, "PAUSE" on it */
void video_display_redraw_paused(void);
/* CD-i hardware cursor enabled (the shell is on screen) */
int  video_display_cursor_on(void);

/* Menu bar of the video window (native on Windows). The callback gets the
 * command id; video_display_menu_check() sets an item's check mark. */
enum {
    VM_QUIT = 1001, VM_SAVE, VM_LOAD, VM_NEXT_SLOT, VM_OPEN_DISC, VM_EJECT, VM_RESET,
    VM_IMPORT_ROM, VM_CONVERT_CHD, VM_PAUSE,
    VM_FULLSCREEN = 1101, VM_HOST_CURSOR, VM_CONSOLE,
    VM_ASPECT_43, VM_ASPECT_169, VM_FILTER0, /* VM_FILTER0 + 0..5 */
    VM_FILTER_LAST = VM_FILTER0 + 5,
    VM_OVERSCAN,             /* Affichage > edges hidden like a TV */
    VM_SWAP = 1201, VM_INPUT_CONFIG, VM_MOUSE, VM_PADS,
    VM_FMV = 1401,           /* Materiel > Digital Video Cartridge */
    VM_FASTBOOT,             /* Materiel > fast boot (options.ini fastboot) */
    VM_DRIVE_PLAY = 1300,    /* + drive letter - 'A': play the disc in it */
    VM_DRIVE_RIP  = 1330,    /* + drive letter - 'A': rip it to .bin / .cue */
    VM_DRIVE_BURN = 1500,    /* + drive letter - 'A': burn an image on it */
    VM_DRIVE_BURN_TEST = 1530 /* + drive letter - 'A': same, test write */
};
typedef void (*video_menu_cb)(int id);
void video_display_set_menu_cb(video_menu_cb cb);
void video_display_menu_check(int id, int on);

/* Windows file dialog for a disc image; 1 + UTF-8 path in out if chosen */
int  video_display_choose_disc(char *out, int out_size);
/* Same with another dialog title (UTF-8) */
int  video_display_choose_image(const char *title, char *out, int out_size);
/* Yes / No box; 1 = yes */
int  video_display_confirm(const char *title, const char *text, const char *yes);

#endif /* VIDEO_DISPLAY_H */
