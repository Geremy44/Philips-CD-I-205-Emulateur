/*
 * mpeg_system.h - MPEG-1 system stream reader, one byte at a time
 *
 * Each decoder chip of the Digital Video Cartridge reads the whole system
 * stream and keeps the packets of its own elementary stream ($C0 + n audio,
 * $E0 + n video) with the clock references and time stamps that pace it.
 * Behaviour: docs/fmv/FMV_COMPORTEMENT.md, section 2.
 */

#ifndef MPEG_SYSTEM_H
#define MPEG_SYSTEM_H

#include <stdint.h>

/* events of the last byte read */
enum { MPS_SCR = 1, MPS_PTS = 2, MPS_DTS = 4, MPS_END = 8 };

typedef struct {
    int      payload;               /* the NEXT byte belongs to the kept stream */
    int64_t  scr, pts, dts;         /* latest values, 90 kHz */
    unsigned events;                /* MPS_* raised by the last byte */
    /* reading state */
    uint8_t  phase, step, with_dts;
    int      counting;
    uint16_t left;                  /* bytes left in the packet (16-bit field) */
    int64_t  f_scr, f_pts, f_dts;   /* fields being assembled */
} mps_t;

void mps_reset(mps_t *s);
/* one byte of the system stream; `own` = stream id of this chip */
void mps_byte(mps_t *s, uint8_t b, uint8_t own);

/* signed difference of two 33-bit time stamps (they wrap) */
static inline int64_t mps_ts_diff(int64_t a, int64_t b)
{
    int64_t d = (a - b) & 0x1FFFFFFFFLL;
    return d >= 0x100000000LL ? d - 0x200000000LL : d;
}

#endif
