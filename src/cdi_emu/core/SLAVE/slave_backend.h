/*
 * slave_backend.h - Backend interface for slave co-processor emulation
 *
 * Defines the abstract interface that both HLE and LLE backends
 * implement, allowing the frontend to switch between them transparently.
 */

#ifndef SLAVE_BACKEND_H
#define SLAVE_BACKEND_H
#include <stdint.h>

/* Callback type for injecting a byte into the UART RX buffer */
typedef void (*uart_inject_fn)(void *bus, uint8_t byte);

/* Backend interface - a set of function pointers implementing slave behavior */
typedef struct slave_backend {
    char    *name;          /* Backend name (e.g. "HLE", "LLE-68HC05") */
    void    *ctx;            /* Backend-specific context data */

    void    (*reset)(void *ctx);
    uint8_t (*read8)(void *ctx, uint32_t addr);
    void    (*write8)(void *ctx, uint32_t addr, uint8_t val);
    int     (*tick)(void *ctx, int master_cycles);
    void    (*push_input)(void *ctx, const uint8_t *report, int len);
    void    (*destroy)(void *ctx);

    /* UART injection callback (set by slave_hle_set_uart) */
    uart_inject_fn inject;   /* Function to call for UART RX injection */
    void          *bus;      /* Bus pointer passed as first arg to inject */
} slave_backend_t;

#endif /* SLAVE_BACKEND_H */
