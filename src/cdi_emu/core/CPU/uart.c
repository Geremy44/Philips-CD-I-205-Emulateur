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

/* io_regs is owned by bus.c; declared there as static — so UART keeps
   its own shadow copy of the raw register bytes it doesn't decode. */
extern uint8_t *uart_io_regs_ptr(void); /* accessor provided by bus.c */

void uart_init(void) {
    bus_t *b = bus_get_global();
    if (!b) return;
    b->uart.mode     = 0x00;
    b->uart.rx_ready = 0;      /* pas de données au depart */
    b->uart.rx_buf   = 0x00;
    b->uart.tx_buf   = 0x00;
    b->uart.clksel   = 0x00;
    b->uart.cmd      = 0x00;
}

/* Inject a byte from the host into the UART RX buffer. */
void uart_feed(uint8_t byte) {
    bus_t *b = bus_get_global();
    if (!b) return;
    if (!b->uart.rx_ready) {
        b->uart.rx_buf   = byte;
        b->uart.rx_ready = 1;
    }
}

void uart_inject_rx(bus_t *b, uint8_t c) {
    if (!b) return;
    b->uart.rx_buf   = c;
    b->uart.rx_ready = 1;
    /* fprintf(stderr, "[UART INJECT] octet %02X ('%c') injecte\n",
       c, (c >= 0x20 && c < 0x7F) ? c : '.'); */
}

/* - Windows : _kbhit() / _getch() - */
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

/* ================================================================
 * UART read handler (8-bit only, base 0x80002000)
 *   0x11 UMR | 0x13 USR(RO) | 0x15 UCS | 0x17 UCR
 *   0x19 UTH(WO) | 0x1B URX(RO) | 0x1D UIPCR(RO)
 * ================================================================ */
// uint8_t uart_read8(bus_t *b, uint32_t addr) {
//     uint32_t off = addr & 0xFF;
//     uint8_t *io_regs = uart_io_regs_ptr();

//     fprintf(stderr, "[UART_READ8] addr=%08X off=%02X\n", addr, off);

//     switch (off) {

//     case UART_UMR:   /* 0x11 — Mode Register (lecture) */
//         fprintf(stderr, "[UMR RD] mode=%02X (bit0=%d)\n",
//                 b->uart.mode, b->uart.mode & 1);
//         return b->uart.mode;

//     case UART_USR: { /* 0x13 — Status Register */
//         uint8_t usr = 0x04;                  /* bit2 TxRDY = toujours pret */
//         if (b->uart.rx_ready) usr |= 0x01;   /* bit0 RxRDY = donnees dispo */
//         fprintf(stderr, "[USR RD] usr=%02X (bit0=%d)\n", usr, usr & 1);
//         return usr;
//     }

//     case UART_UCS:   /* 0x15 */
//         return b->uart.clksel;

//     case UART_UCR:   /* 0x17 — command, generalement WO */
//         return b->uart.cmd;

//     case UART_URX:   /* 0x1B — RX Holding */
//         b->uart.rx_ready = 0;
//         return b->uart.rx_buf;

//     default:
//         return io_regs[off];
//     }
// }
uint8_t uart_read8(bus_t *b, uint32_t addr) {
    uint32_t off = addr & 0xFF;
    uint8_t *io_regs = uart_io_regs_ptr();
    uint8_t val = io_regs[off];

    switch (off) {

    case UART_USR: { /* 0x13 — status register */
        val = 0x00;
        if (uart_console_has_input()) val |= 0x01; /* RXRDY */
        val |= 0x04; /* TXRDY toujours prêt */
        val |= 0x08; /* TXEMT toujours vide */
        break;
    }

    case UART_UTH: /* 0x19 — registre partagé, lu ici comme RX */
        if (uart_console_has_input()) {
            b->uart.rx_buf = uart_console_get_input();
        }
        val = b->uart.rx_buf;
        break;

    default:
        break;
    }

    io_regs[off] = val;
    return val;
}

/* ================================================================
 * UART write handler (8-bit only)
 * ================================================================ */
// void uart_write8(bus_t *b, uint32_t addr, uint8_t v) {
//     uint32_t off = addr & 0xFF;
//     uint8_t *io_regs = uart_io_regs_ptr();

//     switch (off) {

//     case UART_UTH:   /* 0x19 — TX : affiche le caractere sur stdout */
//         b->uart.tx_buf = v;
//         putchar((char)v);
//         fflush(stdout);
//         return;

//     case UART_UCR:   /* 0x17 */
//         b->uart.cmd = v;
//         break;

//     case UART_UMR:   /* 0x11 */
//         fprintf(stderr, "[UMR WR] = %02X\n", v);
//         b->uart.mode = v;
//         break;

//     case UART_UCS:   /* 0x15 */
//         b->uart.clksel = v;
//         break;
//     }
//     io_regs[off] = v;
// }
void uart_write8(bus_t *b, uint32_t addr, uint8_t v) {
    uint32_t off = addr & 0xFF;
    uint8_t *io_regs = uart_io_regs_ptr();

    switch (off) {

    case UART_UTH:   /* 0x19 — TX */
        b->uart.tx_buf = v;
        uart_console_putchar((char)v);   /* <-- remplace putchar()/fflush() */
        return;

    case UART_UCR:   /* 0x17 */
        b->uart.cmd = v;
        break;

    case UART_UMR:   /* 0x11 */
        b->uart.mode = v;
        break;

    case UART_UCS:   /* 0x15 */
        b->uart.clksel = v;
        break;
    }
    io_regs[off] = v;
}