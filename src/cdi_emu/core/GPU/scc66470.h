/* SCC66470 Video/System Controller interface (plain C99). */
#ifndef SCC66470_H
#define SCC66470_H

#include <stdint.h>

/* SCC66470 register addresses.  16-bit registers are big-endian on the bus. */
#define SCC_C0       0x1FFFC0u
#define SCC_C2       0x1FFFC2u
#define SCC_C8       0x1FFFC8u
#define SCC_CSR      0x1FFFE0u
#define SCC_DCR      0x1FFFE2u
#define SCC_VSR      0x1FFFE4u
#define SCC_BCR      0x1FFFE6u
#define SCC_DCR2     0x1FFFE8u
#define SCC_DCP      0x1FFFEAu
#define SCC_SWM      0x1FFFECu
#define SCC_A        0x1FFFF0u
#define SCC_B        0x1FFFF2u
#define SCC_PCR      0x1FFFF4u
#define SCC_MASK     0x1FFFF6u
#define SCC_SHIFT    0x1FFFF8u
#define SCC_INDEX    0x1FFFFBu
#define SCC_FC       0x1FFFFCu
#define SCC_BC       0x1FFFFDu
#define SCC_TC       0x1FFFFEu

#define SCC_CSR_DA  0x80u
#define SCC_CYCLES_PER_LINE  960u     /* 64 us PAL line at 15 MHz -> 50 Hz frames */
#define SCC_LINES_PER_FRAME  312u
#define SCC_ACTIVE_LINES     288u
#define SCC_CYCLES_PER_FRAME ((uint64_t)SCC_CYCLES_PER_LINE * SCC_LINES_PER_FRAME)
#define SCC_ACTIVE_CYCLES    ((uint64_t)SCC_CYCLES_PER_LINE * SCC_ACTIVE_LINES)

typedef struct scc66470 {
    uint16_t csr_control;
    uint16_t csr_status;
    uint16_t dcr;
    uint16_t vsr;
    uint16_t bcr;
    uint16_t dcr2;
    uint16_t dcp;
    uint16_t swm;
    uint16_t a;
    uint16_t b;
    uint16_t pcr;
    uint16_t mask;
    uint16_t shift;
    uint8_t  index;
    uint8_t  fc;
    uint8_t  bc;
    uint8_t  tc;
    uint16_t reg_c0;
    uint16_t reg_c2;
    uint16_t reg_c8;
    uint64_t cycle_counter;
    uint32_t clut[256];
    uint32_t ica_pointer;   /* $400 (master) or $80400 (slave dual), Table 8 */
    uint8_t  it1;           /* CSR status IT1: INTERRUPT instruction executed */
    int      it_logged;
} scc66470_t;

void scc66470_init(scc66470_t *v);
uint8_t scc66470_status(const scc66470_t *v);
/* Advances the beam; returns 1 when a new frame starts (vertical retrace) */
int  scc66470_tick(scc66470_t *v, int cpu_cycles);

/* At the vertical retrace: run the ICA program and set IT1 if it holds an
 * INTERRUPT instruction (and CSR DI1, bit 15, does not disable it) */
void scc66470_vretrace(scc66470_t *v, const uint8_t *dram, uint32_t dram_size);

/* IRQ request: IT1 pending (the 205 wires it to level 5, autovector 61) */
int  scc66470_irq(const scc66470_t *v);
uint8_t  scc66470_read8 (scc66470_t *v, uint32_t addr);
uint16_t scc66470_read16(scc66470_t *v, uint32_t addr);
void scc66470_write8 (scc66470_t *v, uint32_t addr, uint8_t val);
void scc66470_write16(scc66470_t *v, uint32_t addr, uint16_t val);
void scc66470_set_clut_entry(scc66470_t *v, int index, uint16_t rgb555);

/* ---- Display generation (datasheet SCC66470/03, "Display control") ----
 * The VSC outputs an 8-bit value per pixel on V(7:0) to an external
 * palette/back-end processor (BEP). scc66470_render() produces those 8-bit
 * values for one frame; the caller maps them to colours.
 *
 * Output buffer geometry: one output pixel = one 4-bit/pixel pixel, so an
 * 8-bit/pixel pixel is two output pixels wide (keeps the aspect ratio when
 * the firmware switches CM mid-frame). */
#define SCC_OUT_MAX_W 768
#define SCC_OUT_MAX_H 280
#define SCC_BEP_MAX   1024

typedef struct scc_frame {
    int      width, height;     /* full-screen size for the current CF/FD/ST */
    int      enabled;           /* DCR.DE */
    uint8_t  pix[SCC_OUT_MAX_W * SCC_OUT_MAX_H];
    uint32_t bep[SCC_BEP_MAX];  /* BEP CONTROL words seen during this frame */
    int16_t  bep_line[SCC_BEP_MAX]; /* output row from which each one applies
                                       (-1 = ICA, before the first line) */
    int      bep_count;
    uint32_t vsr_start;         /* display start address after the ICA */
    int      ica_count;         /* ICA instructions executed */
    int      interrupt;         /* an INTERRUPT ran (ICA or any DCA) */
    uint32_t line_addr[SCC_OUT_MAX_H]; /* first byte of each active line (tests) */
} scc_frame_t;

/* Render one frame from video DRAM (the first 1 MB of system DRAM). */
void scc66470_render(const scc66470_t *v, const uint8_t *dram, uint32_t dram_size,
                     scc_frame_t *out);

#endif
