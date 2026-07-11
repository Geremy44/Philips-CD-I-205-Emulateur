CC      = gcc
SDL_CFLAGS = $(filter-out -Dmain=SDL_main,$(shell pkg-config --cflags sdl2 SDL2_ttf))
CFLAGS  = -g -O2 -Wall -Wextra -std=c11 -DSDL_MAIN_HANDLED $(SDL_CFLAGS)
LDFLAGS = $(shell pkg-config --libs sdl2 SDL2_ttf) -mconsole
SRCS    = main.c core/bus.c core/rom_loader.c core/CPU/m68k.c core/CPU/uart_console.c core/CPU/m68k_ea.c core/CPU/m68k_ops.c core/CPU/uart.c core/GPU/scc66470.c core/SLAVE/slave.c core/SLAVE/slave_hle.c core/SLAVE/slave_lle.c
OBJS    = $(SRCS:.c=.o)
TARGET  = cdi205

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe core/*.bak *.bak