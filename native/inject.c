/*
 * Synthetic keyboard / mouse input for test runs (console commands key, keydown, keyup, mouse, mbtn, cursor, wmkey).
 *
 * The game reads the keyboard and the mouse through DirectInput 8 (our proxy owns dinput8.dll and has patched the devices'
 * GetDeviceState / GetDeviceData slots, testmode.c) and also calls GetAsyncKeyState / GetCursorPos. The injected state is
 * merged into what the game gets from those calls: it never reaches the owner's real devices or any other window, and the real
 * keyboard/mouse data is still dropped while the test window is in the background (REV1_BACKGROUND). Off in a player's game
 * (nothing is injected unless a console command asked for it).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"

#define FOREVER 0xFFFFFFFFu

static struct { const char *name; int dik; int vk; } keytab[] = {
    {"esc", 0x01, VK_ESCAPE}, {"1", 0x02, '1'}, {"2", 0x03, '2'}, {"3", 0x04, '3'}, {"4", 0x05, '4'}, {"5", 0x06, '5'}, {"6", 0x07, '6'},
    {"7", 0x08, '7'}, {"8", 0x09, '8'}, {"9", 0x0A, '9'}, {"0", 0x0B, '0'}, {"backspace", 0x0E, VK_BACK}, {"tab", 0x0F, VK_TAB},
    {"q", 0x10, 'Q'}, {"w", 0x11, 'W'}, {"e", 0x12, 'E'}, {"r", 0x13, 'R'}, {"t", 0x14, 'T'}, {"y", 0x15, 'Y'}, {"u", 0x16, 'U'},
    {"i", 0x17, 'I'}, {"o", 0x18, 'O'}, {"p", 0x19, 'P'}, {"enter", 0x1C, VK_RETURN}, {"lctrl", 0x1D, VK_LCONTROL}, {"a", 0x1E, 'A'},
    {"s", 0x1F, 'S'}, {"d", 0x20, 'D'}, {"f", 0x21, 'F'}, {"g", 0x22, 'G'}, {"h", 0x23, 'H'}, {"j", 0x24, 'J'}, {"k", 0x25, 'K'},
    {"l", 0x26, 'L'}, {"lshift", 0x2A, VK_LSHIFT}, {"z", 0x2C, 'Z'}, {"x", 0x2D, 'X'}, {"c", 0x2E, 'C'}, {"v", 0x2F, 'V'},
    {"b", 0x30, 'B'}, {"n", 0x31, 'N'}, {"m", 0x32, 'M'}, {"rshift", 0x36, VK_RSHIFT}, {"lalt", 0x38, VK_LMENU}, {"space", 0x39, VK_SPACE},
    {"caps", 0x3A, VK_CAPITAL}, {"f1", 0x3B, VK_F1}, {"f2", 0x3C, VK_F2}, {"f3", 0x3D, VK_F3}, {"f4", 0x3E, VK_F4}, {"f5", 0x3F, VK_F5},
    {"f6", 0x40, VK_F6}, {"f7", 0x41, VK_F7}, {"f8", 0x42, VK_F8}, {"f9", 0x43, VK_F9}, {"f10", 0x44, VK_F10}, {"f11", 0x57, VK_F11},
    {"f12", 0x58, VK_F12}, {"rctrl", 0x9D, VK_RCONTROL}, {"ralt", 0xB8, VK_RMENU}, {"home", 0xC7, VK_HOME}, {"up", 0xC8, VK_UP},
    {"pgup", 0xC9, VK_PRIOR}, {"left", 0xCB, VK_LEFT}, {"right", 0xCD, VK_RIGHT}, {"end", 0xCF, VK_END}, {"down", 0xD0, VK_DOWN},
    {"pgdn", 0xD1, VK_NEXT}, {"insert", 0xD2, VK_INSERT}, {"delete", 0xD3, VK_DELETE}, {"kpenter", 0x9C, VK_RETURN},
    {"apostrophe", 0x28, VK_OEM_7}, {"minus", 0x0C, VK_OEM_MINUS}, {"equals", 0x0D, VK_OEM_PLUS}, {"grave", 0x29, VK_OEM_3},
};

static volatile DWORD key_until[256];   /* by DIK code: 0 = up, FOREVER = until keyup, else the tick when it is released */
static unsigned char key_prev[256];     /* what buffered readers were already told */
static volatile LONG mouse_dx, mouse_dy, mouse_dz;
static volatile DWORD mbtn_until[8];
static unsigned char mbtn_prev[8];
static volatile LONG cursor_on, cursor_x, cursor_y;
static volatile LONG used;               /* anything injected yet: 0 = hooks stay out of the way */

int inject_key_lookup(const char *s, int *dik, int *vk)
{
    for (unsigned i = 0; i < sizeof keytab / sizeof keytab[0]; i++)
        if (!strcmp(keytab[i].name, s)) {
            *dik = keytab[i].dik, *vk = keytab[i].vk;
            return 1;
        }
    if (s[0] == '0' && s[1] == 'x') {
        *dik = (int)strtoul(s, NULL, 16) & 0xFF;
        *vk = 0;
        for (unsigned i = 0; i < sizeof keytab / sizeof keytab[0]; i++)
            if (keytab[i].dik == *dik)
                *vk = keytab[i].vk;
        return 1;
    }
    return 0;
}

void inject_key(int dik, int ms)
{
    used = 1;
    key_until[dik & 0xFF] = ms <= 0 ? FOREVER : (GetTickCount() + (DWORD)ms) | 1;
}

void inject_key_up(int dik) { key_until[dik & 0xFF] = 0; }

static int key_down(int dik)
{
    DWORD u = key_until[dik & 0xFF];
    if (u == 0)
        return 0;
    if (u == FOREVER)
        return 1;
    if ((LONG)(GetTickCount() - u) >= 0) {
        key_until[dik & 0xFF] = 0;
        return 0;
    }
    return 1;
}

void inject_mouse_move(int dx, int dy, int wheel)
{
    used = 1;
    InterlockedAdd(&mouse_dx, dx), InterlockedAdd(&mouse_dy, dy), InterlockedAdd(&mouse_dz, wheel);
}

void inject_mouse_button(int n, int ms)
{
    used = 1;
    if (n >= 0 && n < 8)
        mbtn_until[n] = ms <= 0 ? FOREVER : (GetTickCount() + (DWORD)ms) | 1;
}

void inject_mouse_button_up(int n)
{
    if (n >= 0 && n < 8)
        mbtn_until[n] = 0;
}

static int mbtn_down(int n)
{
    DWORD u = mbtn_until[n];
    if (u == 0)
        return 0;
    if (u == FOREVER)
        return 1;
    if ((LONG)(GetTickCount() - u) >= 0) {
        mbtn_until[n] = 0;
        return 0;
    }
    return 1;
}

void inject_cursor(int on, int x, int y)
{
    used = 1;
    cursor_x = x, cursor_y = y, cursor_on = on;
}

/* GetCursorPos replacement: client coordinates of the fake cursor, as screen coordinates of the game window; 1 = answered */
int inject_cursor_pos(POINT *p)
{
    HWND w = testmode_window();
    if (!cursor_on || !w)
        return 0;
    POINT o = {0, 0};
    ClientToScreen(w, &o);
    p->x = o.x + cursor_x, p->y = o.y + cursor_y;
    return 1;
}

int inject_active(void) { return used; }

/* 256-byte DirectInput keyboard state */
void inject_apply_keyboard(unsigned char *state)
{
    if (!used)
        return;
    for (int i = 0; i < 256; i++)
        if (key_down(i))
            state[i] |= 0x80;
}

/* DIMOUSESTATE (16 bytes) / DIMOUSESTATE2 (20 bytes): LONG lX, lY, lZ, then buttons */
void inject_apply_mouse(void *data, unsigned size)
{
    if (!used || (size != 16 && size != 20))
        return;
    LONG *l = (LONG *)data;
    l[0] += InterlockedExchange(&mouse_dx, 0), l[1] += InterlockedExchange(&mouse_dy, 0), l[2] += InterlockedExchange(&mouse_dz, 0);
    unsigned char *b = (unsigned char *)data + 12;
    for (int i = 0; i < (size == 16 ? 4 : 8); i++)
        if (mbtn_down(i))
            b[i] |= 0x80;
}

/* buffered reads (GetDeviceData): the events a game that reads buffered data would miss. dev_kind 1 = keyboard, 2 = mouse.
 * DIDEVICEOBJECTDATA: dwOfs, dwData, dwTimeStamp, dwSequence (+ uAppData in the DX8 layout, objsize 20). Returns events appended. */
int inject_apply_data(int dev_kind, void *buf, unsigned objsize, unsigned *count, unsigned room)
{
    if (!used || !buf || !count || objsize < 16)
        return 0;
    unsigned n = *count, added = 0;
    unsigned char *p = (unsigned char *)buf;
    DWORD now = GetTickCount();
    if (dev_kind == 1) {
        for (int i = 0; i < 256 && n + added < room; i++) {
            unsigned char d = (unsigned char)key_down(i);
            if (d != key_prev[i]) {
                DWORD *e = (DWORD *)(p + (size_t)(n + added) * objsize);
                memset(e, 0, objsize);
                e[0] = (DWORD)i, e[1] = d ? 0x80 : 0, e[2] = now;
                key_prev[i] = d, added++;
            }
        }
    } else if (dev_kind == 2) {
        LONG v[3] = {InterlockedExchange(&mouse_dx, 0), InterlockedExchange(&mouse_dy, 0), InterlockedExchange(&mouse_dz, 0)};
        for (int i = 0; i < 3; i++)
            if (v[i] && n + added < room) {
                DWORD *e = (DWORD *)(p + (size_t)(n + added) * objsize);
                memset(e, 0, objsize);
                e[0] = (DWORD)(i * 4), e[1] = (DWORD)v[i], e[2] = now;
                added++;
            }
        for (int i = 0; i < 8 && n + added < room; i++) {
            unsigned char d = (unsigned char)mbtn_down(i);
            if (d != mbtn_prev[i]) {
                DWORD *e = (DWORD *)(p + (size_t)(n + added) * objsize);
                memset(e, 0, objsize);
                e[0] = 12 + (DWORD)i, e[1] = d ? 0x80 : 0, e[2] = now;
                mbtn_prev[i] = d, added++;
            }
        }
    }
    *count = n + added;
    return (int)added;
}

/* GetAsyncKeyState: 0x8000 while an injected key with that virtual-key code is down */
int inject_vk_down(int vk)
{
    if (!used)
        return 0;
    for (unsigned i = 0; i < sizeof keytab / sizeof keytab[0]; i++)
        if (keytab[i].vk == vk && key_down(keytab[i].dik))
            return 1;
    return 0;
}

void inject_vk_from_dik(int dik, int *vk)
{
    *vk = 0;
    for (unsigned i = 0; i < sizeof keytab / sizeof keytab[0]; i++)
        if (keytab[i].dik == dik)
            *vk = keytab[i].vk;
}
