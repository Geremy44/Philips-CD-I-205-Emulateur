/*
 * slave.h - Slave co-processor (68HC05) interface
 *
 * The CD-i has a secondary 8-bit microcontroller (68HC05 family)
 * that handles CD-ROM servo control, input devices, and NVRAM.
 * This module provides a frontend that dispatches to either an
 * HLE (high-level emulation) or LLE (low-level emulation) backend.
 */

#ifndef SLAVE_H
#define SLAVE_H
#include "slave_backend.h"

/* Slave register addresses in the CPU address space */
#define SLAVE_REG_STAT  0x200005   /* Status register (read) */
#define SLAVE_REG_CMD   0x200007   /* Command register (write) */

/* Slave co-processor frontend state */
typedef struct {
    slave_backend_t *be;   /* Active backend (HLE or LLE) */
    int irq_line;           /* Current IRQ line state */
    struct bus *bus;        /* Bus reference for UART injection */
} slave_t;

/* Backend constructors */
slave_backend_t *slave_hle_create(void);
slave_backend_t *slave_lle_create(const uint8_t *hc05_rom, int rom_len);

/* Frontend operations */
void    slave_attach(slave_t *s, slave_backend_t *be);
void    slave_reset(slave_t *s);
uint8_t slave_read8 (slave_t *s, uint32_t addr);
void    slave_write8(slave_t *s, uint32_t addr, uint8_t val);
void    slave_tick  (slave_t *s, int master_cycles);
void    slave_push_input(slave_t *s, const uint8_t *report, int len);

/* Link the HLE backend's UART injection callback */
void slave_hle_set_uart(slave_backend_t *be, uart_inject_fn fn, void *bus);
/* Disc in the drive (1) or not (0): the HLE reports it with B0 messages */
void slave_hle_set_disc(slave_backend_t *be, int present, int notify);
/* Drive door: 1 = the shell opened it ($89), 2 = closed it ($92), 0 = none */
int  slave_hle_take_tray_event(slave_backend_t *be);
/* Host opens / closes the door (with or without a disc) and reports it */
void slave_hle_set_tray(slave_backend_t *be, int open, int disc_present);
/* 1 once if the CPU asked the 68HC05 for a reset ($8A on $200005) */
int  slave_hle_take_reset(slave_backend_t *be);
/* Call once per video frame (pointer: relative mode time-out) */
void slave_hle_frame(slave_backend_t *be);
/* 1 while the pointer driver recentres the cursor; *x, *y = CD-i pointer */
int  slave_hle_pointer_relative(slave_backend_t *be, int *x, int *y);

#endif /* SLAVE_H */
