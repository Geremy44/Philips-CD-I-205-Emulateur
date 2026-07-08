CC      = gcc
CFLAGS  = -g -O2 -Wall -Wextra -std=c11
SRCS    = main.c core/bus.c core/rom_loader.c core/CPU/m68k.c core/CPU/m68k_ea.c core/CPU/m68k_ops.c core/CPU/uart.c core/GPU/scc66470.c core/SLAVE/slave.c core/SLAVE/slave_hle.c core/SLAVE/slave_lle.c
OBJS    = $(SRCS:.c=.o)
TARGET  = cdi205

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe core/*.bak *.bak
