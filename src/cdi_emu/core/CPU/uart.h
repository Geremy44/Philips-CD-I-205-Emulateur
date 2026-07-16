#ifndef UART_H
#define UART_H

#include <stdint.h>

/* ===== Base UART 68070 =====
   Les registres sont mappés sur les adresses IMPAIRES à partir de 0x80002011.
   On garde une base à 0x80002000 et on décode les offsets absolus. */
#define UART_BASE  0x80002000u
#define UART_SIZE  0x00000020u   /* 0x80002010 .. 0x8000201F */
#define UART_END    0x8000201F

/* ===== Offsets ABSOLUS (addr - 0x80002000) — datasheet p.67 ===== */
#define UART_MR_OFF   0x11   /* UMR  - Mode Register        */
#define UART_SR_OFF   0x13   /* USR  - Status Register (RO) */
#define UART_CS_OFF   0x15   /* UCS  - Clock Select         */
#define UART_CR_OFF   0x17   /* UCR  - Command Register     */
#define UART_TX_OFF   0x19   /* UTHR - TX Holding Register  */
#define UART_RX_OFF   0x1B   /* URHR - RX Holding Register  */

/* ===== UART Status Register bits (Fig.41) ===== */
#define USR_RXRDY     0x01   /* bit0 : RX ready            */
#define USR_TXRDY     0x04   /* bit2 : TX ready            */
#define USR_TXEMT     0x08   /* bit3 : TX empty            */
#define USR_OVERRUN   0x10   /* bit4 : overrun error       */
#define USR_PARITY    0x20   /* bit5 : parity error        */
#define USR_FRAMING   0x40   /* bit6 : framing error       */
#define USR_RXBREAK   0x80   /* bit7 : received break      */

typedef struct uart_s {
    uint8_t rx_buf;
    uint8_t rx_ready;
    uint8_t tx_buf;
    uint8_t mode;
    uint8_t cmd;
    uint8_t clksel;
} uart_t;

struct bus;

void    uart_init(void);
void    uart_feed(uint8_t byte);
void    uart_poll_host_input(void);
void    uart_inject_rx(struct bus *b, uint8_t c);

uint8_t uart_read8(struct bus *b, uint32_t addr);
void    uart_write8(struct bus *b, uint32_t addr, uint8_t v);

#endif /* UART_H */