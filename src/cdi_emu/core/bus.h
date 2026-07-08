#ifndef BUS_H
#define BUS_H
#include <stdint.h>

#include "./SLAVE/slave_backend.h"
#include "./CPU/uart.h"

typedef struct bus {
    uint8_t dram[0x100000];   /* 1 MB   @ 0x000000 */
    uint8_t rom [0x080000];   /* 512 KB @ 0x180000 */
    uint8_t nvram[0x004000];  /* 16 KB  @ 0x3F8000 */
    uart_t  uart;             /* UART peripheral state */
    int trace;
    slave_backend_t *slave;
} bus_t;

bus_t *bus_create(void);
void   bus_destroy(bus_t *b);
int    bus_load_ihex(bus_t *b, const char *path);

/* Internal global accessor (used by uart.c) */
bus_t *bus_get_global(void);

uint8_t  bus_read8 (bus_t *b, uint32_t addr);
uint16_t bus_read16(bus_t *b, uint32_t addr);
uint32_t bus_read32(bus_t *b, uint32_t addr);
void bus_write8 (bus_t *b, uint32_t addr, uint8_t  v);
void bus_write16(bus_t *b, uint32_t addr, uint16_t v);
void bus_write32(bus_t *b, uint32_t addr, uint32_t v);

/* ===== Address helpers (used by bus_read8/bus_write8) ===== */
static inline int is_dram(uint32_t a)  { return a < 0x100000; }
static inline int is_nvram(uint32_t a) { return a >= 0x3F8000 && a < 0x3FC000; }
static inline int is_rom(uint32_t a)   { return a >= 0x180000 && a < 0x200000; }
static inline uint32_t rom_off(uint32_t a) { return a - 0x180000; }
static inline uint32_t nv_off(uint32_t a)  { return a - 0x3F8000; }

#endif
