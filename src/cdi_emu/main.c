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
#include "core/CDIC/cdrom_win.h"
#include "core/FMV/fmv.h"
#ifdef _WIN32
#include <windows.h>
#endif

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
    case 11:
        video_display_toggle_fullscreen();
        return 1;
    case 8:
        video_display_toggle_host_cursor();
        return 1;
    case 7:
        g_swap_buttons = !g_swap_buttons;
        video_display_set_swap(g_swap_buttons);
        video_display_menu_check(VM_SWAP, g_swap_buttons);
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

/* Recording / replay state (see "Input recording / replay" below) */
static long long g_step;
static FILE *g_rec, *g_play;
static char  g_play_disc[1100];          /* replay: disc chosen in the dialog */
static int   g_play_disc_ok = -1;        /* -1 none pending, 0 cancelled, 1 path */

/* Disc changes: the shell's OPEN button, the File menu */
static cdic_t          *g_cdic = NULL;
static int              g_reset_request = 0;  /* File > Reset (Ctrl+R) */
static slave_backend_t *g_be   = NULL;

/* Ask for an image and insert it; the door ends closed (with the disc if
 * one was chosen and loads, empty otherwise when close_empty is set). */
static void insert_disc_dialog(int close_empty)
{
    char path[1100];
    int chosen;
    if (g_play) {                        /* replay: the recorded choice */
        chosen = (g_play_disc_ok == 1);
        if (chosen) snprintf(path, sizeof(path), "%s", g_play_disc);
        if (g_play_disc_ok < 0) fprintf(stderr, "[INPUT] replay: no disc recorded for this dialog\n");
        g_play_disc_ok = -1;
    } else {
        chosen = video_display_choose_disc(path, sizeof(path));
        if (g_rec) {                     /* "# disc step path" ("-" = cancelled) */
            fprintf(g_rec, "# disc %lld %s\n", g_step, chosen ? path : "-");
            fflush(g_rec);
        }
    }
    if (chosen && cdic_insert(g_cdic, path) == 0) {
        fprintf(stderr, "[DISC] inserted '%s'\n", path);
        slave_hle_set_tray(g_be, 0, cdic_disc_kind(g_cdic));  /* door closed, disc ready */
    } else if (close_empty) {
        slave_hle_set_tray(g_be, 0, 0);
    }
}

/* Folder for ripped discs: "CDI-205/Games" found above the executable
 * (development tree), else "Games" next to it (portable build) */
static void games_dir(char *out, int n)
{
    char *base = SDL_GetBasePath();
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s", base ? base : ".\\");
    SDL_free(base);
    for (int up = 0; up < 6; up++) {
        char probe[1200];
        snprintf(probe, sizeof(probe), "%sCDI-205\\Games", dir);
        wchar_t w[1200];
        if (MultiByteToWideChar(CP_UTF8, 0, probe, -1, w, 1200) > 0) {
            DWORD a = GetFileAttributesW(w);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { snprintf(out, n, "%s", probe); return; }
        }
        size_t l = strlen(dir);                     /* parent folder */
        if (l < 2) break;
        dir[l - 1] = 0;
        char *sl = strrchr(dir, '\\');
        if (!sl) break;
        sl[1] = 0;
    }
    base = SDL_GetBasePath();
    snprintf(out, n, "%sGames", base ? base : ".\\");
    SDL_free(base);
}

/* Insert the disc of a PC drive ("cdrom:F") */
static void insert_drive(char letter)
{
    char path[16];
    snprintf(path, sizeof(path), "cdrom:%c", letter);
    cdic_eject(g_cdic);
    slave_hle_set_tray(g_be, 1, 0);
    if (cdic_insert(g_cdic, path) == 0) {
        fprintf(stderr, "[DISC] inserted the disc of drive %c:\n", letter);
        slave_hle_set_tray(g_be, 0, cdic_disc_kind(g_cdic));
    } else {
        slave_hle_set_tray(g_be, 0, 0);
        char msg[96];
        snprintf(msg, sizeof(msg), "Pas de disque lisible dans le lecteur %c:", letter);
        video_display_message("Lecteur CD", msg);
    }
}

/* Ripping a drive: progress in the window title, result in a box */
static void rip_poll(void)
{
    static int last = -2;
    int done = 0;
    char msg[1300];
    int p = cdrom_rip_progress(&done, msg, sizeof(msg));
    if (p < 0) return;
    if (done) {
        if (last != -3) {
            video_display_set_title(NULL);
            char text[1400];
            if (strstr(msg, ".cue") && !strstr(msg, "Impossible") && !strstr(msg, "Pas de disque"))
                snprintf(text, sizeof(text), "Disque extrait :\n%s\n\nIl s'ouvre avec Fichier > Ouvrir un disque.", msg);
            else
                snprintf(text, sizeof(text), "%s", msg);
            fprintf(stderr, "[RIP] %s\n", msg);
            video_display_message("Extraction du disque", text);
            last = -3;
        }
        return;
    }
    if (p != last) {
        char title[96];
        snprintf(title, sizeof(title), "CD-i 205 - Extraction du disque : %d %%", p);
        video_display_set_title(title);
        last = p;
    }
}

/* Menu bar of the video window */
static void menu_action(int id)
{
    if (g_rec && (id == VM_OPEN_DISC || id == VM_EJECT || id == VM_RESET ||
                  (id >= VM_DRIVE_PLAY && id < VM_DRIVE_PLAY + 26))) {   /* replayed at this step */
        /* (a reset requested from the frame block happens at the next step) */
        fprintf(g_rec, "# menu %lld %d\n", g_step + (id == VM_RESET ? 1 : 0), id);
        fflush(g_rec);
    }
    if (id >= VM_DRIVE_PLAY && id < VM_DRIVE_PLAY + 26) { insert_drive((char)('A' + id - VM_DRIVE_PLAY)); return; }
    if (id >= VM_DRIVE_RIP && id < VM_DRIVE_RIP + 26) {
        char dir[1200], letter = (char)('A' + id - VM_DRIVE_RIP);
        games_dir(dir, sizeof(dir));
        if (cdrom_rip_start(letter, dir) == 0) {
            fprintf(stderr, "[RIP] drive %c: -> %s\n", letter, dir);
            video_display_set_title("CD-i 205 - Extraction du disque : 0 %");
        } else {
            video_display_message("Extraction du disque", "Une extraction est d\xC3\xA9j\xC3\xA0 en cours.");
        }
        return;
    }
    switch (id) {
    case VM_OPEN_DISC:                                  /* change disc at any time */
        cdic_eject(g_cdic);
        slave_hle_set_tray(g_be, 1, 0);                 /* door open: CD-RTOS forgets the disc */
        insert_disc_dialog(1);
        break;
    case VM_FMV:                                        /* cartridge in / out + reset */
        if (!fmv_rom_loaded()) {
            video_display_message("Cartouche vid\xC3\xA9o num\xC3\xA9rique",
                "ROM de la cartouche introuvable :\nROM/FMV_combined_7308hi_7307lo.bin");
            break;
        }
        fmv_set_enabled(!fmv_enabled());
        fmv_option_save(fmv_enabled());
        video_display_menu_check(VM_FMV, fmv_enabled());
        g_reset_request = 1;                            /* CD-RTOS looks for it at boot */
        break;
    case VM_RESET:                                      /* reset button: warm reboot */
        g_reset_request = 1;
        break;
    case VM_EJECT:
        cdic_eject(g_cdic);
        slave_hle_set_tray(g_be, 1, 0);                 /* stays open: the shell shows CLOSE */
        fprintf(stderr, "[DISC] ejected\n");
        break;
    case VM_QUIT: { SDL_Event q; SDL_zero(q); q.type = SDL_QUIT; SDL_PushEvent(&q); break; }
    case VM_SAVE:        hotkey(5); break;
    case VM_LOAD:        hotkey(9); break;
    case VM_NEXT_SLOT:   hotkey(6); break;
    case VM_FULLSCREEN:  video_display_toggle_fullscreen(); break;
    case VM_HOST_CURSOR: video_display_toggle_host_cursor(); break;
    case VM_CONSOLE:     uart_console_set_visible(!uart_console_is_visible()); break;
    case VM_SWAP:        hotkey(7); break;
    }
}

/* Input recording / replay (deterministic tests):
 *   EMU_INPUT_REC=file   writes "step x y buttons" for every pointer event
 *   EMU_INPUT_PLAY=file  replays them at the same instruction counts, and
 *                        ignores the real mouse meanwhile */
static long long g_play_step = -1;
static int   g_play_x, g_play_y, g_play_b;
static int   g_play_menu = 0;            /* next event is a menu command */

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
    /* (the left / right swap is applied to the mouse in video_display) */
    if (g_rec) { fprintf(g_rec, "%lld %d %d %d\n", g_step, x, y, buttons); fflush(g_rec); }
    push_pointer(ctx, x, y, buttons);
}

/* Next replay event. "# disc" lines only store the choice for the next
 * file dialog; "# menu" lines are events like the pointer ones. */
static void play_next(void)
{
    char line[1200];
    g_play_menu = 0;
    while (fgets(line, sizeof(line), g_play)) {
        long long st; int id; char *p;
        if (sscanf(line, "%lld %d %d %d", &g_play_step, &g_play_x, &g_play_y, &g_play_b) == 4)
            return;
        if (sscanf(line, "# menu %lld %d", &st, &id) == 2) {
            g_play_step = st; g_play_x = id; g_play_menu = 1;
            return;
        }
        if (!strncmp(line, "# disc ", 7) && (p = strchr(line + 7, ' ')) != NULL) {
            p++;
            p[strcspn(p, "\r\n")] = 0;
            g_play_disc_ok = strcmp(p, "-") ? 1 : 0;
            snprintf(g_play_disc, sizeof(g_play_disc), "%s", p);
        }
    }
    {
        fprintf(stderr, "[INPUT] replay finished at step %lld\n", g_step);
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
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");   /* 1 ms sleeps on Windows */

#ifdef _WIN32
    setvbuf(stdin, NULL, _IONBF, 0);
#endif

    /* --- Initialize the bus and load the firmware ROM --- */
    bus_t *bus = bus_create();
    if (bus_load_ihex(bus, "./ROM/CDI205-00_PS-7211_REL.2.1.hex") != 0) {
        fprintf(stderr,"Failed to load ROM\n");
        bus_destroy(bus); return 1;
    }
    if (getenv("EMU_ROM_DUMP")) {                 /* debug: the ROM as loaded */
        FILE *rf = fopen(getenv("EMU_ROM_DUMP"), "wb");
        if (rf) { fwrite(bus->rom, 1, sizeof(bus->rom), rf); fclose(rf); }
    }
    /* Digital Video Cartridge (MPEG), optional: menu Materiel (options.ini),
     * EMU_FMV=1 / 0 overrides it. Off by default. */
    if (fmv_init("./ROM/FMV_combined_7308hi_7307lo.bin") == 0) {
        int want = fmv_option_load() == 1;
        if (getenv("EMU_FMV") && *getenv("EMU_FMV")) want = *getenv("EMU_FMV") == '1';
        fmv_set_enabled(want);
        fmv_reset();
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
        slave_hle_set_disc(be, cdic_disc_kind(cdic), 0);
    }
    g_cdic = cdic;
    g_be = be;
    uart_console_set_hotkey_cb(hotkey);

    /* Reset the CPU (loads PC and SP from the ROM reset vector) */
    m68k_reset(&cpu);

    /* Open the SDL2 UART terminal window */
    uart_console_init();

    /* Open the SDL2 video display window (with its menu bar) */
    video_display_init();
    video_display_set_menu_cb(menu_action);
    video_display_set_swap(g_swap_buttons);
    video_display_menu_check(VM_SWAP, g_swap_buttons);
    video_display_menu_check(VM_FMV, fmv_enabled());
    video_display_menu_check(VM_CONSOLE, uart_console_is_visible());

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
    long long trace_max = 0;                 /* 0 = no limit */
    {
        const char *envm = getenv("EMU_TRACE_MAX");
        if (envm) trace_max = atoll(envm);
    }

    printf("\nStarting emulation loop...\n");

    long long traced = 0;
    int running = 1;
    long long i = 0;
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
    long long pc_poll = getenv("EMU_PC_POLL") ? atoll(getenv("EMU_PC_POLL")) : 0;
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
    long long ac_x = -1, ac_y = 0, ac_step = 0, ac_count = 8;   /* 4th value: number of clicks */
    long long watch_after = getenv("EMU_WATCH_AFTER") ? atoll(getenv("EMU_WATCH_AFTER")) : 0;
    if (getenv("EMU_AUTOCLICK"))
        sscanf(getenv("EMU_AUTOCLICK"), "%lld,%lld,%lld,%lld", &ac_x, &ac_y, &ac_step, &ac_count);
    /* Real-time pacing: emulated time (CPU cycles / 15 MHz) follows the host
     * clock. EMU_SPEED = percent of real speed (default 100, 0 = unlimited). */
    int speed = getenv("EMU_SPEED") ? atoi(getenv("EMU_SPEED")) : 100;
    uint64_t pace_cycles0 = 0, pace_t0 = SDL_GetPerformanceCounter();
    /* EMU_PACE_LOG=1: histogram of the host time between two frames */
    int pace_log = getenv("EMU_PACE_LOG") != NULL;
    long pace_hist[8] = {0};
    uint64_t pace_last = 0;
    int os9_log = getenv("EMU_OS9_LOG") && *getenv("EMU_OS9_LOG") && *getenv("EMU_OS9_LOG") != '0';
    int ring_pos = 0;

    /* --- Main emulation loop --- */
    while (running && (trace_max <= 0 || i < trace_max)) {

        /* Print instruction trace (skipping known noise loops) */
        if (trace_enable && (trace_max <= 0 || traced < trace_max)) {
            if (traced &&
                 !(cpu.pc >= 0x181cd4 && cpu.pc <= 0x181ce0) &&
                 !(cpu.pc >= 0x181CE2 && cpu.pc <= 0x181CEE) &&
                 !(cpu.pc >= 0x181D02 && cpu.pc <= 0x181D0A) &&
                 !(cpu.pc >= 0x181E8E && cpu.pc <= 0x181EAC)
                ) {
                uint32_t pc_before = cpu.pc;

                printf("[%6lld] PC=%08X op=%04X \nD0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X\nA0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X\n",
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
                fprintf(stderr, "[BREAK] PC=%06X reached at step %lld, last PCs:\n", brks[hit], i);
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
                /* EMU_DUMP=addr,len (hex): also dump that memory */
                if (getenv("EMU_DUMP")) {
                    unsigned da = 0, dl = 0;
                    sscanf(getenv("EMU_DUMP"), "%x,%x", &da, &dl);
                    for (unsigned k = 0; k < dl; k += 2) {
                        if (!(k & 31)) fprintf(stderr, "\n  MEM %06X:", da + k);
                        fprintf(stderr, " %04X", bus_read16(bus, (da + k) & ~1u));
                    }
                    fprintf(stderr, "\n");
                }
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
            if (g_play_menu) { int id = g_play_x; play_next(); menu_action(id); }
            else {
                static int last_b = 0;
                if (g_play_b != last_b && getenv("EMU_INPUT_LOG"))
                    fprintf(stderr, "[INPUT] step %lld buttons %d at %d,%d\n", i, g_play_b, g_play_x, g_play_y);
                last_b = g_play_b;
                push_pointer(be, g_play_x, g_play_y, g_play_b);
                play_next();
            }
        }
        if (ac_x >= 0) {
            /* glide towards the target (16 moves), like a real mouse, then press */
            if (i >= ac_step - 4000000 && i < ac_step && (ac_step - i) % 250000 == 0) {
                long long k = 16 - (ac_step - i) / 250000;          /* 0..15 */
                push_pointer(be, (int)(200 + (ac_x - 200) * k / 15), (int)(300 + (ac_y - 300) * k / 15), 0);
            }
            if (i == ac_step) { push_pointer(be, (int)ac_x, (int)ac_y, 1); fprintf(stderr, "[CLICK] %lld,%lld\n", ac_x, ac_y); }
            if (i == ac_step + 3000000) {      /* release, and click again later (8 times) */
                push_pointer(be, (int)ac_x, (int)ac_y, 0);
                static int ac_n = 0;
                if (++ac_n < ac_count) ac_step += 100000000; else ac_x = -1;
            }
        }
        if (os9_log)
            os9trace_step(bus, cpu.pc, cpu.a[7], cpu.d, cpu.a, cpu.sr);

        /* Execute one CPU instruction */
        int cycles = m68k_step(&cpu);
        /* EMU_CPU_STATS=1: average clock periods per instruction, every
         * 50 million instructions (instructions run, not STOP waits) */
        {
            static int stats = -1;
            static long long st_cyc = 0, st_n = 0;
            if (stats < 0) stats = getenv("EMU_CPU_STATS") != NULL;
            if (stats && !cpu.stopped) {
                st_cyc += cycles; st_n++;
                if (st_n == 50000000) {
                    fprintf(stderr, "[CPU] %.2f cycles per instruction (%.2f MIPS at 15 MHz)\n",
                            (double)st_cyc / st_n, 15.0 * st_n / st_cyc);
                    st_cyc = st_n = 0;
                }
            }
        }

        if (pc_poll && (i % pc_poll) == 0)
            fprintf(stderr, "[PCPOLL] i=%lld PC=%08X A7=%08X\n", i, cpu.pc, cpu.a[7]);

        /* Advance video timing by the consumed cycles */
        int new_frame = scc66470_tick(video, cycles);
        if (new_frame) { scc66470_vretrace(video, bus->dram, (uint32_t)sizeof(bus->dram)); fmv_frame(); }
        if (scc66470_tick(video_b, cycles)) scc66470_vretrace(video_b, bus->dram, (uint32_t)sizeof(bus->dram));

        /* On-chip timer, then present the highest pending interrupt:
         * on-chip sources (autovectors 57..63) and the 68HC05 on IN2N
         * (level 2, autovector 26 - descriptors ptr/pck/slave). */
        rtc_virtual_cycles += (uint64_t)cycles;
        onchip68070_tick(onchip, cycles);
        cdic_tick(cdic, cycles);
        fmv_tick(cycles);                          /* Digital Video Cartridge, if inserted */
        {
            int vector = 0, v2 = 0;
            int level = onchip68070_irq(onchip, &vector);
            int l2 = cdic_irq(cdic, &v2);          /* CDIC: level 4, vector from IVEC */
            if (l2 > level) { level = l2; vector = v2; }
            /* Digital Video Cartridge: level 4 too (descriptors mv / ma), vectored */
            if (level < 4 && l2 < 4) { int v3 = 0; if (fmv_irq(&v3)) { level = 4; vector = v3; } }
            /* SCC66470 IT1 (master or slave): level 5, autovector 61 ("vid") */
            if (level < 5 && (scc66470_irq(video) || scc66470_irq(video_b))) { level = 5; vector = 61; }
            if (level < 2 && be->irq && be->irq(be->ctx)) {
                level = 2;
                vector = 26;
            }
            m68k_set_irq(&cpu, level, vector);
        }

        /* 68HC05 reset request ($8A): warm reboot, the DRAM and the disc stay */
        static long long force_reset = -2;            /* EMU_FORCE_RESET=step (tests) */
        if (force_reset == -2) force_reset = getenv("EMU_FORCE_RESET") ? atoll(getenv("EMU_FORCE_RESET")) : -1;
        if (slave_hle_take_reset(be) || i == force_reset || g_reset_request) {
            g_reset_request = 0;
            fprintf(stderr, "[RESET] 68HC05 resets the 68070 (warm reboot) at step %lld\n", i);
            onchip68070_reset(onchip);
            {   /* the chips keep their wiring: the slave's ICA is at $80400 */
                uint32_t ica_a = video->ica_pointer, ica_b = video_b->ica_pointer;
                scc66470_init(video);
                scc66470_init(video_b);
                video->ica_pointer = ica_a;
                video_b->ica_pointer = ica_b;
            }
            video_display_reset_vsd();            /* V.S.D. too (palette, weights) */
            cdic_init(cdic);
            fmv_reset();
            if (be->reset) be->reset(be->ctx);
            m68k_reset(&cpu);
            uart_feed('1');
            continue;
        }

        /* Check for CPU halt (unimplemented opcode, etc.) */
        if (cpu.halted) {
            fprintf(stderr, "[HALT] CPU stopped/halted at step %lld\n", traced);
            break;
        }

        /* Once per video frame (master VSC vertical retrace, 50 Hz): poll SDL
         * events, present the picture, then pace to real time. Doing it per
         * frame (and not every N instructions) avoids presenting twice per
         * frame and sleeping in uneven chunks, which made video/audio stutter. */
        if (new_frame) {
            running = uart_console_update();
            bus_nvram_poll(bus);              /* autosave game saves / clock */
            slave_hle_frame(be);
            fflush(stderr);                   /* logs survive a killed process */
            rip_poll();                       /* disc ripping in progress */
            {   /* PC mouse captured while the game recentres the cursor */
                int cx, cy;
                int rel = slave_hle_pointer_relative(be, &cx, &cy);
                video_display_pointer_mode(rel && !g_play, cx, cy);
            }
            /* Drive door moved by the shell (OPEN / CLOSE buttons) */
            switch (slave_hle_take_tray_event(be)) {
            case 1:                                       /* OPEN: eject, choose a disc */
                cdic_eject(cdic);
                slave_hle_set_disc(be, 0, 0);
                insert_disc_dialog(0);                    /* cancel: door stays open; replay: recorded choice */
                break;
            case 2:                                       /* CLOSE: with the disc, if any */
                slave_hle_set_tray(be, 0, cdic_disc_kind(cdic));
                break;
            }
            if (pace_log) {
                uint64_t now = SDL_GetPerformanceCounter();
                if (pace_last) {
                    double ms = (double)(now - pace_last) * 1000.0 / (double)SDL_GetPerformanceFrequency();
                    int k = ms < 15 ? 0 : ms < 19 ? 1 : ms < 21 ? 2 : ms < 25 ? 3 : ms < 35 ? 4 : ms < 60 ? 5 : 6;
                    pace_hist[k]++;
                }
                pace_last = now;
            }
            if (running)
                running = video_display_update(video, bus);
            if (speed > 0) {
                /* emulated time (cycles / 15 MHz) vs host high-resolution clock */
                uint64_t freq = SDL_GetPerformanceFrequency();
                uint64_t emu_t  = (rtc_virtual_cycles - pace_cycles0) * freq / 150000u / (uint64_t)speed;
                uint64_t host_t = SDL_GetPerformanceCounter() - pace_t0;
                if (emu_t > host_t) {
                    uint64_t wait = emu_t - host_t;
                    uint64_t ms = wait * 1000u / freq;
                    if (ms > 1) SDL_Delay((Uint32)(ms - 1));          /* coarse sleep */
                    while (SDL_GetPerformanceCounter() - pace_t0 < emu_t) { }   /* fine wait (<1 ms) */
                } else if (host_t - emu_t > freq / 4) {               /* >250 ms late: resync */
                    pace_cycles0 = rtc_virtual_cycles;
                    pace_t0 = SDL_GetPerformanceCounter();
                }
            }
        }

        i++;
    }

    fflush(stdout);
    if (pace_log)
        fprintf(stderr, "[PACE] frame intervals: <15ms %ld | 15-19 %ld | 19-21 %ld | 21-25 %ld | 25-35 %ld | 35-60 %ld | >60 %ld\n",
                pace_hist[0], pace_hist[1], pace_hist[2], pace_hist[3], pace_hist[4], pace_hist[5], pace_hist[6]);

    /* Closing the video window (or Fichier > Quitter) ends the emulator.
     * If the CPU stopped by itself (halt), keep the windows open to read
     * the console, until the user closes them. */
    int win_alive = (running && !getenv("EMU_TRACE_MAX")) ? 1 : 0;
    if (win_alive) {
        fprintf(stderr, "[MAIN] CPU loop finished (traced=%lld), keeping window open.\n", traced);
        uart_console_set_visible(1);
    }
    while (win_alive) {
        win_alive = uart_console_update();
        SDL_Delay(16);  /* ~60 fps idle */
    }

    video_display_restore_host_cursor();
    audio_out_close();
    bus_destroy(bus);
    printf("\nEmulation stopped\n");
    return 0;
}
