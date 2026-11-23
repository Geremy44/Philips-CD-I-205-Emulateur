/*
 * cdburn_win.c - burn a disc image to a blank CD (see cdburn.h)
 *
 * MMC sequence (session at once):
 *   TEST UNIT READY, GET CONFIGURATION (CD-R / CD-RW, CD mastering
 *   feature), READ DISC INFORMATION (blank, capacity), MODE SELECT page 05
 *   (write type SAO, test write, buffer underrun protection, session
 *   format CD-i / CD-ROM XA / CD-DA), SET CD SPEED, SEND CUE SHEET,
 *   WRITE(10) from LBA -150 (pregap of track 1) to the lead-out,
 *   SYNCHRONIZE CACHE (the drive writes the lead-out and closes the disc).
 * Then, for a real burn, the data tracks are read back and compared with
 * the image, and the disc is ejected.
 */

#include "cdburn.h"
#include "disc.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#define RAW 2352
#define MAX_CHUNK 26                       /* sectors per WRITE (61 KB) */

/* ---------------- SCSI pass-through ---------------- */

/* 0 = good, -1 = the request did not reach the drive,
 * else 0x1000000 | sense key << 16 | ASC << 8 | ASCQ */
static int scsi(HANDLE h, const uint8_t *cdb, int cl, void *buf, int len, int dir, int timeout)
{
    struct { SCSI_PASS_THROUGH_DIRECT s; ULONG pad; UCHAR sense[32]; } p;
    memset(&p, 0, sizeof(p));
    p.s.Length = sizeof(p.s);
    p.s.CdbLength = (UCHAR)cl;
    p.s.SenseInfoLength = sizeof(p.sense);
    p.s.SenseInfoOffset = (ULONG)((char *)p.sense - (char *)&p);
    p.s.DataIn = (UCHAR)(len ? dir : SCSI_IOCTL_DATA_UNSPECIFIED);
    p.s.DataTransferLength = (ULONG)len;
    p.s.TimeOutValue = (ULONG)timeout;
    p.s.DataBuffer = len ? buf : NULL;
    memcpy(p.s.Cdb, cdb, (size_t)cl);
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &p, sizeof(p), &p, sizeof(p), &got, NULL))
        return -1;
    if (p.s.ScsiStatus == 0) return 0;
    int key, asc, ascq;
    if ((p.sense[0] & 0x7F) == 0x72) { key = p.sense[1] & 15; asc = p.sense[2]; ascq = p.sense[3]; }
    else                             { key = p.sense[2] & 15; asc = p.sense[12]; ascq = p.sense[13]; }
    return 0x1000000 | key << 16 | asc << 8 | ascq;
}

/* Log: stderr and gravure.log next to the executable (kept across runs:
 * the window has no console, and a burn must leave a trace) */
static void blog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    char p[MAX_PATH + 16];
    DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    while (n > 0 && p[n - 1] != 92 && p[n - 1] != '/') n--;      /* 92 = backslash */
    strcpy(p + n, "gravure.log");
    FILE *f = fopen(p, "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* Test write bit of the current write parameters page: 1, 0, -1 = unreadable */
static int page05_test(HANDLE h, int *byte2)
{
    uint8_t cdb[10] = { 0x5A, 0x08, 0x05, 0, 0, 0, 0, 0, 64, 0 };
    uint8_t ms[64];
    memset(ms, 0, sizeof(ms));
    if (scsi(h, cdb, 10, ms, 64, SCSI_IOCTL_DATA_IN, 10) != 0) return -1;
    int bdl = ms[6] << 8 | ms[7];
    if (8 + bdl + 3 > 64) return -1;
    const uint8_t *pg = ms + 8 + bdl;
    if ((pg[0] & 0x3F) != 0x05) return -1;
    if (byte2) *byte2 = pg[2];
    return (pg[2] & 0x10) ? 1 : 0;
}

#define S_KEY(r)  (((r) >> 16) & 0x0F)
#define S_ASC(r)  (((r) >> 8) & 0xFF)
#define S_ASCQ(r) ((r) & 0xFF)
/* not ready: becoming ready / operation or long write in progress */
#define S_BUSY(r) ((r) > 0 && S_KEY(r) == 2 && S_ASC(r) == 0x04)

static const char *sense_text(int r)
{
    if (r < 0) return "le graveur ne répond pas";
    if (S_KEY(r) == 2 && S_ASC(r) == 0x3A) return "pas de disque";
    if (S_KEY(r) == 3 && S_ASC(r) == 0x73) return "échec de calibrage du laser (disque de mauvaise qualité ou incompatible)";
    if (S_KEY(r) == 3 && S_ASC(r) == 0x0C) return "erreur d'écriture";
    if (S_KEY(r) == 3) return "erreur du support";
    if (S_KEY(r) == 4) return "erreur matérielle du graveur";
    if (S_KEY(r) == 5 && S_ASC(r) == 0x21) return "adresse d'écriture refusée";
    if (S_KEY(r) == 5 && S_ASC(r) == 0x24) return "commande refusée par le graveur";
    if (S_KEY(r) == 5 && S_ASC(r) == 0x26) return "paramètres refusés par le graveur";
    if (S_KEY(r) == 5 && S_ASC(r) == 0x64) return "mode d'écriture refusé pour cette piste";
    if (S_KEY(r) == 5) return "requête refusée par le graveur";
    if (S_KEY(r) == 6) return "le disque a changé";
    return "erreur du graveur";
}

/* ---------------- state shared with the UI thread ---------------- */

static CRITICAL_SECTION lock;
static int lock_init;
static volatile LONG st_progress = -1;     /* -1 idle */
static volatile LONG st_done;
static char st_stage[64], st_msg[1600];
static char b_letter, b_image[1100];
static int  b_simulate;

static void init_lock(void)
{
    if (!lock_init) { InitializeCriticalSection(&lock); lock_init = 1; }
}

static void set_stage(const char *s, int pct)
{
    EnterCriticalSection(&lock);
    snprintf(st_stage, sizeof(st_stage), "%s", s);
    LeaveCriticalSection(&lock);
    InterlockedExchange(&st_progress, pct);
}

static void finish(const char *msg)
{
    EnterCriticalSection(&lock);
    snprintf(st_msg, sizeof(st_msg), "%s", msg);
    LeaveCriticalSection(&lock);
    InterlockedExchange(&st_done, 1);
}

/* ---------------- image ---------------- */

/* Raw sector at image LBA, pregaps (INDEX 00) included */
static int img_read(disc_t *d, int lba, uint8_t *out)
{
    int t = 0;
    for (int i = 0; i < d->ntracks; i++) if (lba >= d->track[i].pregap) t = i;
    int64_t off = d->track[t].file_offset + (int64_t)(lba - d->track[t].start) * RAW;
    FILE *f = d->files[d->track[t].file];
    if (off < 0 || _fseeki64(f, off, SEEK_SET) != 0 || fread(out, 1, RAW, f) != RAW) {
        memset(out, 0, RAW);
        return -1;
    }
    return 0;
}

static const uint8_t SYNC[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };

/* 0 = audio, 1 = mode 1, 2 = mode 2 (CD-ROM XA / CD-i) */
static int track_kind(disc_t *d, int i)
{
    uint8_t s[RAW];
    if (d->track[i].audio) return 0;
    if (img_read(d, d->track[i].start, s) != 0 || memcmp(s, SYNC, 12)) return 0;
    return s[15] == 1 ? 1 : 2;
}

static int image_is_cdi(disc_t *d)
{
    uint8_t s[RAW];
    for (int i = 0; i < d->ntracks; i++) if (d->track[i].cdi) return 1;
    if (d->ntracks < 1 || track_kind(d, 0) != 2) return 0;
    if (img_read(d, d->track[0].start + 16, s) != 0) return 0;
    return s[24] == 1 && !memcmp(s + 25, "CD-I ", 5);
}

static int open_image(disc_t *d, const char *path, char *err, int n)
{
    memset(d, 0, sizeof(*d));
    if (disc_open(d, path) != 0) { snprintf(err, n, "Impossible de lire l'image :\n%s", path); return -1; }
    if (d->chd) {
        disc_close(d);
        snprintf(err, n, "Image CHD : convertissez-la d'abord en BIN/CUE (Fichier > Convertir une image CHD en BIN/CUE), "
                         "puis gravez le .cue obtenu.");
        return -1;
    }
    if (d->sector_size != RAW) {
        disc_close(d);
        snprintf(err, n, "Cette image est en secteurs de 2048 octets (.iso) : seules les images brutes "
                         "(.cue + .bin, secteurs de 2352 octets) se gravent ici.");
        return -1;
    }
    int shift = d->track[0].start;
    if (d->leadout - shift < 300) { disc_close(d); snprintf(err, n, "Image trop petite."); return -1; }
    return 0;
}

int cdburn_describe(const char *image, char *out, int n)
{
    disc_t d;
    if (open_image(&d, image, out, n) != 0) return -1;
    int sectors = d.leadout - d.track[0].start;
    int audio = 0, m1 = 0, m2 = 0;
    for (int i = 0; i < d.ntracks; i++) {
        int k = track_kind(&d, i);
        if (k == 0) audio++; else if (k == 1) m1++; else m2++;
    }
    const char *type = image_is_cdi(&d) ? "disque CD-i" : m2 ? "CD-ROM XA (mode 2)" : m1 ? "CD-ROM (mode 1)" : "CD audio";
    const char *name = strrchr(image, '\\');
    if (!name) name = strrchr(image, '/');
    name = name ? name + 1 : image;
    char tracks[160] = "";
    int k = 0;
    if (m2) k += snprintf(tracks + k, sizeof(tracks) - k, "%d de données (mode 2)", m2);
    if (m1) k += snprintf(tracks + k, sizeof(tracks) - k, "%s%d de données (mode 1)", k ? ", " : "", m1);
    if (audio) snprintf(tracks + k, sizeof(tracks) - k, "%s%d audio", k ? ", " : "", audio);
    int secs = (sectors + 150) / 75;
    snprintf(out, n, "%s\n\nType : %s\nPistes : %d (%s)\nDurée : %d min %02d s (%d Mo)",
             name, type, d.ntracks, tracks, secs / 60, secs % 60,
             (int)((int64_t)sectors * RAW / 1000000));
    disc_close(&d);
    return 0;
}

/* ---------------- burn ---------------- */

typedef struct {
    int lba;                    /* disc LBA (absolute time - 150) */
    uint8_t ctl, tno, idx, form;
} cue_entry_t;

static void put_lba(uint8_t *p, int lba)
{
    p[0] = (uint8_t)(lba >> 24); p[1] = (uint8_t)(lba >> 16);
    p[2] = (uint8_t)(lba >> 8);  p[3] = (uint8_t)lba;
}

/* Wait while the drive reports "not ready, in progress"; returns the
 * last status (0 = ready) */
static int wait_ready(HANDLE h, int max_ms)
{
    static const uint8_t tur[6] = { 0x00 };
    int r = -1;
    for (int t = 0; t <= max_ms; t += 250) {
        r = scsi(h, tur, 6, NULL, 0, SCSI_IOCTL_DATA_UNSPECIFIED, 10);
        if (r == 0) return 0;
        if (!S_BUSY(r) && !(r > 0 && S_KEY(r) == 6)) return r;   /* unit attention: try again */
        Sleep(250);
    }
    return r;
}

/* Generated pregap of track 1 (host-sent): empty mode 2 form 2 sectors
 * (EDC 0 = not computed, allowed for form 2) or digital silence */
static void pregap_sector(uint8_t *s, int lba, int kind)
{
    memset(s, 0, RAW);
    if (kind == 0) return;
    memcpy(s, SYNC, 12);
    disc_lba_to_msf(lba, s + 12);
    s[15] = 2;
    s[18] = s[22] = 0x20;                  /* submode: form 2 */
}

static DWORD WINAPI burn_thread(LPVOID arg)
{
    (void)arg;
    char msg[1600];
    disc_t img;
    HANDLE h = INVALID_HANDLE_VALUE;
    uint8_t *buf = NULL, *small = NULL, *rbuf = NULL;
    int locked = 0, sent_cue = 0;
    const int sim = b_simulate;

    set_stage("Préparation", 0);
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        blog("\n[BURN] %04d-%02d-%02d %02d:%02d:%02d  %s  drive %c:  image %s\n", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, sim ? "SIMULATION (test write)" : "REAL BURN", b_letter, b_image);
    }
    if (open_image(&img, b_image, msg, sizeof(msg)) != 0) { finish(msg); return 0; }

    buf   = VirtualAlloc(NULL, MAX_CHUNK * RAW, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    rbuf  = VirtualAlloc(NULL, MAX_CHUNK * RAW, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    small = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf || !rbuf || !small) { snprintf(msg, sizeof(msg), "Mémoire insuffisante."); goto out; }

    char path[8];
    snprintf(path, sizeof(path), "\\\\.\\%c:", b_letter);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(msg, sizeof(msg), "Impossible d'ouvrir le lecteur %c: en écriture "
                 "(essayez de lancer l'émulateur en administrateur).", b_letter);
        goto out;
    }

    /* largest transfer the adapter takes */
    int chunk = MAX_CHUNK;
    {
        STORAGE_PROPERTY_QUERY q;
        STORAGE_ADAPTER_DESCRIPTOR ad;
        DWORD got = 0;
        memset(&q, 0, sizeof(q));
        q.PropertyId = StorageAdapterProperty;
        q.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &ad, sizeof(ad), &got, NULL)
            && got >= sizeof(ad)) {
            unsigned long max = ad.MaximumTransferLength;
            if (ad.MaximumPhysicalPages > 1 && (ad.MaximumPhysicalPages - 1) * 4096 < max)
                max = (ad.MaximumPhysicalPages - 1) * 4096;
            if (max / RAW < (unsigned long)chunk) chunk = (int)(max / RAW);
            if (chunk < 1) chunk = 1;
        }
    }

    int r = wait_ready(h, 20000);
    if (r != 0) {
        snprintf(msg, sizeof(msg), r > 0 && S_KEY(r) == 2 && S_ASC(r) == 0x3A
                 ? "Pas de disque dans le lecteur %c:. Insérez un CD-R vierge."
                 : "Le lecteur %c: n'est pas prêt.", b_letter);
        goto out;
    }

    /* current profile: CD-R (0009h) or CD-RW (000Ah) */
    {
        uint8_t cdb[10] = { 0x46, 0x00, 0, 0, 0, 0, 0, 0, 8, 0 };
        memset(small, 0, 64);
        r = scsi(h, cdb, 10, small, 8, SCSI_IOCTL_DATA_IN, 10);
        int profile = small[6] << 8 | small[7];
        if (r != 0 || (profile != 0x09 && profile != 0x0A)) {
            snprintf(msg, sizeof(msg), "Le disque du lecteur %c: n'est pas un CD-R ou un CD-RW "
                     "(type %04X). Insérez un CD-R vierge.", b_letter, profile);
            goto out;
        }
    }
    /* CD mastering feature (002Eh): session at once, test write, BUFE */
    int bufe = 1;
    {
        uint8_t cdb[10] = { 0x46, 0x02, 0x00, 0x2E, 0, 0, 0, 0, 16, 0 };
        memset(small, 0, 64);
        r = scsi(h, cdb, 10, small, 16, SCSI_IOCTL_DATA_IN, 10);
        int flags = (small[8] == 0 && small[9] == 0x2E) ? small[12] : 0;
        if (r != 0 || !(flags & 0x20)) {
            snprintf(msg, sizeof(msg), "Le graveur %c: ne sait pas graver en mode « disque en une passe » (SAO).", b_letter);
            goto out;
        }
        if (sim && !(flags & 0x04)) {
            snprintf(msg, sizeof(msg), "Le graveur %c: ne sait pas simuler une gravure.", b_letter);
            goto out;
        }
        bufe = (flags & 0x40) != 0;
    }
    /* blank disc, big enough */
    {
        uint8_t cdb[10] = { 0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0 };
        memset(small, 0, 64);
        r = scsi(h, cdb, 10, small, 34, SCSI_IOCTL_DATA_IN, 10);
        if (r != 0) { snprintf(msg, sizeof(msg), "Lecture des informations du disque impossible (%s).", sense_text(r)); goto out; }
        if ((small[2] & 3) != 0) {
            snprintf(msg, sizeof(msg), "Le disque du lecteur %c: n'est pas vierge.%s", b_letter,
                     (small[2] & 0x10) ? " (CD-RW : effacez-le d'abord avec Windows ou un logiciel de gravure.)" : "");
            goto out;
        }
        int m = small[21], s = small[22], f = small[23];
        int cap = (m * 60 + s) * 75 + f - 150;
        int need = img.leadout - img.track[0].start;
        if (m != 0xFF && cap > 0 && need > cap) {
            snprintf(msg, sizeof(msg), "Le disque est trop petit : l'image dure %d min %02d s, le disque %d min %02d s.",
                     (need + 150) / 75 / 60, (need + 150) / 75 % 60, (cap + 150) / 75 / 60, (cap + 150) / 75 % 60);
            goto out;
        }
    }

    /* tracks on the disc */
    const int shift = img.track[0].start;          /* image LBA - disc LBA */
    const int leadout = img.leadout - shift;
    int kind[DISC_MAX_TRACKS], dstart[DISC_MAX_TRACKS], dpre[DISC_MAX_TRACKS];
    for (int i = 0; i < img.ntracks; i++) {
        kind[i] = track_kind(&img, i);
        dstart[i] = img.track[i].start - shift;
        dpre[i] = i == 0 ? -150 : img.track[i].pregap - shift;
        if (i > 0 && (dpre[i] < dstart[i - 1] + 1 || dstart[i] < dpre[i])) {
            snprintf(msg, sizeof(msg), "Pistes de l'image dans le désordre (piste %d).", img.track[i].number);
            goto out;
        }
    }
    const int cdi = image_is_cdi(&img);
    int any_m2 = 0;
    for (int i = 0; i < img.ntracks; i++) if (kind[i] == 2) any_m2 = 1;
    const int session = cdi ? 0x10 : any_m2 ? 0x20 : 0x00;
    /* mode 1 first track: the drive makes its pregap (no EDC/ECC here) */
    const int first = kind[0] == 1 ? 0 : -150;

    /* write parameters page */
    {
        uint8_t cdb[10] = { 0x5A, 0x08, 0x05, 0, 0, 0, 0, 0, 255, 0 };
        memset(small, 0, 256);
        r = scsi(h, cdb, 10, small, 255, SCSI_IOCTL_DATA_IN, 10);
        int bdl = small[6] << 8 | small[7];
        uint8_t *pg = small + 8 + bdl;
        if (r != 0 || (pg[0] & 0x3F) != 0x05 || pg[1] < 0x32) {
            snprintf(msg, sizeof(msg), "Lecture des paramètres d'écriture impossible (%s).", sense_text(r));
            goto out;
        }
        pg[0] &= 0x3F;
        pg[2] = (uint8_t)((bufe ? 0x40 : 0) | (sim ? 0x10 : 0) | 0x02);   /* SAO */
        pg[3] = (uint8_t)(kind[0] ? 0x04 : 0x00);  /* no multisession, track mode */
        pg[4] = 0x00;                              /* data block: raw 2352 */
        pg[5] = 0;
        pg[7] = 0;
        pg[8] = (uint8_t)session;
        int len = 8 + bdl + 2 + pg[1];
        small[0] = small[1] = 0;
        uint8_t sel[10] = { 0x55, 0x10, 0, 0, 0, 0, 0, (uint8_t)(len >> 8), (uint8_t)len, 0 };
        r = scsi(h, sel, 10, small, len, SCSI_IOCTL_DATA_OUT, 10);
        if (r > 0 && S_KEY(r) == 5) {              /* some drives want a cooked block type */
            pg[4] = kind[0] ? 0x08 : 0x00;
            pg[3] = 0x04;
            r = scsi(h, sel, 10, small, len, SCSI_IOCTL_DATA_OUT, 10);
        }
        if (r != 0) {
            snprintf(msg, sizeof(msg), "Le graveur refuse les paramètres d'écriture (%s, %06X).", sense_text(r), r & 0xFFFFFF);
            goto out;
        }
        /* the drive must have kept the mode asked for: a simulation that
         * the drive turned into a real burn writes the disc for good */
        int b2 = 0, t = page05_test(h, &b2);
        blog("[BURN] write parameters: byte2 %02X, test write %d (asked %d)\n", b2, t, sim);
        if (t != sim) {
            snprintf(msg, sizeof(msg), sim
                ? "Simulation annulée : le graveur %c: n'a pas gardé le mode test (laser éteint). Rien n'a été écrit."
                : "Gravure annulée : le graveur %c: n'a pas pris les paramètres d'écriture. Rien n'a été écrit.", b_letter);
            goto out;
        }
    }
    /* 16x: a speed CD-i players read well, the drive picks the closest */
    {
        uint8_t cdb[12] = { 0xBB, 0, 0xFF, 0xFF, (16 * 176) >> 8, (16 * 176) & 0xFF, 0, 0, 0, 0, 0, 0 };
        scsi(h, cdb, 12, NULL, 0, SCSI_IOCTL_DATA_UNSPECIFIED, 10);
    }

    /* cue sheet; if the drive refuses it, other data forms with the same
     * meaning (lead-in made by the drive, CD-ROM XA raw / mode 2 raw) */
    cue_entry_t cue[2 * DISC_MAX_TRACKS + 2];
    int ncue = 0;
    static const uint8_t form_m2[3] = { 0x21, 0x21, 0x31 };
    static const uint8_t form_in[3] = { 0x01, 0x24, 0x01 };
    for (int attempt = 0; attempt < 3 && !sent_cue; attempt++) {
        ncue = 0;
        uint8_t ctl0 = kind[0] ? 0x41 : 0x01;
        uint8_t lin = kind[0] == 0 ? 0x01 : kind[0] == 1 ? (attempt == 1 ? 0x14 : 0x01) : form_in[attempt];
        cue[ncue++] = (cue_entry_t){ -150, ctl0, 0, 0, lin };
        for (int i = 0; i < img.ntracks; i++) {
            uint8_t ctl = kind[i] ? 0x41 : 0x01;
            uint8_t form = kind[i] == 0 ? 0x00 : kind[i] == 1 ? 0x11 : form_m2[attempt];
            uint8_t tno = (uint8_t)(i + 1);
            if (i == 0)
                cue[ncue++] = (cue_entry_t){ -150, ctl, tno, 0, kind[0] == 1 ? 0x14 : form };
            else if (dpre[i] < dstart[i])
                cue[ncue++] = (cue_entry_t){ dpre[i], ctl, tno, 0, form };
            cue[ncue++] = (cue_entry_t){ dstart[i], ctl, tno, 1, form };
        }
        uint8_t ctln = kind[img.ntracks - 1] ? 0x41 : 0x01;
        cue[ncue++] = (cue_entry_t){ leadout, ctln, 0xAA, 1, 0x01 };

        for (int i = 0; i < ncue; i++) {
            uint8_t *e = small + i * 8;
            int a = cue[i].lba + 150;
            e[0] = cue[i].ctl; e[1] = cue[i].tno; e[2] = cue[i].idx; e[3] = cue[i].form;
            e[4] = 0;
            e[5] = (uint8_t)(a / 4500); e[6] = (uint8_t)(a / 75 % 60); e[7] = (uint8_t)(a % 75);
            blog("[BURN]   %02X %02X %02X %02X 00 %02d:%02d:%02d\n", e[0], e[1], e[2], e[3], e[5], e[6], e[7]);
        }
        int len = ncue * 8;
        uint8_t cdb[10] = { 0x5D, 0, 0, 0, 0, 0, (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, 0 };
        r = scsi(h, cdb, 10, small, len, SCSI_IOCTL_DATA_OUT, 30);
        blog("[BURN] cue sheet (try %d, %d entries): %s %06X\n", attempt + 1, ncue,
                r == 0 ? "accepted" : "refused", r > 0 ? r & 0xFFFFFF : 0);
        if (r == 0) sent_cue = 1;
        else if (!(r > 0 && S_KEY(r) == 5)) break;
    }
    if (!sent_cue) {
        snprintf(msg, sizeof(msg), "Le graveur refuse la table des pistes (%s, %06X).", sense_text(r), r > 0 ? r & 0xFFFFFF : 0);
        goto out;
    }

    {   /* keep the tray closed while writing */
        uint8_t cdb[6] = { 0x1E, 0, 0, 0, 1, 0 };
        locked = scsi(h, cdb, 6, NULL, 0, SCSI_IOCTL_DATA_UNSPECIFIED, 10) == 0;
    }

    /* data: from the first host-sent sector to the lead-out, one WRITE
     * never crossing a cue sheet entry */
    {   /* last check before the first WRITE: still the mode asked for */
        int b2 = 0, t = page05_test(h, &b2);
        if (t != sim) {
            blog("[BURN] test write bit changed before writing: byte2 %02X\n", b2);
            snprintf(msg, sizeof(msg), "%s annulée : le graveur %c: a changé de mode d'écriture avant de commencer. Rien n'a été écrit.",
                     sim ? "Simulation" : "Gravure", b_letter);
            goto out;
        }
    }
    set_stage(sim ? "Simulation" : "Gravure", 0);
    blog("[BURN] %c: %s, %d tracks, LBA %d..%d, session %02X, %d sectors per write\n",
            b_letter, sim ? "test write" : "write", img.ntracks, first, leadout, session, chunk);
    for (int lba = first; lba < leadout; ) {
        int n = leadout - lba < chunk ? leadout - lba : chunk;
        for (int i = 0; i < ncue; i++)
            if (cue[i].lba > lba && cue[i].lba < lba + n) n = cue[i].lba - lba;
        for (int i = 0; i < n; i++) {
            int a = lba + i;
            if (a < 0) pregap_sector(buf + i * RAW, a, kind[0]);
            else       img_read(&img, a + shift, buf + i * RAW);
        }
        uint8_t cdb[10] = { 0x2A, 0, 0, 0, 0, 0, 0, (uint8_t)(n >> 8), (uint8_t)n, 0 };
        put_lba(cdb + 2, lba);
        DWORD t0 = GetTickCount();
        for (;;) {
            r = scsi(h, cdb, 10, buf, n * RAW, SCSI_IOCTL_DATA_OUT, 120);
            if (!S_BUSY(r) || GetTickCount() - t0 > 120000) break;
            Sleep(10);                             /* drive buffer full */
        }
        if (r != 0) {
            snprintf(msg, sizeof(msg), "%s interrompue au secteur %d sur %d : %s (%06X).%s",
                     sim ? "Simulation" : "Gravure", lba, leadout, sense_text(r), r > 0 ? r & 0xFFFFFF : 0,
                     sim ? "" : "\nLe disque est probablement inutilisable.");
            goto out;
        }
        lba += n;
        InterlockedExchange(&st_progress, (LONG)((int64_t)(lba - first) * 100 / (leadout - first)));
    }

    /* lead-out, disc closed */
    set_stage("Finalisation", 100);
    {
        uint8_t cdb[10] = { 0x35, 0x02, 0, 0, 0, 0, 0, 0, 0, 0 };   /* IMMED */
        r = scsi(h, cdb, 10, NULL, 0, SCSI_IOCTL_DATA_UNSPECIFIED, 900);
        if (r == 0 || S_BUSY(r)) r = wait_ready(h, 15 * 60 * 1000);
        if (r != 0) {
            snprintf(msg, sizeof(msg), "Finalisation du disque en échec : %s (%06X).", sense_text(r), r > 0 ? r & 0xFFFFFF : 0);
            goto out;
        }
    }
    if (locked) {
        uint8_t cdb[6] = { 0x1E, 0, 0, 0, 0, 0 };
        scsi(h, cdb, 6, NULL, 0, SCSI_IOCTL_DATA_UNSPECIFIED, 10);
        locked = 0;
    }
    if (sim) {
        /* the disc must still be blank; if not, the drive burned for real */
        uint8_t cdb[10] = { 0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0 };
        memset(small, 0, 64);
        wait_ready(h, 60000);
        r = scsi(h, cdb, 10, small, 34, SCSI_IOCTL_DATA_IN, 10);
        blog("[BURN] after simulation: disc status %d\n", r == 0 ? small[2] & 3 : -1);
        if (r == 0 && (small[2] & 3) != 0) {
            snprintf(msg, sizeof(msg), "ATTENTION : le graveur %c: a réellement gravé le disque pendant la "
                     "simulation (il ignore le mode test). N'utilisez pas la simulation avec ce graveur.", b_letter);
            goto out;
        }
        snprintf(msg, sizeof(msg), "Simulation réussie : le graveur %c: a accepté toute l'image "
                 "(%d pistes, %d secteurs), laser éteint.\nLe disque est resté vierge : "
                 "la vraie gravure peut être lancée.", b_letter, img.ntracks, leadout);
        goto out;
    }

    /* read the data tracks back and compare with the image */
    set_stage("Vérification", 0);
    wait_ready(h, 60000);
    int total = 0, checked = 0, bad = 0, unread = 0;
    for (int i = 0; i < img.ntracks; i++)
        if (kind[i]) total += (i + 1 < img.ntracks ? dpre[i + 1] : leadout) - dstart[i];
    for (int i = 0; i < img.ntracks && total > 0; i++) {
        if (!kind[i]) continue;
        int end = i + 1 < img.ntracks ? dpre[i + 1] : leadout;
        for (int lba = dstart[i]; lba < end; ) {
            int n = end - lba < chunk ? end - lba : chunk;
            uint8_t cdb[12] = { 0xBE, 0, 0, 0, 0, 0, (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, 0xF8, 0, 0 };
            put_lba(cdb + 2, lba);
            r = -1;
            for (int tries = 0; tries < 3 && r != 0; tries++)
                r = scsi(h, cdb, 12, rbuf, n * RAW, SCSI_IOCTL_DATA_IN, 60);
            for (int k = 0; k < n; k++) {
                img_read(&img, lba + k + shift, buf);
                if (r != 0) unread++;
                else if (memcmp(buf, rbuf + k * RAW, RAW)) bad++;
            }
            lba += n;
            checked += n;
            InterlockedExchange(&st_progress, (LONG)((int64_t)checked * 100 / total));
        }
    }
    {   /* eject: the disc is ready */
        uint8_t cdb[6] = { 0x1B, 0, 0, 0, 0x02, 0 };
        scsi(h, cdb, 6, NULL, 0, SCSI_IOCTL_DATA_UNSPECIFIED, 30);
    }
    if (bad == 0 && unread == 0)
        snprintf(msg, sizeof(msg), "Gravure terminée et vérifiée : %d pistes, %d secteurs de données "
                 "identiques à l'image.%s", img.ntracks, checked, cdi ? "\nDisque CD-i prêt." : "");
    else
        snprintf(msg, sizeof(msg), "Gravure terminée, mais la vérification trouve %d secteurs différents "
                 "et %d illisibles sur %d.", bad, unread, checked);

out:
    if (locked) {
        uint8_t cdb[6] = { 0x1E, 0, 0, 0, 0, 0 };
        scsi(h, cdb, 6, NULL, 0, SCSI_IOCTL_DATA_UNSPECIFIED, 10);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    if (rbuf) VirtualFree(rbuf, 0, MEM_RELEASE);
    if (small) VirtualFree(small, 0, MEM_RELEASE);
    disc_close(&img);
    blog("[BURN] %s\n", msg);
    finish(msg);
    return 0;
}

int cdburn_start(char letter, const char *image, int simulate)
{
    init_lock();
    if (st_progress >= 0 && !st_done) return -1;          /* already running */
    b_letter = letter;
    b_simulate = simulate;
    snprintf(b_image, sizeof(b_image), "%s", image);
    InterlockedExchange(&st_done, 0);
    set_stage("Préparation", 0);
    HANDLE th = CreateThread(NULL, 0, burn_thread, NULL, 0, NULL);
    if (!th) { InterlockedExchange(&st_progress, -1); return -1; }
    CloseHandle(th);
    return 0;
}

int cdburn_progress(int *done, char *stage, int sn, char *msg, int n)
{
    init_lock();
    *done = st_done;
    EnterCriticalSection(&lock);
    if (stage) snprintf(stage, sn, "%s", st_stage);
    if (st_done && msg) snprintf(msg, n, "%s", st_msg);
    LeaveCriticalSection(&lock);
    return st_progress;
}

#else  /* not Windows */

int cdburn_describe(const char *image, char *out, int n)
{
    (void)image;
    snprintf(out, n, "La gravure n'existe que dans la version Windows.");
    return -1;
}
int cdburn_start(char l, const char *i, int s) { (void)l; (void)i; (void)s; return -1; }
int cdburn_progress(int *d, char *st, int sn, char *m, int n) { (void)st; (void)sn; (void)m; (void)n; *d = 0; return -1; }

#endif
