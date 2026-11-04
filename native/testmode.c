/*
 * Test instances: window placement, render adapter, running unfocused, no foreign input (env driven; all off in a
 * player's game).
 *
 *   REV1_WINPOS=x,y     the game window's client area at x,y (screen coordinates) and kept there
 *   REV1_WINSIZE=w,h    the client area w x h (the game's back buffer follows in windowed mode)
 *   REV1_ADAPTER=window the Direct3D 9 device on the adapter that drives the window's monitor; =<n> adapter n
 *   REV1_BACKGROUND=1   never full screen, never takes focus, keeps running when not in front; keyboard, mouse,
 *                       pads and the cursor reach it only while it really is the foreground window (it never reads
 *                       or clips the owner's input meant for another window)
 *   REV1_PADS=1         pads reach the game also when it is not in front and it is told it is the foreground window (virtual ViGEm pads in tests; default: hidden)
 *   REV1_WINLOG=1       window / dialog / message box calls in the log (always on for message boxes)
 *
 * Hooks go through the exe's import table and COM vtables; game code is not patched.
 */
#include <windows.h>
#include <d3d9.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"

static int pads_allowed;
static int have_pos, win_x, win_y, have_size, win_w, win_h, background, winlog;
static char adapter_mode[16];
static HWND game_window;
static WNDPROC game_wndproc;

int testmode_background(void) { return background; }
int testmode_force_windowed(void) { return background || have_pos || have_size; }
HWND testmode_window(void) { return game_window; }
void testmode_window_size(int *w, int *h) { *w = have_size ? win_w : 0, *h = have_size ? win_h : 0; }

static int really_in_front(void)
{
    return game_window && GetForegroundWindow() == game_window;
}

typedef HWND (WINAPI *CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND (WINAPI *CreateWindowExW_t)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI *SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *ShowWindow_t)(HWND, int);
typedef HWND (WINAPI *SetFocus_t)(HWND);
typedef BOOL (WINAPI *SetForegroundWindow_t)(HWND);
typedef void (WINAPI *SwitchToThisWindow_t)(HWND, BOOL);
typedef SHORT (WINAPI *GetAsyncKeyState_t)(int);
typedef BOOL (WINAPI *GetKeyboardState_t)(PBYTE);
typedef BOOL (WINAPI *ClipCursor_t)(const RECT *);
typedef BOOL (WINAPI *SetCursorPos_t)(int, int);
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE *);
static CreateWindowExA_t real_CreateWindowExA;
static CreateWindowExW_t real_CreateWindowExW;
static SetWindowPos_t real_SetWindowPos;
static ShowWindow_t real_ShowWindow;
static SetFocus_t real_SetFocus;
static SetForegroundWindow_t real_SetForegroundWindow;
static SwitchToThisWindow_t real_SwitchToThisWindow;
static GetAsyncKeyState_t real_GetAsyncKeyState;
static GetKeyboardState_t real_GetKeyboardState;
static ClipCursor_t real_ClipCursor;
static SetCursorPos_t real_SetCursorPos;
static XInputGetState_t real_XInputGetState;
static XInputGetState_t priv_XInputGetState;   /* REV1_PADS=1: XInputGetState of a private copy of the system's xinput1_4.dll (see priv_xinput_init) */

/* The Steam overlay (gameoverlayrenderer.dll, loaded into the test copy by the Steam stub) hooks XInput for Steam Input and answers "connected, packet 0, all zero"
 * to every poll while the game window is not the focused one - which is the normal state of a background test instance. XInputEnable(TRUE) did not help.
 * A private copy of the system DLL under another name is a module of its own that the overlay does not know. Test mode only (REV1_PADS=1). */
static void priv_xinput_init(void)
{
    char sys[MAX_PATH], tmp[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    if (!GetSystemDirectoryA(sys, sizeof sys) || !GetTempPathA(sizeof tmp, tmp))
        return;
    snprintf(src, sizeof src, "%s\\xinput1_4.dll", sys);
    snprintf(dst, sizeof dst, "%sRev1CoopXInputPriv.dll", tmp);
    if (!CopyFileA(src, dst, FALSE) && GetFileAttributesA(dst) == INVALID_FILE_ATTRIBUTES) {
        r1_log("xinput: private copy %s -> %s failed (%lu)", src, dst, GetLastError());
        return;
    }
    HMODULE m = LoadLibraryA(dst);
    if (m)
        priv_XInputGetState = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
    r1_log("xinput: private copy %s loaded %p, XInputGetState %p (REV1_PADS=1: the Steam overlay's XInput hook is bypassed)", dst, (void *)m, (void *)priv_XInputGetState);
}

static DWORD poll_real(DWORD user, XINPUT_STATE *st)
{
    return pads_allowed && priv_XInputGetState ? priv_XInputGetState(user, st) : real_XInputGetState(user, st);
}

/* The outer rectangle for the wanted client rectangle at win_x, win_y. */
static void outer_rect(HWND w, int *x, int *y, int *cw, int *ch)
{
    DWORD style = (DWORD)GetWindowLongA(w, GWL_STYLE), ex = (DWORD)GetWindowLongA(w, GWL_EXSTYLE);
    RECT cr;
    GetClientRect(w, &cr);
    RECT r = {win_x, win_y, win_x + (have_size ? win_w : cr.right), win_y + (have_size ? win_h : cr.bottom)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    *x = r.left, *y = r.top, *cw = r.right - r.left, *ch = r.bottom - r.top;
}

static void place(HWND w)
{
    int x, y, cw, ch;
    outer_rect(w, &x, &y, &cw, &ch);
    real_SetWindowPos(w, NULL, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT bg_proc(WNDPROC next, int wide, HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (background) {
        /* Why this matters (stage 2a, task 1): the game's logic thread idles in a Sleep(1) loop until its application object says the
         * window is active (byte app+0x2551, app = [exe+0x11DF9CC]). The window procedure keeps that byte like this: WM_ACTIVATEAPP(true)
         * or WM_NCACTIVATE(true) set it to 1 (and the latter sets app+0x2657 = 1); WM_ACTIVATE(active) with the byte already 1 but
         * app+0x2657 still 0 CLEARS it again (it calls the game's SetActive(false) at exe+0x8763E0). Swallowing WM_NCACTIVATE made
         * every WM_ACTIVATE that Windows sent later (any other window being activated on the desktop) switch the game off: the start
         * then stayed in that idle loop for seconds up to minutes (one run: 200 s). With WM_NCACTIVATE delivered the state is stable. */
        /* The game idles without rendering (no Present at all) until it hears that it is the active application, and it
         * pauses when it loses activation: in background mode it always hears "active" and never "lost focus". */
        if (msg == WM_NCACTIVATE)
            wp = TRUE;   /* the game's own handler must run: it sets its "window is active" flags (app+0x2551, +0x2657), see below */
        else if (msg == WM_ACTIVATEAPP)
            wp = TRUE;
        else if (msg == WM_ACTIVATE)
            wp = WA_ACTIVE;
        else if (msg == WM_KILLFOCUS)
            return 0;
    }
    if (have_pos && msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS *p = (WINDOWPOS *)lp;
        int x, y, cw, ch;
        outer_rect(w, &x, &y, &cw, &ch);
        p->x = x, p->y = y;
        if (have_size)
            p->cx = cw, p->cy = ch;
        p->flags &= ~(UINT)(SWP_NOMOVE | (have_size ? SWP_NOSIZE : 0));
    }
    return wide ? CallWindowProcW(next, w, msg, wp, lp) : CallWindowProcA(next, w, msg, wp, lp);
}

static LRESULT CALLBACK bg_wndprocA(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return bg_proc(game_wndproc, 0, w, msg, wp, lp); }
static LRESULT CALLBACK bg_wndprocW(HWND w, UINT msg, WPARAM wp, LPARAM lp) { return bg_proc(game_wndproc, 1, w, msg, wp, lp); }

/* The game's main window is the one whose window procedure is the game's (0x9BABD0 in the supported build); the "ExternalRegion" helper window is not. */
static int is_main_window(HWND hw, int wide)
{
    return (DWORD)(wide ? GetWindowLongPtrW(hw, GWLP_WNDPROC) : GetWindowLongPtrA(hw, GWLP_WNDPROC)) == R1_WNDPROC;
}

static void adopt_window(HWND hw, DWORD style, const char *cls, int wide)
{
    game_window = hw;
    game_wndproc = wide ? (WNDPROC)SetWindowLongW(hw, GWL_WNDPROC, (LONG)bg_wndprocW) : (WNDPROC)SetWindowLongA(hw, GWL_WNDPROC, (LONG)bg_wndprocA);
    if (have_pos)
        place(hw);
    r1_log("window %p class=%s style=%08lx created (%s)", (void *)hw, cls, style, wide ? "unicode" : "ansi");
}

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                                        HMENU menu, HINSTANCE inst, LPVOID param)
{
    int top = parent == NULL && !(style & WS_CHILD);
    if (top && have_pos)
        x = win_x, y = win_y;
    if (top && background)
        ex |= WS_EX_NOACTIVATE;   /* Windows never gives this window the foreground: the owner's keyboard focus stays where it is */
    HWND hw = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (hw && top && winlog)
        r1_log("CreateWindowExA %p class=%s name=%s style=%08lx %d,%d %dx%d", (void *)hw, IS_INTRESOURCE(cls) ? "#" : cls,
               name && !IS_INTRESOURCE(name) ? name : "", style, x, y, w, h);
    if (hw && top && !game_window && is_main_window(hw, 0))
        adopt_window(hw, style, IS_INTRESOURCE(cls) ? "#" : cls, 0);
    return hw;
}

static HWND WINAPI hook_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                                        HMENU menu, HINSTANCE inst, LPVOID param)
{
    int top = parent == NULL && !(style & WS_CHILD);
    if (top && have_pos)
        x = win_x, y = win_y;
    if (top && background)
        ex |= WS_EX_NOACTIVATE;
    HWND hw = real_CreateWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    char c[64] = "#";
    if (!IS_INTRESOURCE(cls))
        WideCharToMultiByte(CP_UTF8, 0, cls, -1, c, sizeof c, NULL, NULL);
    if (hw && top && winlog) {
        char n[128] = "";
        if (name && !IS_INTRESOURCE(name))
            WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof n, NULL, NULL);
        r1_log("CreateWindowExW %p class=%s name=%s style=%08lx %d,%d %dx%d", (void *)hw, c, n, style, x, y, w, h);
    }
    if (hw && top && !game_window && is_main_window(hw, 1))
        adopt_window(hw, style, c, 1);
    return hw;
}

static BOOL WINAPI hook_ShowWindow(HWND w, int cmd)
{
    if (background && w == game_window && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_RESTORE))
        cmd = SW_SHOWNOACTIVATE;
    BOOL r = real_ShowWindow(w, cmd);
    static int told;
    if (background && w == game_window && !told) {   /* nobody activates a no-activate window: tell the game once */
        told = 1;
        SendMessageA(w, WM_ACTIVATEAPP, TRUE, (LPARAM)GetCurrentThreadId());
        SendMessageA(w, WM_NCACTIVATE, TRUE, 0);
        SendMessageA(w, WM_ACTIVATE, WA_ACTIVE, 0);
        r1_log("background: the game window was told it is active");
    }
    return r;
}

static BOOL WINAPI hook_SetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT f)
{
    if (background && w == game_window) {
        f |= SWP_NOACTIVATE;
        if (after == HWND_TOP || after == HWND_TOPMOST)   /* (never over the owner's windows) */
            f |= SWP_NOZORDER;
    }
    return real_SetWindowPos(w, after, x, y, cx, cy, f);
}

static HWND WINAPI hook_SetFocus(HWND w)
{
    if (background && !really_in_front())
        return NULL;
    return real_SetFocus(w);
}

static BOOL WINAPI hook_SetForegroundWindow(HWND w)
{
    if (background)
        return FALSE;
    return real_SetForegroundWindow(w);
}

static void WINAPI hook_SwitchToThisWindow(HWND w, BOOL alt)
{
    if (!background)
        real_SwitchToThisWindow(w, alt);
}

static SHORT WINAPI hook_GetAsyncKeyState(int key)
{
    SHORT r = 0;
    if (!(background && !really_in_front()))
        r = real_GetAsyncKeyState(key);
    if (inject_vk_down(key))
        r = (SHORT)(r | 0x8000);
    return r;
}

typedef BOOL (WINAPI *GetCursorPos_t)(POINT *);
static GetCursorPos_t real_GetCursorPos;
static BOOL WINAPI hook_GetCursorPos(POINT *p)
{
    if (p && inject_cursor_pos(p))
        return TRUE;
    return real_GetCursorPos(p);
}

static BOOL WINAPI hook_GetKeyboardState(PBYTE keys)
{
    BOOL ok = real_GetKeyboardState(keys);
    if (ok && background && !really_in_front())
        memset(keys, 0, 256);
    return ok;
}

/* the game locks the mouse to its window (config LockMouse/MouseClip = ON): a background test never does */
static BOOL WINAPI hook_ClipCursor(const RECT *r)
{
    if (background)
        return real_ClipCursor(NULL);
    return real_ClipCursor(r);
}

static BOOL WINAPI hook_SetCursorPos(int x, int y)
{
    if (background && !really_in_front())
        return TRUE;
    return real_SetCursorPos(x, y);
}

/* pads: a background test sees no pad unless it is in front (the owner's pad belongs to the owner) */
static volatile LONG xinput_calls, xinput_ok;
static volatile LONG xinput_seen[4][3];
void testmode_xinput_stats(char *out, int size)
{
    int o = snprintf(out, size, "xinput: hook %s, %ld calls of the game, %ld answered with a connected pad; real answers per slot (ok/not connected/other):",
                     real_XInputGetState ? "in place" : "NOT in place", xinput_calls, xinput_ok);
    for (int i = 0; i < 4; i++)
        o += snprintf(out + o, size - o, " %d=%ld/%ld/%ld", i, xinput_seen[i][0], xinput_seen[i][1], xinput_seen[i][2]);
}

/* Synthetic pads (console "pad"): the test harness drives the game's pad slots itself. Virtual ViGEm pads did not reach the
 * game: with Steam running the game's XInput answers "not connected" for them (Steam Input owns the device and feeds only the
 * game that has the focus), so the hook answers for a slot the test has set. */
static struct { volatile LONG active; volatile LONG base_active; XINPUT_GAMEPAD pad; DWORD packet; volatile DWORD until; } synth[4];
/* xinput_seen per slot: [0] real success, [1] real not connected, [2] other */

void testmode_pad_set(int slot, int active, const void *gamepad)
{
    if (slot < 0 || slot > 3)
        return;
    synth[slot].until = 0;
    if (gamepad)
        synth[slot].pad = *(const XINPUT_GAMEPAD *)gamepad;
    synth[slot].packet++;
    synth[slot].base_active = active;
    InterlockedExchange(&synth[slot].active, active);
}

/* padhold: the state is set now and goes back (after ms milliseconds, without a second command) to what the slot was before: a neutral synthetic pad
 * (`pad N 0`), or no synthetic pad at all - then the keep-alive / the human's real pad of the co-op glue answers (stage 5) */
void testmode_pad_hold(int slot, const void *gamepad, int ms)
{
    if (slot < 0 || slot > 3)
        return;
    synth[slot].pad = *(const XINPUT_GAMEPAD *)gamepad;
    synth[slot].packet++;
    synth[slot].until = (GetTickCount() + (DWORD)ms) | 1;
    InterlockedExchange(&synth[slot].active, 1);
}

/* ---- the local human's real pad in co-op (stage 5) ----
 * The game takes pads from XInput users 0..3 (pad slot k = k-th connected user, logical player 1 = pad slot 0, logical player 2 = pad slot 1). In co-op the glue
 * keeps users 0 and 1 "connected" (a pad that vanishes ends the split screen within a second) and says which game user carries the local human: the real pad
 * of this PC (the first connected user; `rpad` can pretend one for tests) is passed through as that game user, the other game user is a neutral pad. Real pads
 * are never shown to the game under their own number while the mapping is on (a real pad 0 would otherwise become a third pad slot). */
static volatile LONG keep_user[4];
static volatile LONG human_user = -1;
static struct { volatile LONG active; XINPUT_GAMEPAD pad; } rpad[4];   /* test: pretend real pads (console rpad) */
static volatile DWORD pad_activity_tick;                               /* GetTickCount of the last non-neutral state of the human's real pad */
static int real_user_cache = -1;
static DWORD real_user_probe;

void testmode_pad_keep(int user, int on)
{
    if (user >= 0 && user < 4)
        InterlockedExchange(&keep_user[user], on);
}

void testmode_pad_human(int game_user)
{
    InterlockedExchange(&human_user, game_user >= 0 && game_user < 4 ? game_user : -1);
    real_user_cache = -1;
}

void testmode_pad_pretend(int user, int active, const void *gamepad)
{
    if (user < 0 || user > 3)
        return;
    if (gamepad)
        rpad[user].pad = *(const XINPUT_GAMEPAD *)gamepad;
    InterlockedExchange(&rpad[user].active, active);
}

DWORD testmode_pad_activity(void) { return pad_activity_tick; }

static int pad_is_active(const XINPUT_GAMEPAD *g)
{
    int d = 9000;   /* a stick dead zone wider than the game's: a resting stick must not claim the input mode */
    return g->wButtons || g->bLeftTrigger > 40 || g->bRightTrigger > 40 || g->sThumbLX > d || g->sThumbLX < -d || g->sThumbLY > d || g->sThumbLY < -d || g->sThumbRX > d ||
           g->sThumbRX < -d || g->sThumbRY > d || g->sThumbRY < -d;
}

/* The state of this PC's real pad (first connected XInput user); 0 = none. Pretended pads (console) come first. */
static int real_pad_read(XINPUT_STATE *st, DWORD *real_user)
{
    for (int u = 0; u < 4; u++)
        if (rpad[u].active) {
            memset(st, 0, sizeof *st);
            st->Gamepad = rpad[u].pad;
            st->dwPacketNumber = GetTickCount();
            *real_user = (DWORD)u;
            return 1;
        }
    if (!real_XInputGetState || (background && !pads_allowed && !really_in_front()))
        return 0;
    DWORD now = GetTickCount();
    if (real_user_cache >= 0 && poll_real((DWORD)real_user_cache, st) == ERROR_SUCCESS) {
        *real_user = (DWORD)real_user_cache;
        return 1;
    }
    if (real_user_cache >= 0 || (LONG)(now - real_user_probe) >= 1000) {   /* (re)probe once a second: scanning absent users costs time */
        real_user_probe = now;
        real_user_cache = -1;
        for (int u = 0; u < 4; u++)
            if (poll_real((DWORD)u, st) == ERROR_SUCCESS) {
                real_user_cache = u;
                *real_user = (DWORD)u;
                return 1;
            }
    }
    return 0;
}

/* which XInput users the game polls, and from which call sites (stage 2 live check of engine-map A9: GetState call sites 0x9BDB4F 0x9BEFF6 0x9C0423 0x9C0555 0x9C0692) */
static struct { DWORD ret; DWORD user; volatile LONG n; volatile LONG ok; } xsite[48];
static volatile LONG n_xsite;
static volatile LONG xuser_calls[8];
static struct { volatile WORD buttons; volatile SHORT ly; volatile DWORD packet; volatile LONG nonzero; } xlast[4];

static void xinput_note(DWORD user, DWORD ret, int ok)
{
    InterlockedIncrement(&xuser_calls[user < 7 ? user : 7]);
    LONG n = n_xsite;
    for (LONG k = 0; k < n && k < 48; k++)
        if (xsite[k].ret == ret && xsite[k].user == user) {
            InterlockedIncrement(&xsite[k].n);
            if (ok)
                InterlockedIncrement(&xsite[k].ok);
            return;
        }
    if (n < 48) {
        LONG slot = InterlockedIncrement(&n_xsite) - 1;
        if (slot < 48) {
            xsite[slot].ret = ret, xsite[slot].user = user, xsite[slot].n = 1, xsite[slot].ok = ok;
            r1_log("xinput: user %lu polled from exe+%lx (VA %08lx)", user, (unsigned long)(ret - 0x400000), (unsigned long)ret);
        }
    }
}

void testmode_xinput_sites(char *out, int size)
{
    int o = snprintf(out, size, "XInputGetState calls per user:");
    for (int i = 0; i < 8; i++)
        if (xuser_calls[i])
            o += snprintf(out + o, size - o, " u%d=%ld", i, xuser_calls[i]);
    for (int i = 0; i < 4; i++)
        o += snprintf(out + o, size - o, "\nlast state the game got, user %d: buttons %04x ly %d packet %lu, polls with input: %ld", i, xlast[i].buttons, xlast[i].ly, (unsigned long)xlast[i].packet, xlast[i].nonzero);
    if (real_XInputGetState) {   /* a direct call of the real function from here (console thread): is the pad data there at all? */
        char mod[MAX_PATH] = "?";
        HMODULE hm = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)real_XInputGetState, &hm) && hm)
            GetModuleFileNameA(hm, mod, sizeof mod);
        for (int i = 0; i < 2; i++) {
            XINPUT_STATE t;
            memset(&t, 0, sizeof t);
            DWORD r = poll_real((DWORD)i, &t);
            o += snprintf(out + o, size - o, "\nreal XInputGetState %p (%s) direct call user %d: r=%lu buttons %04x ly %d packet %lu", (void *)real_XInputGetState, mod, i, (unsigned long)r, t.Gamepad.wButtons, t.Gamepad.sThumbLY, (unsigned long)t.dwPacketNumber);
        }
    }
    o += snprintf(out + o, size - o, "\ncall sites (return address, user, calls, answered connected):");
    LONG n = n_xsite < 48 ? n_xsite : 48;
    for (LONG k = 0; k < n && o + 90 < size; k++)
        o += snprintf(out + o, size - o, "\n  %08lx user %lu: %ld calls, %ld connected", (unsigned long)xsite[k].ret, xsite[k].user, xsite[k].n, xsite[k].ok);
}

/* split screen: while the second human's menu is open the game's pad slots 0 and 1 are fed from the OTHER physical pad (the menus read slot 0 as "the local human") */
static volatile LONG pad_swap;
static volatile LONG pad_swap_base;   /* the co-op lobby's "player 1 uses gamepad 2": a permanent exchange; the menu exchange above toggles on top of it (XOR) */
void testmode_pad_swap(int on) { InterlockedExchange(&pad_swap, on != 0); }
int testmode_pad_swapped(void) { return pad_swap != 0; }
void testmode_pad_swap_base(int on) { InterlockedExchange(&pad_swap_base, on != 0); }
int testmode_pad_swap_base_get(void) { return pad_swap_base != 0; }

static DWORD xinput_inner(DWORD i, XINPUT_STATE *st)
{
    static LONG logged;
    if ((pad_swap ^ pad_swap_base) && i < 2)
        i ^= 1;
    if (i < 4 && synth[i].active && st) {
        if (synth[i].until && (LONG)(GetTickCount() - synth[i].until) >= 0) {
            memset(&synth[i].pad, 0, sizeof synth[i].pad);
            synth[i].until = 0;
            synth[i].packet++;
            InterlockedExchange(&synth[i].active, synth[i].base_active);
        }
        if (synth[i].active) {
            st->dwPacketNumber = synth[i].packet;
            st->Gamepad = synth[i].pad;
            InterlockedIncrement(&xinput_ok);
            return ERROR_SUCCESS;
        }
    }
    if (i < 4 && keep_user[i] && st) {   /* co-op: this game user is always connected */
        XINPUT_STATE rs;
        DWORD ru;
        memset(st, 0, sizeof *st);
        if ((LONG)i == human_user && real_pad_read(&rs, &ru)) {
            *st = rs;
            if (pad_is_active(&st->Gamepad))
                pad_activity_tick = GetTickCount() | 1;
        } else
            st->dwPacketNumber = 1;
        InterlockedIncrement(&xinput_ok);
        return ERROR_SUCCESS;
    }
    if (human_user >= 0 && st) {   /* mapping on: real pads only through the human's game user */
        memset(st, 0, sizeof *st);
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    if (background && !pads_allowed && !really_in_front()) {
        if (InterlockedIncrement(&logged) == 1)
            r1_log("xinput: pads hidden while the window is not in front (REV1_BACKGROUND)");
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD r = poll_real(i, st);
    if (i < 4)
        InterlockedIncrement(&xinput_seen[i][r == ERROR_SUCCESS ? 0 : r == ERROR_DEVICE_NOT_CONNECTED ? 1 : 2]);
    if (r == ERROR_SUCCESS)
        InterlockedIncrement(&xinput_ok);
    static LONG n_pressed;
    if (r == ERROR_SUCCESS && st && st->Gamepad.wButtons && InterlockedIncrement(&n_pressed) <= 5)
        r1_log("xinput: pad %lu buttons=%04x (seen by the game's own call)", i, st->Gamepad.wButtons);
    return r;
}

static DWORD WINAPI hook_XInputGetState(DWORD i, XINPUT_STATE *st)
{
    DWORD ret_addr = (DWORD)__builtin_return_address(0);
    InterlockedIncrement(&xinput_calls);
    DWORD fr = xinput_inner(i, st);
    xinput_note(i, ret_addr, fr == ERROR_SUCCESS);
    if (i < 4 && st && fr == ERROR_SUCCESS) {   /* what the game really gets (console `pads`): last state per user and how often a button was down */
        xlast[i].buttons = st->Gamepad.wButtons;
        xlast[i].ly = st->Gamepad.sThumbLY;
        xlast[i].packet = st->dwPacketNumber;
        if (st->Gamepad.wButtons || st->Gamepad.sThumbLX || st->Gamepad.sThumbLY)
            InterlockedIncrement(&xlast[i].nonzero);
    }
    return fr;
}

/* XInputGetCapabilities (ordinal 4) / XInputSetState (3): the game's pad scan (sPad_AssignXInputPad) asks for the capabilities of a user it found
 * connected and gives up when that call fails, so a spoofed pad must answer this too (Type 1 gamepad, SubType 1 gamepad). */
typedef DWORD (WINAPI *XInputGetCapabilities_t)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef DWORD (WINAPI *XInputSetState_t)(DWORD, XINPUT_VIBRATION *);
static XInputGetCapabilities_t real_XInputGetCapabilities;
static XInputSetState_t real_XInputSetState;

static DWORD WINAPI hook_XInputGetCapabilities(DWORD i, DWORD flags, XINPUT_CAPABILITIES *c)
{
    if (i < 4 && (synth[i].active || keep_user[i]) && c) {
        memset(c, 0, sizeof *c);
        c->Type = XINPUT_DEVTYPE_GAMEPAD;
        c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        c->Flags = 0;
        c->Gamepad.wButtons = 0xF3FF;
        c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 255;
        c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = (SHORT)-64;
        return ERROR_SUCCESS;
    }
    return real_XInputGetCapabilities ? real_XInputGetCapabilities(i, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}

/* XInputEnable (ordinal 5 of XINPUT1_3): the game turns XInput OFF when its window is not the active application, and XInput then answers every
 * XInputGetState with "connected, packet 0, all zero" (found 2026-11-04: ViGEm pads reached an external process and the 32-bit XInput1_3 inside the game's own
 * process, but the game's polls saw nothing). A test instance that must read virtual pads in the background (REV1_PADS=1) keeps XInput on. */
typedef void (WINAPI *XInputEnable_t)(BOOL);
static XInputEnable_t real_XInputEnable;
static volatile LONG n_xenable_off;

static void WINAPI hook_XInputEnable(BOOL enable)
{
    if (!enable && pads_allowed) {
        if (InterlockedIncrement(&n_xenable_off) <= 3)
            r1_log("xinput: the game asked XInputEnable(FALSE); kept on (REV1_PADS=1)");
        enable = TRUE;
    }
    if (real_XInputEnable)
        real_XInputEnable(enable);
}

static DWORD WINAPI hook_XInputSetState(DWORD i, XINPUT_VIBRATION *v)
{
    if (i < 4 && (LONG)i == human_user && keep_user[i] && real_XInputGetState && real_XInputSetState && real_user_cache >= 0)   /* rumble goes to the human's real pad */
        return real_XInputSetState((DWORD)real_user_cache, v);
    if (i < 4 && (synth[i].active || keep_user[i]))
        return ERROR_SUCCESS;
    if (human_user >= 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return real_XInputSetState ? real_XInputSetState(i, v) : ERROR_DEVICE_NOT_CONNECTED;
}

/* The game ignores its pads unless it is the foreground window: a test instance that is meant to read virtual pads
 * (REV1_PADS=1) is told it is. Our own checks (really_in_front) call the real function and are not fooled. */
typedef HWND (WINAPI *GetForegroundWindow_t)(void);
static GetForegroundWindow_t real_GetForegroundWindow;

static HWND WINAPI hook_GetForegroundWindow(void)
{
    HWND w = real_GetForegroundWindow();
    return background && game_window ? game_window : w;   /* the game keeps "my window is the foreground window" (obj+0x285E1) and ignores input without it */
}

/* ---- message boxes and dialogs: always logged (a start that waits on one shows nothing else) ---- */
typedef int (WINAPI *MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
typedef int (WINAPI *MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
typedef INT_PTR (WINAPI *DialogBoxParamA_t)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
typedef INT_PTR (WINAPI *DialogBoxParamW_t)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM);
static MessageBoxA_t real_MessageBoxA;
static MessageBoxW_t real_MessageBoxW;
static DialogBoxParamA_t real_DialogBoxParamA;
static DialogBoxParamW_t real_DialogBoxParamW;

static int WINAPI hook_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT type)
{
    r1_log("MessageBoxA \"%s\": %s", cap ? cap : "", text ? text : "");
    if (background) {   /* a test instance never shows a modal box on the owner's desktop: logged, answered OK at once */
        r1_log("MessageBoxA: not shown (REV1_BACKGROUND), answered 1");
        return 1;
    }
    int r = real_MessageBoxA(w, text, cap, type);
    r1_log("MessageBoxA answered %d", r);
    return r;
}

static int WINAPI hook_MessageBoxW(HWND w, LPCWSTR text, LPCWSTR cap, UINT type)
{
    char t[512], c[128];
    WideCharToMultiByte(CP_UTF8, 0, text ? text : L"", -1, t, sizeof t, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, cap ? cap : L"", -1, c, sizeof c, NULL, NULL);
    r1_log("MessageBoxW \"%s\": %s", c, t);
    int r = real_MessageBoxW(w, text, cap, type);
    r1_log("MessageBoxW answered %d", r);
    return r;
}

static INT_PTR WINAPI hook_DialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    r1_log("DialogBoxParamA template %s (%u)", IS_INTRESOURCE(tmpl) ? "(id)" : tmpl, IS_INTRESOURCE(tmpl) ? (unsigned)(UINT_PTR)tmpl : 0);
    INT_PTR r = real_DialogBoxParamA(inst, tmpl, parent, proc, lp);
    r1_log("DialogBoxParamA returned %ld", (long)r);
    return r;
}

static INT_PTR WINAPI hook_DialogBoxParamW(HINSTANCE inst, LPCWSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    r1_log("DialogBoxParamW template id %u", IS_INTRESOURCE(tmpl) ? (unsigned)(UINT_PTR)tmpl : 0);
    INT_PTR r = real_DialogBoxParamW(inst, tmpl, parent, proc, lp);
    r1_log("DialogBoxParamW returned %ld", (long)r);
    return r;
}

/* ---- DirectInput: no keyboard/mouse data while another window is in front ---- */
typedef HRESULT (WINAPI *CreateDevice_DI_t)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *GetDeviceData_t)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT (WINAPI *SetCoopLevel_t)(void *, HWND, DWORD);
static CreateDevice_DI_t real_DICreateDevice;
static GetDeviceState_t real_GetDeviceState;
static GetDeviceData_t real_GetDeviceData;
static SetCoopLevel_t real_SetCoopLevel;

static void *kb_dev, *mouse_dev;
static volatile LONG di_state_calls[2], di_data_calls[2];   /* [0] keyboard, [1] mouse */
static DWORD di_state_size[2], di_data_objsize[2];

static HRESULT WINAPI bg_GetDeviceState(void *dev, DWORD size, void *data)
{
    HRESULT hr = real_GetDeviceState(dev, size, data);
    if (background && !really_in_front() && data)
        memset(data, 0, size);
    if (data && (dev == kb_dev || dev == mouse_dev)) {
        int k = dev == mouse_dev;
        di_state_size[k] = size;
        if (InterlockedIncrement(&di_state_calls[k]) == 1)
            r1_log("dinput: the game reads the %s with GetDeviceState (size %lu)", k ? "mouse" : "keyboard", size);
        if (k)
            inject_apply_mouse(data, size);
        else if (size == 256)
            inject_apply_keyboard((unsigned char *)data);
    }
    return hr;
}

static HRESULT WINAPI bg_GetDeviceData(void *dev, DWORD objsize, void *data, DWORD *count, DWORD flags)
{
    DWORD room = count ? *count : 0;
    HRESULT hr = real_GetDeviceData(dev, objsize, data, count, flags);
    if (background && !really_in_front() && count && !(flags & 1 /* DIGDD_PEEK */))
        *count = 0;
    if (data && count && (dev == kb_dev || dev == mouse_dev) && !(flags & 1)) {
        int k = dev == mouse_dev;
        di_data_objsize[k] = objsize;
        if (InterlockedIncrement(&di_data_calls[k]) == 1)
            r1_log("dinput: the game reads the %s with GetDeviceData (object size %lu, buffer %lu)", k ? "mouse" : "keyboard", objsize, room);
        inject_apply_data(k ? 2 : 1, data, objsize, (unsigned *)count, room);
    }
    return hr;
}

void testmode_dinput_stats(char *out, int size)
{
    snprintf(out, size, "dinput: keyboard %p GetDeviceState %ld (size %lu) GetDeviceData %ld (objsize %lu); mouse %p GetDeviceState %ld (size %lu) GetDeviceData %ld (objsize %lu)",
             kb_dev, di_state_calls[0], di_state_size[0], di_data_calls[0], di_data_objsize[0], mouse_dev, di_state_calls[1], di_state_size[1],
             di_data_calls[1], di_data_objsize[1]);
}

/* Exclusive/foreground levels fail or lose the device for a window that is not in front. */
static HRESULT WINAPI bg_SetCoopLevel(void *dev, HWND w, DWORD flags)
{
    if (background)
        flags = (flags & ~(DWORD)(0x1 | 0x4)) | 0x2 | 0x8;   /* EXCLUSIVE|FOREGROUND -> NONEXCLUSIVE|BACKGROUND */
    return real_SetCoopLevel(dev, w, flags);
}

static HRESULT WINAPI hook_DICreateDevice(void *di, REFGUID guid, void **out, void *outer)
{
    HRESULT hr = real_DICreateDevice(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void *p;
        static LONG n;
        if (InterlockedIncrement(&n) <= 8)
            r1_log("dinput: CreateDevice %08lx-%04x -> %p", guid->Data1, guid->Data2, *out);
        if (guid->Data1 == 0x6f1d2b61)
            kb_dev = *out;
        else if (guid->Data1 == 0x6f1d2b60)
            mouse_dev = *out;
        if ((p = r1_patch_vtable(*out, 9, (void *)bg_GetDeviceState)))
            real_GetDeviceState = (GetDeviceState_t)p;
        if ((p = r1_patch_vtable(*out, 10, (void *)bg_GetDeviceData)))
            real_GetDeviceData = (GetDeviceData_t)p;
        if ((p = r1_patch_vtable(*out, 13, (void *)bg_SetCoopLevel)))
            real_SetCoopLevel = (SetCoopLevel_t)p;
    }
    return hr;
}

/* IDirectInput8A/W::CreateDevice is slot 3 in both. */
void testmode_wrap_dinput(void *dinput)
{
    void *p = r1_patch_vtable(dinput, 3, (void *)hook_DICreateDevice);
    if (p)
        real_DICreateDevice = (CreateDevice_DI_t)p;
}

/* ---- what the game fetches dynamically (GetProcAddress / LoadLibrary): logged, the first 60 (finding pad and audio paths) ---- */
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
typedef HMODULE (WINAPI *LoadLibraryA_t)(LPCSTR);
typedef HMODULE (WINAPI *LoadLibraryW_t)(LPCWSTR);
static GetProcAddress_t real_GetProcAddress;
static LoadLibraryA_t real_LoadLibraryA;
static LoadLibraryW_t real_LoadLibraryW;
static LONG n_dyn;

static FARPROC WINAPI hook_GetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC p = real_GetProcAddress(m, name);
    if (InterlockedIncrement(&n_dyn) <= 60 && (m != (HMODULE)GetModuleHandleA("kernel32.dll"))) {
        char mod[MAX_PATH] = "?";
        GetModuleFileNameA(m, mod, sizeof mod);
        const char *b = strrchr(mod, '\\');
        if (IS_INTRESOURCE(name))
            r1_log("dyn: GetProcAddress(%s, #%u) -> %p", b ? b + 1 : mod, (unsigned)(UINT_PTR)name, (void *)p);
        else
            r1_log("dyn: GetProcAddress(%s, %s) -> %p", b ? b + 1 : mod, name, (void *)p);
    }
    return p;
}

static HMODULE WINAPI hook_LoadLibraryA(LPCSTR n)
{
    HMODULE m = real_LoadLibraryA(n);
    if (InterlockedIncrement(&n_dyn) <= 60)
        r1_log("dyn: LoadLibraryA(%s) -> %p", n ? n : "(null)", (void *)m);
    return m;
}

static HMODULE WINAPI hook_LoadLibraryW(LPCWSTR n)
{
    HMODULE m = real_LoadLibraryW(n);
    char t[MAX_PATH] = "";
    if (n)
        WideCharToMultiByte(CP_UTF8, 0, n, -1, t, sizeof t, NULL, NULL);
    if (InterlockedIncrement(&n_dyn) <= 60)
        r1_log("dyn: LoadLibraryW(%s) -> %p", t, (void *)m);
    return m;
}

/* ---- adapter choice ---- */
int testmode_adapter_pick(void *d3dv, HWND focus, unsigned asked)
{
    IDirect3D9 *d3d = (IDirect3D9 *)d3dv;
    UINT n = d3d->lpVtbl->GetAdapterCount(d3d);
    if (adapter_mode[0] >= '0' && adapter_mode[0] <= '9') {
        UINT want = (UINT)atoi(adapter_mode);
        return want < n ? (int)want : (int)asked;
    }
    if (strcmp(adapter_mode, "window") == 0) {
        HMONITOR mon = have_pos ? MonitorFromPoint((POINT){win_x + 50, win_y + 50}, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromWindow(focus, MONITOR_DEFAULTTONEAREST);
        for (UINT i = 0; i < n; i++)
            if (d3d->lpVtbl->GetAdapterMonitor(d3d, i) == mon)
                return (int)i;
    }
    return (int)asked;
}

void testmode_install(void)
{
    char v[64];
    if (r1_env("REV1_WINPOS", v, sizeof v) && sscanf(v, "%d,%d", &win_x, &win_y) == 2)
        have_pos = 1;
    if (r1_env("REV1_WINSIZE", v, sizeof v) && sscanf(v, "%d,%d", &win_w, &win_h) == 2 && win_w > 0 && win_h > 0)
        have_size = 1;
    background = r1_env("REV1_BACKGROUND", v, sizeof v) && v[0] == '1';
    pads_allowed = r1_env("REV1_PADS", v, sizeof v) && v[0] == '1';   /* virtual test pads reach the game while it is in the background */
    winlog = r1_env("REV1_WINLOG", v, sizeof v) && v[0] == '1';
    r1_env("REV1_ADAPTER", adapter_mode, sizeof adapter_mode);
    {
        real_MessageBoxA = (MessageBoxA_t)r1_patch_iat("USER32.dll", "MessageBoxA", (void *)hook_MessageBoxA);
        real_DialogBoxParamA = (DialogBoxParamA_t)r1_patch_iat("USER32.dll", "DialogBoxParamA", (void *)hook_DialogBoxParamA);
        real_DialogBoxParamW = (DialogBoxParamW_t)r1_patch_iat("USER32.dll", "DialogBoxParamW", (void *)hook_DialogBoxParamW);
        real_GetProcAddress = (GetProcAddress_t)r1_patch_iat("KERNEL32.dll", "GetProcAddress", (void *)hook_GetProcAddress);
        real_LoadLibraryA = (LoadLibraryA_t)r1_patch_iat("KERNEL32.dll", "LoadLibraryA", (void *)hook_LoadLibraryA);
        real_LoadLibraryW = (LoadLibraryW_t)r1_patch_iat("KERNEL32.dll", "LoadLibraryW", (void *)hook_LoadLibraryW);
        real_MessageBoxW = (MessageBoxW_t)r1_patch_iat("USER32.dll", "MessageBoxW", (void *)hook_MessageBoxW);
    }
    {   /* XInput: always hooked (counts which users the game polls; the co-op glue keeps pads connected) */
        real_XInputGetState = (XInputGetState_t)r1_patch_iat("XINPUT1_3.dll", "#2", (void *)hook_XInputGetState);
        real_XInputSetState = (XInputSetState_t)r1_patch_iat("XINPUT1_3.dll", "#3", (void *)hook_XInputSetState);
        real_XInputGetCapabilities = (XInputGetCapabilities_t)r1_patch_iat("XINPUT1_3.dll", "#4", (void *)hook_XInputGetCapabilities);
        real_XInputEnable = (XInputEnable_t)r1_patch_iat("XINPUT1_3.dll", "#5", (void *)hook_XInputEnable);
        if (pads_allowed && real_XInputEnable)
            real_XInputEnable(TRUE);
        if (pads_allowed)
            priv_xinput_init();
    }
    if (!(have_pos || have_size || background || winlog || adapter_mode[0]))
        return;
    real_CreateWindowExA = (CreateWindowExA_t)r1_patch_iat("USER32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
    real_CreateWindowExW = (CreateWindowExW_t)r1_patch_iat("USER32.dll", "CreateWindowExW", (void *)hook_CreateWindowExW);
    real_SetWindowPos = (SetWindowPos_t)r1_patch_iat("USER32.dll", "SetWindowPos", (void *)hook_SetWindowPos);
    real_ShowWindow = (ShowWindow_t)r1_patch_iat("USER32.dll", "ShowWindow", (void *)hook_ShowWindow);
    real_SetFocus = (SetFocus_t)r1_patch_iat("USER32.dll", "SetFocus", (void *)hook_SetFocus);
    real_SetForegroundWindow = (SetForegroundWindow_t)r1_patch_iat("USER32.dll", "SetForegroundWindow", (void *)hook_SetForegroundWindow);
    real_SwitchToThisWindow = (SwitchToThisWindow_t)r1_patch_iat("USER32.dll", "SwitchToThisWindow", (void *)hook_SwitchToThisWindow);
    real_GetAsyncKeyState = (GetAsyncKeyState_t)r1_patch_iat("USER32.dll", "GetAsyncKeyState", (void *)hook_GetAsyncKeyState);
    real_GetKeyboardState = (GetKeyboardState_t)r1_patch_iat("USER32.dll", "GetKeyboardState", (void *)hook_GetKeyboardState);
    real_GetCursorPos = (GetCursorPos_t)r1_patch_iat("USER32.dll", "GetCursorPos", (void *)hook_GetCursorPos);
    real_ClipCursor = (ClipCursor_t)r1_patch_iat("USER32.dll", "ClipCursor", (void *)hook_ClipCursor);
    real_SetCursorPos = (SetCursorPos_t)r1_patch_iat("USER32.dll", "SetCursorPos", (void *)hook_SetCursorPos);
    if (background)
        real_GetForegroundWindow = (GetForegroundWindow_t)r1_patch_iat("USER32.dll", "GetForegroundWindow", (void *)hook_GetForegroundWindow);
    r1_log("test mode: pos=%d,%d (%s) size=%dx%d (%s) background=%d adapter=%s", win_x, win_y, have_pos ? "on" : "off", win_w, win_h,
           have_size ? "on" : "off", background, adapter_mode[0] ? adapter_mode : "game's");
}

/* The state the game would get from XInputGetState(user): a synthetic pad (console pad/padhold) first, else the real one (when allowed); 1 = connected */
int testmode_pad_peek(int user, void *out)
{
    XINPUT_GAMEPAD *g = (XINPUT_GAMEPAD *)out;
    if (user < 0 || user > 3)
        return 0;
    if (synth[user].active) {
        if (synth[user].until && (LONG)(GetTickCount() - synth[user].until) >= 0) {
            memset(&synth[user].pad, 0, sizeof synth[user].pad);
            synth[user].until = 0;
            synth[user].packet++;
            InterlockedExchange(&synth[user].active, synth[user].base_active);
        }
        if (synth[user].active) {
            *g = synth[user].pad;
            return 1;
        }
    }
    if (!real_XInputGetState || (background && !pads_allowed && !really_in_front()))
        return 0;
    XINPUT_STATE st;
    if (poll_real((DWORD)user, &st) != ERROR_SUCCESS)
        return 0;
    *g = st.Gamepad;
    return 1;
}
