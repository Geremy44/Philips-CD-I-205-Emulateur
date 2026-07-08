#include "scc66470.h"
#include <stdio.h>

/* ---- Bit de statut CSR (lu en 0x1FFFE1, octet de poids faible) ----
   Bit 7 = DA (Display Active). Le firmware le poll pour se synchroniser
   sur le balayage vidéo. Il faut qu'il OSCILLE : haut en zone active,
   bas pendant le blanking vertical (vblank). */
#define CSR_DA_BIT        0x80


// Timing vidéo PAL (625 lignes total)
#define SCANLINES_TOTAL     625         // Lignes PAL complètes
#define SCANLINES_ACTIVE    576         // Lignes visibles (avant blanking vertical)

/* Constantes CD-I PAL réalistes */
#define CYCLES_PER_LINE   1600     // ~ cycles CPU par ligne
#define LINES_PER_FRAME   312      // PAL
#define ACTIVE_LINES      288      // zone visible
#define ACTIVE_CYCLES     (ACTIVE_LINES * CYCLES_PER_LINE)
#define CYCLES_PER_FRAME  (LINES_PER_FRAME * CYCLES_PER_LINE)

/* ---- Carte des registres SCC66470 (Channel B = plane principal) ---- */
#define SCC_CSR   0x1FFFE0   /* W: control      R: status (E1: bit7=DA) */
#define SCC_DCR   0x1FFFE2   /* Display Command Register                */
#define SCC_VSR   0x1FFFE4   /* Video Start/Storage Register            */
#define SCC_BCR   0x1FFFE6   /* Border Color Register                   */
#define SCC_DCR2  0x1FFFE8   /* Display Command Register 2              */
#define SCC_DCP   0x1FFFEA   /* Display Command Pointer                 */
#define SCC_SWM   0x1FFFEC   /* Sync Width Modify                       */
#define SCC_A     0x1FFFF0   /* source pointer A (DMA/copy)             */
#define SCC_B     0x1FFFF2   /* pointer B                               */
#define SCC_PCR   0x1FFFF4   /* Pixel Control Register                  */
#define SCC_MASK  0x1FFFF6
#define SCC_SHIFT 0x1FFFF8
#define SCC_INDEX 0x1FFFFB
#define SCC_FC    0x1FFFFC   /* Foreground Color                        */
#define SCC_BC    0x1FFFFD   /* Background Color                        */
#define SCC_TC    0x1FFFFE   /* Transparent Color                       */

void scc66470_init(scc66470_t *v) {
    v->csr             = 0;
    v->csr_control     = 0;
    v->dcr             = 0;
    v->dcr2            = 0;
    v->vsr             = 0;
    v->bcr             = 0;
    v->reg_c0 = v->reg_c2 = v->reg_c8 = 0;
    v->scanline_counter = 0;
}

/* Appelé depuis la boucle CPU. C'EST ICI qu'on fait avancer l'horloge
   vidéo, proportionnellement aux cycles CPU réellement écoulés. */
void scc66470_tick(scc66470_t *v, int cpu_cycles) {
    v->scanline_counter += cpu_cycles;
    while (v->scanline_counter >= (long long)CYCLES_PER_FRAME)
        v->scanline_counter -= CYCLES_PER_FRAME;
}

static uint8_t scc66470_status_byte(scc66470_t *v) {
    long long pos = v->scanline_counter % CYCLES_PER_FRAME;
    return (pos < ACTIVE_CYCLES) ? CSR_DA_BIT : 0x00;
}

uint8_t scc66470_read8(scc66470_t *v, uint32_t addr) {
    switch (addr) {
    // case 0x1FFFE1: {   /* CSR status (low byte) — bit7 = DA */
    //     /* Filet de sécurité : si le tick CPU n'est pas branché, on fait
    //        quand même avancer l'horloge à chaque poll pour ne jamais
    //        bloquer la boucle DBEQ. Retire ce bloc dès que scc66470_tick()
    //        est appelé depuis la boucle CPU. */
    //     //if (v->scanline_counter == 0)        /* horloge figée ? */
    //     v->scanline_counter += 1000;     /* avance fine -> DA oscille */
    //     if (v->scanline_counter >= CYCLES_PER_FRAME)   // ⚠️ CYCLES_PER_FRAME vaut combien ?
    //         v->scanline_counter = 0;

    //     uint8_t status = scc66470_status_byte(v);

    //     // fprintf(stderr, "[scc66470] CSR status -> %02X (DA=%d, sc=%lld)\n",
    //     //         status, (status >> 7) & 1, (long long)v->scanline_counter);
    //     return status;
    // }

    case 0x1FFFE1: {   // Registre STATUS SCC66470 (VSC)
        // Le firmware (FUN_00180dc2) attend une transition bit7 : 1→0→1
        // On TOGGLE bit7 à chaque lecture pour garantir les deux phases.
        v->vsc_status_toggle ^= 1;
        uint8_t b7 = v->vsc_status_toggle ? 0x80 : 0x00;
        return b7;   // (ajoute d'autres bits de status si besoin)
    }

    case 0x1FFFE0:   /* CSR status (high byte) */
        return 0x00;

    default:
        // fprintf(stderr, "[scc66470] read8 reg inconnu %06X\n", addr);
        return 0x00;
    }
}

void scc66470_write8(scc66470_t *v, uint32_t addr, uint8_t val) {
    switch (addr) {
    case 0x1FFFE0:                       /* CSR high byte */
        v->csr = (uint16_t)((v->csr & 0x00FF) | (val << 8));
        break;
    case 0x1FFFE1:                       /* CSR low byte  */
        v->csr = (uint16_t)((v->csr & 0xFF00) | val);
        break;
    default:
        //fprintf(stderr, "[scc66470] write8 reg inconnu %06X = %02X\n", addr, val);
        break;
    }
}

uint16_t scc66470_read16(scc66470_t *v, uint32_t addr) {
    switch (addr) {
    case SCC_CSR:  return scc66470_status_byte(v);  /* statut, pas le control */
    case SCC_DCR:  return v->dcr;
    case SCC_VSR:  return v->vsr;
    case SCC_BCR:  return v->bcr;
    case SCC_DCR2: return v->dcr2;
    default:
        //fprintf(stderr, "[scc66470] read16 reg inconnu %06X\n", addr);
        return 0x0000;
    }
}

void scc66470_write16(scc66470_t *v, uint32_t addr, uint16_t val) {
    switch (addr) {
    case SCC_CSR:    /* CSR control register */
        v->csr_control = val;
        // fprintf(stderr, "[scc66470] CSR ctrl <= %04X "
        //         "(DM=%d TD=%d ED=%d BE=%d)\n",
        //         val, (val >> 6) & 3, (val >> 5) & 1, (val >> 2) & 1, val & 1);
        return;

    case SCC_DCR:  v->dcr  = val; return;   /* Display Command Register   */
    case SCC_VSR:  v->vsr  = val; return;   /* Video Start Register       */
    case SCC_BCR:  v->bcr  = val; return;   /* Border Color Register (E6) */
    case SCC_DCR2: v->dcr2 = val; return;   /* Display Command Register 2 */

    /* --- Channel A (second plane), bloc miroir à 0x1FFFC0 --- */
    case 0x1FFFC0: v->reg_c0 = val; return; /* CSR ctrl A   */
    case 0x1FFFC2: v->reg_c2 = val; return; /* DCR A        */
    case 0x1FFFC8: v->reg_c8 = val; return; /* DCR2 A       */

    default:
        // fprintf(stderr, "[scc66470] write16 reg inconnu %06X = %04X\n",
        //         addr, val);
        return;
    }
}
