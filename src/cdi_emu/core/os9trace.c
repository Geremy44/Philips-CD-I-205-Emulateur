/*
 * os9trace.c - OS-9 system call tracer (EMU_OS9_LOG=1)
 *
 * Before each instruction: a "TRAP #0 / dc.w code" is remembered with its
 * return PC and stack pointer; when the CPU comes back there, the result is
 * printed (carry set = error, error code in D1.w). Path arguments (A0) of
 * the calls that take one are printed too. EMU_OS9_LOG=2 also prints the
 * calls that succeed (very verbose).
 */

#include "os9trace.h"
#include "bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { int code; const char *name; } names[] = {
    {0x00,"F$Link"},{0x01,"F$Load"},{0x02,"F$UnLink"},{0x03,"F$Fork"},{0x04,"F$Wait"},
    {0x05,"F$Chain"},{0x06,"F$Exit"},{0x07,"F$Mem"},{0x08,"F$Send"},{0x09,"F$Icpt"},
    {0x0A,"F$Sleep"},{0x0C,"F$ID"},{0x0F,"F$PErr"},{0x10,"F$PrsNam"},{0x11,"F$CmpNam"},
    {0x15,"F$Time"},{0x16,"F$STime"},{0x18,"F$GPrDsc"},{0x1A,"F$GModDr"},{0x1B,"F$CpyMem"},
    {0x1D,"F$UnLoad"},{0x22,"F$DFork"},{0x25,"F$DatMod"},{0x28,"F$SRqMem"},{0x29,"F$SRtMem"},
    {0x2A,"F$IRQ"},{0x32,"F$SSvc"},{0x38,"F$Move"},{0x4E,"F$FModul"},{0x52,"F$SysDbg"},
    {0x53,"F$Event"},{0x56,"F$Alarm"},{0x5C,"F$SRqCMem"},{0x5E,"F$Panic"},
    {0x80,"I$Attach"},{0x81,"I$Detach"},{0x82,"I$Dup"},{0x83,"I$Create"},{0x84,"I$Open"},
    {0x85,"I$MakDir"},{0x86,"I$ChgDir"},{0x87,"I$Delete"},{0x88,"I$Seek"},{0x89,"I$Read"},
    {0x8A,"I$Write"},{0x8B,"I$ReadLn"},{0x8C,"I$WritLn"},{0x8D,"I$GetStt"},{0x8E,"I$SetStt"},
    {0x8F,"I$Close"},
};

static const char *name_of(int code)
{
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].code == code) return names[i].name;
    return "?";
}

static int takes_path(int code)
{
    switch (code) {
    case 0x00: case 0x01: case 0x03: case 0x05: case 0x80: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87: return 1;
    default: return 0;
    }
}

#define PENDING 256
static struct { uint32_t ret, sp; int code; char path[48]; uint32_t d0, d1; } pend[PENDING];
static int level = -1;

static void read_path(bus_t *b, uint32_t a, char *out, int n)
{
    int i = 0;
    for (; i < n - 1; i++) {
        uint8_t c = bus_read8(b, a + i);
        if (c < 0x20 || c >= 0x7F) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

void os9trace_step(bus_t *b, uint32_t pc, uint32_t sp, const uint32_t d[8], const uint32_t a[8], uint16_t sr)
{
    if (level < 0) {
        const char *s = getenv("EMU_OS9_LOG");
        level = (s && *s) ? atoi(s) : 0;
    }
    if (!level) return;

    /* back from a call? */
    for (int i = 0; i < PENDING; i++) {
        if (pend[i].ret && pend[i].ret == pc && pend[i].sp == sp) {
            int err = sr & 1;
            if (err || level >= 2) {
                fprintf(stderr, "[OS9] %-9s %s%s%s -> %s", name_of(pend[i].code),
                        pend[i].path[0] ? "\"" : "", pend[i].path, pend[i].path[0] ? "\"" : "",
                        err ? "ERROR" : "ok");
                if (err) fprintf(stderr, " #%03u ($%02X:%02X)", d[1] & 0xFFFF, (d[1] >> 8) & 0xFF, d[1] & 0xFF);
                fprintf(stderr, "  [in d0=%08X d1=%08X, out d0=%08X d1=%08X, pc=%06X]\n",
                        pend[i].d0, pend[i].d1, d[0], d[1], pc - 4);
            }
            pend[i].ret = 0;
        }
    }

    /* TRAP #0 + function code */
    if (bus_read16(b, pc) == 0x4E40) {
        int code = bus_read16(b, pc + 2) & 0xFFFF;
        int cd_driver = (pc >= 0x1A8214 && pc < 0x1AC950);   /* cdapdriv: keep everything */
        if (!cd_driver && (code == 0x08 || code == 0x0A || code == 0x53 || code == 0x2B)) return;
        if (cd_driver && (code == 0x08 || code == 0x0A))
            fprintf(stderr, "[OS9] cdapdriv %s d0=%08X d1=%08X at %06X\n",
                    code == 8 ? "F$Send" : "F$Sleep", d[0], d[1], pc);
        static int next = 0;                 /* ring: stale entries get reused */
        for (int k = 0; k < PENDING; k++) {
            int i = (next + k) % PENDING;
            if (!pend[i].ret || k == PENDING - 1) {
                next = (i + 1) % PENDING;
                pend[i].ret = pc + 4;
                pend[i].sp = sp;
                pend[i].code = code;
                pend[i].d0 = d[0];
                pend[i].d1 = d[1];
                pend[i].path[0] = 0;
                if (takes_path(code)) read_path(b, a[0], pend[i].path, sizeof(pend[i].path));
                break;
            }
        }
        if (code == 0x5E || code == 0x06)
            fprintf(stderr, "[OS9] %s d0=%08X d1=%08X at %06X\n", name_of(code), d[0], d[1], pc);
    }
}
