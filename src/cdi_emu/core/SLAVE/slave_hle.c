/*
 * slave_hle.c - High-Level Emulation (HLE) of the slave co-processor
 *
 * Instead of emulating the 68HC05 microcontroller cycle-by-cycle,
 * HLE implements the expected protocol at a high level: the firmware
 * writes commands to register $200007, and the HLE responds with
 * the expected values at register $200005.
 *
 * This is sufficient for early boot since the firmware mainly uses
 * the slave for initialization handshake and input device polling.
 */

#include "slave.h"
#include "slave_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Slave register addresses */
#define SLAVE_BASE   0x200000
#define REG_STATUS   (SLAVE_BASE + 5)   /* $200005: status/response (read) */
#define REG_EXCHANGE (SLAVE_BASE + 7)   /* $200007: command (write) */

/* Known command values */
#define CMD_INIT     0xF6   /* Firmware init command */
#define ACK_INIT     0xF6   /* Expected acknowledgement */

/* HLE backend context */
typedef struct {
    uint8_t last_request;  /* Last command written by the firmware */
    uint8_t ack;            /* Acknowledgement value */
    uint8_t status;         /* Status flag (1 = response ready) */

    uint8_t response;      /* Current response value */

    /* UART injection callback for feeding responses back to the CPU */
    uart_inject_fn inject;
    void          *bus;
} hle_ctx_t;

/* Reset the HLE state, preserving UART callback links */
static void hle_reset(void *ctx)
{
    hle_ctx_t *c = ctx;
    uart_inject_fn inj = c->inject;
    void *bus = c->bus;
    memset(c, 0, sizeof(hle_ctx_t));
    c->inject = inj;
    c->bus    = bus;
}

/*
 * Handle a write to a slave register.
 * The main register is the command/exchange register at $200007.
 * The HLE echoes back the command value as the response.
 */
static void hle_write8(void *ctx, uint32_t addr, uint8_t val)
{
    hle_ctx_t *c = ctx;

    if (addr == REG_EXCHANGE) {
        c->last_request = val;
        fprintf(stderr, "[SLAVE] CMD 0x%02X @200007\n", val);

        switch (val) {
        case 0xF0: c->response = 0xF0; break;    /* Test echo */
        default:   c->response = val;  break;    /* Generic echo */
        }
    }
}

/*
 * Handle a read from a slave register.
 * Returns the current response value from the status register at $200005.
 */
static uint8_t hle_read8(void *ctx, uint32_t addr)
{
    hle_ctx_t *c = ctx;
    if (addr == REG_STATUS) {
        fprintf(stderr, "[SLAVE RD] @200005 -> 0x%02X\n", c->response);
        return c->response;
    }
    return 0xFF;
}

/* Tick: no-op in HLE (timing is handled by the main CPU loop) */
static int hle_tick(void *ctx, int cyc) { (void)ctx; (void)cyc; return 0; }

/* Input push: no-op in current HLE implementation */
static void hle_push_input(void *ctx, const uint8_t *rep, int len)
{ (void)ctx; (void)rep; (void)len; }

/* Destroy the HLE context */
static void hle_destroy(void *ctx) { free(ctx); }

/* Create the HLE backend instance */
slave_backend_t *slave_hle_create(void)
{
    slave_backend_t *be = calloc(1, sizeof(*be));
    if (!be) return NULL;

    be->ctx = calloc(1, sizeof(hle_ctx_t));
    if (!be->ctx) { free(be); return NULL; }

    be->name       = "HLE";
    be->reset      = hle_reset;
    be->read8      = hle_read8;
    be->write8     = hle_write8;
    be->tick       = hle_tick;
    be->push_input = hle_push_input;
    be->destroy    = hle_destroy;
    return be;
}

/* Set the UART injection callback so the slave can feed data to the CPU's UART */
void slave_hle_set_uart(slave_backend_t *be, uart_inject_fn fn, void *bus)
{
    hle_ctx_t *c = be->ctx;
    c->inject = fn;
    c->bus    = bus;
}
