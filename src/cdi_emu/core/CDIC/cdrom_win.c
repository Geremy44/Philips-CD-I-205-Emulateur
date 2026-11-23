/*
 * cdrom_win.c - physical CD drive access (see cdrom_win.h)
 */

#include "cdrom_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>

#define RAW 2352

struct cdrom {
    HANDLE h;
    int    spti;                     /* SCSI pass-through usable */
};

int cdrom_list_drives(char *letters, int max)
{
    int n = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && n < max; i++) {
        if (!(mask & (1u << i))) continue;
        char root[4] = { (char)('A' + i), ':', '\\', 0 };
        if (GetDriveTypeA(root) == DRIVE_CDROM) letters[n++] = (char)('A' + i);
    }
    return n;
}

cdrom_t *cdrom_open(char letter)
{
    char path[8];
    snprintf(path, sizeof(path), "\\\\.\\%c:", letter);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    int spti = 1;
    if (h == INVALID_HANDLE_VALUE) {             /* read only: no SCSI pass-through */
        h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        spti = 0;
    }
    if (h == INVALID_HANDLE_VALUE) return NULL;
    cdrom_t *c = calloc(1, sizeof(*c));
    if (!c) { CloseHandle(h); return NULL; }
    c->h = h;
    c->spti = spti;
    return c;
}

void cdrom_close(cdrom_t *c)
{
    if (!c) return;
    CloseHandle(c->h);
    free(c);
}

/* Full TOC (READ TOC format 2, the raw Q-channel points of the lead-in).
 * Needed for CD-i discs written to the Green Book: the TOC holds only the
 * A0 / A1 / A2 points (disc type 10h, lead-out) and no entry for the CD-i
 * track, so the normal TOC request fails ("no track"). The CD-i track then
 * starts at LBA 0 and runs to the lead-out. */
static int read_full_toc(cdrom_t *c, cdrom_toc_t *toc)
{
    if (!c->spti) return -1;
    static uint8_t buf[4 + 11 * 110];
    struct { SCSI_PASS_THROUGH_DIRECT s; UCHAR sense[32]; } p;
    memset(&p, 0, sizeof(p));
    memset(buf, 0, sizeof(buf));
    p.s.Length = sizeof(p.s);
    p.s.CdbLength = 10;
    p.s.SenseInfoLength = sizeof(p.sense);
    p.s.SenseInfoOffset = (ULONG)((char *)p.sense - (char *)&p);
    p.s.DataIn = SCSI_IOCTL_DATA_IN;
    p.s.DataTransferLength = sizeof(buf);
    p.s.TimeOutValue = 30;
    p.s.DataBuffer = buf;
    p.s.Cdb[0] = 0x43;
    p.s.Cdb[1] = 0x02;                           /* MSF */
    p.s.Cdb[2] = 0x02;                           /* format 2: full TOC */
    p.s.Cdb[7] = (UCHAR)(sizeof(buf) >> 8);
    p.s.Cdb[8] = (UCHAR)sizeof(buf);
    DWORD got = 0;
    if (!DeviceIoControl(c->h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &p, sizeof(p), &p, sizeof(p), &got, NULL)
        || p.s.ScsiStatus != 0)
        return -1;
    int len = (buf[0] << 8 | buf[1]) + 2;
    if (len > (int)sizeof(buf)) len = sizeof(buf);
    int n = 0, leadout = -1, cdi = 0;
    memset(toc, 0, sizeof(*toc));
    for (int o = 4; o + 11 <= len; o += 11) {
        const uint8_t *e = buf + o;
        if (e[0] != 1) continue;                 /* first session only */
        int point = e[3], lba = (e[8] * 60 + e[9]) * 75 + e[10] - 150;
        if (point == 0xA0 && e[9] == 0x10) cdi = 1;
        else if (point == 0xA2) leadout = lba;
        else if (point >= 1 && point <= 99 && n < CDROM_MAX_TRACKS) {
            toc->number[n] = point;
            toc->start[n] = lba < 0 ? 0 : lba;
            toc->audio[n] = !(e[1] & 4);
            n++;
        }
    }
    if (leadout <= 0) return -1;
    if (n == 0) {                                /* Green Book CD-i: hidden track */
        toc->number[0] = 1;
        toc->start[0] = 0;
        toc->audio[0] = 0;
        n = 1;
        fprintf(stderr, "[CDROM] no track in the TOC%s: one data track 0..%d\n",
                cdi ? " (CD-i disc)" : "", leadout);
    }
    toc->ntracks = n;
    toc->leadout = leadout;
    return 0;
}

int cdrom_read_toc(cdrom_t *c, cdrom_toc_t *toc)
{
    CDROM_TOC t;
    DWORD got = 0;
    memset(toc, 0, sizeof(*toc));
    if (!DeviceIoControl(c->h, IOCTL_CDROM_READ_TOC, NULL, 0, &t, sizeof(t), &got, NULL))
        return read_full_toc(c, toc);
    int n = t.LastTrack - t.FirstTrack + 1;
    if (n < 1 || n > CDROM_MAX_TRACKS) return read_full_toc(c, toc);
    for (int i = 0; i <= n; i++) {
        const TRACK_DATA *d = &t.TrackData[i];
        int lba = (d->Address[1] * 60 + d->Address[2]) * 75 + d->Address[3] - 150;
        if (i == n) { toc->leadout = lba; break; }
        toc->number[i] = d->TrackNumber;
        toc->start[i] = lba < 0 ? 0 : lba;
        toc->audio[i] = !(d->Control & 4);
    }
    toc->ntracks = n;
    return 0;
}

/* SCSI READ CD ($BE): any sector type, sync + header + subheader + user
 * data + EDC/ECC = the 2352 raw bytes, CD-DA included */
static int read_cd_spti(cdrom_t *c, int lba, int count, uint8_t *out)
{
    struct { SCSI_PASS_THROUGH_DIRECT s; UCHAR sense[32]; } p;
    memset(&p, 0, sizeof(p));
    p.s.Length = sizeof(p.s);
    p.s.CdbLength = 12;
    p.s.SenseInfoLength = sizeof(p.sense);
    p.s.SenseInfoOffset = (ULONG)((char *)p.sense - (char *)&p);
    p.s.DataIn = SCSI_IOCTL_DATA_IN;
    p.s.DataTransferLength = (ULONG)count * RAW;
    p.s.TimeOutValue = 30;
    p.s.DataBuffer = out;
    p.s.Cdb[0] = 0xBE;
    p.s.Cdb[2] = (UCHAR)(lba >> 24); p.s.Cdb[3] = (UCHAR)(lba >> 16);
    p.s.Cdb[4] = (UCHAR)(lba >> 8);  p.s.Cdb[5] = (UCHAR)lba;
    p.s.Cdb[6] = (UCHAR)(count >> 16); p.s.Cdb[7] = (UCHAR)(count >> 8); p.s.Cdb[8] = (UCHAR)count;
    p.s.Cdb[9] = 0xF8;
    DWORD got = 0;
    if (!DeviceIoControl(c->h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &p, sizeof(p), &p, sizeof(p), &got, NULL))
        return -1;
    return p.s.ScsiStatus == 0 ? 0 : -1;
}

static int read_cd_ioctl(cdrom_t *c, int lba, int count, int audio, uint8_t *out)
{
    RAW_READ_INFO r;
    r.DiskOffset.QuadPart = (LONGLONG)lba * 2048;    /* this IOCTL counts in 2048-byte units */
    r.SectorCount = (ULONG)count;
    r.TrackMode = audio ? CDDA : XAForm2;
    DWORD got = 0;
    if (!DeviceIoControl(c->h, IOCTL_CDROM_RAW_READ, &r, sizeof(r), out, (DWORD)count * RAW, &got, NULL))
        return -1;
    return got == (DWORD)count * RAW ? 0 : -1;
}

int cdrom_read_raw(cdrom_t *c, int lba, int count, int audio, uint8_t *out)
{
    if (c->spti && read_cd_spti(c, lba, count, out) == 0) return 0;
    if (read_cd_ioctl(c, lba, count, audio, out) == 0) return 0;
    return -1;
}

static void clean_name(const uint8_t *src, int len, char *out, int n)
{
    int k = 0;
    for (int i = 0; i < len && k < n - 1; i++) {
        int ch = src[i];
        if (isalnum(ch) || ch == '-' || ch == '_') out[k++] = (char)ch;
        else if (ch == ' ' || ch == '.') out[k++] = '_';
    }
    while (k > 0 && out[k - 1] == '_') k--;      /* trailing padding */
    out[k] = 0;
}

void cdrom_disc_name(cdrom_t *c, const cdrom_toc_t *toc, char *out, int n)
{
    snprintf(out, n, "%s", (toc->ntracks > 0 && toc->audio[0]) ? "CD_audio" : "Disque");
    if (toc->ntracks <= 0 || toc->audio[0]) return;
    uint8_t s[RAW];
    if (cdrom_read_raw(c, toc->start[0] + 16, 1, 0, s) != 0) return;
    const uint8_t *d = (s[15] == 2) ? s + 24 : s + 16;   /* mode 2: after the subheader */
    if ((!memcmp(d + 1, "CD-I ", 5) || !memcmp(d + 1, "CD001", 5)) && d[0] == 1) {
        char name[64];
        clean_name(d + 40, 32, name, sizeof(name));
        if (name[0]) snprintf(out, n, "%s", name);
    }
}

/* ---------------- ripping ---------------- */

static CRITICAL_SECTION rip_lock;
static volatile LONG rip_state = -1;     /* -1 idle, 0..100 progress */
static volatile LONG rip_done = 0;
static char rip_msg[1200];
static char rip_letter;
static char rip_dir[1100];

static FILE *fopen_utf8(const char *path, const wchar_t *mode)
{
    wchar_t w[1100];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100) <= 0) return NULL;
    return _wfopen(w, mode);
}

static int mkdir_utf8(const char *path)
{
    wchar_t w[1100];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100) <= 0) return -1;
    if (CreateDirectoryW(w, NULL)) return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 1 : -1;
}

static void rip_finish(const char *msg)
{
    EnterCriticalSection(&rip_lock);
    snprintf(rip_msg, sizeof(rip_msg), "%s", msg);
    LeaveCriticalSection(&rip_lock);
    InterlockedExchange(&rip_done, 1);
}

static DWORD WINAPI rip_thread(LPVOID arg)
{
    (void)arg;
    char msg[1200];
    cdrom_t *c = cdrom_open(rip_letter);
    if (!c) { snprintf(msg, sizeof(msg), "Impossible d'ouvrir le lecteur %c:", rip_letter); rip_finish(msg); return 0; }
    cdrom_toc_t toc;
    if (cdrom_read_toc(c, &toc) != 0) {
        cdrom_close(c);
        snprintf(msg, sizeof(msg), "Pas de disque lisible dans le lecteur %c:", rip_letter);
        rip_finish(msg);
        return 0;
    }
    char name[64], folder[1200], bin[1300], cue[1300];
    cdrom_disc_name(c, &toc, name, sizeof(name));
    mkdir_utf8(rip_dir);
    /* games_dir/<name>, <name>_2, ... (never overwrite) */
    for (int k = 1; k < 100; k++) {
        if (k == 1) snprintf(folder, sizeof(folder), "%s\\%s", rip_dir, name);
        else        snprintf(folder, sizeof(folder), "%s\\%s_%d", rip_dir, name, k);
        if (mkdir_utf8(folder) == 0) break;
    }
    const char *base = strrchr(folder, '\\') + 1;
    snprintf(bin, sizeof(bin), "%s\\%s.bin", folder, base);
    snprintf(cue, sizeof(cue), "%s\\%s.cue", folder, base);

    FILE *fb = fopen_utf8(bin, L"wb");
    if (!fb) { cdrom_close(c); snprintf(msg, sizeof(msg), "Impossible de cr\xC3\xA9" "er %s", bin); rip_finish(msg); return 0; }
    enum { CHUNK = 24 };
    static uint8_t buf[CHUNK * RAW];
    int total = toc.leadout, errors = 0;
    for (int lba = 0; lba < total; ) {
        int t = 0;
        for (int i = 0; i < toc.ntracks; i++) if (lba >= toc.start[i]) t = i;
        int n = total - lba < CHUNK ? total - lba : CHUNK;
        if (t + 1 < toc.ntracks && lba + n > toc.start[t + 1]) n = toc.start[t + 1] - lba;  /* one track per read */
        if (cdrom_read_raw(c, lba, n, toc.audio[t], buf) != 0) {
            for (int i = 0; i < n; i++) {         /* sector by sector, 3 tries */
                int ok = 0;
                for (int r = 0; r < 3 && !ok; r++) ok = cdrom_read_raw(c, lba + i, 1, toc.audio[t], buf + i * RAW) == 0;
                if (!ok) { memset(buf + i * RAW, 0, RAW); errors++; }
            }
        }
        fwrite(buf, RAW, (size_t)n, fb);
        lba += n;
        InterlockedExchange(&rip_state, (LONG)((int64_t)lba * 100 / total));
    }
    fclose(fb);
    cdrom_close(c);

    FILE *fc = fopen_utf8(cue, L"w");
    if (fc) {
        fprintf(fc, "FILE \"%s.bin\" BINARY\n", base);
        for (int i = 0; i < toc.ntracks; i++) {
            int a = toc.start[i];
            fprintf(fc, "  TRACK %02d %s\n    INDEX 01 %02d:%02d:%02d\n", toc.number[i],
                    toc.audio[i] ? "AUDIO" : "MODE2/2352", a / 4500, (a / 75) % 60, a % 75);
        }
        fclose(fc);
    }
    if (errors) snprintf(msg, sizeof(msg), "%s (%d secteurs illisibles, remplis de z\xC3\xA9ros)", cue, errors);
    else        snprintf(msg, sizeof(msg), "%s", cue);
    rip_finish(msg);
    return 0;
}

int cdrom_rip_start(char letter, const char *games_dir)
{
    static int init = 0;
    if (!init) { InitializeCriticalSection(&rip_lock); init = 1; }
    if (rip_state >= 0 && !rip_done) return -1;           /* already running */
    rip_letter = letter;
    snprintf(rip_dir, sizeof(rip_dir), "%s", games_dir);
    InterlockedExchange(&rip_done, 0);
    InterlockedExchange(&rip_state, 0);
    HANDLE th = CreateThread(NULL, 0, rip_thread, NULL, 0, NULL);
    if (!th) { InterlockedExchange(&rip_state, -1); return -1; }
    CloseHandle(th);
    return 0;
}

int cdrom_rip_progress(int *done, char *msg, int n)
{
    static int init = 0;
    if (!init) { InitializeCriticalSection(&rip_lock); init = 1; }
    *done = rip_done;
    if (rip_done && msg) {
        EnterCriticalSection(&rip_lock);
        snprintf(msg, n, "%s", rip_msg);
        LeaveCriticalSection(&rip_lock);
    }
    return rip_state;
}

#else  /* not Windows: no drive access */

int      cdrom_list_drives(char *l, int m) { (void)l; (void)m; return 0; }
cdrom_t *cdrom_open(char l) { (void)l; return NULL; }
void     cdrom_close(cdrom_t *c) { (void)c; }
int      cdrom_read_toc(cdrom_t *c, cdrom_toc_t *t) { (void)c; (void)t; return -1; }
int      cdrom_read_raw(cdrom_t *c, int a, int n, int au, uint8_t *o) { (void)c; (void)a; (void)n; (void)au; (void)o; return -1; }
void     cdrom_disc_name(cdrom_t *c, const cdrom_toc_t *t, char *o, int n) { (void)c; (void)t; snprintf(o, n, "Disque"); }
int      cdrom_rip_start(char l, const char *d) { (void)l; (void)d; return -1; }
int      cdrom_rip_progress(int *d, char *m, int n) { (void)m; (void)n; *d = 0; return -1; }

#endif
