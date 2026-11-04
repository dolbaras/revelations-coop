/*
 * Overlay of the Revelations 1 co-op lobby, see overlay.h. GDI composition + UpdateSurface + StretchRect (the technique of the Revelations 2 mod).
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "overlay.h"
#include "r1.h"

static float g_scale = 1.0f, g_ox = 0.0f, g_oy = 0.0f;
static int g_bbw = 1920, g_bbh = 1080;
static unsigned g_frames, g_fail_font, g_fail_tex;
static int g_cls;

static struct { int x, y, w, h; } g_hit[L1_ROWS];
static int g_hit_n;

static int face_class(int lang) { return lang == 5 ? 1 : lang == 11 ? 2 : lang == 12 ? 3 : 0; }
static const wchar_t *face_name(int cls)
{
    switch (cls) {
    case 1: return L"Yu Gothic UI";
    case 2: return L"Microsoft YaHei UI";
    case 3: return L"Microsoft JhengHei UI";
    default: return L"Segoe UI";
    }
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
    struct { int px, bold, cls; HFONT f; } fonts[16];
} G;

static void gdi_free(void)
{
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
        if (G.fonts[i].f) DeleteObject(G.fonts[i].f);
    if (G.sys) IDirect3DSurface9_Release(G.sys);
    if (G.def) IDirect3DSurface9_Release(G.def);
    if (G.dc) DeleteDC(G.dc);
    if (G.bmp) DeleteObject(G.bmp);
    memset(&G, 0, sizeof G);
}
void overlay_release(void) { gdi_free(); }

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

static void rect_px(float x, float y, float w, float h, DWORD argb)
{
    int x0 = (int)(g_ox + x * g_scale), y0 = (int)(g_oy + y * g_scale), x1 = (int)(g_ox + (x + w) * g_scale + 0.5f), y1 = (int)(g_oy + (y + h) * g_scale + 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G.w) x1 = G.w;
    if (y1 > G.h) y1 = G.h;
    unsigned a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    for (int yy = y0; yy < y1; yy++) {
        unsigned *row = G.px + (size_t)yy * (size_t)G.w;
        for (int xx = x0; xx < x1; xx++) {
            if (a >= 255) row[xx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            else {
                unsigned d = row[xx];
                unsigned dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
                row[xx] = 0xFF000000u | (((r * a + dr * (255 - a)) / 255) << 16) | (((g * a + dg * (255 - a)) / 255) << 8) | ((b * a + db * (255 - a)) / 255);
            }
        }
    }
}

static void frame_px(float x, float y, float w, float h, float t, DWORD argb)
{
    rect_px(x, y, w, t, argb), rect_px(x, y + h - t, w, t, argb), rect_px(x, y, t, h, argb), rect_px(x + w - t, y, t, h, argb);
}

static HFONT gdi_font(int px, int bold, int cls)
{
    unsigned free_slot = (unsigned)-1;
    for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++) {
        if (G.fonts[i].f && G.fonts[i].px == px && G.fonts[i].bold == bold && G.fonts[i].cls == cls) return G.fonts[i].f;
        if (!G.fonts[i].f && free_slot == (unsigned)-1) free_slot = i;
    }
    if (free_slot == (unsigned)-1) {
        for (unsigned i = 0; i < sizeof G.fonts / sizeof G.fonts[0]; i++)
            if (G.fonts[i].f) DeleteObject(G.fonts[i].f), G.fonts[i].f = NULL;
        free_slot = 0;
    }
    HFONT f = CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         face_name(cls));
    if (!f) {
        g_fail_font++;
        return NULL;
    }
    G.fonts[free_slot].px = px, G.fonts[free_slot].bold = bold, G.fonts[free_slot].cls = cls, G.fonts[free_slot].f = f;
    return f;
}

/* text in a box (virtual coordinates); wrap = 1: several lines from the top of the box, else one line centred vertically */
static void text(const char *s, float x, float y, float w, float h, int px, int bold, DWORD color, DWORD fmt, int wrap)
{
    if (!s || !s[0]) return;
    wchar_t ws[600];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 599);
    if (n <= 0) return;
    HFONT f = gdi_font((int)(px * g_scale + 0.5f), bold, g_cls);
    if (!f) return;
    HGDIOBJ old = SelectObject(G.dc, f);
    SetBkMode(G.dc, TRANSPARENT);
    SetTextColor(G.dc, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255));
    RECT rc = {(LONG)(g_ox + x * g_scale), (LONG)(g_oy + y * g_scale), (LONG)(g_ox + (x + w) * g_scale), (LONG)(g_oy + (y + h) * g_scale)};
    DrawTextW(G.dc, ws, -1, &rc, fmt | DT_NOPREFIX | (wrap ? DT_WORDBREAK : (DT_SINGLELINE | DT_VCENTER | DT_NOCLIP)));
    SelectObject(G.dc, old);
}

#define COL_TEXT 0xFFE8ECEF
#define COL_GREY 0xFF6A7178
#define COL_INFO 0xFFB8C2CC
#define COL_ACCENT 0xFFC0392B
#define COL_SEL 0xCCA82020

static DWORD status_color(int kind) { return kind == L1S_WARN ? 0xFFFFC857 : kind == L1S_OK ? 0xFF86E394 : 0xFFD6DDE3; }

static void draw_lobby(const ov_scene *sc)
{
    const l1_view *v = &sc->v;
    rect_px(-400, -100, 2720, 1280, 0xFF090B0E);   /* opaque, covers the letterbox bars too */
    text(v->title, 160, 60, 1600, 90, 56, 1, 0xFFFFFFFF, DT_LEFT, 0);
    rect_px(160, 154, 560, 4, COL_ACCENT);
    text(v->subtitle, 160, 176, 1600, 80, 26, 0, COL_INFO, DT_LEFT, 1);
    float lx = 160, ly = 276, lw = 1180, rh = 58, step = 68;
    g_hit_n = 0;
    for (int i = 0; i < v->nrows; i++) {
        const l1_row *r = &v->rows[i];
        int sel = i == v->sel;
        float y = ly + step * (float)i;
        if (sel) {
            rect_px(lx, y, lw, rh, COL_SEL);
            rect_px(lx, y, 6, rh, 0xFFFF4A3A);
        } else
            rect_px(lx, y, lw, rh, 0xFF14181C);
        DWORD col = r->kind == L1V_GREY ? COL_GREY : r->kind == L1V_INFO ? COL_INFO : COL_TEXT;
        text(r->label, lx + 28, y, lw * 0.36f, rh, 30, sel, col, DT_LEFT, 0);
        if (r->value[0]) text(r->value, lx + lw * 0.34f, y, lw * 0.66f - 24, rh, 27, 0, r->kind == L1V_GREY ? COL_GREY : (sel ? 0xFFFFFFFF : COL_INFO), DT_RIGHT, 0);
        if (g_hit_n < L1_ROWS) g_hit[g_hit_n].x = (int)lx, g_hit[g_hit_n].y = (int)y, g_hit[g_hit_n].w = (int)lw, g_hit[g_hit_n].h = (int)rh, g_hit_n++;
    }
    if (v->cards_on) {
        for (int c = 0; c < 2; c++) {
            float cx = 1400, cy = 276 + 184.0f * (float)c, cw = 360, ch = 168;
            rect_px(cx, cy, cw, ch, 0xFF101418);
            frame_px(cx, cy, cw, ch, 2, c == 0 ? 0xFFC0392B : 0xFF3B7DD8);
            rect_px(cx, cy, cw, 6, c == 0 ? 0xFFC0392B : 0xFF3B7DD8);
            text(v->card[c].who, cx + 24, cy + 14, cw - 48, 34, 24, 0, COL_INFO, DT_LEFT, 0);
            text(v->card[c].chr, cx + 24, cy + 50, cw - 48, 64, 46, 1, 0xFFFFFFFF, DT_LEFT, 0);
            text(v->card[c].pad, cx + 24, cy + 114, cw - 48, 36, 24, 0, COL_TEXT, DT_LEFT, 0);
        }
    }
    text(v->status, 160, 772, 1600, 84, 28, 0, status_color(v->status_kind), DT_LEFT, 1);
    text(v->note, 160, 890, 1600, 60, 22, 0, 0xFFC9A66B, DT_LEFT, 1);
    text(v->note2, 160, 952, 1600, 56, 22, 0, 0xFF8C96A0, DT_LEFT, 1);
    text(v->hint, 160, 1016, 1640, 40, 22, 0, 0xFF7E8890, DT_LEFT, 0);
}

/* the hint strip: the part of the virtual layout it covers is returned in pixel coordinates */
static void hint_strip_rect(RECT *r)
{
    float x = 260, y = 980, w = 1400, h = 70;
    r->left = (LONG)(g_ox + x * g_scale), r->top = (LONG)(g_oy + y * g_scale), r->right = (LONG)(g_ox + (x + w) * g_scale + 0.5f), r->bottom = (LONG)(g_oy + (y + h) * g_scale + 0.5f);
    if (r->left < 0) r->left = 0;
    if (r->top < 0) r->top = 0;
    if (r->right > G.w) r->right = G.w;
    if (r->bottom > G.h) r->bottom = G.h;
}

static void draw_hint(const ov_scene *sc)
{
    float x = 260, y = 980, w = 1400, h = 70;
    rect_px(x, y, w, h, 0xFF0C0F13);
    frame_px(x, y, w, h, 2, 0xFF3A424A);
    rect_px(x, y, 6, h, COL_ACCENT);
    text(sc->hint, x + 24, y, w - 48, h, 28, 0, 0xFFE8ECEF, DT_CENTER, 0);
}

static void compose(const ov_scene *sc)
{
    g_cls = face_class(sc->lang);
    if (sc->kind == OV_LOBBY) {
        for (size_t i = 0, n = (size_t)G.w * (size_t)G.h; i < n; i++) G.px[i] = 0xFF090B0Eu;
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
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) G.px[i] = 0xFF303840u;   /* stands for the game picture behind a hint strip */
    compose(sc);
    GdiFlush();
    *pixels_out = G.px;
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
    /* the scene changes rarely: compose again only then */
    unsigned sig = 2166136261u;
    if (sc->kind == OV_LOBBY) {
        const unsigned char *b = (const unsigned char *)&sc->v;
        for (size_t i = 0; i < sizeof sc->v; i++) sig = (sig ^ b[i]) * 16777619u;
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
    IDirect3DSurface9 *bb = NULL;
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        HRESULT hr = IDirect3DDevice9_StretchRect(dev, G.def, &strip, bb, &strip, D3DTEXF_NONE);
        if (SUCCEEDED(hr)) g_frames++;
        IDirect3DSurface9_Release(bb);
    }
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
    snprintf(out, (size_t)size, "overlay: %u frames copied, failures: font %u surface %u, scale %.2f, back buffer %dx%d", g_frames, g_fail_font, g_fail_tex, g_scale, g_bbw, g_bbh);
}
