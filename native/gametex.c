/*
 * gametex.c - see gametex.h. The inflater follows the structure of Mark Adler's public domain "puff" (canonical Huffman decoding by code counts).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gametex.h"

/* ---------------------------------------------------------------- inflate */
#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const unsigned char *in;
    unsigned inlen, incnt;
    int bitbuf, bitcnt;
    unsigned char *out;
    unsigned outlen, outcnt;
    int err;
} inf_t;

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
} huff_t;

static int inf_bits(inf_t *s, int need)
{
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) {
            s->err = 1;
            return 0;
        }
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int inf_decode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count, first += count, first <<= 1, code <<= 1;
    }
    return -1;
}

static int inf_construct(huff_t *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1, left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int inf_codes(inf_t *s, const huff_t *lencode, const huff_t *distcode)
{
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = inf_decode(s, lencode);
        if (symbol < 0) return 1;
        if (symbol < 256) {
            if (s->outcnt >= s->outlen) return 1;
            s->out[s->outcnt++] = (unsigned char)symbol;
        } else if (symbol == 256)
            return 0;
        else {
            symbol -= 257;
            if (symbol >= 29) return 1;
            int len = lens[symbol] + inf_bits(s, lext[symbol]);
            symbol = inf_decode(s, distcode);
            if (symbol < 0 || symbol >= 30) return 1;
            unsigned dist = (unsigned)(dists[symbol] + inf_bits(s, dext[symbol]));
            if (s->err || dist > s->outcnt || s->outcnt + (unsigned)len > s->outlen) return 1;
            while (len--) s->out[s->outcnt] = s->out[s->outcnt - dist], s->outcnt++;
        }
    }
}

static int inf_dynamic(inf_t *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[MAXLCODES + MAXDCODES];
    huff_t lencode, distcode;
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return 1;
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)inf_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (inf_construct(&lencode, lengths, 19) != 0) return 1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = inf_decode(s, &lencode);
        if (symbol < 0) return 1;
        if (symbol < 16) lengths[index++] = (short)symbol;
        else {
            int len = 0, rep;
            if (symbol == 16) {
                if (index == 0) return 1;
                len = lengths[index - 1], rep = 3 + inf_bits(s, 2);
            } else if (symbol == 17)
                rep = 3 + inf_bits(s, 3);
            else
                rep = 11 + inf_bits(s, 7);
            if (index + rep > nlen + ndist) return 1;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (s->err || lengths[256] == 0) return 1;
    int err = inf_construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return 1;
    err = inf_construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return 1;
    return inf_codes(s, &lencode, &distcode);
}

static int inflate_raw(inf_t *s)
{
    int last;
    do {
        last = inf_bits(s, 1);
        int type = inf_bits(s, 2);
        if (s->err) return 1;
        int err;
        if (type == 0) {
            s->bitbuf = 0, s->bitcnt = 0;
            if (s->incnt + 4 > s->inlen) return 1;
            unsigned len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
            s->incnt += 4;
            if (s->incnt + len > s->inlen || s->outcnt + len > s->outlen) return 1;
            memcpy(s->out + s->outcnt, s->in + s->incnt, len);
            s->incnt += len, s->outcnt += len;
            err = 0;
        } else if (type == 1) {
            huff_t lencode, distcode;
            short lengths[FIXLCODES];
            int sym;
            for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
            for (; sym < 256; sym++) lengths[sym] = 9;
            for (; sym < 280; sym++) lengths[sym] = 7;
            for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
            inf_construct(&lencode, lengths, FIXLCODES);
            for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
            inf_construct(&distcode, lengths, MAXDCODES);
            err = inf_codes(s, &lencode, &distcode);
        } else if (type == 2)
            err = inf_dynamic(s);
        else
            err = 1;
        if (err) return err;
    } while (!last);
    return 0;
}

int gt_inflate_zlib(const unsigned char *in, unsigned insize, unsigned char *out, unsigned outsize)
{
    inf_t s;
    memset(&s, 0, sizeof s);
    if (insize >= 2 && (in[0] & 0x0F) == 8 && ((in[0] << 8) | in[1]) % 31 == 0) in += 2, insize -= 2;   /* zlib header */
    s.in = in, s.inlen = insize, s.out = out, s.outlen = outsize;
    if (inflate_raw(&s)) return 0;
    return s.outcnt == outsize;
}

/* ---------------------------------------------------------------- ARC v7 */
static unsigned rd32(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }

unsigned char *gt_arc_read(const char *arc_path, const char *entry, unsigned *size_out)
{
    FILE *f = fopen(arc_path, "rb");
    if (!f) return NULL;
    unsigned char hdr[8];
    unsigned char *res = NULL;
    if (fread(hdr, 1, 8, f) == 8 && !memcmp(hdr, "ARC\0", 4)) {
        unsigned n = hdr[6] | (unsigned)hdr[7] << 8;
        unsigned char *tab = (unsigned char *)malloc((size_t)n * 80);
        if (tab && fread(tab, 80, n, f) == n) {
            for (unsigned i = 0; i < n; i++) {
                const unsigned char *e = tab + (size_t)i * 80;
                if (strncmp((const char *)e, entry, 64) != 0) continue;
                unsigned cs = rd32(e + 68), sz = rd32(e + 72) & 0x1fffffff, off = rd32(e + 76);
                if (sz == 0 || sz > 64u * 1024 * 1024 || cs > 64u * 1024 * 1024) break;
                unsigned char *raw = (unsigned char *)malloc(cs ? cs : 1);
                unsigned char *out = (unsigned char *)malloc(sz);
                if (raw && out && fseek(f, (long)off, SEEK_SET) == 0 && fread(raw, 1, cs, f) == cs) {
                    if (cs == sz) memcpy(out, raw, sz), res = out, out = NULL;
                    else if (gt_inflate_zlib(raw, cs, out, sz)) res = out, out = NULL;
                }
                free(raw), free(out);
                if (res && size_out) *size_out = sz;
                break;
            }
        }
        free(tab);
    }
    fclose(f);
    return res;
}

/* ---------------------------------------------------------------- TEX */
static unsigned rgb565(unsigned c)
{
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}
static unsigned mix(unsigned a, unsigned b, unsigned wa, unsigned wb, unsigned div)
{
    unsigned r = (((a >> 16) & 255) * wa + ((b >> 16) & 255) * wb) / div, g = (((a >> 8) & 255) * wa + ((b >> 8) & 255) * wb) / div, bl = ((a & 255) * wa + (b & 255) * wb) / div;
    return (r << 16) | (g << 8) | bl;
}

/* kind: 1 DXT1, 3 DXT3, 5 DXT5 */
static void dxt_decode(const unsigned char *d, int w, int h, int kind, unsigned *out)
{
    int bs = kind == 1 ? 8 : 16;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4, d += bs) {
            unsigned alpha[16];
            const unsigned char *cb = d;
            if (kind == 3) {
                for (int i = 0; i < 16; i++) alpha[i] = ((d[i / 2] >> (4 * (i & 1))) & 15) * 17u;
                cb = d + 8;
            } else if (kind == 5) {
                unsigned a0 = d[0], a1 = d[1], tab[8];
                unsigned long long bits = 0;
                for (int i = 0; i < 6; i++) bits |= (unsigned long long)d[2 + i] << (8 * i);
                tab[0] = a0, tab[1] = a1;
                if (a0 > a1)
                    for (unsigned i = 1; i < 7; i++) tab[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                else {
                    for (unsigned i = 1; i < 5; i++) tab[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    tab[6] = 0, tab[7] = 255;
                }
                for (int i = 0; i < 16; i++) alpha[i] = tab[(bits >> (3 * i)) & 7];
                cb = d + 8;
            } else
                for (int i = 0; i < 16; i++) alpha[i] = 255;
            unsigned c0 = cb[0] | cb[1] << 8, c1 = cb[2] | cb[3] << 8, idx = rd32(cb + 4), col[4];
            col[0] = rgb565(c0), col[1] = rgb565(c1);
            if (c0 > c1 || kind != 1) col[2] = mix(col[0], col[1], 2, 1, 3), col[3] = mix(col[0], col[1], 1, 2, 3);
            else col[2] = mix(col[0], col[1], 1, 1, 2), col[3] = 0;
            for (int i = 0; i < 16; i++) {
                int x = bx + (i & 3), y = by + (i >> 2);
                if (x < w && y < h) out[(size_t)y * (size_t)w + (size_t)x] = (alpha[i] << 24) | col[(idx >> (2 * i)) & 3];
            }
        }
}

void gt_img_free(gt_img *im)
{
    if (im) free(im->px), im->px = NULL, im->w = im->h = 0;
}

int gt_tex_decode(const unsigned char *t, unsigned size, gt_img *out)
{
    memset(out, 0, sizeof *out);
    if (size < 24 || memcmp(t, "TEX\0", 4) != 0) return 0;
    unsigned w2 = rd32(t + 8), w3 = rd32(t + 12), off = rd32(t + 16);
    int w = (int)((w2 >> 6) & 0x1fff), h = (int)((w2 >> 19) & 0x1fff), fmt = (int)((w3 >> 8) & 255);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || off >= size) return 0;
    unsigned avail = size - off;
    unsigned long long px = (unsigned long long)w * (unsigned long long)h;
    unsigned *img = (unsigned *)malloc((size_t)px * 4);
    if (!img) return 0;
    int kind = 0;
    if (avail >= px * 4) kind = 32;                                 /* uncompressed BGRA (0x27) */
    else if (avail >= px) kind = fmt == 0x15 ? 3 : 5;                /* 8 bits per pixel: DXT5 (DXT3 for 0x15) */
    else if (avail >= px / 2) kind = 1;                              /* 4 bits per pixel: DXT1 */
    if (kind == 32) {
        for (unsigned long long i = 0; i < px; i++) {
            const unsigned char *p = t + off + i * 4;
            img[i] = (unsigned)p[3] << 24 | (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0];
        }
    } else if (kind)
        dxt_decode(t + off, w, h, kind, img);
    else {
        free(img);
        return 0;
    }
    out->w = w, out->h = h, out->px = img;
    return 1;
}

/* ---------------------------------------------------------------- GFD fonts */
struct gt_font {
    char arc[512], pagefmt[128];
    gt_glyph *g;
    unsigned n;
    int cell;
    gt_img page[8];
    int tried[8];
};

static int glyph_cmp(const void *a, const void *b)
{
    unsigned x = ((const gt_glyph *)a)->cp, y = ((const gt_glyph *)b)->cp;
    return x < y ? -1 : x > y;
}

gt_font *gt_font_open(const char *arc_path, const char *gfd_entry, const char *page_fmt)
{
    unsigned size = 0;
    unsigned char *d = gt_arc_read(arc_path, gfd_entry, &size);
    if (!d) return NULL;
    gt_font *f = NULL;
    if (size > 0x40 && !memcmp(d, "GFD\0", 4)) {
        unsigned n = rd32(d + 0x1c);
        if (n > 0 && n < 100000 && (unsigned long long)n * 16 < size) {
            const unsigned char *rec = d + size - (size_t)n * 16;
            f = (gt_font *)calloc(1, sizeof *f);
            if (f) {
                f->g = (gt_glyph *)malloc((size_t)n * sizeof(gt_glyph));
                if (!f->g) {
                    free(f);
                    f = NULL;
                } else {
                    snprintf(f->arc, sizeof f->arc, "%s", arc_path), snprintf(f->pagefmt, sizeof f->pagefmt, "%s", page_fmt);
                    f->n = n;
                    int cellh[64];
                    memset(cellh, 0, sizeof cellh);
                    for (unsigned i = 0; i < n; i++) {
                        unsigned cp = rd32(rec + i * 16), w1 = rd32(rec + i * 16 + 4), w2 = rd32(rec + i * 16 + 8), w3 = rd32(rec + i * 16 + 12);
                        gt_glyph *g = &f->g[i];
                        g->cp = cp, g->page = (unsigned char)(w1 & 255), g->x = (unsigned short)((w1 >> 8) & 0x3ff), g->y4 = (unsigned short)((w1 >> 18) & 0xfff);
                        g->w = (unsigned short)((w2 >> 8) & 0x3ff), g->h4 = (unsigned short)((w2 >> 18) & 0xfff), g->adv = (unsigned char)(w3 & 255);
                        if (g->h4 / 4 < 64) cellh[g->h4 / 4]++;
                    }
                    int best = 0;
                    for (int i = 1; i < 64; i++)
                        if (cellh[i] > cellh[best]) best = i;
                    f->cell = best;
                    qsort(f->g, n, sizeof(gt_glyph), glyph_cmp);
                }
            }
        }
    }
    free(d);
    return f;
}

void gt_font_free(gt_font *f)
{
    if (!f) return;
    for (int i = 0; i < 8; i++) gt_img_free(&f->page[i]);
    free(f->g), free(f);
}

const gt_glyph *gt_font_find(const gt_font *f, unsigned cp)
{
    if (!f) return NULL;
    gt_glyph key;
    key.cp = cp;
    return (const gt_glyph *)bsearch(&key, f->g, f->n, sizeof(gt_glyph), glyph_cmp);
}

const gt_img *gt_font_page(gt_font *f, int page)
{
    if (!f || page < 0 || page >= 8) return NULL;
    if (!f->tried[page]) {
        f->tried[page] = 1;
        char name[200];
        snprintf(name, sizeof name, f->pagefmt, page);
        unsigned size = 0;
        unsigned char *t = gt_arc_read(f->arc, name, &size);
        if (t) {
            gt_tex_decode(t, size, &f->page[page]);
            free(t);
        }
    }
    return f->page[page].px ? &f->page[page] : NULL;
}

int gt_font_cell(const gt_font *f) { return f ? f->cell : 0; }
