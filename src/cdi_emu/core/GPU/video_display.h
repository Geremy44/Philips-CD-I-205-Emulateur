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

#endif /* VIDEO_DISPLAY_H */
