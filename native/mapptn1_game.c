/*
 * mapptn1_game.c - the glue of the minimap partner patch (mapptn1.c) to the running game: process memory, the REV1_MAPPTN variable, the split screen
 * (split.c) and the network session (net1.c), one pass per logic frame, the evidence log and the console command `mapptn`.
 *
 * Off by default: a single-player game runs on the original bytes. The patch is written while a co-op game runs (a split screen that is requested and
 * not blocked, or a network session that has STARTED), REV1_MAPPTN=1 (always) / 0 (never), or by the console (`mapptn apply` / `revert` / `auto`).
 * Evidence (live proof): while the patch is on, each minimap set (the stock one and the split's second one) logs its partner icon when the icon
 * appears / disappears / changes type, and every 10 s while it is shown: `mapptn: minimap (stock|second) shows the partner: icon type T at map (x, y),
 * partner unit U (slot S, world x, y, z); own unit O (world x, y, z)`.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"
#include "capture.h"
#include "net1.h"
#include "split.h"
#include "mapptn1.h"

static mp1_t G;
static int inited, last_want = -1;
static CRITICAL_SECTION cs;

static int m_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    return r1_safe_read((const void *)(uintptr_t)a, out, n);
}

static int m_write(void *u, uint32_t a, const void *p, uint32_t n)
{
    (void)u;
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(uintptr_t)a, p, n);
    VirtualProtect((void *)(uintptr_t)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)a, n);
    return 1;
}

static uint32_t m_alloc(void *u, uint32_t n)
{
    (void)u;
    return (uint32_t)(uintptr_t)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void m_log(void *u, const char *line)
{
    (void)u;
    r1_log("%s", line);
}

static const mp1_mem MEM = {m_read, m_write, m_alloc, m_log, NULL};

static uint32_t rd(uint32_t a)
{
    uint32_t v = 0;
    return a && r1_safe_read((const void *)(uintptr_t)a, &v, 4) ? v : 0;
}

static float rf(uint32_t a)
{
    float v = 0;
    return a && r1_safe_read((const void *)(uintptr_t)a, &v, 4) ? v : 0.0f;
}

/* a heap field of the game (a GUI node): written only when it reads back (the node lives as long as its GUI, which called us) */
static void wr(uint32_t a, const void *p, unsigned n)
{
    uint32_t probe;
    if (a && r1_safe_read((const void *)(uintptr_t)a, &probe, 4))
        memcpy((void *)(uintptr_t)a, p, n);
}

static int split_on(void)
{
    split_state_t st;
    split_get_state(&st);
    return st.requested && !st.blocked;
}

/* ---- evidence: what each minimap set shows as its partner (uGUI_Map +0x3A8 map position, +0x3B8 icon type; the units for the record) ---- */
static int ev_on = 1;
static struct { uint32_t type; DWORD t_log; } ev[2];
static unsigned ev_lines;

/* the units the minimap of `own` (the slot the set answers for) looks at: own = the player unit of that slot, partner = the first other player unit */
static void units_of(int own, uint32_t *own_u, uint32_t *ptn_u)
{
    *own_u = *ptn_u = 0;
    uint32_t sp = rd(MP1_SPLAYER);
    if (!sp)
        return;
    uint32_t node = rd(sp + 0x30), end = rd(sp + 0x34);   /* FUN_0041BC10's list: node +0x10 unit, +0x08 next */
    for (int n = 0; node && node != end && n < 16; n++, node = rd(node + 8)) {
        uint32_t u = rd(node + 0x10);
        if (!u)
            continue;
        int slot = (int)rd(u + 0xE2C);
        if (slot == own && !*own_u)
            *own_u = u;
        else if (slot != own && !*ptn_u)
            *ptn_u = u;
    }
}

static void ev_set(int k, uint32_t mm, int own)
{
    uint32_t map = rd(mm + 0x48);
    if (!map)
        return;
    uint32_t type = rd(map + 0x3B8);
    DWORD now = GetTickCount();
    if (type == ev[k].type && (!type || now - ev[k].t_log < 10000))
        return;
    uint32_t ou, pu;
    units_of(own, &ou, &pu);
    if (type)
        r1_log("mapptn: minimap (%s) shows the partner: icon type %u at map (%.1f, %.1f), partner unit %08x (slot %d, world %.0f, %.0f, %.0f); own unit %08x (slot %d, world %.0f, %.0f, %.0f)",
               k ? "second" : "stock", type, rf(map + 0x3A8), rf(map + 0x3AC), pu, pu ? (int)rd(pu + 0xE2C) : -1, rf(pu + 0x40), rf(pu + 0x44), rf(pu + 0x48), ou, own, rf(ou + 0x40), rf(ou + 0x44),
               rf(ou + 0x48));
    else
        r1_log("mapptn: minimap (%s) shows NO partner icon (was type %u); own slot %d, partner unit %08x", k ? "second" : "stock", ev[k].type, own, pu);
    ev[k].type = type, ev[k].t_log = now;
    ev_lines++;
}

static void evidence(void)
{
    uint32_t ck = rd(MP1_COCKPIT);
    if (!ck)
        return;
    int own = capture_own_effective();
    own = own > 0 ? 1 : 0;
    ev_set(0, rd(ck + 0x2C), own);
    uint32_t smm = split_second_minimap();
    if (smm)
        ev_set(1, smm, 1 - own);
}

/* ---- the map screen (BACK): the partner marker on the 3D world map (mapptn1.h) ---- */
#define TC __attribute__((thiscall))
static int wm_want;                    /* the co-op patch is wanted (set by the frame callback) */
static int wm_part = 7;                /* LAB `mapptn wmpart <0-8>`: which stock part is cloned for the marker (7 = the player's own marker) */
static uint32_t wm_gui, wm_clone, wm_vt_orig;
static int wm_vt_on, wm_shown = -1;
static unsigned wm_clones, wm_places, wm_lines;
static void (TC *wm_orig8)(void *);

static void wm_after(uint32_t gui)
{
    if (!wm_want) {
        if (wm_clone && gui == wm_gui && wm_shown != 0) {
            ((void (TC *)(uint32_t, uint32_t, int))(uintptr_t)MP1_WM_VIS)(gui, wm_clone, 0);
            wm_shown = 0;
        }
        return;
    }
    uint32_t src = rd(gui + (wm_part <= 5 ? 0x274 + 0x20 * (uint32_t)wm_part : wm_part == 6 ? 0x4F4 : wm_part == 8 ? 0x794 : 0x774));
    if (!rd(gui + 0x774) || !src)
        return;   /* the GUI is not loaded yet */
    if (gui != wm_gui) {   /* a new map GUI (the cockpit is rebuilt by every load): clone the part once for it */
        wm_gui = gui, wm_shown = -1;
        wm_clone = ((uint32_t (TC *)(uint32_t, uint32_t))(uintptr_t)MP1_WM_CLONE)(gui, src);
        if (wm_clone) {
            ((void (TC *)(uint32_t, uint32_t))(uintptr_t)MP1_WM_ADD)(gui, wm_clone);
            wm_clones++;
        }
        r1_log("mapptn: map screen: partner marker %s for the 3D map GUI %08x (a clone of part %d %08x -> %08x)", wm_clone ? "made" : "NOT made", gui, wm_part, src, wm_clone);
    }
    if (!wm_clone)
        return;
    uint8_t on = 0;
    r1_safe_read((const void *)(uintptr_t)(gui + 0x770), &on, 1);
    float pos[4] = {0, 0, 0, 0};
    int own = capture_own_effective() > 0 ? 1 : 0;
    /* FUN_008D30D0 is a method of the world-map model (the stock caller FUN_007C10D0 passes [uSubMenuManager +0x7C]); called without it the first live
     * try (split mm1s, 00:09:44) read a garbage pointer at exe+4D3265 and the game died */
    uint32_t ck = rd(MP1_COCKPIT), smm = ck ? rd(ck + MP1_SUBMENU_OFF) : 0, model = smm ? rd(smm + 0x7C) : 0;
    if (on && smm && rd(smm + 0x78) == gui && model && rd(model + 0x110)) {   /* answers for "the local player": for this call the partner (this thread only) */
        capture_own_scope(1 - own);
        ((void (TC *)(uint32_t, float *))(uintptr_t)MP1_WM_POS)(model, pos);
        capture_own_scope(-1);
    }
    int show = mp1_wm_show(1, on, pos), same = 0;
    if (show) {
        mp1_wm_slot s;
        memset(&s, 0, sizeof s);
        s.on = 1, s.part = wm_clone;
        memcpy(s.pos, pos, sizeof s.pos);
        ((void (TC *)(uint32_t, mp1_wm_slot *))(uintptr_t)MP1_WM_PLACE)(gui, &s);
        wm_places++;
        float ownp[3] = {rf(gui + 0x780), rf(gui + 0x784), rf(gui + 0x788)};   /* the stock player slot +0x770: pos at +0x10 */
        same = mp1_wm_same_spot(pos, ownp);
        if (same) {   /* (one room: the marker beside the player's own, not hidden under it) */
            float x = rf(wm_clone + 0xA0) + MP1_WM_SAME_DX;
            wr(wm_clone + 0xA0, &x, 4);
            uint32_t fl = rd(wm_clone + 0x54) | 0x10000u;
            wr(wm_clone + 0x54, &fl, 4);
        }
    } else if (wm_shown != 0) {
        ((void (TC *)(uint32_t, uint32_t, int))(uintptr_t)MP1_WM_VIS)(gui, wm_clone, 0);
    }
    if (show != wm_shown) {
        wm_lines++;
        r1_log("mapptn: map screen: partner marker %s (own slot %d, partner slot %d, partner at world-map (%.1f, %.1f, %.1f), the player marker %s at (%.1f, %.1f, %.1f)%s; screen (%.0f, %.0f))", show ? "SHOWN" : "hidden", own,
               1 - own, pos[0], pos[1], pos[2], on ? "shown" : "off", rf(gui + 0x780), rf(gui + 0x784), rf(gui + 0x788), same ? ", the same room: drawn beside it" : "",
               rf(wm_clone + 0xA0), rf(wm_clone + 0xA4));
        wm_shown = show;
    }
}

static void TC wm_vf8(void *self)
{
    wm_orig8(self);
    wm_after((uint32_t)(uintptr_t)self);
}

/* installed at the first co-op frame and kept (the wrapper hides the clone itself when co-op ends: the stock refresh resets only its own slots) */
static void wm_install(void)
{
    uint32_t slot = MP1_WM_VTABLE + 8 * 4, cur = rd(slot), mine = (uint32_t)(uintptr_t)wm_vf8;
    if (wm_vt_on || wm_vt_orig)
        return;
    if (cur != MP1_WM_VF8) {
        r1_log("mapptn: map screen: vtable slot %08x is %08x, not %08x: no partner marker", slot, cur, MP1_WM_VF8);
        wm_vt_orig = cur ? cur : 1;
        return;
    }
    wm_orig8 = (void (TC *)(void *))(uintptr_t)MP1_WM_VF8;
    if (m_write(NULL, slot, &mine, 4)) {
        wm_vt_on = 1;
        r1_log("mapptn: map screen: uGUI_WorldMapIcon vf8 wrapped (the partner marker on the 3D map)");
    }
}

static void mapptn_frame(void)
{
    EnterCriticalSection(&cs);
    int net = net1_session_active(), spl = split_on();
    int want = mp1_want(&G, spl, net);
    if (want && (last_want != 1 || G.state != MP1_ON)) {
        if (last_want != 1 || G.state == MP1_OFF)   /* off -> on: write, or refuse once and say why (no retry every frame) */
            mp1_apply(&G, &MEM);
    } else if (!want && last_want != 0) {
        mp1_revert(&G, &MEM);
    }
    last_want = want;
    wm_want = want && G.state == MP1_ON;
    if (wm_want)
        wm_install();
    if (ev_on && (G.state == MP1_ON || spl || net))   /* also without the patch in a co-op game: the stock "no partner" is logged too (before / after) */
        evidence();
    LeaveCriticalSection(&cs);
}

int mapptn_init(void)
{
    if (inited)
        return 0;
    inited = 1;
    InitializeCriticalSection(&cs);
    char v[16];
    int env = r1_env("REV1_MAPPTN", v, sizeof v) && v[0] ? atoi(v) != 0 : -1;
    mp1_init(&G, env);
    lt_add_frame_cb(mapptn_frame);
    r1_log("mapptn: module ready (REV1_MAPPTN=%d; the HUD minimap shows the co-op partner: the patch at %08x is written only while a split screen or a network session runs)", env, MP1_SITE);
    return 0;
}

void mapptn_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "";
    int n = 0;
    sscanf(arg, "%15s %d", sub, &n);
    EnterCriticalSection(&cs);
    int net = net1_session_active(), spl = split_on();
    if (!inited) {
        snprintf(out, size, "mapptn: not initialised");
    } else if (!strcmp(sub, "apply")) {
        G.mode = MP1_MODE_ON;
        mp1_apply(&G, &MEM);
        last_want = 1;
        mp1_status(&G, &MEM, spl, net, out, (size_t)size);
    } else if (!strcmp(sub, "revert")) {
        G.mode = MP1_MODE_OFF;
        mp1_revert(&G, &MEM);
        last_want = 0;
        mp1_status(&G, &MEM, spl, net, out, (size_t)size);
    } else if (!strcmp(sub, "auto")) {
        G.mode = MP1_MODE_AUTO;
        mp1_status(&G, &MEM, spl, net, out, (size_t)size);
    } else if (!strcmp(sub, "wmpart")) {   /* LAB: mapptn wmpart <0-8>: which stock part of the 3D map is cloned for the partner marker (next map GUI) */
        if (n >= 0 && n <= 8) wm_part = n;
        wm_gui = 0;   /* clone again at the next update */
        snprintf(out, size, "mapptn: map screen partner marker = a clone of part %d (made at the next map update)", wm_part);
    } else if (!strcmp(sub, "log")) {
        ev_on = n != 0;
        snprintf(out, size, "mapptn: evidence lines %s (%u so far)", ev_on ? "on" : "off", ev_lines);
    } else if (!sub[0] || !strcmp(sub, "status")) {
        int o = 0;
        mp1_status(&G, &MEM, spl, net, out, (size_t)size);
        o = (int)strlen(out);
        o += snprintf(out + o, size - o, "\n  map screen: vf8 wrap %s, GUI %08x, marker clone %08x of part %d (%u made), placed %u times, now %s",
                      wm_vt_on ? "ON" : (wm_vt_orig ? "REFUSED" : "off"), wm_gui, wm_clone, wm_part, wm_clones, wm_places, wm_shown == 1 ? "shown" : "hidden");
        uint32_t ck = rd(MP1_COCKPIT), smm = split_second_minimap();
        uint32_t sets[2] = {ck ? rd(ck + 0x2C) : 0, smm};
        for (int k = 0; k < 2 && o < size; k++) {
            uint32_t map = sets[k] ? rd(sets[k] + 0x48) : 0;
            if (map)
                o += snprintf(out + o, size - o, "\n  minimap %s %08x: uGUI_Map %08x partner icon type %u at map (%.1f, %.1f), own at map (%.1f, %.1f)", k ? "second" : "stock", sets[k], map,
                              rd(map + 0x3B8), rf(map + 0x3A8), rf(map + 0x3AC), rf(map + 0x388), rf(map + 0x38C));
        }
    } else {
        snprintf(out, size, "mapptn status | apply (force on) | revert (force off) | auto (co-op decides) | log 0|1 (evidence lines)");
    }
    LeaveCriticalSection(&cs);
}
