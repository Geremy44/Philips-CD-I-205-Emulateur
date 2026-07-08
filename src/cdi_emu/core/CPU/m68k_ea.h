#ifndef M68K_EA_H
#define M68K_EA_H

#include "m68k.h"

uint32_t ea_calc(m68k_t *cpu, uint8_t mode, uint8_t reg, int size);
void     ea_write(m68k_t *cpu, uint8_t mode, uint8_t reg, int size, uint32_t val);
uint32_t ea_read(m68k_t *cpu, uint8_t mode, uint8_t reg, int size);

#endif
