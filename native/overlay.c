/*
 * Overlay of the Revelations 1 co-op lobby, see overlay.h. GDI composition + UpdateSurface + StretchRect (the technique of the Revelations 2 mod).
 * Look: the game's own menu style (dark teal vignette over the menu picture, spaced capital title between thin lines with dot ornaments, centred rows in a condensed face
 * with a soft glow ring on the selected row, round A / B legend). Source file is plain ASCII: texts come from lobby1_text.c.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "r1.h"

static float g_scale = 1.0f, g_ox = 0.0f, g_oy = 0.0f;
static int g_bbw = 1920, g_bbh = 1080;
static unsigned g_frames, g_fail_font, g_fail_tex, g_bd_ok, g_bd_fail;
static int g_cls;
static float g_hx = 260, g_hy = 1014, g_hw = 1400, g_hh = 54;   /* below the stock help line and the key legend (virtual y 914 / 980 on the Russian screen) */   /* hint strip, virtual 1920x1080 coordinates (console: menu hint x y w h) */
void overlay_set_hint_rect(float x, float y, float w, float h)
{
    if (w < 200) w = 200;
    if (h < 30) h = 30;
    g_hx = x, g_hy = y, g_hw = w, g_hh = h;
}

static struct { int x, y, w, h; } g_hit[L1_ROWS];
static int g_hit_n;

/* offline preview only: a picture that stands for the game frame behind the lobby / hint (tests/overlay_preview.c) */
static const unsigned *g_pv_px;
static int g_pv_w, g_pv_h;
void overlay_set_preview_backdrop(const unsigned *px, int w, int h) { g_pv_px = px, g_pv_w = w, g_pv_h = h; }

static int face_class(int lang) { return lang == 5 ? 1 : lang == 11 ? 2 : lang == 12 ? 3 : 0; }

/* ---------------------------------------------------------------- fonts
 * The game's own menu face is a condensed capital face baked into texture atlases inside its archives: GDI cannot load it. The closest face that every Windows 10 1709+ / 11 ships
 * is Bahnschrift (DIN-like, has Cyrillic, Greek and Latin Extended) in its Condensed widths; where it is missing the overlay falls back to Segoe UI with the same letter spacing.
 * CJK languages keep the system UI faces (no condensed capitals there). */
static int CALLBACK enum_face(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM p)
{
    (void)lf, (void)tm, (void)type;
    *(int *)p = 1;
    return 0;
}
static int face_present(const wchar_t *name)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, name, LF_FACESIZE - 1);
    int found = 0;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, enum_face, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}
static int g_have_bahn = -1;
static void fontspec(int cls, int role, const wchar_t **face, int *weight, float *scale)
{
    if (cls == 0) {
        if (g_have_bahn < 0) g_have_bahn = face_present(L"Bahnschrift Condensed") && face_present(L"Bahnschrift SemiBold Condensed");
        if (g_have_bahn) {
            *face = role == 0 ? L"Bahnschrift SemiBold Condensed" : L"Bahnschrift Condensed", *weight = 0, *scale = 1.0f;
            return;
        }
        *face = role == 0 ? L"Segoe UI Semibold" : L"Segoe UI", *weight = 0, *scale = 0.9f;
        return;
    }
    *face = cls == 1 ? L"Yu Gothic UI" : cls == 2 ? L"Microsoft YaHei UI" : L"Microsoft JhengHei UI";
    *weight = role == 0 ? FW_SEMIBOLD : FW_NORMAL, *scale = 0.95f;
}

static struct {
    HDC dc;
    HBITMAP bmp;
    unsigned *px;
    int w, h;
    IDirect3DSurface9 *sys, *def;
    D3DFORMAT fmt;
    unsigned sig;
    int have_sig;
    unsigned *bg;        /* the backdrop: the captured game frame, blurred, graded and darkened (w * h) */
    unsigned *cap;       /* scratch for the capture */
    int have_bg;
    unsigned bg_stamp;   /* tick of the capture series start */
    int bg_shots;        /* captures made in this series */
    int bg_final;        /* the last capture was a real picture (not a fade to black): no more shots */
    int lobby_was_on;
    struct { int px, role, cls; HFONT f; } fonts[40];
} G;

static void gdi_free(void)
{
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
        if (G.fonts[i].f) DeleteObject(G.fonts[i].f);
    if (G.sys) IDirect3DSurface9_Release(G.sys);
    if (G.def) IDirect3DSurface9_Release(G.def);
    if (G.dc) DeleteDC(G.dc);
    if (G.bmp) DeleteObject(G.bmp);
    free(G.bg), free(G.cap);
    memset(&G, 0, sizeof G);
}
static void rt_free(void);
void overlay_release(void)
{
    gdi_free();
    rt_free();
}

static int gdi_make_bitmap(int w, int h)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(NULL);
    G.dc = CreateCompatibleDC(sdc);
    G.bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&G.px, NULL, 0);
    ReleaseDC(NULL, sdc);
    if (!G.dc || !G.bmp || !G.px) return 0;
    SelectObject(G.dc, G.bmp);
    G.w = w, G.h = h;
    return 1;
}

/* ---------------------------------------------------------------- pixels (all positions in virtual 1920x1080 coordinates unless the name says dev) */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static void put_dev(int x, int y, unsigned rgb, unsigned a)
{
    if (x < 0 || y < 0 || x >= G.w || y >= G.h || a == 0) return;
    unsigned *p = G.px + (size_t)y * (size_t)G.w + (size_t)x;
    unsigned r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (a >= 255) {
        *p = 0xFF000000u | rgb;
        return;
    }
    unsigned d = *p, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    *p = 0xFF000000u | (((r * a + dr * (255 - a)) / 255) << 16) | (((g * a + dg * (255 - a)) / 255) << 8) | ((b * a + db * (255 - a)) / 255);
}

static void rect_px(float x, float y, float w, float h, DWORD argb)
{
    int x0 = (int)(g_ox + x * g_scale), y0 = (int)(g_oy + y * g_scale), x1 = (int)(g_ox + (x + w) * g_scale + 0.5f), y1 = (int)(g_oy + (y + h) * g_scale + 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G.w) x1 = G.w;
    if (y1 > G.h) y1 = G.h;
    unsigned a = argb >> 24;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) put_dev(xx, yy, argb & 0xFFFFFF, a);
}

static void frame_px(float x, float y, float w, float h, float t, DWORD argb)
{
    rect_px(x, y, w, t, argb), rect_px(x, y + h - t, w, t, argb), rect_px(x, y, t, h, argb), rect_px(x + w - t, y, t, h, argb);
}

/* a thin horizontal line whose ends fade out; amax 0..255, fade = fraction of the length faded at each end */
static void hline_fade(float x0, float x1, float y, float thick, unsigned rgb, float amax, float fade)
{
    int dx0 = (int)(g_ox + x0 * g_scale), dx1 = (int)(g_ox + x1 * g_scale), dy = (int)(g_oy + y * g_scale), th = (int)(thick * g_scale + 0.5f);
    if (th < 1) th = 1;
    for (int x = dx0; x < dx1; x++) {
        float t = (float)(x - dx0) / (float)(dx1 - dx0), e = t < 0.5f ? t : 1.0f - t;
        float k = fade > 0 ? clampf(e / fade, 0, 1) : 1;
        k = k * k * (3 - 2 * k);
        for (int j = 0; j < th; j++) put_dev(x, dy + j, rgb, (unsigned)(amax * k));
    }
}

/* soft elliptical glow: alpha falls from amax in the centre to 0 at the rim */
static void glow_ellipse(float cx, float cy, float rx, float ry, unsigned rgb, float amax)
{
    int x0 = (int)(g_ox + (cx - rx) * g_scale), x1 = (int)(g_ox + (cx + rx) * g_scale) + 1, y0 = (int)(g_oy + (cy - ry) * g_scale), y1 = (int)(g_oy + (cy + ry) * g_scale) + 1;
    float dcx = g_ox + cx * g_scale, dcy = g_oy + cy * g_scale, drx = rx * g_scale, dry = ry * g_scale;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float nx = ((float)x - dcx) / drx, ny = ((float)y - dcy) / dry, d = sqrtf(nx * nx + ny * ny);
            if (d >= 1.0f) continue;
            float k = 1.0f - d;
            put_dev(x, y, rgb, (unsigned)(amax * k * k));
        }
}

/* thin elliptical ring (anti-aliased) */
static void ring_ellipse(float cx, float cy, float rx, float ry, float thick, unsigned rgb, float amax)
{
    int x0 = (int)(g_ox + (cx - rx) * g_scale) - 3, x1 = (int)(g_ox + (cx + rx) * g_scale) + 3, y0 = (int)(g_oy + (cy - ry) * g_scale) - 3, y1 = (int)(g_oy + (cy + ry) * g_scale) + 3;
    float dcx = g_ox + cx * g_scale, dcy = g_oy + cy * g_scale, drx = rx * g_scale, dry = ry * g_scale, th = thick * g_scale;
    if (th < 1.0f) th = 1.0f;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float nx = ((float)x - dcx) / drx, ny = ((float)y - dcy) / dry, d = sqrtf(nx * nx + ny * ny);
            if (d < 0.001f) continue;
            float gx = nx / (d * drx), gy = ny / (d * dry), dist = fabsf(d - 1.0f) / sqrtf(gx * gx + gy * gy);   /* pixel distance to the ring: |d - 1| over the gradient of d */
            float k = clampf(1.0f - (dist - th * 0.5f), 0, 1);
            if (k > 0) put_dev(x, y, rgb, (unsigned)(amax * k));
        }
}

/* anti-aliased filled disc, centre and radius in virtual units */
static void disc(float cx, float cy, float r, unsigned rgb, float amax)
{
    float dcx = g_ox + cx * g_scale, dcy = g_oy + cy * g_scale, dr = r * g_scale;
    for (int y = (int)(dcy - dr) - 1; y <= (int)(dcy + dr) + 1; y++)
        for (int x = (int)(dcx - dr) - 1; x <= (int)(dcx + dr) + 1; x++) {
            float d = sqrtf(((float)x + 0.5f - dcx) * ((float)x + 0.5f - dcx) + ((float)y + 0.5f - dcy) * ((float)y + 0.5f - dcy));
            float k = clampf(dr - d + 0.5f, 0, 1);
            if (k > 0) put_dev(x, y, rgb, (unsigned)(amax * k));
        }
}

/* round button icon: vertical shade, darker rim, a letter on it */
static void text(const char *s, float x, float y, float w, float h, int px, int role, DWORD color, int align, float sp, int flags);
static void button_round(float cx, float cy, float r, unsigned top, unsigned bot, const char *letter)
{
    float dcx = g_ox + cx * g_scale, dcy = g_oy + cy * g_scale, dr = r * g_scale;
    for (int y = (int)(dcy - dr) - 1; y <= (int)(dcy + dr) + 1; y++)
        for (int x = (int)(dcx - dr) - 1; x <= (int)(dcx + dr) + 1; x++) {
            float dx = (float)x + 0.5f - dcx, dy = (float)y + 0.5f - dcy, d = sqrtf(dx * dx + dy * dy);
            float k = clampf(dr - d + 0.5f, 0, 1);
            if (k <= 0) continue;
            float t = clampf((dy / dr + 1.0f) * 0.5f, 0, 1), rim = clampf((d - (dr - 1.6f * g_scale)) / (1.6f * g_scale), 0, 1);
            unsigned cr = (unsigned)(((top >> 16) & 255) * (1 - t) + ((bot >> 16) & 255) * t), cg = (unsigned)(((top >> 8) & 255) * (1 - t) + ((bot >> 8) & 255) * t), cb = (unsigned)((top & 255) * (1 - t) + (bot & 255) * t);
            cr = (unsigned)(cr * (1 - 0.35f * rim)), cg = (unsigned)(cg * (1 - 0.35f * rim)), cb = (unsigned)(cb * (1 - 0.35f * rim));
            put_dev(x, y, (cr << 16) | (cg << 8) | cb, (unsigned)(255 * k));
        }
    text(letter, cx - r, cy - r, 2 * r, 2 * r, (int)(r * 1.25f), 0, 0xFFFFFFFF, 1, 0, 0);
}

/* ---------------------------------------------------------------- text */
static HFONT gdi_font(int px, int role, int cls)
{
    unsigned free_slot = (unsigned)-1;
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++) {
        if (G.fonts[i].f && G.fonts[i].px == px && G.fonts[i].role == role && G.fonts[i].cls == cls) return G.fonts[i].f;
        if (!G.fonts[i].f && free_slot == (unsigned)-1) free_slot = i;
    }
    if (free_slot == (unsigned)-1) {
        for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
            if (G.fonts[i].f) DeleteObject(G.fonts[i].f), G.fonts[i].f = NULL;
        free_slot = 0;
    }
    const wchar_t *face;
    int weight;
    float fs;
    fontspec(cls, role, &face, &weight, &fs);
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f) {
        g_fail_font++;
        return NULL;
    }
    G.fonts[free_slot].px = px, G.fonts[free_slot].role = role, G.fonts[free_slot].cls = cls, G.fonts[free_slot].f = f;
    return f;
}

enum { TA_LEFT_ = 0, TA_CENTER_ = 1, TA_RIGHT_ = 2 };
enum { TF_UP = 1, TF_WRAP = 2, TF_GLOW = 4 };

static void blur_u8(unsigned char *buf, int w, int h, int r)
{
    if (r < 1) return;
    unsigned char *tmp = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!tmp) return;
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {   /* horizontal */
            unsigned char *s = buf + (size_t)y * (size_t)w, *d = tmp + (size_t)y * (size_t)w;
            int sum = 0;
            for (int x = -r; x <= r; x++) sum += s[x < 0 ? 0 : x >= w ? w - 1 : x];
            for (int x = 0; x < w; x++) {
                d[x] = (unsigned char)(sum / (2 * r + 1));
                int a = x + r + 1, b = x - r;
                sum += s[a >= w ? w - 1 : a] - s[b < 0 ? 0 : b];
            }
        }
        for (int x = 0; x < w; x++) {   /* vertical */
            int sum = 0;
            for (int y = -r; y <= r; y++) sum += tmp[(size_t)(y < 0 ? 0 : y >= h ? h - 1 : y) * (size_t)w + (size_t)x];
            for (int y = 0; y < h; y++) {
                buf[(size_t)y * (size_t)w + (size_t)x] = (unsigned char)(sum / (2 * r + 1));
                int a = y + r + 1, b = y - r;
                sum += tmp[(size_t)(a >= h ? h - 1 : a) * (size_t)w + (size_t)x] - tmp[(size_t)(b < 0 ? 0 : b) * (size_t)w + (size_t)x];
            }
        }
    }
    free(tmp);
}

/* soft glow behind a text run: the text is drawn white into a small bitmap, blurred and added to the screen in the text colour */
static void glow_run(const wchar_t *ws, int n, HFONT f, int extra, int x0, int base, int width, unsigned rgb, float strength)
{
    TEXTMETRICW tm;
    SelectObject(G.dc, f);
    GetTextMetricsW(G.dc, &tm);
    int rad = tm.tmHeight / 5 > 2 ? tm.tmHeight / 5 : 2, pad = rad * 3;
    int gw = width + 2 * pad, gh = tm.tmHeight + 2 * pad;
    if (gw < 4 || gh < 4 || gw > 3000 || gh > 600) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = gw, bi.bmiHeader.biHeight = -gh, bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    unsigned *px = NULL;
    HDC sdc = GetDC(NULL);
    HDC dc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, sdc);
    unsigned char *m = (unsigned char *)malloc((size_t)gw * (size_t)gh);
    if (dc && bmp && px && m) {
        HGDIOBJ ob = SelectObject(dc, bmp), of = SelectObject(dc, f);
        memset(px, 0, (size_t)gw * (size_t)gh * 4);
        SetBkMode(dc, TRANSPARENT), SetTextColor(dc, RGB(255, 255, 255)), SetTextCharacterExtra(dc, extra), SetTextAlign(dc, TA_LEFT | TA_BASELINE);
        ExtTextOutW(dc, pad, pad + tm.tmAscent, 0, NULL, ws, (UINT)n, NULL);
        GdiFlush();
        for (int i = 0; i < gw * gh; i++) m[i] = (unsigned char)(px[i] & 255);
        blur_u8(m, gw, gh, rad);
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++) {
                unsigned a = (unsigned)(m[y * gw + x] * strength * 2.2f);
                if (a > 255) a = 255;
                put_dev(x0 - pad + x, base - tm.tmAscent - pad + y, rgb, a);
            }
        SelectObject(dc, of), SelectObject(dc, ob);
    }
    free(m);
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
}

static float g_glow_strength = 0.55f;

/* ---------------------------------------------------------------- the game's own material (gametex.c)
 * Fonts and sprites are read from the installed game's archives (nativePC\archive\gui\resident.arc: the fonts Caption / Font_eng / ..._chS / ..._chT and the interface atlas
 * common_00_ID) on first use, decoded on the CPU and cached; nothing is shipped. Any failure leaves RT.ok = 0 and the GDI / imitation drawing stays in charge. */
#include "gametex.h"

static char g_rt_arc[520];
void overlay_set_game_archive(const char *path)
{
    snprintf(g_rt_arc, sizeof g_rt_arc, "%s", path ? path : "");
}

typedef struct { int x, y, w, h; } rt_rect;
enum { SPR_ARC, SPR_RING, SPR_DISC, SPR_FRAME, SPR_SPARK, SPR_COUNT };
static struct {
    int tried, ok, sprites_ok;
    gt_font *font[4][2];   /* [language class][0 caption face, 1 body face] */
    gt_img common;
    rt_rect spr[SPR_COUNT];
    unsigned stat_real, stat_gdi;   /* strings drawn with the real face / by GDI */
    float capmid[4][2];             /* vertical middle of a capital H inside the glyph cell, atlas pixels (0 = not measured) */
} RT;

static void rt_free(void)
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 2; r++)
            if (c != 1) gt_font_free(RT.font[c][r]);   /* class 1 (Japanese) shares the pointers of class 0: one free only */
    gt_img_free(&RT.common);
    memset(&RT, 0, sizeof RT);
}

/* tight box of the visible (alpha > 12) pixels inside a coarse rectangle of the atlas */
static rt_rect rt_tighten(const gt_img *im, int x, int y, int w, int h)
{
    int x0 = im->w, y0 = im->h, x1 = -1, y1 = -1;
    for (int yy = y; yy < y + h && yy < im->h; yy++)
        for (int xx = x; xx < x + w && xx < im->w; xx++)
            if ((im->px[(size_t)yy * (size_t)im->w + (size_t)xx] >> 24) > 12) {
                if (xx < x0) x0 = xx;
                if (xx > x1) x1 = xx;
                if (yy < y0) y0 = yy;
                if (yy > y1) y1 = yy;
            }
    rt_rect r = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    if (x1 < 0) r.w = r.h = 0;
    return r;
}

static void rt_init(void)
{
    if (RT.tried) return;
    RT.tried = 1;
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    char path[520];
    if (g_rt_arc[0]) snprintf(path, sizeof path, "%s", g_rt_arc);
    else {
        char exe[520];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = n ? strrchr(exe, '\\') : NULL;
        if (!slash) return;
        *slash = 0;
        snprintf(path, sizeof path, "%s\\nativePC\\archive\\gui\\resident.arc", exe);
    }
    static const char *const gfd[4][2] = {{"id_bhr\\00_font\\Caption", "id_bhr\\00_font\\Font_eng"}, {"id_bhr\\00_font\\Caption", "id_bhr\\00_font\\Font_eng"},
                                          {"id_bhr\\00_font\\Caption_chS", "id_bhr\\00_font\\Font_chS"}, {"id_bhr\\00_font\\Caption_chT", "id_bhr\\00_font\\Font_chT"}};
    static const char *const pages[4][2] = {{"id_bhr\\00_font\\Caption_%02d_ID", "id_bhr\\00_font\\Font_eng_%02d_ID"}, {"id_bhr\\00_font\\Caption_%02d_ID", "id_bhr\\00_font\\Font_eng_%02d_ID"},
                                            {"id_bhr\\00_font\\Caption_chS_%02d_ID", "id_bhr\\00_font\\Font_chS_%02d_ID"}, {"id_bhr\\00_font\\Caption_chT_%02d_ID", "id_bhr\\00_font\\Font_chT_%02d_ID"}};
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 2; r++) {
            if (c == 1) RT.font[c][r] = RT.font[0][r];   /* Japanese: the Latin face and the same table (kana and kanji live in Font_eng) */
            else RT.font[c][r] = gt_font_open(path, gfd[c][r], pages[c][r]);
        }
    RT.ok = RT.font[0][0] && RT.font[0][1];
    unsigned size = 0;
    unsigned char *t = gt_arc_read(path, "id_bhr\\05_option\\tex_HD\\common_00_ID", &size);
    if (t) {
        if (gt_tex_decode(t, size, &RT.common) && RT.common.w == 1024 && RT.common.h == 256) {
            const gt_img *im = &RT.common;
            RT.spr[SPR_ARC] = rt_tighten(im, 380, 126, 644, 48);
            RT.spr[SPR_RING] = rt_tighten(im, 14, 128, 56, 54);
            RT.spr[SPR_DISC] = rt_tighten(im, 331, 132, 48, 52);
            RT.spr[SPR_FRAME] = rt_tighten(im, 172, 54, 90, 60);
            RT.spr[SPR_SPARK] = rt_tighten(im, 608, 172, 24, 20);
            RT.sprites_ok = 1;
            for (int i = 0; i < SPR_COUNT; i++)
                if (RT.spr[i].w < 4 || RT.spr[i].h < 4) RT.sprites_ok = 0;
        }
        free(t);
    }
    QueryPerformanceCounter(&q1);
    r1_log("overlay: game material %s (fonts %s, sprites %s) in %lld ms from %s", RT.ok || RT.sprites_ok ? "loaded" : "NOT available", RT.ok ? "ok" : "missing",
           RT.sprites_ok ? "ok" : "missing", qf.QuadPart ? (long long)((q1.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart) : -1LL, path);
}

/* bilinear alpha of an atlas page, clamped to the rectangle [x0, x0 + w) x [y0, y0 + h) */
static float rt_alpha(const gt_img *pg, float u, float v, float x0, float y0, float x1, float y1)
{
    if (u < x0) u = x0;
    if (v < y0) v = y0;
    if (u > x1) u = x1;
    if (v > y1) v = y1;
    int iu = (int)(u - 0.5f), iv = (int)(v - 0.5f);
    float fu = u - 0.5f - (float)iu, fv = v - 0.5f - (float)iv;
    if (u - 0.5f < 0) iu = 0, fu = 0;
    if (v - 0.5f < 0) iv = 0, fv = 0;
    int iu1 = iu + 1 < pg->w ? iu + 1 : iu, iv1 = iv + 1 < pg->h ? iv + 1 : iv;
    float a00 = (float)(pg->px[(size_t)iv * (size_t)pg->w + (size_t)iu] >> 24), a10 = (float)(pg->px[(size_t)iv * (size_t)pg->w + (size_t)iu1] >> 24);
    float a01 = (float)(pg->px[(size_t)iv1 * (size_t)pg->w + (size_t)iu] >> 24), a11 = (float)(pg->px[(size_t)iv1 * (size_t)pg->w + (size_t)iu1] >> 24);
    return ((a00 * (1 - fu) + a10 * fu) * (1 - fv) + (a01 * (1 - fu) + a11 * fu) * fv) / 255.0f;
}

typedef struct { const gt_glyph *g; const gt_img *pg; } rt_g;

/* the glyph of a code point in the caption face (role 0) or the body face (role 1), falling back to the other face (kana and kanji live in the body face) */
static int rt_find(int cls, int role, unsigned cp, rt_g *out)
{
    for (int k = 0; k < 2; k++) {
        gt_font *f = RT.font[cls][k == 0 ? role : 1 - role];
        const gt_glyph *g = gt_font_find(f, cp);
        if (!g || g->w == 0) {
            if (g && cp == 0x20) {   /* a space: no pixels, advance only */
                out->g = g, out->pg = NULL;
                return 1;
            }
            continue;
        }
        const gt_img *pg = gt_font_page(f, g->page);
        if (!pg || g->x + g->w > pg->w || g->y4 / 4 + g->h4 / 4 > pg->h) continue;
        out->g = g, out->pg = pg;
        return 1;
    }
    return 0;
}

/* vertical middle of the capital letters of a face inside its glyph cell: lines are centred on it, so the caption face and the body face share a baseline */
static float rt_capmid(int cls, int role)
{
    if (RT.capmid[cls][role] > 0) return RT.capmid[cls][role];
    float mid = 14.0f;
    gt_font *f = RT.font[cls][role];
    const gt_glyph *g = gt_font_find(f, 'H');
    const gt_img *pg = g ? gt_font_page(f, g->page) : NULL;
    if (g && pg && g->y4 / 4 + g->h4 / 4 <= pg->h) {
        int top = -1, bot = -1, gy = g->y4 / 4, gh = g->h4 / 4;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < g->w; x++)
                if ((pg->px[(size_t)(gy + y) * (size_t)pg->w + (size_t)(g->x + x)] >> 24) > 110) {
                    if (top < 0) top = y;
                    bot = y;
                    break;
                }
        if (top >= 0) mid = ((float)top + (float)bot + 1.0f) * 0.5f;
    }
    RT.capmid[cls][role] = mid;
    return mid;
}

/* the cell height used to scale: both faces are 28 px high in the atlas, the pixel size we are asked for (px) maps to 24 atlas pixels */
#define RT_EM 24.0f

/* looks every character up; returns the width in device pixels at scale s with extra spacing, or -1 when a glyph is missing */
static float rt_measure(const wchar_t *ws, int n, int cls, int role, float s, float extra, rt_g *gl)
{
    float w = 0;
    for (int i = 0; i < n; i++) {
        if (!rt_find(cls, role, ws[i], &gl[i])) return -1;
        w += (float)gl[i].g->adv * s + extra;
    }
    return w - (n ? extra : 0);
}

/* draws (or, with mask, accumulates) a laid-out run; x = left of the first glyph, ycell = top of the atlas cell in device pixels */
static void rt_draw_run(const rt_g *gl, int n, float x, float ytop, float s, float extra, unsigned rgb, float amul, unsigned char *mask, int mw, int mh, int mox, int moy)
{
    for (int i = 0; i < n; i++) {
        if (gl[i].pg) {
            const gt_glyph *g = gl[i].g;
            float gw = (float)g->w * s, gh = (float)(g->h4 / 4) * s, gy0 = (float)(g->y4 / 4);
            int dx0 = (int)floorf(x), dy0 = (int)floorf(ytop), dx1 = (int)ceilf(x + gw), dy1 = (int)ceilf(ytop + gh);
            for (int dy = dy0; dy < dy1; dy++)
                for (int dx = dx0; dx < dx1; dx++) {
                    float u = (float)g->x + ((float)dx + 0.5f - x) / s, v = gy0 + ((float)dy + 0.5f - ytop) / s;
                    float a = rt_alpha(gl[i].pg, u, v, (float)g->x, gy0, (float)(g->x + g->w), gy0 + (float)(g->h4 / 4)) * amul;
                    if (a <= 0.01f) continue;
                    if (mask) {
                        int mx = dx - mox, my = dy - moy;
                        if (mx >= 0 && my >= 0 && mx < mw && my < mh) {
                            unsigned v8 = (unsigned)(a * 255.0f);
                            if (v8 > mask[my * mw + mx]) mask[my * mw + mx] = (unsigned char)v8;
                        }
                    } else
                        put_dev(dx, dy, rgb, (unsigned)(a * 255.0f + 0.5f));
                }
        }
        x += (float)gl[i].g->adv * s + extra;
    }
}

/* one line; returns 1 when drawn with the real face (0: the caller uses GDI) */
static int rt_line(const wchar_t *ws, int n, float bx, float by, float bw, float bh, float pxd, int role, DWORD color, int align, float sp, int flags, float *width_out)
{
    if (!RT.ok || n <= 0 || n > 400) return 0;
    rt_g gl[400];
    float s = pxd / RT_EM, extra = sp * pxd;
    float w = rt_measure(ws, n, g_cls, role, s, extra, gl);
    if (w < 0) return 0;
    if (width_out) {
        *width_out = w;
        return 1;
    }
    if (w > bw && bw > 0) {   /* shrink to the box, down to 60 % */
        float k = bw / w;
        if (k < 0.6f) k = 0.6f;
        s *= k, extra *= k;
        w = rt_measure(ws, n, g_cls, role, s, extra, gl);
    }
    float x = align == TA_CENTER_ ? bx + (bw - w) * 0.5f : align == TA_RIGHT_ ? bx + bw - w : bx;
    float cellh = (float)gt_font_cell(RT.font[g_cls][role] ? RT.font[g_cls][role] : RT.font[0][0]) * s;
    if (cellh <= 0) cellh = 28.0f * s;
    float ytop = by + bh * 0.5f - rt_capmid(g_cls, role) * s;
    (void)cellh;
    if (flags & TF_GLOW) {   /* soft glow: the run into a mask, blurred, added in the text colour */
        int rad = (int)(pxd * 0.18f) > 2 ? (int)(pxd * 0.18f) : 2, pad = rad * 3;
        int mw = (int)w + 2 * pad + 2, mh = (int)cellh + 2 * pad + 2;
        if (mw > 3000 || mh > 600) goto plain;
        unsigned char *m = (unsigned char *)calloc((size_t)mw * (size_t)mh, 1);
        if (m) {
            int mox = (int)floorf(x) - pad, moy = (int)floorf(ytop) - pad;
            rt_draw_run(gl, n, x, ytop, s, extra, 0xFFFFFF, 1.0f, m, mw, mh, mox, moy);
            blur_u8(m, mw, mh, rad);
            for (int yy = 0; yy < mh; yy++)
                for (int xx = 0; xx < mw; xx++) {
                    unsigned a = (unsigned)(m[yy * mw + xx] * g_glow_strength * 2.2f);
                    if (a > 255) a = 255;
                    put_dev(mox + xx, moy + yy, color & 0xFFFFFF, a);
                }
            free(m);
        }
    }
plain:
    rt_draw_run(gl, n, x, ytop, s, extra, color & 0xFFFFFF, 1.0f, NULL, 0, 0, 0, 0);
    return 1;
}

static int rt_is_break(wchar_t c) { return c == L' ' || c == 0x3000; }
static int rt_is_cjk(wchar_t c) { return c >= 0x2E80 && c <= 0xFFEF && c != 0xFF0C && c != 0x3001 && c != 0x3002; }

/* wrapped text: greedy lines in the box, the size shrinks until the lines fit the height; returns 0 when a glyph is missing */
static int rt_wrap(const wchar_t *ws, int n, float bx, float by, float bw, float bh, float pxd, int role, DWORD color, int align, float sp)
{
    if (!RT.ok || n <= 0 || n > 400) return 0;
    rt_g gl[400];
    float s = pxd / RT_EM, minS = s * 0.6f;
    if (rt_measure(ws, n, g_cls, role, s, 0, gl) < 0) return 0;
    for (int it = 0; it < 10; it++) {
        float extra = sp * s * RT_EM, line_h = (float)gt_font_cell(RT.font[0][role]) * s * 1.12f;
        int starts[64], ends[64], nl = 0, pos = 0;
        while (pos < n && nl < 64) {
            float w = 0;
            int last_ok = pos, i = pos;
            while (i < n) {
                float cw = (float)gl[i].g->adv * s + extra;
                if (w + cw > bw && i > pos) break;
                w += cw, i++;
                if (rt_is_break(ws[i - 1]) || (rt_is_cjk(ws[i - 1]) && i < n)) last_ok = i;
            }
            int e = i;
            if (i < n && last_ok > pos) e = last_ok;
            starts[nl] = pos, ends[nl] = e, nl++;
            pos = e;
            while (pos < n && ws[pos] == L' ') pos++;
        }
        if (((float)nl * line_h <= bh + 1.0f) || s <= minS) {
            for (int l = 0; l < nl; l++) {
                int a = starts[l], b = ends[l];
                while (b > a && ws[b - 1] == L' ') b--;
                float lw = 0;
                for (int i = a; i < b; i++) lw += (float)gl[i].g->adv * s + extra;
                lw -= b > a ? extra : 0;
                float x = align == TA_CENTER_ ? bx + (bw - lw) * 0.5f : align == TA_RIGHT_ ? bx + bw - lw : bx;
                rt_draw_run(gl + a, b - a, x, by + (float)l * line_h, s, extra, color & 0xFFFFFF, 1.0f, NULL, 0, 0, 0, 0);
            }
            return 1;
        }
        s *= 0.93f;
    }
    return 1;
}

/* a sprite of the interface atlas, stretched to (dx, dy, dw, dh) in virtual units, tinted with rgb (the sprite's alpha is the shape) */
static void spr_draw(int id, float dx, float dy, float dw, float dh, unsigned rgb, float amax, int flipx)
{
    if (!RT.sprites_ok) return;
    const rt_rect *r = &RT.spr[id];
    float x0 = g_ox + dx * g_scale, y0 = g_oy + dy * g_scale, x1 = g_ox + (dx + dw) * g_scale, y1 = g_oy + (dy + dh) * g_scale;
    for (int y = (int)floorf(y0); y < (int)ceilf(y1); y++)
        for (int x = (int)floorf(x0); x < (int)ceilf(x1); x++) {
            float fx = ((float)x + 0.5f - x0) / (x1 - x0), fy = ((float)y + 0.5f - y0) / (y1 - y0);
            if (fx < 0 || fx > 1 || fy < 0 || fy > 1) continue;
            if (flipx) fx = 1.0f - fx;
            float a = rt_alpha(&RT.common, (float)r->x + fx * (float)r->w, (float)r->y + fy * (float)r->h, (float)r->x, (float)r->y, (float)(r->x + r->w), (float)(r->y + r->h)) * amax;
            if (a > 0.5f) put_dev(x, y, rgb, (unsigned)a);
        }
}

/* the rounded frame as a nine-patch (corners keep their size) */
static void spr_frame(float dx, float dy, float dw, float dh, unsigned rgb, float amax)
{
    if (!RT.sprites_ok) return;
    const rt_rect *r = &RT.spr[SPR_FRAME];
    float m = 8.0f;   /* source margin = corner size in the atlas */
    float sc = 1.0f;
    float sx[4] = {(float)r->x, (float)r->x + m, (float)(r->x + r->w) - m, (float)(r->x + r->w)}, sy[4] = {(float)r->y, (float)r->y + m, (float)(r->y + r->h) - m, (float)(r->y + r->h)};
    float ddx[4] = {dx, dx + m * sc, dx + dw - m * sc, dx + dw}, ddy[4] = {dy, dy + m * sc, dy + dh - m * sc, dy + dh};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            float px0 = g_ox + ddx[i] * g_scale, px1 = g_ox + ddx[i + 1] * g_scale, py0 = g_oy + ddy[j] * g_scale, py1 = g_oy + ddy[j + 1] * g_scale;
            for (int y = (int)floorf(py0); y < (int)ceilf(py1); y++)
                for (int x = (int)floorf(px0); x < (int)ceilf(px1); x++) {
                    float fx = ((float)x + 0.5f - px0) / (px1 - px0), fy = ((float)y + 0.5f - py0) / (py1 - py0);
                    if (fx < 0 || fx > 1 || fy < 0 || fy > 1) continue;
                    float u = sx[i] + fx * (sx[i + 1] - sx[i]), v = sy[j] + fy * (sy[j + 1] - sy[j]);
                    float a = rt_alpha(&RT.common, u, v, sx[i], sy[j], sx[i + 1], sy[j + 1]) * amax;
                    if (a > 0.5f) put_dev(x, y, rgb, (unsigned)a);
                }
        }
}

/* text in a box (virtual coordinates). One line: aligned in the box, shrunk to fit its width, centred vertically on the capital height.
 * TF_WRAP: several lines from the top of the box, shrunk to fit its height. sp = letter spacing in em (halved for CJK). TF_UP: capitals (Latin, Cyrillic, Greek). */
static void text(const char *s, float x, float y, float w, float h, int px, int role, DWORD color, int align, float sp, int flags)
{
    if (!s || !s[0]) return;
    wchar_t ws[700];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 699);
    if (n <= 1) return;
    n--;
    if (flags & TF_UP) CharUpperBuffW(ws, (DWORD)n);
    const wchar_t *face;
    int weight;
    float fs;
    fontspec(g_cls, role, &face, &weight, &fs);
    if (g_cls != 0) sp *= 0.5f;
    float bx = g_ox + x * g_scale, by = g_oy + y * g_scale, bw = w * g_scale, bh = h * g_scale;
    float pxd = (float)px * g_scale * fs;
    float pxmin = pxd * 0.6f;
    rt_init();
    if (RT.ok) {   /* the game's own face when every character is in it */
        int done = (flags & TF_WRAP) ? rt_wrap(ws, n, bx, by, bw, bh, (float)px * g_scale, role, color, align, sp) : rt_line(ws, n, bx, by, bw, bh, (float)px * g_scale, role, color, align, sp, flags, NULL);
        if (done) {
            RT.stat_real++;
            return;
        }
        RT.stat_gdi++;
    }
    SetBkMode(G.dc, TRANSPARENT);
    SetTextColor(G.dc, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
    if (flags & TF_WRAP) {
        HFONT f = NULL;
        RECT rc;
        SetTextAlign(G.dc, TA_LEFT | TA_TOP);
        for (int it = 0; it < 8; it++) {
            f = gdi_font((int)(pxd + 0.5f), role, g_cls);
            if (!f) return;
            SelectObject(G.dc, f);
            SetTextCharacterExtra(G.dc, (int)(sp * pxd));
            rc.left = 0, rc.top = 0, rc.right = (LONG)bw, rc.bottom = 0;
            DrawTextW(G.dc, ws, n, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_LEFT);
            if ((float)rc.bottom <= bh || pxd <= pxmin) break;
            pxd *= 0.93f;
        }
        rc.left = (LONG)bx, rc.top = (LONG)by, rc.right = (LONG)(bx + bw), rc.bottom = (LONG)(by + bh + 40);
        DrawTextW(G.dc, ws, n, &rc, DT_WORDBREAK | DT_NOPREFIX | (align == TA_CENTER_ ? DT_CENTER : align == TA_RIGHT_ ? DT_RIGHT : DT_LEFT));
        SetTextCharacterExtra(G.dc, 0);
        return;
    }
    HFONT f = NULL;
    SIZE sz = {0, 0};
    int extra = 0;
    for (int it = 0; it < 10; it++) {
        f = gdi_font((int)(pxd + 0.5f), role, g_cls);
        if (!f) return;
        SelectObject(G.dc, f);
        extra = (int)(sp * pxd + 0.5f);
        SetTextCharacterExtra(G.dc, extra);
        GetTextExtentPoint32W(G.dc, ws, n, &sz);
        if ((float)(sz.cx - extra) <= bw || pxd <= pxmin) break;
        pxd *= clampf(bw / (float)(sz.cx - extra), 0.6f, 0.97f);
    }
    int vis = sz.cx - extra;
    float tx = align == TA_CENTER_ ? bx + (bw - (float)vis) * 0.5f : align == TA_RIGHT_ ? bx + bw - (float)vis : bx;
    int base = (int)(by + bh * 0.5f + pxd * 0.36f + 0.5f);
    if (flags & TF_GLOW) glow_run(ws, n, f, extra, (int)tx, base, vis, color & 0xFFFFFF, g_glow_strength);
    SelectObject(G.dc, f);
    SetTextCharacterExtra(G.dc, extra);
    SetTextAlign(G.dc, TA_LEFT | TA_BASELINE);
    ExtTextOutW(G.dc, (int)tx, base, 0, NULL, ws, (UINT)n, NULL);
    SetTextCharacterExtra(G.dc, 0);
}

/* width of a single line in virtual units, as text() would draw it (no shrinking) */
static float text_w(const char *s, int px, int role, float sp, int flags)
{
    wchar_t ws[300];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 299);
    if (n <= 1) return 0;
    n--;
    if (flags & TF_UP) CharUpperBuffW(ws, (DWORD)n);
    const wchar_t *face;
    int weight;
    float fs;
    fontspec(g_cls, role, &face, &weight, &fs);
    if (g_cls != 0) sp *= 0.5f;
    float pxd = (float)px * g_scale * fs;
    rt_init();
    if (RT.ok) {
        float rw = 0;
        if (rt_line(ws, n, 0, 0, 0, 0, (float)px * g_scale, role, 0, 0, sp, 0, &rw)) return rw / g_scale;
    }
    HFONT f = gdi_font((int)(pxd + 0.5f), role, g_cls);
    if (!f) return 0;
    SelectObject(G.dc, f);
    int extra = (int)(sp * pxd + 0.5f);
    SetTextCharacterExtra(G.dc, extra);
    SIZE sz;
    GetTextExtentPoint32W(G.dc, ws, n, &sz);
    SetTextCharacterExtra(G.dc, 0);
    return (float)(sz.cx - extra) / g_scale;
}

/* ---------------------------------------------------------------- palette: sampled from the game's own menu (row_cursor4.png of the stage 6 notes) */
#define C_HI 0x74C9D4          /* selected item / title: light cyan */
#define C_HI_DIM 0x4E9AA6
#define C_ITEM 0x9A9892        /* unselected item: warm grey */
#define C_VAL 0x9FB4B8         /* value of an unselected row */
#define C_INFO 0x7F9A9E
#define C_GREY 0x5B6B6E
#define C_TEXT 0xD2D6D6        /* help line / legend: light grey */
#define C_LINE 0x46888F        /* thin lines */

static unsigned status_rgb(int kind) { return kind == L1S_WARN ? 0xF0C46E : kind == L1S_OK ? 0x8FE3C2 : 0xB9CFD2; }

/* ---------------------------------------------------------------- backdrop: the game frame under a teal-black vignette */
static unsigned lerp8(float a, float b, float t) { return (unsigned)clampf(a + (b - a) * t, 0, 255); }

/* src may be NULL (no capture): a plain vignette */
static void backdrop_build(const unsigned *src, int w, int h, unsigned *dst)
{
    const int f = 8;
    int sw = (w + f - 1) / f, sh = (h + f - 1) / f;
    float *sm = NULL, *tmp = NULL;
    if (src) {
        sm = (float *)malloc((size_t)sw * (size_t)sh * 3 * sizeof(float));
        tmp = (float *)malloc((size_t)sw * (size_t)sh * 3 * sizeof(float));
        if (!sm || !tmp) free(sm), free(tmp), sm = tmp = NULL;
    }
    if (sm) {
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                float a[3] = {0, 0, 0};
                int cnt = 0;
                for (int j = 0; j < f && y * f + j < h; j++)
                    for (int i = 0; i < f && x * f + i < w; i++) {
                        unsigned p = src[(size_t)(y * f + j) * (size_t)w + (size_t)(x * f + i)];
                        a[0] += (float)((p >> 16) & 255), a[1] += (float)((p >> 8) & 255), a[2] += (float)(p & 255), cnt++;
                    }
                for (int c = 0; c < 3; c++) sm[((size_t)y * (size_t)sw + (size_t)x) * 3 + (size_t)c] = a[c] / (float)(cnt ? cnt : 1);
            }
        for (int pass = 0; pass < 3; pass++) {   /* box blur, radius 2, three passes (about a gaussian) */
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    for (int c = 0; c < 3; c++) {
                        float s = 0;
                        for (int i = -2; i <= 2; i++) s += sm[((size_t)y * (size_t)sw + (size_t)(x + i < 0 ? 0 : x + i >= sw ? sw - 1 : x + i)) * 3 + (size_t)c];
                        tmp[((size_t)y * (size_t)sw + (size_t)x) * 3 + (size_t)c] = s / 5;
                    }
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    for (int c = 0; c < 3; c++) {
                        float s = 0;
                        for (int j = -2; j <= 2; j++) s += tmp[((size_t)(y + j < 0 ? 0 : y + j >= sh ? sh - 1 : y + j) * (size_t)sw + (size_t)x) * 3 + (size_t)c];
                        sm[((size_t)y * (size_t)sw + (size_t)x) * 3 + (size_t)c] = s / 5;
                    }
        }
    }
    for (int y = 0; y < h; y++) {
        float ny = ((float)y + 0.5f) / (float)h * 2.0f - 1.0f;
        for (int x = 0; x < w; x++) {
            float nx = ((float)x + 0.5f) / (float)w * 2.0f - 1.0f;
            float r2 = nx * nx * 0.62f + ny * ny * 0.95f;
            float vig = clampf(1.0f - r2 * 0.80f, 0, 1);
            vig = vig * vig;
            /* base: very dark teal at the rim, deeper teal in the middle, a soft glow behind the title */
            float tgx = nx / 0.55f, tgy = (ny + 0.80f) / 0.34f, tg = clampf(1.0f - sqrtf(tgx * tgx + tgy * tgy), 0, 1);
            float br = 3 + 13 * vig + 22 * tg * tg, bg = 8 + 34 * vig + 56 * tg * tg, bb = 10 + 40 * vig + 62 * tg * tg;
            float sr = 0, sg = 0, sb = 0;
            if (sm) {   /* bilinear sample of the blurred frame */
                float fx = ((float)x + 0.5f) / (float)f - 0.5f, fy = ((float)y + 0.5f) / (float)f - 0.5f;
                int ix = (int)floorf(fx), iy = (int)floorf(fy);
                float ax = fx - (float)ix, ay = fy - (float)iy;
                int x0 = ix < 0 ? 0 : ix >= sw ? sw - 1 : ix, x1 = ix + 1 < 0 ? 0 : ix + 1 >= sw ? sw - 1 : ix + 1, y0 = iy < 0 ? 0 : iy >= sh ? sh - 1 : iy, y1 = iy + 1 < 0 ? 0 : iy + 1 >= sh ? sh - 1 : iy + 1;
                const float *p00 = sm + ((size_t)y0 * (size_t)sw + (size_t)x0) * 3, *p10 = sm + ((size_t)y0 * (size_t)sw + (size_t)x1) * 3, *p01 = sm + ((size_t)y1 * (size_t)sw + (size_t)x0) * 3,
                            *p11 = sm + ((size_t)y1 * (size_t)sw + (size_t)x1) * 3;
                float c[3];
                for (int k = 0; k < 3; k++) c[k] = (p00[k] * (1 - ax) + p10[k] * ax) * (1 - ay) + (p01[k] * (1 - ax) + p11[k] * ax) * ay;
                float l = 0.30f * c[0] + 0.59f * c[1] + 0.11f * c[2];   /* the picture is kept as light and shade, graded to the menu's teal */
                float wgt = 0.42f + 0.70f * vig;
                sr = (l * 0.62f * 0.6f + c[0] * 0.4f) * wgt, sg = (l * 0.98f * 0.6f + c[1] * 0.4f) * wgt, sb = (l * 1.05f * 0.6f + c[2] * 0.4f) * wgt;
            }
            float r = br + sr, g = bg + sg, b = bb + sb;
            dst[(size_t)y * (size_t)w + (size_t)x] = 0xFF000000u | (lerp8(0, 255, clampf(r / 255, 0, 1)) << 16) | (lerp8(0, 255, clampf(g / 255, 0, 1)) << 8) | lerp8(0, 255, clampf(b / 255, 0, 1));
        }
    }
    free(sm), free(tmp);
}

/* ---------------------------------------------------------------- the lobby */
static void ornament_line(float y, float x0, float x1, float cx)
{
    hline_fade(x0, x1, y, 1.6f, C_LINE, 190, 0.35f);
    rect_px(cx - 0.8f, y - 22, 1.6f, 44, 0x90000000 | C_LINE);   /* vertical tick through the line, as in the game's frames */
    if (RT.sprites_ok) spr_draw(SPR_SPARK, cx - 15, y - 13, 30, 26, 0xD8F6FF, 255, 0);
    else {
        disc(cx, y, 4.5f, 0xA0E8F2, 220);
        disc(cx, y, 9.0f, C_HI, 46);
    }
}

static void draw_arc(float ycentre, float yedge)
{
    if (RT.sprites_ok) {   /* the game's own arc, once for each half of the screen */
        float ext = g_ox / g_scale;
        spr_draw(SPR_ARC, -ext, yedge - 4, 960 + ext, ycentre - yedge + 12, C_LINE + 0x203030, 235, 0);
        spr_draw(SPR_ARC, 960, yedge - 4, 960 + ext, ycentre - yedge + 12, C_LINE + 0x203030, 235, 1);
        return;
    }
    /* the long shallow curve over the top of the stock screens, drawn in segments of one pixel column */
    int w = G.w;
    for (int x = 0; x < w; x++) {
        float t = ((float)x - (float)w * 0.5f) / ((float)w * 0.5f);
        float yv = ycentre + (yedge - ycentre) * (t * t);
        float e = 1.0f - fabsf(t) * 0.35f;
        float yd = g_oy + yv * g_scale;
        int yi = (int)yd;
        float fr = yd - (float)yi;
        put_dev(x, yi, C_LINE, (unsigned)(120 * (1 - fr) * e));
        put_dev(x, yi + 1, C_LINE, (unsigned)(120 * fr * e));
    }
}

static void draw_value(const l1_row *r, int sel, float xv, float y, float rh, float wv)
{
    const char *v = r->value;
    int grey = r->kind == L1V_GREY;
    DWORD col = grey ? (sel ? 0x86A3A7 : C_GREY) : sel ? 0xE4F7FA : C_VAL;
    size_t n = strlen(v);
    const float mk = 34;   /* room of a < or > marker */
    if (n >= 4 && v[0] == '<' && v[1] == ' ' && v[n - 1] == '>' && v[n - 2] == ' ') {
        char mid[160];
        size_t m = n - 4 < sizeof mid - 1 ? n - 4 : sizeof mid - 1;
        memcpy(mid, v + 2, m), mid[m] = 0;
        unsigned mcol = sel ? C_HI : C_HI_DIM;
        text("<", xv, y, mk, rh, 30, 0, mcol, TA_LEFT_, 0, 0);
        text(mid, xv + mk, y, wv - 2 * mk, rh, 28, 1, col, TA_LEFT_, 0.02f, 0);
        float tw = text_w(mid, 28, 1, 0.02f, 0);
        if (tw > wv - 2 * mk) tw = wv - 2 * mk;
        text(">", xv + mk + tw + 8, y, mk, rh, 30, 0, mcol, TA_LEFT_, 0, 0);
    } else if (n)
        text(v, xv + mk, y, wv - mk, rh, 28, 1, col, TA_LEFT_, 0.02f, 0);
}

static void draw_legend(const l1_view *v, float cy)
{
    int pad = v->pads > 0;
    const char *ok = v->leg_ok, *bk = v->leg_back;
    float r = 19, gap = 12, between = 56, wok = text_w(ok, 26, 1, 0.04f, 0), wbk = text_w(bk, 26, 1, 0.04f, 0);
    float iw = pad ? 2 * r : 0, kw1 = 0, kw2 = 0;
    if (!pad) kw1 = text_w("Enter", 20, 0, 0.04f, 0) + 24, kw2 = text_w("Esc", 20, 0, 0.04f, 0) + 24, iw = 0;
    float total = (pad ? 2 * r : kw1) + gap + wok + between + (pad ? 2 * r : kw2) + gap + wbk;
    float x = 960 - total * 0.5f;
    for (int k = 0; k < 2; k++) {
        const char *word = k == 0 ? ok : bk;
        float ww = k == 0 ? wok : wbk;
        if (pad) {
            button_round(x + r, cy, r, k == 0 ? 0x7FD35E : 0xE0584F, k == 0 ? 0x3D8F2E : 0x9C2A25, k == 0 ? "A" : "B");
            x += 2 * r;
        } else {
            float kw = k == 0 ? kw1 : kw2, kh = 34;
            rect_px(x, cy - kh / 2, kw, kh, 0xE02A3338);
            frame_px(x, cy - kh / 2, kw, kh, 2, 0xFF8A969A);
            text(k == 0 ? "Enter" : "Esc", x, cy - kh / 2, kw, kh, 20, 0, 0xFFE0E6E8, TA_CENTER_, 0, 0);
            x += kw;
        }
        x += gap;
        text(word, x, cy - 20, ww + 4, 40, 26, 1, C_TEXT | 0xFF000000, TA_LEFT_, 0.04f, 0);
        x += ww + between;
        (void)iw;
    }
}

static void draw_card(const l1_card *c, float x, float y, float w, float h, unsigned accent)
{
    rect_px(x, y, w, h, 0xB4040E12);
    if (RT.sprites_ok) spr_frame(x, y, w, h, accent, 235);
    else frame_px(x, y, w, h, 1.6f, 0xFF000000 | ((accent >> 1) & 0x7F7F7F) | 0x101818);
    hline_fade(x, x + w, y, 3.0f, accent, 230, 0.0f);
    disc(x + w * 0.5f, y, 5, 0xE0F4F8, 230);
    text(c->who, x + 22, y + 14, w - 44, 30, 20, 0, C_INFO | 0xFF000000, TA_LEFT_, 0.16f, TF_UP);
    text(c->chr, x + 22, y + 48, w - 44, 56, 40, 0, 0xFF000000 | accent, TA_LEFT_, 0.02f, 0);
    text(c->pad, x + 22, y + h - 56, w - 44, 36, 24, 1, C_VAL | 0xFF000000, TA_LEFT_, 0.04f, 0);
}

static void draw_lobby(const ov_scene *sc)
{
    const l1_view *v = &sc->v;
    draw_arc(112, 70);
    g_glow_strength = 0.5f;
    text(v->title, 360, 56, 1200, 92, 58, 0, 0xFF000000 | C_HI, TA_CENTER_, 0.12f, TF_UP | TF_GLOW);
    ornament_line(176, 160, 1760, 960);
    g_glow_strength = 0.5f;
    text(v->subtitle, 380, 198, 1160, 76, 27, 1, C_INFO | 0xFF000000, TA_CENTER_, 0.02f, TF_WRAP);
    float ly = 292, step = v->nrows > 7 ? 54.0f : 60.0f, rh = 52, axis = 960;
    float lx = 400, lw = 1120;
    g_hit_n = 0;
    for (int i = 0; i < v->nrows; i++) {
        const l1_row *r = &v->rows[i];
        int sel = i == v->sel;
        float y = ly + step * (float)i;
        if (sel && RT.sprites_ok) {   /* the stock selection: a soft disc and two thin circles behind the item */
            float cyy = y + rh / 2;
            spr_draw(SPR_DISC, axis - 230, cyy - 62, 460, 124, 0x2FA6B8, 120, 0);
            spr_draw(SPR_RING, axis - 52, cyy - 52, 104, 104, C_HI, 190, 0);
            spr_draw(SPR_RING, axis - 74, cyy - 74, 148, 148, C_HI, 80, 0);
        } else if (sel) {
            glow_ellipse(axis, y + rh / 2, 560, 46, 0x2FA6B8, 150);
            ring_ellipse(axis, y + rh / 2, 520, 31, 1.6f, C_HI, 105);
            ring_ellipse(axis, y + rh / 2, 548, 38, 1.2f, C_HI, 40);
        }
        DWORD col = r->kind == L1V_GREY ? (sel ? 0x86A3A7 : C_GREY) : r->kind == L1V_INFO ? C_INFO : sel ? C_HI : C_ITEM;
        g_glow_strength = 0.6f;
        text(r->label, lx, y, axis - 24 - lx, rh, 31, 0, 0xFF000000 | col, TA_RIGHT_, 0.07f, TF_UP | (sel ? TF_GLOW : 0));
        if (r->value[0]) draw_value(r, sel, axis + 24, y, rh, lx + lw - axis - 24);
        if (g_hit_n < L1_ROWS) g_hit[g_hit_n].x = (int)lx, g_hit[g_hit_n].y = (int)y, g_hit[g_hit_n].w = (int)lw, g_hit[g_hit_n].h = (int)rh, g_hit_n++;
    }
    if (v->cards_on) {
        draw_card(&v->card[0], 60, 292, 300, 176, 0x7FDCE8);
        draw_card(&v->card[1], 1560, 292, 300, 176, 0x74B2E4);
    }
    ornament_line(738, 160, 1760, 960);
    text(v->status, 360, 752, 1200, 62, 30, 1, 0xFF000000 | status_rgb(v->status_kind), TA_CENTER_, 0.02f, TF_WRAP);
    text(v->note, 210, 812, 1500, 62, 24, 1, 0xFF000000 | 0xC2B07C, TA_CENTER_, 0.02f, TF_WRAP);
    text(v->note2, 210, 874, 1500, 62, 24, 1, 0xFF000000 | C_INFO, TA_CENTER_, 0.02f, TF_WRAP);
    text(v->hint, 110, 950, 1700, 32, 23, 1, 0xFF000000 | C_GREY, TA_CENTER_, 0.02f, 0);
    draw_legend(v, 1022);
    if (sc->cursor_on) {   /* a small pointer while the mouse is in use */
        float cx = (float)sc->cursor_x, cy = (float)sc->cursor_y;
        rect_px(cx - 1, cy - 12, 3, 25, 0xFFFFFFFF), rect_px(cx - 12, cy - 1, 25, 3, 0xFFFFFFFF);
        rect_px(cx, cy - 11, 1, 23, 0xFF2AA9BC), rect_px(cx - 11, cy, 23, 1, 0xFF2AA9BC);
    }
}

/* the hint strip: the part of the virtual layout it covers is returned in pixel coordinates */
static void hint_strip_rect(RECT *r)
{
    float x = g_hx, y = g_hy, w = g_hw, h = g_hh;
    r->left = (LONG)(g_ox + x * g_scale), r->top = (LONG)(g_oy + y * g_scale), r->right = (LONG)(g_ox + (x + w) * g_scale + 0.5f), r->bottom = (LONG)(g_oy + (y + h) * g_scale + 0.5f);
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > G.w) r->right = G.w;
    if (r->bottom > G.h) r->bottom = G.h;
}

static void draw_hint(const ov_scene *sc)
{
    float x = g_hx, y = g_hy, w = g_hw, h = g_hh;
    rect_px(x, y, w, h, 0xFF071317);
    frame_px(x, y, w, h, 1.6f, 0xFF2E6A72);
    rect_px(x, y, 5, h, 0xFF000000 | C_HI);
    glow_ellipse(x + w * 0.5f, y + h * 0.5f, w * 0.45f, h * 0.9f, 0x1E6872, 60);
    text(sc->hint, x + 24, y, w - 48, h, 28, 1, 0xFF000000 | C_TEXT, TA_CENTER_, 0.03f, 0);
}

static void compose(const ov_scene *sc)
{
    g_cls = face_class(sc->lang);
    if (sc->kind == OV_LOBBY) {
        if (G.bg) memcpy(G.px, G.bg, (size_t)G.w * (size_t)G.h * 4);
        else
            for (size_t i = 0, n = (size_t)G.w * (size_t)G.h; i < n; i++) G.px[i] = 0xFF050D10u;
        draw_lobby(sc);
    } else if (sc->kind == OV_HINT)
        draw_hint(sc);
}

static void set_metrics(int w, int h)
{
    g_bbw = w, g_bbh = h;
    float sx = (float)w / 1920.0f, sy = (float)h / 1080.0f;
    g_scale = sx < sy ? sx : sy;
    g_ox = ((float)w - 1920.0f * g_scale) * 0.5f;
    g_oy = ((float)h - 1080.0f * g_scale) * 0.5f;
}

int overlay_render_to_bitmap(const ov_scene *sc, int w, int h, unsigned **pixels_out)
{
    gdi_free();
    if (!gdi_make_bitmap(w, h)) {
        gdi_free();
        return 0;
    }
    set_metrics(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)   /* stands for the game picture: the preview picture scaled, else a flat grey */
            G.px[(size_t)y * (size_t)w + (size_t)x] = g_pv_px && g_pv_w > 0 ? g_pv_px[(size_t)(y * g_pv_h / h) * (size_t)g_pv_w + (size_t)(x * g_pv_w / w)] | 0xFF000000u : 0xFF303840u;
    if (sc->kind == OV_LOBBY) {
        G.bg = (unsigned *)malloc((size_t)w * (size_t)h * 4);
        if (G.bg) backdrop_build(g_pv_px ? G.px : NULL, w, h, G.bg);
    }
    compose(sc);
    GdiFlush();
    *pixels_out = G.px;
    return 1;
}

/* the finished game frame -> G.cap (32-bit only); returns 1 when it worked */
static int capture_frame(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb)
{
    if (!G.sys || (G.fmt != D3DFMT_X8R8G8B8 && G.fmt != D3DFMT_A8R8G8B8)) return 0;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, G.sys))) return 0;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(G.sys, &lr, NULL, D3DLOCK_READONLY))) return 0;
    if (!G.cap) G.cap = (unsigned *)malloc((size_t)G.w * (size_t)G.h * 4);
    int ok = G.cap != NULL;
    if (ok)
        for (int y = 0; y < G.h; y++) {
            const unsigned *s = (const unsigned *)((const unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch);
            unsigned *d = G.cap + (size_t)y * (size_t)G.w;
            for (int x = 0; x < G.w; x++) d[x] = s[x] | 0xFF000000u;
        }
    IDirect3DSurface9_UnlockRect(G.sys);
    return ok;
}

/* mean brightness (0..255) of a captured frame, sampled */
static int frame_mean(const unsigned *px, int w, int h)
{
    unsigned long sum = 0, n = 0;
    for (size_t i = 0, total = (size_t)w * (size_t)h; i < total; i += 17, n++) sum += ((px[i] >> 16) & 255) + ((px[i] >> 8) & 255) + (px[i] & 255);
    return n ? (int)(sum / (n * 3)) : 0;
}

/* the backdrop is taken from the game frame when the lobby opens; while that frame is black (a fade) it is taken again every 300 ms, up to 6 shots; the lobby is a still picture */
static int backdrop_update(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, int lobby_on, unsigned now)
{
    if (!lobby_on) {
        G.lobby_was_on = 0;
        return 0;
    }
    int need = 0;
    if (!G.lobby_was_on || !G.bg) G.bg_stamp = now, G.bg_shots = 0, G.bg_final = 0, need = 1;
    else if (!G.bg_final && now - G.bg_stamp > 300u * (unsigned)G.bg_shots) need = 1;
    G.lobby_was_on = 1;
    if (!need) return 0;
    if (!G.bg) G.bg = (unsigned *)malloc((size_t)G.w * (size_t)G.h * 4);
    if (!G.bg) return 0;
    int cap = capture_frame(dev, bb);
    backdrop_build(cap ? G.cap : NULL, G.w, G.h, G.bg);
    if (cap) g_bd_ok++;
    else g_bd_fail++;
    G.bg_shots++;
    G.bg_final = !cap || frame_mean(G.cap, G.w, G.h) >= 5 || G.bg_shots >= 6;
    return 1;
}

static void gdi_frame(IDirect3DDevice9 *dev, const ov_scene *sc, UINT W, UINT H, D3DFORMAT fmt)
{
    if (G.w != (int)W || G.h != (int)H || G.fmt != fmt || !G.dc) {
        gdi_free();
        if (!gdi_make_bitmap((int)W, (int)H)) {
            g_fail_tex++;
            gdi_free();
            return;
        }
        G.fmt = fmt, G.have_sig = 0;
        if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, fmt, D3DPOOL_SYSTEMMEM, &G.sys, NULL)) ||
            FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, fmt, D3DPOOL_DEFAULT, &G.def, NULL))) {
            g_fail_tex++;
            r1_log("overlay: GDI backend: cannot create the staging surfaces (format %d)", (int)fmt);
            gdi_free();
            return;
        }
        r1_log("overlay: GDI backend ready (%ux%u, format %d)", W, H, (int)fmt);
    }
    set_metrics((int)W, (int)H);
    IDirect3DSurface9 *bb = NULL;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    int fresh = backdrop_update(dev, bb, sc->kind == OV_LOBBY, GetTickCount());
    if (fresh) r1_log("overlay: backdrop %s (shot %d)", G.cap && g_bd_ok ? "captured" : "fallback gradient", G.bg_shots);
    /* the scene changes rarely: compose again only then */
    unsigned sig = 2166136261u;
    if (sc->kind == OV_LOBBY) {
        const unsigned char *b = (const unsigned char *)&sc->v;
        for (size_t i = 0; i < sizeof sc->v; i++) sig = (sig ^ b[i]) * 16777619u;
        sig = (sig ^ (unsigned)sc->cursor_on ^ ((unsigned)sc->cursor_x << 3) ^ ((unsigned)sc->cursor_y << 17)) * 16777619u;
        sig = (sig ^ (unsigned)G.bg_shots) * 16777619u;
    } else if (sc->kind == OV_HINT)
        for (const char *p = sc->hint; *p; p++) sig = (sig ^ (unsigned char)*p) * 16777619u;
    sig = (sig ^ (unsigned)sc->kind) * 16777619u, sig = (sig ^ (unsigned)sc->lang) * 16777619u, sig = (sig ^ (unsigned)W) * 16777619u, sig = (sig ^ (unsigned)H) * 16777619u;
    RECT strip = {0, 0, G.w, G.h};
    if (sc->kind == OV_HINT) hint_strip_rect(&strip);
    if (!G.have_sig || sig != G.sig) {
        compose(sc);
        D3DLOCKED_RECT lr;
        RECT lk = strip;
        if (SUCCEEDED(IDirect3DSurface9_LockRect(G.sys, &lr, NULL, 0))) {
            for (int y = lk.top; y < lk.bottom; y++)
                memcpy((unsigned char *)lr.pBits + (size_t)y * (size_t)lr.Pitch + (size_t)lk.left * 4, G.px + (size_t)y * (size_t)G.w + (size_t)lk.left, (size_t)(lk.right - lk.left) * 4);
            IDirect3DSurface9_UnlockRect(G.sys);
            POINT pt = {lk.left, lk.top};
            IDirect3DDevice9_UpdateSurface(dev, G.sys, &lk, G.def, &pt);
            G.sig = sig, G.have_sig = 1;
        }
    }
    {
        HRESULT hr = IDirect3DDevice9_StretchRect(dev, G.def, &strip, bb, &strip, D3DTEXF_NONE);
        if (SUCCEEDED(hr)) g_frames++;
    }
    IDirect3DSurface9_Release(bb);
}

void overlay_draw(void *vdev, const ov_scene *sc)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)vdev;
    if (!dev || !sc || sc->kind == OV_NONE) return;
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    IDirect3DSurface9_GetDesc(bb, &desc);
    IDirect3DSurface9_Release(bb);
    gdi_frame(dev, sc, desc.Width, desc.Height, desc.Format);
}

unsigned overlay_frames(void) { return g_frames; }

int overlay_row_at_client(int px, int py, int cw, int ch, int *vx, int *vy)
{
    if (cw <= 0 || ch <= 0 || g_scale <= 0.0f) return -1;
    float bx = (float)px * (float)g_bbw / (float)cw, by = (float)py * (float)g_bbh / (float)ch;
    int x = (int)((bx - g_ox) / g_scale), y = (int)((by - g_oy) / g_scale);
    if (vx) *vx = x;
    if (vy) *vy = y;
    for (int i = 0; i < g_hit_n; i++)
        if (x >= g_hit[i].x && x < g_hit[i].x + g_hit[i].w && y >= g_hit[i].y && y < g_hit[i].y + g_hit[i].h) return i;
    return -1;
}

void overlay_stats(char *out, int size)
{
    snprintf(out, (size_t)size, "overlay: %u frames copied, failures: font %u surface %u, backdrop captured %u / fallback %u, game material %s (strings: %u real face, %u GDI %s), scale %.2f, back buffer %dx%d", g_frames,
             g_fail_font, g_fail_tex, g_bd_ok, g_bd_fail, RT.ok ? "loaded" : "not available", RT.stat_real, RT.stat_gdi, g_have_bahn > 0 ? "Bahnschrift" : "Segoe UI", g_scale, g_bbw, g_bbh);
}
