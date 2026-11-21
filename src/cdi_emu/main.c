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
#include "core/CDIC/cdic.h"
#include "core/os9trace.h"
#include "core/audio_out.h"
#include "core/rtc_m48t08.h"

/* ---- Save / recall hotkeys (NVRAM + clock) ----
 *   F5 = save to the current slot, F9 = recall it, F6 = next slot (1..9) */
static bus_t *g_hotkey_bus = NULL;
static int    g_save_slot  = 1;

/* F7 / EMU_SWAP_BUTTONS: swap the left and right mouse buttons (default on) */
static int g_swap_buttons = 0;

static int hotkey(int fkey)
{
    if (!g_hotkey_bus) return 0;
    switch (fkey) {
    case 7:
        g_swap_buttons = !g_swap_buttons;
        fprintf(stderr, "[INPUT] mouse buttons %s (F7)\n", g_swap_buttons ? "SWAPPED" : "normal");
        return 1;
    case 5: bus_save_slot(g_hotkey_bus, g_save_slot); return 1;
    case 9: bus_load_slot(g_hotkey_bus, g_save_slot); return 1;
    case 6:
        g_save_slot = g_save_slot % 9 + 1;
        fprintf(stderr, "[SAVE] current slot: %d (F5 save, F9 recall)\n", g_save_slot);
        return 1;
    }
    return 0;
}

/* Input recording / replay (deterministic tests):
 *   EMU_INPUT_REC=file   writes "step x y buttons" for every pointer event
 *   EMU_INPUT_PLAY=file  replays them at the same instruction counts, and
 *                        ignores the real mouse meanwhile */
static long  g_step = 0;
static FILE *g_rec = NULL, *g_play = NULL;
static long  g_play_step = -1;
static int   g_play_x, g_play_y, g_play_b;

static void push_pointer(void *ctx, int x, int y, int buttons)
{
    slave_backend_t *be = ctx;
    /* VIDEO_PTR_IMPULSE ($100): x, y are a relative impulse (report type 2) */
    uint8_t rep[6] = { (buttons & 0x100) ? 2 : 1, (uint8_t)(x >> 8), (uint8_t)x,
                       (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(buttons & 7) };
    if (be && be->push_input) be->push_input(be->ctx, rep, (int)sizeof(rep));
}

/* Pointer events from the video window -> 68HC05 HLE (pointer packets) */
static void pointer_event(void *ctx, int x, int y, int buttons)
{
    if (g_play) return;                          /* replay in progress */
    if (g_swap_buttons)                          /* left <-> right */
        buttons = (buttons & ~3) | ((buttons & 1) << 1) | ((buttons >> 1) & 1);
    if (g_rec) { fprintf(g_rec, "%ld %d %d %d\n", g_step, x, y, buttons); fflush(g_rec); }
    push_pointer(ctx, x, y, buttons);
}

static void play_next(void)
{
    if (fscanf(g_play, "%ld %d %d %d", &g_play_step, &g_play_x, &g_play_y, &g_play_b) != 4) {
        fprintf(stderr, "[INPUT] replay finished at step %ld\n", g_step);
        fclose(g_play);
        g_play = NULL;
        g_play_step = -1;
    }
}

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
   scc66470_t *video_b = bus_get_video_slave(bus);
   onchip68070_t *onchip = bus_get_onchip(bus);
   cdic_t *cdic = bus_get_cdic(bus);

   debug_rom_checksum(bus);
   
   /* Initialize video controller with proper defaults */
   scc66470_init(video);

   /* --- Initialize the M68000 CPU --- */
    m68k_t cpu;
    m68k_init(&cpu);
    m68k_set_bus(&cpu, bus);

    /* --- Slave co-processor (68HC05): the bus owns the backend (HLE).
     * With a 68HC05 ROM dump, slave_lle_create() could replace it. --- */
    slave_backend_t *be = bus->slave;
    fprintf(stderr, "[SLAVE] backend = %s\n", be->name);
    slave_hle_set_uart(be, (uart_inject_fn)uart_inject_rx, bus);
    video_display_set_pointer_cb(pointer_event, be);
    g_hotkey_bus = bus;
    /* Default: swapped (left click = CD-i button 2, as the user expects on
     * Asterix). EMU_SWAP_BUTTONS=0 restores the direct mapping; F7 toggles. */
    g_swap_buttons = !(getenv("EMU_SWAP_BUTTONS") && *getenv("EMU_SWAP_BUTTONS") == '0');

    /* Disc image: first argument, or EMU_DISC (.cue/.bin/.iso) */
    {
        const char *disc = (argc > 1) ? argv[1] : getenv("EMU_DISC");
        if (disc && *disc) {
            if (cdic_insert(cdic, disc) != 0)
                fprintf(stderr, "[DISC] cannot load '%s', the drive stays empty\n", disc);
        } else {
            fprintf(stderr, "[DISC] no disc (give an image as argument or EMU_DISC)\n");
        }
        slave_hle_set_disc(be, cdic_has_disc(cdic), 0);
    }
    uart_console_set_hotkey_cb(hotkey);

    /* Reset the CPU (loads PC and SP from the ROM reset vector) */
    m68k_reset(&cpu);

    /* Open the SDL2 UART terminal window */
    uart_console_init();

    /* Open the SDL2 video display window */
    video_display_init();

    /* Host sound: CD-i ADPCM decoded by the CDIC */
    audio_out_init();                    /* no device: audio_out_push still dumps */
    cdic_set_audio(cdic, audio_out_push, NULL);

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
    /* Recording / replay also freeze the clock source: the RTC starts at the
     * time written in the file header and follows the emulated cycles, so
     * a replay sees exactly the same dates as the recorded session. */
    if (getenv("EMU_INPUT_REC") && *getenv("EMU_INPUT_REC")) {
        g_rec = fopen(getenv("EMU_INPUT_REC"), "w");
        if (g_rec) {
            rtc_virtual_base = (int64_t)time(NULL);
            fprintf(g_rec, "# rtc %lld\n", (long long)rtc_virtual_base);
        }
        fprintf(stderr, "[INPUT] recording pointer events to '%s'\n", getenv("EMU_INPUT_REC"));
    }
    if (getenv("EMU_INPUT_PLAY") && *getenv("EMU_INPUT_PLAY")) {
        g_play = fopen(getenv("EMU_INPUT_PLAY"), "r");
        if (g_play) {
            long long base = 0;
            int ch = fgetc(g_play);
            if (ch == '#' && fscanf(g_play, " rtc %lld", &base) == 1) rtc_virtual_base = base;
            else if (ch != EOF) ungetc(ch, g_play);
            if (!rtc_virtual_base) rtc_virtual_base = 946684800;   /* old file: fixed 2000-01-01 */
            fprintf(stderr, "[INPUT] replaying '%s' (clock base %lld)\n",
                    getenv("EMU_INPUT_PLAY"), (long long)rtc_virtual_base);
            play_next();
        }
    }
    int update_counter = 0;
    long pc_poll = getenv("EMU_PC_POLL") ? atol(getenv("EMU_PC_POLL")) : 0;
    /* EMU_BREAK=hex: print the last 64 PCs (and registers) when PC reaches it */
    /* several addresses: EMU_BREAK=1A96D0,1A9706 (each prints up to 4 hits) */
    uint32_t brks[8] = {0};
    int nbrk = 0, brk_hits[8] = {0};
    if (getenv("EMU_BREAK")) {
        const char *p = getenv("EMU_BREAK");
        while (*p && nbrk < 8) {
            char *end;
            brks[nbrk++] = (uint32_t)strtoul(p, &end, 16);
            if (end == p) break;
            p = (*end == ',') ? end + 1 : end;
        }
    }
    int brk = nbrk > 0;
    uint32_t pc_ring[64] = {0};
    uint32_t user_ring[32] = {0};            /* last distinct user-mode PCs */
    int user_pos = 0;
    /* EMU_AUTOCLICK=x,y,step: press button 1 at (x,y) at that instruction count
     * (pointer units: x = frame pixel 0..767, y = half line 0..479) */
    long ac_x = -1, ac_y = 0, ac_step = 0;
    long watch_after = getenv("EMU_WATCH_AFTER") ? atol(getenv("EMU_WATCH_AFTER")) : 0;
    if (getenv("EMU_AUTOCLICK"))
        sscanf(getenv("EMU_AUTOCLICK"), "%ld,%ld,%ld", &ac_x, &ac_y, &ac_step);
    /* Real-time pacing: emulated time (CPU cycles / 15 MHz) follows the host
     * clock. EMU_SPEED = percent of real speed (default 100, 0 = unlimited). */
    int speed = getenv("EMU_SPEED") ? atoi(getenv("EMU_SPEED")) : 100;
    uint64_t pace_cycles0 = 0, pace_ms0 = SDL_GetTicks64();
    int os9_log = getenv("EMU_OS9_LOG") && *getenv("EMU_OS9_LOG") && *getenv("EMU_OS9_LOG") != '0';
    int ring_pos = 0;

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

        if (brk) {
            int hit = -1;
            for (int k = 0; k < nbrk; k++)
                if (cpu.pc == brks[k] && brk_hits[k] < 16) { hit = k; brk_hits[k]++; }
            if (hit >= 0) {
                fprintf(stderr, "[BREAK] PC=%06X reached at step %ld, last PCs:\n", brks[hit], i);
                for (int k = 0; k < 64; k++)
                    fprintf(stderr, "  %06X\n", pc_ring[(ring_pos + k) & 63]);
                fprintf(stderr, "  D0-D7 %08X %08X %08X %08X %08X %08X %08X %08X\n",
                        cpu.d[0], cpu.d[1], cpu.d[2], cpu.d[3], cpu.d[4], cpu.d[5], cpu.d[6], cpu.d[7]);
                fprintf(stderr, "  A0-A7 %08X %08X %08X %08X %08X %08X %08X %08X SR=%04X\n",
                        cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3], cpu.a[4], cpu.a[5], cpu.a[6], cpu.a[7], cpu.sr);
                for (int k = 0; k < 16; k++)
                    fprintf(stderr, "  [A7+%02X] %08X\n", k * 4, bus_read32(bus, cpu.a[7] + k * 4));
                fprintf(stderr, "  last user-mode PCs:");
                for (int k = 0; k < 32; k++) fprintf(stderr, " %06X", user_ring[(user_pos + k) & 31]);
                fprintf(stderr, "\n");
                /* code around the PC (for programs loaded in RAM) */
                fprintf(stderr, "  CODE %06X:", (cpu.pc - 32) & ~1u);
                for (int k = 0; k < 40; k++)
                    fprintf(stderr, " %04X", bus_read16(bus, ((cpu.pc - 32) & ~1u) + 2 * k));
                fprintf(stderr, "\n");
                fflush(stderr);
            }
            pc_ring[ring_pos] = cpu.pc;
            ring_pos = (ring_pos + 1) & 63;
            if (!(cpu.sr & 0x2000) && user_ring[(user_pos + 31) & 31] != cpu.pc) {
                user_ring[user_pos] = cpu.pc;
                user_pos = (user_pos + 1) & 31;
            }
        }

        g_step = i;
        bus_watch_pc = cpu.pc;
        bus_watch_on = (i >= watch_after);
        while (g_play && i == g_play_step) {
            push_pointer(be, g_play_x, g_play_y, g_play_b);
            play_next();
        }
        if (ac_x >= 0) {
            /* glide towards the target (16 moves), like a real mouse, then press */
            if (i >= ac_step - 4000000 && i < ac_step && (ac_step - i) % 250000 == 0) {
                long k = 16 - (ac_step - i) / 250000;          /* 0..15 */
                push_pointer(be, (int)(200 + (ac_x - 200) * k / 15), (int)(300 + (ac_y - 300) * k / 15), 0);
            }
            if (i == ac_step) { push_pointer(be, (int)ac_x, (int)ac_y, 1); fprintf(stderr, "[CLICK] %ld,%ld\n", ac_x, ac_y); }
            if (i == ac_step + 3000000) {      /* release, and click again later (8 times) */
                push_pointer(be, (int)ac_x, (int)ac_y, 0);
                static int ac_n = 0;
                if (++ac_n < 8) ac_step += 100000000; else ac_x = -1;
            }
        }
        if (os9_log)
            os9trace_step(bus, cpu.pc, cpu.a[7], cpu.d, cpu.a, cpu.sr);

        /* Execute one CPU instruction */
        int cycles = m68k_step(&cpu);

        if (pc_poll && (i % pc_poll) == 0)
            fprintf(stderr, "[PCPOLL] i=%ld PC=%08X A7=%08X\n", i, cpu.pc, cpu.a[7]);

        /* Advance video timing by the consumed cycles */
        if (scc66470_tick(video, cycles))   scc66470_vretrace(video,   bus->dram, (uint32_t)sizeof(bus->dram));
        if (scc66470_tick(video_b, cycles)) scc66470_vretrace(video_b, bus->dram, (uint32_t)sizeof(bus->dram));

        /* On-chip timer, then present the highest pending interrupt:
         * on-chip sources (autovectors 57..63) and the 68HC05 on IN2N
         * (level 2, autovector 26 - descriptors ptr/pck/slave). */
        rtc_virtual_cycles += (uint64_t)cycles;
        onchip68070_tick(onchip, cycles);
        cdic_tick(cdic, cycles);
        {
            int vector = 0, v2 = 0;
            int level = onchip68070_irq(onchip, &vector);
            int l2 = cdic_irq(cdic, &v2);          /* CDIC: level 4, vector from IVEC */
            if (l2 > level) { level = l2; vector = v2; }
            /* SCC66470 IT1 (master or slave): level 5, autovector 61 ("vid") */
            if (level < 5 && (scc66470_irq(video) || scc66470_irq(video_b))) { level = 5; vector = 61; }
            if (level < 2 && be->irq && be->irq(be->ctx)) {
                level = 2;
                vector = 26;
            }
            m68k_set_irq(&cpu, level, vector);
        }

        /* 68HC05 reset request ($8A): warm reboot, the DRAM and the disc stay */
        if (slave_hle_take_reset(be)) {
            fprintf(stderr, "[RESET] 68HC05 resets the 68070 (warm reboot) at step %ld\n", i);
            onchip68070_reset(onchip);
            scc66470_init(video);
            scc66470_init(video_b);
            cdic_init(cdic);
            if (be->reset) be->reset(be->ctx);
            m68k_reset(&cpu);
            uart_feed('1');
            continue;
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
            bus_nvram_poll(bus);              /* autosave game saves / clock */
            if (speed > 0) {
                uint64_t emu_ms = (rtc_virtual_cycles - pace_cycles0) * 100u / (15000u * (uint64_t)speed);
                uint64_t host_ms = SDL_GetTicks64() - pace_ms0;
                if (emu_ms > host_ms + 1)
                    SDL_Delay((Uint32)(emu_ms - host_ms));
                else if (host_ms > emu_ms + 250) {      /* too slow: do not try to catch up */
                    pace_cycles0 = rtc_virtual_cycles;
                    pace_ms0 = SDL_GetTicks64();
                }
            }
            if (running)
                running = video_display_update(video, bus);
        }

        i++;
    }

    fflush(stdout);

    fprintf(stderr, "[MAIN] CPU loop finished (traced=%ld), keeping window open.\n", traced);
    fprintf(stderr, "[MAIN] Close with the window's X button.\n");

    /* Keep the window alive after the CPU loop exits */
    int win_alive = getenv("EMU_TRACE_MAX") ? 0 : 1;
    while (win_alive) {
        win_alive = uart_console_update();
        SDL_Delay(16);  /* ~60 fps idle */
    }

    audio_out_close();
    bus_destroy(bus);
    printf("\nEmulation stopped\n");
    return 0;
}
