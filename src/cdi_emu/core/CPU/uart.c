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
    b->uart.rx_buf   = c;
    b->uart.rx_ready = 1;
    //fprintf(stderr, "[UART] inject RX = 0x%02X (rx_ready=1)\n", c);
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

void uart_write8(bus_t *b, uint32_t addr, uint8_t v) {
    uint32_t off = addr - UART_BASE;   /* base = 0x80002000 */
    uint8_t *io_regs = uart_io_regs_ptr();

    //fprintf(stderr, "[UART WR] addr=0x%08X off=0x%02X val=0x%02X\n", addr, off, v);

    switch (off) {

    case UART_MR_OFF:   /* 0x11 UMR */
        b->uart.mode = v;
        //fprintf(stderr, "  -> UMR (Mode) = 0x%02X\n", v);
        break;

    case UART_SR_OFF:   /* 0x13 USR — RO, mais certains firmwares écrivent : on ignore */
        //fprintf(stderr, "  -> USR write ignore (RO) val=0x%02X\n", v);
        break;

    case UART_CS_OFF:   /* 0x15 UCS Clock Select */
        b->uart.clksel = v;
        //fprintf(stderr, "  -> UCS (Clock Select) = 0x%02X\n", v);
        break;

    case UART_CR_OFF:   /* 0x17 UCR Command */
        b->uart.cmd = v;
        /* Décodage commandes standard (reset RX/TX etc.) */
        switch (v & 0xF0) {
            case 0x20: /* reset RX pointer */ b->uart.rx_ready = 0; break;
            case 0x30: /* reset TX pointer */ break;
            case 0x40: /* reset error status */ break;
        }
        //fprintf(stderr, "  -> UCR (Command) = 0x%02X\n", v);
        break;

    case UART_TX_OFF:   /* 0x19 UTHR — TX */
        b->uart.tx_buf = v;
        uart_console_putchar((char)v);
        // fprintf(stderr, "  -> UTHR TX = 0x%02X ('%c')\n",
        //         v, (v >= 32 && v < 127) ? v : '.');
        break;

    case UART_RX_OFF:   /* 0x1B URHR — RO */
        //fprintf(stderr, "  -> URHR write ignore (RO)\n");
        break;

    default:
        /* Registres hors UART pur (PICR1/2 @0x45/0x47, Timer @0x20+) : shadow */
        fprintf(stderr, "  -> Offset non-UART 0x%02X (shadow)\n", off);
        break;
    }

    io_regs[off] = v;
}

uint8_t uart_read8(bus_t *b, uint32_t addr) {
    uint32_t off = addr - UART_BASE;
    uint8_t *io_regs = uart_io_regs_ptr();
    uint8_t val = io_regs[off];

    //fprintf(stderr, "[UART RD] addr=0x%08X off=0x%02X\n", addr, off);

    switch (off) {

    case UART_MR_OFF:
        val = b->uart.mode;
        //fprintf(stderr, "  -> UMR (Mode) = 0x%02X\n", val);
        break;

    case UART_SR_OFF: {   /* USR — construit dynamiquement */
        // fprintf(stderr, "[USR READ] rx_ready=%d has_input=%d\n",
        //     b->uart.rx_ready, uart_console_has_input());
        val = USR_TXRDY | USR_TXEMT;         /* TX toujours prêt en HLE */
        if (uart_console_has_input())
            val |= USR_RXRDY;                /* RX prêt si input dispo  */
        // fprintf(stderr, "  -> USR = 0x%02X (RxRDY=%d TxRDY=1 TxEMT=1)\n",
        //         val, (val & USR_RXRDY) ? 1 : 0);
        break;
    }

    case UART_CS_OFF:
        val = b->uart.clksel;
        //fprintf(stderr, "  -> UCS = 0x%02X\n", val);
        break;

    case UART_CR_OFF:
        val = b->uart.cmd;
        //fprintf(stderr, "  -> UCR = 0x%02X\n", val);
        break;

    case UART_RX_OFF: {   /* URHR — lecture data RX */
        if (uart_console_has_input()) {
            val = uart_console_get_input();
            //fprintf(stderr, "  -> URHR RX = 0x%02X ('%c')\n",
            //        val, (val >= 32 && val < 127) ? val : '.');
        } else {
            val = 0x00;   /* rien à lire : pas de faux caractère */
            //fprintf(stderr, "  -> URHR RX (vide) = 0x00\n");
        }
        break;
    }

    default:
        fprintf(stderr, "  -> Offset non-UART 0x%02X (shadow)\n", off);
        break;
    }

    io_regs[off] = val;
    return val;
}

void uart_inject_rx_void(void *bus, uint8_t c) {
    uart_inject_rx((bus_t*)bus, c);
}