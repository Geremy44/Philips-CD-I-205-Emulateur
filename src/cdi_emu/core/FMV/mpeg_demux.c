/*
 * mpeg_demux.c - see mpeg_demux.h
 *
 * Portions derived from MAME: Copyright (c) Alexandre Derumier,
 * BSD-3-Clause; full notice in THIRD_PARTY_LICENSES.md at the repository root.
 */

#include "mpeg_demux.h"
#include <string.h>

enum {
    IDLE = 0, MAGIC0, MAGIC2, MAGIC_MATCH,
    PACK0, PACK1, PACK2, PACK3, PACK4, PACK5,
    PES0, PES1, PES2, PES3, PES4, PES5, PES6, PES7, PES8,
    DTS0, DTS1, DTS2, DTS3, DTS4
};

void mpeg_demux_reset(mpeg_demux_t *d)
{
    memset(d, 0, sizeof(*d));
}

/* replace bits [shift, shift+width) of *v */
static void put(int64_t *v, int shift, int64_t width_mask, int64_t bits)
{
    *v = (*v & ~(width_mask << shift)) | ((bits & width_mask) << shift);
}

void mpeg_demux_byte(mpeg_demux_t *d, uint8_t data, uint8_t filter)
{
    d->scr_updated = d->pts_updated = d->dts_updated = d->program_end = 0;

    if (d->length_decreasing) {
        if (d->length == 1) { d->length_decreasing = 0; d->packet_body = 0; }
        d->length--;
    }

    switch (d->state) {
    case PACK5: d->state = IDLE; d->scr = d->scr_temp; d->scr_updated = 1; break;
    case PACK4: d->state = PACK5; put(&d->scr_temp, 0, 0x7f, data >> 1); break;
    case PACK3: d->state = PACK4; put(&d->scr_temp, 7, 0xff, data); break;
    case PACK2: d->state = PACK3; put(&d->scr_temp, 15, 0x7f, data >> 1); break;
    case PACK1: d->state = PACK2; put(&d->scr_temp, 22, 0xff, data); break;
    case PACK0: d->state = PACK1; put(&d->scr_temp, 30, 0x7, data >> 1); break;

    case PES8:      /* a PTS was present (a DTS never comes alone) */
        d->state = IDLE;
        d->pts = d->pts_temp; d->pts_updated = 1;
        d->dts = d->dts_present ? d->dts_temp : d->pts_temp;   /* no DTS: the PTS */
        d->dts_updated = 1;
        break;

    case DTS4:
        if (data & 1) { put(&d->dts_temp, 0, 0x7f, data >> 1); d->packet_body = 1; d->state = PES8; }
        else d->state = IDLE;
        break;
    case DTS3: d->state = DTS4; put(&d->dts_temp, 7, 0xff, data); break;
    case DTS2:
        if (data & 1) { put(&d->dts_temp, 15, 0x7f, data >> 1); d->state = DTS3; }
        else d->state = IDLE;
        break;
    case DTS1: d->state = DTS2; put(&d->dts_temp, 22, 0xff, data); break;
    case DTS0:
        if ((data & 0xf1) == 0x11) { put(&d->dts_temp, 30, 0x7, data >> 1); d->state = DTS1; }
        else d->state = IDLE;
        break;

    case PES7:
        if (data & 1) {
            put(&d->pts_temp, 0, 0x7f, data >> 1);
            if (d->dts_present) d->state = DTS0;
            else { d->packet_body = 1; d->state = PES8; }
        } else d->state = IDLE;
        break;
    case PES6: d->state = PES7; put(&d->pts_temp, 7, 0xff, data); break;
    case PES5:
        if (data & 1) { put(&d->pts_temp, 15, 0x7f, data >> 1); d->state = PES6; }
        else d->state = IDLE;
        break;
    case PES4: d->state = PES5; put(&d->pts_temp, 22, 0xff, data); break;
    case PES3: d->state = PES2; break;          /* 2nd byte of the STD buffer size */
    case PES2:
        if ((data & 0xf1) == 0x21) {            /* PTS only */
            put(&d->pts_temp, 30, 0x7, data >> 1); d->dts_present = 0; d->state = PES4;
        } else if ((data & 0xf1) == 0x31) {     /* PTS and DTS */
            put(&d->pts_temp, 30, 0x7, data >> 1); d->dts_present = 1; d->state = PES4;
        } else if (data == 0x0f) {              /* neither */
            d->state = IDLE; d->packet_body = 1;
        } else if ((data & 0xc0) == 0x40) d->state = PES3;   /* STD buffer size */
        else if (data == 0xff) d->state = PES2;              /* stuffing */
        else d->state = IDLE;
        break;
    case PES1: d->state = PES2; d->length = (uint16_t)((d->length & 0xff00) | data); d->length_decreasing = 1; break;
    case PES0: d->state = PES1; d->length = (uint16_t)((d->length & 0x00ff) | (data << 8)); break;

    case MAGIC_MATCH:
        if (data == 0xba) d->state = PACK0;
        else if ((data & 0xf0) == 0xc0 || (data & 0xf0) == 0xe0)
            d->state = (data == filter) ? PES0 : IDLE;   /* this chip's own stream only */
        else if (data == 0xb9) { d->program_end = 1; d->state = IDLE; }
        else d->state = IDLE;
        break;
    case MAGIC2:
        if (data == 0x01) d->state = MAGIC_MATCH;
        else if (data == 0x00) d->state = MAGIC2;
        else d->state = IDLE;
        break;
    case MAGIC0: d->state = (data == 0x00) ? MAGIC2 : IDLE; break;
    case IDLE:
    default:
        d->state = (data == 0x00 && !d->packet_body) ? MAGIC0 : IDLE;
        break;
    }
}
