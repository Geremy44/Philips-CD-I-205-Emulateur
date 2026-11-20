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

/* Shadow array for I/O registers not handled by specific devices */
static uint8_t io_regs[IO_SIZE];

/* Accessor for uart.c to read/write the raw I/O shadow array */
uint8_t *uart_io_regs_ptr(void) { return io_regs; }

#define NVRAM_FILE "nvram.bin"

/*
 * Load NVRAM contents from disk. If the file doesn't exist or is
 * the wrong size, NVRAM starts zeroed.
 */
static void nvram_load(bus_t *b) {
    FILE *f = fopen(NVRAM_FILE, "rb");
    if (!f) {
        printf("[NVRAM] No '%s' file, starting fresh\n", NVRAM_FILE);
        return;
    }
    size_t n = fread(b->nvram, 1, sizeof(b->nvram), f);
    fclose(f);
    if (n == sizeof(b->nvram))
        printf("[NVRAM] Loaded from '%s' (%zu bytes)\n", NVRAM_FILE, n);
    else
        printf("[NVRAM] '%s' unexpected size (%zu/%zu), partial load\n",
               NVRAM_FILE, n, sizeof(b->nvram));
}

/* Save NVRAM contents to disk on shutdown */
static void nvram_save(bus_t *b) {
    FILE *f = fopen(NVRAM_FILE, "wb");
    if (!f) {
        fprintf(stderr, "[NVRAM] ERROR: cannot write '%s'\n", NVRAM_FILE);
        return;
    }
    size_t n = fwrite(b->nvram, 1, sizeof(b->nvram), f);
    fclose(f);
    printf("[NVRAM] Saved to '%s' (%zu bytes)\n", NVRAM_FILE, n);
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

uint8_t bus_read8(bus_t *b, uint32_t addr) {

    /* UART register access (any address matching the UART base pattern) */
    if ((addr & 0xFFFFFFF0u) == 0x80002010u) {
        return uart_read8(b, addr);
    }

    /* CDIC RAM */
    if (cdic_is_ram(addr))
        return cdic_ram_read8(&g_cdic, addr);

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
    if (is_rom(addr))   return b->rom[rom_off(addr)];

    return io_read_stub(addr) & 0xFF;
}

uint16_t bus_read16(bus_t *b, uint32_t addr) {
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
    if (addr >= 0x300000 && addr <= 0x303FFF) {
        return cdic_read16(addr);
    }

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

    /* CDIC RAM */
    if (cdic_is_ram(addr)) { cdic_ram_write8(&g_cdic, addr, v); return; }

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
        if ((addr & 0xFFFF00) == 0x002000) { uart_write8(b, addr, v); return; }
        io_regs[addr - IO_BASE] = v;
        return;
    }

    /* Standard memory regions */
    if (is_dram(addr))  { b->dram[addr] = v; return; }
    if (is_ram2(addr))  { b->ram2[ram2_off(addr)] = v; return; }
    if (is_nvram(addr)) { b->nvram[nv_off(addr)] = v; return; }
    if (is_rom(addr))   return;  /* ROM is read-only */

    io_write_stub(addr, v);
}

void bus_write16(bus_t *b, uint32_t addr, uint16_t v) {

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
        b->nvram[o]     = v >> 8;
        b->nvram[o + 1] = v & 0xFF;
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
