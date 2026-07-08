#ifndef UART_H
#define UART_H

#include <stdint.h>

/* ===== UART register offsets (base 0x80002000) ===== */
#define UART_UMR  0x11   /* Mode Register       (R/W) */
#define UART_USR  0x13   /* Status Register     (RO)  */
#define UART_UCS  0x15   /* Clock Select        (R/W) */
#define UART_UCR  0x17   /* Command Register    (WO)  */
#define UART_UTH  0x19   /* Transmit Holding    (WO)  */
#define UART_URX  0x1B   /* Receive Holding     (RO)  */

/* ===== UART state ===== */
typedef struct uart_s {
    uint8_t rx_buf;    /* last RX byte received */
    uint8_t rx_ready;  /* 1 = rx byte available (bit 0 of USR) */
    uint8_t tx_buf;    /* last TX byte */
    uint8_t mode;      /* UMR register snapshot */
    uint8_t cmd;       /* UCR register snapshot */
    uint8_t clksel;    /* UCS register snapshot */
} uart_t;

/* Forward declaration matching bus.h's real struct tag ("struct bus"),
   to avoid circular include. Do NOT use "struct bus_s" — it doesn't exist. */
struct bus;

/* ===== UART public API ===== */
void    uart_init(void);
void    uart_feed(uint8_t byte);
void    uart_poll_host_input(void);
void    uart_inject_rx(struct bus *b, uint8_t c);

/* Called by bus.c dispatch */
uint8_t uart_read8(struct bus *b, uint32_t addr);
void    uart_write8(struct bus *b, uint32_t addr, uint8_t v);

#endif /* UART_H */