#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
#endif
#include "core/CPU/m68k.h"
#include "core/CPU/uart_console.h"
#include "core/bus.h"
#include "core/rom_loader.h"
#include "core/GPU/scc66470.h"
#include "core/SLAVE/slave.h"

static scc66470_t g_video;   /* ou dans ta struct system globale */
static slave_t g_slave;

int main(void) {
    printf("=== CD-I 205 Emulator ===\n");

#ifdef _WIN32
    setvbuf(stdin, NULL, _IONBF, 0);
#endif

    bus_t *bus = bus_create();
    if (bus_load_ihex(bus, "./ROM/CDI205-00_PS-7211_REL.2.1.hex") != 0) {
        fprintf(stderr,"Failed to load ROM\n");
        bus_destroy(bus); return 1;
    }
    printf("ROM loaded successfully\n");

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
    slave_attach(&g_slave, be);
    slave_reset(&g_slave);



    m68k_reset(&cpu);


    uart_console_init();

    int running = 1;


    printf("PC=0x%08X SR=0x%04X A7=0x%08X\n",
           cpu.pc, cpu.sr, cpu.a[7]);

    uart_feed('1');

    /* ================================================================
     * Contrôle du traçage instruction-par-instruction
     * - trace_enable : 1 = actif, 0 = désactivé
     * - trace_max    : nombre d'instructions avant arrêt (défaut=0 → illimité)
     * ================================================================ */
    int  trace_enable = 1;
    long trace_max = 10000;
    bool enter_service_mode = true;
    {
        //const char *env = getenv("EMU_TRACE");
        const char *envm = getenv("EMU_TRACE_MAX");
        if (envm) trace_max = atol(envm);
    }

    printf("\nStarting emulation loop...\n");
    // if (trace_enable) {
    //     printf("[TRACE] actif (max=%ld instr)  format: "
    //            "STEP  PC  ->  OPCODE  | A7 SR  |  D0 A4 A5\n", trace_max);
    // }

    long traced = 0;

    if (enter_service_mode)
        uart_inject_rx(bus, 0x05);   /* ^E pour entrer dans le menu de test */

    // /* Hypothèse "^E" = deux caractères 0x5E puis 0x45 */
    // static const uint8_t script[] = { 0x5E, 0x45 };
    // static int idx = 0;
    // if (enter_service_mode && idx < (int)sizeof(script))
    //     uart_inject_rx(bus, script[idx++]);

    for (long i = 0; i < 7000; i++) {
        uart_poll_host_input();

        /* Trace AVANT exécution (état entrant de l'instruction) */
        if (trace_enable && traced < trace_max) {
            if (traced > 6290 /*&& traced < 8000*/) {
                uint32_t pc_before = cpu.pc;
                printf("[STEP %6ld] PC=0x%08X -> opcode=0x%04X "
                    "| A7=0x%08X SR=0x%04X | D0=0x%08X D7=0x%08X A1=0x%08X A2=0x%08X A3=0x%08X A4=0x%08X A5=0x%08X\n",
                    traced,
                    pc_before,
                    bus_read16(bus, pc_before),
                    cpu.a[7], cpu.sr,
                    cpu.d[0], cpu.d[7], cpu.a[1], cpu.a[2], cpu.a[3], cpu.a[4], cpu.a[5]);

                
            }
      
            if (enter_service_mode && traced == 20)
                uart_inject_rx(bus, 0x05);
            traced++;
        }

        if (cpu.pc == 0x00180E40)
            fprintf(stderr, "[CPU] cmp attendu = %02X, D1 lu = %02X\n", (m68k_read16(&cpu, (cpu.pc + 2)) && 0xFF ) , cpu.d[1] & 0xFF);

        /* UNE SEULE exécution par tour */
        int cycles = m68k_step(&cpu);

        /* Avance la synchro vidéo avec le bon nombre de cycles */
        scc66470_tick(&g_video, cycles);


        /* Détection de halt : à adapter selon ta convention de retour */
        if (cpu.halted) {              /* ou: if (cycles <= 0) selon ton API */
            fprintf(stderr, "[HALT] CPU stopped/halted at step %ld\n", traced);
            break;
        }

        if (trace_max > 0 && traced >= trace_max) {
            fflush(stdout);
            return 0;
        }

        /* Rafraîchir la fenêtre UART régulièrement (pas nécessairement
         * à chaque instruction — toutes les N cycles ou 1x/frame) */
        running = uart_console_update();
    }

    bus_destroy(bus);
    uart_console_close();
    printf("\nEmulation stopped\n");
    return 0;
}
