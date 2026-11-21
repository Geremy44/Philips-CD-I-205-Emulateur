/*
 * bus.c - Memory bus implementation for the CD-i 205 emulator
 *
 * Routes all memory accesses to the appropriate device or memory region.
 * The CD-i 205 address space is organized as follows:
 *
 *   $000000-$0FFFFF  DRAM (1 MB)
 *   $180000-$1FFFFF  ROM  (512 KB, firmware)
 *   $1FFFC0-$1FFFFF  SCC66470 video controller registers
 *   $200000-$207FFF  Slave co-processor registers
 *   $300000-$303FFF  CDIC RAM
 *   $3F8000-$3FBFFF  NVRAM (16 KB, battery-backed)
 *   $80002010-$8000201F  UART registers
 *   $80000000+      General I/O registers
 */

#include "bus.h"
#include "rom_loader.h"
#include "./GPU/scc66470.h"
#include "./SLAVE/slave.h"
#include "./CPU/uart.h"
#include "./CDIC/cdic.h"
#include "./CPU/onchip68070.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

/* I/O register shadow space for non-decoded registers */
#define IO_BASE  0x80000000
#define IO_SIZE  0x00080000

/* Video register window access logging (enabled via EMU_VIDEO_LOG env var) */
static int video_log_state = -1;
static int video_log_on(void) {
    if (video_log_state < 0)
        video_log_state = (getenv("EMU_VIDEO_LOG") != NULL) ? 1 : 0;
    return video_log_state;
}

static void video_log(const char *op, uint32_t addr, uint16_t val) {
    if (video_log_on())
        fprintf(stderr, "[VDSC] %s %06X = %04X\n", op, addr, val);
}

/* Global bus pointer - allows UART callbacks to access the bus
 * without needing it passed as a parameter. Set in bus_create(),
 * cleared in bus_destroy(). */
static bus_t *g_bus = NULL;

bus_t *bus_get_global(void) { return g_bus; }

/* Shared video controller and CDIC instances used by the bus */
/* The 205 has two SCC66470: the master (registers $1FFFE0-$1FFFFF, plane A,
 * DRAM bank 1) and the slave (registers $1FFFC0-$1FFFDF, plane B, DRAM
 * bank 2). Slave addresses are shifted by +$20 so both share the register
 * definitions of scc66470.h. */
static scc66470_t g_video;
static scc66470_t g_video_slave;
scc66470_t *bus_get_video(bus_t *b)       { (void)b; return &g_video; }
scc66470_t *bus_get_video_slave(bus_t *b) { (void)b; return &g_video_slave; }

static scc66470_t *vsc_select(uint32_t *addr)
{
    if (*addr < 0x1FFFE0u) { *addr += 0x20u; return &g_video_slave; }
    return &g_video;
}
static cdic_t     g_cdic;

/* SCC68070 on-chip timer + interrupt priority registers */
static onchip68070_t g_onchip;
onchip68070_t *bus_get_onchip(bus_t *b) { (void)b; return &g_onchip; }
cdic_t        *bus_get_cdic(bus_t *b)   { (void)b; return &g_cdic; }

/* Shadow array for I/O registers not handled by specific devices */
static uint8_t io_regs[IO_SIZE];

/* EMU_WATCH=start-end (hex): report the first writes into that range, with
 * the CPU PC (bus_watch_pc) and whether the CDIC DMA did it */
uint32_t bus_watch_pc = 0;
int      bus_watch_on = 1;           /* main clears it until EMU_WATCH_AFTER */
static uint32_t watch_lo = 1, watch_hi = 0;
static int watch_left = 40, watch_init = 0, in_dma = 0;
static void watch(uint32_t addr, uint32_t v, int size)
{
    if (!watch_init) {
        watch_init = 1;
        const char *e = getenv("EMU_WATCH");
        if (e) sscanf(e, "%x-%x", &watch_lo, &watch_hi);
    }
    if (bus_watch_on && addr >= watch_lo && addr <= watch_hi && watch_left > 0) {
        watch_left--;
        fprintf(stderr, "[WATCH] W%d %06X = %0*X  pc=%06X%s\n", size, addr, size / 4, v,
                bus_watch_pc, in_dma ? " (CDIC DMA)" : "");
    }
}

/* ===== SCC68070 DMA controller, channel 1 ($80004000) <-> CDIC =====
 * Used by cdapdriv ($1A961A): CSR=$FF (clear), MAR (+$0C) = destination,
 * MTC (+$0A) = words, OCR (+$05) $92 = device -> memory, CCR (+$07) $80 =
 * start, then CDIC DMACTL ($303FF8) = buffer offset | $8000 (| $4000 for
 * memory -> CDIC). The driver polls CSR bit 7 (COC), then checks bit 4
 * (ERR) and reads the advanced MAR back. CSR bits are "write 1 to clear";
 * bit 3 (channel active) must read 0 before a new transfer. */
#define DMA1      0x4000u           /* offset of channel 1 in io_regs */
#define DMA_CSR   0x00
#define DMA_CER   0x01
#define DMA_OCR   0x05
#define DMA_CCR   0x07
#define DMA_MTC   0x0A
#define DMA_MAR   0x0C

static int is_dma_csr(uint32_t addr)
{
    return addr == IO_BASE + DMA1 + DMA_CSR || addr == IO_BASE + DMA1 + 0x40 + DMA_CSR;
}

static void cdic_dma(bus_t *b, uint16_t ctl)
{
    uint8_t *r = &io_regs[DMA1];
    if (!(ctl & 0x8000)) return;
    if (!(r[DMA_CCR] & 0x80)) {
        fprintf(stderr, "[DMA] CDIC DMACTL=%04X but channel 1 not started\n", ctl);
        return;
    }
    uint32_t mtc = ((uint32_t)r[DMA_MTC] << 8) | r[DMA_MTC + 1];
    uint32_t mar = ((uint32_t)r[DMA_MAR] << 24) | ((uint32_t)r[DMA_MAR + 1] << 16)
                 | ((uint32_t)r[DMA_MAR + 2] << 8) | r[DMA_MAR + 3];
    uint32_t off = ctl & 0x3FFE;
    int to_cdic = (ctl & 0x4000) != 0;
    in_dma = 1;
    if (getenv("EMU_DMA_LOG"))
        fprintf(stderr, "[DMA] %s %u words, CDIC +%04X %s MAR %06X (pc=%06X)\n",
                to_cdic ? "mem->CDIC" : "CDIC->mem", mtc, off, to_cdic ? "<-" : "->", mar, bus_watch_pc);
    for (uint32_t n = 0; n < mtc; n++) {
        uint32_t c = CDIC_RAM_BASE + ((off + 2 * n) & 0x3FFE);
        if (to_cdic) cdic_write16(&g_cdic, c, bus_read16(b, mar));
        else         bus_write16(b, mar, cdic_read16(&g_cdic, c));
        mar += 2;
    }
    r[DMA_MAR] = (uint8_t)(mar >> 24); r[DMA_MAR + 1] = (uint8_t)(mar >> 16);
    r[DMA_MAR + 2] = (uint8_t)(mar >> 8); r[DMA_MAR + 3] = (uint8_t)mar;
    in_dma = 0;
    r[DMA_MTC] = r[DMA_MTC + 1] = 0;
    r[DMA_CCR] &= 0x7F;                       /* operation done */
    r[DMA_CSR] = (uint8_t)((r[DMA_CSR] | 0x80) & ~0x18);   /* COC, not active, no error */
    r[DMA_CER] = 0;
}

/* Accessor for uart.c to read/write the raw I/O shadow array */
uint8_t *uart_io_regs_ptr(void) { return io_regs; }

/* ===== NVRAM / RTC persistence ===== */

#define NVRAM_BASENAME "nvram"
#define AUTOSAVE_DELAY 2          /* seconds after the last NVRAM change */
#define SAVE_MAGIC     "CDI205SV"
#define SAVE_VERSION   1u
#define SAVE_HDR       36         /* bytes before the NVRAM image */

static void nvram_paths(char *bin, char *rtc, size_t n)
{
    const char *base = getenv("EMU_NVRAM");
    if (!base || !*base) base = NVRAM_BASENAME;
    snprintf(bin, n, "%s.bin", base);
    snprintf(rtc, n, "%s.rtc", base);
}

/* Write a file atomically: temp file, then rename (never a half-written save) */
static int write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || n != len) { remove(tmp); return -1; }
    remove(path);                         /* rename() does not overwrite on Windows */
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

static void nvram_load(bus_t *b)
{
    char bin[512], rtc[512];
    nvram_paths(bin, rtc, sizeof(bin));
    FILE *f = fopen(bin, "rb");
    if (!f) {
        printf("[NVRAM] No '%s' file, starting fresh\n", bin);
    } else {
        size_t n = fread(b->nvram, 1, sizeof(b->nvram), f);
        fclose(f);
        if (n == sizeof(b->nvram))
            printf("[NVRAM] Loaded from '%s' (%zu bytes)\n", bin, n);
        else
            printf("[NVRAM] '%s' unexpected size (%zu/%zu), partial load\n",
                   bin, n, sizeof(b->nvram));
    }
    if (rtc_load(&b->rtc, rtc) == 0)
        printf("[RTC] Clock offset loaded from '%s' (%lld s)\n", rtc, (long long)b->rtc.offset);
    else
        printf("[RTC] No '%s', the clock follows the host time\n", rtc);
}

int bus_nvram_flush(bus_t *b)
{
    char bin[512], rtc[512];
    nvram_paths(bin, rtc, sizeof(bin));
    int err = write_atomic(bin, b->nvram, sizeof(b->nvram));
    if (rtc_save(&b->rtc, rtc) != 0) err = -1;
    if (err) fprintf(stderr, "[NVRAM] ERROR: cannot write '%s' / '%s'\n", bin, rtc);
    else     b->nvram_dirty = 0;
    return err;
}

static void nvram_save(bus_t *b)
{
    if (bus_nvram_flush(b) == 0) {
        char bin[512], rtc[512];
        nvram_paths(bin, rtc, sizeof(bin));
        printf("[NVRAM] Saved to '%s' + '%s'\n", bin, rtc);
    }
}

static void nvram_touch(bus_t *b)
{
    b->nvram_dirty = 1;
    b->nvram_last_write = time(NULL);
}

void bus_nvram_poll(bus_t *b)
{
    if (b->nvram_dirty && time(NULL) - b->nvram_last_write >= AUTOSAVE_DELAY) {
        if (bus_nvram_flush(b) == 0)
            fprintf(stderr, "[NVRAM] autosaved\n");
    }
}

/* Save slot file (little-endian):
 *   magic[8] "CDI205SV", version u32, rtc offset i64, stopped u32,
 *   stopped_at i64, control u32, then the 16 KB NVRAM image */
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_i64(uint8_t *p, int64_t v)  { for (int i = 0; i < 8; i++) p[i] = (uint8_t)((uint64_t)v >> (8 * i)); }
static uint32_t get_u32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static int64_t  get_i64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return (int64_t)v; }

static void slot_path(char *out, size_t n, int slot)
{
    snprintf(out, n, "saves/slot%d.sav", slot);
}

int bus_save_slot(bus_t *b, int slot)
{
    static uint8_t buf[SAVE_HDR + 0x4000];
    char path[256];
    memcpy(buf, SAVE_MAGIC, 8);
    put_u32(buf + 8,  SAVE_VERSION);
    put_i64(buf + 12, b->rtc.offset);
    put_u32(buf + 20, (uint32_t)b->rtc.stopped);
    put_i64(buf + 24, b->rtc.stopped_at);
    put_u32(buf + 32, b->rtc.control & 0x3Fu);
    memcpy(buf + SAVE_HDR, b->nvram, sizeof(b->nvram));
#ifdef _WIN32
    _mkdir("saves");
#else
    mkdir("saves", 0777);
#endif
    slot_path(path, sizeof(path), slot);
    if (write_atomic(path, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "[SAVE] ERROR: cannot write '%s'\n", path);
        return -1;
    }
    fprintf(stderr, "[SAVE] NVRAM + clock saved to '%s'\n", path);
    return 0;
}

int bus_load_slot(bus_t *b, int slot)
{
    static uint8_t buf[SAVE_HDR + 0x4000];
    char path[256];
    slot_path(path, sizeof(path), slot);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[SAVE] '%s' not found\n", path); return -1; }
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n != sizeof(buf) || memcmp(buf, SAVE_MAGIC, 8) != 0 || get_u32(buf + 8) != SAVE_VERSION) {
        fprintf(stderr, "[SAVE] '%s' is not a valid save (%zu bytes)\n", path, n);
        return -1;
    }
    b->rtc.offset     = get_i64(buf + 12);
    b->rtc.stopped    = (int)get_u32(buf + 20);
    b->rtc.stopped_at = get_i64(buf + 24);
    b->rtc.control    = (uint8_t)(get_u32(buf + 32) & 0x3Fu);
    b->rtc.staged     = 0;
    memcpy(b->nvram, buf + SAVE_HDR, sizeof(b->nvram));
    nvram_touch(b);                       /* the recalled state becomes the current one */
    fprintf(stderr, "[SAVE] NVRAM + clock recalled from '%s'\n", path);
    return 0;
}

/* ===== Bus lifecycle ===== */

bus_t *bus_create(void) {
    bus_t *b = calloc(1, sizeof(bus_t));
    if (!b) return NULL;

    /* Initialize memory regions to power-on defaults */
    memset(b->rom,   0xFF, sizeof(b->rom));    /* Virgin EPROM state */
    memset(b->dram,  0x00, sizeof(b->dram));
    memset(b->ram2,  0x00, sizeof(b->ram2));
    memset(b->nvram, 0x00, sizeof(b->nvram));
    rtc_init(&b->rtc);
    nvram_load(b);
    b->trace = 1;

    g_bus = b;

    /* Initialize on-chip peripherals */
    uart_init();
    scc66470_init(&g_video);
    scc66470_init(&g_video_slave);
    g_video_slave.ica_pointer = 0x80400u;   /* Table 8: slave dual, bank 1 disabled */

    /* Set up the slave co-processor (HLE mode) */
    b->slave = slave_hle_create();
    b->slave->reset(b->slave->ctx);

    cdic_init(&g_cdic);
    onchip68070_reset(&g_onchip);

    return b;
}

void bus_destroy(bus_t *b) {
    fprintf(stderr, "[BUS] bus_destroy called (b=%p)\n", (void*)b);
    if (b) nvram_save(b);
    g_bus = NULL;
    free(b);
}

int bus_load_ihex(bus_t *b, const char *path) {
    size_t size = 0;
    uint8_t *rom_data = load_rom_hex(path, &size);

    if (!rom_data) {
        fprintf(stderr, "Failed to load ROM\n");
        return -1;
    }

    if (size > sizeof(b->rom)) {
        fprintf(stderr, "ROM file too large (%zu > %zu)\n",
                size, sizeof(b->rom));
        free(rom_data);
        return -1;
    }

    memcpy(b->rom, rom_data, size);
    free(rom_data);
    return 0;
}

/* ===== Stub handlers for unmapped I/O ===== */

static uint16_t io_read_stub(uint32_t addr) {
    /* Candidate VDSC register windows: log silent 0xFFFF reads */
    if ((addr >= 0x4FFE00 && addr <= 0x4FFFFF) ||
        (addr >= 0x1FFFC0 && addr <= 0x1FFFFF)) {
        if (addr < 0x1FFFC0 || addr > 0x1FFFFF)
            video_log("R?", addr, 0xFFFF);
    }
    return 0xFFFF;
}

static void io_write_stub(uint32_t addr, uint16_t v) {
    (void)v;
    /* Candidate VDSC register windows */
    if ((addr >= 0x4FFE00 && addr <= 0x4FFFFF)) {
        video_log("W?", addr, v);
        return;
    }
    /* Silently ignore writes to known unmapped regions */
    if (addr >= 0x1FFC00 && addr <= 0x1FFFFF) return;
    if (addr >= 0x200000 && addr <= 0x207FFF) return;
    if (addr >= 0x280000 && addr <= 0x28FFFF) return;
    if (addr >= 0x300000 && addr <= 0x30FFFF) return;
    if (addr >= 0x3FBFF0 && addr <= 0x3FBFFF) return;
    //fprintf(stderr, "BUS WRITE UNMAPPED %06X = %04X\n", addr, v);
}

/* ===== Bus read/write - 8/16/32 bit ===== */

/* SCC68070 address space: the external bus has 24 address lines, so bits
 * 31-24 are ignored ($40076CDC reads $076CDC -- Asterix keeps flags in the
 * top byte of its pointers); only the on-chip peripherals ($80000000+) are
 * decoded on 32 bits. */
static inline uint32_t addr24(uint32_t addr)
{
    return (addr & 0x80000000u) ? addr : (addr & 0x00FFFFFFu);
}

uint8_t bus_read8(bus_t *b, uint32_t addr) {
    addr = addr24(addr);

    /* UART register access (any address matching the UART base pattern) */
    if ((addr & 0xFFFFFFF0u) == 0x80002010u) {
        return uart_read8(b, addr);
    }

    /* CDIC RAM */
    if (cdic_is_ram(addr))
        return cdic_read8(&g_cdic, addr);

    /* SCC66470 video controller registers */
    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF) {
        uint32_t a = addr;
        scc66470_t *vsc = vsc_select(&a);
        uint8_t v = scc66470_read8(vsc, a);
        video_log("R8", addr, v);
        return v;
    }

    /* Slave co-processor registers */
    if (addr >= 0x200000 && addr <= 0x207FFF) {
        uint8_t v = b->slave->read8(b->slave->ctx, addr);
        return v;
    }

    /* UART registers (canonical range check) */
    if (addr >= UART_BASE && addr < UART_BASE + UART_SIZE) {
        return uart_read8(b, addr);
    }

    /* SCC68070 timer / PICR */
    if (onchip68070_handles(addr))
        return onchip68070_read8(&g_onchip, addr);

    /* General I/O register shadow */
    if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE) {
        return io_regs[addr - IO_BASE];
    }

    /* Standard memory regions */
    if (is_dram(addr))  return b->dram[addr];
    if (is_ram2(addr))  return b->ram2[ram2_off(addr)];
    if (is_nvram(addr)) return b->nvram[nv_off(addr)];
    if (rtc_handles(addr)) return rtc_read8(&b->rtc, addr);
    if (is_rom(addr))   return b->rom[rom_off(addr)];

    return io_read_stub(addr) & 0xFF;
}

uint16_t bus_read16(bus_t *b, uint32_t addr) {
    addr = addr24(addr);
    /* SCC66470 16-bit register access */
    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF)
    {
        uint32_t a = addr;
        scc66470_t *vsc = vsc_select(&a);
        return scc66470_read16(vsc, a);
    }

    /* Slave co-processor: assemble from two 8-bit reads */
    if (addr >= 0x200000 && addr <= 0x207FFF)
        return ((uint16_t)bus_read8(b, addr) << 8) | bus_read8(b, addr + 1);

    /* CDIC 16-bit register access */
    if (cdic_is_ram(addr))
        return cdic_read16(&g_cdic, addr);

    /* DRAM (big-endian) */
    if (is_dram(addr)) {
        return ((uint16_t)b->dram[addr] << 8) | b->dram[addr + 1];
    }
    /* RAM bank 2 (big-endian) */
    if (is_ram2(addr)) {
        uint32_t o = ram2_off(addr);
        return ((uint16_t)b->ram2[o] << 8) | b->ram2[o + 1];
    }
    /* NVRAM (big-endian) */
    if (is_nvram(addr)) {
        uint32_t o = nv_off(addr);
        return ((uint16_t)b->nvram[o] << 8) | b->nvram[o + 1];
    }
    /* ROM (big-endian) */
    if (is_rom(addr)) {
        uint32_t o = rom_off(addr);
        return ((uint16_t)b->rom[o] << 8) | b->rom[o + 1];
    }

    /* Fallback: assemble from two 8-bit reads (covers MMIO) */
    return ((uint16_t)bus_read8(b, addr) << 8) | bus_read8(b, addr + 1);
}

uint32_t bus_read32(bus_t *b, uint32_t addr) {
    return ((uint32_t)bus_read16(b, addr) << 16) | bus_read16(b, addr + 2);
}

void bus_write8(bus_t *b, uint32_t addr, uint8_t v) {
    addr = addr24(addr);
    watch(addr, v, 8);

    /* CDIC RAM */
    if (cdic_is_ram(addr)) {
        cdic_write8(&g_cdic, addr, v);
        if ((addr & 0x3FFF) == 0x3FF9) cdic_dma(b, cdic_read16(&g_cdic, addr & ~1u));
        return;
    }

    /* SCC66470 video controller registers */
    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF) {
        uint32_t a = addr;
        scc66470_t *vsc = vsc_select(&a);
        scc66470_write8(vsc, a, v);
        video_log("W8", addr, v);
        return;
    }

    /* Slave co-processor registers */
    if (addr >= 0x200000 && addr <= 0x207FFF) {
        b->slave->write8(b->slave->ctx, addr, v);
        return;
    }

    /* General I/O registers (includes UART sub-range) */
    if (addr >= IO_BASE && addr < (IO_BASE + IO_SIZE)) {
        if (onchip68070_handles(addr)) { onchip68070_write8(&g_onchip, addr, v); return; }
        if (is_dma_csr(addr)) { io_regs[addr - IO_BASE] &= (uint8_t)~v; return; }  /* write 1 to clear */
        if ((addr & 0xFFFF00) == 0x002000) { uart_write8(b, addr, v); return; }
        io_regs[addr - IO_BASE] = v;
        return;
    }

    /* Standard memory regions */
    if (is_dram(addr))  { b->dram[addr] = v; return; }
    if (is_ram2(addr))  { b->ram2[ram2_off(addr)] = v; return; }
    if (is_nvram(addr)) {
        if (b->nvram[nv_off(addr)] != v) { b->nvram[nv_off(addr)] = v; nvram_touch(b); }
        return;
    }
    if (rtc_handles(addr)) { rtc_write8(&b->rtc, addr, v); nvram_touch(b); return; }
    if (is_rom(addr))   return;  /* ROM is read-only */

    io_write_stub(addr, v);
}

void bus_write16(bus_t *b, uint32_t addr, uint16_t v) {
    addr = addr24(addr);
    watch(addr, v, 16);

    /* CDIC RAM and registers */
    if (cdic_is_ram(addr)) {
        cdic_write16(&g_cdic, addr, v);
        if ((addr & 0x3FFE) == 0x3FF8) cdic_dma(b, v);
        return;
    }

    /* SCC66470 video controller 16-bit registers */
    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF) {
        uint32_t a = addr;
        scc66470_t *vsc = vsc_select(&a);
        scc66470_write16(vsc, a, v);
        return;
    }

    /* DRAM (big-endian) */
    if (is_dram(addr)) {
        b->dram[addr]     = v >> 8;
        b->dram[addr + 1] = v & 0xFF;
        return;
    }
    /* RAM bank 2 (big-endian) */
    if (is_ram2(addr)) {
        uint32_t o = ram2_off(addr);
        b->ram2[o]     = v >> 8;
        b->ram2[o + 1] = v & 0xFF;
        return;
    }
    /* NVRAM (big-endian) */
    if (is_nvram(addr)) {
        uint32_t o = nv_off(addr);
        if (b->nvram[o] != (uint8_t)(v >> 8) || b->nvram[o + 1] != (uint8_t)v) {
            b->nvram[o]     = v >> 8;
            b->nvram[o + 1] = v & 0xFF;
            nvram_touch(b);
        }
        return;
    }

    /* Fallback: decompose into two 8-bit writes (covers CDIC, MMIO) */
    bus_write8(b, addr,     (uint8_t)(v >> 8));
    bus_write8(b, addr + 1, (uint8_t)(v & 0xFF));

    io_write_stub(addr, v);
}

void bus_write32(bus_t *b, uint32_t addr, uint32_t v) {
    bus_write16(b, addr,     (uint16_t)(v >> 16));
    bus_write16(b, addr + 2, (uint16_t)(v & 0xFFFF));
}
