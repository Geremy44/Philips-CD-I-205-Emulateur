/*
 * os9trace.h - OS-9 system call tracer (EMU_OS9_LOG=1: errors, 2: all)
 */

#ifndef OS9TRACE_H
#define OS9TRACE_H

#include <stdint.h>
#include "bus.h"

/* Call before executing the instruction at pc */
void os9trace_step(bus_t *b, uint32_t pc, uint32_t sp, const uint32_t d[8], const uint32_t a[8], uint16_t sr);

#endif /* OS9TRACE_H */
