/*
 * vsd.h - CD-i 205 Video Synthesizer/Decoder ("V.S.D.", the back-end
 * processor fed by the two SCC66470 video ports, see the 205 block diagram)
 *
 * The SCC66470 forwards "BEP CONTROL" words (ICA/DCA instructions with
 * bit 31 set) to this chip. Their encoding matches the back-end command
 * set later integrated in the MCD212:
 *   80..BF  load CLUT entry (bank * 64 + low 6 bits) with RGB 8:8:8
 *   C0      image coding method   (plane A in bits 3-0, plane B in 11-8)
 *   C1      transparency control  (plane A in bits 3-0, plane B in 11-8)
 *   C2      plane order
 *   C3      CLUT bank (0..3)
 *   C4/C6   transparent colour A/B,  C7/C9 mask colour A/B
 *   CD/CE/CF cursor position / control / pattern row
 *   D8      backdrop colour,  DB/DC weight A/B
 * Only what the 205 boot and the CD-RTOS shell use is interpreted; the
 * encoding is inferred from the firmware traces (see VSD_NOTES in vsd.c).
 */

#ifndef VSD_H
#define VSD_H

#include <stdint.h>

typedef struct vsd {
    uint32_t clut[256];        /* RGB 8:8:8 */
    int      bank;
    uint32_t icm;              /* image coding method */
    uint32_t tcr;              /* transparency control */
    uint32_t plane_order;
    uint32_t transp_a, transp_b, mask_a, mask_b;
    uint32_t backdrop;
    uint32_t weight_a, weight_b;
    uint32_t cursor_pos, cursor_ctrl;
    uint16_t cursor_pat[16];
    uint32_t unknown_count;    /* commands not interpreted */
} vsd_t;

void     vsd_reset(vsd_t *v);
void     vsd_command(vsd_t *v, uint32_t word);

/* Convert a plane-A pixel value (V7..V0 of the master VSC) to ARGB8888 */
uint32_t vsd_pixel_a(const vsd_t *v, uint8_t value);

/* Overlay the hardware cursor on an ARGB frame (w x h, row pitch in pixels) */
void     vsd_draw_cursor(const vsd_t *v, uint32_t *argb, int w, int h, int pitch);

#endif /* VSD_H */
