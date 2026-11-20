/*
 * bus.h - Memory bus and MMIO dispatch for the CD-i 205 emulator
 *
 * The bus routes all memory reads and writes to the appropriate
 * device: DRAM, ROM, NVRAM, UART, video controller, slave processor,
 * CDIC, or general I/O registers.
 */

#ifndef BUS_H
#define BUS_H
#include <stdint.h>

#include "./SLAVE/slave_backend.h"
#include "./CPU/uart.h"
#include "./GPU/scc66470.h"

/*
 * Main bus structure
 * Contains memory regions and peripheral state accessible via the bus.
 */
typedef struct bus {
    uint8_t dram[0x100000];   /* 1 MB DRAM mapped at $000000 */
    uint8_t ram2[0x080000];   /* 512 KB RAM bank mapped at $500000 */
    uint8_t rom [0x080000];   /* 512 KB ROM mapped at $180000 */
    uint8_t nvram[0x004000];  /* 16 KB NVRAM mapped at $3F8000 */
    uart_t  uart;             /* UART peripheral state */
    int trace;                /* Trace flag (unused in current code) */
    slave_backend_t *slave;   /* Slave co-processor backend */
} bus_t;

/* Lifecycle */
bus_t *bus_create(void);
void   bus_destroy(bus_t *b);

/* Load a ROM image from an Intel HEX file into the bus ROM region */
int    bus_load_ihex(bus_t *b, const char *path);

/* Global bus accessor (used by UART callbacks that lack a bus pointer) */
bus_t *bus_get_global(void);

/* Bus read/write primitives - 8, 16, and 32-bit */
uint8_t  bus_read8 (bus_t *b, uint32_t addr);
uint16_t bus_read16(bus_t *b, uint32_t addr);
uint32_t bus_read32(bus_t *b, uint32_t addr);
void bus_write8 (bus_t *b, uint32_t addr, uint8_t  v);
void bus_write16(bus_t *b, uint32_t addr, uint16_t v);
void bus_write32(bus_t *b, uint32_t addr, uint32_t v);

/* Address range helpers used by bus_read8/bus_write8 */
static inline int is_dram(uint32_t a)  { return a < 0x100000; }
static inline int is_ram2(uint32_t a)  { return a >= 0x500000 && a < 0x580000; }
static inline int is_nvram(uint32_t a) { return a >= 0x3F8000 && a < 0x3FC000; }
static inline int is_rom(uint32_t a)   { return a >= 0x180000 && a < 0x200000; }
static inline uint32_t rom_off(uint32_t a) { return a - 0x180000; }
static inline uint32_t nv_off(uint32_t a)  { return a - 0x3F8000; }
static inline uint32_t ram2_off(uint32_t a){ return a - 0x500000; }

scc66470_t *bus_get_video(bus_t *b);

struct onchip68070;
struct onchip68070 *bus_get_onchip(bus_t *b);

#endif /* BUS_H */
