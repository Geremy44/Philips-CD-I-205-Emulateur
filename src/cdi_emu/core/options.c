/*
 * options.c - see options.h
 */

#include "options.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 32

static void option_path(char *out, int n)
{
    char *base = SDL_GetBasePath();
    snprintf(out, n, "%soptions.ini", base ? base : "");
    SDL_free(base);
}

typedef struct { char key[32]; int value; } kv_t;

static int load_all(kv_t *kv)
{
    char p[1100], line[128];
    option_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    int n = 0;
    while (n < MAX_KEYS && fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (line[0] == ';' || line[0] == '#' || !eq) continue;
        *eq = 0;
        snprintf(kv[n].key, sizeof(kv[n].key), "%s", line);
        kv[n].value = atoi(eq + 1);
        n++;
    }
    fclose(f);
    return n;
}

int option_get(const char *key, int def)
{
    kv_t kv[MAX_KEYS];
    int n = load_all(kv);
    for (int i = 0; i < n; i++) if (!strcmp(kv[i].key, key)) return kv[i].value;
    return def;
}

void option_set(const char *key, int value)
{
    kv_t kv[MAX_KEYS];
    int n = load_all(kv), i;
    for (i = 0; i < n; i++) if (!strcmp(kv[i].key, key)) break;
    if (i == n) {
        if (n == MAX_KEYS) return;
        snprintf(kv[n].key, sizeof(kv[n].key), "%s", key);
        n++;
    }
    kv[i].value = value;
    char p[1100];
    option_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "; CD-i 205 - options (menus Affichage et Materiel)\n");
    for (i = 0; i < n; i++) fprintf(f, "%s=%d\n", kv[i].key, kv[i].value);
    fclose(f);
}
