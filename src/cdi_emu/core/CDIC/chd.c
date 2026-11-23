/*
 * chd.c - MAME CHD (v5) CD image reader, see chd.h
 *
 * Everything here is written for this emulator from the format
 * descriptions: deflate (RFC 1951), LZMA (range coder + LZ77 with the
 * lc=3 lp=0 pb=2 literal model), FLAC (frames, fixed / LPC predictors,
 * Rice residuals), the CHD v5 map (canonical Huffman + run lengths) and the
 * CD-ROM Reed-Solomon product code (ECMA-130 annex A) for the ECC bytes.
 */

#include "chd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ================================================================ */
/* file helpers                                                      */

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

static uint64_t be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

/* ================================================================ */
/* MSB-first bit reader (CHD map, FLAC)                              */

typedef struct { const uint8_t *d; size_t n, bit; int over; } bitm_t;

static uint32_t bm_read(bitm_t *b, int k)
{
    uint32_t v = 0;
    while (k > 0) {
        size_t byte = b->bit >> 3;
        int off = (int)(b->bit & 7), avail = 8 - off, take = k < avail ? k : avail;
        uint32_t cur;
        if (byte < b->n) cur = b->d[byte];
        else { cur = 0; b->over = 1; }
        v = (v << take) | ((cur >> (avail - take)) & ((1u << take) - 1));
        b->bit += (size_t)take;
        k -= take;
    }
    return v;
}

static uint32_t bm_peek(bitm_t *b, int k)
{
    size_t save = b->bit;
    int over = b->over;
    uint32_t v = bm_read(b, k);
    b->bit = save;
    b->over = over;
    return v;
}

static int32_t bm_signed(bitm_t *b, int k)
{
    if (k == 0) return 0;
    uint32_t v = bm_read(b, k);
    if (k < 32 && (v & (1u << (k - 1)))) v |= ~0u << k;
    return (int32_t)v;
}

static uint32_t bm_unary(bitm_t *b)               /* zeros before the next 1 */
{
    uint32_t n = 0;
    for (;;) {
        size_t byte = b->bit >> 3;
        if (byte >= b->n) { b->over = 1; return n; }
        int off = (int)(b->bit & 7);
        uint8_t cur = (uint8_t)(b->d[byte] << off);
        if (cur == 0) { n += (uint32_t)(8 - off); b->bit += (size_t)(8 - off); continue; }
        while (!(cur & 0x80)) { cur <<= 1; n++; b->bit++; }
        b->bit++;                                  /* the 1 */
        return n;
    }
}

/* ================================================================ */
/* deflate (raw, RFC 1951)                                           */

typedef struct { const uint8_t *d; size_t n, pos; uint32_t buf; int cnt; int err; } bitl_t;

static int bl_bits(bitl_t *s, int need)
{
    uint32_t v = s->buf;
    while (s->cnt < need) {
        if (s->pos >= s->n) { s->err = 1; return 0; }
        v |= (uint32_t)s->d[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    s->buf = need < 32 ? v >> need : 0;
    s->cnt -= need;
    return (int)(v & ((1u << need) - 1));
}

typedef struct { short count[16]; short symbol[320]; } huff_t;

static int huff_build(huff_t *h, const uint8_t *len, int n)
{
    short offs[16];
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;                   /* over-subscribed */
    }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
    return left;
}

static int huff_decode(bitl_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= bl_bits(s, 1);
        int count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->err) return -1;
    }
    return -1;
}

static const short LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short LEXT[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
static const short DEXT[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int inflate_codes(bitl_t *s, uint8_t *out, size_t outn, size_t *o, const huff_t *lc, const huff_t *dc)
{
    for (;;) {
        int sym = huff_decode(s, lc);
        if (sym < 0 || s->err) return -1;
        if (sym < 256) {
            if (*o >= outn) return -1;
            out[(*o)++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = LBASE[sym] + bl_bits(s, LEXT[sym]);
            int ds = huff_decode(s, dc);
            if (ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)(DBASE[ds] + bl_bits(s, DEXT[ds]));
            if (s->err || dist > *o || *o + (size_t)len > outn) return -1;
            for (int i = 0; i < len; i++, (*o)++) out[*o] = out[*o - dist];
        }
    }
}

/* Returns the number of bytes written, -1 on a broken stream */
static long inflate_raw(const uint8_t *src, size_t srcn, uint8_t *out, size_t outn)
{
    bitl_t s = { src, srcn, 0, 0, 0, 0 };
    size_t o = 0;
    static const uint8_t ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int last;
    do {
        last = bl_bits(&s, 1);
        int type = bl_bits(&s, 2);
        if (s.err) return -1;
        if (type == 0) {                               /* stored */
            s.buf = 0; s.cnt = 0;
            if (s.pos + 4 > s.n) return -1;
            unsigned len = s.d[s.pos] | s.d[s.pos + 1] << 8;
            unsigned nlen = s.d[s.pos + 2] | s.d[s.pos + 3] << 8;
            s.pos += 4;
            if ((len ^ 0xFFFF) != nlen || s.pos + len > s.n || o + len > outn) return -1;
            memcpy(out + o, s.d + s.pos, len);
            s.pos += len;
            o += len;
        } else if (type == 1) {                        /* fixed codes */
            static huff_t lc, dc;
            static int ready = 0;
            if (!ready) {
                uint8_t l[288];
                int i = 0;
                for (; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                huff_build(&lc, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                huff_build(&dc, l, 30);
                ready = 1;
            }
            if (inflate_codes(&s, out, outn, &o, &lc, &dc) != 0) return -1;
        } else if (type == 2) {                        /* dynamic codes */
            int nlen = bl_bits(&s, 5) + 257, ndist = bl_bits(&s, 5) + 1, ncode = bl_bits(&s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return -1;
            uint8_t l[320];
            memset(l, 0, sizeof(l));
            for (int i = 0; i < ncode; i++) l[ORDER[i]] = (uint8_t)bl_bits(&s, 3);
            huff_t cl, lc, dc;
            if (huff_build(&cl, l, 19) != 0) return -1;
            int i = 0;
            while (i < nlen + ndist) {
                int sym = huff_decode(&s, &cl);
                if (sym < 0) return -1;
                if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
                int len = 0, rep;
                if (sym == 16) { if (i == 0) return -1; len = l[i - 1]; rep = 3 + bl_bits(&s, 2); }
                else if (sym == 17) rep = 3 + bl_bits(&s, 3);
                else rep = 11 + bl_bits(&s, 7);
                if (i + rep > nlen + ndist) return -1;
                while (rep--) l[i++] = (uint8_t)len;
            }
            if (l[256] == 0) return -1;
            int e = huff_build(&lc, l, nlen);
            if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return -1;
            e = huff_build(&dc, l + nlen, ndist);
            if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return -1;
            if (inflate_codes(&s, out, outn, &o, &lc, &dc) != 0) return -1;
        } else return -1;
    } while (!last);
    return (long)o;
}

/* ================================================================ */
/* LZMA (raw stream, no header, lc=3 lp=0 pb=2, no end marker)       */

#define LZ_LC 3
#define LZ_PB 2
typedef uint16_t prob_t;

typedef struct {
    const uint8_t *in;
    size_t n, pos;
    uint32_t range, code;
    int err;
} rc_t;

static uint8_t rc_byte(rc_t *r)
{
    if (r->pos < r->n) return r->in[r->pos++];
    r->err = 1;
    return 0;
}

static int rc_bit(rc_t *r, prob_t *p)
{
    uint32_t bound = (r->range >> 11) * *p;
    int bit;
    if (r->code < bound) { r->range = bound; *p = (prob_t)(*p + ((2048 - *p) >> 5)); bit = 0; }
    else { r->range -= bound; r->code -= bound; *p = (prob_t)(*p - (*p >> 5)); bit = 1; }
    if (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | rc_byte(r); }
    return bit;
}

static uint32_t rc_direct(rc_t *r, int n)
{
    uint32_t v = 0;
    while (n--) {
        r->range >>= 1;
        uint32_t bit = r->code >= r->range;
        if (bit) r->code -= r->range;
        v = (v << 1) | bit;
        if (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | rc_byte(r); }
    }
    return v;
}

static unsigned rc_tree(rc_t *r, prob_t *p, int bits)
{
    unsigned m = 1;
    for (int i = 0; i < bits; i++) m = (m << 1) + (unsigned)rc_bit(r, &p[m]);
    return m - (1u << bits);
}

static unsigned rc_rtree(rc_t *r, prob_t *p, int bits)
{
    unsigned m = 1, sym = 0;
    for (int i = 0; i < bits; i++) {
        unsigned b = (unsigned)rc_bit(r, &p[m]);
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

typedef struct { prob_t choice, choice2, low[16][8], mid[16][8], high[256]; } lzlen_t;

static unsigned lz_len(rc_t *r, lzlen_t *l, unsigned ps)
{
    if (!rc_bit(r, &l->choice)) return rc_tree(r, l->low[ps], 3);
    if (!rc_bit(r, &l->choice2)) return 8 + rc_tree(r, l->mid[ps], 3);
    return 16 + rc_tree(r, l->high, 8);
}

typedef struct {
    prob_t is_match[12][16], is_rep[12], is_rep_g0[12], is_rep_g1[12], is_rep_g2[12];
    prob_t is_rep0_long[12][16], pos_slot[4][64], spec_pos[115], align[16];
    lzlen_t len, rep_len;
    prob_t literal[0x300 << LZ_LC];
} lzprobs_t;

static void lz_init_probs(prob_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = 1024; }

static int lzma_decode(const uint8_t *src, size_t srcn, uint8_t *out, size_t outn)
{
    static lzprobs_t P;                    /* 12 KB, one decode at a time */
    lz_init_probs((prob_t *)&P, sizeof(P) / sizeof(prob_t));
    rc_t r = { src, srcn, 0, 0xFFFFFFFFu, 0, 0 };
    rc_byte(&r);                                       /* always 0 */
    for (int i = 0; i < 4; i++) r.code = (r.code << 8) | rc_byte(&r);
    uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    unsigned state = 0;
    size_t o = 0;
    while (o < outn) {
        unsigned ps = (unsigned)(o & ((1u << LZ_PB) - 1));
        if (!rc_bit(&r, &P.is_match[state][ps])) {
            uint8_t prev = o ? out[o - 1] : 0;
            prob_t *lp = P.literal + 0x300 * (unsigned)(prev >> (8 - LZ_LC));
            unsigned sym = 1;
            if (state >= 7) {
                if (rep0 >= o) return -1;
                unsigned mb = out[o - rep0 - 1];
                do {
                    unsigned mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    unsigned b = (unsigned)rc_bit(&r, &lp[((1 + mbit) << 8) + sym]);
                    sym = (sym << 1) | b;
                    if (mbit != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | (unsigned)rc_bit(&r, &lp[sym]);
            out[o++] = (uint8_t)sym;
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        unsigned len;
        if (rc_bit(&r, &P.is_rep[state])) {
            if (o == 0) return -1;
            if (!rc_bit(&r, &P.is_rep_g0[state])) {
                if (!rc_bit(&r, &P.is_rep0_long[state][ps])) {          /* short rep */
                    state = state < 7 ? 9 : 11;
                    if (rep0 >= o) return -1;
                    out[o] = out[o - rep0 - 1];
                    o++;
                    continue;
                }
            } else {
                uint32_t dist;
                if (!rc_bit(&r, &P.is_rep_g1[state])) dist = rep1;
                else {
                    if (!rc_bit(&r, &P.is_rep_g2[state])) dist = rep2;
                    else { dist = rep3; rep3 = rep2; }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = lz_len(&r, &P.rep_len, ps);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2; rep2 = rep1; rep1 = rep0;
            len = lz_len(&r, &P.len, ps);
            state = state < 7 ? 7 : 10;
            unsigned ls = len < 3 ? len : 3;
            unsigned slot = rc_tree(&r, P.pos_slot[ls], 6);
            uint32_t dist;
            if (slot < 4) dist = slot;
            else {
                int nd = (int)(slot >> 1) - 1;
                dist = (2 | (slot & 1)) << nd;
                if (slot < 14) dist += rc_rtree(&r, P.spec_pos + dist - slot, nd);
                else {
                    dist += rc_direct(&r, nd - 4) << 4;
                    dist += rc_rtree(&r, P.align, 4);
                }
                if (dist == 0xFFFFFFFFu) break;                         /* end marker */
            }
            rep0 = dist;
        }
        len += 2;
        if (rep0 >= o) return -1;
        while (len-- && o < outn) { out[o] = out[o - rep0 - 1]; o++; }
        if (r.err) return -1;
    }
    return r.err ? -1 : 0;
}

/* ================================================================ */
/* FLAC (stereo 16-bit, frames only: no STREAMINFO in a CHD hunk)    */

static int flac_residual(bitm_t *b, int32_t *res, int bs, int order)
{
    int method = (int)bm_read(b, 2);
    if (method > 1) return -1;
    int pbits = method ? 5 : 4, esc = method ? 31 : 15;
    int porder = (int)bm_read(b, 4);
    int parts = 1 << porder, k = order;
    if ((bs >> porder) < order) return -1;
    for (int p = 0; p < parts; p++) {
        int n = (bs >> porder) - (p == 0 ? order : 0);
        int param = (int)bm_read(b, pbits);
        if (param == esc) {
            int raw = (int)bm_read(b, 5);
            for (int i = 0; i < n; i++) res[k++] = bm_signed(b, raw);
        } else {
            for (int i = 0; i < n; i++) {
                uint32_t q = bm_unary(b);
                uint32_t v = (q << param) | (param ? bm_read(b, param) : 0);
                res[k++] = (int32_t)(v >> 1) ^ -(int32_t)(v & 1);
            }
        }
        if (b->over) return -1;
    }
    return 0;
}

static int flac_subframe(bitm_t *b, int32_t *s, int bs, int bps)
{
    if (bm_read(b, 1)) return -1;
    int type = (int)bm_read(b, 6);
    int wasted = 0;
    if (bm_read(b, 1)) { wasted = (int)bm_unary(b) + 1; bps -= wasted; }
    if (bps <= 0 || bps > 32) return -1;
    if (type == 0) {
        int32_t v = bm_signed(b, bps);
        for (int i = 0; i < bs; i++) s[i] = v;
    } else if (type == 1) {
        for (int i = 0; i < bs; i++) s[i] = bm_signed(b, bps);
    } else if (type >= 8 && type <= 12) {
        int order = type - 8;
        for (int i = 0; i < order; i++) s[i] = bm_signed(b, bps);
        if (flac_residual(b, s, bs, order) != 0) return -1;
        for (int i = order; i < bs; i++) {
            int64_t p = 0;
            switch (order) {
            case 1: p = s[i - 1]; break;
            case 2: p = 2 * (int64_t)s[i - 1] - s[i - 2]; break;
            case 3: p = 3 * (int64_t)s[i - 1] - 3 * (int64_t)s[i - 2] + s[i - 3]; break;
            case 4: p = 4 * (int64_t)s[i - 1] - 6 * (int64_t)s[i - 2] + 4 * (int64_t)s[i - 3] - s[i - 4]; break;
            }
            s[i] += (int32_t)p;
        }
    } else if (type >= 32) {
        int order = type - 31;
        for (int i = 0; i < order; i++) s[i] = bm_signed(b, bps);
        int prec = (int)bm_read(b, 4) + 1;
        if (prec == 16) return -1;
        int shift = bm_signed(b, 5);
        if (shift < 0) return -1;
        int32_t c[32];
        for (int i = 0; i < order; i++) c[i] = bm_signed(b, prec);
        if (flac_residual(b, s, bs, order) != 0) return -1;
        for (int i = order; i < bs; i++) {
            int64_t sum = 0;
            for (int j = 0; j < order; j++) sum += (int64_t)c[j] * s[i - 1 - j];
            s[i] += (int32_t)(sum >> shift);
        }
    } else return -1;
    if (wasted) for (int i = 0; i < bs; i++) s[i] = (int32_t)((uint32_t)s[i] << wasted);
    return b->over ? -1 : 0;
}

/* Decodes stereo frames until 'samples' per channel are out, as big-endian
 * 16-bit interleaved (the CHD byte order). Returns the bytes of 'src' used
 * (end of the last frame), -1 on error. */
static long flac_decode(const uint8_t *src, size_t n, uint8_t *out, int samples)
{
    static int32_t ch[2][65536];
    bitm_t b = { src, n, 0, 0 };
    int done = 0;
    while (done < samples) {
        b.bit = (b.bit + 7) & ~(size_t)7;
        if (bm_read(&b, 14) != 0x3FFE) return -1;
        bm_read(&b, 2);                                /* reserved, blocking strategy */
        int bsc = (int)bm_read(&b, 4), src_code = (int)bm_read(&b, 4);
        int chan = (int)bm_read(&b, 4), ssc = (int)bm_read(&b, 3);
        bm_read(&b, 1);
        uint32_t first = bm_read(&b, 8);               /* UTF-8 style frame number */
        int extra = 0;
        while (extra < 7 && (first & (0x80u >> extra))) extra++;
        for (int i = 1; i < extra; i++) bm_read(&b, 8);
        int bs;
        if (bsc == 1) bs = 192;
        else if (bsc >= 2 && bsc <= 5) bs = 576 << (bsc - 2);
        else if (bsc == 6) bs = (int)bm_read(&b, 8) + 1;
        else if (bsc == 7) bs = (int)bm_read(&b, 16) + 1;
        else if (bsc >= 8) bs = 256 << (bsc - 8);
        else return -1;
        if (src_code == 12) bm_read(&b, 8);
        else if (src_code == 13 || src_code == 14) bm_read(&b, 16);
        bm_read(&b, 8);                                /* CRC-8 */
        int bps = ssc == 0 || ssc == 4 ? 16 : ssc == 1 ? 8 : ssc == 2 ? 12 : ssc == 5 ? 20 : ssc == 6 ? 24 : 0;
        if (!bps || bs > 65536 || b.over) return -1;
        int nch = chan < 8 ? chan + 1 : 2;
        if (nch != 2) return -1;
        for (int c = 0; c < 2; c++) {
            int sb = bps + ((chan == 8 && c == 1) || (chan == 9 && c == 0) || (chan == 10 && c == 1));
            if (flac_subframe(&b, ch[c], bs, sb) != 0) return -1;
        }
        for (int i = 0; i < bs; i++) {
            int32_t l = ch[0][i], r = ch[1][i];
            if (chan == 8) r = l - r;                  /* left / side */
            else if (chan == 9) l = l + r;             /* side / right */
            else if (chan == 10) {                     /* mid / side */
                int32_t mid = (int32_t)((uint32_t)l << 1) | (r & 1);
                l = (mid + r) >> 1;
                r = (mid - r) >> 1;
            }
            if (done + i < samples) {
                uint8_t *d = out + (size_t)(done + i) * 4;
                d[0] = (uint8_t)(l >> 8); d[1] = (uint8_t)l;
                d[2] = (uint8_t)(r >> 8); d[3] = (uint8_t)r;
            }
        }
        done += bs;
        b.bit = (b.bit + 7) & ~(size_t)7;
        bm_read(&b, 16);                               /* CRC-16 */
        if (b.over) return -1;
    }
    return (long)(b.bit >> 3);
}

/* ================================================================ */
/* CD-ROM ECC (P and Q parity, ECMA-130 annex A)                     */

static uint8_t ecc_f[256], ecc_b[256];

static void ecc_tables(void)
{
    static int ready = 0;
    if (ready) return;
    for (int i = 0; i < 256; i++) {
        int j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
        ecc_f[i] = (uint8_t)j;
        ecc_b[i ^ j] = (uint8_t)i;
    }
    ready = 1;
}

static void ecc_block(const uint8_t *src, int major_count, int minor_count, int major_mult, int minor_inc, uint8_t *dest)
{
    int size = major_count * minor_count;
    for (int major = 0; major < major_count; major++) {
        int index = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0, b = 0;
        for (int minor = 0; minor < minor_count; minor++) {
            uint8_t t = src[index];
            index += minor_inc;
            if (index >= size) index -= size;
            a ^= t;
            b ^= t;
            a = ecc_f[a];
        }
        a = ecc_b[ecc_f[a] ^ b];
        dest[major] = a;
        dest[major + major_count] = a ^ b;
    }
}

static void ecc_generate(uint8_t *s)
{
    ecc_tables();
    ecc_block(s + 12, 86, 24, 2, 86, s + 2076);        /* P */
    ecc_block(s + 12, 52, 43, 86, 88, s + 2248);       /* Q */
}

/* ================================================================ */
/* CHD v5                                                            */

enum { CT_0 = 0, CT_NONE = 4, CT_SELF = 5, CT_PARENT = 6, CT_RLE_SMALL = 7, CT_RLE_LARGE = 8,
       CT_SELF_0 = 9, CT_SELF_1 = 10, CT_PARENT_SELF = 11, CT_PARENT_0 = 12, CT_PARENT_1 = 13 };

typedef struct { uint8_t type; uint32_t length; uint64_t offset; } hunkmap_t;

struct chd {
    FILE *f;
    uint32_t codec[4];
    uint64_t logical;
    uint32_t hunkbytes, unitbytes, hunkcount;
    hunkmap_t *map;
    uint8_t *hunk, *cmp, *tmp;
    int64_t cached;
    int ntracks;
    chd_track_t track[CHD_MAX_TRACKS];
};

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* Huffman of the compressed map: 16 symbols, codes up to 8 bits */
static int map_huffman(bitm_t *b, uint8_t lookup_sym[256], uint8_t lookup_len[256])
{
    uint8_t nbits[16] = { 0 };
    int cur = 0;
    while (cur < 16) {
        int nb = (int)bm_read(b, 4);
        if (nb != 1) nbits[cur++] = (uint8_t)nb;
        else {
            nb = (int)bm_read(b, 4);
            if (nb == 1) nbits[cur++] = 1;
            else {
                int rep = (int)bm_read(b, 4) + 3;
                while (rep-- && cur < 16) nbits[cur++] = (uint8_t)nb;
            }
        }
    }
    uint32_t histo[33] = { 0 }, code[16];
    for (int i = 0; i < 16; i++) { if (nbits[i] > 8) return -1; histo[nbits[i]]++; }
    uint32_t start = 0;
    for (int len = 32; len > 0; len--) {
        uint32_t next = (start + histo[len]) >> 1;
        histo[len] = start;
        start = next;
    }
    for (int i = 0; i < 16; i++) code[i] = nbits[i] ? histo[nbits[i]]++ : 0;
    memset(lookup_len, 0, 256);
    for (int i = 0; i < 16; i++) {
        if (!nbits[i]) continue;
        int shift = 8 - nbits[i];
        uint32_t lo = code[i] << shift, hi = ((code[i] + 1) << shift);
        if (hi > 256) return -1;
        for (uint32_t k = lo; k < hi; k++) { lookup_sym[k] = (uint8_t)i; lookup_len[k] = nbits[i]; }
    }
    return 0;
}

static int map_symbol(bitm_t *b, const uint8_t *sym, const uint8_t *len)
{
    uint32_t k = bm_peek(b, 8);
    if (!len[k]) return -1;
    b->bit += len[k];
    return sym[k];
}

static int read_map(chd_t *c, uint64_t mapoffset, int compressed)
{
    c->map = calloc(c->hunkcount, sizeof(hunkmap_t));
    if (!c->map) return -1;
    if (!compressed) {                                 /* 4 bytes per hunk: offset / hunkbytes */
        uint8_t *raw = malloc((size_t)c->hunkcount * 4);
        if (!raw) return -1;
        if (_fseeki64(c->f, (int64_t)mapoffset, SEEK_SET) != 0 || fread(raw, 4, c->hunkcount, c->f) != c->hunkcount) { free(raw); return -1; }
        for (uint32_t i = 0; i < c->hunkcount; i++) {
            c->map[i].type = CT_NONE;
            c->map[i].offset = be(raw + i * 4, 4) * c->hunkbytes;
            c->map[i].length = c->hunkbytes;
        }
        free(raw);
        return 0;
    }
    uint8_t h[16];
    if (_fseeki64(c->f, (int64_t)mapoffset, SEEK_SET) != 0 || fread(h, 1, 16, c->f) != 16) return -1;
    uint32_t mapbytes = (uint32_t)be(h, 4);
    uint64_t curoff = be(h + 4, 6);
    int lenbits = h[12], selfbits = h[13], parentbits = h[14];
    uint8_t *buf = malloc(mapbytes + 8);
    if (!buf || fread(buf, 1, mapbytes, c->f) != mapbytes) { free(buf); return -1; }
    bitm_t b = { buf, mapbytes, 0, 0 };
    uint8_t lsym[256], llen[256];
    if (map_huffman(&b, lsym, llen) != 0) { free(buf); return -1; }
    int last = 0, rep = 0;
    for (uint32_t i = 0; i < c->hunkcount; i++) {
        if (rep > 0) { c->map[i].type = (uint8_t)last; rep--; continue; }
        int v = map_symbol(&b, lsym, llen);
        if (v < 0) { free(buf); return -1; }
        if (v == CT_RLE_SMALL) { c->map[i].type = (uint8_t)last; rep = 2 + map_symbol(&b, lsym, llen); }
        else if (v == CT_RLE_LARGE) {
            c->map[i].type = (uint8_t)last;
            rep = 2 + 16 + (map_symbol(&b, lsym, llen) << 4);
            rep += map_symbol(&b, lsym, llen);
        } else c->map[i].type = (uint8_t)(last = v);
    }
    uint64_t last_self = 0, last_parent = 0;
    for (uint32_t i = 0; i < c->hunkcount; i++) {
        hunkmap_t *m = &c->map[i];
        m->offset = curoff;
        m->length = 0;
        switch (m->type) {
        case 0: case 1: case 2: case 3:
            m->length = bm_read(&b, lenbits); curoff += m->length; bm_read(&b, 16); break;
        case CT_NONE:
            m->length = c->hunkbytes; curoff += m->length; bm_read(&b, 16); break;
        case CT_SELF:
            last_self = m->offset = bm_read(&b, selfbits); break;
        case CT_PARENT:
            m->offset = bm_read(&b, parentbits); last_parent = m->offset; break;
        case CT_SELF_1:
            last_self++;
            /* fall through */
        case CT_SELF_0:
            m->type = CT_SELF; m->offset = last_self; break;
        case CT_PARENT_SELF:
            m->type = CT_PARENT; last_parent = m->offset = (uint64_t)i * c->hunkbytes / c->unitbytes; break;
        case CT_PARENT_1:
            last_parent += c->hunkbytes / c->unitbytes;
            /* fall through */
        case CT_PARENT_0:
            m->type = CT_PARENT; m->offset = last_parent; break;
        default:
            free(buf); return -1;
        }
    }
    free(buf);
    return b.over ? -1 : 0;
}

static int track_type(const char *t, int *size)
{
    static const struct { const char *name; int type, size; } T[] = {
        { "MODE1_RAW", CHD_MODE1_RAW, 2352 }, { "MODE1/2352", CHD_MODE1_RAW, 2352 },
        { "MODE1", CHD_MODE1, 2048 }, { "MODE1/2048", CHD_MODE1, 2048 },
        { "MODE2_RAW", CHD_MODE2_RAW, 2352 }, { "MODE2/2352", CHD_MODE2_RAW, 2352 },
        { "CDI/2352", CHD_MODE2_RAW, 2352 },
        { "MODE2_FORM1", CHD_MODE2_FORM1, 2048 }, { "MODE2/2048", CHD_MODE2_FORM1, 2048 },
        { "MODE2_FORM2", CHD_MODE2_FORM2, 2324 }, { "MODE2/2324", CHD_MODE2_FORM2, 2324 },
        { "MODE2_FORM_MIX", CHD_MODE2, 2336 }, { "MODE2", CHD_MODE2, 2336 }, { "MODE2/2336", CHD_MODE2, 2336 },
        { "AUDIO", CHD_AUDIO, 2352 },
    };
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (!strcmp(t, T[i].name)) { *size = T[i].size; return T[i].type; }
    return -1;
}

static int read_tracks(chd_t *c, uint64_t meta)
{
    int frame = 0;
    while (meta && c->ntracks < CHD_MAX_TRACKS) {
        uint8_t h[16];
        if (_fseeki64(c->f, (int64_t)meta, SEEK_SET) != 0 || fread(h, 1, 16, c->f) != 16) return -1;
        uint32_t tag = (uint32_t)be(h, 4), len = (uint32_t)be(h + 5, 3);
        uint64_t next = be(h + 8, 8);
        if ((tag == TAG('C', 'H', 'T', '2') || tag == TAG('C', 'H', 'T', 'R')) && len < 512) {
            char txt[512], type[32] = "", sub[32] = "", pgtype[32] = "", pgsub[32] = "";
            if (fread(txt, 1, len, c->f) != len) return -1;
            txt[len] = 0;
            int num = 0, frames = 0, pregap = 0, postgap = 0;
            if (tag == TAG('C', 'H', 'T', '2'))
                sscanf(txt, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                       &num, type, sub, &frames, &pregap, pgtype, pgsub, &postgap);
            else
                sscanf(txt, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &num, type, sub, &frames);
            chd_track_t *t = &c->track[c->ntracks];
            t->type = track_type(type, &t->datasize);
            if (t->type < 0 || frames <= 0) return -2;
            t->frames = frames;
            t->pregap = pregap;
            t->pregap_stored = pgtype[0] == 'V';
            t->postgap = postgap;
            t->first_frame = frame;
            frame += (frames + 3) & ~3;                /* tracks padded to 4 frames */
            c->ntracks++;
        }
        meta = next;
    }
    return c->ntracks > 0 ? 0 : -1;
}

chd_t *chd_open(const char *path, char *err, int n)
{
    chd_t *c = calloc(1, sizeof(*c));
    uint8_t h[124];
    if (!c) { snprintf(err, n, "Mémoire insuffisante."); return NULL; }
    c->cached = -1;
    c->f = fopen_u8(path, "rb");
    if (!c->f) { snprintf(err, n, "Impossible d'ouvrir l'image CHD."); goto fail; }
    if (fread(h, 1, 124, c->f) != 124 || memcmp(h, "MComprHD", 8)) { snprintf(err, n, "Ce fichier n'est pas une image CHD."); goto fail; }
    if (be(h + 12, 4) != 5) { snprintf(err, n, "Image CHD de version %d : seule la version 5 est lue.", (int)be(h + 12, 4)); goto fail; }
    for (int i = 0; i < 4; i++) c->codec[i] = (uint32_t)be(h + 16 + 4 * i, 4);
    c->logical = be(h + 32, 8);
    uint64_t mapoff = be(h + 40, 8), metaoff = be(h + 48, 8);
    c->hunkbytes = (uint32_t)be(h + 56, 4);
    c->unitbytes = (uint32_t)be(h + 60, 4);
    for (int i = 0; i < 20; i++) if (h[104 + i]) { snprintf(err, n, "Image CHD « différentielle » (elle dépend d'une autre image) : non gérée."); goto fail; }
    if (c->unitbytes != CHD_FRAME || c->hunkbytes == 0 || c->hunkbytes % CHD_FRAME || c->hunkbytes > 1024 * 1024) {
        snprintf(err, n, "Cette image CHD n'est pas un CD (disque dur, laserdisc ou GD-ROM ?)."); goto fail;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t k = c->codec[i];
        if (k && k != TAG('c', 'd', 'l', 'z') && k != TAG('c', 'd', 'z', 'l') && k != TAG('c', 'd', 'f', 'l')) {
            snprintf(err, n, "Compression « %c%c%c%c » non gérée (gérées : cdlz, cdzl, cdfl).",
                     (char)(k >> 24), (char)(k >> 16), (char)(k >> 8), (char)k);
            goto fail;
        }
    }
    c->hunkcount = (uint32_t)((c->logical + c->hunkbytes - 1) / c->hunkbytes);
    int r = read_tracks(c, metaoff);
    if (r != 0) { snprintf(err, n, r == -2 ? "Type de piste inconnu dans l'image CHD." : "Liste des pistes introuvable dans l'image CHD."); goto fail; }
    if (read_map(c, mapoff, c->codec[0] != 0) != 0) { snprintf(err, n, "Table des blocs de l'image CHD illisible."); goto fail; }
    if (getenv("EMU_CHD_LOG")) {                       /* tests: hunks per compression */
        unsigned cnt[16] = { 0 };
        for (uint32_t i = 0; i < c->hunkcount; i++) cnt[c->map[i].type & 15]++;
        fprintf(stderr, "[CHD] %u hunks: codec0 %u codec1 %u codec2 %u codec3 %u none %u self %u parent %u\n",
                c->hunkcount, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6]);
    }
    c->hunk = malloc(c->hunkbytes);
    c->cmp = malloc(c->hunkbytes + 1024);
    c->tmp = malloc(c->hunkbytes);
    if (!c->hunk || !c->cmp || !c->tmp) { snprintf(err, n, "Mémoire insuffisante."); goto fail; }
    return c;
fail:
    chd_close(c);
    return NULL;
}

void chd_close(chd_t *c)
{
    if (!c) return;
    if (c->f) fclose(c->f);
    free(c->map);
    free(c->hunk);
    free(c->cmp);
    free(c->tmp);
    free(c);
}

int chd_ntracks(const chd_t *c) { return c->ntracks; }
const chd_track_t *chd_track(const chd_t *c, int i) { return &c->track[i]; }

/* CD codecs: [ECC bitmap][base length, 2 or 3 bytes][base][subcode] for
 * cdlz / cdzl; [FLAC frames][subcode] for cdfl. Base = all the sectors of
 * the hunk (2352 each), subcode = all their 96-byte subcodes. */
static int decode_cd(chd_t *c, uint32_t codec, const uint8_t *src, uint32_t n, uint8_t *dest)
{
    int frames = (int)(c->hunkbytes / CHD_FRAME);
    size_t base = (size_t)frames * 2352, sub = (size_t)frames * 96;
    uint8_t *tmp = c->tmp;
    const uint8_t *ecc = NULL;
    if (codec == TAG('c', 'd', 'f', 'l')) {
        long used = flac_decode(src, n, tmp, frames * 588);
        if (used < 0 || (uint32_t)used > n) return -1;
        if (inflate_raw(src + used, n - (uint32_t)used, tmp + base, sub) != (long)sub) return -1;
    } else {
        int eccb = (frames + 7) / 8, lenb = c->hunkbytes < 65536 ? 2 : 3;
        if (n < (uint32_t)(eccb + lenb)) return -1;
        ecc = src;
        uint32_t blen = (uint32_t)be(src + eccb, lenb);
        const uint8_t *b = src + eccb + lenb;
        if (eccb + lenb + blen > n) return -1;
        if (codec == TAG('c', 'd', 'l', 'z')) { if (lzma_decode(b, blen, tmp, base) != 0) return -1; }
        else if (inflate_raw(b, blen, tmp, base) != (long)base) return -1;
        if (inflate_raw(b + blen, n - eccb - lenb - blen, tmp + base, sub) != (long)sub) return -1;
    }
    static const uint8_t SYNC[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    for (int i = 0; i < frames; i++) {
        uint8_t *d = dest + (size_t)i * CHD_FRAME;
        memcpy(d, tmp + (size_t)i * 2352, 2352);
        memcpy(d + 2352, tmp + base + (size_t)i * 96, 96);
        if (ecc && (ecc[i / 8] & (1 << (i % 8)))) {     /* sync + ECC removed by the compressor */
            memcpy(d, SYNC, 12);
            ecc_generate(d);
        }
    }
    return 0;
}

static int read_hunk(chd_t *c, uint32_t hn, uint8_t *dest, int depth)
{
    if (hn >= c->hunkcount || depth > 4) return -1;
    hunkmap_t *m = &c->map[hn];
    switch (m->type) {
    case 0: case 1: case 2: case 3:
        if (m->length > c->hunkbytes + 1024) return -1;
        if (_fseeki64(c->f, (int64_t)m->offset, SEEK_SET) != 0 || fread(c->cmp, 1, m->length, c->f) != m->length) return -1;
        return decode_cd(c, c->codec[m->type], c->cmp, m->length, dest);
    case CT_NONE:
        if (m->offset == 0) { memset(dest, 0, c->hunkbytes); return 0; }
        if (_fseeki64(c->f, (int64_t)m->offset, SEEK_SET) != 0 || fread(dest, 1, c->hunkbytes, c->f) != c->hunkbytes) return -1;
        return 0;
    case CT_SELF:
        return read_hunk(c, (uint32_t)m->offset, dest, depth + 1);
    default:
        return -1;                                     /* parent */
    }
}

int chd_read_frame(chd_t *c, int frame, uint8_t *out)
{
    uint64_t pos = (uint64_t)frame * CHD_FRAME;
    if (frame < 0 || pos >= c->logical) return -1;
    uint32_t hn = (uint32_t)(pos / c->hunkbytes);
    if (c->cached != (int64_t)hn) {
        c->cached = -1;
        if (read_hunk(c, hn, c->hunk, 0) != 0) return -1;
        c->cached = hn;
    }
    memcpy(out, c->hunk + (pos % c->hunkbytes), CHD_FRAME);
    return 0;
}
