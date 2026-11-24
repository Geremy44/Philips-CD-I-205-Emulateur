/*
 * fmv_internal.h - shared between the parts of the Digital Video Cartridge
 * (fmv.c the board, mcd251.c the MPEG video chip, fma.c the audio chip)
 */

#ifndef FMV_INTERNAL_H
#define FMV_INTERNAL_H

#include <stdint.h>

#define FMV_CPU_HZ 15000000ull           /* emulated time base: 68070 clock periods */
#define FMV_NEVER  UINT64_MAX

extern uint64_t fmv_now;                 /* current time, in CPU clock periods */
extern int      fmv_log;                 /* EMU_FMV_LOG level */

static inline uint64_t fmv_ticks(uint64_t ticks, uint64_t hz) { return ticks * FMV_CPU_HZ / hz; }

/* board callbacks */
void fmv_board_drq(int for_audio);       /* a chip asks for the next stream transfer */

/* video chip (MCD251) */
void     fmvv_reset(void);
uint16_t fmvv_read(int reg);             /* word register index (byte offset / 2) */
void     fmvv_write(int reg, uint16_t v);
void     fmvv_data(uint8_t b);           /* system stream byte */
void     fmvv_end_of_transfer(void);
void     fmvv_tick(void);                /* timers, called often */
void     fmvv_vblank(int state);
int      fmvv_irq_pending(void);
uint8_t  fmvv_vector(void);
int      fmvv_pixel(int x, int y, uint32_t *rgb);
int      fmvv_showing(void);

/* audio chip (FMA) */
void     fmva_reset(void);
uint16_t fmva_read(int reg);
void     fmva_write(int reg, uint16_t v);
void     fmva_data(uint8_t b);
void     fmva_end_of_transfer(int dma);
void     fmva_tick(void);
void     fmva_poll(void);                /* video chip timer tick */
int      fmva_irq_pending(void);
uint8_t  fmva_vector(void);
int      fmva_wants_data(void);
void     fmva_info(int *kbps, int *rate, int *playing);
extern unsigned long fmvv_shown;
void     fmvv_info(int *w, int *h, int *playing);

#endif
