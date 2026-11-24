/*
 * mpeg1_video.c - MPEG-1 video decoder (see mpeg1_video.h)
 *
 * Written for this emulator from ISO/IEC 11172-2: syntax of the sequence,
 * picture, slice, macroblock and block layers, the variable length code
 * tables of annex B, dequantisation (2.4.4), motion vector reconstruction
 * (2.4.4.2) and half-pel prediction. The inverse DCT is a plain separable
 * transform with 13-bit integer cosines.
 */

#include "mpeg1_video.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ======================= variable length codes ======================= */

typedef struct { int16_t val; uint8_t len; } vlc_t;
typedef struct { const char *code; int val; } vlc_src_t;

/* lookup table indexed by the next `bits` bits of the stream */
static void vlc_build(vlc_t *t, int bits, const vlc_src_t *src, int n)
{
    memset(t, 0, sizeof(vlc_t) << bits);
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(src[i].code);
        unsigned v = 0;
        for (int k = 0; k < len; k++) v = (v << 1) | (unsigned)(src[i].code[k] == '1');
        unsigned first = v << (bits - len), count = 1u << (bits - len);
        for (unsigned k = 0; k < count; k++) { t[first + k].val = (int16_t)src[i].val; t[first + k].len = (uint8_t)len; }
    }
}

#define MBAI_STUFF 34
#define MBAI_ESC   35
/* Table B.1: macroblock_address_increment */
static const vlc_src_t MBAI[] = {
    {"1",1},{"011",2},{"010",3},{"0011",4},{"0010",5},{"00011",6},{"00010",7},
    {"0000111",8},{"0000110",9},{"00001011",10},{"00001010",11},{"00001001",12},
    {"00001000",13},{"00000111",14},{"00000110",15},{"0000010111",16},{"0000010110",17},
    {"0000010101",18},{"0000010100",19},{"0000010011",20},{"0000010010",21},
    {"00000100011",22},{"00000100010",23},{"00000100001",24},{"00000100000",25},
    {"00000011111",26},{"00000011110",27},{"00000011101",28},{"00000011100",29},
    {"00000011011",30},{"00000011010",31},{"00000011001",32},{"00000011000",33},
    {"00000001111",MBAI_STUFF},{"00000001000",MBAI_ESC},
};

/* macroblock_type flags */
#define MB_QUANT   0x10
#define MB_FWD     0x08
#define MB_BWD     0x04
#define MB_PATTERN 0x02
#define MB_INTRA   0x01
/* Table B.2: macroblock_type for I, P and B pictures */
static const vlc_src_t MBT_I[] = { {"1",MB_INTRA},{"01",MB_INTRA|MB_QUANT} };
static const vlc_src_t MBT_P[] = {
    {"1",MB_FWD|MB_PATTERN},{"01",MB_PATTERN},{"001",MB_FWD},{"00011",MB_INTRA},
    {"00010",MB_QUANT|MB_FWD|MB_PATTERN},{"00001",MB_QUANT|MB_PATTERN},{"000001",MB_QUANT|MB_INTRA},
};
static const vlc_src_t MBT_B[] = {
    {"10",MB_FWD|MB_BWD},{"11",MB_FWD|MB_BWD|MB_PATTERN},{"010",MB_BWD},{"011",MB_BWD|MB_PATTERN},
    {"0010",MB_FWD},{"0011",MB_FWD|MB_PATTERN},{"00011",MB_INTRA},
    {"00010",MB_QUANT|MB_FWD|MB_BWD|MB_PATTERN},{"000011",MB_QUANT|MB_FWD|MB_PATTERN},
    {"000010",MB_QUANT|MB_BWD|MB_PATTERN},{"000001",MB_QUANT|MB_INTRA},
};

/* Table B.3: coded_block_pattern */
static const vlc_src_t CBP[] = {
    {"111",60},{"1101",4},{"1100",8},{"1011",16},{"1010",32},{"10011",12},{"10010",48},
    {"10001",20},{"10000",40},{"01111",28},{"01110",44},{"01101",52},{"01100",56},
    {"01011",1},{"01010",61},{"01001",2},{"01000",62},{"001111",24},{"001110",36},
    {"001101",3},{"001100",63},{"0010111",5},{"0010110",9},{"0010101",17},{"0010100",33},
    {"0010011",6},{"0010010",10},{"0010001",18},{"0010000",34},{"00011111",7},
    {"00011110",11},{"00011101",19},{"00011100",35},{"00011011",13},{"00011010",49},
    {"00011001",21},{"00011000",41},{"00010111",14},{"00010110",50},{"00010101",22},
    {"00010100",42},{"00010011",15},{"00010010",51},{"00010001",23},{"00010000",43},
    {"00001111",25},{"00001110",37},{"00001101",26},{"00001100",38},{"00001011",29},
    {"00001010",45},{"00001001",53},{"00001000",57},{"00000111",30},{"00000110",46},
    {"00000101",54},{"00000100",58},{"000000111",31},{"000000110",47},{"000000101",55},
    {"000000100",59},{"000000011",27},{"000000010",39},
};

/* Table B.4: motion_code, magnitude part (a sign bit follows, 1 = negative) */
static const char *const MOTION_MAG[17] = {
    "1","01","001","0001","000011","0000101","0000100","0000011","000001011",
    "000001010","000001001","0000010001","0000010000","0000001111","0000001110",
    "0000001101","0000001100",
};

/* Table B.5: dct_dc_size_luminance / chrominance */
static const vlc_src_t DC_LUM[] = {
    {"100",0},{"00",1},{"01",2},{"101",3},{"110",4},{"1110",5},{"11110",6},{"111110",7},{"1111110",8},
};
static const vlc_src_t DC_CHR[] = {
    {"00",0},{"01",1},{"10",2},{"110",3},{"1110",4},{"11110",5},{"111110",6},{"1111110",7},{"11111110",8},
};

/* Table B.5c: dct_coeff_next (run, level), sign bit not included. The
 * first coefficient of a non-intra block uses "1" for (0,1) instead. */
#define DCT_EOB -1
#define DCT_ESC -2
#define RL(r, l) ((r) << 8 | (l))
static const vlc_src_t DCT[] = {
    {"10",DCT_EOB},{"000001",DCT_ESC},{"11",RL(0,1)},
    {"011",RL(1,1)},{"0100",RL(0,2)},{"0101",RL(2,1)},{"00101",RL(0,3)},{"00111",RL(3,1)},
    {"00110",RL(4,1)},{"000110",RL(1,2)},{"000111",RL(5,1)},{"000101",RL(6,1)},{"000100",RL(7,1)},
    {"0000110",RL(0,4)},{"0000100",RL(2,2)},{"0000111",RL(8,1)},{"0000101",RL(9,1)},
    {"00100110",RL(0,5)},{"00100001",RL(0,6)},{"00100101",RL(1,3)},{"00100100",RL(3,2)},
    {"00100111",RL(10,1)},{"00100011",RL(11,1)},{"00100010",RL(12,1)},{"00100000",RL(13,1)},
    {"0000001010",RL(0,7)},{"0000001100",RL(1,4)},{"0000001011",RL(2,3)},{"0000001111",RL(4,2)},
    {"0000001001",RL(5,2)},{"0000001110",RL(14,1)},{"0000001101",RL(15,1)},{"0000001000",RL(16,1)},
    {"000000011101",RL(0,8)},{"000000011000",RL(0,9)},{"000000010011",RL(0,10)},{"000000010000",RL(0,11)},
    {"000000011011",RL(1,5)},{"000000010100",RL(2,4)},{"000000011100",RL(3,3)},{"000000010010",RL(4,3)},
    {"000000011110",RL(6,2)},{"000000010101",RL(7,2)},{"000000010001",RL(8,2)},{"000000011111",RL(17,1)},
    {"000000011010",RL(18,1)},{"000000011001",RL(19,1)},{"000000010111",RL(20,1)},{"000000010110",RL(21,1)},
    {"0000000011010",RL(0,12)},{"0000000011001",RL(0,13)},{"0000000011000",RL(0,14)},{"0000000010111",RL(0,15)},
    {"0000000010110",RL(1,6)},{"0000000010101",RL(1,7)},{"0000000010100",RL(2,5)},{"0000000010011",RL(3,4)},
    {"0000000010010",RL(5,3)},{"0000000010001",RL(9,2)},{"0000000010000",RL(10,2)},{"0000000011111",RL(22,1)},
    {"0000000011110",RL(23,1)},{"0000000011101",RL(24,1)},{"0000000011100",RL(25,1)},{"0000000011011",RL(26,1)},
    {"00000000011111",RL(0,16)},{"00000000011110",RL(0,17)},{"00000000011101",RL(0,18)},{"00000000011100",RL(0,19)},
    {"00000000011011",RL(0,20)},{"00000000011010",RL(0,21)},{"00000000011001",RL(0,22)},{"00000000011000",RL(0,23)},
    {"00000000010111",RL(0,24)},{"00000000010110",RL(0,25)},{"00000000010101",RL(0,26)},{"00000000010100",RL(0,27)},
    {"00000000010011",RL(0,28)},{"00000000010010",RL(0,29)},{"00000000010001",RL(0,30)},{"00000000010000",RL(0,31)},
    {"000000000011000",RL(0,32)},{"000000000010111",RL(0,33)},{"000000000010110",RL(0,34)},{"000000000010101",RL(0,35)},
    {"000000000010100",RL(0,36)},{"000000000010011",RL(0,37)},{"000000000010010",RL(0,38)},{"000000000010001",RL(0,39)},
    {"000000000010000",RL(0,40)},{"000000000011111",RL(1,8)},{"000000000011110",RL(1,9)},{"000000000011101",RL(1,10)},
    {"000000000011100",RL(1,11)},{"000000000011011",RL(1,12)},{"000000000011010",RL(1,13)},{"000000000011001",RL(1,14)},
    {"0000000000010011",RL(1,15)},{"0000000000010010",RL(1,16)},{"0000000000010001",RL(1,17)},{"0000000000010000",RL(1,18)},
    {"0000000000010100",RL(6,3)},{"0000000000011010",RL(11,2)},{"0000000000011001",RL(12,2)},{"0000000000011000",RL(13,2)},
    {"0000000000010111",RL(14,2)},{"0000000000010110",RL(15,2)},{"0000000000010101",RL(16,2)},{"0000000000011111",RL(27,1)},
    {"0000000000011110",RL(28,1)},{"0000000000011101",RL(29,1)},{"0000000000011100",RL(30,1)},{"0000000000011011",RL(31,1)},
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static vlc_t t_mbai[1 << 11], t_mbt_i[1 << 2], t_mbt_p[1 << 6], t_mbt_b[1 << 6];
static vlc_t t_cbp[1 << 9], t_motion[1 << 11], t_dc_lum[1 << 7], t_dc_chr[1 << 8];
static vlc_t t_dct[1 << 16];
static int32_t idct_k[8][8];             /* 8192 * C(u)/2 * cos((2x+1)u pi/16) */

static const uint8_t ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63,
};
/* default intra quantiser matrix (2.4.3.2), natural order */
static const uint8_t DEFAULT_INTRA[64] = {
     8,16,19,22,26,27,29,34, 16,16,22,24,27,29,34,37,
    19,22,26,27,29,34,34,38, 22,22,26,27,29,34,37,40,
    22,26,27,29,32,35,40,48, 26,27,29,32,35,40,48,58,
    26,27,29,34,38,46,56,69, 27,29,35,38,46,56,69,83,
};

static void tables_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    vlc_build(t_mbai, 11, MBAI, N(MBAI));
    vlc_build(t_mbt_i, 2, MBT_I, N(MBT_I));
    vlc_build(t_mbt_p, 6, MBT_P, N(MBT_P));
    vlc_build(t_mbt_b, 6, MBT_B, N(MBT_B));
    vlc_build(t_cbp, 9, CBP, N(CBP));
    vlc_build(t_dc_lum, 7, DC_LUM, N(DC_LUM));
    vlc_build(t_dc_chr, 8, DC_CHR, N(DC_CHR));
    vlc_build(t_dct, 16, DCT, N(DCT));
    {   /* motion codes with their sign bit */
        static char codes[33][16];
        vlc_src_t src[33];
        int n = 0;
        for (int m = 0; m <= 16; m++) {
            for (int s = 0; s < (m ? 2 : 1); s++) {
                strcpy(codes[n], MOTION_MAG[m]);
                if (m) strcat(codes[n], s ? "1" : "0");
                src[n].code = codes[n];
                src[n].val = s ? -m : m;
                n++;
            }
        }
        vlc_build(t_motion, 11, src, n);
    }
    for (int u = 0; u < 8; u++)
        for (int x = 0; x < 8; x++) {
            double c = (u ? 1.0 : sqrt(0.5)) / 2.0 * cos((2 * x + 1) * u * M_PI / 16.0);
            idct_k[u][x] = (int32_t)lround(c * 8192.0);
        }
}

/* ======================= decoder state ======================= */

struct mpv {
    uint8_t *buf;
    size_t   cap, len, pos;          /* pos: first byte not consumed */
    size_t   bit;                    /* bit reader position, from buf[0] */
    size_t   end_bits;               /* reading past this = broken data */
    int      err;

    int      has_seq, width, height, mb_w, mb_h;
    uint8_t  intra_q[64], inter_q[64];       /* natural order */

    mpv_frame_t frm[3];
    int      ref_old, ref_new, b_idx;

    /* picture */
    int      type, full_fwd, fcode_fwd, full_bwd, fcode_bwd;
    mpv_frame_t *cur;
    int      qscale, mb_addr, dc_pred[3];
    int      pmv_fwd[2], pmv_bwd[2];         /* vector predictors, coded units */
    int      last_flags, mv_fwd[2], mv_bwd[2]; /* previous macroblock (B skips), half-pels */
    int16_t  blk[64];
};

/* ======================= bit reading ======================= */

static uint32_t peek(mpv_t *d, int n)
{
    size_t p = d->bit >> 3;
    int o = (int)(d->bit & 7);
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | (p + (size_t)i < d->len ? d->buf[p + (size_t)i] : 0);
    return (v << o) >> (32 - n);
}

static uint32_t get(mpv_t *d, int n)
{
    uint32_t v = peek(d, n);
    d->bit += (size_t)n;
    if (d->bit > d->end_bits) d->err = 1;
    return v;
}

static int vlc(mpv_t *d, const vlc_t *t, int bits)
{
    vlc_t e = t[peek(d, bits)];
    if (!e.len) { d->err = 1; d->bit++; return 0; }
    d->bit += e.len;
    if (d->bit > d->end_bits) d->err = 1;
    return e.val;
}

/* next start code (00 00 01 xx) at or after byte p: offset of its first
 * byte, or -1 */
static long find_sc(const mpv_t *d, size_t p)
{
    for (; p + 3 < d->len; p++)
        if (d->buf[p] == 0 && d->buf[p + 1] == 0 && d->buf[p + 2] == 1) return (long)p;
    return -1;
}

static long find_sc_code(const mpv_t *d, size_t p, int code)
{
    long s;
    while ((s = find_sc(d, p)) >= 0) {
        if (d->buf[s + 3] == code) return s;
        p = (size_t)s + 3;
    }
    return -1;
}

/* ======================= frames ======================= */

static void frames_alloc(mpv_t *d)
{
    for (int i = 0; i < 3; i++) {
        mpv_frame_t *f = &d->frm[i];
        free(f->y.data); free(f->cb.data); free(f->cr.data);
        f->width = d->width; f->height = d->height;
        f->y.width = d->mb_w * 16; f->y.height = d->mb_h * 16;
        f->cb.width = f->cr.width = d->mb_w * 8;
        f->cb.height = f->cr.height = d->mb_h * 8;
        size_t ny = (size_t)f->y.width * f->y.height, nc = (size_t)f->cb.width * f->cb.height;
        f->y.data = malloc(ny); f->cb.data = malloc(nc); f->cr.data = malloc(nc);
        if (f->y.data) memset(f->y.data, 16, ny);           /* black */
        if (f->cb.data) memset(f->cb.data, 128, nc);
        if (f->cr.data) memset(f->cr.data, 128, nc);
    }
    d->ref_old = 0; d->ref_new = 1; d->b_idx = 2;
}

/* ======================= public: input ======================= */

mpv_t *mpv_create(void)
{
    tables_init();
    mpv_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cap = 256 * 1024;
    d->buf = malloc(d->cap);
    if (!d->buf) { free(d); return NULL; }
    memcpy(d->intra_q, DEFAULT_INTRA, 64);
    memset(d->inter_q, 16, 64);
    return d;
}

void mpv_destroy(mpv_t *d)
{
    if (!d) return;
    for (int i = 0; i < 3; i++) { free(d->frm[i].y.data); free(d->frm[i].cb.data); free(d->frm[i].cr.data); }
    free(d->buf);
    free(d);
}

static void compact(mpv_t *d)
{
    if (d->pos == 0) return;
    memmove(d->buf, d->buf + d->pos, d->len - d->pos);
    d->len -= d->pos;
    d->pos = 0;
}

void mpv_write(mpv_t *d, const uint8_t *data, size_t n)
{
    if (d->len + n > d->cap) {
        compact(d);
        if (d->len + n > d->cap) {
            size_t c = d->cap;
            while (c < d->len + n) c *= 2;
            uint8_t *nb = realloc(d->buf, c);
            if (!nb) return;
            d->buf = nb; d->cap = c;
        }
    }
    memcpy(d->buf + d->len, data, n);
    d->len += n;
}

size_t mpv_buffered(const mpv_t *d) { return d->len - d->pos; }
int mpv_width(const mpv_t *d) { return d->width; }
int mpv_height(const mpv_t *d) { return d->height; }

const mpv_frame_t *mpv_frame(const mpv_t *d, int which)
{
    int i = which == MPV_REF_OLD ? d->ref_old : which == MPV_REF_NEW ? d->ref_new : d->b_idx;
    return d->has_seq ? &d->frm[i] : NULL;
}

/* ======================= sequence header ======================= */

/* parse the sequence header whose start code is at byte s; 0 if it is not
 * all in yet */
static int sequence_header(mpv_t *d, size_t s)
{
    if (find_sc(d, s + 4) < 0) return 0;     /* the next start code bounds it */
    d->bit = (s + 4) * 8;
    d->end_bits = d->len * 8;
    d->err = 0;
    int w = (int)get(d, 12), h = (int)get(d, 12);
    get(d, 4); get(d, 4);                    /* aspect ratio, picture rate */
    get(d, 18); get(d, 1); get(d, 10); get(d, 1);   /* bit rate, marker, vbv, constrained */
    if (get(d, 1)) for (int i = 0; i < 64; i++) d->intra_q[ZIGZAG[i]] = (uint8_t)get(d, 8);
    else memcpy(d->intra_q, DEFAULT_INTRA, 64);
    if (get(d, 1)) for (int i = 0; i < 64; i++) d->inter_q[ZIGZAG[i]] = (uint8_t)get(d, 8);
    else memset(d->inter_q, 16, 64);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 1;   /* nonsense: keep the old one */
    if (!d->has_seq || w != d->width || h != d->height) {
        d->width = w; d->height = h;
        d->mb_w = (w + 15) / 16; d->mb_h = (h + 15) / 16;
        frames_alloc(d);
    }
    d->has_seq = 1;
    return 1;
}

int mpv_has_header(mpv_t *d)
{
    if (d->has_seq) return 1;
    long s = find_sc_code(d, d->pos, 0xB3);
    if (s < 0) {                             /* nothing usable before a header */
        if (d->len - d->pos > 3) d->pos = d->len - 3;
        return 0;
    }
    d->pos = (size_t)s;
    if (!sequence_header(d, (size_t)s)) return 0;
    return 1;
}

/* ======================= inverse DCT ======================= */

static void idct(const int16_t *in, int *out)
{
    int64_t t[64];
    for (int r = 0; r < 8; r++) {            /* rows: horizontal frequencies */
        const int16_t *c = in + r * 8;
        int64_t *o = t + r * 8;
        if (!(c[0] | c[1] | c[2] | c[3] | c[4] | c[5] | c[6] | c[7])) { memset(o, 0, 8 * sizeof(*o)); continue; }
        for (int x = 0; x < 8; x++) {
            int64_t s = 0;
            for (int u = 0; u < 8; u++) s += (int64_t)c[u] * idct_k[u][x];
            o[x] = s;
        }
    }
    for (int x = 0; x < 8; x++)              /* columns: vertical frequencies */
        for (int y = 0; y < 8; y++) {
            int64_t s = 0;
            for (int v = 0; v < 8; v++) s += t[v * 8 + x] * idct_k[v][y];
            int p = (int)((s + ((int64_t)1 << 25)) >> 26);
            out[y * 8 + x] = p < -256 ? -256 : p > 255 ? 255 : p;
        }
}

static uint8_t clip(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* ======================= motion compensation ======================= */

/* bs x bs block at (x, y) of plane `dst` predicted from `ref` with the
 * half-pel vector (vx, vy); avg: average with what is there (bidirectional) */
static void mc(const mpv_plane_t *ref, const mpv_plane_t *dst, int x, int y, int bs, int vx, int vy, int avg)
{
    int ix = x + (vx >> 1), iy = y + (vy >> 1), hx = vx & 1, hy = vy & 1;
    int pw = ref->width, ph = ref->height;
    for (int j = 0; j < bs; j++) {
        int y0 = iy + j, y1 = y0 + hy;
        y0 = y0 < 0 ? 0 : y0 >= ph ? ph - 1 : y0;
        y1 = y1 < 0 ? 0 : y1 >= ph ? ph - 1 : y1;
        const uint8_t *r0 = ref->data + (size_t)y0 * pw, *r1 = ref->data + (size_t)y1 * pw;
        uint8_t *o = dst->data + (size_t)(y + j) * dst->width + x;
        for (int i = 0; i < bs; i++) {
            int x0 = ix + i, x1 = x0 + hx;
            x0 = x0 < 0 ? 0 : x0 >= pw ? pw - 1 : x0;
            x1 = x1 < 0 ? 0 : x1 >= pw ? pw - 1 : x1;
            int p;
            if (hx && hy) p = (r0[x0] + r0[x1] + r1[x0] + r1[x1] + 2) >> 2;
            else if (hx)  p = (r0[x0] + r0[x1] + 1) >> 1;
            else if (hy)  p = (r0[x0] + r1[x0] + 1) >> 1;
            else          p = r0[x0];
            o[i] = (uint8_t)(avg ? (o[i] + p + 1) >> 1 : p);
        }
    }
}

static void predict(mpv_t *d, int mbx, int mby, const mpv_frame_t *ref, const int *v, int avg)
{
    mc(&ref->y, &d->cur->y, mbx * 16, mby * 16, 16, v[0], v[1], avg);
    int cx = v[0] / 2, cy = v[1] / 2;        /* MPEG-1: chroma vector = luma / 2, towards zero */
    mc(&ref->cb, &d->cur->cb, mbx * 8, mby * 8, 8, cx, cy, avg);
    mc(&ref->cr, &d->cur->cr, mbx * 8, mby * 8, 8, cx, cy, avg);
}

/* prediction of a non-intra macroblock from the flags and vectors */
static void predict_mb(mpv_t *d, int addr, int flags, const int *vf, const int *vb)
{
    int mbx = addr % d->mb_w, mby = addr / d->mb_w;
    if (mby >= d->mb_h) return;
    const mpv_frame_t *fwd = d->type == 2 ? &d->frm[d->ref_new] : &d->frm[d->ref_old];
    const mpv_frame_t *bwd = &d->frm[d->ref_new];
    static const int zero[2] = { 0, 0 };
    if (d->type == 2) { predict(d, mbx, mby, fwd, (flags & MB_FWD) ? vf : zero, 0); return; }
    if (flags & MB_FWD) predict(d, mbx, mby, fwd, vf, 0);
    if (flags & MB_BWD) predict(d, mbx, mby, bwd, vb, (flags & MB_FWD) != 0);
}

/* one motion vector component (2.4.4.2): updates the predictor, returns it */
static int mv_comp(mpv_t *d, int fcode, int *pred)
{
    int code = vlc(d, t_motion, 11);
    int f = 1 << (fcode - 1), delta;
    if (f == 1 || code == 0) delta = code;
    else {
        int r = (int)get(d, fcode - 1);
        delta = (abs(code) - 1) * f + r + 1;
        if (code < 0) delta = -delta;
    }
    int v = *pred + delta;
    if (v > 16 * f - 1) v -= 32 * f;
    else if (v < -16 * f) v += 32 * f;
    *pred = v;
    return v;
}

/* ======================= blocks ======================= */

static void block(mpv_t *d, int i, int intra, int mbx, int mby)
{
    int16_t *b = d->blk;
    memset(b, 0, sizeof(d->blk));
    int n;                                    /* zigzag index of the last coefficient */
    const uint8_t *q = intra ? d->intra_q : d->inter_q;
    if (intra) {
        int comp = i < 4 ? 0 : i - 3;
        int size = vlc(d, comp ? t_dc_chr : t_dc_lum, comp ? 8 : 7), diff = 0;
        if (size) {
            diff = (int)get(d, size);
            if (diff < (1 << (size - 1))) diff -= (1 << size) - 1;
        }
        d->dc_pred[comp] += diff * 8;
        b[0] = (int16_t)d->dc_pred[comp];
        n = 0;
    } else {
        n = -1;
        if (peek(d, 1)) {                     /* first coefficient: "1s" is (0,1) */
            get(d, 1);
            int lvl = get(d, 1) ? -1 : 1;
            n = 0;
            int v = ((2 * lvl + (lvl > 0 ? 1 : -1)) * d->qscale * q[0]) / 16;
            if (!(v & 1)) v -= v > 0 ? 1 : -1;
            b[0] = (int16_t)v;
        }
    }
    for (;;) {
        int e = vlc(d, t_dct, 16), run, lvl;
        if (d->err) return;
        if (e == DCT_EOB) break;
        if (e == DCT_ESC) {
            run = (int)get(d, 6);
            lvl = (int)get(d, 8);
            if (lvl == 0) lvl = (int)get(d, 8);
            else if (lvl == 128) lvl = (int)get(d, 8) - 256;
            else if (lvl > 128) lvl -= 256;
        } else {
            run = e >> 8; lvl = e & 0xFF;
            if (get(d, 1)) lvl = -lvl;
        }
        n += run + 1;
        if (n > 63) { d->err = 1; return; }
        int z = ZIGZAG[n], v;
        if (intra) v = (2 * lvl * d->qscale * q[z]) / 16;
        else       v = ((2 * lvl + (lvl > 0 ? 1 : -1)) * d->qscale * q[z]) / 16;
        if (v && !(v & 1)) v -= v > 0 ? 1 : -1;            /* oddification */
        b[z] = (int16_t)(v > 2047 ? 2047 : v < -2048 ? -2048 : v);
    }
    int px[64];
    idct(b, px);
    const mpv_plane_t *pl = i < 4 ? &d->cur->y : i == 4 ? &d->cur->cb : &d->cur->cr;
    int x = i < 4 ? mbx * 16 + (i & 1) * 8 : mbx * 8;
    int y = i < 4 ? mby * 16 + (i >> 1) * 8 : mby * 8;
    for (int r = 0; r < 8; r++) {
        uint8_t *o = pl->data + (size_t)(y + r) * pl->width + x;
        for (int c = 0; c < 8; c++) o[c] = intra ? clip(px[r * 8 + c]) : clip(o[c] + px[r * 8 + c]);
    }
}

/* ======================= macroblocks and slices ======================= */

static void skipped_mb(mpv_t *d, int addr)
{
    d->dc_pred[0] = d->dc_pred[1] = d->dc_pred[2] = 1024;
    if (d->type == 2) {                       /* P: copy, vector predictor reset */
        d->pmv_fwd[0] = d->pmv_fwd[1] = 0;
        predict_mb(d, addr, 0, NULL, NULL);
    } else if (d->type == 3) {                /* B: same prediction as the previous one */
        predict_mb(d, addr, d->last_flags, d->mv_fwd, d->mv_bwd);
    }
}

static void macroblock(mpv_t *d, int first)
{
    int incr = 0;
    for (;;) {
        int c = vlc(d, t_mbai, 11);
        if (d->err) return;
        if (c == MBAI_STUFF) continue;
        if (c == MBAI_ESC) { incr += 33; continue; }
        incr += c;
        break;
    }
    if (first) d->mb_addr += incr;
    else {
        for (int k = 1; k < incr; k++) skipped_mb(d, d->mb_addr + k);
        d->mb_addr += incr;
    }
    if (d->mb_addr >= d->mb_w * d->mb_h) { d->err = 1; return; }

    int flags = d->type == 1 ? vlc(d, t_mbt_i, 2) : d->type == 2 ? vlc(d, t_mbt_p, 6) : vlc(d, t_mbt_b, 6);
    if (d->err) return;
    if (flags & MB_QUANT) d->qscale = (int)get(d, 5);
    if (flags & MB_FWD) {
        int s = d->full_fwd ? 1 : 0;
        d->mv_fwd[0] = mv_comp(d, d->fcode_fwd, &d->pmv_fwd[0]) << s;
        d->mv_fwd[1] = mv_comp(d, d->fcode_fwd, &d->pmv_fwd[1]) << s;
    }
    if (flags & MB_BWD) {
        int s = d->full_bwd ? 1 : 0;
        d->mv_bwd[0] = mv_comp(d, d->fcode_bwd, &d->pmv_bwd[0]) << s;
        d->mv_bwd[1] = mv_comp(d, d->fcode_bwd, &d->pmv_bwd[1]) << s;
    }
    int cbp = (flags & MB_INTRA) ? 63 : (flags & MB_PATTERN) ? vlc(d, t_cbp, 9) : 0;
    if (d->err) return;
    int mbx = d->mb_addr % d->mb_w, mby = d->mb_addr / d->mb_w;

    if (flags & MB_INTRA) {
        d->pmv_fwd[0] = d->pmv_fwd[1] = d->pmv_bwd[0] = d->pmv_bwd[1] = 0;
        for (int i = 0; i < 6 && !d->err; i++) block(d, i, 1, mbx, mby);
        d->last_flags = MB_INTRA;
        return;
    }
    d->dc_pred[0] = d->dc_pred[1] = d->dc_pred[2] = 1024;
    if (d->type == 2 && !(flags & MB_FWD)) {  /* P without motion: zero vector */
        d->pmv_fwd[0] = d->pmv_fwd[1] = 0;
        d->mv_fwd[0] = d->mv_fwd[1] = 0;
    }
    predict_mb(d, d->mb_addr, flags, d->mv_fwd, d->mv_bwd);
    d->last_flags = flags;
    for (int i = 0; i < 6 && !d->err; i++)
        if (cbp & (32 >> i)) block(d, i, 0, mbx, mby);
}

static void slice(mpv_t *d, int row)
{
    d->mb_addr = (row - 1) * d->mb_w - 1;
    d->qscale = (int)get(d, 5);
    while (get(d, 1)) get(d, 8);              /* extra_information_slice */
    d->dc_pred[0] = d->dc_pred[1] = d->dc_pred[2] = 1024;
    d->pmv_fwd[0] = d->pmv_fwd[1] = d->pmv_bwd[0] = d->pmv_bwd[1] = 0;
    int first = 1;
    do {
        macroblock(d, first);
        first = 0;
    } while (!d->err && peek(d, 23) != 0);
}

/* ======================= pictures ======================= */

int mpv_decode(mpv_t *d, int flush)
{
    if (!mpv_has_header(d)) return MPV_WAIT;
    long s;
    for (;;) {                               /* to the next picture, reading sequence headers */
        s = find_sc(d, d->pos);
        if (s < 0) {
            if (d->len - d->pos > 3) d->pos = d->len - 3;
            return MPV_NO_PICTURE;
        }
        int code = d->buf[s + 3];
        if (code == 0x00) break;
        if (code == 0xB3 && !sequence_header(d, (size_t)s)) { d->pos = (size_t)s; return MPV_WAIT; }
        d->pos = (size_t)s + 3;
    }
    d->pos = (size_t)s;
    long next = find_sc_code(d, (size_t)s + 4, 0x00);
    if (next < 0 && !flush) return MPV_WAIT;  /* the picture is not all in yet */

    /* picture header */
    d->bit = ((size_t)s + 4) * 8;
    d->end_bits = (next < 0 ? d->len : (size_t)next) * 8;
    d->err = 0;
    get(d, 10);                               /* temporal reference */
    d->type = (int)get(d, 3);
    get(d, 16);                               /* vbv delay */
    if (d->type == 2 || d->type == 3) { d->full_fwd = (int)get(d, 1); d->fcode_fwd = (int)get(d, 3); }
    if (d->type == 3) { d->full_bwd = (int)get(d, 1); d->fcode_bwd = (int)get(d, 3); }
    while (get(d, 1)) get(d, 8);              /* extra_information_picture */

    int result = d->type;
    if (d->type < 1 || d->type > 3 || ((d->type >= 2) && !d->fcode_fwd) || (d->type == 3 && !d->fcode_bwd)) {
        result = MPV_SKIPPED;                 /* D picture or broken header */
    } else {
        int target = d->type == 3 ? d->b_idx : d->ref_old;
        d->cur = &d->frm[target];
        d->last_flags = 0;
        d->mv_fwd[0] = d->mv_fwd[1] = d->mv_bwd[0] = d->mv_bwd[1] = 0;
        size_t p = (d->bit + 7) >> 3;
        long sc;
        while ((sc = find_sc(d, p)) >= 0 && (next < 0 || sc < next)) {
            int code = d->buf[sc + 3];
            p = (size_t)sc + 4;
            if (code < 0x01 || code > 0xAF) continue;      /* extension / user data */
            if (code > d->mb_h) continue;
            d->bit = p * 8;
            d->err = 0;
            slice(d, code);
            p = (d->bit + 7) >> 3;
        }
        if (d->type != 3) {                   /* new reference */
            d->ref_old = d->ref_new;
            d->ref_new = target;
        }
    }
    d->pos = next < 0 ? d->len : (size_t)next;
    if (d->pos > 64 * 1024 && d->pos * 2 > d->len) compact(d);
    return result;
}
