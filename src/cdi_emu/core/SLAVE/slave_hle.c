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
 *   byte 0: 1 0 B1 B2 B3 X9 X8 X7   (bit 5 = button 1 / action, bit 4 = button 2)
 *   byte 1: 0 X6..X0
 *   byte 2: 0 0 0 0 0 Y9 Y8 Y7
 *   byte 3: 0 Y6..Y0
 *
 * Set position (pointer driver -> 68HC05, written on channel 0; $1B07F8):
 *   byte 0: 1 1 Y5..Y0   byte 1: 0 X9..X7 Y9..Y6   byte 2: 0 X6..X0
 * The driver stores that position as the current one and only signals the
 * application when a later packet differs. Games that read the pointer as
 * a joystick (Asterix character selection) re-center it every frame
 * ("C7 35 00" = 384,327) and use the displacement. So while the driver
 * keeps setting the position (last one < 0.5 s ago) the HLE moves it by
 * the host mouse *movement*; otherwise the pointer follows the host mouse
 * position (absolute), so the CD-i cursor stays under the PC mouse.
 * Relative "impulses" (arrow keys) are always added.
 *
 * Channel 3 also carries the CD drive messages of cdapdriv (4 bytes,
 * IRQ handler $1AC04C reading $200007):
 *   B0 xx yy zz  disc status. cdapdriv sends "FA B0 00 00 00" at init.
 *                zz & $0F = drive state (1 or > 5: disc removed/changed),
 *                yy & 7 = disc type, yy bit 6 = ask for the B1 address.
 *                With a disc: B0 00 02 15 (CD-i disc ready).
 *                Empty drive: B0 00 00 00. zz = 01 shows the tray open
 *                (the shell offers CLOSE instead of OPEN).
 *   B1 aa aa aa  disc base address (multi-session), 0 here.
 * The 68HC05 sends B0 again by itself when the disc changes.
 *
 * $8A written on $200005: reset request. The kernel sends it after
 * "Fatal System Error; rebooting system", which is also how the shell
 * starts a disc (csdinit calls F$SysDbg with the magic reboot values):
 * the 68HC05 resets the 68070, which then boots the disc application.
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

    /* pointer state: position reported to the CPU, last host mouse position */
    int     px, py, pbuttons;
    int     mx, my, have_mouse;
    uint8_t sp[3];              /* set-position command being received */
    int     sp_n;
    int     rel_frames;         /* >0: the driver set the position recently */

    /* CD drive: multi-byte command on channel 3 */
    int     disc;               /* 1 = disc in the drive */
    uint8_t cd_cmd;
    int     cd_args;            /* argument bytes still expected */

    int     reset_req;          /* $8A on channel 2: reset the 68070 */

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
    int disc = c->disc;
    memset(c, 0, sizeof(*c));
    c->inject = inj;
    c->bus    = bus;
    c->disc   = disc;                /* the disc stays in the drive */
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

static void disc_status(hle_ctx_t *c)
{
    static const uint8_t present[4] = { 0xB0, 0x00, 0x02, 0x15 };
    static const uint8_t absent[4]  = { 0xB0, 0x00, 0x00, 0x00 };  /* 01 = tray open ("CLOSE") */
    const uint8_t *r = c->disc ? present : absent;
    for (int i = 0; i < 4; i++) fifo_push(&c->out[3], r[i]);
    if (c->log) fprintf(stderr, "[SLAVE] disc status %02X %02X %02X %02X\n", r[0], r[1], r[2], r[3]);
}

static void command(hle_ctx_t *c, uint8_t cmd)
{
    if (c->cd_args) {                      /* argument byte of B0/B1 */
        if (--c->cd_args == 0) {
            if (c->cd_cmd == 0xB0) disc_status(c);
            else { fifo_push(&c->out[3], 0xB1); for (int i = 0; i < 3; i++) fifo_push(&c->out[3], 0); }
        }
        return;
    }
    switch (cmd) {
    case 0xB0: case 0xB1: c->cd_cmd = cmd; c->cd_args = 3; break;
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
    if (ch == 0) {                  /* pointer driver: set position */
        if ((val & 0xC0) == 0xC0) { c->sp[0] = val; c->sp_n = 1; }
        else if (val & 0x80) c->sp_n = 0;           /* other command ($83, $84...) */
        else if (c->sp_n > 0) {
            c->sp[c->sp_n++] = val;
            if (c->sp_n == 3) {
                c->py = ((c->sp[1] & 0x0F) << 6) | (c->sp[0] & 0x3F);
                c->px = ((c->sp[1] & 0x70) << 3) | (c->sp[2] & 0x7F);
                c->sp_n = 0;
                c->rel_frames = 25;              /* relative mode for 0.5 s */
                if (c->log) fprintf(stderr, "[SLAVE] pointer set to %d,%d\n", c->px, c->py);
            }
        }
    } else if (ch == 3)
        command(c, val);
    else if (ch == 2 && val == 0x8A)
        c->reset_req = 1;            /* kernel "Fatal System Error; rebooting" */
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
 *   report[0] = 1: host mouse, [1..2] = X (big-endian, 0..1023),
 *                  [3..4] = Y (0..1023), [5] = buttons (bit 0, bit 1, bit 2)
 *   report[0] = 2: impulse, [1..2] = dX, [3..4] = dY (signed), [5] = buttons
 */
static void hle_push_input(void *ctx, const uint8_t *rep, int len)
{
    hle_ctx_t *c = ctx;
    if (len < 6 || (rep[0] != 1 && rep[0] != 2)) return;
    int b = rep[5] & 7;
    int x, y;
    if (rep[0] == 1) {                       /* host mouse, absolute: use its movement */
        int hx = ((rep[1] << 8) | rep[2]) & 0x3FF;
        int hy = ((rep[3] << 8) | rep[4]) & 0x3FF;
        if (!c->have_mouse || c->rel_frames <= 0) { x = hx; y = hy; c->have_mouse = 1; }
        else { x = c->px + (hx - c->mx); y = c->py + (hy - c->my); }
        c->mx = hx; c->my = hy;
    } else {                                 /* impulse: signed dx, dy */
        x = c->px + (int16_t)((rep[1] << 8) | rep[2]);
        y = c->py + (int16_t)((rep[3] << 8) | rep[4]);
    }
    if (x < 0) x = 0;
    if (x > 1023) x = 1023;
    if (y < 0) y = 0;
    if (y > 1023) y = 1023;
    if (x == c->px && y == c->py && b == c->pbuttons) return;
    c->px = x; c->py = y; c->pbuttons = b;

    if (c->out[0].count > FIFO_SIZE - 8) return;  /* driver not reading: drop */
    /* host button 1 (left click) = CD-i button 1 = bit 5 ($20): the shell
     * accepts either button, but games (Asterix menu) only validate with
     * bit 5; host button 2 (right click) = bit 4 ($10) */
    uint8_t b0 = 0x80 | ((b & 1) ? 0x20 : 0) | ((b & 2) ? 0x10 : 0) | ((b & 4) ? 0x08 : 0)
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

void slave_hle_set_disc(slave_backend_t *be, int present, int notify)
{
    hle_ctx_t *c = be->ctx;
    int changed = (c->disc != (present != 0));
    c->disc = (present != 0);
    /* unsolicited status, like the 68HC05 (not at power-on: nobody reads
     * channel 3 before cdapdriv is initialized, IN2N would stay asserted) */
    if (changed && notify) disc_status(c);
}

int slave_hle_take_reset(slave_backend_t *be)
{
    hle_ctx_t *c = be->ctx;
    int r = c->reset_req;
    c->reset_req = 0;
    return r;
}

void slave_hle_frame(slave_backend_t *be)
{
    hle_ctx_t *c = be->ctx;
    if (c->rel_frames > 0) c->rel_frames--;
}

void slave_hle_set_uart(slave_backend_t *be, uart_inject_fn fn, void *bus)
{
    hle_ctx_t *c = be->ctx;
    c->inject = fn;
    c->bus    = bus;
}
