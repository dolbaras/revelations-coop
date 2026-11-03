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

#include "r1.h"

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Direct3DCreate9_t real_Direct3DCreate9;
static CreateDevice_t real_CreateDevice;
static Present_t real_Present;
static Reset_t real_Reset;

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

static HRESULT WINAPI hook_Present(IDirect3DDevice9 *dev, const RECT *s, const RECT *d, HWND w, const RGNDATA *r)
{
    unsigned n = ++frames;
    if (n == 1) {
        char st[160];
        r1_text_state(st, sizeof st);
        r1_log("first Present (the game renders): %s", st);
        r1_iat_check("first Present");
        lt_install();
        probe_install();
    }
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

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    log_pp("device Reset asked", pp);
    if (testmode_force_windowed() && pp) {
        pp->Windowed = TRUE;                   /* a test instance never goes exclusive full screen; windowed needs refresh 0 */
        pp->FullScreen_RefreshRateInHz = 0;
    }
    HRESULT hr = real_Reset(dev, pp);
    r1_log("device Reset %ux%u windowed=%d -> hr=0x%08lx", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1,
           (unsigned long)hr);
    return hr;
}

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
    r1_log("CreateDevice asked adapter %u, using %u (%s) %ux%u fmt=%d windowed=%d (was %s) buffers=%u msaa=%d flags=0x%lx", adapter, use,
           id.Description, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->BackBufferFormat : 0, pp ? pp->Windowed : -1,
           was_full ? "full screen" : "windowed", pp ? pp->BackBufferCount : 0, pp ? pp->MultiSampleType : 0, flags);
    HRESULT hr = real_CreateDevice(d3d, use, type, focus, flags, pp, out);
    r1_log("CreateDevice -> hr=0x%08lx", (unsigned long)hr);
    if (FAILED(hr) && use != adapter)
        hr = real_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        void *p;
        t0 = GetTickCount();
        if ((p = r1_patch_vtable(*out, 16, (void *)hook_Reset)))
            real_Reset = (Reset_t)p;
        if ((p = r1_patch_vtable(*out, 17, (void *)hook_Present)))
            real_Present = (Present_t)p;
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
            CreateDirectoryA(exe, NULL);
            snprintf(shot_dir, sizeof shot_dir, "%s\\shots", exe);
        }
    }
    if (r1_env("REV1_SHOTS", shots, sizeof shots))
        for (char *t = strtok(shots, ","); t && shot_count < 32; t = strtok(NULL, ","))
            shot_secs[shot_count++] = atoi(t);
    real_Direct3DCreate9 = (Direct3DCreate9_t)r1_patch_iat("d3d9.dll", "Direct3DCreate9", (void *)hook_Direct3DCreate9);
    r1_log("d3d: Direct3DCreate9 import %s, screenshots to %s", real_Direct3DCreate9 ? "hooked" : "NOT hooked", shot_dir);
}
