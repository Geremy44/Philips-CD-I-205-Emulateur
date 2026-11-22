/*
 * fmv.c - Digital Video Cartridge (see fmv.h)
 *
 * Board glue: ROM, 1 MB of RAM, the video chip's DRAM, the register window
 * of the two decoder chips (audio $E03000, video $E04000), the stream
 * transfers over the 68070 DMA channel 2 and the shared interrupt line
 * (level 4, vectored). The board behaviour follows MAME's cdidvc device
 * (pull request #16181, BSD-3-Clause, Alexandre Derumier, from the
 * CDi_MiSTer core); see fmv_video.c and fmv_audio.c for the chips.
 *
 * Portions derived from MAME: Copyright (c) Alexandre Derumier,
 * BSD-3-Clause; full notice in THIRD_PARTY_LICENSES.md at the repository root.
 */

#include "fmv.h"
#include "fmv_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#define ROM_BASE  0xE40000u
#define ROM_SIZE  0x20000u
#define VRAM_BASE 0xE80000u
#define VRAM_SIZE 0x80000u
#define REG_BASE  0xE00000u
#define REG_END   0xE3FFFFu
#define XRAM_SIZE 0x100000u   /* 1 MB extra DRAM (DRC), system memory */

static int      on = 0;
static uint8_t *rom = NULL, *vram = NULL, *xram = NULL;
static uint32_t xram_base = 0xD00000u;    /* EMU_FMV_RAM=hex to try another base */
static int      log_level = -1;
static FILE    *dump = NULL;
static int      dump_tried = 0;                /* EMU_FMV_DUMP=file: raw stream sent to the chips */
static unsigned long stream_bytes = 0;
uint64_t fmv_now = 0;
int      fmv_log = 0;
static int      drq_pending = -1;           /* chip waiting for a transfer: 0 video, 1 audio */
static int      xfer_audio = 0;             /* chip the transfer in progress belongs to */
static uint64_t vblank_end_at = FMV_NEVER;
static uint16_t rd_cache; static uint32_t rd_cache_addr = 1;  /* byte reads of a word register */
static uint8_t  wr_hi; static uint32_t wr_hi_addr = 1;        /* byte writes of a word register */
extern uint32_t bus_watch_pc;               /* PC of the current instruction (bus.c) */

int fmv_enabled(void) { return on; }

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

int fmv_rom_loaded(void) { return rom != NULL; }

void fmv_set_enabled(int enable)
{
    on = enable && rom != NULL;
    fprintf(stderr, "[FMV] cartridge %s\n", on ? "inserted" : "removed");
}

static void option_path(char *out, int n)
{
    char *base = SDL_GetBasePath();
    snprintf(out, n, "%soptions.ini", base ? base : "");
    SDL_free(base);
}

int fmv_option_load(void)
{
    char p[1100], line[128];
    option_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int v = -1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "fmv=", 4)) v = atoi(line + 4) != 0;
    fclose(f);
    return v;
}

void fmv_option_save(int enable)
{
    char p[1100];
    option_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "; CD-i 205 - options (menu Materiel)\nfmv=%d\n", enable ? 1 : 0);
    fclose(f);
}

int fmv_handles(uint32_t addr)
{
    return on && ((addr >= REG_BASE && addr <= 0xEFFFFFu) ||
                  (addr >= xram_base && addr < xram_base + XRAM_SIZE));
}

static int logging(void)
{
    if (log_level < 0) { const char *e = getenv("EMU_FMV_LOG"); log_level = e ? atoi(e) : 0; }
    return log_level;
}

/* The kernel's RAM probe of "Extension 1" ($500000-$FFFFFF, init module:
 * 5A/A5 every 32 KB, byte accesses, code in the 205 ROM) must find no
 * memory in the cartridge, or it puts system memory over the chips and the
 * FMV DRAM, and the drivers' writes then corrupt the memory lists (reboot
 * loop after fmvconf). On the real cartridge the chips and the 16-bit DRAM
 * do not answer such accesses; here: accesses made by the 205 ROM code see
 * an empty bus, the cartridge's own drivers see registers and RAM. */
static int from_base_rom(void)
{
    /* only the two probe routines of the 205 ROM (seen in the traces); the
     * kernel itself must use the FMV DRAM once fmvconf has declared it as
     * colored memory type $90 ($E80000, 512 KB) */
    uint32_t pc = bus_watch_pc;
    return (pc >= 0x1806F0u && pc < 0x180790u) || (pc >= 0x184820u && pc < 0x184870u);
}

/* ======================= stream and transfers ======================= */

extern int bus_dma2_transfer(void (*sink)(uint16_t));    /* bus.c, -1 if not armed */

static void to_chip(uint16_t w)
{
    if (dump) { fputc(w >> 8, dump); fputc(w & 0xFF, dump); }
    stream_bytes += 2;
    if (xfer_audio) { fmva_data((uint8_t)(w >> 8)); fmva_data((uint8_t)w); }
    else            { fmvv_data((uint8_t)(w >> 8)); fmvv_data((uint8_t)w); }
}

static void run_dma(void)
{
    if (drq_pending < 0) return;
    xfer_audio = drq_pending;
    int n = bus_dma2_transfer(to_chip);
    if (n < 0) return;                             /* channel not started yet */
    drq_pending = -1;
    if (fmv_log >= 2) fprintf(stderr, "[FMV] DMA %d words to the %s chip\n", n, xfer_audio ? "audio" : "video");
    if (xfer_audio) fmva_end_of_transfer(1); else fmvv_end_of_transfer();
}

void fmv_board_drq(int for_audio) { drq_pending = for_audio; run_dma(); }
void fmv_dma_started(void) { if (on) run_dma(); }

/* ======================= time, display, interrupts ======================= */

void fmv_reset(void)
{
    if (!rom) return;
    fmv_log = logging();
    if (!dump_tried) { dump_tried = 1; const char *e = getenv("EMU_FMV_DUMP"); if (e) dump = fopen(e, "wb"); }
    drq_pending = -1; xfer_audio = 0; vblank_end_at = FMV_NEVER;
    fmvv_reset();
    fmva_reset();
}

void fmv_tick(int cycles)
{
    if (!on) return;
    fmv_now += (uint64_t)cycles;
    if (fmv_now >= vblank_end_at) { vblank_end_at = FMV_NEVER; fmvv_vblank(0); }
    fmvv_tick();
    fmva_tick();
}

void fmv_frame(void)
{
    if (!on) return;
    fmvv_vblank(1);
    vblank_end_at = fmv_now + FMV_CPU_HZ * 16 / 10000;     /* 1.6 ms of blanking */
}

int fmv_irq(int *vector)
{
    if (!on) return 0;
    if (fmva_irq_pending()) { *vector = fmva_vector(); return 4; }   /* audio chip first */
    if (fmvv_irq_pending()) { *vector = fmvv_vector(); return 4; }
    return 0;
}

int fmv_video_pixel(int x, int y, uint32_t *rgb) { return on && fmvv_pixel(x, y, rgb); }
int fmv_video_showing(void) { return on && fmvv_showing(); }

/* ======================= register window ======================= */

uint16_t fmv_reg_read16(uint32_t addr)
{
    uint16_t reg = (uint16_t)((addr >> 1) & 0x7fff), d = 0;   /* mirrors every 64 KB */
    if ((reg & 0xff00) == 0x1800) d = fmva_read(reg & 0xff);
    else if ((reg & 0xff00) == 0x2000) d = fmvv_read(reg & 0xff);
    return d;
}

void fmv_reg_write16(uint32_t addr, uint16_t v)
{
    uint16_t reg = (uint16_t)((addr >> 1) & 0x7fff);
    if ((reg >> 8) == 0x08) return;               /* pixel clock select ($E01000) */
    if (reg == 0x206f) {                          /* transfer port ($E040DE), by hand */
        to_chip(v);
        if (xfer_audio) fmva_end_of_transfer(0); else fmvv_end_of_transfer();
        return;
    }
    if ((reg & 0xff00) == 0x1800) fmva_write(reg & 0xff, v);
    else if ((reg & 0xff00) == 0x2000) fmvv_write(reg & 0xff, v);
    else if (fmv_log >= 2) fprintf(stderr, "[FMV] W %06X = %04X pc=%06X\n", (unsigned)addr, v, (unsigned)bus_watch_pc);
}

static int is_rom(uint32_t a) { return a >= ROM_BASE && a < 0xE80000u; }   /* 128 KB mirrored */

uint16_t fmv_read16(uint32_t addr)
{
    if (is_rom(addr)) { uint32_t o = (addr - ROM_BASE) & (ROM_SIZE - 1); return (uint16_t)(rom[o] << 8 | rom[o + 1]); }
    if (addr >= xram_base && addr < xram_base + XRAM_SIZE) { uint32_t o = addr - xram_base; return (uint16_t)(xram[o] << 8 | xram[o + 1]); }
    if (from_base_rom()) return 0xFFFF;
    if (addr >= VRAM_BASE && addr < VRAM_BASE + VRAM_SIZE) { uint32_t o = addr - VRAM_BASE; return (uint16_t)(vram[o] << 8 | vram[o + 1]); }
    if (addr <= REG_END) return fmv_reg_read16(addr & ~1u);
    return 0xFFFF;
}

void fmv_write16(uint32_t addr, uint16_t v)
{
    if (addr >= xram_base && addr < xram_base + XRAM_SIZE) { uint32_t o = addr - xram_base; xram[o] = (uint8_t)(v >> 8); xram[o + 1] = (uint8_t)v; return; }
    if (from_base_rom()) return;
    if (addr >= VRAM_BASE && addr < VRAM_BASE + VRAM_SIZE) { uint32_t o = addr - VRAM_BASE; vram[o] = (uint8_t)(v >> 8); vram[o + 1] = (uint8_t)v; return; }
    if (addr >= REG_BASE && addr <= REG_END) fmv_reg_write16(addr & ~1u, v);
}

/* byte accesses: memory directly, registers as words (an even byte read
 * does the word access, the odd one that follows gets its low byte) */
uint8_t fmv_read8(uint32_t addr)
{
    if (is_rom(addr)) return rom[(addr - ROM_BASE) & (ROM_SIZE - 1)];
    if (addr >= xram_base && addr < xram_base + XRAM_SIZE) return xram[addr - xram_base];
    if (from_base_rom()) return 0xFF;
    if (addr >= VRAM_BASE && addr < VRAM_BASE + VRAM_SIZE) return vram[addr - VRAM_BASE];
    if (addr <= REG_END) {
        if ((addr & 1) && rd_cache_addr == (addr & ~1u)) { rd_cache_addr = 1; return (uint8_t)rd_cache; }
        rd_cache = fmv_reg_read16(addr & ~1u);
        rd_cache_addr = addr & ~1u;
        return (addr & 1) ? (uint8_t)rd_cache : (uint8_t)(rd_cache >> 8);
    }
    return 0xFF;
}

void fmv_write8(uint32_t addr, uint8_t v)
{
    if (addr >= xram_base && addr < xram_base + XRAM_SIZE) { xram[addr - xram_base] = v; return; }
    if (from_base_rom()) return;
    if (addr >= VRAM_BASE && addr < VRAM_BASE + VRAM_SIZE) { vram[addr - VRAM_BASE] = v; return; }
    if (addr >= REG_BASE && addr <= REG_END) {
        if (!(addr & 1)) { wr_hi = v; wr_hi_addr = addr; return; }
        uint16_t hi = (wr_hi_addr == (addr & ~1u)) ? wr_hi : 0;
        wr_hi_addr = 1;
        fmv_reg_write16(addr & ~1u, (uint16_t)(hi << 8 | v));
    }
}
