/*
 * disc.c - CD image reader for the CDIC (see disc.h)
 */

#include "disc.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* fopen() with a UTF-8 path (Windows paths may contain accents) */
static FILE *fopen_u8(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1100], wm[8];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1100) > 0 &&
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8) > 0)
        return _wfopen(wp, wm);
#endif
    return fopen(path, mode);
}

uint8_t disc_bcd(int v)       { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
int     disc_unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

void disc_lba_to_msf(int lba, uint8_t msf[3])
{
    int a = lba + 150;
    if (a < 0) a = 0;
    msf[0] = disc_bcd(a / (60 * 75));
    msf[1] = disc_bcd((a / 75) % 60);
    msf[2] = disc_bcd(a % 75);
}

int disc_msf_to_lba(uint8_t m, uint8_t s, uint8_t f)
{
    return (disc_unbcd(m) * 60 + disc_unbcd(s)) * 75 + disc_unbcd(f) - 150;
}

static int64_t file_size(FILE *f)
{
    if (_fseeki64(f, 0, SEEK_END) != 0) return -1;
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    return n;
}

static const char *ext_of(const char *p)
{
    const char *dot = strrchr(p, '.');
    const char *sl1 = strrchr(p, '/'), *sl2 = strrchr(p, '\\');
    const char *sl = sl1 > sl2 ? sl1 : sl2;
    return (dot && dot > (sl ? sl : p)) ? dot + 1 : "";
}

static int ext_is(const char *p, const char *e)
{
    const char *x = ext_of(p);
    while (*x && *e) if (tolower((unsigned char)*x++) != *e++) return 0;
    return *x == 0 && *e == 0;
}

/* Single-track image: the whole file is track 1 starting at LBA 0 */
static int open_plain(disc_t *d, const char *path, int sector_size)
{
    d->f = fopen_u8(path, "rb");
    if (!d->f) return -1;
    int64_t n = file_size(d->f);
    d->sector_size = sector_size;
    d->ntracks = 1;
    d->track[0].number = 1;
    d->track[0].audio = 0;
    d->track[0].start = 0;
    d->track[0].file_offset = 0;
    d->leadout = (int)(n / sector_size);
    return 0;
}

static int open_cue(disc_t *d, const char *cue)
{
    FILE *c = fopen_u8(cue, "r");
    if (!c) return -1;
    char line[1024], bin[1100] = "";
    int cur = -1, nfiles = 0, sector = DISC_RAW;

    while (fgets(line, sizeof(line), c)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "FILE", 4)) {
            if (++nfiles > 1) continue;              /* only one BIN supported */
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            char name[540];
            if (q1 && q2) { size_t l = (size_t)(q2 - q1 - 1); memcpy(name, q1 + 1, l); name[l] = 0; }
            else if (sscanf(p + 4, "%511s", name) != 1) continue;
            /* relative to the .cue directory */
            const char *s1 = strrchr(cue, '/'), *s2 = strrchr(cue, '\\');
            const char *sl = s1 > s2 ? s1 : s2;
            if (sl && !strchr(name, ':') && name[0] != '/' && name[0] != '\\')
                snprintf(bin, sizeof(bin), "%.*s%s", (int)(sl - cue + 1), cue, name);
            else
                snprintf(bin, sizeof(bin), "%s", name);
        } else if (!strncmp(p, "TRACK", 5) && nfiles == 1) {
            int num; char mode[32] = "";
            if (sscanf(p + 5, "%d %31s", &num, mode) < 1 || d->ntracks >= DISC_MAX_TRACKS) continue;
            cur = d->ntracks++;
            d->track[cur].number = num;
            d->track[cur].audio = !strncmp(mode, "AUDIO", 5);
            if (strstr(mode, "/2048")) sector = 2048;
        } else if (!strncmp(p, "INDEX", 5) && cur >= 0 && nfiles == 1) {
            int idx, m, s, f;
            if (sscanf(p + 5, "%d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 1) {
                int frames = (m * 60 + s) * 75 + f;
                d->track[cur].start = frames;
                d->track[cur].file_offset = (int64_t)frames * sector;
            }
        }
    }
    fclose(c);
    if (nfiles > 1)
        fprintf(stderr, "[DISC] warning: %d FILE entries in the .cue, only the first is used\n", nfiles);
    if (!bin[0] || d->ntracks == 0) return -1;
    d->f = fopen_u8(bin, "rb");
    if (!d->f) { fprintf(stderr, "[DISC] cannot open '%s'\n", bin); return -1; }
    d->sector_size = sector;
    d->leadout = (int)(file_size(d->f) / sector);
    return 0;
}

int disc_open(disc_t *d, const char *path)
{
    disc_close(d);
    memset(d, 0, sizeof(*d));
    int r;
    if (ext_is(path, "cue"))      r = open_cue(d, path);
    else if (ext_is(path, "iso")) r = open_plain(d, path, 2048);
    else                          r = open_plain(d, path, DISC_RAW);
    if (r != 0) { disc_close(d); memset(d, 0, sizeof(*d)); return -1; }
    snprintf(d->path, sizeof(d->path), "%s", path);
    fprintf(stderr, "[DISC] '%s': %d track(s), lead-out LBA %d, %d-byte sectors\n",
            path, d->ntracks, d->leadout, d->sector_size);
    for (int i = 0; i < d->ntracks; i++)
        fprintf(stderr, "[DISC]   track %02d %s at LBA %d\n", d->track[i].number,
                d->track[i].audio ? "audio" : "data ", d->track[i].start);
    return 0;
}

void disc_close(disc_t *d)
{
    if (d->f) fclose(d->f);
    d->f = NULL;
    d->ntracks = 0;
}

int disc_present(const disc_t *d) { return d->f != NULL; }

int disc_track_of(const disc_t *d, int lba)
{
    int t = -1;
    for (int i = 0; i < d->ntracks; i++)
        if (lba >= d->track[i].start) t = i;
    return t;
}

static void header(uint8_t *s, int lba, int mode)
{
    static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memcpy(s, sync, 12);
    disc_lba_to_msf(lba, s + 12);
    s[15] = (uint8_t)mode;
}

int disc_read(disc_t *d, int lba, uint8_t out[DISC_RAW])
{
    memset(out, 0, DISC_RAW);
    int t = disc_track_of(d, lba);
    if (!d->f || t < 0 || lba < 0 || lba >= d->leadout) {
        header(out, lba, 2);                          /* empty mode 2 sector */
        return -1;
    }
    int64_t off = d->track[t].file_offset + (int64_t)(lba - d->track[t].start) * d->sector_size;
    if (_fseeki64(d->f, off, SEEK_SET) != 0) { header(out, lba, 2); return -1; }
    if (d->sector_size == 2048) {
        header(out, lba, 1);
        if (fread(out + 16, 1, 2048, d->f) != 2048) return -1;
    } else if (fread(out, 1, DISC_RAW, d->f) != DISC_RAW) {
        header(out, lba, 2);
        return -1;
    }
    return t;
}
