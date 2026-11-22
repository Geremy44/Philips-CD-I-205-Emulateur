/*
 * rom_import.c - see rom_import.h
 *
 * Console ROM (PS-7211 REL 2.1): 512 KB, in the byte order of the dumps
 * (that of the Intel HEX file). Its 16-bit checksum is stored at
 * $7FBFE (low byte) / $7FBFF (high byte): the sum of bytes $0-$7FBFD plus
 * $FC00. A dump in the other byte order, or split in two 256 KB halves
 * (interleaved or one after the other), is recognised with this checksum.
 *
 * Digital Video Cartridge ROM: two 64 KB EPROMs, P7308 for the even bytes
 * (D15-D8) and P7307 for the odd bytes (D7-D0). The 16-bit sum of each chip
 * is printed on its label: FFD9 for P7308, 4BA9 for P7307 (release 4.1).
 * The merged ROM starts with the OS-9 module sync $4AFC.
 */

#include "rom_import.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(d) _mkdir(d)
#else
#include <sys/stat.h>
#define make_dir(d) mkdir(d, 0755)
#endif

#define CONSOLE_SIZE 0x80000u
#define FMV_SIZE     0x20000u
#define FMV_CHIP     0x10000u
#define MAX_FILE     0x100000u
#define MAX_FILES    8

typedef struct {
    const char *path;
    uint8_t    *data;
    size_t      size;
} rfile_t;

static void say(char *msg, size_t n, const char *fmt, ...)
{
    size_t len = strlen(msg);
    if (len + 1 >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg + len, n - len, fmt, ap);
    va_end(ap);
}

static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *s = p; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Intel HEX (records 00, 01, 02, 04) -> image; size = highest address + 1 */
static uint8_t *read_ihex(FILE *f, size_t *size)
{
    uint8_t *img = malloc(MAX_FILE);
    if (!img) return NULL;
    memset(img, 0xFF, MAX_FILE);
    char line[600];
    uint32_t base = 0, top = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ':');
        if (!p) continue;
        int cnt = hexv(p[1]) << 4 | hexv(p[2]);
        int addr = hexv(p[3]) << 12 | hexv(p[4]) << 8 | hexv(p[5]) << 4 | hexv(p[6]);
        int type = hexv(p[7]) << 4 | hexv(p[8]);
        if (cnt < 0 || addr < 0 || type < 0) continue;
        if (type == 0) {
            for (int i = 0; i < cnt; i++) {
                int hi = hexv(p[9 + 2 * i]), lo = hexv(p[10 + 2 * i]);
                if (hi < 0 || lo < 0) break;
                uint32_t a = base + (uint32_t)addr + (uint32_t)i;
                if (a < MAX_FILE) { img[a] = (uint8_t)(hi << 4 | lo); if (a + 1 > top) top = a + 1; }
            }
        } else if (type == 1) {
            break;
        } else if (type == 2 || type == 4) {
            int x = hexv(p[9]) << 12 | hexv(p[10]) << 8 | hexv(p[11]) << 4 | hexv(p[12]);
            base = type == 4 ? (uint32_t)x << 16 : (uint32_t)x << 4;
        }
    }
    *size = top;
    return img;
}

static int read_file(rfile_t *r, char *msg, size_t n)
{
    FILE *f = fopen(r->path, "rb");
    if (!f) { say(msg, n, "- %s : impossible d'ouvrir le fichier\n", base_name(r->path)); return -1; }
    int c;
    do { c = fgetc(f); } while (c == ' ' || c == '\r' || c == '\n' || c == '\t');
    rewind(f);
    if (c == ':') {
        r->data = read_ihex(f, &r->size);
    } else {
        r->data = malloc(MAX_FILE + 1);
        r->size = r->data ? fread(r->data, 1, MAX_FILE + 1, f) : 0;
    }
    fclose(f);
    if (!r->data || r->size == 0 || r->size > MAX_FILE) {
        say(msg, n, "- %s : fichier vide ou trop gros\n", base_name(r->path));
        return -1;
    }
    return 0;
}

static uint16_t sum16(const uint8_t *d, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += d[i];
    return (uint16_t)s;
}

/* 1 if this 512 KB image (dump byte order) carries a valid checksum */
static int console_ok(const uint8_t *d)
{
    uint32_t s = 0xFC00;
    for (uint32_t i = 0; i < 0x7FBFE; i++) s += d[i];
    return (uint16_t)s == (uint16_t)(d[0x7FBFE] | d[0x7FBFF] << 8);
}

static void swap_pairs(uint8_t *d, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) { uint8_t t = d[i]; d[i] = d[i + 1]; d[i + 1] = t; }
}

static int write_rom(const char *dir, const char *name, const uint8_t *d, size_t n,
                     char *msg, size_t msz)
{
    char path[1200], tmp[1210];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(d, 1, n, f) != n) {
        if (f) fclose(f);
        remove(tmp);
        say(msg, msz, "=> impossible d'écrire %s\n", path);
        return -1;
    }
    fclose(f);
    remove(path);
    if (rename(tmp, path) != 0) { say(msg, msz, "=> impossible d'écrire %s\n", path); return -1; }
    say(msg, msz, "=> %s écrit\n", path);
    return 0;
}

/* two 256 KB halves -> console ROM: interleaved or one after the other,
 * either order, either byte order; the checksum decides */
static int merge_console(const uint8_t *a, const uint8_t *b, uint8_t *out)
{
    const uint8_t *src[2][2] = { { a, b }, { b, a } };
    for (int o = 0; o < 2; o++) {
        const uint8_t *x = src[o][0], *y = src[o][1];
        for (int mode = 0; mode < 2; mode++) {
            if (mode == 0) for (uint32_t i = 0; i < CONSOLE_SIZE / 2; i++) { out[2 * i] = x[i]; out[2 * i + 1] = y[i]; }
            else { memcpy(out, x, CONSOLE_SIZE / 2); memcpy(out + CONSOLE_SIZE / 2, y, CONSOLE_SIZE / 2); }
            if (console_ok(out)) return 1;
            swap_pairs(out, CONSOLE_SIZE);
            if (console_ok(out)) return 1;
        }
    }
    return 0;
}

int rom_import_files(const char *const *paths, int n, const char *rom_dir,
                     char *msg, size_t msz)
{
    rfile_t f[MAX_FILES];
    int done = 0;
    msg[0] = 0;
    if (n > MAX_FILES) n = MAX_FILES;
    memset(f, 0, sizeof(f));

    const uint8_t *half[2] = { NULL, NULL };  int nhalf = 0;
    const uint8_t *chip_hi = NULL, *chip_lo = NULL;
    const uint8_t *chip_unknown[2] = { NULL, NULL }; int nunk = 0;
    uint8_t *console = malloc(CONSOLE_SIZE), *fmv = malloc(FMV_SIZE);
    if (!console || !fmv) { free(console); free(fmv); say(msg, msz, "Mémoire insuffisante\n"); return 0; }
    make_dir(rom_dir);                         /* may already exist */

    for (int i = 0; i < n; i++) {
        f[i].path = paths[i];
        if (read_file(&f[i], msg, msz) != 0) continue;
        const char *nm = base_name(f[i].path);
        uint8_t *d = f[i].data;
        if (f[i].size == CONSOLE_SIZE) {
            int ok = console_ok(d);
            if (!ok) { swap_pairs(d, CONSOLE_SIZE); ok = console_ok(d); if (ok) say(msg, msz, "- %s : ROM console (octets inversés, remis dans l'ordre)\n", nm); }
            else say(msg, msz, "- %s : ROM console, somme de contrôle %02X%02X correcte\n", nm, d[0x7FBFF], d[0x7FBFE]);
            if (!ok) { say(msg, msz, "- %s : 512 Ko mais somme de contrôle fausse (dump abîmé ?), ignoré\n", nm); continue; }
            if (write_rom(rom_dir, ROM_CONSOLE_FILE, d, CONSOLE_SIZE, msg, msz) == 0) done |= ROM_IMPORTED_CONSOLE;
        } else if (f[i].size == CONSOLE_SIZE / 2) {
            if (nhalf < 2) half[nhalf++] = d;
            say(msg, msz, "- %s : moitié de ROM console (256 Ko)\n", nm);
        } else if (f[i].size == FMV_SIZE) {
            if (d[0] == 0x4A && d[1] == 0xFC) {
                say(msg, msz, "- %s : ROM de cartouche FMV déjà fusionnée\n", nm);
                if (write_rom(rom_dir, ROM_FMV_FILE, d, FMV_SIZE, msg, msz) == 0) done |= ROM_IMPORTED_FMV;
            } else if (d[0] == 0xFC && d[1] == 0x4A) {
                swap_pairs(d, FMV_SIZE);
                say(msg, msz, "- %s : ROM de cartouche FMV (octets inversés, remis dans l'ordre)\n", nm);
                if (write_rom(rom_dir, ROM_FMV_FILE, d, FMV_SIZE, msg, msz) == 0) done |= ROM_IMPORTED_FMV;
            } else say(msg, msz, "- %s : 128 Ko mais pas une ROM de cartouche FMV reconnue, ignoré\n", nm);
        } else if (f[i].size == FMV_CHIP) {
            uint16_t s = sum16(d, FMV_CHIP);
            if (s == 0xFFD9) { chip_hi = d; say(msg, msz, "- %s : EPROM P7308 (octets pairs), somme FFD9\n", nm); }
            else if (s == 0x4BA9) { chip_lo = d; say(msg, msz, "- %s : EPROM P7307 (octets impairs), somme 4BA9\n", nm); }
            else {
                if (nunk < 2) chip_unknown[nunk++] = d;
                say(msg, msz, "- %s : EPROM de 64 Ko, somme %04X (version inconnue)\n", nm, s);
            }
        } else {
            say(msg, msz, "- %s : taille %u octets non reconnue, ignoré\n", nm, (unsigned)f[i].size);
        }
    }

    if (nhalf == 2) {
        if (merge_console(half[0], half[1], console)) {
            say(msg, msz, "Moitiés de la ROM console fusionnées (somme de contrôle correcte)\n");
            if (write_rom(rom_dir, ROM_CONSOLE_FILE, console, CONSOLE_SIZE, msg, msz) == 0) done |= ROM_IMPORTED_CONSOLE;
        } else say(msg, msz, "Les deux moitiés ne donnent pas une ROM console valide\n");
    } else if (nhalf == 1) say(msg, msz, "Il manque la deuxième moitié de la ROM console\n");

    /* EPROMs of an unknown release: the merged ROM must start with $4AFC */
    for (int k = 0; k < nunk; k++) {
        const uint8_t *d = chip_unknown[k];
        if (!chip_hi && d[0] == 0x4A) chip_hi = d;
        else if (!chip_lo && d[0] == 0xFC) chip_lo = d;
    }
    if (chip_hi && chip_lo) {
        for (uint32_t i = 0; i < FMV_CHIP; i++) { fmv[2 * i] = chip_hi[i]; fmv[2 * i + 1] = chip_lo[i]; }
        if (fmv[0] == 0x4A && fmv[1] == 0xFC) {
            say(msg, msz, "EPROM P7308 + P7307 fusionnées en ROM de cartouche FMV\n");
            if (write_rom(rom_dir, ROM_FMV_FILE, fmv, FMV_SIZE, msg, msz) == 0) done |= ROM_IMPORTED_FMV;
        } else say(msg, msz, "Les deux EPROM ne donnent pas une ROM de cartouche FMV valide\n");
    } else if (chip_hi || chip_lo || nunk) {
        say(msg, msz, "Il manque une EPROM de la cartouche FMV (il faut P7307 et P7308)\n");
    }

    for (int i = 0; i < n; i++) free(f[i].data);
    free(console);
    free(fmv);
    if (!done && !msg[0]) say(msg, msz, "Aucun fichier\n");
    return done;
}
