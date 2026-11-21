/*
 * uart.c - Motorola 68070 on-chip UART emulation
 *
 * Emulates the UART peripheral registers and bridges them to the
 * SDL2 console window for host I/O. Output from the emulated firmware
 * (writes to the TX register) appears in the terminal window, while
 * keyboard input in the terminal is fed back through the RX register.
 */

#include "uart.h"
#include "../bus.h"
#include "uart_console.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
    #include <conio.h>
#else
    #include <sys/select.h>
    #include <unistd.h>
#endif

/* The I/O register shadow array is owned by bus.c and accessed
 * through this accessor function for registers we don't explicitly decode. */
extern uint8_t *uart_io_regs_ptr(void);

/* Initialize UART to idle state */
void uart_init(void) {
    bus_t *b = bus_get_global();
    if (!b) return;
    b->uart.mode     = 0x00;
    b->uart.rx_ready = 0;
    b->uart.rx_buf   = 0x00;
    b->uart.tx_buf   = 0x00;
    b->uart.clksel   = 0x00;
    b->uart.cmd      = 0x00;
}

/* Inject a byte from the host into the UART RX buffer */
void uart_feed(uint8_t byte) {
    bus_t *b = bus_get_global();
    if (!b) return;
    if (!b->uart.rx_ready) {
        b->uart.rx_buf   = byte;
        b->uart.rx_ready = 1;
    }
}

/* Direct injection into the bus UART RX buffer (used by slave HLE) */
void uart_inject_rx(bus_t *b, uint8_t c) {
    b->uart.rx_buf   = c;
    b->uart.rx_ready = 1;
}

/*
 * Poll host stdin for input (platform-specific, non-blocking).
 * On Windows uses _kbhit()/_getch(); on Unix uses select().
 */
void uart_poll_host_input(void) {
    bus_t *b = bus_get_global();
    if (!b) return;
    if (b->uart.rx_ready) return;

#ifdef _WIN32
    if (_kbhit()) {
        int c = _getch();
        if (c != EOF) {
            if (c == '\r') c = '\n';
            uart_feed((uint8_t)c);
        }
    }
#else
    fd_set rfds; struct timeval tv = {0, 0};
    FD_ZERO(&rfds); FD_SET(STDIN_FILENO, &rfds);
    if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == '\r') c = '\n';
            uart_feed(c);
        }
    }
#endif
}

/*
 * Handle a write to a UART register from the CPU bus.
 * TX output is forwarded to the SDL2 terminal display.
 */
void uart_write8(bus_t *b, uint32_t addr, uint8_t v) {
    uint32_t off = addr - UART_BASE;
    uint8_t *io_regs = uart_io_regs_ptr();

    switch (off) {

    case UART_MR_OFF:   /* Mode Register */
        b->uart.mode = v;
        break;

    case UART_SR_OFF:   /* Status Register (read-only, ignore writes) */
        break;

    case UART_CS_OFF:   /* Clock Select */
        b->uart.clksel = v;
        break;

    case UART_CR_OFF:   /* Command Register */
        b->uart.cmd = v;
        /* Decode standard UART commands */
        switch (v & 0xF0) {
            case 0x20: /* Reset RX */ b->uart.rx_ready = 0; break;
            case 0x30: /* Reset TX */ break;
            case 0x40: /* Reset error status */ break;
        }
        break;

    case UART_TX_OFF:   /* TX Holding Register - output to terminal */
        b->uart.tx_buf = v;
        uart_console_putchar((char)v);
        {   /* EMU_UART_LOG=1: copy the console output to stderr */
            static int log = -1;
            if (log < 0) { const char *s = getenv("EMU_UART_LOG"); log = (s && *s && *s != '0'); }
            if (log) fputc(v, stderr);
        }
        break;

    case UART_RX_OFF:   /* RX Holding Register (read-only) */
        break;

    default:
        /* Non-UART registers in the I/O space are shadowed */
        fprintf(stderr, "  -> Offset non-UART 0x%02X (shadow)\n", off);
        break;
    }

    io_regs[off] = v;
}

/*
 * Handle a read from a UART register.
 * The Status Register (USR) is dynamically constructed: TX is always
 * ready (HLE), and RX ready is set if the terminal has input available.
 */
uint8_t uart_read8(bus_t *b, uint32_t addr) {
    uint32_t off = addr - UART_BASE;
    uint8_t *io_regs = uart_io_regs_ptr();
    uint8_t val = io_regs[off];

    switch (off) {

    case UART_MR_OFF:   /* Mode Register */
        val = b->uart.mode;
        break;

    case UART_SR_OFF:   /* Status Register - dynamically constructed */
        val = USR_TXRDY | USR_TXEMT;         /* TX always ready in HLE */
        if (uart_console_has_input())
            val |= USR_RXRDY;                /* RX ready if input available */
        break;

    case UART_CS_OFF:   /* Clock Select */
        val = b->uart.clksel;
        break;

    case UART_CR_OFF:   /* Command Register */
        val = b->uart.cmd;
        break;

    case UART_RX_OFF:   /* RX Holding Register - read from terminal input */
        if (uart_console_has_input()) {
            val = uart_console_get_input();
        } else {
            val = 0x00;
        }
        break;

    default:
        fprintf(stderr, "  -> Offset non-UART 0x%02X (shadow)\n", off);
        break;
    }

    io_regs[off] = val;
    return val;
}

/* Wrapper for uart_inject_rx with void* bus pointer (used by slave backend) */
void uart_inject_rx_void(void *bus, uint8_t c) {
    uart_inject_rx((bus_t*)bus, c);
}
