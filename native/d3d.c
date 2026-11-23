/*
 * Direct3D 9 hooks: device creation (adapter, windowed for test instances), Reset, Present, screenshots.
 *
 * The exe imports d3d9.dll!Direct3DCreate9 by name, so the hook goes through its import slot; everything after that is COM
 * vtables (IDirect3D9::CreateDevice slot 16, IDirect3DDevice9::Reset 16, Present 17). Presents are also the "the stub is long
 * done, the game renders" marker: the first one is logged with the .text state.
 *
 *   REV1_SHOTDIR=<dir>  where screenshots go (default: rev1_coop\shots next to the exe)
 *   REV1_SHOTS=s1,s2    also save a frame this many seconds after the device was created
 *   F11 (game window in front) or the console command "shot": save the next presented frame as shotNNNN_SSS.bmp
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "r1.h"
#include "devreset.h"
#include "flash2.h"

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Direct3DCreate9_t real_Direct3DCreate9;
static CreateDevice_t real_CreateDevice;
static Present_t real_Present;
static Reset_t real_Reset;

/* test hook (console `devreset <n>`, test instances only): the next n Reset calls answer DEVICELOST without the device
   being lost, so the wait in hook_Reset can be run for real. Requested here, run by hook_Present on the render thread. */
static LONG devreset_inject, devreset_test_pending;
static D3DPRESENT_PARAMETERS dr_pp;
/* fix026f: the second human's flashlight is written into the light while his viewport draws (flash2.c) */
typedef HRESULT (WINAPI *SetViewport_t)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
static SetViewport_t real_SetViewport;
static HRESULT WINAPI hook_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v)
{
    if (v)
        flash2_viewport(v->X, v->Y, v->Width, v->Height, dr_pp.BackBufferWidth, dr_pp.BackBufferHeight);
    return real_SetViewport(d, v);
}   /* the last parameters of CreateDevice / Reset, for the test Reset */
static HRESULT do_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (devreset_inject > 0) {
        devreset_inject--;
        return (HRESULT)DEVRESET_LOST;
    }
    return real_Reset(dev, pp);
}
void d3d_devreset_request(int n) { InterlockedExchange(&devreset_test_pending, n); }

static char shot_dir[MAX_PATH];
static int shot_secs[32], shot_count, shot_next, shot_serial;
static DWORD t0;
static volatile LONG shot_asked;
static volatile unsigned frames;

unsigned d3d_frames(void) { return frames; }
void d3d_request_shot(void) { InterlockedExchange(&shot_asked, 1); }

static void save_frame(IDirect3DDevice9 *dev)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
        r1_log("shot: back buffer format %d not supported", d.Format);
        goto done;
    }
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr) && FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
        /* (a multisampled back buffer: resolve through a plain render target) */
        IDirect3DSurface9 *rt = NULL;
        hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, rt, NULL, D3DTEXF_NONE);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
        if (rt)
            IDirect3DSurface9_Release(rt);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        r1_log("shot: copy of the back buffer failed (hr=0x%08lx, %ux%u format %d multisample %d)", (unsigned long)hr, d.Width, d.Height,
               d.Format, d.MultiSampleType);
        goto done;
    }
    char path[MAX_PATH + 40];
    /* (the folders are made by the first screenshot, not at load: a player who never takes one gets no rev1_coop\ in the game folder;
     * the uninstaller removes them only while empty) */
    snprintf(path, sizeof path, "%s", shot_dir);
    char *up = strrchr(path, '\\');
    if (up && up > path) {
        *up = 0;
        CreateDirectoryA(path, NULL);
    }
    CreateDirectoryA(shot_dir, NULL);
    snprintf(path, sizeof path, "%s\\shot%04d_%03lu.bmp", shot_dir, ++shot_serial, (GetTickCount() - t0) / 1000);
    FILE *f = fopen(path, "wb");
    if (f) {
        DWORD row = d.Width * 4, size = row * d.Height;
        BITMAPFILEHEADER fh = {0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
        BITMAPINFOHEADER ih = {sizeof ih, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB, size, 0, 0, 0, 0};
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        for (UINT y = 0; y < d.Height; y++)
            fwrite((BYTE *)lr.pBits + y * lr.Pitch, row, 1, f);
        fclose(f);
        r1_log("shot: %s (%ux%u)", path, d.Width, d.Height);
    } else
        r1_log("shot: cannot write %s", path);
    IDirect3DSurface9_UnlockRect(sys);
done:
    if (sys)
        IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp);

/* ====================================================================== fix026h LAB: one-frame device call trace (console `d3dtrace [frames]`) ======================================================================
 * Why: the split screen's HUD is drawn into each half's viewport and looks stretched (top/bottom: wide, side by side: narrow). To fix it at the right place the calls the GUI makes
 * must be seen: viewports, vertex shader constants, render targets, draws. The vtable slots are patched only when the command is first used; the trace is written to
 * <log>.d3d.txt by the render thread (Present) for the asked number of frames (1..3). Nothing runs unless the command is issued. */
enum { TR_VIEWPORT, TR_VSCONST, TR_RT, TR_DRAW, TR_CLEAR, TR_SCISSOR, TR_VS, TR_PS, TR_TRANSFORM, TR_PSCONST, TR_TEXTURE };
typedef struct { unsigned char type; unsigned char n; unsigned short a; unsigned b, c; float f[8]; } tr_ev;
#define TR_MAX 40000
static tr_ev tr_buf[TR_MAX];
static volatile LONG tr_n, tr_frames_left, tr_hooked;
static IDirect3DDevice9 *tr_dev;
static void *tr_real[16];
typedef HRESULT(WINAPI *f_vp)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);
typedef HRESULT(WINAPI *f_vsc)(IDirect3DDevice9 *, UINT, const float *, UINT);
typedef HRESULT(WINAPI *f_rt)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
typedef HRESULT(WINAPI *f_clear)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(WINAPI *f_sc)(IDirect3DDevice9 *, const RECT *);
typedef HRESULT(WINAPI *f_dp)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(WINAPI *f_dip)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(WINAPI *f_dpup)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, const void *, UINT);
typedef HRESULT(WINAPI *f_dipup)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT);
typedef HRESULT(WINAPI *f_vs)(IDirect3DDevice9 *, void *);
typedef HRESULT(WINAPI *f_tf)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
static tr_ev *tr_get(int type)
{
    if (tr_frames_left <= 0)
        return NULL;
    LONG i = InterlockedIncrement(&tr_n) - 1;
    if (i >= TR_MAX)
        return NULL;
    tr_ev *e = &tr_buf[i];
    memset(e, 0, sizeof *e);
    e->type = (unsigned char)type;
    return e;
}
static HRESULT WINAPI tr_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v)
{
    tr_ev *e = tr_get(TR_VIEWPORT);
    if (e && v) { e->f[0] = (float)v->X; e->f[1] = (float)v->Y; e->f[2] = (float)v->Width; e->f[3] = (float)v->Height; e->f[4] = v->MinZ; e->f[5] = v->MaxZ; }
    return ((f_vp)tr_real[0])(d, v);
}
static HRESULT WINAPI tr_SetVSC(IDirect3DDevice9 *d, UINT s, const float *p, UINT c)
{
    tr_ev *e = s == 10 ? NULL : tr_get(TR_VSCONST);   /* (register 10 is logged by hf_SetVSC with both the original and the sent values) */
    if (e && p) { e->a = (unsigned short)s; e->b = c; for (int i = 0; i < 8 && i < (int)c * 4; i++) e->f[i] = p[i]; }
    return ((f_vsc)tr_real[1])(d, s, p, c);
}
static HRESULT WINAPI tr_SetPSC(IDirect3DDevice9 *d, UINT s, const float *p, UINT c)
{
    tr_ev *e = tr_get(TR_PSCONST);
    if (e && p) { e->a = (unsigned short)s; e->b = c; for (int i = 0; i < 8 && i < (int)c * 4; i++) e->f[i] = p[i]; }
    return ((f_vsc)tr_real[2])(d, s, p, c);
}
static HRESULT WINAPI tr_SetRT(IDirect3DDevice9 *d, DWORD i, IDirect3DSurface9 *s)
{
    tr_ev *e = tr_get(TR_RT);
    if (e) {
        e->a = (unsigned short)i; e->b = (unsigned)(size_t)s;
        D3DSURFACE_DESC sd;
        if (s && SUCCEEDED(IDirect3DSurface9_GetDesc(s, &sd))) { e->f[0] = (float)sd.Width; e->f[1] = (float)sd.Height; e->f[2] = (float)sd.Format; }
    }
    return ((f_rt)tr_real[3])(d, i, s);
}
static HRESULT WINAPI tr_Clear(IDirect3DDevice9 *d, DWORD n, const D3DRECT *r, DWORD fl, D3DCOLOR c, float z, DWORD st)
{
    tr_ev *e = tr_get(TR_CLEAR);
    if (e) { e->b = fl; e->c = c; e->a = (unsigned short)n; if (r && n) { e->f[0] = (float)r->x1; e->f[1] = (float)r->y1; e->f[2] = (float)r->x2; e->f[3] = (float)r->y2; } }
    return ((f_clear)tr_real[4])(d, n, r, fl, c, z, st);
}
static HRESULT WINAPI tr_Scissor(IDirect3DDevice9 *d, const RECT *r)
{
    tr_ev *e = tr_get(TR_SCISSOR);
    if (e && r) { e->f[0] = (float)r->left; e->f[1] = (float)r->top; e->f[2] = (float)r->right; e->f[3] = (float)r->bottom; }
    return ((f_sc)tr_real[5])(d, r);
}
static HRESULT WINAPI tr_DP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT s, UINT c)
{
    tr_ev *e = tr_get(TR_DRAW);
    if (e) { e->a = 0; e->b = t; e->c = c; }
    return ((f_dp)tr_real[6])(d, t, s, c);
}
static HRESULT WINAPI tr_DIP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT pc)
{
    tr_ev *e = tr_get(TR_DRAW);
    if (e) { e->a = 1; e->b = t; e->c = pc; e->f[0] = (float)nv; }
    return ((f_dip)tr_real[7])(d, t, bv, mi, nv, si, pc);
}
static HRESULT WINAPI tr_DPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT c, const void *p, UINT st)
{
    tr_ev *e = tr_get(TR_DRAW);
    if (e) { e->a = 2; e->b = t; e->c = c; e->f[0] = (float)st; if (p && st >= 12) { const float *v = p; e->f[1] = v[0]; e->f[2] = v[1]; e->f[3] = v[2]; } }
    return ((f_dpup)tr_real[8])(d, t, c, p, st);
}
static HRESULT WINAPI tr_DIPUP(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *ix, D3DFORMAT ixf, const void *vd, UINT st)
{
    tr_ev *e = tr_get(TR_DRAW);
    if (e) { e->a = 3; e->b = t; e->c = pc; e->f[0] = (float)st; if (vd && st >= 12) { const float *v = vd; e->f[1] = v[0]; e->f[2] = v[1]; e->f[3] = v[2]; } }
    return ((f_dipup)tr_real[9])(d, t, mi, nv, pc, ix, ixf, vd, st);
}
static HRESULT WINAPI tr_SetVS(IDirect3DDevice9 *d, void *s)
{
    tr_ev *e = tr_get(TR_VS);
    if (e) e->b = (unsigned)(size_t)s;
    return ((f_vs)tr_real[10])(d, s);
}
static HRESULT WINAPI tr_SetPS(IDirect3DDevice9 *d, void *s)
{
    tr_ev *e = tr_get(TR_PS);
    if (e) e->b = (unsigned)(size_t)s;
    return ((f_vs)tr_real[11])(d, s);
}
static HRESULT WINAPI tr_SetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    tr_ev *e = tr_get(TR_TRANSFORM);
    if (e && m) { e->a = (unsigned short)t; for (int i = 0; i < 8; i++) e->f[i] = ((const float *)m)[i]; }
    return ((f_tf)tr_real[12])(d, t, m);
}
static void tr_hook_all(IDirect3DDevice9 *dev)
{
    if (InterlockedExchange(&tr_hooked, 1))
        return;
    static const struct { int slot; void *fn; } tab[] = {{47, tr_SetViewport}, {94, tr_SetVSC}, {109, tr_SetPSC}, {37, tr_SetRT}, {43, tr_Clear}, {75, tr_Scissor}, {81, tr_DP}, {82, tr_DIP},
                                                          {83, tr_DPUP}, {84, tr_DIPUP}, {92, tr_SetVS}, {107, tr_SetPS}, {44, tr_SetTransform}};
    for (int i = 0; i < 13; i++)
        tr_real[i] = r1_patch_vtable(dev, tab[i].slot, tab[i].fn);
    r1_log("d3dtrace: %d device slots patched", 13);
}
void d3d_trace_request(int frames)
{
    if (!tr_dev) { r1_log("d3dtrace: no device yet"); return; }
    tr_hook_all(tr_dev);
    InterlockedExchange(&tr_n, 0);
    InterlockedExchange(&tr_frames_left, frames < 1 ? 1 : frames > 3 ? 3 : frames);
}
static void tr_flush(void)
{
    static const char *nm[] = {"VIEWPORT", "VSC", "RT", "DRAW", "CLEAR", "SCISSOR", "VS", "PS", "XFORM", "PSC", "TEX"};
    char path[MAX_PATH + 16], lp[MAX_PATH];
    if (!r1_env("REV1_LOG", lp, sizeof lp))
        strcpy(lp, "rev1_coop.log");
    snprintf(path, sizeof path, "%s.d3d.txt", lp);
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    LONG n = tr_n < TR_MAX ? tr_n : TR_MAX;
    fprintf(f, "=== frame trace %ld events (tick %lu)\n", (long)n, (unsigned long)GetTickCount());
    for (LONG i = 0; i < n; i++) {
        tr_ev *e = &tr_buf[i];
        fprintf(f, "%s a=%u b=%08x c=%08x f=%.4g %.4g %.4g %.4g %.4g %.4g %.4g %.4g\n", nm[e->type], e->a, e->b, e->c, e->f[0], e->f[1], e->f[2], e->f[3], e->f[4], e->f[5], e->f[6], e->f[7]);
    }
    fclose(f);
    r1_log("d3dtrace: %ld events written to %s", (long)n, path);
}

static HRESULT WINAPI hook_Present(IDirect3DDevice9 *dev, const RECT *s, const RECT *d, HWND w, const RGNDATA *r)
{
    unsigned n = ++frames;
    if (tr_frames_left > 0) {   /* fix026h trace: this Present ends a traced frame */
        if (InterlockedDecrement(&tr_frames_left) == 0)
            tr_flush();
    }
    LONG tn = InterlockedExchange(&devreset_test_pending, 0);
    if (tn > 0) {   /* `devreset <n>` (test): a Reset on this (render) thread with n injected lost answers */
        D3DPRESENT_PARAMETERS tp = dr_pp;
        devreset_inject = tn;
        HRESULT th = hook_Reset(dev, &tp);
        r1_log("devreset test: %ld injected lost answers, Reset returned 0x%08lx, %ld injections left", (long)tn, (unsigned long)th, (long)devreset_inject);
        devreset_inject = 0;
    }
    if (n == 1) {
        char st[160];
        r1_text_state(st, sizeof st);
        r1_log("first Present (the game renders): %s", st);
        r1_iat_check("first Present");
        lt_install();
        probe_install();
    }
    menu_present(dev);   /* the co-op lobby / title hint (nothing happens unless the menu module published a scene); BEFORE the screenshots so that they show it */
    /* F11: only while the game window is really in front (never reacts to keys meant for another window) */
    static int f11_was;
    HWND gw = testmode_window();
    int f11 = (GetAsyncKeyState(VK_F11) & 0x8000) && gw && GetForegroundWindow() == gw;
    if (f11 && !f11_was)
        shot_asked = 1;
    f11_was = f11;
    if (shot_asked && InterlockedExchange(&shot_asked, 0))
        save_frame(dev);
    if (shot_next < shot_count && GetTickCount() - t0 >= (DWORD)shot_secs[shot_next] * 1000) {
        shot_next++;
        save_frame(dev);
    }
    return real_Present(dev, s, d, w, r);
}

static void log_pp(const char *what, const D3DPRESENT_PARAMETERS *pp)
{
    if (!pp)
        return;
    r1_log("%s: %ux%u fmt=%d buffers=%u msaa=%d/%lu swap=%d hwnd=%p windowed=%d depth=%d/%d flags=0x%lx refresh=%u interval=0x%x thread %lu", what, pp->BackBufferWidth,
           pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->MultiSampleType, pp->MultiSampleQuality, pp->SwapEffect, (void *)pp->hDeviceWindow, pp->Windowed,
           pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval, GetCurrentThreadId());
}

/* device-lost recovery (devreset.h): the game shows a fatal error box at the first lost Reset, so the wait is here */
#define DEVRESET_BUDGET_MS 20000u
#define DEVRESET_POLL_MS   100u
struct devreset_ctx {
    IDirect3DDevice9 *dev;
    D3DPRESENT_PARAMETERS *pp;
};
static long devreset_coop(void *c) { return (long)IDirect3DDevice9_TestCooperativeLevel(((struct devreset_ctx *)c)->dev); }
static long devreset_reset(void *c)
{
    struct devreset_ctx *x = c;
    return (long)do_reset(x->dev, x->pp);
}
static void devreset_sleep(void *c, unsigned ms) { (void)c; Sleep(ms); }
static unsigned devreset_now(void *c) { (void)c; return (unsigned)GetTickCount(); }

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    log_pp("device Reset asked", pp);
    menu_before_reset();
    if (testmode_force_windowed() && pp) {
        pp->Windowed = TRUE;                   /* a test instance never goes exclusive full screen; windowed needs refresh 0 */
        pp->FullScreen_RefreshRateInHz = 0;
    }
    if (pp)
        dr_pp = *pp;
    HRESULT hr = do_reset(dev, pp);
    r1_log("device Reset %ux%u windowed=%d -> hr=0x%08lx", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1,
           (unsigned long)hr);
    if (devreset_is_lost((long)hr)) {
        struct devreset_ctx x = {dev, pp};
        unsigned tries = 0;
        r1_log("device lost on Reset (hr=0x%08lx): waiting up to %u s for TestCooperativeLevel, then Reset again", (unsigned long)hr,
               DEVRESET_BUDGET_MS / 1000u);
        hr = (HRESULT)devreset_wait((long)hr, devreset_coop, devreset_reset, devreset_sleep, devreset_now, &x,
                                    DEVRESET_BUDGET_MS, DEVRESET_POLL_MS, &tries);
        r1_log("device Reset after the wait: %u more Reset call(s) -> hr=0x%08lx%s", tries, (unsigned long)hr,
               devreset_is_lost((long)hr) ? " (still lost: the game will show its error)" : " (no longer lost: returned to the game)");
    }
    return hr;
}


/* ====================================================================== fix026h: the HUD of a split half keeps its proportions ======================================================================
 * Cause of the "stretched" HUD (live trace 2026-11-22, notes\split-hud-stretch.md): every GUI unit (cockpit weapon box and numbers, minimap, objective and help text,
 * messages) is drawn in the viewport pass of its half with the GUI projection constant (vertex shader register 10: x = 2 / screenW, y = -2 / screenH, then the offset -1 / +1)
 * that maps the WHOLE 16:9 screen onto the current D3D viewport. A half is 2:1 wide (top / bottom) or 1:2 tall (side by side), so the whole GUI is squeezed to half its height or
 * half its width: wide, flat boxes and text on top / bottom, narrow tall ones side by side. The world is not affected (the engine sets each camera's aspect from its rect).
 * Fix: while the split runs and the current viewport is not the GUI's aspect, the two scales and the two offsets of that constant are multiplied by the same factors per axis
 * (Ax = screenW * k / vpW, Ay = screenH * k / vpH, k = the largest uniform scale that fits): the 16:9 HUD is drawn in a centred 16:9 box of the half (centre of the half = centre
 * of the GUI, so the reticle stays in the middle of each view). Whole view (pause, cutscene, solo) is never touched: the aspects are equal there.
 * Switch: console `split hudfit 0|1` (default 1), env REV1_HUDFIT=0 at start. The device slots SetViewport (47) and SetVertexShaderConstantF (94) are patched once at device creation. */
volatile LONG d3d_hudfit_on = 1;
static volatile LONG hf_fixed;     /* constants changed so far (status) */
static D3DVIEWPORT9 hf_vp;
static volatile LONG hf_have;
static float hf_gui_w, hf_gui_h;   /* the whole screen's GUI size seen last in a GUI projection (2 / x scale, -2 / y scale) */
static void *hf_real_vp, *hf_real_vsc;
int split_is_active(void);
static HRESULT WINAPI hf_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v)
{
    if (v) {
        hf_vp = *v;
        hf_have = 1;
    }
    return ((HRESULT(WINAPI *)(IDirect3DDevice9 *, const D3DVIEWPORT9 *))hf_real_vp)(d, v);
}
static HRESULT WINAPI hf_SetVSC(IDirect3DDevice9 *d, UINT s, const float *p, UINT c)
{
    float f[4];
    if (tr_frames_left > 0 && s == 10) {   /* lab trace (d3dtrace): what the device really gets for register 10: original in f[0..3], sent in f[4..7], n = 1 when changed */
        tr_ev *e = tr_get(TR_VSCONST);
        if (e && p) {
            e->a = (unsigned short)s;
            e->b = c;
            for (int i = 0; i < 4 && i < (int)c * 4; i++)
                e->f[i] = e->f[4 + i] = p[i];
            e->n = 2;
        }
    }
    if (s == 10 && c == 1 && p && d3d_hudfit_on && hf_have && p[0] > 0.0f && p[1] < 0.0f) {
        float W = 2.0f / p[0], H = -2.0f / p[1], vw = (float)hf_vp.Width, vh = (float)hf_vp.Height;
        float asp = W / H;
        int gui = W >= 320.0f && H >= 180.0f && asp > 1.75f && asp < 1.80f;   /* the whole screen's GUI space (every GUI unit) */
        /* the minimap icons (uMapIcon) are drawn with a projection of the VIEWPORT's pixel space (W = vpW, H = vpH, positions computed with the half's size cell of 0.2.x): the same
         * fit, taken with the whole screen's size remembered from the GUI draws, lands them on the map */
        int icon = !gui && hf_gui_w > 0.0f && fabsf(W - vw) < 1.5f && fabsf(H - vh) < 1.5f;
        if (gui) {
            hf_gui_w = W;
            hf_gui_h = H;
        } else if (icon) {
            W = hf_gui_w;
            H = hf_gui_h;
        }
        if ((gui || icon) && vw >= 64.0f && vh >= 64.0f && vw <= W + 1.0f && vh <= H + 1.0f &&
            (vw * H - vh * W > 0.01f * vw * H || vh * W - vw * H > 0.01f * vw * H) && split_is_active()) {
            float k = vw / W < vh / H ? vw / W : vh / H;
            float ax = W * k / vw, ay = H * k / vh;
            f[0] = p[0] * ax;
            f[1] = p[1] * ay;
            f[2] = p[2] * ax;
            f[3] = p[3] * ay;
            InterlockedIncrement(&hf_fixed);
            if (tr_frames_left > 0) {
                tr_ev *e = tr_get(TR_VSCONST);
                if (e) { e->a = 10; e->b = 1; e->n = 1; for (int i = 0; i < 4; i++) { e->f[i] = p[i]; e->f[4 + i] = f[i]; } }
            }
            return ((HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, const float *, UINT))hf_real_vsc)(d, s, f, c);
        }
    }
    return ((HRESULT(WINAPI *)(IDirect3DDevice9 *, UINT, const float *, UINT))hf_real_vsc)(d, s, p, c);
}
static void hf_install(IDirect3DDevice9 *dev)
{
    char e[8];
    if (r1_env("REV1_HUDFIT", e, sizeof e) && e[0] == '0')
        d3d_hudfit_on = 0;
    hf_real_vp = r1_patch_vtable(dev, 47, (void *)hf_SetViewport);
    hf_real_vsc = r1_patch_vtable(dev, 94, (void *)hf_SetVSC);
    r1_log("hudfit: SetViewport %s, SetVertexShaderConstantF %s, %s", hf_real_vp ? "hooked" : "NOT hooked", hf_real_vsc ? "hooked" : "NOT hooked",
           d3d_hudfit_on ? "on" : "off (REV1_HUDFIT=0)");
    if (!hf_real_vp || !hf_real_vsc)
        d3d_hudfit_on = 0;
}
int d3d_hudfit_is_on(void) { return d3d_hudfit_on != 0; }
void d3d_hudfit_set(int on) { InterlockedExchange(&d3d_hudfit_on, on != 0 && hf_real_vp && hf_real_vsc); }
unsigned d3d_hudfit_count(void) { return (unsigned)hf_fixed; }

static HRESULT WINAPI hook_CreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                        IDirect3DDevice9 **out)
{
    UINT use = (UINT)testmode_adapter_pick(d3d, focus, adapter);
    int was_full = pp && !pp->Windowed;
    log_pp("CreateDevice asked", pp);
    if (testmode_force_windowed() && pp) {   /* a test instance never goes exclusive full screen */
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    D3DADAPTER_IDENTIFIER9 id;
    memset(&id, 0, sizeof id);
    IDirect3D9_GetAdapterIdentifier(d3d, use, 0, &id);
    r1_log("CreateDevice asked adapter %u, using %u (%s, vendor 0x%04lx device 0x%04lx) %ux%u fmt=%d windowed=%d (was %s) buffers=%u msaa=%d flags=0x%lx", adapter, use,
           id.Description, (unsigned long)id.VendorId, (unsigned long)id.DeviceId, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->BackBufferFormat : 0, pp ? pp->Windowed : -1,
           was_full ? "full screen" : "windowed", pp ? pp->BackBufferCount : 0, pp ? pp->MultiSampleType : 0, flags);
    if (testmode_need_vendor() && id.VendorId != testmode_need_vendor()) {   /* REV1_VENDOR: never render on another GPU (the owner's discrete card is for his own game) */
        r1_log("CreateDevice ABORT: adapter %u is vendor 0x%04lx, REV1_VENDOR requires 0x%04x: the process ends before any device exists", use, (unsigned long)id.VendorId,
               testmode_need_vendor());
        TerminateProcess(GetCurrentProcess(), 0x5A5A);
        return D3DERR_NOTAVAILABLE;
    }
    HRESULT hr = real_CreateDevice(d3d, use, type, focus, flags, pp, out);
    r1_log("CreateDevice -> hr=0x%08lx", (unsigned long)hr);
    if (FAILED(hr) && use != adapter && !testmode_need_vendor())
        hr = real_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        void *p;
        t0 = GetTickCount();
        tr_dev = *out;
        hf_install(*out);
        if (pp)
            dr_pp = *pp;
        if ((p = r1_patch_vtable(*out, 16, (void *)hook_Reset)))
            real_Reset = (Reset_t)p;
        if ((p = r1_patch_vtable(*out, 17, (void *)hook_Present)))
            real_Present = (Present_t)p;
        if ((p = r1_patch_vtable(*out, 47, (void *)hook_SetViewport)))
            real_SetViewport = (SetViewport_t)p;
    }
    return hr;
}

static IDirect3D9 *WINAPI hook_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d = real_Direct3DCreate9(sdk);
    r1_log("Direct3DCreate9(%u) -> %p, %u adapters", sdk, (void *)d3d, d3d ? IDirect3D9_GetAdapterCount(d3d) : 0);
    if (d3d) {
        void *p = r1_patch_vtable(d3d, 16, (void *)hook_CreateDevice);
        if (p)
            real_CreateDevice = (CreateDevice_t)p;
        for (UINT i = 0; i < IDirect3D9_GetAdapterCount(d3d) && i < 4; i++) {
            D3DADAPTER_IDENTIFIER9 id;
            memset(&id, 0, sizeof id);
            if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, i, 0, &id)))
                r1_log("  adapter %u: %s (vendor 0x%04lx)", i, id.Description, id.VendorId);
        }
    }
    return d3d;
}

void d3d_install(void)
{
    char shots[256];
    t0 = GetTickCount();
    if (!r1_env("REV1_SHOTDIR", shot_dir, sizeof shot_dir)) {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
        char *slash = strrchr(exe, '\\');
        if (n && slash && slash - exe + 24 < MAX_PATH) {
            strcpy(slash + 1, "rev1_coop");
            snprintf(shot_dir, sizeof shot_dir, "%s\\shots", exe);
        }
    }
    if (r1_env("REV1_SHOTS", shots, sizeof shots))
        for (char *t = strtok(shots, ","); t && shot_count < 32; t = strtok(NULL, ","))
            shot_secs[shot_count++] = atoi(t);
    real_Direct3DCreate9 = (Direct3DCreate9_t)r1_patch_iat("d3d9.dll", "Direct3DCreate9", (void *)hook_Direct3DCreate9);
    r1_log("d3d: Direct3DCreate9 import %s, screenshots to %s", real_Direct3DCreate9 ? "hooked" : "NOT hooked", shot_dir);
}
