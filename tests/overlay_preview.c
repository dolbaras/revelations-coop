/*
 * overlay_preview.c - renders the lobby screen and the title hint of every language into BMP files without the game (GDI only), to look at layout and fonts.
 *   sh tests/build_preview.sh  ->  tests/build/overlay_preview.exe <out dir> [width height]
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

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int w = argc > 3 ? atoi(argv[2]) : 1920, h = argc > 3 ? atoi(argv[3]) : 1080;
    CreateDirectoryA(dir, NULL);
    static ov_scene sc;
    for (int lang = 0; lang < L1_LANGS; lang++) {
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
