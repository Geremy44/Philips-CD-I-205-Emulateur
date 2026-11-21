/*
 * rtc_m48t08.c - SGS-Thomson M48T08 TIMEKEEPER clock (see rtc_m48t08.h)
 */

#include "rtc_m48t08.h"
#include <time.h>

/* Host time, or a virtual clock (deterministic input replays): when
 * rtc_virtual_base is set, time = base + emulated CPU cycles / 15 MHz. */
int64_t  rtc_virtual_base = 0;
uint64_t rtc_virtual_cycles = 0;

int64_t rtc_host_now(void)
{
    if (rtc_virtual_base)
        return rtc_virtual_base + (int64_t)(rtc_virtual_cycles / 15000000u);
    return (int64_t)time(NULL);
}
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint8_t to_bcd(int v)   { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int     from_bcd(uint8_t v) { return ((v >> 4) & 0xF) * 10 + (v & 0xF); }

static int64_t emulated_now(const rtc_m48t08_t *r)
{
    if (r->stopped) return r->stopped_at;
    return rtc_host_now() + r->offset;
}

/* Fill regs[1..7] with the BCD fields of emulated time t (local time) */
static void latch(rtc_m48t08_t *r, int64_t t)
{
    time_t tt = (time_t)t;
    struct tm tmv;
    struct tm *lt = localtime(&tt);
    if (!lt) return;
    tmv = *lt;
    r->regs[1] = (uint8_t)(to_bcd(tmv.tm_sec) | (r->stopped ? 0x80 : 0));
    r->regs[2] = to_bcd(tmv.tm_min);
    r->regs[3] = to_bcd(tmv.tm_hour);
    r->regs[4] = (uint8_t)(tmv.tm_wday + 1);
    r->regs[5] = to_bcd(tmv.tm_mday);
    r->regs[6] = to_bcd(tmv.tm_mon + 1);
    r->regs[7] = to_bcd(tmv.tm_year % 100);
}

/* Apply the staged registers: new offset so that "now" equals them */
static void commit(rtc_m48t08_t *r)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_sec  = from_bcd(r->regs[1] & 0x7F);
    tmv.tm_min  = from_bcd(r->regs[2] & 0x7F);
    tmv.tm_hour = from_bcd(r->regs[3] & 0x3F);
    tmv.tm_mday = from_bcd(r->regs[5] & 0x3F);
    tmv.tm_mon  = from_bcd(r->regs[6] & 0x1F) - 1;
    int yy = from_bcd(r->regs[7]);
    tmv.tm_year = (yy <= 58) ? yy + 100 : yy;           /* same rule as sgstom */
    tmv.tm_isdst = -1;
    if (tmv.tm_mday < 1) tmv.tm_mday = 1;
    if (tmv.tm_mon < 0)  tmv.tm_mon = 0;

    time_t t = mktime(&tmv);
    if (t == (time_t)-1) return;

    int stop = (r->regs[1] & 0x80) != 0;
    r->offset = (int64_t)t - rtc_host_now();
    r->stopped = stop;
    r->stopped_at = (int64_t)t;
    r->staged = 0;
}

void rtc_init(rtc_m48t08_t *r)
{
    memset(r, 0, sizeof(*r));
}

int rtc_handles(uint32_t addr)
{
    return addr >= RTC_BASE && addr <= RTC_END;
}

uint8_t rtc_read8(rtc_m48t08_t *r, uint32_t addr)
{
    if (addr & 1) return 0xFF;                   /* upper byte lane only */
    int reg = (int)((addr - RTC_BASE) >> 1);
    if (reg == 0) return r->control;

    /* R or W set: registers are frozen (latched / being written) */
    if (!(r->control & 0xC0) && !r->staged)
        latch(r, emulated_now(r));
    return r->regs[reg];
}

void rtc_write8(rtc_m48t08_t *r, uint32_t addr, uint8_t v)
{
    if (addr & 1) return;
    int reg = (int)((addr - RTC_BASE) >> 1);

    if (reg == 0) {
        uint8_t old = r->control;
        /* R or W rising: freeze the current time in the registers */
        if (!(old & 0xC0) && (v & 0xC0) && !r->staged)
            latch(r, emulated_now(r));
        r->control = v;
        /* W falling: transfer the written registers to the clock */
        if ((old & 0x80) && !(v & 0x80) && r->staged)
            commit(r);
        return;
    }

    if (!(r->control & 0xC0) && !r->staged)
        latch(r, emulated_now(r));
    r->regs[reg] = v;
    r->staged = 1;
    /* Writing without the W bit (e.g. ST alone): apply immediately */
    if (!(r->control & 0x80))
        commit(r);
}

int rtc_save(const rtc_m48t08_t *r, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# CD-i 205 M48T08 clock: emulated time = host time + offset\n");
    fprintf(f, "offset=%lld\nstopped=%d\nstopped_at=%lld\ncontrol=%u\n",
            (long long)r->offset, r->stopped, (long long)r->stopped_at,
            (unsigned)(r->control & 0x3F));
    return fclose(f);
}

int rtc_load(rtc_m48t08_t *r, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long long v;
        unsigned u;
        if (sscanf(line, "offset=%lld", &v) == 1)     r->offset = v;
        else if (sscanf(line, "stopped=%lld", &v) == 1) r->stopped = (int)v;
        else if (sscanf(line, "stopped_at=%lld", &v) == 1) r->stopped_at = v;
        else if (sscanf(line, "control=%u", &u) == 1) r->control = (uint8_t)(u & 0x3F);
    }
    fclose(f);
    return 0;
}
