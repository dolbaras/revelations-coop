/*
 * Hot-loadable plug-in: split screen prototype (stage 5). Loaded with the console "plug load <path>/r1split_N.dll", commands "P split <sub> ...".
 *
 * What it does (notes\stage5.md): the engine already renders a viewport array. sCamera ([0x11E00B8], class sGameCamera) owns 8 viewport objects (stride 0x190
 * at +0x30); the scene pass 0x9AE4C0 runs once per ACTIVE viewport of the display, each with its own camera object, rect (from the layout byte vp+0x13, by
 * 0x9AB1A0: 2 top, 3 bottom, 4 left, 5 right half), matrices, frustum and shadow view. The stock game switches on viewport 0 only (camera = the first
 * uCameraManage [sCamera+0xCE0]) and also creates a SECOND uCameraManage [sCamera+0xCE4] that the player code uses for the character that is not the own slot.
 * This plug-in switches on viewport 2 with a camera object, sets both layout bytes and keeps them for as long as it is on.
 * It installs NO inline hook; it writes plain bytes once per logic pass (on_frame) and puts everything back on "off" / unload.
 *
 *   P split probe            read-only dump of viewports, camera managers, GUI rect
 *   P split hud              read-only dump of the HUD managers (sIDCockpit) with their view masks
 *   P split on [dup|mgr1] [v|h]   dup: viewport 2 shows the SAME camera as viewport 0 (tests the engine path alone), mgr1 (default): viewport 2 = the second
 *                            camera manager with the partner (unit slot 1) as target. v = side by side (modes 4/5, default), h = top / bottom (2/3)
 *   P split off              back to the stock single view
 *   P split lay v|h          change the layout while on
 *   P split cam dup|mgr1     change the camera source while on
 *   P split vp <i> <field> <value>   raw write of a viewport byte (field: active layer mode display), for experiments (decimal)
 *   P split follow on|off    mgr1 target follows the partner (slot 1) every pass (default on)
 *   P split status           one line
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "r1.h"

static const r1_api_t *A;

#ifndef CAMROOT_PTR
#define CAMROOT_PTR   0x11E00B8u
#endif
#ifndef SPLAYER_PTR
#define SPLAYER_PTR   0x1169394u
#endif
#ifndef GAMESYS_PTR
#define GAMESYS_PTR   0x1169108u
#endif
#ifndef COOPMGR_PTR
#define COOPMGR_PTR   0x1169398u
#endif
#ifndef RENDER_PTR
#define RENDER_PTR    0x11E1B9Cu
#endif
#ifndef CS_FLAG_GLOB
#define CS_FLAG_GLOB  0x11E04ECu
#endif
#define VP_STRIDE     0x190u
#define VP_BASE       0x30u
#define VP_SECOND     2

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    A->safe_read((void *)a, &v, 4);
    return v;
}
static unsigned rd8(unsigned a)
{
    unsigned char v = 0;
    A->safe_read((void *)a, &v, 1);
    return v;
}
static float rdf(unsigned a)
{
    float v = 0;
    A->safe_read((void *)a, &v, 4);
    return v;
}
static int wr(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)a, p, n, &got) && got == n;
}
static int wr32(unsigned a, unsigned v) { return wr(a, &v, 4); }
static int wr8(unsigned a, unsigned v)
{
    unsigned char c = (unsigned char)v;
    return wr(a, &c, 1);
}

static unsigned root(void) { return rd32(CAMROOT_PTR); }
static unsigned vp_addr(unsigned r, unsigned i) { return r + VP_BASE + VP_STRIDE * i; }

static void cname(unsigned obj, char *out, int size)
{
    if (!obj) {
        snprintf(out, (size_t)size, "-");
        return;
    }
    if (!A->object_name(obj, out, size))
        snprintf(out, (size_t)size, "?");
}

static int is_player_class(unsigned unit)
{
    char cn[96];
    return unit && A->object_name(unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4);
}

/* the player unit with this slot field (+0xE2C), from the sPlayer list 0 (same walk as plug_cap.c) */
static unsigned unit_by_slot(unsigned slot)
{
    unsigned sp = rd32(SPLAYER_PTR);
    if (!sp)
        return 0;
    unsigned head = rd32(sp + 0x30), endp = rd32(sp + 0x34);
    for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8)) {
        unsigned unit = rd32(head + 0x10);
        if (unit && rd32(unit + 0xE2C) == slot && is_player_class(unit))
            return unit;
    }
    return 0;
}

/* campaign only: the Raid (game mode != 0 or sCoopManager+0xF4 in {1,2}) is never touched */
static int raid_active(void)
{
    unsigned gs = rd32(GAMESYS_PTR);
    unsigned data = gs ? rd32(gs + 0x24) : 0;
    unsigned mode = data ? rd32(data + 0x20) : 0;
    unsigned cm = rd32(COOPMGR_PTR);
    unsigned pm = cm ? rd32(cm + 0xF4) : 0xFFFFFFFFu;
    return mode != 0 || pm == 1 || pm == 2;
}

/* ---- state ---- */
static int on;                 /* split requested */
static int src_mgr1 = 1;       /* 1: viewport 2 = second camera manager, 0: same camera as viewport 0 */
static int vertical = 1;       /* 1: side by side (modes 4 / 5), 0: top / bottom (2 / 3) */
static int follow = 1;         /* mgr1 target follows the partner */
static int have_saved;
static struct { unsigned cam, act, mode, disp, layer, mode0, act0; } sv;
static unsigned n_pass, n_reasserted, n_nocam, n_raid, n_susp;
static unsigned last_cam2, last_mgr1;
static int suspended;          /* viewport 0 shows something else than the gameplay camera (cutscene / title camera): whole view meanwhile */

static void remember(unsigned r)
{
    if (have_saved)
        return;
    unsigned v2 = vp_addr(r, VP_SECOND), v0 = vp_addr(r, 0);
    sv.cam = rd32(v2 + 4), sv.act = rd8(v2 + 0x10), sv.mode = rd8(v2 + 0x13), sv.disp = rd8(v2 + 0x14), sv.layer = rd8(v2 + 0x12);
    sv.mode0 = rd8(v0 + 0x13), sv.act0 = rd8(v0 + 0x10);
    have_saved = 1;
}

static void restore(void)
{
    unsigned r = root();
    if (!r || !have_saved)
        return;
    unsigned v2 = vp_addr(r, VP_SECOND), v0 = vp_addr(r, 0);
    wr8(v2 + 0x10, 0);
    wr32(v2 + 4, 0);
    wr8(v2 + 0x13, sv.mode);
    wr8(v2 + 0x14, sv.disp);
    wr8(v2 + 0x12, sv.layer);
    wr8(v0 + 0x13, sv.mode0);
    have_saved = 0;
}

/* one logic pass, before the frame: make the viewport state what we want. The engine rewrites the rects itself every frame from the layout bytes. */
static void apply(void)
{
    unsigned r = root();
    if (!on || !r)
        return;
    n_pass++;
    if (raid_active()) {
        n_raid++;
        A->log("plug_split: Raid detected, split screen switched off");
        restore();
        on = 0;
        return;
    }
    unsigned mgr0 = rd32(r + 0xCE0), mgr1 = rd32(r + 0xCE4);
    unsigned want = src_mgr1 ? mgr1 : mgr0;
    if (!want || !mgr0) {
        n_nocam++;
        return;
    }
    remember(r);
    unsigned v2 = vp_addr(r, VP_SECOND), v0 = vp_addr(r, 0);
    /* The demo scheduler (cutscenes) and the title put their own camera object on viewport 0 (0x891E07: it keeps the old one in this+0x38 and restores it
     * afterwards). While viewport 0 does not show manager 0, the view is whole: layout 0 and viewport 2 off; the split comes back by itself. */
    /* LIVE (2026-11-04): the pause menu and the game-over screen are GUI units (sIDCockpit managers 2 = uGUI_GameOverHD, 3 = uGUI_PauseHD) that the engine draws in
     * EVERY active viewport (view mask 0xF), i.e. squeezed into both halves. They are "running" when the unit flags have 0x400 | 0x800 set (pause: 0x60B2 -> 0x6CB2).
     * Whole view meanwhile. */
    int full_gui = 0;
    {
        unsigned ck = rd32(0x1169120);
        for (unsigned k = 2; ck && k <= 3; k++) {
            unsigned g = rd32(ck + 0x28 + 4 * k);
            if (g && (rd32(g + 0xC) & 0xC00) == 0xC00)
                full_gui = 1;
        }
    }
    if (full_gui || rd32(v0 + 4) != mgr0) {
        if (!suspended) {
            suspended = 1;
            n_susp++;
            A->log("plug_split: whole view (%s): viewport 0 camera %08x, manager 0 %08x", full_gui ? "pause / game over screen" : "cutscene or other camera on viewport 0", rd32(v0 + 4), mgr0);
        }
        wr8(v2 + 0x10, 0);
        wr8(v0 + 0x13, sv.mode0);
        wr8(v2 + 0x13, sv.mode);
        return;
    }
    if (suspended) {
        suspended = 0;
        A->log("plug_split: gameplay view again: split resumes");
    }
    int fix = 0;
    if (rd32(v2 + 4) != want)
        wr32(v2 + 4, want), fix++;
    if (rd8(v2 + 0x10) != 1)
        wr8(v2 + 0x10, 1), fix++;
    if (rd8(v2 + 0x14) != 0)
        wr8(v2 + 0x14, 0), fix++;
    if (rd8(v2 + 0x12) != 5)
        wr8(v2 + 0x12, 5);
    unsigned m2 = vertical ? 5 : 3, m0 = vertical ? 4 : 2;
    if (rd8(v2 + 0x13) != m2)
        wr8(v2 + 0x13, m2), fix++;
    if (rd8(v0 + 0x13) != m0)
        wr8(v0 + 0x13, m0), fix++;
    if (fix && n_pass > 2)
        n_reasserted++;
    last_cam2 = want, last_mgr1 = mgr1;
    if (src_mgr1) {
        /* LIVE (2026-11-04): the engine creates manager 1 with the unit flags 0x400 | 0x800 cleared and never sets them (manager 0 gets them): it is not updated
         * and stays at its default pose (0,200,-700). With both bits set it follows its target like manager 0. */
        unsigned fl = rd32(mgr1 + 0xC);
        if ((fl & 0xC00) != 0xC00)
            wr32(mgr1 + 0xC, fl | 0xC00);
    }
    if (src_mgr1 && follow) {
        unsigned u = unit_by_slot(1);
        if (u && rd32(mgr1 + 0x74) != u) {
            wr32(mgr1 + 0x74, u);
            wr32(mgr1 + 0x78, u);
        }
    }
}

/* ---- H1 experiment: the stock HUD units are drawn once per viewport (view mask 0xF); is their data read at DRAW time? ----
 * Hook on sUnit::draw (0x9B4760, __thiscall(sUnit, cmd), ret 4): while the viewport-2 pass draws, the own slot answers 1 (sCoopManager+0xF4 = 2, +0xF8 = 1; the
 * capture module's own-slot hook falls through to the original code, which reads them), restored right after the call (the draw waits for its worker jobs). */
#define TC __attribute__((thiscall))
typedef struct {
    unsigned target;
    int n;
    unsigned char orig[16];
    void *tramp;
    int on;
} hk_t;

static int hook_install(hk_t *h, unsigned target, const unsigned char *expect, int n, void *hook)
{
    unsigned char cur[16];
    if (!A->safe_read((void *)target, cur, (unsigned)n) || memcmp(cur, expect, (size_t)n) != 0) {
        A->log("plug_split: bytes at %08x differ from the expected build or are hooked already, not patching", target);
        return 0;
    }
    unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t)
        return 0;
    memcpy(t, cur, (size_t)n);
    t[n] = 0xE9;
    *(int *)(t + n + 1) = (int)((target + (unsigned)n) - (unsigned)(t + n + 5));
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    unsigned char *p = (unsigned char *)target;
    memcpy(h->orig, cur, (size_t)n);
    for (int i = 5; i < n; i++)
        p[i] = 0x90;
    *(int *)(p + 1) = (int)((unsigned)hook - (target + 5));
    p[0] = 0xE9;
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    h->target = target, h->n = n, h->tramp = t, h->on = 1;
    return 1;
}

static void hook_remove(hk_t *h)
{
    if (!h->on)
        return;
    DWORD old;
    if (VirtualProtect((void *)h->target, (SIZE_T)h->n, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void *)h->target, h->orig, (size_t)h->n);
        VirtualProtect((void *)h->target, (SIZE_T)h->n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void *)h->target, (SIZE_T)h->n);
    }
    h->on = 0;
}

static hk_t hk_draw;
static void (TC *orig_draw)(unsigned self, unsigned cmd);
static int hud1;               /* the experiment is armed */
static unsigned n_draw_ovr, n_draw_all, n_hud_upd;

static void TC hook_draw(unsigned self, unsigned cmd)
{
    n_draw_all++;
    if (hud1 && on && !suspended && rd8(cmd + 0x158) == VP_SECOND) {
        unsigned cm = rd32(COOPMGR_PTR);
        unsigned f4 = rd32(cm + 0xF4), f8 = rd32(cm + 0xF8);
        wr32(cm + 0xF4, 2);
        wr32(cm + 0xF8, 1);
        n_draw_ovr++;
        if (hud1 >= 2) {   /* H1b: the HUD state is a scratch pad: prepare it for the other player right before this viewport draws (phases 8 and 9 of the main cockpit manager) */
            unsigned ck = rd32(0x1169120), mgr = ck ? rd32(ck + 0x28) : 0;
            if (mgr) {
                ((void (__attribute__((fastcall)) *)(unsigned))0x7B10F0)(mgr);
                ((void (__attribute__((fastcall)) *)(unsigned))0x7B0BA0)(mgr);
                n_hud_upd++;
            }
        }
        orig_draw(self, cmd);
        wr32(cm + 0xF4, f4);
        wr32(cm + 0xF8, f8);
        return;
    }
    orig_draw(self, cmd);
}

/* ---- H2 experiment: a SECOND set of the cockpit (main manager + its children), drawn only in viewport 2 and updated as the other player ----
 * Made with the engine's own calls (sIDCockpit ctor 0x849880 does the same for the stock set): cdecl factory 0x7AFFB0(size 0x98, align 0x10), ctor 0x7AFFE0
 * (__fastcall(this)), children 0x7B06D0 (__fastcall(manager), ends with the child chain build 0x7B0220), then 0x9B5280 on the unit manager [0x11E0184]
 * (__thiscall(sUnit, group 0x16, unit, 0, 0), ret 0x10). View masks (unit+0xC bits 16..25) decide the viewport: the new set gets bit 2 only, the stock main
 * cockpit and the minimap get bit 0 only. The update phases of the new manager (0x7B10F0 / 0x7B0BA0, which also run its children) run with the own slot = 1. */
#define FC __attribute__((fastcall))
static unsigned hud2_mgr;
static hk_t hk_v8h, hk_v9h;
static void (FC *orig_v8h)(unsigned);
static void (FC *orig_v9h)(unsigned);
static unsigned hud2_orig[64][2];   /* unit, flags before we changed the view mask */
static int hud2_norig;
static unsigned n_h2_upd;

static void own_scope_begin(unsigned *f4, unsigned *f8)
{
    unsigned cm = rd32(COOPMGR_PTR);
    *f4 = rd32(cm + 0xF4), *f8 = rd32(cm + 0xF8);
    wr32(cm + 0xF4, 2);
    wr32(cm + 0xF8, 1);
}
static void own_scope_end(unsigned f4, unsigned f8)
{
    unsigned cm = rd32(COOPMGR_PTR);
    wr32(cm + 0xF4, f4);
    wr32(cm + 0xF8, f8);
}
static void FC hook_v8h(unsigned self)
{
    if (hud2_mgr && self == hud2_mgr) {
        unsigned a, b;
        own_scope_begin(&a, &b);
        n_h2_upd++;
        orig_v8h(self);
        own_scope_end(a, b);
        return;
    }
    orig_v8h(self);
}
static void FC hook_v9h(unsigned self)
{
    if (hud2_mgr && self == hud2_mgr) {
        unsigned a, b;
        own_scope_begin(&a, &b);
        orig_v9h(self);
        own_scope_end(a, b);
        return;
    }
    orig_v9h(self);
}

static void set_view_mask(unsigned unit, unsigned mask, int remember)
{
    unsigned fl = rd32(unit + 0xC);
    if (remember && hud2_norig < 64) {
        hud2_orig[hud2_norig][0] = unit, hud2_orig[hud2_norig][1] = fl;
        hud2_norig++;
    }
    wr32(unit + 0xC, (fl & ~0x3FF0000u) | (mask << 16));
}
/* mask on a manager and every child of its chain (+0x3C head, next at +0x270) */
static int set_set_mask(unsigned mgr, unsigned mask, int remember)
{
    int n = 0;
    set_view_mask(mgr, mask, remember);
    for (unsigned c = rd32(mgr + 0x3C); c && n < 40; c = rd32(c + 0x270), n++)
        set_view_mask(c, mask, remember);
    return n;
}

static void hud2_restore_masks(void)
{
    for (int i = 0; i < hud2_norig; i++)
        wr32(hud2_orig[i][0] + 0xC, (rd32(hud2_orig[i][0] + 0xC) & ~0x3FF0000u) | (hud2_orig[i][1] & 0x3FF0000u));
    hud2_norig = 0;
}

static int hud2_make(char *out, int size)
{
    static const unsigned char b_v8[] = {0xA1, 0x10, 0x91, 0x16, 0x01, 0x83, 0xEC, 0x0C};
    static const unsigned char b_v9[] = {0xA1, 0x10, 0x91, 0x16, 0x01, 0x57, 0x8B, 0xF9};
    unsigned ck = rd32(0x1169120), su = rd32(0x11E0184);
    if (!ck || !su || !rd32(ck + 0x28)) {
        snprintf(out, (size_t)size, "hud2: no cockpit yet");
        return 1;
    }
    if (hud2_mgr) {
        snprintf(out, (size_t)size, "hud2: second cockpit exists (%08x)", hud2_mgr);
        return 1;
    }
    if (!hk_v8h.on && !hook_install(&hk_v8h, 0x7B10F0, b_v8, 8, (void *)hook_v8h)) {
        snprintf(out, (size_t)size, "hud2: cannot hook 0x7B10F0");
        return 1;
    }
    orig_v8h = (void (FC *)(unsigned))hk_v8h.tramp;
    if (!hk_v9h.on && !hook_install(&hk_v9h, 0x7B0BA0, b_v9, 8, (void *)hook_v9h)) {
        snprintf(out, (size_t)size, "hud2: cannot hook 0x7B0BA0");
        return 1;
    }
    orig_v9h = (void (FC *)(unsigned))hk_v9h.tramp;
    unsigned mem = ((unsigned (*)(unsigned, unsigned))0x7AFFB0)(0x98, 0x10);
    if (!mem) {
        snprintf(out, (size_t)size, "hud2: allocation failed");
        return 1;
    }
    unsigned m = ((unsigned (FC *)(unsigned))0x7AFFE0)(mem);
    A->log("plug_split: hud2: allocated %08x, constructed %08x", mem, m);
    ((void (FC *)(unsigned))0x7B06D0)(m);
    A->log("plug_split: hud2: children built");
    wr32(m + 0xC, rd32(m + 0xC) | 0x2000);
    hud2_mgr = m;   /* from here on its update phases run as slot 1 */
    int nk = set_set_mask(m, 4, 0);
    ((void (TC *)(unsigned, unsigned, unsigned, unsigned, unsigned))0x9B5280)(su, 0x16, m, 0, 0);
    A->log("plug_split: hud2: registered in group 0x16, %d children, flags %08x", nk, rd32(m + 0xC));
    /* the stock main cockpit and the minimap only in viewport 0 */
    int ns = set_set_mask(rd32(ck + 0x28), 1, 1);
    set_view_mask(rd32(ck + 0x2C), 1, 1);
    snprintf(out, (size_t)size, "hud2: second cockpit %08x made (%d children, view mask bit 2), stock cockpit restricted to viewport 0 (%d children)", m, nk, ns);
    return 1;
}

static int hud1_set(int want)
{
    if (want && !hk_draw.on) {
        static const unsigned char b_draw[] = {0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x57};
        if (!hook_install(&hk_draw, 0x9B4760, b_draw, 7, (void *)hook_draw))
            return 0;
        orig_draw = (void (TC *)(unsigned, unsigned))hk_draw.tramp;
    }
    hud1 = want;
    return 1;
}

/* ---- probe ---- */
static int probe(char *out, int size)
{
    int o = 0;
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    unsigned r = root();
    char cn[96], cn2[96];
    if (!r) {
        snprintf(out, (size_t)size, "no sCamera yet ([0x11E00B8] = 0)");
        return 1;
    }
    cname(r, cn, sizeof cn);
    P("sCamera %08x (%s) window rect (%d,%d)-(%d,%d) clear colour %08x  DAT_0116C3E4=%u DAT_0111BDE8=%u cs=%u/%u\n", r, cn, (int)rd32(r + 0xCB0), (int)rd32(r + 0xCB4), (int)rd32(r + 0xCB8),
      (int)rd32(r + 0xCBC), rd32(r + 0xCD0), rd32(0x116C3E4), rd32(0x111BDE8), rd8(r + 0x1C), rd8(CS_FLAG_GLOB));
    for (unsigned i = 0; i < 5; i++) {
        unsigned v = vp_addr(r, i), cam = rd32(v + 4);
        cname(cam, cn, sizeof cn);
        P(" vp%u @%08x cam %08x %-16s act %u layer %u mode %u disp %u rt %08x rect (%d,%d)-(%d,%d) camstate %u\n", i, v, cam, cn, rd8(v + 0x10), rd8(v + 0x12), rd8(v + 0x13), rd8(v + 0x14), rd32(v + 0xC),
          (int)rd32(v + 0x18), (int)rd32(v + 0x1C), (int)rd32(v + 0x20), (int)rd32(v + 0x24), cam ? (rd32(cam + 0xC) & 7) : 0);
    }
    for (unsigned k = 0; k < 2; k++) {
        unsigned m = rd32(r + 0xCE0 + 4 * k);
        cname(m, cn, sizeof cn);
        P("mgr%u %08x %s flags(+C) %08x state %u\n", k, m, cn, rd32(m + 0xC), rd32(m + 0xC) & 7);
        if (!m)
            continue;
        unsigned t1 = rd32(m + 0x74), t2 = rd32(m + 0x78);
        cname(t1, cn, sizeof cn), cname(t2, cn2, sizeof cn2);
        P("    target +74 %08x %s  +78 %08x %s  pos(%.1f,%.1f,%.1f) +60(%.1f,%.1f,%.1f)\n", t1, cn, t2, cn2, rdf(m + 0x40), rdf(m + 0x44), rdf(m + 0x48), rdf(m + 0x60), rdf(m + 0x64), rdf(m + 0x68));
        P("    cams: count(+D8) %u array(+E4) %08x cur(+CC) %d prev(+D0) %d tps(+E8) %d (+EC) %d flags(+138) %08x  proj near(+30) %.2f far(+34) %.1f aspect(+38) %.3f fov(+3C) %.3f owner(+70) %u\n",
          rd32(m + 0xD8), rd32(m + 0xE4), (int)rd32(m + 0xCC), (int)rd32(m + 0xD0), (int)rd32(m + 0xE8), (int)rd32(m + 0xEC), rd32(m + 0x138), rdf(m + 0x30), rdf(m + 0x34), rdf(m + 0x38),
          rdf(m + 0x3C), rd32(m + 0x70));
        unsigned n = rd32(m + 0xD8), arr = rd32(m + 0xE4);
        for (unsigned j = 0; j < n && j < 8; j++) {
            unsigned c = rd32(arr + 4 * j);
            cname(c, cn, sizeof cn);
            P("      cam[%u] %08x %s\n", j, c, cn);
        }
    }
    unsigned rn = rd32(RENDER_PTR);
    P("sRender %08x: back buffer %dx%d, GUI rect (%d,%d)-(%d,%d) size %dx%d, displays %u\n", rn, (int)rd32(rn + 0x433E30), (int)rd32(rn + 0x433E34), (int)rd32(rn + 0xA4), (int)rd32(rn + 0xA8),
      (int)rd32(rn + 0xAC), (int)rd32(rn + 0xB0), (int)rd32(rn + 0xB8), (int)rd32(rn + 0xBC), rd32(rn + 0x447DB4));
    unsigned u0 = unit_by_slot(0), u1 = unit_by_slot(1);
    cname(u0, cn, sizeof cn), cname(u1, cn2, sizeof cn2);
    P("units: slot0 %08x %s pos(%.0f,%.0f,%.0f)  slot1 %08x %s pos(%.0f,%.0f,%.0f)  raid=%d\n", u0, cn, rdf(u0 + 0x40), rdf(u0 + 0x44), rdf(u0 + 0x48), u1, cn2, rdf(u1 + 0x40), rdf(u1 + 0x44),
      rdf(u1 + 0x48), raid_active());
#undef P
    return 1;
}

/* HUD probe (read-only): the six GUI managers of sIDCockpit ([0x1169120]+0x28..0x3C), their view masks (unit+0xC bits 16..25) and the children of the main cockpit */
static int hud_probe(char *out, int size)
{
    int o = 0;
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    unsigned ck = rd32(0x1169120);
    char cn[96];
    if (!ck) {
        snprintf(out, (size_t)size, "no sIDCockpit yet ([0x1169120] = 0)");
        return 1;
    }
    P("sIDCockpit %08x\n", ck);
    for (unsigned k = 0; k < 6; k++) {
        unsigned u = rd32(ck + 0x28 + 4 * k);
        cname(u, cn, sizeof cn);
        P(" mgr[%u] %08x %-28s flags %08x state %u viewmask %03x\n", k, u, cn, rd32(u + 0xC), rd32(u + 0xC) & 7, (rd32(u + 0xC) >> 16) & 0x3FF);
    }
    unsigned m = rd32(ck + 0x28), n = 0;
    for (unsigned c = rd32(m + 0x3C); c && n < 40; c = rd32(c + 0x270), n++) {
        cname(c, cn, sizeof cn);
        P("   child %08x %-28s flags %08x en(bit14) %u skip(+274) %u viewmask %03x\n", c, cn, rd32(c + 0xC), (rd32(c + 0xC) >> 14) & 1, rd8(c + 0x274), (rd32(c + 0xC) >> 16) & 0x3FF);
    }
    if (hud2_mgr) {
        P(" hud2 manager %08x flags %08x state %u viewmask %03x%s", hud2_mgr, rd32(hud2_mgr + 0xC), rd32(hud2_mgr + 0xC) & 7, (rd32(hud2_mgr + 0xC) >> 16) & 0x3FF, "\n");
        unsigned n2 = 0;
        for (unsigned c = rd32(hud2_mgr + 0x3C); c && n2 < 40; c = rd32(c + 0x270), n2++) {
            cname(c, cn, sizeof cn);
            P("   h2 child %08x %-28s flags %08x viewmask %03x%s", c, cn, rd32(c + 0xC), (rd32(c + 0xC) >> 16) & 0x3FF, "\n");
        }
    }
    P("   (%u children of the main cockpit listed)\n", n);
#undef P
    return 1;
}

static int on_cmd(const char *name, const char *arg, char *out, int size)
{
    if (strcmp(name, "split"))
        return 0;
    char sub[32] = "", a1[32] = "", a2[32] = "", a3[32] = "";
    sscanf(arg ? arg : "", "%31s %31s %31s %31s", sub, a1, a2, a3);
    if (!strcmp(sub, "") || !strcmp(sub, "status")) {
        snprintf(out, (size_t)size, "split %s%s: camera %s, layout %s, follow %s; passes %u, re-asserted %u, no-camera passes %u, suspended %u times, raid stops %u; vp2 camera %08x (mgr1 %08x)", on ? "ON" : "off",
                 suspended ? " (suspended: viewport 0 is not the gameplay camera)" : "", src_mgr1 ? "mgr1 (partner)" : "dup (same as vp0)", vertical ? "side by side" : "top/bottom", follow ? "on" : "off", n_pass,
                 n_reasserted, n_nocam, n_susp, n_raid, last_cam2, last_mgr1);
        return 1;
    }
    if (!strcmp(sub, "probe"))
        return probe(out, size);
    if (!strcmp(sub, "hud"))
        return hud_probe(out, size);
    if (!strcmp(sub, "hud2")) {   /* hud2 make | hud2 restore */
        if (!strcmp(a1, "make"))
            return hud2_make(out, size);
        if (!strcmp(a1, "restore")) {
            hud2_restore_masks();
            if (hud2_mgr)
                set_set_mask(hud2_mgr, 0, 0);
            snprintf(out, (size_t)size, "hud2: stock view masks restored, second cockpit hidden (mask 0); extra updates so far %u", n_h2_upd);
            return 1;
        }
        snprintf(out, (size_t)size, "hud2 make | hud2 restore (second cockpit %08x, updates %u)", hud2_mgr, n_h2_upd);
        return 1;
    }
    if (!strcmp(sub, "hud1")) {   /* hud1 on|off: own slot = 1 while the viewport-2 pass draws (H1 experiment) */
        int want = !strcmp(a1, "off") ? 0 : !strcmp(a1, "2") ? 2 : 1;
        int ok = hud1_set(want);
        snprintf(out, (size_t)size, "hud1 %s%s (draw hook %s, draws %u, overridden %u, extra cockpit updates %u)", want ? (want == 2 ? "2 (own slot + extra cockpit update)" : "1 (own slot only)") : "off", ok ? "" : " FAILED",
                 hk_draw.on ? "installed" : "not installed", n_draw_all, n_draw_ovr, n_hud_upd);
        return 1;
    }
    if (!strcmp(sub, "on")) {
        for (const char *p = a1; p; p = (p == a1) ? a2 : NULL) {
            if (!strcmp(p, "dup"))
                src_mgr1 = 0;
            else if (!strcmp(p, "mgr1"))
                src_mgr1 = 1;
            else if (!strcmp(p, "v"))
                vertical = 1;
            else if (!strcmp(p, "h"))
                vertical = 0;
        }
        if (raid_active()) {
            snprintf(out, (size_t)size, "refused: Raid / game mode != 0");
            return 1;
        }
        n_pass = n_reasserted = n_nocam = n_susp = 0;
        suspended = 0;
        on = 1;
        apply();
        snprintf(out, (size_t)size, "split ON (%s, %s); camera objects: mgr0 %08x mgr1 %08x", src_mgr1 ? "mgr1" : "dup", vertical ? "side by side" : "top/bottom", rd32(root() + 0xCE0), rd32(root() + 0xCE4));
        return 1;
    }
    if (!strcmp(sub, "off")) {
        on = 0;
        restore();
        snprintf(out, (size_t)size, "split off (viewport 2 inactive, layout bytes restored)");
        return 1;
    }
    if (!strcmp(sub, "lay")) {
        vertical = a1[0] != 'h';
        snprintf(out, (size_t)size, "layout %s", vertical ? "side by side" : "top/bottom");
        return 1;
    }
    if (!strcmp(sub, "cam")) {
        src_mgr1 = strcmp(a1, "dup") != 0;
        snprintf(out, (size_t)size, "camera source %s", src_mgr1 ? "mgr1" : "dup");
        return 1;
    }
    if (!strcmp(sub, "follow")) {
        follow = strcmp(a1, "off") != 0;
        snprintf(out, (size_t)size, "mgr1 follow %s", follow ? "on" : "off");
        return 1;
    }
    if (!strcmp(sub, "vp")) {   /* vp <i> <field> <value> */
        unsigned r = root(), i = (unsigned)atoi(a1);
        int v = atoi(a3);
        if (!r || i > 7) {
            snprintf(out, (size_t)size, "usage: split vp <0-7> active|layer|mode|display <value>");
            return 1;
        }
        unsigned off = !strcmp(a2, "active") ? 0x10 : !strcmp(a2, "layer") ? 0x12 : !strcmp(a2, "mode") ? 0x13 : !strcmp(a2, "display") ? 0x14 : 0;
        if (!off) {
            snprintf(out, (size_t)size, "usage: split vp <0-7> active|layer|mode|display <value>");
            return 1;
        }
        wr8(vp_addr(r, i) + off, (unsigned)v);
        snprintf(out, (size_t)size, "vp%u %s = %d (the plug-in's own pass may overwrite it while split is on)", i, a2, v);
        return 1;
    }
    snprintf(out, (size_t)size, "split: probe | hud | on [dup|mgr1] [v|h] | off | lay v|h | cam dup|mgr1 | follow on|off | vp <i> <field> <v> | status");
    return 1;
}

static void on_frame_(void) { apply(); }

static void on_exit_(void)
{
    hud1 = 0;
    hud2_restore_masks();
    if (hud2_mgr)
        set_set_mask(hud2_mgr, 0, 0);
    hook_remove(&hk_draw);
    hook_remove(&hk_v8h);
    hook_remove(&hk_v9h);
    if (on || have_saved) {
        on = 0;
        restore();
    }
    A->log("plug_split: unloaded, viewport 2 off, layout bytes restored");
}

__declspec(dllexport) int plug_init(const r1_api_t *api, r1_plug_t *out)
{
    A = api;
    A->log("plug_split: loaded (api v%u); no hook installed, bytes only", A->version);
    out->on_cmd = on_cmd;
    out->on_frame = on_frame_;
    out->on_exit = on_exit_;
    return 0;
}
