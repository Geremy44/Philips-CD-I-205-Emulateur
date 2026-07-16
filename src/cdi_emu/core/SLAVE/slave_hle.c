#include "slave.h"
#include "slave_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Slave base + offsets (voir manuel STEP 10) ---- */
#define SLAVE_BASE   0x200000
#define REG_STATUS   (SLAVE_BASE + 5)   /* 0x200005 : flag "prêt"          */
#define REG_EXCHANGE (SLAVE_BASE + 7)   /* 0x200007 : request/ack          */

#define CMD_INIT     0xF6   /* commande écrite par le firmware  */
#define ACK_INIT     0xF6   /* acquittement attendu             */

typedef struct {
    uint8_t last_request;
    uint8_t ack;
    uint8_t status;   /* 0x01 quand l'ack est prêt */

    /* accès UART pour injecter la réponse RX */
    uart_inject_fn inject;
    void          *bus;
} hle_ctx_t;

static void hle_reset(void *ctx)
{
    hle_ctx_t *c = ctx;
    uart_inject_fn inj = c->inject;   /* préserve les liens */
    void *bus = c->bus;
    memset(c, 0, sizeof(hle_ctx_t));
    c->inject = inj;
    c->bus    = bus;
}

static void hle_write8(void *ctx, uint32_t addr, uint8_t val)
{
    hle_ctx_t *c = ctx;

    if (addr == REG_EXCHANGE) {          /* 0x200007 */
        c->last_request = val;
        fprintf(stderr, "[SLAVE] CMD 0x%02X @200007\n", val);

        /* Réponse : le slave renvoie un octet via l'UART RX.
           ⚠️ Valeur 0x00 = À L'AVEUGLE (à ajuster selon le dump 0x181272) */
        if (c->inject && c->bus) {
            uint8_t reply = 0x00;        /* <-- valeur à confirmer */
            c->inject(c->bus, reply);
        }
    }
}

static uint8_t hle_read8(void *ctx, uint32_t addr)
{
    hle_ctx_t *c = ctx;
    if (addr == REG_STATUS)              /* 0x200005 */
        return c->status;
    return 0xFF;
}

static int hle_tick(void *ctx, int cyc) { (void)ctx; (void)cyc; return 0; }
static void hle_push_input(void *ctx, const uint8_t *rep, int len)
{ (void)ctx; (void)rep; (void)len; }
static void hle_destroy(void *ctx) { free(ctx); }

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

/* dans slave_hle.c */
void slave_hle_set_uart(slave_backend_t *be, uart_inject_fn fn, void *bus)
{
    hle_ctx_t *c = be->ctx;
    c->inject = fn;
    c->bus    = bus;
}
