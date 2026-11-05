/*
 * overlay_preview.c - renders the lobby screen and the title hint of every language into BMP files without the game (GDI only), to look at layout and fonts.
 *   sh tests/build_preview.sh  ->  tests/build/overlay_preview.exe <out dir> [width height [backdrop.bmp [lang code [resident.arc]]]]
 * backdrop.bmp (24 / 32-bit BMP) stands for the game frame behind the lobby (a stock menu screenshot); lang code (eng, rus, jpn ...) renders one language only (all = every language); resident.arc = nativePC\\archive\\gui\\resident.arc of the game (its own fonts and sprites; without it the imitation is drawn).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/overlay.h"

void r1_log(const char *fmt, ...) { (void)fmt; }

static int write_bmp(const char *path, unsigned *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    BITMAPFILEHEADER fh = {0x4d42, (DWORD)(sizeof fh + sizeof(BITMAPINFOHEADER) + (size_t)w * (size_t)h * 4), 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof ih, w, -h, 1, 32, BI_RGB, (DWORD)((size_t)w * (size_t)h * 4), 0, 0, 0, 0};
    fwrite(&fh, sizeof fh, 1, f), fwrite(&ih, sizeof ih, 1, f), fwrite(px, 4, (size_t)w * (size_t)h, f);
    fclose(f);
    return 1;
}

/* reads a 24 / 32-bit uncompressed BMP into 32-bit top-down pixels */
static unsigned *read_bmp(const char *path, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    if (fread(&fh, sizeof fh, 1, f) != 1 || fread(&ih, sizeof ih, 1, f) != 1 || ih.biCompression != BI_RGB || (ih.biBitCount != 24 && ih.biBitCount != 32)) {
        fclose(f);
        return NULL;
    }
    int bw = ih.biWidth, bh = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight, bpp = ih.biBitCount / 8, stride = (bw * bpp + 3) & ~3;
    unsigned char *row = (unsigned char *)malloc((size_t)stride);
    unsigned *px = (unsigned *)malloc((size_t)bw * (size_t)bh * 4);
    fseek(f, (long)fh.bfOffBits, SEEK_SET);
    for (int y = 0; y < bh; y++) {
        if (fread(row, 1, (size_t)stride, f) != (size_t)stride) break;
        int dy = ih.biHeight < 0 ? y : bh - 1 - y;
        for (int x = 0; x < bw; x++) px[(size_t)dy * (size_t)bw + (size_t)x] = 0xFF000000u | ((unsigned)row[x * bpp + 2] << 16) | ((unsigned)row[x * bpp + 1] << 8) | row[x * bpp];
    }
    free(row);
    fclose(f);
    *w = bw, *h = bh;
    return px;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int w = argc > 3 ? atoi(argv[2]) : 1920, h = argc > 3 ? atoi(argv[3]) : 1080;
    CreateDirectoryA(dir, NULL);
    if (argc > 4) {
        int bw = 0, bh = 0;
        unsigned *bpx = read_bmp(argv[4], &bw, &bh);
        if (bpx) overlay_set_preview_backdrop(bpx, bw, bh);
        else fprintf(stderr, "cannot read the backdrop %s\n", argv[4]);
    }
    const char *only = argc > 5 && strcmp(argv[5], "all") ? argv[5] : NULL;
    if (argc > 6) overlay_set_game_archive(argv[6]);
    static ov_scene sc;
    for (int lang = 0; lang < L1_LANGS; lang++) {
        if (only && strcmp(only, l1_lang_codes[lang])) continue;
        for (int armed = 0; armed < 2; armed++) {
            l1_t *l = l1_new();
            l1_settings s = {armed, lang & 1, lang & 2 ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE, 0};
            l1_env env;
            memset(&env, 0, sizeof env);
            env.lang = lang, env.pads_connected = armed ? (lang % 3) : 2;
            l1_open(l, &s, &env);
            memset(&sc, 0, sizeof sc);
            sc.kind = OV_LOBBY, sc.lang = lang;
            l1_event e = {L1E_DOWN, 0};
            l1_event_in(l, &e);
            l1_view_get(l, &sc.v);
            unsigned *px = NULL;
            char path[512];
            if (overlay_render_to_bitmap(&sc, w, h, &px)) {
                snprintf(path, sizeof path, "%s/lobby_%s_%s.bmp", dir, l1_lang_codes[lang], armed ? "on" : "off");
                write_bmp(path, px, w, h);
            }
            memset(&sc, 0, sizeof sc);
            sc.kind = OV_HINT, sc.lang = lang;
            l1_hint_text(&s, &env, sc.hint, sizeof sc.hint);
            if (overlay_render_to_bitmap(&sc, w, h, &px)) {
                snprintf(path, sizeof path, "%s/hint_%s_%s.bmp", dir, l1_lang_codes[lang], armed ? "on" : "off");
                write_bmp(path, px, w, h);
            }
            l1_free(l);
        }
    }
    printf("rendered %d languages to %s\n", L1_LANGS, dir);
    return 0;
}
