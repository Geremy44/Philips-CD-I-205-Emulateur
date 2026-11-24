/*
 * mp2_audio.c - MPEG-1 audio layer II decoder (see mp2_audio.h)
 *
 * Written for this emulator from ISO/IEC 11172-3: frame header, bit
 * allocation tables 3-B.2a to 3-B.2d, scale factor selection, grouped and
 * ungrouped samples, requantisation and the polyphase synthesis subband
 * filter. The scale factors (2^(1 - i/3)) and the requantised values
 * ((2 code - (steps - 1)) / steps) follow from their definition and are
 * computed. The synthesis window D[i] is the standard's table 3-B.3 in units
 * of 1/65536 (every value is an exact multiple), as reproduced in
 * "MPEG-Layer3 Bitstream Syntax and Decoding", M. Sieler and
 * R. Sperschneider, issue 1.2 (1997). Sources: docs/fmv/SOURCES_MPEG.md.
 */

#include "mp2_audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SBLIMIT 32

/* table 3-B.3, D[i] * 65536 */
static const int32_t WINDOW[512] = {
         0,     -1,     -1,     -1,     -1,     -1,     -1,     -2,
        -2,     -2,     -2,     -3,     -3,     -4,     -4,     -5,
        -5,     -6,     -7,     -7,     -8,     -9,    -10,    -11,
       -13,    -14,    -16,    -17,    -19,    -21,    -24,    -26,
       -29,    -31,    -35,    -38,    -41,    -45,    -49,    -53,
       -58,    -63,    -68,    -73,    -79,    -85,    -91,    -97,
      -104,   -111,   -117,   -125,   -132,   -139,   -147,   -154,
      -161,   -169,   -176,   -183,   -190,   -196,   -202,   -208,
       213,    218,    222,    225,    227,    228,    228,    227,
       224,    221,    215,    208,    200,    189,    177,    163,
       146,    127,    106,     83,     57,     29,     -2,    -36,
       -72,   -111,   -153,   -197,   -244,   -294,   -347,   -401,
      -459,   -519,   -581,   -645,   -711,   -779,   -848,   -919,
      -991,  -1064,  -1137,  -1210,  -1283,  -1356,  -1428,  -1498,
     -1567,  -1634,  -1698,  -1759,  -1817,  -1870,  -1919,  -1962,
     -2001,  -2032,  -2057,  -2075,  -2085,  -2087,  -2080,  -2063,
      2037,   2000,   1952,   1893,   1822,   1739,   1644,   1535,
      1414,   1280,   1131,    970,    794,    605,    402,    185,
       -45,   -288,   -545,   -814,  -1095,  -1388,  -1692,  -2006,
     -2330,  -2663,  -3004,  -3351,  -3705,  -4063,  -4425,  -4788,
     -5153,  -5517,  -5879,  -6237,  -6589,  -6935,  -7271,  -7597,
     -7910,  -8209,  -8491,  -8755,  -8998,  -9219,  -9416,  -9585,
     -9727,  -9838,  -9916,  -9959,  -9966,  -9935,  -9863,  -9750,
     -9592,  -9389,  -9139,  -8840,  -8492,  -8092,  -7640,  -7134,
      6574,   5959,   5288,   4561,   3776,   2935,   2037,   1082,
        70,   -998,  -2122,  -3300,  -4533,  -5818,  -7154,  -8540,
     -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189,
    -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
    -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137,
    -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420,
    -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
     75038,  74992,  74856,  74630,  74313,  73908,  73415,  72835,
     72169,  71420,  70590,  69679,  68692,  67629,  66494,  65290,
     64019,  62684,  61289,  59838,  58333,  56778,  55178,  53534,
     51853,  50137,  48390,  46617,  44821,  43006,  41176,  39336,
     37489,  35640,  33791,  31947,  30112,  28289,  26482,  24694,
     22929,  21189,  19478,  17799,  16155,  14548,  12980,  11455,
      9975,   8540,   7154,   5818,   4533,   3300,   2122,    998,
       -70,  -1082,  -2037,  -2935,  -3776,  -4561,  -5288,  -5959,
      6574,   7134,   7640,   8092,   8492,   8840,   9139,   9389,
      9592,   9750,   9863,   9935,   9966,   9959,   9916,   9838,
      9727,   9585,   9416,   9219,   8998,   8755,   8491,   8209,
      7910,   7597,   7271,   6935,   6589,   6237,   5879,   5517,
      5153,   4788,   4425,   4063,   3705,   3351,   3004,   2663,
      2330,   2006,   1692,   1388,   1095,    814,    545,    288,
        45,   -185,   -402,   -605,   -794,   -970,  -1131,  -1280,
     -1414,  -1535,  -1644,  -1739,  -1822,  -1893,  -1952,  -2000,
      2037,   2063,   2080,   2087,   2085,   2075,   2057,   2032,
      2001,   1962,   1919,   1870,   1817,   1759,   1698,   1634,
      1567,   1498,   1428,   1356,   1283,   1210,   1137,   1064,
       991,    919,    848,    779,    711,    645,    581,    519,
       459,    401,    347,    294,    244,    197,    153,    111,
        72,     36,      2,    -29,    -57,    -83,   -106,   -127,
      -146,   -163,   -177,   -189,   -200,   -208,   -215,   -221,
      -224,   -227,   -228,   -228,   -227,   -225,   -222,   -218,
       213,    208,    202,    196,    190,    183,    176,    169,
       161,    154,    147,    139,    132,    125,    117,    111,
       104,     97,     91,     85,     79,     73,     68,     63,
        58,     53,     49,     45,     41,     38,     35,     31,
        29,     26,     24,     21,     19,     17,     16,     14,
        13,     11,     10,      9,      8,      7,      7,      6,
         5,      5,      4,      4,      3,      3,      2,      2,
         2,      2,      1,      1,      1,      1,      1,      1,
};

/* quantisation classes: number of steps, bits per codeword, and whether
 * three samples share one codeword (grouping) */
typedef struct { uint16_t steps; uint8_t bits; uint8_t grouped; } qclass_t;
static const qclass_t QC[] = {
    {    3,  5, 1 }, {    5,  7, 1 }, {    7,  3, 0 }, {    9, 10, 1 },
    {   15,  4, 0 }, {   31,  5, 0 }, {   63,  6, 0 }, {  127,  7, 0 },
    {  255,  8, 0 }, {  511,  9, 0 }, { 1023, 10, 0 }, { 2047, 11, 0 },
    { 4095, 12, 0 }, { 8191, 13, 0 }, {16383, 14, 0 }, {32767, 15, 0 },
    {65535, 16, 0 },
};
enum { Q3, Q5, Q7, Q9, Q15, Q31, Q63, Q127, Q255, Q511, Q1023, Q2047, Q4095, Q8191, Q16383, Q32767, Q65535 };

/* bit allocation of a subband: width of the field, class of values 1..15 */
typedef struct { uint8_t nbal; int8_t cls[15]; } alloc_t;
static const alloc_t A_HI3 = { 4, { Q3, Q7, Q15, Q31, Q63, Q127, Q255, Q511, Q1023, Q2047, Q4095, Q8191, Q16383, Q32767, Q65535 } };
static const alloc_t A_HI8 = { 4, { Q3, Q5, Q7, Q9, Q15, Q31, Q63, Q127, Q255, Q511, Q1023, Q2047, Q4095, Q8191, Q65535 } };
static const alloc_t A_MID = { 3, { Q3, Q5, Q7, Q9, Q15, Q31, Q65535 } };
static const alloc_t A_LO  = { 2, { Q3, Q5, Q65535 } };
static const alloc_t A_LB4 = { 4, { Q3, Q5, Q9, Q15, Q31, Q63, Q127, Q255, Q511, Q1023, Q2047, Q4095, Q8191, Q16383, Q32767 } };
static const alloc_t A_LB3 = { 3, { Q3, Q5, Q9, Q15, Q31, Q63, Q127 } };

/* tables 3-B.2a (27 subbands) and 3-B.2b (30): 0 / 1;
 * 3-B.2c (8) and 3-B.2d (12): 2 / 3 */
static const alloc_t *alloc_for(int table, int sb)
{
    if (table >= 2) return sb < 2 ? &A_LB4 : &A_LB3;
    if (sb < 3) return &A_HI3;
    if (sb < 11) return &A_HI8;
    if (sb < 23) return &A_MID;
    return &A_LO;
}

static const int BITRATE[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
static const int RATE[3] = { 44100, 48000, 32000 };

static double scalefactor[64];
static double window_d[512];
static double matrix[64][32];        /* N[i][k] = cos((16 + i)(2k + 1) pi / 64) */

static void tables_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    for (int i = 0; i < 63; i++) scalefactor[i] = pow(2.0, 1.0 - i / 3.0);
    scalefactor[63] = 0;                 /* reserved value: silence */
    for (int i = 0; i < 512; i++) window_d[i] = WINDOW[i] / 65536.0;
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++) matrix[i][k] = cos((16 + i) * (2 * k + 1) * M_PI / 64.0);
}

struct mpa {
    uint8_t *buf;
    size_t   cap, len, pos;
    size_t   bit, end_bits;
    int      rate;
    double   v[2][1024];                 /* synthesis FIFO V per channel */
    int      voff[2];                    /* where V[0] is in it */
    float    out[1152 * 2];
};

mpa_t *mpa_create(void)
{
    tables_init();
    mpa_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->cap = 64 * 1024;
    a->buf = malloc(a->cap);
    if (!a->buf) { free(a); return NULL; }
    a->rate = 44100;
    return a;
}

void mpa_destroy(mpa_t *a)
{
    if (!a) return;
    free(a->buf);
    free(a);
}

static void compact(mpa_t *a)
{
    memmove(a->buf, a->buf + a->pos, a->len - a->pos);
    a->len -= a->pos;
    a->pos = 0;
}

void mpa_write(mpa_t *a, const uint8_t *data, size_t n)
{
    if (a->len + n > a->cap) {
        compact(a);
        if (a->len + n > a->cap) {
            size_t c = a->cap;
            while (c < a->len + n) c *= 2;
            uint8_t *nb = realloc(a->buf, c);
            if (!nb) return;
            a->buf = nb; a->cap = c;
        }
    }
    memcpy(a->buf + a->len, data, n);
    a->len += n;
}

size_t mpa_buffered(const mpa_t *a) { return a->len - a->pos; }
int mpa_samplerate(const mpa_t *a) { return a->rate; }

static uint32_t get(mpa_t *a, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t b = a->bit + (size_t)i;
        int x = b < a->end_bits ? (a->buf[b >> 3] >> (7 - (b & 7))) & 1 : 0;
        v = (v << 1) | (uint32_t)x;
    }
    a->bit += (size_t)n;
    return v;
}

/* 32 new subband samples -> 32 output samples: shift V by 64, matrixing,
 * then U (blocks 0..31 and 96..127 of every 128 values of V) times the
 * window, summed over 16 */
static void synthesis(mpa_t *a, int ch, const double *s, float *out, int stride)
{
    double *v = a->v[ch];
    int off = a->voff[ch] = (a->voff[ch] - 64) & 1023;
    for (int i = 0; i < 64; i++) {
        double sum = 0;
        for (int k = 0; k < 32; k++) sum += matrix[i][k] * s[k];
        v[(off + i) & 1023] = sum;
    }
    for (int j = 0; j < 32; j++) {
        double sum = 0;
        for (int i = 0; i < 16; i++) {
            int vi = (i >> 1) * 128 + ((i & 1) ? 96 : 0) + j;
            sum += window_d[i * 32 + j] * v[(off + vi) & 1023];
        }
        out[j * stride] = (float)sum;
    }
}

int mpa_decode(mpa_t *a, const float **samples)
{
    for (;;) {
        /* sync word, ID 1 (MPEG-1), layer '10' (II) */
        while (a->pos + 4 <= a->len &&
               !(a->buf[a->pos] == 0xFF && (a->buf[a->pos + 1] & 0xFE) == 0xFC))
            a->pos++;
        if (a->pos + 4 > a->len) return 0;
        const uint8_t *h = a->buf + a->pos;
        int bri = h[2] >> 4, fsi = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
        if (bri == 0 || bri == 15 || fsi == 3) { a->pos++; continue; }
        int fs = RATE[fsi], br = BITRATE[bri];
        size_t flen = (size_t)(144000 * br / fs + pad);
        if (a->pos + flen > a->len) return 0;     /* not all in yet */
        int prot = !(h[1] & 1), mode = h[3] >> 6, ext = (h[3] >> 4) & 3;
        int nch = mode == 3 ? 1 : 2;
        int bound = mode == 1 ? 4 * (ext + 1) : SBLIMIT;   /* joint stereo */
        /* allocation table, from the bit rate per channel */
        int brch = br / nch, table, sblimit;
        if (brch <= 48) { table = fsi == 2 ? 3 : 2; sblimit = fsi == 2 ? 12 : 8; }
        else if (fsi == 1 || brch <= 80) { table = 0; sblimit = 27; }
        else { table = 1; sblimit = 30; }
        if (bound > sblimit) bound = sblimit;

        a->bit = (a->pos + 4) * 8;
        a->end_bits = (a->pos + flen) * 8;
        if (prot) get(a, 16);                     /* CRC, not checked */
        int8_t cls[2][SBLIMIT];
        memset(cls, -1, sizeof(cls));
        for (int sb = 0; sb < sblimit; sb++) {
            const alloc_t *t = alloc_for(table, sb);
            for (int ch = 0; ch < (sb < bound ? nch : 1); ch++) {
                int v = (int)get(a, t->nbal);
                cls[ch][sb] = (int8_t)(v ? t->cls[v - 1] : -1);
            }
            if (sb >= bound) cls[1][sb] = cls[0][sb];
        }
        uint8_t scfsi[2][SBLIMIT], sf[2][SBLIMIT][3];
        for (int sb = 0; sb < sblimit; sb++)
            for (int ch = 0; ch < nch; ch++)
                if (cls[ch][sb] >= 0) scfsi[ch][sb] = (uint8_t)get(a, 2);
        for (int sb = 0; sb < sblimit; sb++)
            for (int ch = 0; ch < nch; ch++) {
                if (cls[ch][sb] < 0) continue;
                uint8_t *f = sf[ch][sb];
                switch (scfsi[ch][sb]) {
                case 0: f[0] = (uint8_t)get(a, 6); f[1] = (uint8_t)get(a, 6); f[2] = (uint8_t)get(a, 6); break;
                case 1: f[0] = f[1] = (uint8_t)get(a, 6); f[2] = (uint8_t)get(a, 6); break;
                case 2: f[0] = f[1] = f[2] = (uint8_t)get(a, 6); break;
                default: f[0] = (uint8_t)get(a, 6); f[1] = f[2] = (uint8_t)get(a, 6); break;
                }
            }
        /* 12 granules of 3 samples per subband */
        for (int gr = 0; gr < 12; gr++) {
            double s[2][3][SBLIMIT];
            memset(s, 0, sizeof(s));
            for (int sb = 0; sb < sblimit; sb++) {
                for (int ch = 0; ch < (sb < bound ? nch : 1); ch++) {
                    int c = cls[ch][sb];
                    if (c < 0) continue;
                    const qclass_t *q = &QC[c];
                    int code[3];
                    if (q->grouped) {
                        int g = (int)get(a, q->bits);
                        for (int k = 0; k < 3; k++) { code[k] = g % q->steps; g /= q->steps; }
                    } else
                        for (int k = 0; k < 3; k++) code[k] = (int)get(a, q->bits);
                    for (int k = 0; k < 3; k++) {
                        double val = (2.0 * code[k] - (q->steps - 1)) / q->steps;
                        s[ch][k][sb] = val * scalefactor[sf[ch][sb][gr >> 2]];
                        if (sb >= bound && nch == 2)     /* shared samples, own scale factors */
                            s[1][k][sb] = val * scalefactor[sf[1][sb][gr >> 2]];
                    }
                }
            }
            for (int k = 0; k < 3; k++)
                for (int ch = 0; ch < nch; ch++)
                    synthesis(a, ch, s[ch][k], a->out + ((gr * 3 + k) * 32) * 2 + ch, 2);
        }
        if (nch == 1)
            for (int i = 0; i < 1152; i++) a->out[2 * i + 1] = a->out[2 * i];
        a->rate = fs;
        a->pos += flen;
        if (a->pos > 32 * 1024 && a->pos * 2 > a->len) compact(a);
        *samples = a->out;
        return 1152;
    }
}
