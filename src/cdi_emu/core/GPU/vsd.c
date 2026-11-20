/*
 * vsd.c - CD-i 205 Video Synthesizer/Decoder (back-end processor)
 *
 * VSD_NOTES - what the traces show (CD-RTOS shell, PS-7211 REL 2.1):
 *   C3000000 80FFFFFF 81EBEBEB ...  BF......   CLUT bank 0, 64 colours
 *   C3000001 80161616 ...           BF......   CLUT bank 1, 64 colours
 *   C00C1313   image coding: plane A = 3 (CLUT7), plane B = 3
 *   C1800101   transparency: plane A = 1 (colour key), plane B = 1
 *   C4070707   transparent colour A = 07 07 07
 *   CD0D428D   cursor position: x = bits 9-0, y = bits 21-12
 *   CE880000   cursor control: bit 23 = enable, bits 3-0 = colour (IRGB)
 *   CF0r pppp  cursor pattern row r, 16 pixels (bit 15 = leftmost)
 *   C2000001   plane order: plane B in front
 * Not modelled yet: weighted mixing, other transparency conditions,
 * DYUV/RGB555 coding.
 */

#include "vsd.h"
#include <string.h>

void vsd_reset(vsd_t *v)
{
    memset(v, 0, sizeof(*v));
    v->tcr = 0x08;                        /* plane A: never transparent */
    v->icm = 0x01;                        /* plane A: CLUT8 */
    for (int i = 0; i < 256; i++)         /* grey ramp until loaded */
        v->clut[i] = ((uint32_t)i << 16) | ((uint32_t)i << 8) | (uint32_t)i;
}

void vsd_command(vsd_t *v, uint32_t w)
{
    uint8_t  op   = (uint8_t)(w >> 24);
    uint32_t data = w & 0x00FFFFFFu;

    if (op >= 0x80 && op <= 0xBF) {       /* load CLUT colour */
        v->clut[((v->bank & 3) << 6) | (op & 0x3F)] = data;
        return;
    }
    switch (op) {
    case 0xC0: v->icm         = data; break;
    case 0xC1: v->tcr         = data; break;
    case 0xC2: v->plane_order = data; break;
    case 0xC3: v->bank        = (int)(data & 3); break;
    case 0xC4: v->transp_a    = data; break;
    case 0xC6: v->transp_b    = data; break;
    case 0xC7: v->mask_a      = data; break;
    case 0xC9: v->mask_b      = data; break;
    case 0xCD: v->cursor_pos  = data; break;
    case 0xCE: v->cursor_ctrl = data; break;
    case 0xCF: v->cursor_pat[(data >> 16) & 0xF] = (uint16_t)data; break;
    case 0xD8: v->backdrop    = data; break;
    case 0xDB: v->weight_a    = data; break;
    case 0xDC: v->weight_b    = data; break;
    default:
        if (op >= 0xD0 && op <= 0xD7) break;   /* region control: not modelled */
        v->unknown_count++;
        break;
    }
}

/* IRGB 4-bit colour (backdrop, cursor) to RGB 8:8:8 */
static uint32_t irgb(uint32_t c)
{
    uint32_t lvl = (c & 8) ? 0xFF : 0x80;
    return ((c & 4) ? lvl << 16 : 0) | ((c & 2) ? lvl << 8 : 0) | ((c & 1) ? lvl : 0);
}

/* Pixel value -> RGB for one plane, per its image coding method.
 * CLUT7 on plane B uses CLUT entries 128..255. Returns 0 if the plane is off. */
static int plane_rgb(const vsd_t *v, int plane_b, uint8_t value, uint32_t *rgb)
{
    uint32_t icm = plane_b ? (v->icm >> 8) & 0xF : v->icm & 0xF;
    uint32_t base = plane_b ? 128u : 0u;
    switch (icm) {
    case 0x0: return 0;                                             /* plane off */
    case 0x1: *rgb = v->clut[value]; break;                         /* CLUT8 (A only) */
    case 0x3:                                                       /* CLUT7 */
    case 0x4: *rgb = v->clut[base + (value & 0x7F)]; break;         /* CLUT77 */
    case 0xB: *rgb = v->clut[base + ((value >> 4) & 0xF)]; break;   /* CLUT4 */
    default:  *rgb = ((uint32_t)value << 16) | ((uint32_t)value << 8) | value; break;
    }
    return 1;
}

/* Transparency condition (bits 2-0, bit 3 inverts): 0 = always, 1 = colour key */
static int plane_transparent(uint32_t mode, uint32_t rgb, uint32_t key, uint32_t mask)
{
    int cond;
    uint32_t cmp = 0xFCFCFCu & ~mask;
    switch (mode & 7) {
    case 0:  cond = 1; break;
    case 1:  cond = ((rgb & cmp) == (key & cmp)); break;
    default: cond = 0; break;                     /* other conditions: not modelled */
    }
    return (mode & 8) ? !cond : cond;
}

uint32_t vsd_mix(const vsd_t *v, uint8_t a, int a_on, uint8_t b, int b_on)
{
    uint32_t rgb_a = 0, rgb_b = 0;
    int vis_a = a_on && plane_rgb(v, 0, a, &rgb_a) &&
                !plane_transparent(v->tcr & 0xF, rgb_a, v->transp_a, v->mask_a);
    int vis_b = b_on && plane_rgb(v, 1, b, &rgb_b) &&
                !plane_transparent((v->tcr >> 8) & 0xF, rgb_b, v->transp_b, v->mask_b);

    /* Overlay (C1 bit 23 set in all traces): the front plane hides the back
     * one where it is not transparent. C2 bit 0 = plane B in front.
     * TODO: weighted mixing (C1 bit 23 clear, weights DB/DC). */
    uint32_t rgb;
    if (v->plane_order & 1) rgb = vis_b ? rgb_b : vis_a ? rgb_a : irgb(v->backdrop);
    else                    rgb = vis_a ? rgb_a : vis_b ? rgb_b : irgb(v->backdrop);
    return 0xFF000000u | rgb;
}

void vsd_draw_cursor(const vsd_t *v, uint32_t *argb, int w, int h, int pitch)
{
    if (!(v->cursor_ctrl & 0x800000u)) return;                    /* disabled */
    int cx = (int)(v->cursor_pos & 0x3FF);                         /* 4-bit pixel units */
    int cy = (int)((v->cursor_pos >> 12) & 0x3FF);
    uint32_t col = 0xFF000000u | irgb(v->cursor_ctrl & 0xF);

    for (int row = 0; row < 16; row++) {
        int y = cy + row;
        if (y < 0 || y >= h) continue;
        for (int bit = 0; bit < 16; bit++) {
            if (!(v->cursor_pat[row] & (0x8000u >> bit))) continue;
            for (int u = 0; u < 2; u++) {                          /* normal-res pixels */
                int x = cx + bit * 2 + u;
                if (x >= 0 && x < w) argb[y * pitch + x] = col;
            }
        }
    }
}
