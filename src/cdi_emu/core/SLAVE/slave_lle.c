#include "slave.h"
#include "slave_backend.h"
#include <stdio.h>
#include <stdlib.h>

/* Placeholder pour le futur cœur 68HC05.
   Le jour où tu obtiens la ROM interne, tu implémentes
   ici un vrai interpréteur d'instructions HC05.        */
typedef struct {
    uint8_t  rom[0x2000];   /* ROM interne du HC05 (dump futur) */
    uint8_t  ram[0x100];    /* RAM interne                       */
    /* registres CPU HC05 : A, X, PC, SP, CCR …                 */
} lle_ctx_t;

static void    lle_reset (void *c){ (void)c; }
static uint8_t lle_read8 (void *c, uint32_t a){ (void)c;(void)a; return 0xFF; }
static void    lle_write8(void *c, uint32_t a, uint8_t v){ (void)c;(void)a;(void)v; }
static int     lle_tick  (void *c, int n){ (void)c;(void)n; return 0; }
static void    lle_push_input(void *c, const uint8_t *r, int l){ (void)c;(void)r;(void)l; }
static void    lle_destroy(void *c){ free(c); }

slave_backend_t *slave_lle_create(const uint8_t *hc05_rom, int rom_len) {
    if (!hc05_rom || rom_len <= 0) {
        fprintf(stderr,
          "[SLAVE/LLE] ROM 68HC05 absente -> backend LLE indisponible.\n"
          "            Utilise le backend HLE.\n");
        return NULL;   /* la façade retombera sur HLE */
    }
    slave_backend_t *be = calloc(1, sizeof(*be));
    lle_ctx_t *ctx = calloc(1, sizeof(lle_ctx_t));
    /* memcpy(ctx->rom, hc05_rom, rom_len);  // quand tu l'auras */
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
