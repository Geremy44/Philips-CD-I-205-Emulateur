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
#include "rtc_m48t08.h"
#include <time.h>

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

    rtc_m48t08_t rtc;         /* M48T08 clock at $3FBFF0-$3FBFFE */
    int     nvram_dirty;      /* NVRAM changed since the last save */
    time_t  nvram_last_write; /* host time of the last NVRAM change */
} bus_t;

/* Lifecycle */
bus_t *bus_create(void);
void   bus_destroy(bus_t *b);

/* Load a ROM image from an Intel HEX file into the bus ROM region */
int    bus_load_ihex(bus_t *b, const char *path);
/* raw 512 KB console ROM in the byte order of the dumps (as ROM import writes it) */
int    bus_load_rom_bin(bus_t *b, const char *path);

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
/* NVRAM proper; $3FBFF0-$3FBFFF is the M48T08 clock (rtc_m48t08.c) */
static inline int is_nvram(uint32_t a) { return a >= 0x3F8000 && a < 0x3FBFF0; }
static inline int is_rom(uint32_t a)   { return a >= 0x180000 && a < 0x200000; }
static inline uint32_t rom_off(uint32_t a) { return a - 0x180000; }
static inline uint32_t nv_off(uint32_t a)  { return a - 0x3F8000; }
static inline uint32_t ram2_off(uint32_t a){ return a - 0x500000; }

scc66470_t *bus_get_video(bus_t *b);

/* ---- NVRAM / RTC persistence ----
 * nvram.bin (16 KB raw image) + nvram.rtc (clock offset), in the current
 * directory, or <EMU_NVRAM>.bin / <EMU_NVRAM>.rtc when EMU_NVRAM is set.
 * bus_nvram_poll(): autosave 2 s after the last NVRAM change.
 * Slots: saves/slot<N>.sav (NVRAM + clock), N = 1..9. */
void bus_nvram_poll(bus_t *b);
int  bus_nvram_flush(bus_t *b);
int  bus_save_slot(bus_t *b, int slot);
int  bus_load_slot(bus_t *b, int slot);
scc66470_t *bus_get_video_slave(bus_t *b);

struct onchip68070;
struct onchip68070 *bus_get_onchip(bus_t *b);
struct cdic_s;
struct cdic_s *bus_get_cdic(bus_t *b);
extern uint32_t bus_watch_pc;        /* CPU PC, for EMU_WATCH reports */
extern int      bus_watch_on;        /* EMU_WATCH active (after EMU_WATCH_AFTER steps) */

#endif /* BUS_H */
