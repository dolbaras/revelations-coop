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
#include <math.h>

#include "r1.h"
#include "capture.h"
#include "split.h"
#include "solo1.h"
#include "mapptn1.h"
#include "aimray1.h"
#include "fallres1.h"
#include "leash1.h"
#include "descent1.h"
#include "downed1.h"
#include "flash2.h"

#define TC __attribute__((thiscall))
#define FC __attribute__((fastcall))

static split_addr_t AD = {0x11E00B8u, 0x1169394u, 0x1169108u, 0x1169398u, 0x1169120u, 0x11E0184u, 0x7AFFB0u, 0x7AFFE0u, 0x7B06D0u, 0x7BA440u, 0x7BA470u, 0x7BA4F0u, 0x9B5280u, 0x11859D0u};
static int harness;   /* split_test_set_addrs was called: fake memory, no code is written (the aim ray's decision still runs) */

void split_test_set_addrs(const split_addr_t *a) { AD = *a; harness = 1; }

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
static int wrf(unsigned a, float v) { return wr(a, &v, 4); }
static float fov_req;   /* console `split fov <degrees>`: vertical FOV written into both camera managers (+0x3C) every pass; 0 = the engine's own (stage 6c experiment: a side by side half has a horizontal FOV of 32 degrees) */
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

/* a live camera manager: class uCameraManage and a unit state of 1 / 2 (a freed or zeroed object after the episode ended / at the title is neither) */
static int is_cam_mgr(unsigned m)
{
    char cn[96];
    unsigned st = rd32(m + 0xC) & 7;
    return m > 0x10000 && (st == 1 || st == 2) && classes_object_name(m, cn, sizeof cn) && !strcmp(cn, "uCameraManage");
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
/* (minimap1, 2026-11-08) uMapIcon_vf9 (0x7B95E0) and the HUD-hidden test FUN_0079BB80 reach "the minimap" through [sIDCockpit+0x2C] (FUN_00849780), the STOCK
 * manager: without this the second view's icons were computed from the first human's uGUI_Map (his map position, his partner). During the second manager's
 * update phases the cell points at the second manager and is put back right after (only if it still points at it: mapptn1.h mp1_swap_*). */
static unsigned n_mm_swaps;
static int mm_swap_on = 1;   /* LAB `split mmswap 0|1`: 0 = the old behaviour (the second view's icons from the stock minimap), for a before / after picture */
static uint32_t mm_cell_begin(unsigned self)
{
    if (!mm_swap_on)
        return 0;
    unsigned ck = rd32(AD.cockpit_ptr);
    uint32_t back = ck ? mp1_swap_begin(rd32(ck + 0x2C), hud.stock_mm, self == hud.mm ? self : 0) : 0;
    if (back)
        wr32(ck + 0x2C, self), n_mm_swaps++;
    return back;
}
static void mm_cell_end(unsigned self, uint32_t back)
{
    unsigned ck = rd32(AD.cockpit_ptr);
    if (ck && mp1_swap_restore_ok(back, rd32(ck + 0x2C), self))
        wr32(ck + 0x2C, back);
}
static void FC wrap_mm8(unsigned self)
{
    own_scope_begin();
    uint32_t back = mm_cell_begin(self);
    orig_mm8(self);
    mm_cell_end(self, back);
    own_scope_end();
}
static void FC wrap_mm9(unsigned self)
{
    own_scope_begin();
    uint32_t back = mm_cell_begin(self);
    orig_mm9(self);
    mm_cell_end(self, back);
    own_scope_end();
}

/* (minimap1, 2026-11-09) the minimap in a half (mapptn1.h, mp1_screen_site): the 8 screen-size reads of uGUI_Map / uMapIcon take the cell mm_screen_cell
 * (written into their instructions in place of the address 0x11E1B9C, once, when a split first draws): the real screen object, or while the split draws
 * the mod's object whose width / height are the half's. LAB `split mmscreen 0|1` (0 = the old behaviour: the whole screen's size, for a before picture). */
static uint32_t mm_screen_cell, mm_screen_obj[0x100 / 4];
static int mm_screen_patched, mm_screen_on = 1, mm_screen_half_last = -1;
static unsigned n_mm_screen_half, mm_screen_prev_count;
static int code_write32(unsigned a, uint32_t v)
{
    DWORD old;
    if (!VirtualProtect((void *)(size_t)a, 4, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    *(volatile uint32_t *)(size_t)a = v;
    VirtualProtect((void *)(size_t)a, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(size_t)a, 4);
    return 1;
}
static void mm_screen(int half, int vertical)
{
    unsigned real = rd32(MP1_SCREEN_PTR);
    if (!real)
        return;
    if (!mm_screen_patched) {
        if (!half)
            return;   /* (written only once a split draws: a single-player game keeps the stock bytes) */
        uint8_t b[8];
        int off[MP1_N_SCREEN_SITES];
        for (int i = 0; i < MP1_N_SCREEN_SITES; i++) {
            if (!r1_safe_read((void *)(size_t)mp1_screen_site[i], b, 6) || (off[i] = mp1_screen_imm_off(b)) < 0 ||
                rd32(mp1_screen_site[i] + (unsigned)off[i]) != MP1_SCREEN_PTR) {
                r1_log("split: minimap screen size: site %08x is not the expected mov reg,[0x11E1B9C]: not written (the split minimaps keep the whole screen's size)",
                       mp1_screen_site[i]);
                mm_screen_patched = -1;
                return;
            }
        }
        mm_screen_cell = real;
        int ok = 0;
        for (int i = 0; i < MP1_N_SCREEN_SITES; i++)
            ok += code_write32(mp1_screen_site[i] + (unsigned)off[i], (uint32_t)(size_t)&mm_screen_cell);
        mm_screen_patched = ok == MP1_N_SCREEN_SITES ? 1 : -1;
        r1_log("split: minimap screen size: %d of %d reads of 0x11E1B9C now read the cell %p (the half's size while the split draws)", ok, MP1_N_SCREEN_SITES,
               (void *)&mm_screen_cell);
    }
    if (mm_screen_patched != 1)
        return;
    if (!half) {   /* (every pass starts with the real object; the split's gameplay path then switches to the half's) */
        mm_screen_cell = real;
        if (mm_screen_half_last == 1 && n_mm_screen_half == mm_screen_prev_count) {
            r1_log("split: minimap screen size: the whole screen's again");
            mm_screen_half_last = 0;
        }
        mm_screen_prev_count = n_mm_screen_half;
        return;
    }
    int on = mm_screen_on;
    if (on) {
        int w = (int)rd32(real + MP1_SCREEN_W), h = (int)rd32(real + MP1_SCREEN_H), hw, hh;
        mp1_half_size(vertical, w, h, &hw, &hh);
        mm_screen_obj[MP1_SCREEN_W / 4] = (uint32_t)hw;
        mm_screen_obj[MP1_SCREEN_H / 4] = (uint32_t)hh;
        mm_screen_cell = (uint32_t)(size_t)mm_screen_obj;
        n_mm_screen_half++;
        if (mm_screen_half_last != 1)
            r1_log("split: minimap screen size: the half's %d x %d (screen %d x %d)", hw, hh, w, h);
        mm_screen_half_last = 1;
    }
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
static DWORD hud_hold_until;   /* fix026: no (re)build until then (after a start record: the autosave notice of a New Game / Continue runs ~1 s after it; tester report: crash right at "Saving game") */
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

/* fix026 (issue #2, owner run 2026-11-22 06:23:28): a SECTION CHANGE (FUN_00720540 from the episode end state, no pause, no cutscene before it) found the second set alive and
 * updating: AccessViolation at 0x401396 (assert cGameSystem.h:62) 15 ms later, esi = the second cockpit manager. The start hook retires the set before the area goes. */
void split_retire_hud(const char *why)
{
    if ((hud.cp || hud.mm) && in_group(COCKPIT_GROUP, hud.cp) && in_group(COCKPIT_GROUP, hud.mm))
        hud_destroy(why);
    else if (hud.cp || hud.mm)
        hud_forget();
    hud_unstable();
    hud_hold_until = GetTickCount() + 6000;
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
    if (!hud.failed && now - stable_since >= hud_settle_ms && (!hud_hold_until || (int)(now - hud_hold_until) >= 0)) {
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
/* scan1 (notes/scan1.md 2.2): the second human's Genesis runs in the same exchange as his menus, kind 3; scan1.c decides when it starts (SPLIT_XG_WANT_ENTER) and
 * ends (SPLIT_XG_WANT_LEAVE) and is told about both (SPLIT_XG_BEGIN / SPLIT_XG_END). No hook = no kind 3. */
static int (*xg_hook)(int op);
void split_set_genesis_hook(int (*hook)(int op)) { xg_hook = hook; }

/* rel027z (zq_2 live: after an exchange manager 0 still had the OTHER human as its target for the pass of the flip): the two managers' targets follow the own slot at once, manager 0 = the own human, manager 1 =
 * the other one (the engine does it later in its own phases; split_frame writes manager 1's every pass anyway). */
static void retarget_managers(void)
{
    unsigned r = rd32(AD.camroot_ptr), m0 = r ? rd32(r + 0xCE0) : 0, m1 = r ? rd32(r + 0xCE4) : 0;
    int own = capture_own_effective();
    unsigned uo = 0, ut = 0;
    {
        unsigned sp = rd32(AD.splayer_ptr), head = sp ? rd32(sp + 0x30) : 0, endp = sp ? rd32(sp + 0x34) : 0;
        for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8)) {
            unsigned unit = rd32(head + 0x10);
            if (!unit || !is_player_class(unit))
                continue;
            if (rd32(unit + 0xE2C) == (unsigned)own)
                uo = unit;
            else if (rd32(unit + 0xE2C) == (unsigned)(1 - own))
                ut = unit;
        }
    }
    if (m0 && uo && rd32(m0 + 0x74) != uo)
        wr32(m0 + 0x74, uo), wr32(m0 + 0x78, uo);
    if (m1 && ut && rd32(m1 + 0x74) != ut)
        wr32(m1 + 0x74, ut), wr32(m1 + 0x78, ut);
}
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
    capture_set_kbx(testmode_pad_kbpad_get());   /* rel027z: keyboard + mouse + one gamepad: the keyboard follows the first human to pad slot 1 */
    retarget_managers();
    mx.on = 1, mx.kind = kind, mx.t_enter = GetTickCount(), mx.seen_gui = 0, mx.quiet = 0;
    n_mx++;
    r1_log("split: second human opens his %s: pads exchanged, own slot %d, roles flipped", kind == 1 ? "item menu (BACK)" : kind == 2 ? "pause menu (START)" : kind == 5 ? "story examine (his half)" : kind == 4 ? "document (his half)" : "Genesis scanner (his half)",
           other_unit);
    if (kind == 3 && xg_hook)
        xg_hook(SPLIT_XG_BEGIN);
}

static DWORD xg_end_t;
static void menu_leave(const char *why)
{
    int kind = mx.kind;
    capture_set_own(mx.old_own_on ? mx.old_own : -1);
    capture_bind(0, mx.old_pad[0]);
    capture_bind(1, mx.old_pad[1]);
    testmode_pad_swap(0);
    capture_set_kbx(0);
    mx.on = 0;
    retarget_managers();
    xg_end_t = GetTickCount() | 1;   /* rel027z: the camera guard below watches the two managers for a few seconds */
    r1_log("split: second human's menu over (%s): pads, own slot and roles back to the split layout", why);
    if (kind == 3 && xg_hook)
        xg_hook(SPLIT_XG_END);
}

/* rel027h (owner 2026-11-25): the item menu and the Genesis scanner of EITHER human open in HIS half instead of over the whole screen. The own human (the one the menus and the
 * scanner belong to) is always viewport 0; the second human's BACK / Genesis exchange makes him the own human, and for the length of it the two layout bytes are swapped (viewport 0
 * = his half, viewport 2 = the first human's half), so the split stays up and nothing but the halves' camera managers changes sides. The pause menu / game over stay whole view.
 * The item menu manager (cockpit +0x3C) is drawn only in viewport 0 while its menu is open (view mask in unit +0xC bits 16..25, like the scan filter units of scan1.c).
 * Switch: console `split halfmenu 0|1`, env REV1_SPLIT_HALFMENU=0 = the old whole view. */
static volatile int half_menus = 1;
static int half_menu_exchange(void) { return half_menus && mx.on && (mx.kind == 3 || mx.kind == 4 || mx.kind == 5); }   /* the Genesis / a document / the story examine of the second human: the layout bytes are swapped (his camera is the own manager 0 meanwhile) */
static int half_menu_open(void) { return half_menus && mx.on && mx.kind != 2; }      /* BACK / Genesis: the split stays up (the pause menu is whole view) */
#define IM_MAX 12
static struct { unsigned unit, old; int group; } im_e[IM_MAX];
static int im_n;
static unsigned n_item_masks;
/* live (h27f, `split units` with the item menu open vs closed): the menu appears as new units of the cockpit group 0x16 - uGUI_PauseBlurHD, uSubMenuManager (view mask 1 already), uGUI_SubMenuBase,
 * uGUI_SubMenu (view mask 0xF: drawn in every viewport) - and uWorldMapModel in group 0x18 (the map). Their view masks are cut to viewport 0 (the own human's) while the menu is open. */
static int im_wanted(const char *cn)
{
    return !strncmp(cn, "uGUI_PauseBlur", 14) || !strncmp(cn, "uSubMenu", 8) || !strncmp(cn, "uGUI_SubMenu", 12) || !strncmp(cn, "uWorldMapModel", 14);
}
static void item_menu_mask(int on, unsigned mask)   /* mask: 1 = viewport 0 (the first human's half), 4 = viewport 2 (the second human's half) */
{
    unsigned su = rd32(AD.unitmgr_ptr);
    if (on && su) {
        static const int groups[2] = {COCKPIT_GROUP, 0x18};
        for (int gi = 0; gi < 2; gi++) {
            unsigned u = rd32(su + 0x2C + 0x18 * (unsigned)groups[gi]);
            for (int guard = 0; u && guard < 200; guard++, u = rd32(u + 0x14)) {
                char cn[48];
                int known = -1;
                for (int k = 0; k < im_n; k++)
                    if (im_e[k].unit == u)
                        known = k;
                unsigned f = rd32(u + 0xC);
                if (known >= 0) {
                    if (((f >> 16) & 0x3FFu) != mask)   /* the opener changed (the second human's exchange began after the menu units were seen) */
                        wr32(u + 0xC, (f & ~0x3FF0000u) | (mask << 16));
                    continue;
                }
                if (im_n >= IM_MAX || !classes_object_name(u, cn, sizeof cn) || !im_wanted(cn))
                    continue;
                if (((f >> 16) & 0x3FFu) == mask)
                    continue;   /* already only there */
                im_e[im_n].unit = u, im_e[im_n].old = f & 0x3FF0000u, im_e[im_n].group = groups[gi], im_n++;
                wr32(u + 0xC, (f & ~0x3FF0000u) | (mask << 16));
                n_item_masks++;
                r1_log("split: item menu unit %08x %s drawn in viewport %d only (view mask %03x -> %03x)", u, cn, mask == 4 ? 2 : 0, (f >> 16) & 0x3FFu, mask);
            }
        }
    } else if (!on && im_n) {
        for (int k = 0; k < im_n; k++)
            if (in_group((unsigned)im_e[k].group, im_e[k].unit))
                wr32(im_e[k].unit + 0xC, (rd32(im_e[k].unit + 0xC) & ~0x3FF0000u) | im_e[k].old);
        im_n = 0;
    }
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

static int exam_wanted_on(void);   /* rel027z (below) */
static int popup_wanted_on(void);
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
            else if (xg_hook && xg_hook(SPLIT_XG_WANT_ENTER))
                menu_enter(3);   /* scan1: his Genesis */
        }
        return;
    }
    if (mx.kind == 3) {   /* scan1: not a menu: it ends when scan1 says his Genesis use is over (a pause / cutscene meanwhile is whole view anyway) */
        if (!xg_hook || xg_hook(SPLIT_XG_WANT_LEAVE))
            menu_leave("the second human's Genesis use is over");
        return;
    }
    if (mx.kind == 5) {   /* rel027z: the story examine of the second human: exam_frame below ends it (it clears ex.on) */
        if (!exam_wanted_on())
            menu_leave("the examine is over");
        return;
    }
    if (mx.kind == 4) {   /* rel027z: a document / manual the second human reads: ends when the reader has closed (popup_wanted_on below) */
        if (!popup_wanted_on())
            menu_leave("the document is closed");
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

/* ====================================================================== camera guard after an exchange (rel027z, owner items 5 / 6) ====================================================================== */
/* Owner: after the second human used the Genesis a second time quickly, the first human's camera sat at his chest and the second human's half had no scope. The scanner view is a camera of the OWN manager
 * (cScopeCamera / cFPSCamera, cameras [2] / [1] of the list at +0xE4); the scan state of the unit ends with a SetCamera(TPS) on the own manager of THAT moment. When the exchange flips the own slot back
 * before the unit has left its scan state (a second press within the stock exit), the leave can land on the wrong manager and leave the scanner camera on the first human's. For 4 s after an exchange a manager
 * that stands on the scope / first-person camera while its target is neither scanning (unit +0xF44 bit 0x20000) nor aiming (+0x1151) for 3 passes in a row gets SetCamera(its TPS index) (0x495EB0). Logged. */
static unsigned n_xg_guard;
static int xg_guard_n[2];
static void xg_guard(unsigned mgr0, unsigned mgr1)
{
    if (!xg_end_t || GetTickCount() - xg_end_t > 4000u || mx.on)
        return;
    unsigned ms[2] = {mgr0, mgr1};
    for (int k = 0; k < 2; k++) {
        unsigned m = ms[k], cur = rd32(m + 0xCC), cnt = rd32(m + 0xD8), arr = rd32(m + 0xE4), cb = cur < cnt && cnt < 16 && arr ? rd32(arr + 4 * cur) : 0, kind = cb ? rd32(cb + 4) & 0xFFu : 99u;
        unsigned unit = rd32(m + 0x74);
        int legit = unit && ((rd32(unit + 0xF44) & 0x20000u) || rd8(unit + 0x1151));
        if ((kind == 1 || kind == 2) && !legit) {
            if (++xg_guard_n[k] >= 3) {
                typedef void (TC * setcam_fn)(void *, unsigned);
                unsigned tps = rd32(m + 0xE8);
                if (tps < cnt) {
                    ((setcam_fn)(size_t)0x495EB0u)((void *)(size_t)m, tps);
                    n_xg_guard++;
                    r1_log("split: camera guard: manager %d (target %08x) stood on camera kind %u after the exchange, nobody scanning / aiming: back on the third-person camera (#%u)", k, unit, kind, n_xg_guard);
                }
                xg_guard_n[k] = 0;
            }
        } else
            xg_guard_n[k] = 0;
    }
}

/* LAB / diagnostics (rel027z items 5 / 6): while an exchange runs and for 4 s after it, every change of a manager's camera kind or of "eye closer than 70 units to its target" is logged (host log
 * "xgwatch:", at most 600 lines) so a broken camera after a quick second Genesis use is caught with its time and state even when no picture is taken at that moment. */
static void xg_watch(unsigned mgr0, unsigned mgr1)
{
    static int last[2] = {-1, -1}, nl;
    static DWORD t_prev;
    DWORD now = GetTickCount();
    if (!mx.on && (!xg_end_t || now - xg_end_t > 4000u)) {
        last[0] = last[1] = -1;
        return;
    }
    unsigned ms[2] = {mgr0, mgr1};
    for (int k = 0; k < 2; k++) {
        unsigned m = ms[k], cur = rd32(m + 0xCC), cnt = rd32(m + 0xD8), arr = rd32(m + 0xE4), cb = cur < cnt && cnt < 16 && arr ? rd32(arr + 4 * cur) : 0, kind = cb ? rd32(cb + 4) & 0xFFu : 99u;
        unsigned unit = rd32(m + 0x74);
        float ex = rdf(m + 0x90) - rdf(unit + 0x40), ey = rdf(m + 0x94) - rdf(unit + 0x44), ez = rdf(m + 0x98) - rdf(unit + 0x48), d = sqrtf(ex * ex + ey * ey + ez * ez);
        int state = (int)kind * 4 + (d < 70.0f ? 1 : 0) + (mx.on ? 2 : 0) + (int)((unit >> 4) & 0xFFFu) * 256;   /* (a change of the target is a state too) */
        if (state != last[k] && nl < 600) {
            nl++;
            r1_log("xgwatch: t%u manager %d camera kind %u eye %.0f u from its target %08x (own slot %d, exchange %s kind %d, unit flags F44 %08x aim %u)", (unsigned)(now % 100000u), k, kind, (double)d, unit, capture_own_effective(),
                   mx.on ? "ON" : "off", mx.on ? mx.kind : 0, unit ? rd32(unit + 0xF44) : 0, unit ? rd8(unit + 0x1151) : 0);
        }
        last[k] = state;
    }
    t_prev = now;
}

/* ====================================================================== the story examine, per half (rel027z, owner items 2 / 3) ====================================================================== */
/* Story examine (the FBC crates of 1-2, demo 12_106_0 and every PcsMainMessageControlSet with a PcsSetCamera): the story puts the camera of VIEWPORT 0's manager on a fixed pose (cPcsCamera, kind 4),
 * draws the message in the shared message boxes (uGUI_MessageBoxSub: view mask 0xF = every viewport), locks the main character and the partner (FsmLockPlayer) and HIDES the partner (PcsMainCharacterSetDraw:
 * unit +0xC bit 0x800 cleared, 0x76DCB0). In the split screen both halves showed the message, and the second human was invisible and locked in his own half.
 * Now: the half of the human who examines (viewport 0: his half, the layout bytes are swapped for the length of a second human's examine like his Genesis, exchange kind 5) shows the camera and the message,
 * the other half keeps its own view; the lock of the other human is lifted by capture.c's unlock pass (he is not the own human); a human the story hid is drawn again. Who examines: the second human when
 * his A was pressed up to 2.5 s before the camera went on, else the first one. Switch: console `split exam 0|1`, env REV1_SPLIT_EXAM=0 = stock (both halves). */
static volatile int exam_sw = 1;
static struct { int on, by, seen, quiet; DWORD t0; unsigned drawn[2]; } ex;
static unsigned n_exam, n_exam_p2;
static DWORD ex_p2_a_t;
static unsigned ex_prev_btn;
#define EM_MAX 8
static struct { unsigned unit, old; } em_e[EM_MAX];
static int em_n;
static int exam_wanted_on(void) { return ex.on; }
static int exam_msg_wanted(const char *cn) { return !strncmp(cn, "uGUI_MessageBox", 15) || !strncmp(cn, "uGUI_Telop2", 11); }   /* live (zi 22:28): the story's examine text is a uGUI_Telop2 (group 0x1C, view mask 0xF) as well */
static void exam_msg_mask(int on, unsigned mask)   /* message boxes: drawn in the examiner's viewport only (mask bits are viewports: 1 = viewport 0) */
{
    unsigned su = rd32(AD.unitmgr_ptr);
    if (on && su) {
        static const int groups[3] = {0x1C, COCKPIT_GROUP, 0x18};
        for (int gi = 0; gi < 3; gi++) {
            unsigned u = rd32(su + 0x2C + 0x18 * (unsigned)groups[gi]);
            for (int guard = 0; u && guard < 200; guard++, u = rd32(u + 0x14)) {
                char cn[48];
                int known = -1;
                for (int k = 0; k < em_n; k++)
                    if (em_e[k].unit == u)
                        known = k;
                unsigned f = rd32(u + 0xC);
                if (known >= 0) {
                    if (((f >> 16) & 0x3FFu) != mask)
                        wr32(u + 0xC, (f & ~0x3FF0000u) | (mask << 16));
                    continue;
                }
                if (em_n >= EM_MAX || !classes_object_name(u, cn, sizeof cn) || !exam_msg_wanted(cn))
                    continue;
                em_e[em_n].unit = u, em_e[em_n].old = f & 0x3FF0000u, em_n++;
                wr32(u + 0xC, (f & ~0x3FF0000u) | (mask << 16));
                r1_log("split: examine: message box %08x %s drawn in viewport %d only (view mask %03x -> %03x)", u, cn, mask == 4 ? 2 : 0, (f >> 16) & 0x3FFu, mask);
            }
        }
    } else if (!on && em_n) {
        for (int k = 0; k < em_n; k++)
            if (in_group(0x1C, em_e[k].unit) || in_group(COCKPIT_GROUP, em_e[k].unit) || in_group(0x18, em_e[k].unit))
                wr32(em_e[k].unit + 0xC, (rd32(em_e[k].unit + 0xC) & ~0x3FF0000u) | em_e[k].old);
        em_n = 0;
    }
}
static void exam_end(const char *why)
{
    if (!ex.on)
        return;
    exam_msg_mask(0, 0);
    ex.on = 0;
    r1_log("split: examine over (%s): message boxes back in every viewport%s", why, mx.on && mx.kind == 5 ? ", exchange ends" : "");
}
static void p2a_track(void)   /* the second human's A presses (his pad as the lobby numbers it, like BACK in menu_frame), every pass, also while the view is whole */
{
    if (mx.on)
        return;
    unsigned char gp[12];
    memset(gp, 0, sizeof gp);
    unsigned b = testmode_pad_peek(1, gp) ? *(unsigned short *)gp : 0;
    if (b & ~ex_prev_btn & 0x1000u)
        ex_p2_a_t = GetTickCount() | 1;
    ex_prev_btn = b;
}
/* rel027z (owner item 4, chapter 2 start): the second human jumps first: the story's jump puts the EVENT camera (cMotionCamera, kind 3) on manager 0 = the first human's, who is still on the platform; with the
 * armed demos of the section the split goes whole view for 30+ s and shows the first human from above (owner img 4). When the second human's A was pressed up to 4 s before manager 0 went to an event camera
 * and the two humans are more than 200 units apart, the event camera is not for the first human: manager 0 is put back on its third-person camera (SetCamera 0x495EB0) for 40 s (every pass, the story keeps
 * it set), the split never goes whole view. `split evcam 0|1` (default 1). */
static volatile int evcam_sw = 1;
static DWORD ev_until;
static unsigned n_ev;
static int evcam_guard(unsigned mgr0, unsigned own_unit, unsigned other_unit)
{
    if (!evcam_sw || mx.on || !own_unit || !other_unit)
        return 0;
    DWORD now = GetTickCount();
    unsigned cur = rd32(mgr0 + 0xCC), cnt = rd32(mgr0 + 0xD8), arr = rd32(mgr0 + 0xE4), cb = cur < cnt && cnt < 16 && arr ? rd32(arr + 4 * cur) : 0;
    if (!cb || (rd32(cb + 4) & 0xFFu) != 3u)
        return 0;
    float dx = rdf(own_unit + 0x40) - rdf(other_unit + 0x40), dy = rdf(own_unit + 0x44) - rdf(other_unit + 0x44), dz = rdf(own_unit + 0x48) - rdf(other_unit + 0x48);
    int active_win = ev_until && (int)(ev_until - now) > 0;
    if (!active_win) {
        int p2 = ex_p2_a_t && now - ex_p2_a_t < 4000u;
        if (!p2 || dx * dx + dy * dy + dz * dz < 200.0f * 200.0f)
            return 0;
        ev_until = (now + 40000u) | 1u;
        n_ev++;
        r1_log("split: event camera: manager 0 went to the event camera right after the second human's action, the humans are %.0f units apart: the first human keeps his third-person camera for 40 s (#%u)", (double)sqrtf(dx * dx + dy * dy + dz * dz), n_ev);
    }
    typedef void (TC * setcam2_fn)(void *, unsigned);
    unsigned tps = rd32(mgr0 + 0xE8);
    if (tps < cnt)
        ((setcam2_fn)(size_t)0x495EB0u)((void *)(size_t)mgr0, tps);
    return 1;
}
static void exam_frame(unsigned mgr0, unsigned own_unit, unsigned other_unit)
{
    DWORD now = GetTickCount();
    if (!exam_sw)
        return;
    unsigned cur = rd32(mgr0 + 0xCC), cnt = rd32(mgr0 + 0xD8), arr = rd32(mgr0 + 0xE4), cb = cur < cnt && cnt < 16 && arr ? rd32(arr + 4 * cur) : 0;
    char cn[40];
    int pcs = cb && classes_object_name(cb, cn, sizeof cn) && !strcmp(cn, "cPcsCamera");
    if (!ex.on) {
        ex.seen = pcs ? ex.seen + 1 : 0;
        if (ex.seen < 2)
            return;
        memset(&ex, 0, sizeof ex);
        ex.on = 1, ex.t0 = now | 1;
        unsigned us[2] = {own_unit, other_unit};
        for (int k = 0; k < 2; k++)
            ex.drawn[k] = us[k] && (rd32(us[k] + 0xC) & 0x800u) ? 1u : 0u;
        n_exam++;
        int p2 = ex_p2_a_t && now - ex_p2_a_t < 2500u;
        if (p2 && !mx.on && !suspended && capture_hooks_ready()) {
            menu_enter(5);
            if (mx.on && mx.kind == 5) {
                ex.by = 1;
                n_exam_p2++;
            }
        }
        r1_log("split: examine starts (camera of manager 0 is cPcsCamera): by the %s human, in his half%s", ex.by ? "second" : "first", ex.by ? " (exchange: his half is viewport 0)" : "");
    }
    /* running: the message boxes only in viewport 0 (the examiner's half), a human the story hid is drawn again */
    exam_msg_mask(1, 1u);
    unsigned us[2] = {own_unit, other_unit};
    for (int k = 0; k < 2; k++)
        if (us[k] && ex.drawn[k] && !(rd32(us[k] + 0xC) & 0x800u))
            wr32(us[k] + 0xC, rd32(us[k] + 0xC) | 0x800u);
    ex.quiet = pcs ? 0 : ex.quiet + 1;
    if (ex.quiet >= 10)
        exam_end("the story's camera is gone");
    else if (now - ex.t0 > 120000u)
        exam_end("guard: 120 s");
}

/* ====================================================================== documents / manuals per half (rel027z, owner item 1) ====================================================================== */
/* The Genesis manual (uGUI_Guide, 1-2 arrival: "1 / 6", closed with B) and the file reader (uGUI_InGameFile, FsmDrawFile / file items) belong to the OWN human: they are drawn in viewport 0 and take pad slot 0
 * (live za3: P1's pad turns the pages and closes it, P2's pad does nothing, it is only in the first half). When the SECOND human opened one (his action button was pressed up to 2.5 s before it came up) it
 * ran in the first human's half and nobody but the first human could close it. Now he reads it in HIS half like his Genesis (exchange kind 4: pads, own slot, roles, layout bytes swapped) until it is closed.
 * Open = uGUI_Guide byte +0x270 (set from its sub objects, vf8 0x7C79D0) or the uGUI_InGameFile's shown bit (unit flags 0x4000 set by its open, cleared at init 0x7B1FB0). `split doc [p2]` (LAB) forces
 * the exchange for the document that is open now; `split popup` dumps both. */
static volatile int doc_sw = 1;
static unsigned doc_guide, doc_file, doc_prev, n_doc, n_doc_p2;
static int doc_find(void)
{
    unsigned su = rd32(AD.unitmgr_ptr);
    if (!su)
        return 0;
    if (doc_guide && in_group(COCKPIT_GROUP, doc_guide) && (!doc_file || in_group(COCKPIT_GROUP, doc_file)))
        return 1;
    doc_guide = doc_file = 0;
    unsigned u = rd32(su + 0x2C + 0x18 * (unsigned)COCKPIT_GROUP);
    for (int guard = 0; u && guard < 200; guard++, u = rd32(u + 0x14)) {
        char cn[48];
        if (!classes_object_name(u, cn, sizeof cn))
            continue;
        if (!strcmp(cn, "uGUI_PauseBlurHD"))
            doc_guide = u;   /* (the name is kept: the unit that shows a document is its blurred backdrop, see doc_open) */
    }
    return doc_guide != 0;
}
/* LIVE (za3 21:25): uGUI_Guide / uGUI_InGameFile never change while the manual opens and closes (flags, +0x270, +0x281 all equal); the one unit that does is the backdrop uGUI_PauseBlurHD (group 0x16):
 * unit flags 0xC00 (update + draw) set while a document is up, clear in play (zex1 19:13 f000f60b2 vs the manual f000f6cb2). The pause menu / game over / item menu set it too: they are told apart by gui_running(). */
static int doc_open(void)
{
    if (!doc_find())
        return 0;
    return (rd32(doc_guide + 0xC) & 0xC00u) == 0xC00u && !gui_running();
}
static int popup_wanted_on(void) { return mx.on && mx.kind == 4 && (doc_open() || GetTickCount() - mx.t_enter < 2500); }
static void doc_frame(void)
{
    if (!doc_sw)
        return;
    int open = doc_open();
    if (open && !doc_prev) {
        n_doc++;
        DWORD now = GetTickCount();
        int p2 = ex_p2_a_t && now - ex_p2_a_t < 2500u;
        if (p2 && !mx.on && !suspended && capture_hooks_ready()) {
            menu_enter(4);
            if (mx.on)
                n_doc_p2++;
        }
        r1_log("split: a document / manual opened (uGUI_Guide %08x), read by the %s human", doc_guide, mx.on && mx.kind == 4 ? "second" : "first");
    }
    doc_prev = open;
}

/* ====================================================================== the second human's aim ray (fix023, aimray1.h) ====================================================================== */
/* Every human's shot ray was made from camera manager 0 (the first human's view): the second human's shots went where the first human looked. The three loads of the
 * sCamera pointer in the aim functions call a stub that hands the second human's weapons a stand-in camera root (manager 1, viewport 2's size) while the split's
 * gameplay view is up. The stub bytes are written once, on the game thread between two frames (no weapon code runs then), at the first gameplay pass of a split:
 * a game without a split keeps the stock bytes. */
static int am_r(void *u, uint32_t a, void *out, uint32_t n) { (void)u; return r1_safe_read((void *)(size_t)a, out, n); }
static int am_w(void *u, uint32_t a, const void *p, uint32_t n)
{
    (void)u;
    DWORD old;
    if (!VirtualProtect((void *)(size_t)a, n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy((void *)(size_t)a, p, n);
    VirtualProtect((void *)(size_t)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(size_t)a, n);
    return 1;
}
static uint32_t am_alloc(void *u, uint32_t n)
{
    (void)u;
    static uint8_t *blk;
    static uint32_t used;
    if (!blk && !(blk = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        return 0;
    if (used + n > 4096)
        return 0;
    uint32_t a = (uint32_t)(size_t)(blk + used);
    used += (n + 15u) & ~15u;
    return a;
}
static void am_log(void *u, const char *line) { (void)u; r1_log("%s", line); }
static const ar1_mem AM = {am_r, am_w, am_alloc, am_log, NULL};
static int aim_tried;   /* the stub bytes were attempted once (a refusal is logged with its reason and not retried every pass) */

static void aim_view(int on, unsigned r, unsigned mgr1)
{
    if (!on) {
        ar1_set_view(&ar1_g, 0, 0, 0, 0, 0);
        return;
    }
    unsigned v2 = vp_addr(r, VP_SECOND);
    int w = (int)rd32(v2 + 0x20) - (int)rd32(v2 + 0x18), h = (int)rd32(v2 + 0x24) - (int)rd32(v2 + 0x1C);
    ar1_set_view(&ar1_g, 1, mgr1, capture_own_effective(), w, h);
    if (!aim_tried && !harness && ar1_g.enabled) {
        aim_tried = 1;
        ar1_apply(&ar1_g, &AM, (uint32_t)(size_t)ar1_choose);
    }
}

/* ====================================================================== the second human's rifle scope (rel027s, Discord B6) ====================================================================== */
/* Jessica's sniper rifle (snow chapter 2-1) showed the second human's half pressed against his own shoulder: no overlay, no zoom (lab sc1 / scp1 / scp2, notes/rel027s.md). The live probe
 * shows the camera part WORKS: manager 1 switches to its own cScopeCamera (kind 2, rScopeCamera shared with manager 0, weapon entry 4) and outputs the scope pose (fov 30). What the engine
 * does only for the OWN human (FUN_006AEF60: his unit's manager == manager 0) and what is missing for the other one:
 *   1. the model is hidden in viewport 0 (unit+0xC bit 16, the per-view mask: (unit+0xC >> 16) & (1 << viewport) & 0x3FF, sUnit::draw) so the camera that sits in his head does not see him;
 *      the second human stays visible in viewport 2 = the camera inside his shoulder (the picture). Same for the weapon object when FUN_006A90D0(unit).
 *   2. the scope overlay (the cockpit's uGUI_Scope child, [cockpit+0x54], shown by FUN_007AE720(gui, weapon, 1), hidden by (gui, 0, 1)) only for the stock cockpit.
 *   3. the scope camera's zoom state machine (FUN_00488CB0, cScopeCamera vf5..vf10) skips when manager+0x88 (the pad slot) is -1 (manager 1) and reads / writes the engine's per-slot pad state
 *      (sGamePad PadData aim / zoom words, the zoom button) of the CURRENT slot (sGamePad+0x870 = 0, the first human's).
 * The fix: scope camera slots 5 / 6 / 7 / 8 / 10 of manager 1's camera object run with sGamePad+0x870 = the second human's pad slot and manager+0x88 = that slot (put back after); the model
 * (bit 18 = viewport 2) and the overlay of the SECOND cockpit set follow the camera (manager 1's current camera is its scope camera) once per pass. Lab: console `split scope2 [mask]`
 * (1 camera wrappers, 2 model, 4 overlay; default 7), env REV1_SPLIT_SCOPE2=<mask>. */
#define SC2_VT_SLOTS 40u
#define SC2_STOCK_VT 0xFF6218u   /* vtbl_cScopeCamera */
static volatile int scope2_mask = 7;
static unsigned sc2_vt[SC2_VT_SLOTS], sc2_orig[SC2_VT_SLOTS];
static unsigned sc2_obj, sc2_mgr1;      /* the patched scope camera object, its manager */
static struct {
    int on;                               /* the visual part (model + overlay) is applied */
    unsigned unit, weapon, gui, cp;
    unsigned unit_had, weapon_had;        /* the viewport-2 bit was set before */
} sc2;
static unsigned n_sc2_calls, n_sc2_on, n_sc2_off, n_sc2_patch;

static int sc2_pad(void)   /* the pad slot of the human manager 1 follows (-1: unknown) */
{
    capture_state_t st;
    capture_get_state(&st);
    unsigned u = sc2_mgr1 ? rd32(sc2_mgr1 + 0x74) : 0, slot = u ? rd32(u + 0xE2C) : 9;
    return slot < 2 && st.role[slot].enabled ? st.role[slot].pad : -1;
}

static unsigned sc2_call(unsigned k, void *self, float dt, int has_dt)
{
    unsigned m = self ? rd32((unsigned)(size_t)self + 0x50) : 0, f = sc2_orig[k];
    int pad = (scope2_mask & 1) && m && m == sc2_mgr1 ? sc2_pad() : -1;
    unsigned r;
    if (pad < 0) {
        r = has_dt ? ((unsigned(TC *)(void *, float))(size_t)f)(self, dt) : (((void(TC *)(void *))(size_t)f)(self), 0u);
        return r;
    }
    unsigned old, mf = rd32(m + 0x88);
    int tok = capture_slot_begin(pad, &old);
    wr32(m + 0x88, (unsigned)pad);
    r = has_dt ? ((unsigned(TC *)(void *, float))(size_t)f)(self, dt) : (((void(TC *)(void *))(size_t)f)(self), 0u);
    wr32(m + 0x88, mf);
    capture_slot_end(tok, old);
    n_sc2_calls++;
    return r;
}
static void TC sc2_w5(void *self) { sc2_call(5, self, 0, 0); }
static void TC sc2_w6(void *self) { sc2_call(6, self, 0, 0); }
static void TC sc2_w7(void *self) { sc2_call(7, self, 0, 0); }
static void TC sc2_w8(void *self) { sc2_call(8, self, 0, 0); }
static unsigned TC sc2_w10(void *self, float dt) { return sc2_call(10, self, dt, 1); }

/* manager 1's scope camera object (list [+0xE4], index [+0xF0]); 0 when there is none */
static unsigned sc2_camera(unsigned mgr1)
{
    unsigned arr = rd32(mgr1 + 0xE4), si = rd32(mgr1 + 0xF0), cnt = rd32(mgr1 + 0xD8);
    return arr && si < cnt ? rd32(arr + 4 * si) : 0;
}

static void sc2_unpatch(void)
{
    if (sc2_obj && sc2_mgr1 && is_cam_mgr(sc2_mgr1) && rd32(sc2_obj) == (unsigned)(size_t)sc2_vt) {
        unsigned cam = sc2_camera(sc2_mgr1);
        if (cam == sc2_obj)
            wr32(sc2_obj, SC2_STOCK_VT);
    }
    sc2_obj = 0;
}

static void sc2_patch(unsigned mgr1)
{
    unsigned cam = sc2_camera(mgr1);
    if (!cam) {
        sc2_obj = 0;
        return;
    }
    if (sc2_obj == cam && rd32(cam) == (unsigned)(size_t)sc2_vt)
        return;
    char cn[64];
    if (rd32(cam) != SC2_STOCK_VT || !classes_object_name(cam, cn, sizeof cn) || strcmp(cn, "cScopeCamera"))
        return;   /* not a stock scope camera (another vtable, a freed object): left alone */
    for (unsigned k = 0; k < SC2_VT_SLOTS; k++)
        sc2_orig[k] = sc2_vt[k] = rd32(SC2_STOCK_VT + 4 * k);
    sc2_vt[5] = (unsigned)(size_t)sc2_w5, sc2_vt[6] = (unsigned)(size_t)sc2_w6, sc2_vt[7] = (unsigned)(size_t)sc2_w7, sc2_vt[8] = (unsigned)(size_t)sc2_w8, sc2_vt[10] = (unsigned)(size_t)sc2_w10;
    sc2_mgr1 = mgr1;
    sc2_obj = cam;
    wr32(cam, (unsigned)(size_t)sc2_vt);
    if (++n_sc2_patch <= 3)
        r1_log("split scope: manager %08x's scope camera %08x runs in the second human's pad slot (rel027s)", mgr1, cam);
}

typedef void (TC *sc2_gui_fn)(void *gui, unsigned weapon, unsigned flag);   /* FUN_007AE720 (uGUI_Scope show / hide), ret 8 */
typedef unsigned (FC *sc2_hide_fn)(unsigned unit);                           /* FUN_006A90D0: the weapon model is hidden with the unit */

static unsigned sc2_weapon(unsigned unit)
{
    unsigned pack = rd32(unit + 0xFE8), cur = pack ? rd32(pack + 0xD4) : 0xF;
    return pack && cur < 12 ? rd32(pack + 4 + 4 * cur) : 0;
}

static void sc2_undo(const char *why)
{
    if (!sc2.on)
        return;
    sc2.on = 0;
    n_sc2_off++;
    if (is_player_class(sc2.unit)) {
        if (sc2.unit_had)
            wr32(sc2.unit + 0xC, rd32(sc2.unit + 0xC) | 0x40000u);
        if (sc2.weapon_had && sc2.weapon && rd32(sc2.unit + 0xFE8) && sc2_weapon(sc2.unit) == sc2.weapon)
            wr32(sc2.weapon + 0xC, rd32(sc2.weapon + 0xC) | 0x40000u);
    }
    if (sc2.gui && sc2.cp && hud.cp == sc2.cp && in_group(COCKPIT_GROUP, sc2.cp))
        ((sc2_gui_fn)(size_t)0x7AE720)((void *)(size_t)sc2.gui, 0, 1);
    r1_log("split scope: the second human's scope view ends (%s): model, overlay back", why);
    memset(&sc2, 0, sizeof sc2);
}

static void sc2_apply(unsigned unit)
{
    unsigned wp = sc2_weapon(unit);
    memset(&sc2, 0, sizeof sc2);
    sc2.unit = unit, sc2.weapon = wp, sc2.on = 1;
    n_sc2_on++;
    if (scope2_mask & 2) {
        unsigned f = rd32(unit + 0xC);
        sc2.unit_had = f & 0x40000u;
        wr32(unit + 0xC, f & ~0x40000u);   /* hidden in viewport 2 (the engine clears bit 16 = viewport 0 for the own human) */
        if (wp && ((sc2_hide_fn)(size_t)0x6A90D0)(unit) & 0xFFu) {
            sc2.weapon_had = rd32(wp + 0xC) & 0x40000u;
            wr32(wp + 0xC, rd32(wp + 0xC) & ~0x40000u);
        }
    }
    if ((scope2_mask & 4) && hud.cp && in_group(COCKPIT_GROUP, hud.cp)) {
        unsigned gui = rd32(hud.cp + 0x54);
        if (gui) {
            sc2.gui = gui, sc2.cp = hud.cp;
            ((sc2_gui_fn)(size_t)0x7AE720)((void *)(size_t)gui, wp, 1);
        }
    }
    r1_log("split scope: the second human (unit %08x) looks through his scope (weapon %08x): model %s, overlay %s", unit, wp, (scope2_mask & 2) ? "hidden in viewport 2" : "untouched",
           sc2.gui ? "shown in his cockpit" : "not shown");
}

/* once per pass: up = split on and gameplay view (mgr1 = the second camera manager); 0 = anything that ends the split / suspends it */
static void scope2_frame(int up, unsigned mgr1)
{
    if (!up || !scope2_mask || !mgr1) {
        sc2_undo(up ? "switched off" : "split view ended");
        sc2_unpatch();
        return;
    }
    if (scope2_mask & 1)
        sc2_patch(mgr1);
    else if (sc2_obj)
        sc2_unpatch();
    unsigned si = rd32(mgr1 + 0xF0), cur = rd32(mgr1 + 0xCC), unit = rd32(mgr1 + 0x74);
    /* (the aim flag unit+0x1151: while the second human's Genesis exchange runs the other human is manager 1's target for a moment with its camera still on the scope) */
    int want = (scope2_mask & 6) && si != 0xFFFFFFFFu && cur == si && is_player_class(unit) && unit_by_slot(1 - (unsigned)capture_own_effective()) == unit && rd8(unit + 0x1151);
    if (sc2.on && (!want || sc2.unit != unit))
        sc2_undo(want ? "another human" : "the camera left the scope");
    if (want && !sc2.on)
        sc2_apply(unit);
    else if (want && sc2.on && (scope2_mask & 4) && !sc2.gui && hud.cp && in_group(COCKPIT_GROUP, hud.cp) && rd32(hud.cp + 0x54)) {   /* the second HUD set was built after the scope began */
        unsigned gui = rd32(hud.cp + 0x54);
        sc2.gui = gui, sc2.cp = hud.cp;
        ((sc2_gui_fn)(size_t)0x7AE720)((void *)(size_t)gui, sc2.weapon, 1);
    }
}

/* ====================================================================== the engine's scene state (fix023) ====================================================================== */
/* Player report 2026-11-17: "the cutscenes do not appear in full screen but remain in split-screen". The old rule (viewport 0 shows another camera than manager 0)
 * never fired in any lab log; the whole views of the lab came from the GUI rule (pause / game over managers running during loads and level-start movies). So the
 * split also asks the engine what runs:
 *   MOVIE  the movie player (sFsmManager [0x1169114], its state +0x2040: 2 delay, 3 preparing, 4 first frames, 5 playing, 6 / 7 the end; cut1.h)
 *   SCENE  an in-engine cutscene: a slot of the scene manager (sPcsManager [0x116A34C], 16 slots: unit +0x284 + 4 i, ended byte +0x1EC + i) running a resource
 *          whose name starts with "demo\" (FsmStartPcsDemo of the story; "game\scene\..." scripts are background loops: rain, waves, draw-off)
 *   CAMERA manager 0's current camera behaviour (list [+0xE4], index +0xCC) is an event camera (cPcsCamera / cMotionCamera)
 * scene_mask (LAB `split scenerule <mask>`): 1 MOVIE, 2 SCENE alone, 4 CAMERA alone, 8 SCENE and CAMERA together. A cutscene slot counts for at most
 * scene_demo_cap_ms after it started (a "demo\" resource that keeps running while the game is played cannot hold the whole view for good); the whole view
 * ends scene_hold_ms after the last signal (back-to-back scenes, a skip with START: no flicker, no stuck view). */
static unsigned sc_fsm_cell = 0x1169114u, sc_pcs_cell = 0x116A34Cu;
static unsigned scene_mask = 0x9, scene_hold_ms = 400, scene_demo_cap_ms = 240000;
static struct { unsigned unit; int demo; DWORD t0; char name[48]; } sc_slot[16];
static DWORD sc_last_seen;
/* fix026 (issue #2, owner runs 2026-11-22): the ship-shake scene (demo\d0	_020_0 / 11_040_0) repeats every ~8 s and door animations are short event-camera demos: none of them
 * is a story cutscene of both characters, and switching the whole screen (second HUD set removed and rebuilt each time) was unneeded and ended in a crash at a section change.
 * AMBIENT: a demo name from the list below, or one that started twice or more within 90 s, never holds the whole view; DEBOUNCE: a scene / camera signal (not a movie) must
 * last scene_debounce_ms without a break before the whole view is taken, so a door animation (~1 s) stays in the split. REV1_SPLIT_AMBIENT=0 or `split ambient 0` = the old rule. */
static int scene_ambient = -1;
static unsigned scene_debounce_ms = 900;
static DWORD sc_hit_since;
static struct { char name[48]; DWORD t[3]; } sc_hist[8];
static unsigned n_sc_ambient;
static int sc_ambient_on(void)
{
    if (scene_ambient < 0) {
        char v[16];
        scene_ambient = r1_env("REV1_SPLIT_AMBIENT", v, sizeof v) && v[0] == '0' ? 0 : 1;
    }
    return scene_ambient;
}
static int sc_is_ambient_name(const char *n)
{
    static const char *const list[] = {"11_020_0", "11_040_0", NULL};   /* the ship shake of the intro deck (flags 4382 / 4383, sub fsm *_sub_vib) */
    for (int i = 0; list[i]; i++)
        if (strstr(n, list[i]))
            return 1;
    return 0;
}
/* a demo slot started: 1 = it is ambient (listed, or the 3rd start within 90 s) */
static int sc_note_start(const char *name, DWORD now)
{
    if (sc_is_ambient_name(name))
        return 1;
    int k = -1, free_k = -1;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(sc_hist[i].name, name)) { k = i; break; }
        if (!sc_hist[i].name[0] && free_k < 0) free_k = i;
    }
    if (k < 0) {
        k = free_k >= 0 ? free_k : (int)(now % 8);
        snprintf(sc_hist[k].name, sizeof sc_hist[k].name, "%s", name);
        memset(sc_hist[k].t, 0, sizeof sc_hist[k].t);
    }
    int recent = 0;
    for (int j = 0; j < 3; j++)
        if (sc_hist[k].t[j] && now - sc_hist[k].t[j] < 90000)
            recent++;
    sc_hist[k].t[2] = sc_hist[k].t[1], sc_hist[k].t[1] = sc_hist[k].t[0], sc_hist[k].t[0] = now ? now : 1;
    return recent >= 2;   /* the 3rd start within 90 s (a Retry replays a cutscene once; the shake came every 7-14 s) */
}
static int sc_on;
static unsigned n_sc_movie, n_sc_demo, n_sc_cam, n_sc_capped;
void split_test_set_scene_cells(unsigned fsm_cell, unsigned pcs_cell) { sc_fsm_cell = fsm_cell, sc_pcs_cell = pcs_cell; }

static int sc_name(unsigned addr, char *out, int size)
{
    out[0] = 0;
    if (addr < 0x10000)
        return 0;
    char tmp[64];
    memset(tmp, 0, sizeof tmp);
    if (!r1_safe_read((void *)(size_t)addr, tmp, sizeof tmp - 1))
        return 0;
    int n = 0;
    while (n < (int)sizeof tmp - 1 && tmp[n] >= 0x20 && tmp[n] < 0x7F)
        n++;
    if (n < 3 || (n < (int)sizeof tmp - 1 && tmp[n] != 0))
        return 0;
    snprintf(out, (size_t)size, "%.*s", n, tmp);
    return 1;
}
/* the slot's resource name. FUN_008916E0 0x891868: `lea ecx,[unit+0x30]; push 0; push 0; push res; push record; call 0xA25480`, which stores the resource at
 * [[unit+0x3C] + 8] (its first 0xD0 entry); the name is inline at res+8 (the barrier hook's '@8', e.g. "demo\d0\11_020_0\11_020_0_main"). Live f23d: the four
 * running slots of 1-2 read "game\scene\12\12_00\12_00_s200_scannertu...", "...50_70_may...", "demo\d0\12_029_0\12_029_0_main", "demo\d0\12_092_0\12_092_0_main"
 * (the two demos were armed during gameplay: a running "demo\" slot alone is NOT a cutscene on screen). */
static int sc_slot_name(unsigned pcs, unsigned i, char *out, int size)
{
    unsigned unit = rd32(pcs + 0x284 + 4 * i), arr = unit ? rd32(unit + 0x3C) : 0, res = arr ? rd32(arr + 8) : 0;
    if (res && sc_name(res + 8, out, size))
        return 1;
    snprintf(out, (size_t)size, "? unit %08x res %08x", unit, res);
    return 0;
}
static int sc_is_cutscene_name(const char *n) { return (n[0] == 'd' || n[0] == 'D') && (n[1] == 'e' || n[1] == 'E') && (n[2] == 'm' || n[2] == 'M') && (n[3] == 'o' || n[3] == 'O') && (n[4] == '\\' || n[4] == '/'); }

/* the signals of this pass: bit 1 MOVIE, 2 SCENE, 4 CAMERA; *what = the first running cutscene's name */
static unsigned scene_signals(unsigned mgr0, const char **what)
{
    unsigned s = 0;
    DWORD now = GetTickCount();
    unsigned fm = rd32(sc_fsm_cell), mst = fm ? rd32(fm + 0x2040) : 0;
    if (mst >= 2 && mst <= 7)
        s |= 1;
    unsigned pcs = rd32(sc_pcs_cell);
    for (unsigned i = 0; i < 16; i++) {
        unsigned u = pcs ? rd32(pcs + 0x284 + 4 * i) : 0;
        if (!u) {
            sc_slot[i].unit = 0;
            continue;
        }
        if (u != sc_slot[i].unit) {   /* a new scene in this slot: its name once */
            sc_slot[i].unit = u, sc_slot[i].t0 = now;
            sc_slot[i].demo = sc_slot_name(pcs, i, sc_slot[i].name, sizeof sc_slot[i].name) && sc_is_cutscene_name(sc_slot[i].name);
            if (sc_slot[i].demo && sc_ambient_on() && sc_note_start(sc_slot[i].name, now)) {
                sc_slot[i].demo = 0;   /* ambient: logged once, it never counts */
                n_sc_ambient++;
                r1_log("split: the demo '%s' is ambient (listed or repeated): it does not take the whole view", sc_slot[i].name);
            }
        }
        if (sc_slot[i].demo && !rd8(pcs + 0x1EC + i)) {
            if (now - sc_slot[i].t0 < scene_demo_cap_ms) {
                if (!(s & 2))
                    *what = sc_slot[i].name;
                s |= 2;
            } else if (sc_slot[i].demo == 1) {
                sc_slot[i].demo = 2;   /* logged once: it no longer counts */
                n_sc_capped++;
                r1_log("split: the cutscene '%s' has run for %u s: it no longer holds the whole view", sc_slot[i].name, scene_demo_cap_ms / 1000);
            }
        }
    }
    unsigned idx = mgr0 ? rd32(mgr0 + 0xCC) : 0xFFFFFFFFu, cnt = mgr0 ? rd32(mgr0 + 0xD8) : 0, arr = mgr0 ? rd32(mgr0 + 0xE4) : 0;
    if (idx < cnt && idx < 16 && arr) {
        char cn[64];
        unsigned cb = rd32(arr + 4 * idx);
        if (cb && classes_object_name(cb, cn, sizeof cn) && (!strcmp(cn, "cPcsCamera") || !strcmp(cn, "cMotionCamera")))
            s |= 4;
    }
    return s;
}

/* rel027 (issue #2, 2026-11-24): the in-engine demo 13_050_0 (Jill alone, Parker waits at his spot with PcsMainCharacterThinkOff until she reaches a zone) came back nine times in 2 minutes
 * (10 s apart, ~1 s each) and took the whole view every time although the slot rule above saw no repeated start (no "is ambient" line in the report). The same demo name that has taken the
 * whole view twice within 90 s does not take it a third time until it has been quiet for 20 s (movies never; camera-only signals never). */
static struct { char name[48]; DWORD t[2]; DWORD last_seen; int supp; } sc_take[4];
static unsigned n_sc_take_supp;
static int sc_take_allowed(const char *name, DWORD now)
{
    int k = -1, fr = -1;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(sc_take[i].name, name)) { k = i; break; }
        if (fr < 0 && (!sc_take[i].name[0] || now - sc_take[i].last_seen > 120000)) fr = i;
    }
    if (k < 0) {
        k = fr >= 0 ? fr : (int)(now % 4);
        memset(&sc_take[k], 0, sizeof sc_take[k]);
        snprintf(sc_take[k].name, sizeof sc_take[k].name, "%s", name);
    }
    if (sc_take[k].supp && sc_take[k].last_seen && now - sc_take[k].last_seen < 20000) {
        sc_take[k].last_seen = now;   /* still repeating: stays suppressed until 20 s without a hit */
        return 0;
    }
    sc_take[k].supp = 0;
    int recent = 0;
    for (int j = 0; j < 2; j++)
        if (sc_take[k].t[j] && now - sc_take[k].t[j] < 90000) recent++;
    sc_take[k].last_seen = now;
    if (recent >= 2) {
        sc_take[k].supp = 1;
        n_sc_take_supp++;
        r1_log("split: the demo '%s' took the whole view twice in 90 s and is back: it stays in the split until it has been quiet for 20 s", name);
        return 0;
    }
    sc_take[k].t[1] = sc_take[k].t[0], sc_take[k].t[0] = now ? now : 1;
    return 1;
}

static int scene_rule(unsigned mgr0, const char **why)
{
    const char *what = "";
    unsigned s = scene_signals(mgr0, &what);
    DWORD now = GetTickCount();
    int hit = ((scene_mask & 1) && (s & 1)) || ((scene_mask & 2) && (s & 2)) || ((scene_mask & 4) && (s & 4)) || ((scene_mask & 8) && (s & 6) == 6);
    if (hit && !(s & 1) && sc_ambient_on() && !sc_on) {   /* debounce: a short scene / camera signal stays in the split */
        if (!sc_hit_since)
            sc_hit_since = now ? now : 1;
        if (now - sc_hit_since < scene_debounce_ms)
            return 0;
    }
    if (!hit)
        sc_hit_since = 0;
    if (hit && !sc_on && !(s & 1) && (s & 2) && what[0] && sc_ambient_on() && !sc_take_allowed(what, now))
        hit = 0;   /* rel027: a demo that keeps coming back stays in the split */
    if (hit) {
        if (!sc_on) {
            if (s & 1) n_sc_movie++;
            if (s & 2) n_sc_demo++;
            if (s & 4) n_sc_cam++;
        }
        sc_on = 1, sc_last_seen = now;
        static char line[96];
        snprintf(line, sizeof line, "%s%s%s%s", (s & 1) && (scene_mask & 1) ? "a movie " : "", s & 2 ? "an in-engine cutscene '" : "", s & 2 ? what : "", s & 2 ? "' " : "");
        if ((s & 4) && (scene_mask & 12))
            snprintf(line + strlen(line), sizeof line - strlen(line), "(event camera)");
        *why = line;
    } else if (sc_on && now - sc_last_seen >= scene_hold_ms) {
        sc_on = 0;
    }
    return sc_on;
}

static void scene_log_pass(void);   /* fix023 LAB (below, with the probes) */

/* ====================================================================== viewports ====================================================================== */
static void teardown(const char *why)
{
    aim_view(0, 0, 0);
    scope2_frame(0, 0);
    exam_end(why);
    doc_prev = 0;
    item_menu_mask(0, 0);
    if (mx.on)
        menu_leave(why);
    testmode_pad_merge(0);
    capture_set_interact(0, 0);
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

/* fix026m: the second human of a split game without ground (he is further from the first human than the loaded stage reaches: the stage streams around the first human only and the
 * engine's fall-out kill spares him) is put next to the first human (fallres1.h frs_split_step). Console `split fall 0|1`; status in `split status`-like line `split fall`. */
static int fall_on = 1;
static frs_t fall_frs;
static unsigned fall_unit, fall_n_ng;
/* the placement of the rescue (also the partner gate's): the second human goes beside the first one, velocity and fall timer cleared */
static void place_next_to(unsigned own_unit, unsigned other_unit)
{
    unsigned mo = rd32(other_unit + 0xE88);
    float pp[3] = {rdf(own_unit + 0x40), rdf(own_unit + 0x44), rdf(own_unit + 0x48)}, to[3];
    unsigned zero = 0;
    frs_target(pp, to);
    if (mo > 0x10000) wr8(mo + 0x2B4, 1);   /* the move object's teleport byte (as the network part's rescue) */
    wr(other_unit + 0x40, to, 12);
    wr(other_unit + 0x4C, &zero, 4);
    if (mo > 0x10000) wrf(mo + 0x2B8, 0.0f);
}
static void fall_rescue(unsigned own_unit, unsigned other_unit)
{
    if (!fall_on || !own_unit || !other_unit)
        return;
    if (fall_unit != other_unit)
        frs_reset(&fall_frs), fall_unit = other_unit;
    unsigned mo = rd32(other_unit + 0xE88);
    int ng = mo > 0x10000 && rd8(mo + 0x2B5) == 1 && rd8(mo + 8) == 1;
    if (ng)
        fall_n_ng++;
    float y = rdf(other_unit + 0x44), py = rdf(own_unit + 0x44);
    if (frs_split_step(&fall_frs, ng, y, py, 1, GetTickCount() | 1u)) {
        float pp[3] = {rdf(own_unit + 0x40), py, rdf(own_unit + 0x48)}, to[3], from[3] = {rdf(other_unit + 0x40), y, rdf(other_unit + 0x48)};
        place_next_to(own_unit, other_unit);
        frs_target(pp, to);
        r1_log("split: the second human (unit %08x) had no ground (or fell far below the first human): put next to him, (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) (rescue #%u)", other_unit, (double)from[0], (double)from[1], (double)from[2], (double)to[0], (double)to[1], (double)to[2], fall_frs.rescues);
    }
}

/* ====================================================================== partner gate v1 (fix026g, leash1.h) ====================================================================== */
extern void menu_strip_cut(int kind, unsigned ms);   /* menu.c: the in-game strip; 4 = L1G_LEASH_FAR, 5 = L1G_LEASH_GATE */
static ls_t ls_g;
static ds_t ds_g;   /* rel027u: descent follow (descent1.h) */
static unsigned ls_n_pass, ls_n_active;
static volatile LONG ls_gate_why_n;

/* any thread: a scene / movie / section change / restart is about to start (cut1_game.inc, rolebarrier1_game.c, the split's own cutscene edge) */
void split_gate_request(const char *why)
{
    if (!ls_g.on) return;
    ls_gate_request(&ls_g, GetTickCount());
    if (InterlockedIncrement(&ls_gate_why_n) <= 40)
        r1_log("gate: %s: the far second human will be brought to the first one (if he is more than %.0f units away)", why ? why : "?", (double)ls_g.gate_r);
}

static void leash_pads(int s0, int s1)   /* permille for the left stick of the pad that plays unit slot 0 / 1 (local pads only) */
{
    int f[4] = {1000, 1000, 1000, 1000}, sc[2] = {s0, s1};
    capture_state_t cs;
    capture_get_state(&cs);
    for (int u = 0; u < 2; u++)
        if (cs.role[u].enabled && cs.role[u].pad >= 0 && cs.role[u].pad < 4 && cs.src[cs.role[u].pad < 2 ? cs.role[u].pad : 0].kind == CAP_NATIVE && sc[u] < f[cs.role[u].pad])
            f[cs.role[u].pad] = sc[u];
    for (int k = 0; k < 4; k++)
        if (testmode_pad_damped(k, NULL) != f[k]) testmode_pad_damp(k, f[k]);
}

static void leash_frame(void)
{
    ls_n_pass++;
    unsigned r = root();
    int ok = r && !raid_active() && inited;
    unsigned mgr0 = ok ? rd32(r + 0xCE0) : 0, mgr1 = ok ? rd32(r + 0xCE4) : 0;
    ok = ok && is_cam_mgr(mgr0) && is_cam_mgr(mgr1);
    capture_state_t cs;
    unsigned u0 = 0, u1 = 0;
    if (ok) {
        capture_get_state(&cs);
        ok = cs.hooks_on && cs.role[0].enabled && cs.role[1].enabled;   /* two humans only: an AI partner is never leashed */
    }
    if (ok) {
        u0 = unit_by_slot(0), u1 = unit_by_slot(1);
        ok = u0 && u1 && (rd32(u0 + 0xC) & 0x400) && (rd32(u1 + 0xC) & 0x400);   /* both in play (a solo part switches the other off) */
    }
    int quiet = ok && (rd32(vp_addr(r, 0) + 4) != mgr0 || gui_running() || (active && suspended) || downed1_own_down());
    float p0[3] = {0}, p1[3] = {0};
    if (ok)
        for (int k = 0; k < 3; k++)
            p0[k] = rdf(u0 + 0x40 + 4 * (unsigned)k), p1[k] = rdf(u1 + 0x40 + 4 * (unsigned)k);
    uint32_t now = GetTickCount() | 1u;
    int hint = ls_step(&ls_g, p0, p1, ok && !quiet, now);
    if (ok && !quiet) ls_n_active++;
    leash_pads(ls_g.scale[0], ls_g.scale[1]);
    if (hint) {
        menu_strip_cut(4, 900);
        if (ls_g.hints <= 3 || ls_g.hints % 20 == 0) r1_log("leash: hint #%u: the humans are %.0f units apart (level %d)", ls_g.hints, (double)ls_g.dist, ls_g.level);
    }
    /* rel027u (B2): one human went down (the chapter 2 start: ONE prompt, a 220 unit jump to the beach) and the other one is still up there: put him next to the first one after a grace time */
    {
        int dwn = -1;
        if (ds_step(&ds_g, p0, p1, ok && !quiet && active, now, &dwn) && dwn >= 0 && !ls_g.gate_at) {
            unsigned du = dwn ? u1 : u0, su = dwn ? u0 : u1;
            float dh = ls_dist(p0, p1), up = (dwn ? p0[1] - p1[1] : p1[1] - p0[1]);
            place_next_to(du, su);
            ls_reset(&ls_g);
            ds_reset(&ds_g);
            menu_strip_cut(5, 4000);
            r1_log("descent: human %d went down and landed; the other one (unit %08x) was still %.0f units above him, %.0f apart: put next to him (descent follow #%u)", dwn, su, (double)up, (double)dh, ds_g.follows);
        }
    }
    /* the gate (b): a scene / movie / section change was announced: split = put the far second human next to the first one (fall rescue's placement), network = hint only */
    if (ok && ls_g.gate_at) {
        if (ls_gate_step(&ls_g, p0, p1, now)) {
            int own = capture_own_effective();
            if (active) {
                unsigned ou = own ? u1 : u0, xu = own ? u0 : u1;
                place_next_to(ou, xu);
                ls_reset(&ls_g);
                menu_strip_cut(5, 4000);
                r1_log("gate: the second human (unit %08x) was %.0f units from the first one before the scene: put next to him (gate move #%u)", xu, (double)ls_dist(p0, p1), ls_g.gate_moves);
            } else {
                menu_strip_cut(4, 3000);
                r1_log("gate: the partner is %.0f units away before the scene (network: hint only, gate #%u)", (double)ls_dist(p0, p1), ls_g.gate_moves);
            }
        }
    } else if (!ok && ls_g.gate_at && (uint32_t)(now - ls_g.gate_at) > LS_GATE_TTL_MS)
        ls_g.gate_at = 0;
}

/* the ini key leash=0|1 (menu.c reads rev1_coop.ini); REV1_LEASH wins */
void split_leash_enable(int on)
{
    char ev[8];
    if (r1_env("REV1_LEASH", ev, sizeof ev) && ev[0]) return;
    ls_g.on = on != 0;
    r1_log("leash: rev1_coop.ini leash=%d: the partner gate's leash is %s", on != 0, ls_g.on ? "on" : "off");
}

static void leash_status(char *out, int size)
{
    unsigned n0 = 0, n1 = 0;
    testmode_pad_damped(0, &n0), testmode_pad_damped(1, &n1);
    snprintf(out, (size_t)size, "split leash %s: hint %.0f u, damping %.0f u (to %d/1000 at +%.0f), gate radius %.0f u; level %d, distance %.0f, scale %d/%d, pads damped now %d/%d, sticks changed %u/%u, hints %u, active passes %u of %u, damped passes %u, gate requests %u, gate moves %u",
             ls_g.on ? "ON" : "off", (double)ls_g.n1, (double)ls_g.n2, LS_MIN_PERMILLE, (double)LS_RAMP, (double)ls_g.gate_r, ls_g.level, (double)ls_g.dist, ls_g.scale[0], ls_g.scale[1], testmode_pad_damped(0, NULL), testmode_pad_damped(1, NULL), n0, n1, ls_g.hints, ls_n_active, ls_n_pass, ls_g.damped_ms, ls_g.gates, ls_g.gate_moves);
}

void split_frame(void)
{
    unsigned r = root();
    n_pass++;
    if (!inited)
        return;
    scene_log_pass();
    mm_screen(0, 0);
    flash2_frame(0, 0);   /* (fix026f: switched on again below once two gameplay views are shown) */
    if (raid_active()) {
        if (active || have_saved || hud.cp || mx.on)
            teardown("Raid detected");
        blocked = SPLIT_BLOCK_RAID;
        aim_view(0, 0, 0);
        return;
    }
    blocked = SPLIT_BLOCK_NONE;
    if (!want_on || !r) {
        if (active || have_saved || hud.cp || mx.on)
            teardown(want_on ? "no camera root" : "switched off");
        aim_view(0, 0, 0);
        return;
    }
    unsigned mgr0 = rd32(r + 0xCE0), mgr1 = rd32(r + 0xCE4);
    if (!mgr0 || !mgr1 || !is_cam_mgr(mgr0) || !is_cam_mgr(mgr1)) {   /* no episode loaded (yet / any more): the split waits, nothing of it may stay switched on */
        if (active || have_saved || hud.cp || mx.on)
            teardown("no gameplay cameras (title / loading)");
        aim_view(0, 0, 0);
        scope2_frame(0, 0);
        return;
    }
    remember(r);
    unsigned v2 = vp_addr(r, VP_SECOND), v0 = vp_addr(r, 0);
    int vertical = layout_req == SPLIT_SIDE_BY_SIDE;   /* the engine's vertical divider: left / right halves */
    int gui = gui_running();
    testmode_pad_merge(gui == 1 && !mx.on);   /* a game over / pause screen that nobody exchanged the pads for: either human can answer (stage 6c live: the second pad did nothing) */
    int cutscene = rd32(v0 + 4) != mgr0;
    /* solo part of the story (or the partner is gone): the second human has no character, his view would freeze on the last pose: whole view for the first human meanwhile */
    unsigned other_unit = unit_by_slot((unsigned)(1 - capture_own_effective()));
    static s1_unit other_s1;   /* ep1-solo: FsmSetEnablePartner(0) keeps the unit in the list and clears its update + draw bits (+0xC 0x400 | 0x800) */
    if (s1_unit_step(&other_s1, other_unit, other_unit ? rd32(other_unit + 0xC) : 0, -1, GetTickCount()))
        r1_log("split: the other human (unit %08x) is %s", other_unit, other_s1.off ? "switched off by the story (solo part)" : "back in play");
    int solo = !other_unit || other_s1.off;
    if (!active) {
        r1_log("split: starting (%s, manager 0 %08x manager 1 %08x)", vertical ? "side by side" : "top/bottom", mgr0, mgr1);
        capture_state_t cs;
        capture_get_state(&cs);
        if (!cs.role[0].enabled && !cs.role[1].enabled) {   /* nobody bound the two humans (console use): the default local layout, first character on pad 0, second on pad 1 */
            capture_bind(0, 0);
            capture_bind(1, 1);
            r1_log("split: no capture roles were bound: character 0 -> pad 0, character 1 -> pad 1");
        }
    }
    active = 1;
    capture_ext_request(hud_req || view_req);
    {
        int own = capture_own_effective();
        capture_set_view_index(own, 0);
        capture_set_view_index(1 - own, view_req && (!mx.on || half_menu_exchange()) ? VP_SECOND : 0);
    }
    menu_frame(gui);
    other_unit = unit_by_slot((unsigned)(1 - capture_own_effective()));   /* rel027z: menu_frame may have flipped the own slot (an exchange began / ended): manager 1's target below must follow it in THIS pass (zq_5: one pass with the wrong target after every flip) */
    p2a_track();
    {
        unsigned ou = unit_by_slot((unsigned)capture_own_effective());
        evcam_guard(mgr0, ou, other_unit);   /* rel027z item 4: before the scene rule looks at the camera */
    }
    int gui_raw = gui;
    if (half_menus && gui == 2)
        gui = 0;   /* rel027h: the item menu stays in its opener's half (item_menu_mask below); pause / game over are still whole view */
    item_menu_mask(gui_raw == 2 && gui == 0, mx.on && mx.kind == 1 ? 4u : 1u);
    if (mx.on && !half_menu_open())
        gui = gui ? gui : 3;   /* the second human's menu is about to open: whole view from the first pass on (own slot flips, the cameras would swap sides) */
    /* The demo scheduler (cutscenes) and the title put their own camera object on viewport 0 (0x891E07 keeps the old one and restores it afterwards); the pause menu, the
     * game over screen and the item menu are GUI units the engine draws in EVERY viewport (squeezed into both halves). Whole view meanwhile; the split comes back by itself.
     * fix023: also while the engine's own scene state says a movie or an in-engine cutscene runs (scene_rule below: the movie player, the scene manager's cutscene
     * slots, manager 0's event camera), which leave viewport 0 on manager 0 (the player report of 2026-11-17: "cutscenes ... remain in split-screen"). */
    const char *scene_why = "";
    int scene = !gui && !cutscene ? scene_rule(mgr0, &scene_why) : 0;
    if (gui || cutscene || scene || solo) {
        capture_set_interact(0, 0);   /* fix022: whole view (menu, cutscene, solo part): the object table of the second human is not kept */
        int why = gui ? SPLIT_SUSP_GUI : cutscene ? SPLIT_SUSP_CAMERA : scene ? SPLIT_SUSP_SCENE : SPLIT_SUSP_SOLO;
        static DWORD why_logged;
        if (!suspended || (suspended != why && GetTickCount() - why_logged >= 2000)) {   /* (a reason that flickers is logged at most every 2 s) */
            if (!suspended) {
                n_susp++;
                if (why == SPLIT_SUSP_CAMERA || why == SPLIT_SUSP_SCENE)
                    split_gate_request(why == SPLIT_SUSP_SCENE ? "a scene starts (split)" : "a cutscene camera starts (split)");
            }
            suspended = why, why_logged = GetTickCount();
            r1_log("split: whole view (%s%s)", gui ? "pause / game over / item menu" : cutscene ? "cutscene or other camera on viewport 0" : scene ? "scene: " : "the other human has no character (solo part)",
                   scene && !gui && !cutscene ? scene_why : "");
        } else if (suspended != why)
            suspended = why;
        if ((gui == 1 || (!gui && (cutscene || scene))) && (hud.cp || hud.mm))   /* the doors to a load: the second HUD set must not be alive when the engine tears the area down */
            hud_destroy(gui == 1 ? "pause / game over screen opened" : "cutscene");
        if (gui == 1 || (!gui && (cutscene || scene)))
            hud_unstable();
        wr8(v2 + 0x10, 0);
        wr8(v0 + 0x13, sv.mode0);
        wr8(v2 + 0x13, sv.mode);
        aim_view(0, 0, 0);   /* fix023: whole view: every human's ray from the stock camera root (viewport 2 is off) */
        scope2_frame(0, 0);
        exam_end("whole view");
        return;
    }
    if (suspended) {
        suspended = 0;
        r1_log("split: gameplay view again, split resumes");
    }
    capture_set_interact(1, VP_SECOND);   /* fix022 (interact1.h): both humans are local and in play: the second one operates the objects he is nearest to */
    fall_rescue(unit_by_slot((unsigned)capture_own_effective()), other_unit);
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
    if (half_menu_exchange()) {   /* rel027h: the second human is the own human (viewport 0) for the length of his menu / scanner: viewport 0 shows HIS half */
        unsigned sw = m0;
        m0 = m2, m2 = sw;
    }
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
    if (other_unit && rd32(mgr1 + 0x74) != other_unit) {
        wr32(mgr1 + 0x74, other_unit);
        wr32(mgr1 + 0x78, other_unit);
    }
    if (fov_req > 1.0f) {
        wrf(mgr0 + 0x3C, fov_req);
        wrf(mgr1 + 0x3C, fov_req);
    }
    mm_screen(1, vertical);
    flash2_frame(1, other_unit);   /* fix026f: the second human's torch */
    aim_view(1, r, mgr1);   /* fix023: the second human's weapons aim through manager 1 / viewport 2 (aimray1.h) */
    scope2_frame(1, mgr1);   /* rel027s: his rifle scope (model, overlay, zoom) */
    exam_frame(mgr0, unit_by_slot((unsigned)capture_own_effective()), other_unit);   /* rel027z: the story examine in the examiner's half only */
    doc_frame();   /* rel027z: a document the second human opened is read in his half */
    xg_guard(mgr0, mgr1);   /* rel027z: no scanner camera left on a manager after an exchange */
    xg_watch(mgr0, mgr1);
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
    ls_init(&ls_g);
    ds_init(&ds_g);
    lt_add_frame_cb(leash_frame);
    char ev[16];
    if (r1_env("REV1_LEASH", ev, sizeof ev) && ev[0]) {   /* REV1_LEASH=0: off; the ini key leash=0|1 is read by menu.c (split_leash_enable) */
        ls_g.on = ev[0] != '0';
        r1_log("leash: REV1_LEASH=%s: the partner gate's leash is %s", ev, ls_g.on ? "on" : "off");
    }
    if (r1_env("REV1_SPLIT_HALFMENU", ev, sizeof ev) && ev[0] == '0')
        half_menus = 0;
    if (r1_env("REV1_SPLIT_EXAM", ev, sizeof ev) && ev[0] == '0')
        exam_sw = 0;
    if (r1_env("REV1_SPLIT_SCOPE2", ev, sizeof ev) && ev[0])
        scope2_mask = atoi(ev) & 7;
    ar1_init(&ar1_g, AD.camroot_ptr, !(r1_env("REV1_SPLIT_AIM", ev, sizeof ev) && ev[0] == '0'));   /* fix023: REV1_SPLIT_AIM=0 = the stock aim ray (lab A / B) */
    if (!ar1_g.enabled)
        r1_log("split: REV1_SPLIT_AIM=0: the second human's shots follow the first human's view (the stock aim ray, lab)");
    if (r1_env("REV1_SPLIT", ev, sizeof ev) && ev[0] && ev[0] != '0') {   /* REV1_SPLIT=v|h|1: start with the split screen requested (tests, owner demos with two pads) */
        layout_req = ev[0] == 'h' ? SPLIT_TOP_BOTTOM : SPLIT_SIDE_BY_SIDE;
        want_on = 1;
        r1_log("split: REV1_SPLIT=%s: split screen requested from the start (%s)", ev, layout_req == SPLIT_TOP_BOTTOM ? "top/bottom" : "side by side");
    }
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
unsigned split_second_minimap(void) { return hud.mm; }   /* the second HUD set's uMiniMapManager (0 = none): mapptn1_game.c logs what it shows */

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
    snprintf(out, (size_t)size, "split %s%s%s: layout %s, second HUD %s (cockpit %08x minimap %08x, %u builds, %u blocks zero-filled, minimap cell swaps %u), menu exchange %s (%u so far); passes %u, re-asserted %u, whole-view episodes %u; vp2 camera %08x",
             want_on ? "ON" : "off", blocked == SPLIT_BLOCK_RAID ? " (blocked: Raid)" : "", suspended ? (suspended == SPLIT_SUSP_GUI ? " (whole view: menu)" : suspended == SPLIT_SUSP_SOLO ? " (whole view: no second character)" : suspended == SPLIT_SUSP_SCENE ? " (whole view: scene)" : " (whole view: cutscene)") : "",
             layout_req == SPLIT_SIDE_BY_SIDE ? "side by side" : "top/bottom", hud_req ? (hud.cp ? "up" : "waiting") : "off", hud.cp, hud.mm, n_builds, n_zeroed, n_mm_swaps, mx.on ? "OPEN" : "idle", n_mx, n_pass, n_reasserted,
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

/* rel027s LAB (read-only), Discord B6: the rifle scope. Per camera manager: the current camera (+0xCC), the scope camera (list index +0xF0): its resource (+0xC0), its weapon index (+0xA0),
 * its phase (+0xCC) and zoom (+0xC8), the manager's pad slot (+0x88), its flags (+0x138) and target (+0x78); per human: unit state +0xF08, the laser sight's scope state (+0xFFC: +0x13C0, +0x13C5), aim flag +0x1151. */
static void v3(unsigned a, float *o);
static int scope_probe(char *out, int size)
{
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    int o = 0;
    unsigned r = root();
    if (!r) {
        snprintf(out, (size_t)size, "no sCamera");
        return 1;
    }
    for (unsigned k = 0; k < 2; k++) {
        unsigned m = rd32(r + 0xCE0 + 4 * k), arr = rd32(m + 0xE4), cnt = rd32(m + 0xD8), cur = rd32(m + 0xCC), si = rd32(m + 0xF0), ti = rd32(m + 0xE8);
        unsigned sc = si < cnt && arr ? rd32(arr + 4 * si) : 0, cc = cur < cnt && arr ? rd32(arr + 4 * cur) : 0;
        char n1[64], n2[64], n3[64];
        cname(sc, n1, sizeof n1), cname(cc, n2, sizeof n2), cname(rd32(sc + 0xC0), n3, sizeof n3);
        P("mgr%u %08x cur %d (%s kind %u) tps %d scope %d (%08x %s) | scope: res %08x (%s) idx %d phase %d zoom %.2f ctx %08x | mgr: padslot %d target %08x fl138 %08x\n", k, m, (int)cur, n2,
          cc ? rd32(cc + 4) & 0xFF : 0, (int)ti, (int)si, sc, n1, sc ? rd32(sc + 0xC0) : 0, n3, sc ? (int)rd32(sc + 0xA0) : -1, sc ? (int)rd32(sc + 0xCC) : -1, sc ? rdf(sc + 0xC8) : 0.0f,
          sc ? rd32(sc + 0x50) : 0, (int)rd32(m + 0x88), rd32(m + 0x78), rd32(m + 0x138));
    }
    for (unsigned k = 0; k < 2; k++) {   /* the scope camera's numbers: fov +0x10, timer +0xC4, eye +0x20, target +0x40; its table entry (resource +0x78 [idx]); the manager's output */
        unsigned m = rd32(r + 0xCE0 + 4 * k), arr = rd32(m + 0xE4), si = rd32(m + 0xF0), sc = si < rd32(m + 0xD8) && arr ? rd32(arr + 4 * si) : 0, res = sc ? rd32(sc + 0xC0) : 0, tab = res ? rd32(res + 0x78) : 0;
        unsigned ix = sc ? rd32(sc + 0xA0) : 0xFFFFFFFFu, e = tab && ix < 64 ? rd32(tab + 4 * ix) : 0;
        float eye[3], tg[3], oe[3], ot[3];
        v3(sc + 0x20, eye), v3(sc + 0x40, tg), v3(m + 0x90, oe), v3(m + 0xB0, ot);
        P("scope%u: fov %.1f timer %.2f frac %.2f phase %d | eye(%.0f,%.0f,%.0f) tgt(%.0f,%.0f,%.0f) | entry %08x: +58 %.1f +5c %.1f +70 %.2f +74 %.1f +78 %.1f +7c %.1f | mgr out fov %.1f eye(%.0f,%.0f,%.0f) tgt(%.0f,%.0f,%.0f)\n", k, sc ? rdf(sc + 0x10) : 0.0f,
          sc ? rdf(sc + 0xC4) : 0.0f, sc ? rdf(sc + 0xC8) : 0.0f, sc ? (int)rd32(sc + 0xCC) : -1, eye[0], eye[1], eye[2], tg[0], tg[1], tg[2], e, e ? rdf(e + 0x58) : 0.0f, e ? rdf(e + 0x5C) : 0.0f,
          e ? rdf(e + 0x70) : 0.0f, e ? rdf(e + 0x74) : 0.0f, e ? rdf(e + 0x78) : 0.0f, e ? rdf(e + 0x7C) : 0.0f, rdf(m + 0x8C), oe[0], oe[1], oe[2], ot[0], ot[1], ot[2]);
    }
    {
        unsigned ck = rd32(AD.cockpit_ptr), scp = ck ? rd32(ck + 0x28) : 0, c2 = hud.cp, g1 = scp ? rd32(scp + 0x54) : 0, g2 = c2 ? rd32(c2 + 0x54) : 0;
        P("cockpit stock %08x scope gui %08x fl %08x viewmask %x | second %08x scope gui %08x fl %08x viewmask %x\n", scp, g1, g1 ? rd32(g1 + 0xC) : 0, g1 ? (rd32(g1 + 0xC) >> 16) & 0x3FF : 0, c2, g2,
          g2 ? rd32(g2 + 0xC) : 0, g2 ? (rd32(g2 + 0xC) >> 16) & 0x3FF : 0);
    }
    for (unsigned k = 0; k < 2; k++) {
        unsigned m = rd32(r + 0xCE0 + 4 * k);
        P("mgr%u words +70..+88: %08x %08x %08x %08x %08x %08x %08x\n", k, rd32(m + 0x70), rd32(m + 0x74), rd32(m + 0x78), rd32(m + 0x7C), rd32(m + 0x80), rd32(m + 0x84), rd32(m + 0x88));
    }
    P("scope2: mask %d camera %s view %s (on %u off %u) wrapped calls %u\n", scope2_mask, sc2_obj ? "patched" : "stock", sc2.on ? "UP" : "off", n_sc2_on, n_sc2_off, n_sc2_calls);
    for (unsigned s = 0; s < 2; s++) {
        unsigned unit = unit_by_slot(s), ls = unit ? rd32(unit + 0xFFC) : 0, pack = unit ? rd32(unit + 0xFE8) : 0, cur = pack ? rd32(pack + 0xD4) : 0xF, wp = pack && cur < 12 ? rd32(pack + 4 + 4 * cur) : 0;
        char cn[96], wn[96];
        cname(unit, cn, sizeof cn), cname(wp, wn, sizeof wn);
        P("slot %u %08x %s state %u aim %u weapon %s %08x wid %08x flags %08x | unit flags %08x | laser %08x scope state %u (+13C5 %u) own slot %d\n", s, unit, cn, unit ? rd32(unit + 0xF08) : 0, unit ? rd8(unit + 0x1151) : 0, wn, wp,
          wp ? rd32(wp + 0xE28) : 0, wp ? rd32(wp + 0xC) : 0, unit ? rd32(unit + 0xC) : 0, ls, ls ? rd32(ls + 0x13C0) : 0, ls ? rd8(ls + 0x13C5) : 0, (int)capture_own_effective());
    }
#undef P
    return 1;
}

/* rel027z LAB (read-only): every camera behaviour of both managers (list +0xE4, count +0xD8): class, kind byte, the current one marked; flags, pad slot, target, output eye / at / fov */
static int cam_probe(char *out, int size)
{
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    int o = 0;
    unsigned r = root();
    if (!r) {
        snprintf(out, (size_t)size, "no sCamera");
        return 1;
    }
    for (unsigned k = 0; k < 2; k++) {
        unsigned m = rd32(r + 0xCE0 + 4 * k), arr = rd32(m + 0xE4), cnt = rd32(m + 0xD8), cur = rd32(m + 0xCC);
        float e[3], t[3];
        v3(m + 0x90, e), v3(m + 0xB0, t);
        P("mgr%u %08x cur %d tps %d scope %d cnt %u fl0C %08x fl138 %08x padslot %d tgt74 %08x tgt78 %08x out eye(%.0f,%.0f,%.0f) at(%.0f,%.0f,%.0f) fov %.1f asp %.3f\n", k, m, (int)cur, (int)rd32(m + 0xE8), (int)rd32(m + 0xF0), cnt, rd32(m + 0xC),
          rd32(m + 0x138), (int)rd32(m + 0x88), rd32(m + 0x74), rd32(m + 0x78), e[0], e[1], e[2], t[0], t[1], t[2], rdf(m + 0x8C), rdf(m + 0x38));
        for (unsigned i = 0; i < cnt && i < 12 && arr; i++) {
            unsigned c = rd32(arr + 4 * i);
            char cn[64];
            cname(c, cn, sizeof cn);
            P("   cam[%u]%s %08x %s kind %u flags %08x\n", i, i == cur ? "*" : " ", c, cn, c ? rd32(c + 4) & 0xFF : 0, c ? rd32(c + 0xC) : 0);
        }
    }
    unsigned g = rd32(0x116C3E4);
    P("DAT_0116C3E4 %u | vp0 cam %08x vp2 cam %08x\n", g, rd32(vp_addr(r, 0) + 4), rd32(vp_addr(r, 2) + 4));
#undef P
    return 1;
}

/* fix023 LAB (read-only): the two camera managers' eye / target / up and each human's current weapon: its clip and the rays FUN_005EFE50 made last
 * (muzzle +0x12E0, shot start +0x12F0, end +0x1300, the camera ray's near point +0x1360, end +0x1370, direction +0x1380) */
static void v3(unsigned a, float *o) { o[0] = rdf(a), o[1] = rdf(a + 4), o[2] = rdf(a + 8); }
static int aim_probe(char *out, int size)
{
    int o = 0;
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    unsigned r = root();
    if (!r) {
        snprintf(out, (size_t)size, "no sCamera");
        return 1;
    }
    float e[3], t[3], u[3];
    for (unsigned k = 0; k < 2; k++) {
        unsigned m = rd32(r + 0xCE0 + 4 * k);
        v3(m + 0x90, e), v3(m + 0xB0, t), v3(m + 0xA0, u);
        P("mgr%u %08x target %08x eye(%.0f,%.0f,%.0f) at(%.0f,%.0f,%.0f) up(%.2f,%.2f,%.2f) fov %.1f aspect %.3f cam %d flags138 %08x\n", k, m, rd32(m + 0x74), e[0], e[1], e[2], t[0], t[1], t[2],
          u[0], u[1], u[2], rdf(m + 0x8C), rdf(m + 0x38), (int)rd32(m + 0xCC), rd32(m + 0x138));
    }
    for (unsigned k = 0; k < 3; k += 2) {
        unsigned v = vp_addr(r, k);
        P("vp%u active %u cam %08x rect (%d,%d)-(%d,%d)\n", k, rd8(v + 0x10), rd32(v + 4), (int)rd32(v + 0x18), (int)rd32(v + 0x1C), (int)rd32(v + 0x20), (int)rd32(v + 0x24));
    }
    for (unsigned s = 0; s < 2; s++) {
        unsigned unit = unit_by_slot(s), pack = unit ? rd32(unit + 0xFE8) : 0, cur = pack ? rd32(pack + 0xD4) : 0xF, wp = pack && cur < 12 ? rd32(pack + 4 + 4 * cur) : 0;
        char cn[96], wn[96];
        cname(unit, cn, sizeof cn), cname(wp, wn, sizeof wn);
        unsigned ls = unit ? rd32(unit + 0xFFC) : 0;   /* the rays live in the human's uLaserSight (unit+0xFFC): the callers of FUN_005EFE50 load ecx = [unit+0xFFC] */
        P("slot %u %08x %s kind %u pos(%.0f,%.0f,%.0f) yaw %.1f hp %.0f | weapon[%u] %08x %s clip %d | sight %08x owner %08x\n", s, unit, cn, unit ? rd32(unit + 0xE30) : 0,
          rdf(unit + 0x40), rdf(unit + 0x44), rdf(unit + 0x48), 2.0 * atan2(rdf(unit + 0x54), rdf(unit + 0x5C)) * 57.29578, rdf(unit + 0xE38), cur, wp, wn,
          wp ? (int)rd32(wp + 0xF7C) : -1, ls, ls ? rd32(ls + 0x13C8) : 0);
        if (ls) {
            float a[3], b[3], c[3], d[3], f[3], g[3];
            v3(ls + 0x12E0, a), v3(ls + 0x12F0, b), v3(ls + 0x1300, c), v3(ls + 0x1360, d), v3(ls + 0x1370, f), v3(ls + 0x1380, g);
            P("   muzzle(%.0f,%.0f,%.0f) shot (%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) camray (%.0f,%.0f,%.0f)->(%.0f,%.0f,%.0f) dir(%.3f,%.3f,%.3f)\n", a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2],
              d[0], d[1], d[2], f[0], f[1], f[2], g[0], g[1], g[2]);
        }
    }
    P("aim fix %s (stubs %s), view %s, calls %u, second-view answers %u", ar1_g.enabled ? "on" : "OFF", ar1_state_name(ar1_g.state), ar1_g.snap.on ? "up" : "off", (unsigned)ar1_g.n_calls,
      (unsigned)ar1_g.n_second);
#undef P
    return 1;
}

/* fix023 LAB (read-only): the engine's scene state next to the split's own rule: viewport 0's camera, manager 0's current camera behaviour (+0xCC into the list
 * [+0xE4], count +0xD8) and its flags +0x138, the movie player (sFsmManager [0x1169114]: state +0x2040, name +0x2064), the in-engine scene manager (sPcsManager
 * [0x116A34C]: 16 slots, unit +0x284 + 4 i, ended byte +0x1EC + i, record [+0x23C] + i * 0x1370 whose resource's name is at +8), the cockpit managers' flags. */
static int scene_line(char *out, int size, int brief)
{
    int o = 0;
#define P(...) do { if (o < size - 1) o += snprintf(out + o, (size_t)(size - o), __VA_ARGS__); } while (0)
    unsigned r = root(), mgr0 = r ? rd32(r + 0xCE0) : 0, vcam = r ? rd32(r + 0x30 + 4) : 0, idx = mgr0 ? rd32(mgr0 + 0xCC) : 0xFFFFFFFFu;
    unsigned cnt = mgr0 ? rd32(mgr0 + 0xD8) : 0, arr = mgr0 ? rd32(mgr0 + 0xE4) : 0, cb = idx < cnt && arr ? rd32(arr + 4 * idx) : 0;
    char c1[64], c2[64], nm[72];
    cname(vcam, c1, sizeof c1), cname(cb, c2, sizeof c2);
    P("vp0 %s%s cam %d %s fl138 %08x", vcam == mgr0 ? "mgr0" : "OTHER ", vcam == mgr0 ? "" : c1, (int)idx, c2, mgr0 ? rd32(mgr0 + 0x138) : 0);
    unsigned fm = rd32(sc_fsm_cell), mst = fm ? rd32(fm + 0x2040) : 0;
    nm[0] = 0;
    if (fm && mst >= 2 && mst <= 7)
        sc_name(fm + 0x2064, nm, sizeof nm);
    P(" | movie st %u %s", mst, nm);
    unsigned pcs = rd32(sc_pcs_cell);
    P(" | pcs cur %d", pcs ? (int)rd32(pcs + 0x28) : -1);
    for (unsigned i = 0; pcs && i < 16; i++) {
        unsigned u = rd32(pcs + 0x284 + 4 * i);
        if (!u)
            continue;
        sc_slot_name(pcs, i, nm, sizeof nm);
        P(" [%u%s %s]", i, rd8(pcs + 0x1EC + i) ? " ended" : "", nm);
    }
    unsigned ck = rd32(AD.cockpit_ptr), cp = ck ? rd32(ck + 0x28) : 0;
    P(" | cockpit %08x gui %d", cp ? rd32(cp + 0xC) : 0, gui_running());
    if (!brief)
        P(" | split %s", suspended == SPLIT_SUSP_CAMERA ? "whole(camera)" : suspended == SPLIT_SUSP_GUI ? "whole(gui)" : suspended == SPLIT_SUSP_SOLO ? "whole(solo)" : active ? "SPLIT" : "off");
#undef P
    return 1;
}
static int scene_log_on;   /* LAB `split scenelog 1`: one log line whenever the scene state's line changes (split-scene: ...), at most 4000 lines */
static unsigned scene_log_n;
static void scene_log_pass(void)
{
    static char last[600];
    char now[600];
    if (!scene_log_on || scene_log_n >= 4000)
        return;
    scene_line(now, sizeof now, 0);
    if (strcmp(now, last)) {
        snprintf(last, sizeof last, "%s", now);
        r1_log("split-scene: %s", now);
        if (++scene_log_n == 4000)
            r1_log("split-scene: 4000 lines: the scene log stops");
    }
}

void split_cmd(const char *arg, char *out, int size)
{
    char sub[32] = "", a1[32] = "", a2[32] = "", a3[32] = "";
    sscanf(arg ? arg : "", "%31s %31s %31s %31s", sub, a1, a2, a3);
    if (!strcmp(sub, "") || !strcmp(sub, "status")) {
        split_status_text(out, size);
    } else if (!strcmp(sub, "probe")) {
        probe(out, size);
    } else if (!strcmp(sub, "hudfit")) {   /* fix026h LAB: split hudfit [0|1] = the HUD of each half in a centred 16:9 box (1, default) or squeezed to the half like the stock split (0, the old behaviour) */
        if (a1[0])
            d3d_hudfit_set(atoi(a1));
        snprintf(out, (size_t)size, "HUD fit %s (%u GUI projections adjusted so far)", d3d_hudfit_is_on() ? "ON" : "OFF (old behaviour)", d3d_hudfit_count());
    } else if (!strcmp(sub, "scanbars")) {   /* rel027z: split scanbars [0|1]: the strips of a half outside the scanner frame's 16:9 box are black while the scan view is open (1, default) or show the filtered world (0, stock) */
        extern volatile LONG d3d_scanbars_on;
        if (a1[0])
            d3d_scanbars_on = atoi(a1) != 0;
        snprintf(out, (size_t)size, "scan view strips %s", d3d_scanbars_on ? "black (ON)" : "stock (the filtered world)");
    } else if (!strcmp(sub, "mmscreen")) {   /* LAB: split mmscreen 0|1 (minimap1): the split minimaps computed with the half's size (1, default) or the whole screen's (0, old) */
        if (a1[0])
            mm_screen_on = atoi(a1) != 0;
        snprintf(out, (size_t)size, "minimap screen size: %s (reads %s, %u passes with the half's size, now %s)", mm_screen_on ? "the half's" : "the whole screen's (old behaviour)",
                 mm_screen_patched == 1 ? "redirected" : mm_screen_patched < 0 ? "REFUSED" : "not written yet", n_mm_screen_half,
                 mm_screen_cell == (uint32_t)(size_t)mm_screen_obj ? "half" : "whole");
    } else if (!strcmp(sub, "mmswap")) {   /* LAB: split mmswap 0|1 (minimap1): the second minimap's icons from its own uGUI_Map (1, default) or from the stock one (0, the old behaviour) */
        if (a1[0])
            mm_swap_on = atoi(a1) != 0;
        snprintf(out, (size_t)size, "second minimap cell swap %s (%u swaps so far)", mm_swap_on ? "ON" : "OFF (old behaviour)", n_mm_swaps);
    } else if (!strcmp(sub, "aim")) {   /* fix023 (aimray1.h): split aim [0|1] = the second human's aim ray from his own view (1, default) or the stock one (0, LAB A / B) */
        if (a1[0])
            ar1_g.enabled = atoi(a1) != 0;
        snprintf(out, (size_t)size, "aim ray of the second human: %s; stubs %s%s%s; view %s (manager 1 %08x, own %u, %d x %d); calls %u, second-view answers %u (last weapon %08x owner %08x)",
                 ar1_g.enabled ? "HIS OWN VIEW" : "stock (the first human's view)", ar1_state_name(ar1_g.state), ar1_g.why[0] ? ": " : "", ar1_g.why,
                 ar1_g.snap.on ? "up" : "off", (unsigned)ar1_g.snap.mgr1, (unsigned)ar1_g.snap.own, (int)ar1_g.snap.w, (int)ar1_g.snap.h, (unsigned)ar1_g.n_calls,
                 (unsigned)ar1_g.n_second, (unsigned)ar1_g.last_weapon, (unsigned)ar1_g.last_owner);
    } else if (!strcmp(sub, "scene")) {   /* fix023 LAB (read-only): the engine's scene state (split scene) / its change log (split scenelog 0|1) */
        scene_line(out, size, 0);
    } else if (!strcmp(sub, "units")) {   /* rel027h LAB: split units [group]: the unit groups (count + first classes), or one group's units with class, flags (+0xC) and group-list link */
        unsigned su = rd32(AD.unitmgr_ptr);
        int o = 0, g0 = a1[0] ? (int)strtoul(a1, NULL, 0) : -1;
        for (int g = 0; su && g < 0x30 && o < size - 200; g++) {
            if (g0 >= 0 && g != g0)
                continue;
            int n = 0, nd = 0;
            unsigned u = rd32(su + 0x2C + 0x18 * (unsigned)g);
            char cn[48], seen[8][48];
            unsigned seen_m[8], seen_n[8];
            for (int guard = 0; u && guard < 600; guard++, u = rd32(u + 0x14), n++) {
                unsigned f = rd32(u + 0xC);
                cn[0] = 0;
                classes_object_name(u, cn, sizeof cn);
                if (g0 >= 0 && o < size - 120) {
                    o += snprintf(out + o, (size_t)(size - o), "%s%08x %s f%08x", n ? " | " : "", u, cn, f);
                } else if (g0 < 0 && (f & 0x4800u) == 0x4800u) {   /* distinct (class, view mask) of the drawn units */
                    int k = 0;
                    for (; k < nd; k++)
                        if (seen_m[k] == ((f >> 16) & 0x3FFu) && !strcmp(seen[k], cn))
                            break;
                    if (k == nd && nd < 8)
                        strncpy(seen[nd], cn, 47), seen[nd][47] = 0, seen_m[nd] = (f >> 16) & 0x3FFu, seen_n[nd] = 0, nd++;
                    if (k < nd)
                        seen_n[k]++;
                }
            }
            if (g0 < 0 && nd) {
                o += snprintf(out + o, (size_t)(size - o), "g%x(%d):", g, n);
                for (int k = 0; k < nd && o < size - 200; k++)
                    o += snprintf(out + o, (size_t)(size - o), " %s m%x x%u,", seen[k], seen_m[k], seen_n[k]);
                o += snprintf(out + o, (size_t)(size - o), "\n");
            }
        }
        if (o == 0)
            snprintf(out, (size_t)size, "no units");
    } else if (!strcmp(sub, "doc")) {   /* rel027z LAB: split doc [0|1 | p2]: documents of the second human in his half (switch) / p2 = force the exchange for the document open now */
        if (!strcmp(a1, "p2")) {
            if (doc_open() && !mx.on && active && !suspended) {
                menu_enter(4);
                snprintf(out, (size_t)size, "document exchange %s", mx.on ? "ON (his half)" : "refused (see log)");
            } else
                snprintf(out, (size_t)size, "no document open, or an exchange / whole view is running");
        } else {
            if (a1[0] >= '0' && a1[0] <= '9')
                doc_sw = atoi(a1) != 0;
            snprintf(out, (size_t)size, "documents per half %s: opened %u (second human %u), open now %d, exchange %s", doc_sw ? "ON" : "off", n_doc, n_doc_p2, doc_open(), mx.on && mx.kind == 4 ? "ON" : "idle");
        }
    } else if (!strcmp(sub, "popup")) {   /* rel027z LAB (read only): the document units' state */
        doc_find();
        unsigned su = rd32(AD.unitmgr_ptr), f = 0;
        unsigned u = su ? rd32(su + 0x2C + 0x18 * (unsigned)COCKPIT_GROUP) : 0, ck = rd32(AD.cockpit_ptr), cp = ck ? rd32(ck + 0x28) : 0;
        for (int guard = 0; cp && guard < 40; guard++) {
            unsigned c = guard ? rd32(f + 0x270) : rd32(cp + 0x3C);
            f = c;
            if (!c)
                break;
            char cn[48];
            if (classes_object_name(c, cn, sizeof cn) && !strcmp(cn, "uGUI_InGameFile")) {
                doc_file = c;
                break;
            }
        }
        (void)u;
        snprintf(out, (size_t)size, "guide %08x flags %08x +270 %02x +281 %02x +290 %08x +294 %08x | file %08x flags %08x +270 %02x +281 %02x +280 %08x +29C %08x | doc open %d, exchange %s", doc_guide, doc_guide ? rd32(doc_guide + 0xC) : 0,
                 doc_guide ? rd8(doc_guide + 0x270) : 0, doc_guide ? rd8(doc_guide + 0x281) : 0, doc_guide ? rd32(doc_guide + 0x290) : 0, doc_guide ? rd32(doc_guide + 0x294) : 0, doc_file, doc_file ? rd32(doc_file + 0xC) : 0,
                 doc_file ? rd8(doc_file + 0x270) : 0, doc_file ? rd8(doc_file + 0x281) : 0, doc_file ? rd32(doc_file + 0x280) : 0, doc_file ? rd32(doc_file + 0x29C) : 0, doc_open(), mx.on && mx.kind == 4 ? "ON" : "idle");
    } else if (!strcmp(sub, "evcam")) {   /* rel027z: split evcam [0|1]: the first human keeps his camera when the second human's jump starts the event camera (0 = stock) */
        if (a1[0] >= '0' && a1[0] <= '9')
            evcam_sw = atoi(a1) != 0;
        snprintf(out, (size_t)size, "event camera guard %s: triggered %u times, window %s", evcam_sw ? "ON" : "off (stock)", n_ev, ev_until && (int)(ev_until - GetTickCount()) > 0 ? "OPEN" : "closed");
    } else if (!strcmp(sub, "exam")) {   /* rel027z: split exam [0|1]: the story examine (camera + message) only in the examiner's half (0 = stock: both halves) */
        if (a1[0] >= '0' && a1[0] <= '9')
            exam_sw = atoi(a1) != 0;
        snprintf(out, (size_t)size, "story examine per half %s: examines seen %u (by the second human %u), now %s", exam_sw ? "ON" : "off (stock)", n_exam, n_exam_p2, ex.on ? (ex.by ? "running (second human)" : "running (first human)") : "idle");
    } else if (!strcmp(sub, "halfmenu")) {   /* rel027h: split halfmenu [0|1]: the item menu / the second human's Genesis stay in their half (0 = the old whole view) */
        if (a1[0])
            half_menus = atoi(a1) ? 1 : 0;
        snprintf(out, (size_t)size, "half menus %s (item menu masks so far %u, menu exchange %s)", half_menus ? "ON" : "off (whole view)", n_item_masks, mx.on ? "OPEN" : "idle");
    } else if (!strcmp(sub, "ambient")) {   /* fix026 LAB: split ambient [0|1 [debounce ms]]: 0 = the old scene rule (every demo + event camera takes the whole view at once) */
        if (a1[0])
            scene_ambient = atoi(a1) ? 1 : 0, memset(sc_hist, 0, sizeof sc_hist);
        if (a2[0])
            scene_debounce_ms = (unsigned)atoi(a2);
        snprintf(out, (size_t)size, "ambient demos %s, debounce %u ms, ambient demos seen %u", sc_ambient_on() ? "stay split" : "take the whole view (old rule)", scene_debounce_ms, n_sc_ambient);
    } else if (!strcmp(sub, "scenerule")) {   /* fix023 LAB: split scenerule [<mask> [<hold ms> [<cap s>]]]: 1 movie, 2 cutscene slot, 4 event camera, 8 cutscene slot and event camera */
        if (a1[0])
            scene_mask = (unsigned)strtoul(a1, NULL, 0);
        if (a2[0])
            scene_hold_ms = (unsigned)atoi(a2);
        if (a3[0])
            scene_demo_cap_ms = 1000u * (unsigned)atoi(a3);
        snprintf(out, (size_t)size, "scene rule mask %u (1 movie, 2 cutscene slot, 4 event camera, 8 slot + camera), hold %u ms, cutscene cap %u s; now %s; entries: movie %u, cutscene %u, camera %u, capped %u",
                 scene_mask, scene_hold_ms, scene_demo_cap_ms / 1000, sc_on ? "ON" : "off", n_sc_movie, n_sc_demo, n_sc_cam, n_sc_capped);
    } else if (!strcmp(sub, "scenelog")) {
        if (a1[0])
            scene_log_on = atoi(a1) != 0, scene_log_n = 0;
        snprintf(out, (size_t)size, "scene log %s (%u lines)", scene_log_on ? "ON" : "off", scene_log_n);
    } else if (!strcmp(sub, "scope2")) {   /* rel027s B6: split scope2 [mask]: 1 camera wrappers (pad slot), 2 model hidden in viewport 2, 4 overlay in his cockpit; 0 = the stock behaviour */
        if (a1[0])
            scope2_mask = atoi(a1) & 7;
        snprintf(out, (size_t)size, "second human's scope: mask %d (1 camera wrappers %s, 2 model, 4 overlay), camera patched %s, view %s (on %u, off %u), wrapped calls %u, patches %u", scope2_mask, sc2_obj ? "ON" : "off", sc2_obj ? "yes" : "no", sc2.on ? "UP" : "off", n_sc2_on, n_sc2_off, n_sc2_calls, n_sc2_patch);
    } else if (!strcmp(sub, "scopeprobe")) {   /* rel027s LAB (read-only): both managers' scope cameras and both humans' scope state */
        scope_probe(out, size);
    } else if (!strcmp(sub, "camprobe")) {   /* rel027z LAB (read-only): all cameras of both managers */
        cam_probe(out, size);
    } else if (!strcmp(sub, "aimprobe")) {   /* fix023 LAB (read-only): both managers' eye / target and each human's current weapon's rays */
        aim_probe(out, size);
    } else if (!strcmp(sub, "fall")) {   /* fix026m: split fall rescue on / off / status */
        if (a1[0])
            fall_on = atoi(a1) != 0;
        snprintf(out, (size_t)size, "split fall rescue %s: rescues %u, no-ground passes %u (the second human is put next to the first one after %u ms without ground; falling too)", fall_on ? "ON" : "off", fall_frs.rescues, fall_n_ng, FRS_SPLIT_MS);
    } else if (!strcmp(sub, "leash")) {   /* fix026g: split leash [0|1 | n1 <u> | n2 <u> | gate <u> | gate now] */
        if (!strcmp(a1, "n1") || !strcmp(a1, "n2") || !strcmp(a1, "gate")) {
            const char *v = a2;
            if (!strcmp(a1, "gate") && !strcmp(v, "now")) split_gate_request("console");
            else if (*v) {
                float x = (float)atof(v);
                if (x > 100.0f) *(!strcmp(a1, "n1") ? &ls_g.n1 : !strcmp(a1, "n2") ? &ls_g.n2 : &ls_g.gate_r) = x;
            }
        } else if (a1[0])
            ls_g.on = atoi(a1) != 0;
        leash_status(out, size);
    } else if (!strcmp(sub, "descent")) {   /* rel027u: split descent [0|1] */
        if (a1[0] >= '0' && a1[0] <= '9')
            ds_g.on = atoi(a1) != 0, ds_reset(&ds_g);
        snprintf(out, (size_t)size, "split descent follow %s: drops seen %u, follows %u, cancelled (he came down himself) %u; who %d", ds_g.on ? "ON" : "off", ds_g.drops, ds_g.follows, ds_g.cancels, ds_g.who);
    } else if (!strcmp(sub, "fov")) {   /* split fov <degrees | 0>: experiment, see fov_req */
        fov_req = (float)atof(a1);
        snprintf(out, (size_t)size, fov_req > 1.0f ? "vertical FOV %.1f written into both camera managers every pass (stock: manager 0 %.1f, manager 1 %.1f)" : "FOV left to the engine (stock now: manager 0 %.1f, manager 1 %.1f)", fov_req > 1.0f ? fov_req : rdf(rd32(root() + 0xCE0) + 0x3C), rdf(rd32(root() + 0xCE0) + 0x3C), rdf(rd32(root() + 0xCE4) + 0x3C));
    } else if (!strcmp(sub, "flash")) {   /* fix026f: split flash [0|1]: the second human's flashlight (flash2.c) */
        if (a1[0] >= '0' && a1[0] <= '9')
            flash2_set(atoi(a1));
        flash2_status(out, size);
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
