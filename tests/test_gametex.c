/*
 * test_gametex.c - offline test of gametex.c: the inflater against zlib streams made by Python (dynamic, fixed and stored blocks, vectors in gametex_vec.h),
 * and, when a game folder is given (argv[1] = path of the archive that holds the interface fonts, argv[2] = "rev1" or "rev2"), the real archive: entry read, TEX decode,
 * GFD glyph lookup and the pixels of one glyph. Without a game the archive part is skipped (the test stays green on a machine without the game).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/gametex.h"
#include "gametex_vec.h"

static int g_fail, g_run;
#define CHECK(c, ...) do { g_run++; if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
    unsigned char out[4096];
    CHECK(gt_inflate_zlib(vec_dyn, sizeof vec_dyn, out, sizeof vec_text) && !memcmp(out, vec_text, sizeof vec_text), "dynamic Huffman block");
    CHECK(gt_inflate_zlib(vec_fixed, sizeof vec_fixed, out, sizeof vec_short) && !memcmp(out, vec_short, sizeof vec_short), "fixed Huffman block");
    CHECK(gt_inflate_zlib(vec_stored, sizeof vec_stored, out, 300) && !memcmp(out, vec_text, 300), "stored block");
    CHECK(!gt_inflate_zlib(vec_dyn, sizeof vec_dyn / 2, out, sizeof vec_text), "truncated stream is refused");
    CHECK(!gt_inflate_zlib(vec_dyn, sizeof vec_dyn, out, sizeof vec_text - 1), "output too small is refused");
    CHECK(gt_arc_read("no\\such\\file.arc", "x", NULL) == NULL, "missing archive gives NULL");
    gt_img im;
    CHECK(!gt_tex_decode((const unsigned char *)"TEX", 3, &im), "short texture refused");
    if (argc > 2) {
        const char *arc = argv[1];
        int rev1 = !strcmp(argv[2], "rev1");
        gt_font *f = rev1 ? gt_font_open(arc, "id_bhr\\00_font\\Font_eng", "id_bhr\\00_font\\Font_eng_%02d_ID") : gt_font_open(arc, "ui_N\\00_font\\sys_eng", "ui_N\\00_font\\sys_eng_%02d_AM_NOMIP");
        CHECK(f != NULL, "font table opens");
        if (f) {
            const gt_glyph *a = gt_font_find(f, 'A'), *ru = gt_font_find(f, 0x430), *jp = gt_font_find(f, 0x3042);
            CHECK(a && ru && jp, "A, Cyrillic a and hiragana a are in the font");
            CHECK(gt_font_find(f, 0x10FFFF) == NULL, "unknown code point");
            CHECK(gt_font_cell(f) >= 20 && gt_font_cell(f) <= 40, "cell height %d", gt_font_cell(f));
            const gt_img *p = a ? gt_font_page(f, a->page) : NULL;
            CHECK(p && p->w == 1024 && p->h >= 512, "atlas page decodes (%dx%d)", p ? p->w : 0, p ? p->h : 0);
            if (p && a) {
                int ink = 0;
                for (int y = 0; y < a->h4 / 4; y++)
                    for (int x = 0; x < a->w; x++) {
                        unsigned px = p->px[(size_t)(a->y4 / 4 + y) * (size_t)p->w + (size_t)(a->x + x)];
                        if (((px >> 24) > 128 && rev1) || (!rev1 && ((px >> 16) & 255) > 128)) ink++;
                    }
                CHECK(ink > 25 && ink < 400, "glyph A has %d inked pixels", ink);
            }
        }
        gt_font_free(f);
    }
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
