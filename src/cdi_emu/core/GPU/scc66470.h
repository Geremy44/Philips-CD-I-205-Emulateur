#ifndef SCC66470_H
#define SCC66470_H
#include <stdint.h>

/* Contrôleur vidéo SCC66470 du CD-I (Video & System Controller) */
typedef struct {
    uint16_t csr_control;   /* écrit via 1FFFE0 */
    uint16_t bcr, dcr2;
    uint16_t csr;        /* Control/Status Register   (0x1FFFE0/E1) */
    uint16_t dcr;        /* Display Command Register  (0x1FFFE2)    */
    uint16_t vsr;        /* Video Start Register      (0x1FFFE8)    */
    uint16_t reg_c0;     /* registres divers          (0x1FFFC0)    */
    uint16_t reg_c2;
    uint16_t reg_c8;
    uint8_t vsc_status_toggle;
    /* État interne pour la synchro vidéo */
    uint64_t scanline_counter;  /* incrémenté par le tick vidéo */
} scc66470_t;

void    scc66470_init(scc66470_t *v);
void    scc66470_tick(scc66470_t *v, int cpu_cycles);   /* avance la synchro */
uint8_t scc66470_read8 (scc66470_t *v, uint32_t addr);
void    scc66470_write8(scc66470_t *v, uint32_t addr, uint8_t val);
uint16_t scc66470_read16 (scc66470_t *v, uint32_t addr);
void     scc66470_write16(scc66470_t *v, uint32_t addr, uint16_t val);

#endif
