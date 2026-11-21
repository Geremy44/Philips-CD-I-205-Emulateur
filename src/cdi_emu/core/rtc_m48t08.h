/*
 * rtc_m48t08.h - SGS-Thomson M48T08 TIMEKEEPER clock of the CD-i 205
 *
 * Memory map (205 service manual): "CLOCK&CAL REGS" at $3FBFF0-$3FBFFE,
 * upper byte lane (even addresses), on top of the 8 KB NVRAM. Driven by the
 * ROM module "sgstom". Registers (offset from $3FBFF0, BCD):
 *   +0 control  : bit 7 W (write), bit 6 R (read/freeze), bits 5-0 calibration
 *   +2 seconds  : bit 7 ST (oscillator stopped)
 *   +4 minutes   +6 hours   +8 day of week (1-7)
 *   +A date      +C month   +E year (00-99, <= $58 means 20xx for CD-RTOS)
 *
 * Emulation: the clock follows the host clock plus an offset (seconds).
 * Setting the time through the W bit only changes the offset, so the emulated
 * clock keeps running between sessions once the offset is saved.
 */

#ifndef RTC_M48T08_H
#define RTC_M48T08_H

#include <stdint.h>

#define RTC_BASE 0x3FBFF0u
#define RTC_END  0x3FBFFFu

typedef struct rtc_m48t08 {
    int64_t  offset;       /* emulated time = host time + offset (seconds) */
    int      stopped;      /* ST bit: oscillator stopped */
    int64_t  stopped_at;   /* emulated time frozen while stopped */
    uint8_t  control;      /* W, R, calibration */
    uint8_t  regs[8];      /* latched / staged time registers (index 1..7) */
    int      staged;       /* time registers written while W was set */
} rtc_m48t08_t;

void    rtc_init (rtc_m48t08_t *r);

/* Time source: host clock, or virtual (base + cycles / 15 MHz) if set */
extern int64_t  rtc_virtual_base;
extern uint64_t rtc_virtual_cycles;
int64_t rtc_host_now(void);
int     rtc_handles(uint32_t addr);
uint8_t rtc_read8 (rtc_m48t08_t *r, uint32_t addr);
void    rtc_write8(rtc_m48t08_t *r, uint32_t addr, uint8_t v);

/* Persistence of the offset / stop state (small text file) */
int     rtc_save(const rtc_m48t08_t *r, const char *path);
int     rtc_load(rtc_m48t08_t *r, const char *path);

#endif /* RTC_M48T08_H */
