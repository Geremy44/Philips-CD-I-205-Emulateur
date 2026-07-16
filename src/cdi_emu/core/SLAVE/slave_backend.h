#ifndef SLAVE_BACKEND_H
#define SLAVE_BACKEND_H
#include <stdint.h>

/* dans slave_backend.h, ajoute : */
typedef void (*uart_inject_fn)(void *bus, uint8_t byte);

typedef struct slave_backend {
    char    *name;
    void    *ctx;

    void    (*reset)(void *ctx);
    uint8_t (*read8)(void *ctx, uint32_t addr);
    void    (*write8)(void *ctx, uint32_t addr, uint8_t val);
    int     (*tick)(void *ctx, int master_cycles);
    void    (*push_input)(void *ctx, const uint8_t *report, int len);
    void    (*destroy)(void *ctx);

    /* --- injection UART --- */
    uart_inject_fn inject;   /* callback vers uart_inject_rx */
    void          *bus;      /* passé en 1er arg de inject   */
} slave_backend_t;

#endif
