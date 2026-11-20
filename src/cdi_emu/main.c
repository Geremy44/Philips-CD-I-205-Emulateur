/*
 * main.c - Entry point for the CD-i 205 emulator
 *
 * Initializes the memory bus, loads the firmware ROM, sets up the
 * Motorola 68070 CPU, slave co-processor, video controller, and UART
 * console, then runs the main emulation loop with SDL2 event polling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
#endif

#include "core/CPU/m68k.h"
#include "core/CPU/uart_console.h"
#include "core/bus.h"
#include "core/rom_loader.h"
#include "core/GPU/scc66470.h"
#include "core/GPU/video_display.h"
#include "core/SLAVE/slave.h"
#include "core/CPU/onchip68070.h"

/* Global instances for video controller and slave co-processor */
static slave_t    g_slave;

/*
 * Debug routine: verify the ROM checksum against reference values
 * stored at the end of the ROM image.
 */
void debug_rom_checksum(bus_t *bus)
{
    uint32_t sum = 0;

    /* Sum all bytes from $180000 to $1FFBFD (excluding the 2-byte checksum) */
    for (uint32_t a = 0x180000; a <= 0x1FFBFD; a++)
        sum += bus_read8(bus, a);

    /* Account for the last 1024 bytes assumed to be 0xFF (virgin EPROM) */
    sum += 0xFC00;

    uint16_t cksum16 = sum & 0xFFFF;
    uint8_t  lsb = cksum16 & 0xFF;
    uint8_t  msb = (cksum16 >> 8) & 0xFF;
    uint8_t  displayed = (lsb + msb) & 0xFF;

    /* Reference values stored in ROM at the checksum location */
    uint8_t  ref_lsb = bus_read8(bus, 0x1FFBFF);
    uint8_t  ref_msb = bus_read8(bus, 0x1FFBFE);
    uint16_t ref_cksum16 = (ref_msb << 8) | ref_lsb;
    uint8_t  ref_displayed = (ref_lsb + ref_msb) & 0xFF;

    fprintf(stderr, "=== ROM CHECKSUM DEBUG ===\n");
    fprintf(stderr, "Somme brute        : 0x%08X\n", sum);
    fprintf(stderr, "Calcule  cksum16   : 0x%04X\n", cksum16);
    fprintf(stderr, "Ref      cksum16   : 0x%04X  %s\n",
            ref_cksum16, (cksum16 == ref_cksum16) ? "<<< MATCH" : "!!! DIFF");
    fprintf(stderr, "Calcule  affiche   : 0x%02X\n", displayed);
    fprintf(stderr, "Ref      affiche   : 0x%02X  %s\n",
            ref_displayed, (displayed == ref_displayed) ? "<<< MATCH" : "!!! DIFF");
}

int main(int argc, char* argv[]) {
    /* Set stdout to line-buffered, stderr to full-buffered for performance */
    setvbuf(stdout, NULL, _IOLBF, 4096);
    setvbuf(stderr, NULL, _IOFBF, 65536);

    printf("=== CD-I 205 Emulator ===\n");
    SDL_SetMainReady();

#ifdef _WIN32
    setvbuf(stdin, NULL, _IONBF, 0);
#endif

    /* --- Initialize the bus and load the firmware ROM --- */
    bus_t *bus = bus_create();
    if (bus_load_ihex(bus, "./ROM/CDI205-00_PS-7211_REL.2.1.hex") != 0) {
        fprintf(stderr,"Failed to load ROM\n");
        bus_destroy(bus); return 1;
    }
    printf("ROM loaded successfully\n");

    /* Pointeur vers le contrôleur vidéo possédé par le bus */
   scc66470_t *video = bus_get_video(bus);
   onchip68070_t *onchip = bus_get_onchip(bus);

   debug_rom_checksum(bus);
   
   /* Initialize video controller with proper defaults */
   scc66470_init(video);

   /* --- Initialize the M68000 CPU --- */
    m68k_t cpu;
    m68k_init(&cpu);
    m68k_set_bus(&cpu, bus);

    /* --- Set up the slave co-processor (HLE mode by default) --- */
    slave_backend_t *be = NULL;

    /* If a 68HC05 ROM dump is available, LLE could be used here:
     * be = slave_lle_create(hc05_rom, hc05_len);
     */

    /* Fall back to HLE (high-level emulation) when no HC05 ROM is available */
    if (!be) {
        be = slave_hle_create();
        fprintf(stderr, "[SLAVE] backend = %s\n", be->name);
    }

    /* Connect the slave's UART injection callback */
    slave_hle_set_uart(be, (uart_inject_fn)uart_inject_rx, bus);

    slave_attach(&g_slave, be);
    slave_reset(&g_slave);

    /* Reset the CPU (loads PC and SP from the ROM reset vector) */
    m68k_reset(&cpu);

    /* Open the SDL2 UART terminal window */
    uart_console_init();

    /* Open the SDL2 video display window */
    video_display_init();

    printf("PC=0x%08X SR=0x%04X A7=0x%08X\n",
           cpu.pc, cpu.sr, cpu.a[7]);

    /* Inject an initial character to start the boot console */
    uart_feed('1');

    /* --- Trace control ---
     * trace_enable: 1 = print each instruction's state (EMU_TRACE=1)
     * trace_max:    max number of instructions to trace (0 = unlimited) */
    int  trace_enable = (getenv("EMU_TRACE") != NULL) ? 1 : 0;
    long trace_max = 1410065408;
    {
        const char *envm = getenv("EMU_TRACE_MAX");
        if (envm) trace_max = atol(envm);
    }

    printf("\nStarting emulation loop...\n");

    long traced = 0;
    int running = 1;
    long i = 0;
    int update_counter = 0;
    long pc_poll = getenv("EMU_PC_POLL") ? atol(getenv("EMU_PC_POLL")) : 0;

    /* --- Main emulation loop --- */
    while (running && i < trace_max) {

        /* Print instruction trace (skipping known noise loops) */
        if (trace_enable && traced < trace_max) {
            if (traced &&
                 !(cpu.pc >= 0x181cd4 && cpu.pc <= 0x181ce0) &&
                 !(cpu.pc >= 0x181CE2 && cpu.pc <= 0x181CEE) &&
                 !(cpu.pc >= 0x181D02 && cpu.pc <= 0x181D0A) &&
                 !(cpu.pc >= 0x181E8E && cpu.pc <= 0x181EAC)
                ) {
                uint32_t pc_before = cpu.pc;

                printf("[%6ld] PC=%08X op=%04X \nD0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X\nA0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X\n",
                traced, pc_before, bus_read16(bus, pc_before),
                cpu.d[0], cpu.d[1], cpu.d[2], cpu.d[3], cpu.d[4], cpu.d[5], cpu.d[6], cpu.d[7],
                cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3], cpu.a[4], cpu.a[5], cpu.a[6], cpu.a[7]);
            }
            traced++;
        }

        /* Execute one CPU instruction */
        int cycles = m68k_step(&cpu);

        if (pc_poll && (i % pc_poll) == 0)
            fprintf(stderr, "[PCPOLL] i=%ld PC=%08X A7=%08X\n", i, cpu.pc, cpu.a[7]);

        /* Advance video timing by the consumed cycles */
        scc66470_tick(video, cycles);

        /* On-chip timer, then present the highest pending interrupt */
        onchip68070_tick(onchip, cycles);
        {
            int vector = 0;
            int level = onchip68070_irq(onchip, &vector);
            m68k_set_irq(&cpu, level, vector);
        }

        /* Check for CPU halt (unimplemented opcode, etc.) */
        if (cpu.halted) {
            fprintf(stderr, "[HALT] CPU stopped/halted at step %ld\n", traced);
            break;
        }

        /* Poll SDL events and update displays every 20000 instructions (~60 Hz) */
        if (++update_counter >= 20000) {
            update_counter = 0;
            running = uart_console_update();
            if (running)
                running = video_display_update(video, bus);
        }

        i++;
    }

    fflush(stdout);

    fprintf(stderr, "[MAIN] CPU loop finished (traced=%ld), keeping window open.\n", traced);
    fprintf(stderr, "[MAIN] Close with the window's X button.\n");

    /* Keep the window alive after the CPU loop exits */
    int win_alive = 1;
    while (win_alive) {
        win_alive = uart_console_update();
        SDL_Delay(16);  /* ~60 fps idle */
    }

    bus_destroy(bus);
    printf("\nEmulation stopped\n");
    return 0;
}
