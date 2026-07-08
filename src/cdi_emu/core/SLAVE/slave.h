#ifndef SLAVE_H
#define SLAVE_H
#include "slave_backend.h"

#define SLAVE_REG_STAT  0x200005   /* read : statut/réponse   */
#define SLAVE_REG_CMD   0x200007   /* write: commande         */

typedef struct {
    slave_backend_t *be;   /* backend actif (HLE ou LLE)  */
    int irq_line;          /* état de l'IRQ SLAVE→M68k    */
} slave_t;

/* choix du backend au démarrage */
slave_backend_t *slave_hle_create(void);
slave_backend_t *slave_lle_create(const uint8_t *hc05_rom, int rom_len);

void    slave_attach(slave_t *s, slave_backend_t *be);
void    slave_reset(slave_t *s);
uint8_t slave_read8 (slave_t *s, uint32_t addr);
void    slave_write8(slave_t *s, uint32_t addr, uint8_t val);
void    slave_tick  (slave_t *s, int master_cycles);
void    slave_push_input(slave_t *s, const uint8_t *report, int len);

#endif
