/*
 * slave_lle.c - Low-Level Emulation (LLE) of the slave co-processor
 *
 * This backend would emulate the actual 68HC05 microcontroller
 * instruction-by-instruction. It is currently a placeholder:
 * when a 68HC05 ROM dump becomes available, the real interpreter
 * should be implemented here.
 *
 * If no ROM is provided, slave_lle_create() returns NULL, and the
 * frontend falls back to the HLE backend.
 */

#include "slave.h"
#include "slave_backend.h"
#include <stdio.h>
#include <stdlib.h>

/* LLE context (placeholder for future 68HC05 interpreter state) */
typedef struct {
    uint8_t  rom[0x2000];   /* Internal 68HC05 ROM (future dump) */
    uint8_t  ram[0x100];    /* Internal 68HC05 RAM */
    /* TODO: 68HC05 CPU registers (A, X, PC, SP, CCR) */
} lle_ctx_t;

/* Placeholder operations - all no-ops until the interpreter is written */
static void    lle_reset (void *c){ (void)c; }
static uint8_t lle_read8 (void *c, uint32_t a){ (void)c;(void)a; return 0xFF; }
static void    lle_write8(void *c, uint32_t a, uint8_t v){ (void)c;(void)a;(void)v; }
static int     lle_tick  (void *c, int n){ (void)c;(void)n; return 0; }
static void    lle_push_input(void *c, const uint8_t *r, int l){ (void)c;(void)r;(void)l; }
static void    lle_destroy(void *c){ free(c); }

/*
 * Create the LLE backend.
 * Returns NULL if no HC05 ROM is provided, causing the frontend
 * to fall back to HLE.
 */
slave_backend_t *slave_lle_create(const uint8_t *hc05_rom, int rom_len) {
    if (!hc05_rom || rom_len <= 0) {
        fprintf(stderr,
          "[SLAVE/LLE] 68HC05 ROM missing -> LLE backend unavailable.\n"
          "            Using HLE backend instead.\n");
        return NULL;
    }

    slave_backend_t *be = calloc(1, sizeof(*be));
    lle_ctx_t *ctx = calloc(1, sizeof(lle_ctx_t));
    /* TODO: copy ROM: memcpy(ctx->rom, hc05_rom, rom_len); */

    be->name       = "LLE-68HC05";
    be->ctx        = ctx;
    be->reset      = lle_reset;
    be->read8      = lle_read8;
    be->write8     = lle_write8;
    be->tick       = lle_tick;
    be->push_input = lle_push_input;
    be->destroy    = lle_destroy;
    return be;
}
