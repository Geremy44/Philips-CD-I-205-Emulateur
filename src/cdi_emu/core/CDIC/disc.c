/*
 * disc.c - CD image reader for the CDIC (see disc.h)
 */

#include "disc.h"
#include "cdrom_win.h"
#include "chd.h"
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
    d->files[0] = d->f;
    d->nfiles = 1;
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

/* Sector holds the CD-ROM sync pattern (00 FF x10 00)? */
static int has_sync(FILE *f, int64_t off)
{
    uint8_t s[12];
    static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (_fseeki64(f, off, SEEK_SET) != 0 || fread(s, 1, 12, f) != 12) return 1;
    return memcmp(s, sync, 12) == 0;
}

static int open_cue(disc_t *d, const char *cue)
{
    FILE *c = fopen_u8(cue, "r");
    if (!c) return -1;
    char line[1024];
    int cur = -1, sector = DISC_RAW;
    int file_base = 0;                 /* LBA of the current FILE's first sector */

    while (fgets(line, sizeof(line), c)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "FILE", 4)) {
            if (d->nfiles > 0)            /* next file starts after the previous one */
                file_base += (int)(file_size(d->files[d->nfiles - 1]) / sector);
            if (d->nfiles >= DISC_MAX_TRACKS) break;
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            char name[540], bin[1100];
            if (q1 && q2) { size_t l = (size_t)(q2 - q1 - 1); memcpy(name, q1 + 1, l); name[l] = 0; }
            else if (sscanf(p + 4, "%511s", name) != 1) continue;
            /* relative to the .cue directory */
            const char *s1 = strrchr(cue, '/'), *s2 = strrchr(cue, '\\');
            const char *sl = s1 > s2 ? s1 : s2;
            if (sl && !strchr(name, ':') && name[0] != '/' && name[0] != '\\')
                snprintf(bin, sizeof(bin), "%.*s%s", (int)(sl - cue + 1), cue, name);
            else
                snprintf(bin, sizeof(bin), "%s", name);
            FILE *f = fopen_u8(bin, "rb");
            if (!f) { fprintf(stderr, "[DISC] cannot open '%s'\n", bin); fclose(c); return -1; }
            d->files[d->nfiles++] = f;
        } else if (!strncmp(p, "TRACK", 5) && d->nfiles > 0) {
            int num; char mode[32] = "";
            if (sscanf(p + 5, "%d %31s", &num, mode) < 1 || d->ntracks >= DISC_MAX_TRACKS) continue;
            cur = d->ntracks++;
            d->track[cur].number = num;
            d->track[cur].audio = !strncmp(mode, "AUDIO", 5);
            d->track[cur].file = d->nfiles - 1;
            d->track[cur].start = file_base;
            d->track[cur].pregap = -1;
            d->track[cur].cdi = !strncmp(mode, "CDI", 3);
            if (strstr(mode, "/2048")) sector = 2048;
        } else if (!strncmp(p, "INDEX", 5) && cur >= 0) {
            int idx, m, s, f;
            if (sscanf(p + 5, "%d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 0)
                d->track[cur].pregap = file_base + (m * 60 + s) * 75 + f;
            else if (sscanf(p + 5, "%d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 1) {
                int frames = (m * 60 + s) * 75 + f;
                d->track[cur].start = file_base + frames;
                d->track[cur].file_offset = (int64_t)frames * sector;
            }
        }
    }
    fclose(c);
    if (d->nfiles == 0 || d->ntracks == 0) return -1;
    d->f = d->files[0];
    d->sector_size = sector;
    d->leadout = file_base + (int)(file_size(d->files[d->nfiles - 1]) / sector);
    for (int i = 0; i < d->ntracks; i++)
        if (d->track[i].pregap < 0 || d->track[i].pregap > d->track[i].start) d->track[i].pregap = d->track[i].start;
    /* "MODE2" track that is really CD-DA (no sync pattern): play it as audio */
    for (int i = 0; i < d->ntracks && sector == DISC_RAW; i++)
        if (!d->track[i].audio && !has_sync(d->files[d->track[i].file], d->track[i].file_offset)) {
            d->track[i].audio = 1;
            fprintf(stderr, "[DISC] track %02d has no CD-ROM sync: audio (CD-DA)\n", d->track[i].number);
        }
    return 0;
}

#define DRIVE_CACHE 32                  /* sectors read at once from a drive */

/* "cdrom:X": the disc in the PC's drive X: */
static int open_drive(disc_t *d, char letter)
{
    cdrom_toc_t toc;
    d->drive = cdrom_open(letter);
    if (!d->drive) { fprintf(stderr, "[DISC] cannot open drive %c:\n", letter); return -1; }
    if (cdrom_read_toc(d->drive, &toc) != 0 || toc.ntracks < 1) {
        fprintf(stderr, "[DISC] no readable disc in drive %c:\n", letter);
        cdrom_close(d->drive); d->drive = NULL;
        return -1;
    }
    d->cache = malloc((size_t)DRIVE_CACHE * DISC_RAW);
    if (!d->cache) { cdrom_close(d->drive); d->drive = NULL; return -1; }
    d->cache_n = 0;
    d->sector_size = DISC_RAW;
    d->ntracks = toc.ntracks;
    for (int i = 0; i < toc.ntracks; i++) {
        d->track[i].number = toc.number[i];
        d->track[i].audio = toc.audio[i];
        d->track[i].start = toc.start[i];
        d->track[i].pregap = toc.start[i];
    }
    d->leadout = toc.leadout;
    return 0;
}

static void header(uint8_t *s, int lba, int mode);

/* MAME CHD image: tracks laid out one after the other from LBA 0, with
 * their pregap (INDEX 00) when the CHD has one */
static int open_chd(disc_t *d, const char *path)
{
    char err[300];
    d->chd = chd_open(path, err, sizeof(err));
    if (!d->chd) { fprintf(stderr, "[DISC] CHD: %s\n", err); return -1; }
    int lba = 0, n = chd_ntracks(d->chd);
    for (int i = 0; i < n && i < DISC_MAX_TRACKS; i++) {
        const chd_track_t *c = chd_track(d->chd, i);
        disc_track_t *t = &d->track[i];
        int stored = c->pregap_stored ? c->pregap : 0;
        t->number = i + 1;
        t->audio = c->type == CHD_AUDIO;
        t->cdi = 0;
        t->chd_type = c->type;
        if (i == 0) { t->pregap = 0; t->start = 0; }  /* track 1 pregap: before LBA 0 */
        else { t->pregap = lba; t->start = lba + c->pregap; }
        t->chd_base = c->first_frame + stored;          /* CHD frame of LBA start */
        t->chd_lo = t->start - (i == 0 ? 0 : stored);
        t->chd_hi = t->start + c->frames - stored;
        lba = t->chd_hi + c->postgap;
        d->ntracks = i + 1;
    }
    d->leadout = lba;
    d->sector_size = DISC_RAW;
    return d->ntracks > 0 ? 0 : -1;
}

/* One sector of a CHD image, as the 2352 raw bytes of the disc */
static int read_chd(disc_t *d, int lba, uint8_t *out)
{
    int t = -1;
    for (int i = 0; i < d->ntracks; i++) if (lba >= d->track[i].pregap) t = i;
    if (t < 0) return -1;
    const disc_track_t *k = &d->track[t];
    if (lba < k->chd_lo || lba >= k->chd_hi) {        /* pregap not in the image */
        if (!k->audio) header(out, lba, k->chd_type == CHD_MODE1 || k->chd_type == CHD_MODE1_RAW ? 1 : 2);
        return t;
    }
    uint8_t f[CHD_FRAME];
    if (chd_read_frame(d->chd, k->chd_base + (lba - k->start), f) != 0) { header(out, lba, 2); return -1; }
    switch (k->chd_type) {
    case CHD_AUDIO:                                    /* stored big-endian */
        for (int i = 0; i < DISC_RAW; i += 2) { out[i] = f[i + 1]; out[i + 1] = f[i]; }
        break;
    case CHD_MODE1_RAW: case CHD_MODE2_RAW:
        memcpy(out, f, DISC_RAW);
        break;
    case CHD_MODE1:
        header(out, lba, 1); memcpy(out + 16, f, 2048);
        break;
    case CHD_MODE2:
        header(out, lba, 2); memcpy(out + 16, f, 2336);
        break;
    case CHD_MODE2_FORM1:
        header(out, lba, 2); memcpy(out + 24, f, 2048);
        break;
    case CHD_MODE2_FORM2:
        header(out, lba, 2); out[18] = out[22] = 0x20; memcpy(out + 24, f, 2324);
        break;
    }
    return t;
}

int disc_open(disc_t *d, const char *path)
{
    disc_close(d);
    memset(d, 0, sizeof(*d));
    int r;
    if (!strncmp(path, "cdrom:", 6) && path[6]) r = open_drive(d, path[6]);
    else if (ext_is(path, "cue")) r = open_cue(d, path);
    else if (ext_is(path, "iso")) r = open_plain(d, path, 2048);
    else if (ext_is(path, "chd")) r = open_chd(d, path);
    else                          r = open_plain(d, path, DISC_RAW);
    if (r != 0) { disc_close(d); memset(d, 0, sizeof(*d)); return -1; }
    if (!d->drive && !d->chd && !ext_is(path, "cue") && d->sector_size == DISC_RAW && !has_sync(d->f, 0)) {
        d->track[0].audio = 1;                       /* raw .bin of an audio CD */
        fprintf(stderr, "[DISC] no CD-ROM sync: audio CD image (one track)\n");
    }
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
    for (int i = 0; i < d->nfiles; i++)
        if (d->files[i]) fclose(d->files[i]);
    d->nfiles = 0;
    d->f = NULL;
    if (d->drive) cdrom_close(d->drive);
    d->drive = NULL;
    if (d->chd) chd_close(d->chd);
    d->chd = NULL;
    free(d->cache);
    d->cache = NULL;
    d->ntracks = 0;
}

int disc_present(const disc_t *d) { return d->f != NULL || d->drive != NULL || d->chd != NULL; }

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
    if (!disc_present(d) || t < 0 || lba < 0 || lba >= d->leadout) {
        header(out, lba, 2);                          /* empty mode 2 sector */
        return -1;
    }
    if (d->chd) return read_chd(d, lba, out);
    if (d->drive) {                                   /* physical drive, read ahead */
        if (lba < d->cache_lba || lba >= d->cache_lba + d->cache_n) {
            int n = d->leadout - lba < DRIVE_CACHE ? d->leadout - lba : DRIVE_CACHE;
            int tr_end = (t + 1 < d->ntracks) ? d->track[t + 1].start : d->leadout;
            if (lba + n > tr_end) n = tr_end - lba;  /* one track per read */
            d->cache_n = 0;
            int ok = 0;
            for (int tries = 0; tries < 3 && !ok; tries++)   /* busy / spinning up */
                ok = cdrom_read_raw(d->drive, lba, n, d->track[t].audio, d->cache) == 0;
            for (int tries = 0; tries < 3 && !ok; tries++) {  /* then this sector alone */
                n = 1;
                ok = cdrom_read_raw(d->drive, lba, 1, d->track[t].audio, d->cache) == 0;
            }
            if (!ok) {
                fprintf(stderr, "[DISC] drive read error at LBA %d\n", lba);
                header(out, lba, 2);
                return -1;
            }
            d->cache_lba = lba;
            d->cache_n = n;
        }
        memcpy(out, d->cache + (size_t)(lba - d->cache_lba) * DISC_RAW, DISC_RAW);
        return t;
    }
    int64_t off = d->track[t].file_offset + (int64_t)(lba - d->track[t].start) * d->sector_size;
    FILE *f = d->files[d->track[t].file];
    if (_fseeki64(f, off, SEEK_SET) != 0) { header(out, lba, 2); return -1; }
    if (d->sector_size == 2048) {
        header(out, lba, 1);
        if (fread(out + 16, 1, 2048, f) != 2048) return -1;
    } else if (fread(out, 1, DISC_RAW, f) != DISC_RAW) {
        header(out, lba, 2);
        return -1;
    }
    return t;
}

/* Disc name for the window title: volume identifier of the CD-i disc label
 * or of the ISO 9660 descriptor (sector 16), else the image file name */
void disc_title(disc_t *d, char *out, int n)
{
    uint8_t s[DISC_RAW];
    out[0] = 0;
    if (!disc_present(d)) return;
    if (d->ntracks > 0 && !d->track[0].audio && disc_read(d, d->track[0].start + 16, s) >= 0) {
        const uint8_t *u = s + (s[15] == 1 ? 16 : 24);
        if ((u[0] == 1 && !memcmp(u + 1, "CD-I ", 5)) || (u[0] == 1 && !memcmp(u + 1, "CD001", 5))) {
            int len = 32, k = 0, lower = 1;
            while (len > 0 && (u[40 + len - 1] == ' ' || u[40 + len - 1] == 0)) len--;
            for (int i = 0; i < len && k < n - 1; i++) {
                char c = (char)u[40 + i];
                if (c == '_') c = ' ';
                if (c < 32 || c > 126) continue;
                if (isupper((unsigned char)c)) lower = 0;
                out[k++] = c;
            }
            out[k] = 0;
            if (k && lower) out[0] = (char)toupper((unsigned char)out[0]);   /* "asterix" */
            if (k) return;
        }
    }
    if (d->drive) {
        snprintf(out, n, "%s du lecteur %c:", d->track[0].audio ? "CD audio" : "Disque", d->path[6]);
        return;
    }
    const char *b = d->path;                          /* file name without folder / extension */
    for (const char *p = d->path; *p; p++) if (*p == '/' || *p == 92) b = p + 1;   /* 92 = backslash */
    snprintf(out, n, "%s", b);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
    if (d->ntracks > 0 && d->track[0].audio) {
        char t[300];
        snprintf(t, sizeof(t), "CD audio - %s", out);
        snprintf(out, n, "%s", t);
    }
}

/* .bin + .cue of any open image (used to convert a CHD). Track 1 starts at
 * the beginning of the .bin; the pregaps of the next tracks are written
 * (INDEX 00) so that every track keeps its place on the disc. */
int disc_write_bincue(disc_t *d, const char *bin, const char *cue, volatile long *progress, char *err, int n)
{
    if (!disc_present(d) || d->leadout <= 0) { snprintf(err, n, "Aucune image ouverte."); return -1; }
    FILE *b = fopen_u8(bin, "wb");
    if (!b) { snprintf(err, n, "Impossible de cr\xC3\xA9" "er :\n%s", bin); return -1; }
    uint8_t *buf = malloc((size_t)64 * DISC_RAW);
    if (!buf) { fclose(b); snprintf(err, n, "M\xC3\xA9moire insuffisante."); return -1; }
    int bad = 0;
    for (int lba = 0; lba < d->leadout; ) {
        int k = d->leadout - lba < 64 ? d->leadout - lba : 64;
        for (int i = 0; i < k; i++)
            if (disc_read(d, lba + i, buf + (size_t)i * DISC_RAW) < 0) bad++;
        if (fwrite(buf, DISC_RAW, (size_t)k, b) != (size_t)k) {
            free(buf); fclose(b);
            snprintf(err, n, "\xC3\x89" "criture impossible (disque plein ?) :\n%s", bin);
            return -1;
        }
        lba += k;
        if (progress) *progress = (long)((int64_t)lba * 100 / d->leadout);
    }
    free(buf);
    if (fclose(b) != 0) { snprintf(err, n, "\xC3\x89" "criture impossible :\n%s", bin); return -1; }
    FILE *c = fopen_u8(cue, "wb");
    if (!c) { snprintf(err, n, "Impossible de cr\xC3\xA9" "er :\n%s", cue); return -1; }
    const char *base = bin;
    for (const char *p = bin; *p; p++) if (*p == '/' || *p == 92) base = p + 1;   /* 92 = backslash */
    fprintf(c, "FILE \"%s\" BINARY\r\n", base);
    for (int i = 0; i < d->ntracks; i++) {
        const disc_track_t *t = &d->track[i];
        const char *mode = t->audio ? "AUDIO"
                         : (d->chd && (t->chd_type == CHD_MODE1 || t->chd_type == CHD_MODE1_RAW)) ? "MODE1/2352" : "MODE2/2352";
        fprintf(c, "  TRACK %02d %s\r\n", i + 1, mode);
        if (i > 0 && t->pregap < t->start)
            fprintf(c, "    INDEX 00 %02d:%02d:%02d\r\n", t->pregap / 4500, t->pregap / 75 % 60, t->pregap % 75);
        fprintf(c, "    INDEX 01 %02d:%02d:%02d\r\n", t->start / 4500, t->start / 75 % 60, t->start % 75);
    }
    fclose(c);
    if (bad) { snprintf(err, n, "%d secteurs illisibles (remplis de z\xC3\xA9ros).", bad); return 1; }
    return 0;
}

