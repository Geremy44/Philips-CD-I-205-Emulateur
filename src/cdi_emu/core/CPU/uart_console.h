#ifndef UART_CONSOLE_H
#define UART_CONSOLE_H

#include <stdint.h>

/* Initialise la fenêtre SDL2 dédiée au terminal UART.
 * A appeler une seule fois au démarrage de l'émulateur. */
void uart_console_init(void);

/* Envoie un caractère à afficher dans le terminal (appelé depuis uart_write8) */
void uart_console_putchar(char c);

/* A appeler à chaque frame / boucle principale de l'émulateur.
 * Gère le rendu + la capture clavier. Retourne 0 si l'utilisateur
 * a fermé la fenêtre (SDL_QUIT), 1 sinon. */
int uart_console_update(void);

/* Indique si un caractère est disponible dans le buffer RX (clavier -> UART) */
int uart_console_has_input(void);

/* Récupère le prochain caractère du buffer RX (à appeler depuis uart_read8) */
uint8_t uart_console_get_input(void);

/* Libère les ressources SDL2 */
void uart_console_close(void);

#endif /* UART_CONSOLE_H */