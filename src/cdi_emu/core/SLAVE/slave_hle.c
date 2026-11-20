/*
 * slave_hle.c - High-Level Emulation of the 68HC05 slave co-processor
 *
 * Protocol, as read in the CD-RTOS drivers of the 205 ROM (see
 * tools/m68k_disasm.py; device descriptors "ptr", "pck", "slave"):
 *
 *   $200001  channel 0  "ptr"   (pointer driver, IRQ handler $1B08B4)
 *   $200003  channel 1  "pck"   (pckdriv)
 *   $200005  channel 2  "slave" (sldriv, IRQ handler $1B0FAE)
 *   $200007  channel 3  command register (sldriv writes 2(a3) = $200007)
 *
 * All three drivers share vector 26 (level 2 autovector, 68070 IN2N): the
 * 68HC05 asserts IN2N while it has bytes to deliver. Each IRQ handler reads
 * its channel; $FF means "nothing for me". A read pops one byte.
 *
 * Channel 2 replies (sldriv): header byte $F0..$F6 (bit 7 set) followed by
 * 7-bit data bytes (bit 7 clear):
 *   F0 + 2 bytes   F1 + 3 bytes   F2/F3/F4/F5/F6 + 1 byte
 * sldriv GetStat $5C sends the query on $200007 and sleeps (max 100 ticks)
 * for the matching reply: sub-code 0 -> F0, 3 -> F3, 7 -> F4, $14 -> F5,
 * $15 -> F6. The boot code also sends F6 and polls $200005 for "F6 <val>".
 * The meaning of the returned values is not known yet: 0 is returned.
 *
 * Channel 0 packets (pointer driver), absolute 10-bit position
 * (X in 4-bit-pixel columns, Y in half display lines: the shell halves Y):
 *   byte 0: 1 0 B1 B0 B2 X9 X8 X7   (bit 5 -> button 1, 4 -> button 0, 3 -> bit 15)
 *   byte 1: 0 X6..X0
 *   byte 2: 0 0 0 0 0 Y9 Y8 Y7
 *   byte 3: 0 Y6..Y0
 */

#include "slave.h"
#include "slave_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLAVE_BASE   0x200000u
#define NCHAN        4
#define FIFO_SIZE    256

typedef struct {
    uint8_t buf[FIFO_SIZE];
    int     head, count;
} fifo_t;

typedef struct {
    fifo_t  out[NCHAN];         /* bytes waiting to be read by the CPU */
    int     log;                /* EMU_SLAVE_LOG */

    /* pointer state */
    int     px, py, pbuttons;

    uart_inject_fn inject;
    void          *bus;
} hle_ctx_t;

static void fifo_push(fifo_t *f, uint8_t v)
{
    if (f->count < FIFO_SIZE) {
        f->buf[(f->head + f->count) % FIFO_SIZE] = v;
        f->count++;
    }
}

static int fifo_pop(fifo_t *f)
{
    if (!f->count) return -1;
    uint8_t v = f->buf[f->head];
    f->head = (f->head + 1) % FIFO_SIZE;
    f->count--;
    return v;
}

static void hle_reset(void *ctx)
{
    hle_ctx_t *c = ctx;
    uart_inject_fn inj = c->inject;
    void *bus = c->bus;
    memset(c, 0, sizeof(*c));
    c->inject = inj;
    c->bus    = bus;
    const char *s = getenv("EMU_SLAVE_LOG");
    c->log = (s && *s && strcmp(s, "0") != 0);
    c->px = 384; c->py = 240;
}

/* Reply on channel 2: header + n data bytes (7-bit) */
static void reply(hle_ctx_t *c, uint8_t header, int n, uint32_t value)
{
    fifo_push(&c->out[2], header);
    for (int i = n - 1; i >= 0; i--)
        fifo_push(&c->out[2], (uint8_t)((value >> (7 * i)) & 0x7F));
}

static void command(hle_ctx_t *c, uint8_t cmd)
{
    switch (cmd) {
    case 0xF0: reply(c, 0xF0, 2, 0); break;
    case 0xF3: reply(c, 0xF3, 1, 0); break;
    case 0xF4: reply(c, 0xF4, 1, 0); break;
    case 0xF5: reply(c, 0xF5, 1, 0); break;
    case 0xF6: reply(c, 0xF6, 1, 0); break;
    default:   break;                 /* settings / front panel: no reply */
    }
}

static void hle_write8(void *ctx, uint32_t addr, uint8_t val)
{
    hle_ctx_t *c = ctx;
    int ch = (int)((addr - SLAVE_BASE) >> 1) & 3;
    if (c->log)
        fprintf(stderr, "[SLAVE] W ch%d %06X = %02X\n", ch, (unsigned)addr, val);
    if (ch == 3)
        command(c, val);
}

static uint8_t hle_read8(void *ctx, uint32_t addr)
{
    hle_ctx_t *c = ctx;
    if (!(addr & 1)) return 0xFF;                 /* data on odd bytes only */
    int ch = (int)((addr - SLAVE_BASE) >> 1) & 3;
    int v = fifo_pop(&c->out[ch]);
    if (c->log && v >= 0)
        fprintf(stderr, "[SLAVE] R ch%d %06X -> %02X\n", ch, (unsigned)addr, v);
    return (v < 0) ? 0xFF : (uint8_t)v;
}

static int hle_tick(void *ctx, int cyc) { (void)ctx; (void)cyc; return 0; }

/* IN2N: asserted while any channel has bytes for the CPU */
static int hle_irq(void *ctx)
{
    hle_ctx_t *c = ctx;
    for (int i = 0; i < NCHAN; i++)
        if (c->out[i].count) return 1;
    return 0;
}

/*
 * Input report from the host:
 *   report[0] = 1 (pointer), [1..2] = X (big-endian, 0..1023),
 *   [3..4] = Y (0..1023), [5] = buttons (bit 0, bit 1, bit 2)
 */
static void hle_push_input(void *ctx, const uint8_t *rep, int len)
{
    hle_ctx_t *c = ctx;
    if (len < 6 || rep[0] != 1) return;
    int x = ((rep[1] << 8) | rep[2]) & 0x3FF;
    int y = ((rep[3] << 8) | rep[4]) & 0x3FF;
    int b = rep[5] & 7;
    if (x == c->px && y == c->py && b == c->pbuttons) return;
    c->px = x; c->py = y; c->pbuttons = b;

    if (c->out[0].count > FIFO_SIZE - 8) return;  /* driver not reading: drop */
    uint8_t b0 = 0x80 | ((b & 2) ? 0x20 : 0) | ((b & 1) ? 0x10 : 0) | ((b & 4) ? 0x08 : 0)
               | (uint8_t)((x >> 7) & 7);
    fifo_push(&c->out[0], b0);
    fifo_push(&c->out[0], (uint8_t)(x & 0x7F));
    fifo_push(&c->out[0], (uint8_t)((y >> 7) & 7));
    fifo_push(&c->out[0], (uint8_t)(y & 0x7F));
}

static void hle_destroy(void *ctx) { free(ctx); }

slave_backend_t *slave_hle_create(void)
{
    slave_backend_t *be = calloc(1, sizeof(*be));
    if (!be) return NULL;
    be->ctx = calloc(1, sizeof(hle_ctx_t));
    if (!be->ctx) { free(be); return NULL; }

    be->name       = "HLE";
    be->reset      = hle_reset;
    be->read8      = hle_read8;
    be->write8     = hle_write8;
    be->tick       = hle_tick;
    be->irq        = hle_irq;
    be->push_input = hle_push_input;
    be->destroy    = hle_destroy;
    hle_reset(be->ctx);
    return be;
}

void slave_hle_set_uart(slave_backend_t *be, uart_inject_fn fn, void *bus)
{
    hle_ctx_t *c = be->ctx;
    c->inject = fn;
    c->bus    = bus;
}
