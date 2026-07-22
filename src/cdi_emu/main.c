#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h> 

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
#endif

#include "core/CPU/m68k.h"
#include "core/CPU/uart_console.h"   /* inclut SDL, mais deja gere */
#include "core/bus.h"
#include "core/rom_loader.h"
#include "core/GPU/scc66470.h"
#include "core/SLAVE/slave.h"

static scc66470_t g_video;   /* ou dans ta struct system globale */
static slave_t g_slave;

void debug_rom_checksum(bus_t *bus)
{
    uint32_t sum = 0;

    /* Somme des octets [0x180000 .. 0x1FFBFD] (exclut les 2 octets checksum) */
    for (uint32_t a = 0x180000; a <= 0x1FFBFD; a++)
        sum += bus_read8(bus, a);

    /* Compensation des 1024 derniers octets supposes = 0xFF */
    sum += 0xFC00;

    uint16_t cksum16 = sum & 0xFFFF;
    uint8_t  lsb = cksum16 & 0xFF;
    uint8_t  msb = (cksum16 >> 8) & 0xFF;
    uint8_t  displayed = (lsb + msb) & 0xFF;

    /* Valeurs de reference stockees en ROM */
    uint8_t ref_lsb = bus_read8(bus, 0x1FFBFF);   /* attendu 0xA6 */
    uint8_t ref_msb = bus_read8(bus, 0x1FFBFE);   /* attendu 0x53 */
    uint16_t ref_cksum16 = (ref_msb << 8) | ref_lsb;      /* 0x53A6 */
    uint8_t  ref_displayed = (ref_lsb + ref_msb) & 0xFF;  /* 0xF9   */

    fprintf(stderr, "=== ROM CHECKSUM DEBUG ===\n");
    fprintf(stderr, "Somme brute        : 0x%08X\n", sum);
    fprintf(stderr, "Calcule  cksum16   : 0x%04X\n", cksum16);
    fprintf(stderr, "Ref      cksum16   : 0x%04X  %s\n",
            ref_cksum16, (cksum16 == ref_cksum16) ? "<<< MATCH" : "!!! DIFF");
    fprintf(stderr, "Calcule  affiche   : 0x%02X\n", displayed);
    fprintf(stderr, "Ref      affiche   : 0x%02X  %s\n",
            ref_displayed, (displayed == ref_displayed) ? "<<< MATCH" : "!!! DIFF");
}

int main(void) {

    // AU LIEU DE _IONBF :
    setvbuf(stdout, NULL, _IOLBF, 4096);   // ligne par ligne
    setvbuf(stderr, NULL, _IOFBF, 65536);  // full buffer 64K

    printf("=== CD-I 205 Emulator ===\n");
    SDL_SetMainReady();

#ifdef _WIN32
    setvbuf(stdin, NULL, _IONBF, 0);
#endif

    bus_t *bus = bus_create();
    if (bus_load_ihex(bus, "./ROM/CDI205-00_PS-7211_REL.2.1.hex") != 0) {
        fprintf(stderr,"Failed to load ROM\n");
        bus_destroy(bus); return 1;
    }
    printf("ROM loaded successfully\n");

    debug_rom_checksum(bus); 

    m68k_t cpu;
    m68k_init(&cpu);
    m68k_set_bus(&cpu, bus);

    slave_backend_t *be = NULL;

    /* essaie LLE si une ROM HC05 est fournie … */
    /* be = slave_lle_create(hc05_rom, hc05_len); */

    /* … sinon (cas actuel) : HLE */
    if (!be) {
        be = slave_hle_create();
        fprintf(stderr, "[SLAVE] backend = %s\n", be->name);
    }
    
    /* après création du slave et du bus : */
    slave_hle_set_uart(be, (uart_inject_fn)uart_inject_rx, bus);

    slave_attach(&g_slave, be);
    slave_reset(&g_slave);

    m68k_reset(&cpu);

    uart_console_init();

    printf("PC=0x%08X SR=0x%04X A7=0x%08X\n",
           cpu.pc, cpu.sr, cpu.a[7]);

    uart_feed('1');

    /* ================================================================
     * Contrôle du traçage instruction-par-instruction
     * - trace_enable : 1 = actif, 0 = désactivé
     * - trace_max    : nombre d'instructions avant arrêt (défaut=0 → illimité)
     * ================================================================ */
    int  trace_enable = 1;
    long trace_max = 1410065408;
    {
        //const char *env = getenv("EMU_TRACE");
        const char *envm = getenv("EMU_TRACE_MAX");
        if (envm) trace_max = atol(envm);
    }

    printf("\nStarting emulation loop...\n");

    long traced = 0;
    int running = 1;
    long i = 0;
    int update_counter = 0;
    /* Entree de la boucle : capturer A1 initial (une seule fois) */
    static int cksum_start_captured = 0;

    while (running && i < trace_max) {

        /* Trace AVANT exécution (état entrant de l'instruction) */
        if (trace_enable && traced < trace_max) {
            if (traced /*> 6290 && traced < 8000*/) {

                // Active le trace SEULEMENT quand on approche la zone
                static int trace_zone = 0;
                if (cpu.pc == 0x00182280) trace_zone = 1;
                if (cpu.pc == 0x001822D0) trace_zone = 0;

                if (trace_zone) {
                    uint32_t pc_before = cpu.pc;

                    // if (cpu.pc == 0x0018228C && !cksum_start_captured) {
                    //     printf("===== Checksum start =====\n");
                    //     cksum_start_captured = 1;
                    // }

                    // if (cpu.pc >= 0x181cf0 && cpu.pc <= 0x181cfa) {
                    //      printf("[%6ld] PC=%08X op=%04X \nD0=%08X D1=%08X D2=%08X D3=%08X D4=%08X D5=%08X D6=%08X D7=%08X\nA0=%08X A1=%08X A2=%08X A3=%08X A4=%08X A5=%08X A6=%08X A7=%08X\n",
                    //         traced, pc_before, bus_read16(bus, pc_before),
                    //         cpu.d[0], cpu.d[1], cpu.d[2], cpu.d[3], cpu.d[4], cpu.d[5], cpu.d[6], cpu.d[7],
                    //         cpu.a[0], cpu.a[1], cpu.a[2], cpu.a[3], cpu.a[4], cpu.a[5], cpu.a[6], cpu.a[7]);
                    // }

                    // if(cpu.pc==0x182298||cpu.pc==0x1822a0||cpu.pc==0x1822aa)
                    //     fprintf(stderr,"[CK] PC=%06X D1=%04X A1=%06X (A1)=%02X\n",
                    //             cpu.pc, cpu.d[1]&0xFFFF, cpu.a[1], bus_read8(bus, cpu.a[1]));

                    // if (cpu.pc >= 0x00181cf0 && cpu.pc <= 0x00181d5c) {
                    //     fprintf(stderr, "[HEXDISP] PC=%06X op=%04X D0=%08X D1=%08X\n",
                    //             cpu.pc, bus_read16(bus, cpu.pc), cpu.d[0], cpu.d[1]);
                    // }

                    // if (cpu.pc == 0x00181d0e) {
                    //     fprintf(stderr, "[DISP-CKSUM] D1=%08X A2=%08X\n", cpu.d[1], cpu.a[2]);
                    // }

                    // if (cpu.pc == 0x181cfa) {
                    //     printf("[CPU] Halted, reached desired PC\n");
                    //     cpu.halted = 1;
                    //     break;
                    // }

                }

            }

            traced++;
        }

        int cycles = m68k_step(&cpu);

        scc66470_tick(&g_video, cycles);
    
        if (cpu.halted) {
            fprintf(stderr, "[HALT] CPU stopped/halted at step %ld\n", traced);
            break;
        }

        if (++update_counter >= 20000) {
            update_counter = 0;
            running = uart_console_update();  /* retourne 0 si SDL_QUIT */
        }
        
        i++;
    }

    fflush(stdout);

    fprintf(stderr, "[MAIN] CPU loop terminee (traced=%ld), fenetre maintenue.\n", traced);
    fprintf(stderr, "[MAIN] Fermez avec la croix de la fenetre.\n");

    int win_alive = 1;
    while (win_alive) {

        if (++update_counter >= 100) {
            update_counter = 0;
            win_alive = uart_console_update();  /* retourne 0 si SDL_QUIT */
        }
        SDL_Delay(16);
    }

    uart_console_close();
    bus_destroy(bus);
    printf("\nEmulation stopped\n");
    return 0;
}
