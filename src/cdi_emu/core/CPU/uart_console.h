/*
 * uart_console.h - SDL2-based UART terminal window
 *
 * Provides a visual 80x25 text terminal that displays output
 * from the emulated UART and captures keyboard input to feed
 * back into the UART RX buffer.
 */

#ifndef UART_CONSOLE_H
#define UART_CONSOLE_H

#include <stdint.h>

/* Create the SDL2 window and renderer. Call once at startup. */
void uart_console_init(void);

/* Send a character to the terminal display (called from uart_write8) */
void uart_console_putchar(char c);

/*
 * Process SDL events (input + rendering). Call periodically
 * from the main emulation loop.
 * Returns 0 if the user closed the window (SDL_QUIT), 1 otherwise.
 */
int uart_console_update(void);

/* Emulator hotkeys (F1..F12), active in every window. The callback gets
 * the function key number (5 for F5...) and returns 1 if it used it. */
typedef int (*uart_hotkey_cb)(int fkey);
void uart_console_set_hotkey_cb(uart_hotkey_cb cb);

/* Check if a keyboard character is available in the RX buffer */
int uart_console_has_input(void);

/* Retrieve the next character from the RX buffer */
uint8_t uart_console_get_input(void);

#endif /* UART_CONSOLE_H */
