/*
 * mpeg_demux.h - MPEG-1 system stream demultiplexer, byte by byte
 *
 * Each decoder chip of the Digital Video Cartridge sees the system stream
 * and keeps its own elementary stream (stream_id: $C0+n audio, $E0+n
 * video), with the SCR / PTS / DTS that pace it.
 *
 * C port of the state machine of MAME's mpeg_demux (pull request #16181,
 * BSD-3-Clause, Alexandre Derumier), itself based on the CDi_MiSTer core.
 *
 * Portions derived from MAME: Copyright (c) Alexandre Derumier,
 * BSD-3-Clause; full notice in THIRD_PARTY_LICENSES.md at the repository root.
 */

#ifndef MPEG_DEMUX_H
#define MPEG_DEMUX_H

#include <stdint.h>

typedef struct {
    int     packet_body;            /* the NEXT byte belongs to the elementary stream */
    int64_t scr, pts, dts;          /* last values, 90 kHz */
    int     scr_updated, pts_updated, dts_updated, program_end;   /* pulses */
    /* private */
    int      state;
    int      length_decreasing;
    uint16_t length;
    int      dts_present;
    int64_t  scr_temp, pts_temp, dts_temp;
} mpeg_demux_t;

void mpeg_demux_reset(mpeg_demux_t *d);
void mpeg_demux_byte(mpeg_demux_t *d, uint8_t data, uint8_t stream_filter);

/* 33-bit timestamps wrap: signed difference a - b */
static inline int64_t mpeg_ts_diff(int64_t a, int64_t b)
{
    int64_t x = (a - b) & 0x1ffffffffLL;
    if (x & 0x100000000LL) x -= 0x200000000LL;
    return x;
}

#endif
