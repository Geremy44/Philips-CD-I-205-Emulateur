/*
 * fmv.c - Digital Video Cartridge (see fmv.h): the board
 *
 * ROM, 1 MB of RAM, the video chip's DRAM, the register window of the two
 * decoder chips (audio fma.c at $E03000, video mcd251.c at $E04000), the
 * stream transfers over the 68070 DMA channel 2 and the shared interrupt
 * line (level 4, vectored). Written for this emulator from
 * docs/fmv/FMV_COMPORTEMENT.md (section 1) and the service manual.
 */

#include "fmv.h"
#include "fmv_internal.h"
#include "../options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE  0xE40000u
#define ROM_SIZE  0x20000u
#define ROM_END   0xE80000u               /* the 128 KB repeat up to here */
#define VRAM_BASE 0xE80000u
#define VRAM_SIZE 0x80000u
#define REG_BASE  0xE00000u
#define REG_END   0xE3FFFFu
#define XRAM_SIZE 0x100000u               /* 1 MB of system memory */

#define WIN_AUDIO 0x1800                  /* register window, word index >> 8 */
#define WIN_VIDEO 0x2000
#define WIN_PIXCLK 0x08
#define DATA_PORT 0x206F                  /* $E040DE */

static int      inserted = 0;
static uint8_t *rom, *vram, *xram;
static uint32_t xram_base = 0xD00000u;   /* EMU_FMV_RAM=hex to try another base */
static FILE    *stream_dump;             /* EMU_FMV_DUMP=file: the stream sent to the chips */
static int      dump_checked;

uint64_t fmv_now = 0;
int      fmv_log = 0;

/* transfers: the chip asking (-1 none, 0 video, 1 audio) and the chip the
 * current one goes to */
static int      asking = -1, receiver = 0;
static uint64_t blank_ends = FMV_NEVER;

/* a word register accessed byte by byte */
static uint16_t rd_word; static uint32_t rd_addr = 1;
static uint8_t  wr_high; static uint32_t wr_addr = 1;

extern uint32_t bus_watch_pc;             /* PC of the current instruction (bus.c) */
extern int bus_dma2_transfer(void (*sink)(uint16_t));    /* bus.c: words moved, -1 not armed */

int fmv_enabled(void) { return inserted; }
int fmv_rom_loaded(void) { return rom != NULL; }

int fmv_init(const char *rom_path)
{
    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "[FMV] cartridge ROM not found: %s\n", rom_path); return -1; }
    rom = calloc(1, ROM_SIZE);
    vram = calloc(1, VRAM_SIZE);
    xram = calloc(1, XRAM_SIZE);
    if (!rom || !vram || !xram) { fclose(f); return -1; }
    if (getenv("EMU_FMV_RAM")) xram_base = (uint32_t)strtoul(getenv("EMU_FMV_RAM"), NULL, 16);
    size_t n = fread(rom, 1, ROM_SIZE, f);
    fclose(f);
    if (n != ROM_SIZE) {
        fprintf(stderr, "[FMV] ROM %s: %u bytes, 131072 expected\n", rom_path, (unsigned)n);
        free(rom); rom = NULL;
        return -1;
    }
    fprintf(stderr, "[FMV] Digital Video Cartridge ROM loaded (%s)\n", rom_path);
    return 0;
}

void fmv_set_enabled(int enable)
{
    inserted = enable && rom != NULL;
    fprintf(stderr, "[FMV] cartridge %s\n", inserted ? "inserted" : "removed");
}

int fmv_option_load(void) { return option_get("fmv", -1); }
void fmv_option_save(int enable) { option_set("fmv", enable ? 1 : 0); }

static int in_xram(uint32_t a) { return a >= xram_base && a < xram_base + XRAM_SIZE; }
static int in_vram(uint32_t a) { return a >= VRAM_BASE && a < VRAM_BASE + VRAM_SIZE; }
static int in_rom(uint32_t a)  { return a >= ROM_BASE && a < ROM_END; }

int fmv_handles(uint32_t addr)
{
    return inserted && ((addr >= REG_BASE && addr <= 0xEFFFFFu) || in_xram(addr));
}

/* The kernel looks for memory in "Extension 1" ($500000-$FFFFFF: 5A/A5
 * every 32 KB, byte accesses, from the 205 ROM). It must find nothing in
 * the chips or the video DRAM, or it lays system memory over them and the
 * drivers then corrupt the memory lists (reboot loop after fmvconf). The
 * real cartridge does not answer such accesses; here the two probe
 * routines of the 205 ROM see an empty bus, everything else the hardware. */
static int memory_probe(void)
{
    uint32_t pc = bus_watch_pc;
    return (pc >= 0x1806F0u && pc < 0x180790u) || (pc >= 0x184820u && pc < 0x184870u);
}

/* ======================= transfers ======================= */

static void deliver(uint16_t w)
{
    if (stream_dump) { fputc(w >> 8, stream_dump); fputc(w & 0xFF, stream_dump); }
    if (receiver) { fmva_data((uint8_t)(w >> 8)); fmva_data((uint8_t)w); }
    else          { fmvv_data((uint8_t)(w >> 8)); fmvv_data((uint8_t)w); }
}

/* a chip is asking: move a block as soon as the DMA channel is running */
static void try_transfer(void)
{
    if (asking < 0) return;
    if (asking == 1 && getenv("EMU_FMA_HOLD") && !fmva_wants_data()) return;   /* test only */
    receiver = asking;
    int words = bus_dma2_transfer(deliver);
    if (words < 0) return;                    /* channel not started yet */
    asking = -1;
    if (fmv_log >= 2) fprintf(stderr, "[FMV] DMA %d words to the %s chip\n", words, receiver ? "audio" : "video");
    if (receiver) fmva_end_of_transfer(1); else fmvv_end_of_transfer();
}

void fmv_board_drq(int for_audio) { asking = for_audio; try_transfer(); }
void fmv_dma_started(void) { if (inserted) try_transfer(); }

/* ======================= time, display, interrupts ======================= */

void fmv_reset(void)
{
    if (!rom) return;
    if (!dump_checked) {
        dump_checked = 1;
        const char *e = getenv("EMU_FMV_DUMP");
        if (e) stream_dump = fopen(e, "wb");
        e = getenv("EMU_FMV_LOG");
        fmv_log = e ? atoi(e) : 0;
    }
    asking = -1; receiver = 0; blank_ends = FMV_NEVER;
    fmvv_reset();
    fmva_reset();
}

void fmv_tick(int cycles)
{
    if (!inserted) return;
    fmv_now += (uint64_t)cycles;
    if (fmv_now >= blank_ends) { blank_ends = FMV_NEVER; fmvv_vblank(0); }
    fmvv_tick();
    fmva_tick();
    if (asking == 1) try_transfer();          /* the audio chip keeps asking */
}

void fmv_frame(void)
{
    if (!inserted) return;
    fmvv_vblank(1);
    blank_ends = fmv_now + FMV_CPU_HZ * 16 / 10000;     /* 1.6 ms of blanking */
}

int fmv_irq(int *vector)
{
    if (!inserted) return 0;
    if (fmva_irq_pending()) { *vector = fmva_vector(); return 4; }   /* audio first */
    if (fmvv_irq_pending()) { *vector = fmvv_vector(); return 4; }
    return 0;
}

int fmv_video_pixel(int x, int y, uint32_t *rgb) { return inserted && fmvv_pixel(x, y, rgb); }
int fmv_video_showing(void) { return inserted && fmvv_showing(); }

/* ======================= register window ======================= */

uint16_t fmv_reg_read16(uint32_t addr)
{
    uint16_t idx = (uint16_t)((addr >> 1) & 0x7FFF);    /* repeats every 64 KB */
    if ((idx & 0xFF00) == WIN_AUDIO) return fmva_read(idx & 0xFF);
    if ((idx & 0xFF00) == WIN_VIDEO) return fmvv_read(idx & 0xFF);
    return 0;
}

void fmv_reg_write16(uint32_t addr, uint16_t v)
{
    uint16_t idx = (uint16_t)((addr >> 1) & 0x7FFF);
    if ((idx >> 8) == WIN_PIXCLK) return;
    if (idx == DATA_PORT) {                   /* a word sent by hand */
        deliver(v);
        if (receiver) fmva_end_of_transfer(0); else fmvv_end_of_transfer();
        return;
    }
    if ((idx & 0xFF00) == WIN_AUDIO) fmva_write(idx & 0xFF, v);
    else if ((idx & 0xFF00) == WIN_VIDEO) fmvv_write(idx & 0xFF, v);
    else if (fmv_log >= 2) fprintf(stderr, "[FMV] W %06X = %04X pc=%06X\n", (unsigned)addr, v, (unsigned)bus_watch_pc);
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

uint16_t fmv_read16(uint32_t addr)
{
    if (in_rom(addr)) return be16(rom + ((addr - ROM_BASE) & (ROM_SIZE - 1)));
    if (in_xram(addr)) return be16(xram + (addr - xram_base));
    if (memory_probe()) return 0xFFFF;
    if (in_vram(addr)) return be16(vram + (addr - VRAM_BASE));
    if (addr <= REG_END) return fmv_reg_read16(addr & ~1u);
    return 0xFFFF;
}

void fmv_write16(uint32_t addr, uint16_t v)
{
    uint8_t *p = NULL;
    if (in_xram(addr)) p = xram + (addr - xram_base);
    else if (memory_probe()) return;
    else if (in_vram(addr)) p = vram + (addr - VRAM_BASE);
    if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return; }
    if (addr >= REG_BASE && addr <= REG_END) fmv_reg_write16(addr & ~1u, v);
}

/* bytes: memory directly; a register as a word (the even byte reads the
 * word, the odd byte right after gets its low half; the even byte written
 * waits for the odd one) */
uint8_t fmv_read8(uint32_t addr)
{
    if (in_rom(addr)) return rom[(addr - ROM_BASE) & (ROM_SIZE - 1)];
    if (in_xram(addr)) return xram[addr - xram_base];
    if (memory_probe()) return 0xFF;
    if (in_vram(addr)) return vram[addr - VRAM_BASE];
    if (addr > REG_END) return 0xFF;
    if ((addr & 1) && rd_addr == (addr & ~1u)) { rd_addr = 1; return (uint8_t)rd_word; }
    rd_word = fmv_reg_read16(addr & ~1u);
    rd_addr = addr & ~1u;
    return (addr & 1) ? (uint8_t)rd_word : (uint8_t)(rd_word >> 8);
}

void fmv_write8(uint32_t addr, uint8_t v)
{
    if (in_xram(addr)) { xram[addr - xram_base] = v; return; }
    if (memory_probe()) return;
    if (in_vram(addr)) { vram[addr - VRAM_BASE] = v; return; }
    if (addr < REG_BASE || addr > REG_END) return;
    if (!(addr & 1)) { wr_high = v; wr_addr = addr; return; }
    uint8_t high = wr_addr == (addr & ~1u) ? wr_high : 0;
    wr_addr = 1;
    fmv_reg_write16(addr & ~1u, (uint16_t)(high << 8 | v));
}

void fmv_status_text(char *out, size_t n)
{
    if (!rom) { snprintf(out, n, "Cartouche FMV : ROM absente"); return; }
    if (!inserted) { snprintf(out, n, "Cartouche FMV : retir\xC3\xA9" "e"); return; }
    static unsigned long last_count = 0;
    static uint64_t last_at = 0;
    static double rate = 0;
    if (fmv_now - last_at >= FMV_CPU_HZ) {               /* pictures per emulated second */
        rate = (double)(fmvv_shown - last_count) * FMV_CPU_HZ / (double)(fmv_now - last_at);
        last_count = fmvv_shown; last_at = fmv_now;
    }
    int w, h, vplay, kbps, srate, aplay;
    fmvv_info(&w, &h, &vplay);
    fmva_info(&kbps, &srate, &aplay);
    char vid[64], aud[64];
    if (vplay && w) snprintf(vid, sizeof(vid), "%dx%d, %.1f img/s", w, h, rate);
    else            snprintf(vid, sizeof(vid), "arr\xC3\xAAt\xC3\xA9" "e");
    if (aplay && kbps) snprintf(aud, sizeof(aud), "%d kbit/s, %.1f kHz", kbps, srate / 1000.0);
    else               snprintf(aud, sizeof(aud), "arr\xC3\xAAt\xC3\xA9");
    snprintf(out, n, "MPEG : vid\xC3\xA9o %s | son %s", vid, aud);
}
