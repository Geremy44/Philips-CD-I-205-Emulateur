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
 *   CA108080   DYUV start value plane A: Y = $10, U = V = $80
 * DYUV (ICM 5): each pair of bytes codes deltas, see vsd_dyuv_line().
 * DB/DC image contribution factors and C1 bit 23 mixing are modelled
 * (fades / cross-fades). Not modelled yet: per-region factors,
 * RGB555 coding, start values reloaded per line by the DCA.
 */

#include "vsd.h"
#include <string.h>

void vsd_reset(vsd_t *v)
{
    memset(v, 0, sizeof(*v));
    v->tcr = 0x800008;                    /* overlay; plane A: never transparent */
    v->weight_a = v->weight_b = 0x3F;     /* full contribution until set */
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
    case 0xCA: v->dyuv_start_a = data; break;
    case 0xCB: v->dyuv_start_b = data; break;
    case 0xD8: v->backdrop    = data; break;
    case 0xDB: v->weight_a    = data; break;
    case 0xDC: v->weight_b    = data; break;
    default:
        if (op >= 0xD0 && op <= 0xD7) { v->region[op & 7] = data; break; }   /* region control */
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
    int keyed = ((rgb & cmp) == (key & cmp));
    /* MCD212 encoding: 0 always, 1 colour key, 2 RGB555 transparency bit,
     * 3/4 region (matte) flag 0/1, 5/6 matte flag 0/1 OR colour key.
     * The RGB555 T bit is not emulated (taken as 0). */
    switch (mode & 7) {
    case 0:  cond = 1; break;
    case 1:  cond = keyed; break;
    case 3:  cond = vsd_rflag[0]; break;
    case 4:  cond = vsd_rflag[1]; break;
    case 5:  cond = vsd_rflag[0] || keyed; break;
    case 6:  cond = vsd_rflag[1] || keyed; break;
    default: cond = 0; break;
    }
    return (mode & 8) ? !cond : cond;
}

/* ---- DYUV (Green Book, "Delta YUV") ----
 * A line is a sequence of byte pairs:  byte 0 = dU:dY0, byte 1 = dV:dY1
 * (4-bit codes, high nibble first). Each code selects a quantized step
 * added modulo 256 to the previous value; the line starts from the CA/CB
 * value. The pair's U/V belong to the second pixel, the first one uses
 * the average of the previous and new values. */
static const uint8_t dyuv_step[16] = {
    0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255
};

static uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x); }

/* CD-i YUV -> RGB (Green Book coefficients, U/V centred on 128) */
static uint32_t yuv_rgb(int y, int u, int v)
{
    u -= 128; v -= 128;
    int r = y + (351 * v) / 256;
    int g = y - (86 * u + 179 * v) / 256;
    int b = y + (444 * u) / 256;
    return ((uint32_t)clamp8(r) << 16) | ((uint32_t)clamp8(g) << 8) | clamp8(b);
}

int vsd_plane_dyuv(const vsd_t *v, int plane_b)
{
    uint32_t icm = plane_b ? (v->icm >> 8) & 0xF : v->icm & 0xF;
    return icm == 0x5;
}

void vsd_dyuv_line(const vsd_t *v, int plane_b, const uint8_t *src, int w, uint32_t *out)
{
    uint32_t st = plane_b ? v->dyuv_start_b : v->dyuv_start_a;
    uint8_t y = (uint8_t)(st >> 16), u = (uint8_t)(st >> 8), vv = (uint8_t)st;
    /* one 8-bit pixel = 2 output pixels: a byte pair spans 4 output pixels */
    for (int x = 0; x + 3 < w; x += 4) {
        uint8_t b0 = src[x], b1 = src[x + 2];
        uint8_t u1 = (uint8_t)(u + dyuv_step[b0 >> 4]);
        uint8_t y0 = (uint8_t)(y + dyuv_step[b0 & 0xF]);
        uint8_t v1 = (uint8_t)(vv + dyuv_step[b1 >> 4]);
        uint8_t y1 = (uint8_t)(y0 + dyuv_step[b1 & 0xF]);
        uint32_t p0 = yuv_rgb(y0, (u + u1) >> 1, (vv + v1) >> 1);
        uint32_t p1 = yuv_rgb(y1, u1, v1);
        out[x] = out[x + 1] = p0;
        out[x + 2] = out[x + 3] = p1;
        y = y1; u = u1; vv = v1;
    }
    for (int x = w & ~3; x < w; x++) out[x] = yuv_rgb(y, u, vv);
}

/* RGB 8:8:8 x icf/63 (icf 0..63) */
static uint32_t scale_rgb(uint32_t rgb, uint32_t icf)
{
    if (icf >= 63) return rgb;
    uint32_t r = ((rgb >> 16) & 0xFF) * icf / 63, g = ((rgb >> 8) & 0xFF) * icf / 63,
             b = (rgb & 0xFF) * icf / 63;
    return (r << 16) | (g << 8) | b;
}

/* external video (MPEG picture of the Digital Video Cartridge) for the
 * pixel being mixed: it takes the place of the backdrop */
int      vsd_ext_on = 0;
uint32_t vsd_ext_rgb = 0;
int      vsd_rflag[2] = { 0, 0 };

/* Region control (D0-D7), as the Journal interactif uses it: bit 23 set =
 * the region changes a flag at X; bit 20 = the new value (8 reset, 9 set),
 * bit 16 = flag 0 / 1. Flags start each line at 0; changes apply in X
 * order. Weight changes (other op codes) are not modelled. */
void vsd_region_flags(const vsd_t *v, int x, int flag[2])
{
    flag[0] = flag[1] = 0;
    int best_x[2] = { -1, -1 };
    for (int i = 0; i < 8; i++) {
        uint32_t r = v->region[i];
        if (!(r & 0x800000u)) continue;
        int rx = (int)(r & 0x3FF), f = (r >> 16) & 1;
        if (rx <= x && rx >= best_x[f]) { best_x[f] = rx; flag[f] = (r >> 20) & 1; }
    }
}

uint32_t vsd_mix(const vsd_t *v, uint8_t a, int a_on, uint8_t b, int b_on)
{
    return vsd_mix_rgb(v, a, a_on, NULL, b, b_on, NULL);
}

uint32_t vsd_mix_rgb(const vsd_t *v, uint8_t a, int a_on, const uint32_t *dyuv_a,
                     uint8_t b, int b_on, const uint32_t *dyuv_b)
{
    uint32_t rgb_a = 0, rgb_b = 0;
    int got_a = dyuv_a ? (rgb_a = *dyuv_a, 1) : plane_rgb(v, 0, a, &rgb_a);
    int got_b = dyuv_b ? (rgb_b = *dyuv_b, 1) : plane_rgb(v, 1, b, &rgb_b);
    int vis_a = a_on && got_a &&
                !plane_transparent(v->tcr & 0xF, rgb_a, v->transp_a, v->mask_a);
    int vis_b = b_on && got_b &&
                !plane_transparent((v->tcr >> 8) & 0xF, rgb_b, v->transp_b, v->mask_b);

    /* Overlay (C1 bit 23 set in all traces): the front plane hides the back
     * one where it is not transparent. C2 bit 0 = plane B in front.
     * TODO: weighted mixing (C1 bit 23 clear, weights DB/DC). */
    /* Image contribution factors (DB plane A, DC plane B, 0..63): each plane
     * is attenuated by ICF/63 -- this is how fades to/from black are done.
     * TCR bit 23 set: overlay (front plane hides the back one); clear:
     * mixing, the two weighted planes are added (cross-fades). */
    if (vis_a) rgb_a = scale_rgb(rgb_a, v->weight_a & 0x3F);
    if (vis_b) rgb_b = scale_rgb(rgb_b, v->weight_b & 0x3F);
    uint32_t rgb;
    if (!(v->tcr & 0x800000u) && (vis_a || vis_b)) {
        uint32_t a = vis_a ? rgb_a : 0, b = vis_b ? rgb_b : 0;
        rgb = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            uint32_t s = ((a >> sh) & 0xFF) + ((b >> sh) & 0xFF);
            rgb |= (s > 255 ? 255u : s) << sh;
        }
    } else {
        uint32_t back = vsd_ext_on ? vsd_ext_rgb : irgb(v->backdrop);
        if (v->plane_order & 1) rgb = vis_b ? rgb_b : vis_a ? rgb_a : back;
        else                    rgb = vis_a ? rgb_a : vis_b ? rgb_b : back;
    }
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
