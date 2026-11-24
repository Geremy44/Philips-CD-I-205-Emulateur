/*
 * mpeg_system.c - see mpeg_system.h
 *
 * Written for this emulator from ISO/IEC 11172-1 (pack and packet headers)
 * and docs/fmv/FMV_COMPORTEMENT.md (what the cartridge chips keep of them).
 */

#include "mpeg_system.h"
#include <string.h>

enum {
    SEEK,           /* looking for 00 00 01 */
    ZERO1,          /* one zero seen */
    ZEROS,          /* two zeros or more */
    CODE,           /* next byte is the start code value */
    PACK,           /* pack header: SCR, step 0..5 */
    LENGTH,         /* packet length, step 0..1 */
    HEADER,         /* stuffing, STD buffer size or time stamp flags */
    STD_LOW,        /* second byte of the STD buffer size */
    STAMP,          /* PTS (step 1..4) then DTS (step 5..9) */
    FIRST           /* first payload byte: the stamps become valid */
};

void mps_reset(mps_t *s) { memset(s, 0, sizeof(*s)); }

/* a 33-bit stamp is sent as 3 + 15 + 15 bits over 5 bytes (marker bits
 * left out); `i` = byte 0..4 */
static void stamp_byte(int64_t *v, int i, uint8_t b)
{
    static const uint8_t shift[5] = { 30, 22, 15, 7, 0 };
    static const uint8_t mask[5]  = { 0x07, 0xFF, 0x7F, 0xFF, 0x7F };
    int64_t bits = (i == 1 || i == 3) ? b : b >> 1;
    *v = (*v & ~((int64_t)mask[i] << shift[i])) | ((bits & mask[i]) << shift[i]);
}

void mps_byte(mps_t *s, uint8_t b, uint8_t own)
{
    s->events = 0;
    if (s->counting) {                       /* packet length runs out */
        if (s->left == 1) { s->counting = 0; s->payload = 0; }
        s->left--;
    }

    switch (s->phase) {
    case SEEK:
        if (b == 0 && !s->payload) s->phase = ZERO1;
        break;
    case ZERO1:
        s->phase = b == 0 ? ZEROS : SEEK;
        break;
    case ZEROS:
        s->phase = b == 1 ? CODE : b == 0 ? ZEROS : SEEK;
        break;

    case CODE:
        s->phase = SEEK;
        if (b == 0xBA) { s->phase = PACK; s->step = 0; }
        else if (b == 0xB9) s->events |= MPS_END;
        else if (((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xE0) && b == own) { s->phase = LENGTH; s->step = 0; }
        break;

    case PACK:
        if (s->step < 5) { stamp_byte(&s->f_scr, s->step++, b); break; }
        s->scr = s->f_scr;                   /* valid on the byte after it */
        s->events |= MPS_SCR;
        s->phase = SEEK;
        break;

    case LENGTH:
        if (s->step++ == 0) s->left = (uint16_t)((s->left & 0x00FF) | b << 8);
        else {
            s->left = (uint16_t)((s->left & 0xFF00) | b);
            s->counting = 1;
            s->phase = HEADER;
        }
        break;

    case HEADER:
        if ((b & 0xE1) == 0x21) {            /* 0010xxx1 PTS, 0011xxx1 PTS + DTS */
            s->with_dts = (b & 0x10) != 0;
            stamp_byte(&s->f_pts, 0, b);
            s->phase = STAMP; s->step = 1;
        } else if (b == 0x0F) {              /* no stamp: payload follows */
            s->payload = 1;
            s->phase = SEEK;
        } else if ((b & 0xC0) == 0x40) s->phase = STD_LOW;
        else if (b != 0xFF) s->phase = SEEK; /* $FF: stuffing */
        break;
    case STD_LOW:
        s->phase = HEADER;
        break;

    case STAMP: {
        int i = s->step % 5;                 /* byte within the stamp */
        int64_t *v = s->step < 5 ? &s->f_pts : &s->f_dts;
        if (s->step == 5 && (b & 0xF1) != 0x11) { s->phase = SEEK; break; }
        if ((i == 0 || i == 2 || i == 4) && !(b & 1)) { s->phase = SEEK; break; }   /* marker bit */
        stamp_byte(v, i, b);
        s->step++;
        if (s->step == 5 && s->with_dts) break;              /* the DTS follows */
        if (s->step == 5 || s->step == 10) { s->payload = 1; s->phase = FIRST; }
        break;
    }

    case FIRST:
        s->pts = s->f_pts;
        s->dts = s->with_dts ? s->f_dts : s->f_pts;
        s->events |= MPS_PTS | MPS_DTS;
        s->phase = SEEK;
        break;
    }
}
