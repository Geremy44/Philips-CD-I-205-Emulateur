#ifndef UART_H
#define UART_H

#include <stdint.h>

/* ===== Adresses absolues UART 68070 ===== */
#define UART_BASE   0x80002010
#define UART_SIZE   0x10

/* ===== Offsets relatifs à UART_BASE ===== */
#define UART_MR_OFF  0x00   /* Mode Register       */
#define UART_SR_OFF  0x01   /* Status Register (RO) */
#define UART_CR_OFF  0x02   /* Command Register    */
#define UART_DR_OFF  0x03   /* Data Register (TX/RX) */

/* Pour compatibilité avec les anciens noms si besoin */
#define UART_UMR  UART_MR_OFF
#define UART_USR  UART_SR_OFF
#define UART_UCR  UART_CR_OFF
#define UART_UTH  UART_DR_OFF
#define UART_URX  UART_DR_OFF

/* ===== UART state ===== */
typedef struct uart_s {
    uint8_t rx_buf;    /* last RX byte received */
    uint8_t rx_ready;  /* 1 = rx byte available (bit 0 of USR) */
    uint8_t tx_buf;    /* last TX byte */
    uint8_t mode;      /* UMR register snapshot */
    uint8_t cmd;       /* UCR register snapshot */
    uint8_t clksel;    /* UCS register snapshot */
} uart_t;


struct bus;  /* forward decl */

void    uart_init(void);
void    uart_feed(uint8_t byte);
void    uart_poll_host_input(void);
void    uart_inject_rx(struct bus *b, uint8_t c);

uint8_t uart_read8(struct bus *b, uint32_t addr);
void    uart_write8(struct bus *b, uint32_t addr, uint8_t v);

#endif /* UART_H */