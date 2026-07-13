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

void uart_write8(bus_t *b, uint32_t addr, uint8_t v) {
    uint32_t off = addr - UART_BASE;  /* <-- CHANGEMENT ! */
    uint8_t *io_regs = uart_io_regs_ptr();

    fprintf(stderr, "[UART WR] addr=0x%08X off=0x%02X val=0x%02X\n", addr, off, v);

    switch (off) {

    case UART_MR_OFF:   /* 0x80002010 — Mode Register */
        b->uart.mode = v;
        fprintf(stderr, "  -> Mode Register = 0x%02X\n", v);
        break;

    case UART_SR_OFF:   /* 0x80002011 — Status Register (normalement RO, mais firmware peut l'utiliser comme Clock) */
        fprintf(stderr, "  -> Clock/Status = 0x%02X\n", v);
        break;

    case UART_CR_OFF:   /* 0x80002012 — Command Register */
        b->uart.cmd = v;
        fprintf(stderr, "  -> Command Register = 0x%02X\n", v);
        break;

    case UART_DR_OFF:   /* 0x80002013 — Data Register (TX) */
        b->uart.tx_buf = v;
        uart_console_putchar((char)v);
        fprintf(stderr, "  -> Data TX = 0x%02X ('%c')\n",
                v, (v >= 32 && v < 127) ? v : '.');
        break;

    default:
        fprintf(stderr, "  -> Offset inconnu 0x%02X\n", off);
        break;
    }
    
    io_regs[off] = v;
}

uint8_t uart_read8(bus_t *b, uint32_t addr) {
    uint32_t off = addr - UART_BASE;  /* <-- CHANGEMENT ! */
    uint8_t *io_regs = uart_io_regs_ptr();
    uint8_t val = io_regs[off];

    fprintf(stderr, "[UART RD] addr=0x%08X off=0x%02X\n", addr, off);

    switch (off) {

    case UART_MR_OFF:   /* Mode Register */
        val = b->uart.mode;
        fprintf(stderr, "  -> Mode = 0x%02X\n", val);
        break;

    case UART_SR_OFF: { /* Status Register */
        val = 0x00;
        if (uart_console_has_input()) val |= 0x01;  /* RxRDY */
        val |= 0x04;  /* TxRDY (toujours prêt) */
        val |= 0x08;  /* TxEMT (toujours vide en HLE) */
        fprintf(stderr, "  -> Status = 0x%02X (RxRDY=%d, TxRDY=1, TxEMT=1)\n",
                val, val & 0x01);
        break;
    }

    case UART_CR_OFF:   /* Command Register */
        val = b->uart.cmd;
        fprintf(stderr, "  -> Command = 0x%02X\n", val);
        break;

    case UART_DR_OFF:   /* Data Register (RX) */
        if (uart_console_has_input()) {
            val = uart_console_get_input();
            fprintf(stderr, "  -> Data RX = 0x%02X ('%c')\n",
                    val, (val >= 32 && val < 127) ? val : '.');
        } else {
            val = b->uart.rx_buf;
            fprintf(stderr, "  -> Data RX (vide) = 0x%02X\n", val);
        }
        break;

    default:
        fprintf(stderr, "  -> Offset inconnu 0x%02X\n", off);
        break;
    }

    io_regs[off] = val;
    return val;
}