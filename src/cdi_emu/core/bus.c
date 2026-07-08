#include "bus.h"
#include "rom_loader.h"
#include "scc66470.h"
#include "slave.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_BASE  0x80000000
#define IO_SIZE  0x00080000

#define UART_BASE  0x80002010
#define UART_SIZE  0x10

/* Global bus pointer so uart_feed() / uart_poll_host_input() work
   without having the bus_t* passed down from main.  Set in bus_create(),
   cleared in bus_destroy(). */
static bus_t *g_bus = NULL;

bus_t *bus_get_global(void) { return g_bus; }

static scc66470_t g_video;

static uint8_t io_regs[IO_SIZE];

/* Accessor so uart.c can touch the raw io_regs shadow array
   without bus.c exposing it globally. */
uint8_t *uart_io_regs_ptr(void) { return io_regs; }

/* ===== Bus create / destroy ===== */

bus_t *bus_create(void) {
    bus_t *b = calloc(1, sizeof(bus_t));
    if (!b) return NULL;

    /* pre-fill ROM with 0xFF (virgin EPROM state) */
    memset(b->rom,   0xFF, sizeof(b->rom));
    memset(b->dram,  0x00, sizeof(b->dram));
    memset(b->nvram, 0x00, sizeof(b->nvram));
    b->trace = 1;

    g_bus = b;

    uart_init();
    scc66470_init(&g_video);

    b->slave = slave_hle_create();
    b->slave->reset(b->slave->ctx);

    return b;
}

void bus_destroy(bus_t *b) {
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

/* ===== I/O stubs ===== */
static uint16_t io_read_stub(uint32_t addr) {
    (void)addr;
    return 0xFFFF;
}

static void io_write_stub(uint32_t addr, uint16_t v) {
    (void)v;
    if (addr >= 0x1FFC00 && addr <= 0x1FFFFF) return;
    if (addr >= 0x200000 && addr <= 0x207FFF) return;
    if (addr >= 0x280000 && addr <= 0x28FFFF) return;
    if (addr >= 0x300000 && addr <= 0x30FFFF) return;
    if (addr >= 0x3FBFF0 && addr <= 0x3FBFFF) return;
    fprintf(stderr, "BUS WRITE UNMAPPED %06X = %04X\n", addr, v);
}

/* ===== Bus read / write 8 / 16 / 32 ===== */

uint8_t bus_read8(bus_t *b, uint32_t addr) {

    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF) {
        return scc66470_read8(&g_video, addr);
    }

    if (addr >= 0x200000 && addr <= 0x207FFF) {
        uint8_t v = b->slave->read8(b->slave->ctx, addr);
        fprintf(stderr, "[SLAVE RD] @%06X -> %02X\n", addr, v);
        return v;
    }

    /* ===== UART : 0x80002010-0x8000201F ===== */
    if (addr >= UART_BASE && addr < UART_BASE + UART_SIZE) {
        fprintf(stderr, "[BUS->UART RD8] addr=%08X\n", addr);
        return uart_read8(b, addr);
    }

    if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE) {
        return io_regs[addr - IO_BASE];
    }

    if (is_dram(addr))  return b->dram[addr];
    if (is_nvram(addr)) return b->nvram[nv_off(addr)];
    if (is_rom(addr))   return b->rom[rom_off(addr)];

    return io_read_stub(addr) & 0xFF;
}

uint16_t bus_read16(bus_t *b, uint32_t addr) {
    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF)
        return scc66470_read16(&g_video, addr);

    if (addr >= 0x200000 && addr <= 0x207FFF)   /* slave via 2x read8 */
        return ((uint16_t)bus_read8(b, addr) << 8) | bus_read8(b, addr + 1);

    if (is_dram(addr)) {
        return ((uint16_t)b->dram[addr] << 8) | b->dram[addr + 1];
    }
    if (is_nvram(addr)) {
        uint32_t o = nv_off(addr);
        return ((uint16_t)b->nvram[o] << 8) | b->nvram[o + 1];
    }
    if (is_rom(addr)) {
        uint32_t o = rom_off(addr);
        return ((uint16_t)b->rom[o] << 8) | b->rom[o + 1];
    }

    /* MMIO 16 bits : decompose en deux lectures 8 bits */
    return ((uint16_t)bus_read8(b, addr) << 8) | bus_read8(b, addr + 1);
}

uint32_t bus_read32(bus_t *b, uint32_t addr) {
    (void)b; (void)addr;
    return 0;
}

void bus_write8(bus_t *b, uint32_t addr, uint8_t v) {

    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF) {
        scc66470_write8(&g_video, addr, v);
        return;
    }

    /* ===== SLAVE 68HC05 ===== */
    if (addr >= 0x200000 && addr <= 0x207FFF) {
        fprintf(stderr, "[SLAVE WR] @%06X = %02X\n", addr, v);
        b->slave->write8(b->slave->ctx, addr, v);
        return;
    }

    if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE) {
        if ((addr & 0xFFFF00) == 0x002000) { uart_write8(b, addr, v); return; }
        io_regs[addr - IO_BASE] = v;
        return;
    }

    if (is_dram(addr))  { b->dram[addr] = v; return; }
    if (is_nvram(addr)) { b->nvram[nv_off(addr)] = v; return; }
    if (is_rom(addr))   return;

    io_write_stub(addr, v);
}

void bus_write16(bus_t *b, uint32_t addr, uint16_t v) {

    if (addr >= 0x1FFFC0 && addr <= 0x1FFFFF) {
        scc66470_write16(&g_video, addr, v);
        return;
    }

    if (is_dram(addr)) {
        b->dram[addr]     = v >> 8;
        b->dram[addr + 1] = v & 0xFF;
        return;
    }
    if (is_nvram(addr)) {
        uint32_t o = nv_off(addr);
        b->nvram[o]     = v >> 8;
        b->nvram[o + 1] = v & 0xFF;
        return;
    }

    io_write_stub(addr, v);
}

void bus_write32(bus_t *b, uint32_t addr, uint32_t v) {
    bus_write16(b, addr,     v >> 16);
    bus_write16(b, addr + 2, v & 0xFFFF);
}
