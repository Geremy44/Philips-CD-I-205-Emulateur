/*
 * uart.h - Motorola 68070 on-chip UART emulation
 *
 * The 68070 integrates a serial UART whose registers are mapped
 * at odd addresses starting from $80002011. This module handles
 * register read/write emulation and bridges to the SDL2 console
 * window for host I/O.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/*
 * UART register map (offsets are absolute from UART_BASE).
 * Registers are on odd addresses per the 68070 datasheet.
 */
#define UART_BASE  0x80002000u
#define UART_SIZE  0x00000020u   /* Covers $80002010 through $8000201F */
#define UART_END    0x8000201F

/* Register offsets (addr - UART_BASE) */
#define UART_MR_OFF   0x11   /* Mode Register        */
#define UART_SR_OFF   0x13   /* Status Register (RO) */
#define UART_CS_OFF   0x15   /* Clock Select         */
#define UART_CR_OFF   0x17   /* Command Register     */
#define UART_TX_OFF   0x19   /* TX Holding Register  */
#define UART_RX_OFF   0x1B   /* RX Holding Register  */

/* UART Status Register bit definitions (Fig.41 of datasheet) */
#define USR_RXRDY     0x01   /* Bit 0: RX ready            */
#define USR_TXRDY     0x04   /* Bit 2: TX ready            */
#define USR_TXEMT     0x08   /* Bit 3: TX empty            */
#define USR_OVERRUN   0x10   /* Bit 4: Overrun error       */
#define USR_PARITY    0x20   /* Bit 5: Parity error        */
#define USR_FRAMING   0x40   /* Bit 6: Framing error       */
#define USR_RXBREAK   0x80   /* Bit 7: Received break      */

/* UART internal state */
typedef struct uart_s {
    uint8_t rx_buf;       /* Current RX byte */
    uint8_t rx_ready;     /* 1 = RX data available */
    uint8_t tx_buf;       /* Current TX byte */
    uint8_t mode;         /* Mode register value */
    uint8_t cmd;          /* Command register value */
    uint8_t clksel;       /* Clock select value */
} uart_t;

struct bus;

/* Initialize UART to idle state */
void uart_init(void);

/* Inject a byte into the UART RX buffer (from host side) */
void uart_feed(uint8_t byte);

/* Poll host stdin for input (platform-specific, non-blocking) */
void uart_poll_host_input(void);

/* Inject a byte directly into the bus UART RX buffer */
void uart_inject_rx(struct bus *b, uint8_t c);

/* Bus-level UART register read/write (called from bus.c) */
uint8_t uart_read8(struct bus *b, uint32_t addr);
void    uart_write8(struct bus *b, uint32_t addr, uint8_t v);

#endif /* UART_H */
