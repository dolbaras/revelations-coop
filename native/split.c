/*
 * split.c - local split screen of Revelations 1 (see split.h for the model and the API; notes\stage5.md for the evidence, all of it from the live game).
 *
 * Views: the engine's own viewport array. Viewport 0 keeps the first camera manager (left / top), viewport 2 is switched on with the second manager
 * ([sCamera+0xCE4], which the player code already uses for the character that is not the own slot) and both layout bytes are set (4/5 side by side, 2/3 top / bottom).
 * The engine rewrites the rects from the layout bytes every frame and sets every camera's aspect from its rect, so nothing is composed or stretched by us. The stock
 * game never enables manager 1 (unit flags 0x400|0x800 stay cleared), this module sets them and points its target at the other human's unit.
 *
 * HUD: the cockpit units are drawn once per active viewport (view mask in unit+0xC bits 16..25), but the data is prepared at UPDATE time from "the local player" (the own
 * slot), so one set shows one human. A second set (uCockpitManagerMain + uMiniMapManager, built with the engine's own factories/constructors/child builders and
 * registered in the cockpit unit group 0x16) gets view mask 4 (viewport 2 only), the stock managers get mask 1 (viewport 0 only), and the second set's update phases run with
 * the own slot answering the OTHER human (capture_own_scope: for the calling thread only). The new blocks are zero-filled (the stock set was built into zeroed memory; the
 * constructors rely on it). The second set's vtables are private copies with slots 8/9 wrapped: no code is patched anywhere in this module.
 *
 * Rules: whole view (layout 0, viewport 2 off) while viewport 0 does not show the gameplay camera (cutscene, title) or a full-screen GUI is running (pause, game over,
 * item menu). The second human's BACK / START opens HIS menu: pads 0/1 are exchanged at the XInput level, the own slot and the capture roles are flipped until the menu
 * closes (the menus read pad slot 0 as "the local human"). Raid: the module never touches it (split ends, HUD set destroyed, resumes when the campaign is back).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r1.h"
#include "capture.h"
#include "split.h"

#define TC __attribute__((thiscall))
#define FC __attribute__((fastcall))

static split_addr_t AD = {0x11E00B8u, 0x1169394u, 0x1169108u, 0x1169398u, 0x1169120u, 0x11E0184u, 0x7AFFB0u, 0x7AFFE0u, 0x7B06D0u, 0x7BA440u, 0x7BA470u, 0x7BA4F0u, 0x9B5280u, 0x11859D0u};

void split_test_set_addrs(const split_addr_t *a) { AD = *a; }

#define VP_STRIDE 0x190u
#define VP_BASE 0x30u
#define VP_SECOND 2
#define COCKPIT_GROUP 0x16u

/* ---- memory helpers ---- */
static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((void *)(size_t)a, &v, 4);
    return v;
}
static unsigned rd8(unsigned a)
{
    unsigned char v = 0;
    r1_safe_read((void *)(size_t)a, &v, 1);
    return v;
}
static float rdf(unsigned a)
{
    float v = 0;
    r1_safe_read((void *)(size_t)a, &v, 4);
    return v;
}
static int wr(unsigned a, const void *p, unsigned n)
{
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void *)(size_t)a, p, n, &got) && got == n;
}
static int wr32(unsigned a, unsigned v) { return wr(a, &v, 4); }
static int wr8(unsigned a, unsigned v)
{
    unsigned char c = (unsigned char)v;
    return wr(a, &c, 1);
}

static unsigned root(void) { return rd32(AD.camroot_ptr); }
static unsigned vp_addr(unsigned r, unsigned i) { return r + VP_BASE + VP_STRIDE * i; }

static int is_player_class(unsigned unit)
{
    char cn[96];
    return unit > 0x10000 && classes_object_name(unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4);
}

/* the player unit with this slot field (+0xE2C), from the sPlayer list 0 (same walk as capture.c) */
static unsigned unit_by_slot(unsigned slot)
{
    unsigned sp = rd32(AD.splayer_ptr);
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
    unsigned gs = rd32(AD.gamesys_ptr);
    unsigned data = gs ? rd32(gs + 0x24) : 0;
    unsigned mode = data ? rd32(data + 0x20) : 0;
    unsigned cm = rd32(AD.coopmgr_ptr);
    unsigned pm = cm ? rd32(cm + 0xF4) : 0xFFFFFFFFu;
    return mode != 0 || pm == 1 || pm == 2;
}

/* is `unit` in the unit group (the engine frees the units of a group on a load: a stale pointer must not be called) */
static int in_group(unsigned group, unsigned unit)
{
    unsigned su = rd32(AD.unitmgr_ptr);
    if (!su || !unit)
        return 0;
    unsigned u = rd32(su + 0x2C + 0x18 * group);
    for (int guard = 0; u && guard < 200; guard++, u = rd32(u + 0x14))
        if (u == unit)
            return 1;
    return 0;
}

/* ---- state ---- */
static volatile int want_on;              /* split_enable(1) */
static volatile int layout_req = SPLIT_SIDE_BY_SIDE;
static volatile int hud_req = 1;
static volatile int view_req;             /* per-view units of the second human (action prompts, message boxes) are made for viewport 2: experimental, console "split view on" */
static int active, blocked, suspended;
static int have_saved;
static struct {
    unsigned cam, act, mode, disp, layer, mode0;
} sv;
static unsigned n_pass, n_reasserted, n_susp, n_builds, last_cam2, last_mgr1;
static int inited;

static void remember(unsigned r)
{
    if (have_saved)
        return;
    unsigned v2 = vp_addr(r, VP_SECOND), v0 = vp_addr(r, 0);
    sv.cam = rd32(v2 + 4), sv.act = rd8(v2 + 0x10), sv.mode = rd8(v2 + 0x13), sv.disp = rd8(v2 + 0x14), sv.layer = rd8(v2 + 0x12);
    sv.mode0 = rd8(v0 + 0x13);
    have_saved = 1;
}

static void restore_views(void)
{
    unsigned r = root();
    if (r && have_saved) {
        unsigned v2 = vp_addr(r, VP_SECOND), v0 = vp_addr(r, 0);
        wr8(v2 + 0x10, 0);
        wr32(v2 + 4, 0);
        wr8(v2 + 0x13, sv.mode);
        wr8(v2 + 0x14, sv.disp);
        wr8(v2 + 0x12, sv.layer);
        wr8(v0 + 0x13, sv.mode0);
    }
    have_saved = 0;
}

/* ====================================================================== the second HUD set ====================================================================== */
/* LIVE (2026-11-04): the stock children are built into zeroed memory (their constructors rely on it) while a later allocation reuses freed heap blocks full of 0x82
 * bytes; the cockpit draw loop 0x7B0070 skips a child whose byte +0x274 is not 0. Every cUnit factory allocates through MtDefaultAllocator vtable slot 7 (0x8DD9E0,
 * thiscall(this, size, align, tag), ret 0xC). While the second set is built, the slot is wrapped (vtable pointer = data, no code patch) and the blocks of THIS thread are
 * zero-filled before they are returned. */
static void *(TC *orig_alloc7)(void *self, unsigned size, unsigned align, unsigned tag);
static volatile DWORD zero_tid;
static unsigned n_zeroed, n_zbytes;

static void *TC hook_alloc7(void *self, unsigned size, unsigned align, unsigned tag)
{
    void *p = orig_alloc7(self, size, align, tag);
    if (p && zero_tid && GetCurrentThreadId() == zero_tid && size && size <= 0x100000u) {
        memset(p, 0, size);
        n_zeroed++, n_zbytes += size;
    }
    return p;
}

/* The allocator of a class is picked by an index in its DTI (flags +0x18 >> 23 & 0x3F, 12 for the cockpit classes) from the table at 0x11859D0; the objects are of DIFFERENT
 * classes (MtDefaultAllocator vtable 0x109DC7C for index 0, MtScalableAllocator 0x109D1A8 for index 12), all with the same slot 7 stub 0x8DD9E0. So every distinct allocator
 * vtable of the table gets its slot 7 wrapped for the duration of the build (found live: wrapping only the default allocator's vtable zero-filled nothing). */
#define ALLOC_STUB 0x8DD9E0u
#define ALLOC_MAXVT 8
static unsigned alloc_slots[ALLOC_MAXVT];
static int n_alloc_slots;

static int alloc_wrap(int enable)
{
    DWORD old;
    if (!AD.alloc_table)
        return 1;   /* harness: nothing to wrap */
    if (!enable) {
        for (int i = 0; i < n_alloc_slots; i++)
            if (VirtualProtect((void *)(size_t)alloc_slots[i], 4, PAGE_READWRITE, &old)) {
                *(unsigned *)(size_t)alloc_slots[i] = ALLOC_STUB;
                VirtualProtect((void *)(size_t)alloc_slots[i], 4, old, &old);
            }
        n_alloc_slots = 0;
        return 1;
    }
    if (n_alloc_slots)
        return 1;
    for (unsigned k = 0; k < 64 && n_alloc_slots < ALLOC_MAXVT; k++) {
        unsigned obj = rd32(AD.alloc_table + 4 * k), vt = obj ? rd32(obj) : 0, slot = vt + 0x1C;
        int dup = 0;
        for (int i = 0; i < n_alloc_slots; i++)
            dup |= alloc_slots[i] == slot;
        if (!vt || dup || rd32(slot) != ALLOC_STUB)
            continue;
        if (!VirtualProtect((void *)(size_t)slot, 4, PAGE_READWRITE, &old))
            continue;
        orig_alloc7 = (void *(TC *)(void *, unsigned, unsigned, unsigned))(size_t)ALLOC_STUB;
        *(unsigned *)(size_t)slot = (unsigned)(size_t)hook_alloc7;
        VirtualProtect((void *)(size_t)slot, 4, old, &old);
        alloc_slots[n_alloc_slots++] = slot;
    }
    if (!n_alloc_slots)
        r1_log("split: no allocator vtable with the expected slot 7 stub %08x found: the second HUD set is not zero-filled", ALLOC_STUB);
    return 1;
}

static struct {
    unsigned cp, mm;               /* the second set's managers (0 = none) */
    unsigned stock_cp, stock_mm;   /* the stock managers it was built next to */
    unsigned stock_orig[2][2];     /* unit, flags of the stock managers before their view mask was restricted */
    int n_orig;
    unsigned build_tick;
    int failed;
} hud;

/* private vtable copies: slots 8 and 9 (update phases) run with the own slot answering the other human */
#define VT_SLOTS 20
static unsigned *vt_cp, *vt_mm;
static void (FC *orig_cp8)(unsigned), (FC *orig_cp9)(unsigned), (FC *orig_mm8)(unsigned), (FC *orig_mm9)(unsigned);

static void own_scope_begin(void) { capture_own_scope(1 - capture_own_effective()); }
static void own_scope_end(void) { capture_own_scope(-1); }
static void FC wrap_cp8(unsigned self)
{
    own_scope_begin();
    orig_cp8(self);
    own_scope_end();
}
static void FC wrap_cp9(unsigned self)
{
    own_scope_begin();
    orig_cp9(self);
    own_scope_end();
}
static void FC wrap_mm8(unsigned self)
{
    own_scope_begin();
    orig_mm8(self);
    own_scope_end();
}
static void FC wrap_mm9(unsigned self)
{
    own_scope_begin();
    orig_mm9(self);
    own_scope_end();
}

static unsigned *vt_copy(unsigned obj, void *w8, void *w9, void (FC **o8)(unsigned), void (FC **o9)(unsigned), unsigned **store)
{
    if (!*store) {
        unsigned vt = rd32(obj);
        unsigned *c = VirtualAlloc(NULL, VT_SLOTS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!c || !vt)
            return NULL;
        for (int i = 0; i < VT_SLOTS; i++)
            c[i] = rd32(vt + 4u * (unsigned)i);
        *o8 = (void (FC *)(unsigned))(size_t)c[8];
        *o9 = (void (FC *)(unsigned))(size_t)c[9];
        c[8] = (unsigned)(size_t)w8;
        c[9] = (unsigned)(size_t)w9;
        *store = c;
    }
    wr32(obj, (unsigned)(size_t)*store);
    return *store;
}

static void set_view_mask(unsigned unit, unsigned mask)
{
    unsigned fl = rd32(unit + 0xC);
    wr32(unit + 0xC, (fl & ~0x3FF0000u) | (mask << 16));
}
/* the main cockpit manager: children in a chain (head +0x3C, next at child +0x270); the minimap manager: four units at +0x30.. */
static void set_cp_mask(unsigned mgr, unsigned mask)
{
    int n = 0;
    set_view_mask(mgr, mask);
    for (unsigned c = rd32(mgr + 0x3C); c && n < 40; c = rd32(c + 0x270), n++)
        set_view_mask(c, mask);
}
static void set_mm_mask(unsigned mgr, unsigned mask)
{
    set_view_mask(mgr, mask);
    for (unsigned k = 0; k < 4; k++) {
        unsigned c = rd32(mgr + 0x30 + 4 * k);
        if (c)
            set_view_mask(c, mask);
    }
}

static void hud_restore_stock(void)
{
    for (int i = 0; i < hud.n_orig; i++)
        if (hud.stock_orig[i][0] == hud.stock_cp || hud.stock_orig[i][0] == hud.stock_mm) {   /* only while the stock manager is still the one we changed */
            unsigned u = hud.stock_orig[i][0];
            wr32(u + 0xC, (rd32(u + 0xC) & ~0x3FF0000u) | (hud.stock_orig[i][1] & 0x3FF0000u));
        }
    hud.n_orig = 0;
}

static void hud_destroy(const char *why)
{
    if (hud.cp || hud.mm)
        r1_log("split: second HUD set removed (%s)", why);
    unsigned set[2] = {hud.cp, hud.mm};
    for (int i = 0; i < 2; i++) {
        unsigned u = set[i];
        if (u && in_group(COCKPIT_GROUP, u)) {   /* the engine's own way to retire a unit: clear "persistent", ask for removal (state 3), the unit sweep frees it */
            wr32(u + 0xC, rd32(u + 0xC) & ~0x2000u);
            ((void (TC *)(unsigned))(size_t)rd32(rd32(u) + 0x3C))(u);
        }
    }
    hud_restore_stock();
    memset(&hud.cp, 0, sizeof hud.cp * 4);   /* cp, mm, stock_cp, stock_mm */
}

static int hud_build(void)
{
    unsigned ck = rd32(AD.cockpit_ptr), su = rd32(AD.unitmgr_ptr);
    unsigned scp = ck ? rd32(ck + 0x28) : 0, smm = ck ? rd32(ck + 0x2C) : 0;
    if (!ck || !su || !scp || !smm || (rd32(scp + 0xC) & 7) != 2 || (rd32(smm + 0xC) & 7) != 2)
        return 0;   /* the stock set is not up yet (loading) */
    if (!alloc_wrap(1))
        return 0;
    zero_tid = GetCurrentThreadId();
    n_zeroed = n_zbytes = 0;
    unsigned mem = ((unsigned (*)(unsigned, unsigned))(size_t)AD.fn_cp_alloc)(0x98, 0x10);
    unsigned cp = mem ? ((unsigned (FC *)(unsigned))(size_t)AD.fn_cp_ctor)(mem) : 0;
    if (cp)
        ((void (FC *)(unsigned))(size_t)AD.fn_cp_kids)(cp);
    unsigned mmem = cp ? ((unsigned (*)(unsigned, unsigned))(size_t)AD.fn_mm_alloc)(0x50, 0x10) : 0;
    unsigned mm = mmem ? ((unsigned (FC *)(unsigned))(size_t)AD.fn_mm_ctor)(mmem) : 0;
    if (mm)
        ((void (FC *)(unsigned))(size_t)AD.fn_mm_kids)(mm);
    zero_tid = 0;
    alloc_wrap(0);
    if (!cp || !mm) {
        r1_log("split: second HUD set could not be allocated (cp %08x mm %08x)", cp, mm);
        hud.failed = 1;
        return 0;
    }
    wr32(cp + 0xC, rd32(cp + 0xC) | 0x2000);
    wr32(mm + 0xC, rd32(mm + 0xC) | 0x2000);
    if (!vt_copy(cp, (void *)wrap_cp8, (void *)wrap_cp9, &orig_cp8, &orig_cp9, &vt_cp) || !vt_copy(mm, (void *)wrap_mm8, (void *)wrap_mm9, &orig_mm8, &orig_mm9, &vt_mm)) {
        r1_log("split: private vtable copy failed");
        hud.failed = 1;
        return 0;
    }
    set_cp_mask(cp, 4);
    set_mm_mask(mm, 4);
    hud.cp = cp, hud.mm = mm, hud.stock_cp = scp, hud.stock_mm = smm;
    ((void (TC *)(unsigned, unsigned, unsigned, unsigned, unsigned))(size_t)AD.fn_register)(su, COCKPIT_GROUP, cp, 0, 0);
    ((void (TC *)(unsigned, unsigned, unsigned, unsigned, unsigned))(size_t)AD.fn_register)(su, COCKPIT_GROUP, mm, 0, 0);
    /* the stock managers only in viewport 0 (their children keep their masks: the managers' own mask decides whether their draw runs in a viewport) */
    hud.stock_orig[0][0] = scp, hud.stock_orig[0][1] = rd32(scp + 0xC);
    hud.stock_orig[1][0] = smm, hud.stock_orig[1][1] = rd32(smm + 0xC);
    hud.n_orig = 2;
    set_view_mask(scp, 1);
    set_view_mask(smm, 1);
    hud.build_tick = GetTickCount();
    n_builds++;
    r1_log("split: second HUD set built: cockpit %08x minimap %08x (%u blocks / %u bytes zero-filled), stock set %08x / %08x restricted to viewport 0", cp, mm, n_zeroed, n_zbytes, scp, smm);
    return 1;
}

/* LIVE CRASH (2026-11-04 03:34, session s5b): the pause menu's "load the last checkpoint" tears the area down (units, GUI resources) a few frames after the menu closes; the
 * second set, alive and updating, hit an engine assertion (0x401396, inside the GUI update chain, esi = the second cockpit manager) and the game died. The same can happen on
 * any load. So the set does not live through anything that can load: it is RETIRED when a pause / game over screen opens or a cutscene starts (those are the doors to a load),
 * and it is (re)built only after the world has been unchanged for hud_settle_ms: same stock cockpit managers, camera managers and player units, gameplay camera on viewport 0. */
static unsigned hud_settle_ms = 1500;
static unsigned stable_key[6];
static DWORD stable_since;

static void hud_forget(void)
{
    hud.n_orig = 0;
    memset(&hud.cp, 0, sizeof hud.cp * 4);   /* cp, mm, stock_cp, stock_mm */
}

static void hud_unstable(void)
{
    memset(stable_key, 0, sizeof stable_key);
    stable_since = 0;
}

/* once per pass while the split is on and the gameplay view is shown */
static void hud_frame(unsigned mgr0, unsigned mgr1)
{
    unsigned ck = rd32(AD.cockpit_ptr), scp = ck ? rd32(ck + 0x28) : 0, smm = ck ? rd32(ck + 0x2C) : 0;
    unsigned key[6] = {scp, smm, mgr0, mgr1, unit_by_slot(0), unit_by_slot(1)};
    DWORD now = GetTickCount();
    int ok = 1;
    for (int i = 0; i < 6; i++)
        ok &= key[i] != 0;
    if (!ok || memcmp(key, stable_key, sizeof key) != 0) {   /* the world changed: nothing of the old set may be touched any more */
        if (hud.cp || hud.mm) {
            if (in_group(COCKPIT_GROUP, hud.cp) || in_group(COCKPIT_GROUP, hud.mm))
                hud_destroy("the world changed (cockpit, cameras or characters)");
            else {
                r1_log("split: second HUD set gone with its group (the engine freed it), forgotten");
                hud_forget();
            }
        }
        memcpy(stable_key, key, sizeof key);
        stable_since = now;
        hud.failed = 0;
        return;
    }
    if (hud.cp || hud.mm) {
        if (!in_group(COCKPIT_GROUP, hud.cp) || !in_group(COCKPIT_GROUP, hud.mm)) {
            r1_log("split: second HUD set is not in the cockpit group any more, forgotten");
            hud_forget();
        }
        return;
    }
    if (!hud.failed && now - stable_since >= hud_settle_ms) {
        static DWORD last_try;
        if (now - last_try > 500 || last_try == 0) {   /* a build that could not start yet (stock set not running) is retried every half second */
            if (!hud_build())
                last_try = now ? now : 1;
            else
                last_try = 0;
        }
    }
}

/* ====================================================================== menus of the second human ====================================================================== */
/* The item / map / pause menus are GUI units that read pad slot 0 and the own-slot human ("the local human"). The human on pad slot 0 is the local human of the split (left /
 * top view, his character = the own slot); for the other human (pad slot 1) the menus run in an EXCHANGE: pads 0/1 swapped at the XInput level, the capture roles' pad slots
 * swapped, the own slot = his character, until the menu GUI has closed; then all three are put back as they were (any lobby layout: Jill left, or Parker left with own slot 1). */
static struct {
    int on;
    unsigned prev_btn;
    DWORD t_enter;
    int seen_gui, quiet;
    int kind;   /* 1 BACK, 2 START */
    int old_pad[2];            /* capture roles' pad slots before the exchange, by unit slot */
    int old_own_on, old_own;   /* own-slot override before the exchange */
} mx;
static unsigned n_mx;

static void menu_enter(int kind)
{
    capture_state_t st;
    capture_get_state(&st);
    int p0 = st.role[0].pad, p1 = st.role[1].pad;
    if (!st.role[0].enabled || !st.role[1].enabled || p0 == p1 || p0 < 0 || p0 > 1 || p1 < 0 || p1 > 1) {
        r1_log("split: second human's menu requested but the two characters are not bound to the two pad slots: left alone");
        return;
    }
    int other_unit = p0 == 1 ? 0 : 1;   /* the character played on pad slot 1 */
    mx.old_pad[0] = p0, mx.old_pad[1] = p1;
    mx.old_own_on = st.own_on, mx.old_own = (int)st.own_slot;
    testmode_pad_swap(1);
    capture_bind(0, 1 - p0);
    capture_bind(1, 1 - p1);
    capture_set_own(other_unit);
    mx.on = 1, mx.kind = kind, mx.t_enter = GetTickCount(), mx.seen_gui = 0, mx.quiet = 0;
    n_mx++;
    r1_log("split: second human opens his %s: pads exchanged, own slot %d, roles flipped", kind == 1 ? "item menu (BACK)" : "pause menu (START)", other_unit);
}

static void menu_leave(const char *why)
{
    capture_set_own(mx.old_own_on ? mx.old_own : -1);
    capture_bind(0, mx.old_pad[0]);
    capture_bind(1, mx.old_pad[1]);
    testmode_pad_swap(0);
    mx.on = 0;
    r1_log("split: second human's menu over (%s): pads, own slot and roles back to the split layout", why);
}

/* a full-screen GUI that the engine draws in every viewport is running: managers 2 (game over), 3 (pause) have unit flags 0xC00 set while they run (live), manager 5
 * (item / map menu manager) while its bit 0x800 is set (static: its update only runs with 0x4800; unverified live) */
static int gui_running(void)   /* 1 = pause / game over (a load can follow), 2 = item / map menu (verified live: flags 0x4800 while open), 0 = none */
{
    unsigned ck = rd32(AD.cockpit_ptr);
    for (unsigned k = 2; ck && k <= 3; k++) {
        unsigned g = rd32(ck + 0x28 + 4 * k);
        if (g && (rd32(g + 0xC) & 0xC00) == 0xC00)
            return 1;
    }
    unsigned sub = ck ? rd32(ck + 0x3C) : 0;
    if (sub && (rd32(sub + 0xC) & 0x4800) == 0x4800)
        return 2;
    return 0;
}

static void menu_frame(int gui)
{
    if (!mx.on) {
        unsigned char gp[12];   /* XINPUT_GAMEPAD: wButtons first */
        memset(gp, 0, sizeof gp);
        unsigned b = 0;
        if (testmode_pad_peek(1, gp))
            b = *(unsigned short *)gp;
        unsigned edge = b & ~mx.prev_btn;
        mx.prev_btn = b;
        if (!gui && !suspended && active && capture_hooks_ready()) {
            if (edge & 0x0020)
                menu_enter(1);   /* BACK */
            else if (edge & 0x0010)
                menu_enter(2);   /* START */
        }
        return;
    }
    if (gui)
        mx.seen_gui = 1, mx.quiet = 0;
    else
        mx.quiet++;
    if (mx.seen_gui && mx.quiet > 8)
        menu_leave("menu closed");
    else if (!mx.seen_gui && GetTickCount() - mx.t_enter > 2500)
        menu_leave("no menu opened");
}

/* ====================================================================== viewports ====================================================================== */
static void teardown(const char *why)
{
    if (mx.on)
        menu_leave(why);
    hud_destroy(why);
    restore_views();
    if (active)
        r1_log("split: ended (%s)", why);
    active = 0;
    suspended = 0;
    capture_ext_request(0);
    capture_set_view_index(0, 0);
    capture_set_view_index(1, 0);
}

void split_frame(void)
{
    unsigned r = root();
    n_pass++;
    if (!inited)
        return;
    if (raid_active()) {
        if (active || have_saved || hud.cp || mx.on)
            teardown("Raid detected");
        blocked = SPLIT_BLOCK_RAID;
        return;
    }
    blocked = SPLIT_BLOCK_NONE;
    if (!want_on || !r) {
        if (active || have_saved || hud.cp || mx.on)
            teardown(want_on ? "no camera root" : "switched off");
        return;
    }
    unsigned mgr0 = rd32(r + 0xCE0), mgr1 = rd32(r + 0xCE4);
    if (!mgr0 || !mgr1)
        return;   /* no episode loaded yet */
    remember(r);
    unsigned v2 = vp_addr(r, VP_SECOND), v0 = vp_addr(r, 0);
    int vertical = layout_req == SPLIT_SIDE_BY_SIDE;   /* the engine's vertical divider: left / right halves */
    int gui = gui_running();
    int cutscene = rd32(v0 + 4) != mgr0;
    if (!active)
        r1_log("split: starting (%s, manager 0 %08x manager 1 %08x)", vertical ? "side by side" : "top/bottom", mgr0, mgr1);
    active = 1;
    capture_ext_request(hud_req || view_req);
    {
        int own = capture_own_effective();
        capture_set_view_index(own, 0);
        capture_set_view_index(1 - own, view_req && !mx.on ? VP_SECOND : 0);
    }
    menu_frame(gui);
    if (mx.on)
        gui = gui ? gui : 3;   /* the second human's menu is about to open: whole view from the first pass on (own slot flips, the cameras would swap sides) */
    /* The demo scheduler (cutscenes) and the title put their own camera object on viewport 0 (0x891E07 keeps the old one and restores it afterwards); the pause menu, the
     * game over screen and the item menu are GUI units the engine draws in EVERY viewport (squeezed into both halves). Whole view meanwhile; the split comes back by itself. */
    if (gui || cutscene) {
        if (!suspended) {
            suspended = gui ? SPLIT_SUSP_GUI : SPLIT_SUSP_CAMERA;
            n_susp++;
            r1_log("split: whole view (%s)", gui ? "pause / game over / item menu" : "cutscene or other camera on viewport 0");
        }
        if ((gui == 1 || (!gui && cutscene)) && (hud.cp || hud.mm))   /* the doors to a load: the second HUD set must not be alive when the engine tears the area down */
            hud_destroy(gui == 1 ? "pause / game over screen opened" : "cutscene");
        if (gui == 1 || (!gui && cutscene))
            hud_unstable();
        wr8(v2 + 0x10, 0);
        wr8(v0 + 0x13, sv.mode0);
        wr8(v2 + 0x13, sv.mode);
        return;
    }
    if (suspended) {
        suspended = 0;
        r1_log("split: gameplay view again, split resumes");
    }
    int fix = 0;
    if (rd32(v2 + 4) != mgr1)
        wr32(v2 + 4, mgr1), fix++;
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
    last_cam2 = mgr1, last_mgr1 = mgr1;
    /* LIVE: the engine creates manager 1 with the unit flags 0x400|0x800 cleared and never sets them (manager 0 gets them): it is not updated and stays at its default pose.
     * With both bits set it follows its target like manager 0. */
    unsigned fl = rd32(mgr1 + 0xC);
    if ((fl & 0xC00) != 0xC00)
        wr32(mgr1 + 0xC, fl | 0xC00);
    /* manager 1 follows the human that is NOT the own slot (the player code talks to manager 1 for that character) */
    unsigned other = unit_by_slot((unsigned)(1 - capture_own_effective()));
    if (other && rd32(mgr1 + 0x74) != other) {
        wr32(mgr1 + 0x74, other);
        wr32(mgr1 + 0x78, other);
    }
    if (hud_req)
        hud_frame(mgr0, mgr1);
    else if (hud.cp || hud.mm)
        hud_destroy("second HUD switched off");
}

/* ====================================================================== API ====================================================================== */
int split_init(void)
{
    if (inited)
        return 1;
    inited = 1;
    lt_add_frame_cb(split_frame);
    r1_log("split: module ready (nothing is touched until split_enable(1) / console \"split on\")");
    return 1;
}

void split_enable(int on) { want_on = on != 0; }

/* Emergency stop from ANY thread (console word "splitkill": works when the game thread is stuck and the normal "split off" cannot run): the request is cleared and the viewport bytes
 * are put back with plain writes; the HUD set, the roles and the own slot are cleaned up by the game thread's next pass if it runs again. */
void split_emergency_off(void)
{
    want_on = 0;
    restore_views();
    testmode_pad_swap(0);
}
void split_set_layout(int layout) { layout_req = layout == SPLIT_TOP_BOTTOM ? SPLIT_TOP_BOTTOM : SPLIT_SIDE_BY_SIDE; }
void split_set_hud(int on) { hud_req = on != 0; }
int split_is_active(void) { return active && !suspended; }

void split_get_state(split_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->requested = want_on, st->active = active, st->blocked = blocked, st->suspended = suspended, st->layout = layout_req;
    st->hud_set = hud.cp != 0, st->menu_exchange = mx.on;
    st->passes = n_pass, st->reasserted = n_reasserted, st->suspensions = n_susp, st->hud_builds = n_builds;
}

void split_status_text(char *out, int size)
{
    split_state_t s;
    split_get_state(&s);
    snprintf(out, (size_t)size, "split %s%s%s: layout %s, second HUD %s (cockpit %08x minimap %08x, %u builds, %u blocks zero-filled), menu exchange %s (%u so far); passes %u, re-asserted %u, whole-view episodes %u; vp2 camera %08x",
             want_on ? "ON" : "off", blocked == SPLIT_BLOCK_RAID ? " (blocked: Raid)" : "", suspended ? (suspended == SPLIT_SUSP_GUI ? " (whole view: menu)" : " (whole view: cutscene)") : "",
             layout_req == SPLIT_SIDE_BY_SIDE ? "side by side" : "top/bottom", hud_req ? (hud.cp ? "up" : "waiting") : "off", hud.cp, hud.mm, n_builds, n_zeroed, mx.on ? "OPEN" : "idle", n_mx, n_pass, n_reasserted,
             n_susp, last_cam2);
}

/* ---- probes (read-only) ---- */
static void cname(unsigned obj, char *out, int size)
{
    if (!obj) {
        snprintf(out, (size_t)size, "-");
        return;
    }
    if (!classes_object_name(obj, out, size))
        snprintf(out, (size_t)size, "?");
}

static int probe(char *out, int size)
{
    int o = 0;
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    unsigned r = root();
    char cn[96], cn2[96];
    if (!r) {
        snprintf(out, (size_t)size, "no sCamera yet");
        return 1;
    }
    P("sCamera %08x window rect (%d,%d)-(%d,%d)\n", r, (int)rd32(r + 0xCB0), (int)rd32(r + 0xCB4), (int)rd32(r + 0xCB8), (int)rd32(r + 0xCBC));
    for (unsigned i = 0; i < 5; i++) {
        unsigned v = vp_addr(r, i), cam = rd32(v + 4);
        cname(cam, cn, sizeof cn);
        P(" vp%u cam %08x %-16s act %u layer %u mode %u disp %u rect (%d,%d)-(%d,%d)\n", i, cam, cn, rd8(v + 0x10), rd8(v + 0x12), rd8(v + 0x13), rd8(v + 0x14), (int)rd32(v + 0x18), (int)rd32(v + 0x1C),
          (int)rd32(v + 0x20), (int)rd32(v + 0x24));
    }
    for (unsigned k = 0; k < 2; k++) {
        unsigned m = rd32(r + 0xCE0 + 4 * k);
        P("mgr%u %08x flags %08x target %08x pos(%.0f,%.0f,%.0f) aspect %.3f fov %.1f\n", k, m, rd32(m + 0xC), rd32(m + 0x74), rdf(m + 0x40), rdf(m + 0x44), rdf(m + 0x48), rdf(m + 0x38), rdf(m + 0x3C));
    }
    unsigned u0 = unit_by_slot(0), u1 = unit_by_slot(1);
    cname(u0, cn, sizeof cn), cname(u1, cn2, sizeof cn2);
    P("units: slot0 %08x %s pos(%.0f,%.0f,%.0f) hp %.0f  slot1 %08x %s pos(%.0f,%.0f,%.0f) hp %.0f  own %d raid=%d\n", u0, cn, rdf(u0 + 0x40), rdf(u0 + 0x44), rdf(u0 + 0x48), rdf(u0 + 0xE38), u1, cn2, rdf(u1 + 0x40),
      rdf(u1 + 0x44), rdf(u1 + 0x48), rdf(u1 + 0xE38), capture_own_effective(), raid_active());
#undef P
    return 1;
}

static int hud_probe(char *out, int size)
{
    int o = 0;
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    unsigned ck = rd32(AD.cockpit_ptr);
    char cn[96];
    if (!ck) {
        snprintf(out, (size_t)size, "no sIDCockpit yet");
        return 1;
    }
    P("sIDCockpit %08x\n", ck);
    for (unsigned k = 0; k < 6; k++) {
        unsigned u = rd32(ck + 0x28 + 4 * k);
        cname(u, cn, sizeof cn);
        P(" mgr[%u] %08x %-24s flags %08x state %u viewmask %03x\n", k, u, cn, rd32(u + 0xC), rd32(u + 0xC) & 7, (rd32(u + 0xC) >> 16) & 0x3FF);
    }
    unsigned sets[2] = {rd32(ck + 0x28), hud.cp};
    for (int s = 0; s < 2; s++) {
        if (!sets[s])
            continue;
        P(" %s children of %08x:\n", s ? "SECOND set" : "stock", sets[s]);
        unsigned n = 0;
        for (unsigned c = rd32(sets[s] + 0x3C); c && n < 40; c = rd32(c + 0x270), n++) {
            cname(c, cn, sizeof cn);
            P("   %08x %-26s flags %08x en %u skip(+274) %02x gui(+280) %08x mask %03x\n", c, cn, rd32(c + 0xC), (rd32(c + 0xC) >> 14) & 1, rd8(c + 0x274), rd32(c + 0x280), (rd32(c + 0xC) >> 16) & 0x3FF);
        }
    }
    if (hud.mm)
        P(" second minimap %08x flags %08x mask %03x\n", hud.mm, rd32(hud.mm + 0xC), (rd32(hud.mm + 0xC) >> 16) & 0x3FF);
#undef P
    return 1;
}

void split_cmd(const char *arg, char *out, int size)
{
    char sub[32] = "", a1[32] = "", a2[32] = "", a3[32] = "";
    sscanf(arg ? arg : "", "%31s %31s %31s %31s", sub, a1, a2, a3);
    if (!strcmp(sub, "") || !strcmp(sub, "status")) {
        split_status_text(out, size);
    } else if (!strcmp(sub, "probe")) {
        probe(out, size);
    } else if (!strcmp(sub, "hud")) {
        if (!strcmp(a1, "on") || !strcmp(a1, "off")) {
            split_set_hud(!strcmp(a1, "on"));
            snprintf(out, (size_t)size, "second HUD set %s", hud_req ? "on" : "off");
        } else if (!strcmp(a1, "rebuild")) {
            if (hud.cp)
                hud_destroy("console rebuild");
            hud.failed = 0;
            snprintf(out, (size_t)size, "second HUD set will be rebuilt at the next pass");
        } else {
            hud_probe(out, size);
        }
    } else if (!strcmp(sub, "on")) {
        if (!strcmp(a1, "h"))
            split_set_layout(SPLIT_TOP_BOTTOM);
        else if (!strcmp(a1, "v"))
            split_set_layout(SPLIT_SIDE_BY_SIDE);
        if (raid_active()) {
            snprintf(out, (size_t)size, "refused: Raid / game mode != 0");
            return;
        }
        split_enable(1);
        snprintf(out, (size_t)size, "split ON requested (%s); the viewports switch on at the next logic pass", layout_req == SPLIT_SIDE_BY_SIDE ? "side by side" : "top/bottom");
    } else if (!strcmp(sub, "off")) {
        split_enable(0);
        snprintf(out, (size_t)size, "split off requested (viewport 2 inactive, layout bytes, view masks and the second HUD set are put back at the next pass)");
    } else if (!strcmp(sub, "lay")) {
        split_set_layout(a1[0] == 'h' ? SPLIT_TOP_BOTTOM : SPLIT_SIDE_BY_SIDE);
        snprintf(out, (size_t)size, "layout %s", layout_req == SPLIT_SIDE_BY_SIDE ? "side by side" : "top/bottom");
    } else if (!strcmp(sub, "view")) {   /* view on|off: DAT_0116C3E4 = 2 while the second human's character runs (his action prompts / message boxes are made for viewport 2) */
        if (!strcmp(a1, "on") || !strcmp(a1, "off"))
            view_req = !strcmp(a1, "on");
        snprintf(out, (size_t)size, "per-view units of the second human: %s", view_req ? "viewport 2" : "stock (viewport 0)");
    } else if (!strcmp(sub, "menu")) {   /* menu 1|2 : force the second human's BACK / START exchange; menu off */
        if (!strcmp(a1, "off")) {
            if (mx.on)
                menu_leave("console");
            snprintf(out, (size_t)size, "menu exchange %s", mx.on ? "still on" : "off");
        } else {
            menu_enter(atoi(a1) == 2 ? 2 : 1);
            snprintf(out, (size_t)size, "menu exchange %s", mx.on ? "ON (leaves by itself when the menu has closed)" : "refused (see log)");
        }
    } else if (!strcmp(sub, "vp")) {   /* vp <i> <field> <value> (experiments, decimal) */
        unsigned r = root(), i = (unsigned)atoi(a1);
        int v = atoi(a3);
        unsigned off = !strcmp(a2, "active") ? 0x10 : !strcmp(a2, "layer") ? 0x12 : !strcmp(a2, "mode") ? 0x13 : !strcmp(a2, "display") ? 0x14 : 0;
        if (!r || i > 7 || !off) {
            snprintf(out, (size_t)size, "usage: split vp <0-7> active|layer|mode|display <value>");
            return;
        }
        wr8(vp_addr(r, i) + off, (unsigned)v);
        snprintf(out, (size_t)size, "vp%u %s = %d (the module's own pass may overwrite it while split is on)", i, a2, v);
    } else {
        snprintf(out, (size_t)size, "split: status | on [v|h] | off | lay v|h | hud [on|off|rebuild] | view [on|off] | menu <1=BACK|2=START|off> | probe | vp <i> <field> <v>");
    }
}
