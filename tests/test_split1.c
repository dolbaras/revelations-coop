/* test_split1.c (fix023): the offline harness of native/split.c, moved into the test suite from the lab's split_main_harness.c (stage 5). No game needed: fake game
 * memory, fake engine calls (cockpit factories, unit registration), stubs for the capture module and the pad layer; includes split.c and runs the whole life cycle:
 * on, layout, re-assert after an engine reset, cutscene / pause / item menu whole-view rules, cockpit recreation, the second human's menu exchange, Raid guard, off;
 * fix023: the second human's aim view (aimray1.h) follows the split's gameplay view and nothing else.
 * Build + run: sh tests/build.sh (target x86-windows-gnu: the module reads 32-bit addresses). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#include "r1.h"
#include "capture.h"
#include "split.h"

/* ---- stubs for the rest of the main DLL ---- */
void r1_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  [log] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
int r1_safe_read(const void *a, void *out, unsigned n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), a, out, n, &got) && got == n;
}
static unsigned g_root, g_mgr0, g_mgr1, g_u0, g_u1, g_demo, g_tps, g_pcscam;
int classes_object_name(unsigned obj, char *out, int size)
{
    if (obj < 0x10000)
        return 0;
    const char *n = obj == g_root ? "sGameCamera" : obj == g_mgr0 || obj == g_mgr1 ? "uCameraManage" : obj == g_u0 ? "uPl< Jill >" : obj == g_u1 ? "uPl< ParkerLight >" : obj == g_demo ? "uCamera" : obj == g_tps ? "cTPSCamera" : obj == g_pcscam ? "cPcsCamera" : "Thing";
    snprintf(out, (size_t)size, "%s", n);
    return 1;
}
static lt_frame_cb the_cb;
void lt_add_frame_cb(lt_frame_cb cb) { the_cb = cb; }

/* capture stubs */
static struct { int en, pad; } crole[2] = {{1, 0}, {1, 1}};
static int c_own_on, c_own_slot, c_ext, c_scope = -1, c_hooks = 1;
static int n_scope_calls, n_scope_slot1;
void capture_bind(int u, int p) { crole[u].en = 1, crole[u].pad = p; }
void capture_set_own(int s)
{
    if (s < 0)
        c_own_on = 0, c_own_slot = 0;
    else
        c_own_on = 1, c_own_slot = s;
}
void capture_get_state(capture_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->own_on = c_own_on, st->own_slot = (unsigned)c_own_slot;
    for (int i = 0; i < 2; i++)
        st->role[i].enabled = crole[i].en, st->role[i].pad = crole[i].pad;
}
int r1_env(const char *n, char *out, int size)
{
    (void)n;
    (void)out;
    (void)size;
    return 0;
}
void capture_ext_request(int on) { c_ext = on; }
static int c_interact;
void capture_set_interact(int on, unsigned view) { (void)view; c_interact = on; }
static unsigned c_view[2];
void capture_set_view_index(int u, unsigned v)
{
    if (u >= 0 && u < 2)
        c_view[u] = v;
}
int capture_hooks_ready(void) { return c_hooks; }
void capture_own_scope(int slot)
{
    c_scope = slot;
    if (slot >= 0) {
        n_scope_calls++;
        if (slot == 1)
            n_scope_slot1++;
    }
}
int capture_own_effective(void) { return c_own_on ? c_own_slot : 0; }
/* pad stubs */
static int pad_swapped;
static unsigned short pad1_buttons;
void testmode_pad_swap(int on) { pad_swapped = on; }
static int pad_merged;
void testmode_pad_merge(int on) { pad_merged = on; }
int testmode_pad_swapped(void) { return pad_swapped; }
int testmode_pad_peek(int user, void *out)
{
    memset(out, 0, 12);
    if (user == 1)
        *(unsigned short *)out = pad1_buttons;
    return 1;
}

static unsigned cells[16];
static void *anywhere(unsigned size) { return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
#define W32(a, v) (*(unsigned *)(size_t)(a) = (unsigned)(v))
#define W8(a, v) (*(unsigned char *)(size_t)(a) = (unsigned char)(v))
#define R8(a) (*(unsigned char *)(size_t)(a))
#define R32(a) (*(unsigned *)(size_t)(a))

/* ---- fake engine: unit manager with groups, cockpit factories ---- */
static unsigned g_su, g_ck;
static unsigned fake_vt[20];      /* shared vtable of the fake cockpit units */
static int n_v8, n_v9, n_retired;
static unsigned g_vt_scope_seen[4];
static void FC_v8(unsigned self) { (void)self; n_v8++; g_vt_scope_seen[0] = (unsigned)c_scope; }
static void FC_v9(unsigned self) { (void)self; n_v9++; g_vt_scope_seen[1] = (unsigned)c_scope; }
__attribute__((thiscall)) static void retire(unsigned self)
{
    n_retired++;
    W32(self + 0xC, (R32(self + 0xC) & ~7u) | 3);   /* state 3: to be removed */
}
__attribute__((fastcall)) static void v8_entry(unsigned self) { FC_v8(self); }
__attribute__((fastcall)) static void v9_entry(unsigned self) { FC_v9(self); }

static unsigned new_unit(unsigned size)
{
    unsigned u = (unsigned)(size_t)anywhere(size);
    W32(u, (unsigned)(size_t)fake_vt);
    return u;
}
static unsigned f_alloc(unsigned size, unsigned align)
{
    (void)align;
    return (unsigned)(size_t)anywhere(size);
}
__attribute__((fastcall)) static unsigned f_cp_ctor(unsigned self)
{
    W32(self, (unsigned)(size_t)fake_vt);
    W32(self + 0xC, 2 | 0xC00 | 0x4000 | (0xF << 16));
    return self;
}
__attribute__((fastcall)) static void f_cp_kids(unsigned m)
{
    unsigned prev = 0;
    for (int i = 0; i < 5; i++) {   /* chain: head +0x3C, next +0x270 */
        unsigned c = (unsigned)(size_t)anywhere(0x300);
        W32(c + 0xC, 0x4000 | (0xF << 16));
        if (prev)
            W32(prev + 0x270, c);
        else
            W32(m + 0x3C, c);
        prev = c;
    }
}
__attribute__((fastcall)) static void f_mm_kids(unsigned m)
{
    for (int i = 0; i < 4; i++) {
        unsigned c = (unsigned)(size_t)anywhere(0x300);
        W32(c + 0xC, 0x4000 | (0xF << 16));
        W32(m + 0x30 + 4 * i, c);
    }
}
__attribute__((thiscall)) static void f_register(unsigned su, unsigned group, unsigned unit, unsigned a, unsigned b)
{
    (void)a;
    (void)b;
    unsigned head = su + 0x2C + 0x18 * group;
    W32(unit + 0x14, R32(head));
    W32(head, unit);
}
static unsigned stock_unit(void)
{
    unsigned u = new_unit(0x400);
    W32(u + 0xC, 2 | 0xC00 | 0x4000 | 0x2000 | (0xF << 16));
    f_register(g_su, 0x16, u, 0, 0);
    return u;
}
static void make_cockpit(void)
{
    W32(g_su + 0x2C + 0x18 * 0x16, 0);   /* the load frees the group */
    unsigned m0 = stock_unit(), m1 = stock_unit(), m2 = stock_unit(), m3 = stock_unit(), m4 = stock_unit(), m5 = stock_unit();
    W32(g_ck + 0x28, m0), W32(g_ck + 0x2C, m1), W32(g_ck + 0x30, m2), W32(g_ck + 0x34, m3), W32(g_ck + 0x38, m4), W32(g_ck + 0x3C, m5);
    W32(m2 + 0xC, 2 | (0xF << 16)), W32(m3 + 0xC, 2 | (0xF << 16)), W32(m5 + 0xC, 2 | 0x4000 | (0xF << 16));   /* idle: not running */
    f_cp_kids(m0);
}
static int in_list(unsigned group, unsigned unit)
{
    for (unsigned u = R32(g_su + 0x2C + 0x18 * group); u; u = R32(u + 0x14))
        if (u == unit)
            return 1;
    return 0;
}

#include "split.c"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } else printf("ok:   %s\n", what); } while (0)
static void passes(int n)
{
    for (int i = 0; i < n; i++)
        split_frame();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned rnd = (unsigned)(size_t)anywhere(0x1000);
    (void)rnd;
    g_root = (unsigned)(size_t)anywhere(0x2000);
    g_mgr0 = (unsigned)(size_t)anywhere(0x400);
    g_mgr1 = (unsigned)(size_t)anywhere(0x400);
    g_demo = (unsigned)(size_t)anywhere(0x400);
    g_u0 = (unsigned)(size_t)anywhere(0x2000);
    g_u1 = (unsigned)(size_t)anywhere(0x2000);
    g_su = (unsigned)(size_t)anywhere(0x1000);
    g_ck = (unsigned)(size_t)anywhere(0x100);
    unsigned sp = (unsigned)(size_t)anywhere(0x200), n0 = (unsigned)(size_t)anywhere(0x40), n1 = (unsigned)(size_t)anywhere(0x40), nend = (unsigned)(size_t)anywhere(0x40);
    unsigned gs = (unsigned)(size_t)anywhere(0x100), gsd = (unsigned)(size_t)anywhere(0x100), cm = (unsigned)(size_t)anywhere(0x200);
    for (int i = 0; i < 20; i++)
        fake_vt[i] = 0x4000 + (unsigned)i;
    fake_vt[8] = (unsigned)(size_t)v8_entry, fake_vt[9] = (unsigned)(size_t)v9_entry, fake_vt[15] = (unsigned)(size_t)retire;

    split_addr_t ad = {(unsigned)(size_t)&cells[0], (unsigned)(size_t)&cells[1], (unsigned)(size_t)&cells[2], (unsigned)(size_t)&cells[3], (unsigned)(size_t)&cells[4], (unsigned)(size_t)&cells[5],
                       (unsigned)(size_t)f_alloc, (unsigned)(size_t)f_cp_ctor, (unsigned)(size_t)f_cp_kids, (unsigned)(size_t)f_alloc, (unsigned)(size_t)f_cp_ctor, (unsigned)(size_t)f_mm_kids,
                       (unsigned)(size_t)f_register, 0};
    split_test_set_addrs(&ad);
    W32(ad.camroot_ptr, g_root);
    W32(g_root + 0xCE0, g_mgr0), W32(g_root + 0xCE4, g_mgr1);
    W32(g_root + 0xCB8, 1280), W32(g_root + 0xCBC, 720);
    W32(g_mgr0 + 0xC, 2 | 0xC00), W32(g_mgr1 + 0xC, 2), W32(g_demo + 0xC, 2);
    W32(g_mgr0 + 0x74, g_u0), W32(g_mgr0 + 0x78, g_u0);
    unsigned v0 = g_root + 0x30, v2 = g_root + 0x30 + 0x190 * 2;
    W32(v0 + 4, g_mgr0), W8(v0 + 0x10, 1), W8(v0 + 0x12, 7), W8(v0 + 0x13, 0), W8(v0 + 0x14, 0);
    for (int i = 1; i < 8; i++)
        W8(g_root + 0x30 + 0x190 * i + 0x12, 7 - i);
    W32(ad.splayer_ptr, sp);
    W32(sp + 0x30, n0), W32(sp + 0x34, nend);
    W32(n0 + 8, n1), W32(n0 + 0x10, g_u0), W32(n1 + 8, nend), W32(n1 + 0x10, g_u1);
    W32(g_u0 + 0xE2C, 0), W32(g_u1 + 0xE2C, 1);
    W32(ad.gamesys_ptr, gs), W32(gs + 0x24, gsd), W32(gsd + 0x20, 0);
    W32(ad.coopmgr_ptr, cm), W32(cm + 0xF4, 0xFFFFFFFFu);
    W32(ad.cockpit_ptr, g_ck), W32(ad.unitmgr_ptr, g_su);
    make_cockpit();
    unsigned stock0 = R32(g_ck + 0x28), stock1 = R32(g_ck + 0x2C);

    split_init();
    hud_settle_ms = 0;
    CHECK(the_cb == split_frame, "split_init registered the per-pass callback");
    passes(3);
    CHECK(R8(v2 + 0x10) == 0 && !c_ext, "split not requested: nothing touched");

    char out[4000];
    split_cmd("on v", out, sizeof out);
    passes(3);
    CHECK(R8(v2 + 0x10) == 1 && R32(v2 + 4) == g_mgr1 && R8(v2 + 0x13) == 5 && R8(v0 + 0x13) == 4, "on: viewport 2 shows manager 1, modes 5 / 4");
    CHECK((R32(g_mgr1 + 0xC) & 0xC00) == 0xC00, "manager 1 gets the unit flags 0xC00");
    /* fix023: the engine writes viewport 2's rect from the layout bytes; the aim view takes its size */
    W32(v2 + 0x18, 960), W32(v2 + 0x1C, 0), W32(v2 + 0x20, 1920), W32(v2 + 0x24, 1080);
    passes(1);
    CHECK(ar1_g.snap.on == 1 && ar1_g.snap.mgr1 == g_mgr1 && ar1_g.snap.own == 0 && ar1_g.snap.w == 960 && ar1_g.snap.h == 1080,
          "fix023: gameplay view of the split: the second human's aim view is up (manager 1, viewport 2's 960 x 1080, own slot 0)");
    CHECK(ar1_g.state == AR1_OFF && ar1_g.stub[0] == 0, "fix023: the harness writes no code (the stub bytes are for the game only)");
    split_cmd("aim 0", out, sizeof out);
    CHECK(!ar1_g.enabled && strstr(out, "stock"), "fix023: `split aim 0` = the stock aim ray (lab A / B)");
    split_cmd("aim 1", out, sizeof out);
    CHECK(ar1_g.enabled && strstr(out, "HIS OWN VIEW"), "fix023: `split aim 1` = his own view again");
    CHECK(R32(g_mgr1 + 0x74) == g_u1 && R32(g_mgr1 + 0x78) == g_u1, "manager 1 follows the other human (slot 1)");
    CHECK(c_ext == 1, "capture own-slot hook requested for the HUD set");
    CHECK(hud.cp && hud.mm && in_list(0x16, hud.cp) && in_list(0x16, hud.mm), "second HUD set built and registered in group 0x16");
    CHECK(((R32(hud.cp + 0xC) >> 16) & 0x3FF) == 4 && ((R32(hud.mm + 0xC) >> 16) & 0x3FF) == 4, "second managers: view mask 4");
    CHECK(((R32(stock0 + 0xC) >> 16) & 0x3FF) == 1 && ((R32(stock1 + 0xC) >> 16) & 0x3FF) == 1, "stock managers restricted to viewport 0");
    CHECK(R32(hud.cp + 0xC) & 0x2000, "second set is marked persistent like the stock one");
    unsigned kid = R32(hud.cp + 0x3C);
    CHECK(((R32(kid + 0xC) >> 16) & 0x3FF) == 4, "second set children: view mask 4");
    ((__attribute__((fastcall)) void (*)(unsigned))(size_t)R32(R32(hud.cp) + 8 * 4))(hud.cp);   /* vtable slot 8 of the second set (our private copy) */
    CHECK(n_v8 == 1 && g_vt_scope_seen[0] == 1, "second set update phase 8 ran with the own-slot scope = the OTHER human (1)");
    ((__attribute__((fastcall)) void (*)(unsigned))(size_t)R32(R32(hud.cp) + 9 * 4))(hud.cp);
    CHECK(n_v9 == 1 && g_vt_scope_seen[1] == 1 && c_scope == -1, "phase 9 scoped too, scope closed afterwards");
    CHECK(R32(stock0) == (unsigned)(size_t)fake_vt, "stock units keep their vtable (nothing patched)");
    unsigned built = hud.cp;
    CHECK(c_view[0] == 0 && c_view[1] == 0, "view index off by default (stock behaviour)");
    split_cmd("view on", out, sizeof out);
    passes(1);
    CHECK(c_view[0] == 0 && c_view[1] == 2, "split view on: the second human's character runs with DAT_0116C3E4 = 2");
    split_cmd("view off", out, sizeof out);
    passes(1);
    CHECK(c_view[0] == 0 && c_view[1] == 0, "split view off: back to 0");

    split_cmd("lay h", out, sizeof out);
    passes(1);
    CHECK(R8(v2 + 0x13) == 3 && R8(v0 + 0x13) == 2, "layout h: modes 3 / 2");
    split_cmd("lay v", out, sizeof out);
    W8(v2 + 0x10, 0);
    W32(v2 + 4, 0);
    passes(1);
    CHECK(R8(v2 + 0x10) == 1 && R32(v2 + 4) == g_mgr1 && R8(v2 + 0x13) == 5, "re-asserted after an engine reset");

    W32(v0 + 4, g_demo);
    passes(1);
    CHECK(R8(v2 + 0x10) == 0 && R8(v0 + 0x13) == 0 && suspended == SPLIT_SUSP_CAMERA, "cutscene camera on viewport 0: whole view");
    CHECK(ar1_g.snap.on == 0, "fix023: whole view (cutscene): the aim view is off (every ray from the stock root)");
    CHECK(!hud.cp && (R32(built + 0xC) & 7) == 3, "cutscene: the second HUD set is retired (state 3) before anything can load");
    W32(v0 + 4, g_mgr0);
    passes(1);
    CHECK(R8(v2 + 0x10) == 1 && R8(v0 + 0x13) == 4 && !suspended, "gameplay camera back: split resumes");
    CHECK(ar1_g.snap.on == 1, "fix023: gameplay again: the aim view is up again");
    passes(3);
    CHECK(hud.cp && hud.cp != built && in_list(0x16, hud.cp), "gameplay again and the world unchanged: a new second HUD set is built");
    built = hud.cp;

    W32(R32(g_ck + 0x34) + 0xC, R32(R32(g_ck + 0x34) + 0xC) | 0xC00);   /* pause menu running */
    passes(1);
    CHECK(R8(v2 + 0x10) == 0 && suspended == SPLIT_SUSP_GUI, "pause menu running: whole view");
    CHECK(ar1_g.snap.on == 0, "fix023: pause menu: the aim view is off");
    CHECK(pad_merged == 1, "pause / game over running with nobody's pad exchange: the second pad is merged into pad 0 (either human can answer)");
    CHECK(!hud.cp && (R32(built + 0xC) & 7) == 3, "pause menu: the second HUD set is retired before the checkpoint load can start");
    W32(R32(g_ck + 0x34) + 0xC, R32(R32(g_ck + 0x34) + 0xC) & ~0xC00u);
    passes(1);
    CHECK(R8(v2 + 0x10) == 1 && !suspended, "pause menu closed: split resumes");
    CHECK(pad_merged == 0, "pause menu closed: the merge is off again");
    passes(3);
    CHECK(hud.cp && hud.cp != built, "pause menu closed and the world unchanged: second HUD set built again");
    built = hud.cp;

    /* second human's menu */
    CHECK(!mx.on, "menu exchange idle");
    pad1_buttons = 0x0020;   /* BACK on physical pad 1 */
    passes(1);
    CHECK(mx.on && pad_swapped && c_own_on && c_own_slot == 1 && crole[0].pad == 1 && crole[1].pad == 0, "pad 1 BACK: pads exchanged, own slot 1, roles flipped");
    CHECK(R8(v2 + 0x10) == 0, "whole view from the first pass of the exchange");
    CHECK(ar1_g.snap.on == 0, "fix023: the second human's menu (own slot flipped): the aim view is off");
    pad1_buttons = 0;
    passes(2);
    CHECK(mx.on, "no menu seen yet: exchange still open");
    W32(R32(g_ck + 0x3C) + 0xC, R32(R32(g_ck + 0x3C) + 0xC) | 0x4800);   /* item menu manager running */
    passes(3);
    CHECK(mx.on && mx.seen_gui, "item menu seen");
    W32(R32(g_ck + 0x3C) + 0xC, R32(R32(g_ck + 0x3C) + 0xC) & ~0x800u);
    passes(12);
    CHECK(!mx.on && !pad_swapped && !c_own_on && crole[0].pad == 0 && crole[1].pad == 1, "menu closed: everything back");
    CHECK(R8(v2 + 0x10) == 1, "split resumes after the menu");
    pad1_buttons = 0x0010;   /* START that opens nothing */
    passes(1);
    CHECK(mx.on, "START on pad 1: exchange");
    pad1_buttons = 0;
    {
        DWORD t = GetTickCount();
        while (GetTickCount() - t < 2700)
            Sleep(50), passes(1);
    }
    CHECK(!mx.on && !pad_swapped, "no menu opened: the exchange gives up after 2.5 s");

    /* lobby layout "first human plays the second character": own slot 1, unit 1 on pad 0, unit 0 on pad 1 */
    crole[0].pad = 1, crole[1].pad = 0, c_own_on = 1, c_own_slot = 1;
    passes(1);
    pad1_buttons = 0x0020;
    passes(1);
    CHECK(mx.on && pad_swapped && c_own_on && c_own_slot == 0 && crole[0].pad == 0 && crole[1].pad == 1, "swapped lobby layout: pad 1 BACK exchanges pads, own slot = the character on pad 1 (0), roles flipped");
    pad1_buttons = 0;
    W32(R32(g_ck + 0x3C) + 0xC, R32(R32(g_ck + 0x3C) + 0xC) | 0x4800);
    passes(2);
    W32(R32(g_ck + 0x3C) + 0xC, R32(R32(g_ck + 0x3C) + 0xC) & ~0x800u);
    passes(12);
    CHECK(!mx.on && !pad_swapped && c_own_on && c_own_slot == 1 && crole[0].pad == 1 && crole[1].pad == 0, "swapped lobby layout restored after the menu (own slot 1, unit 0 on pad 1, unit 1 on pad 0)");
    CHECK(R32(g_mgr1 + 0x74) == g_u0, "swapped layout: manager 1 follows the character that is not the own slot (slot 0)");
    CHECK(ar1_g.snap.on == 1 && ar1_g.snap.own == 1, "fix023: swapped layout (own slot 1): the aim view is up with own slot 1 (the slot-0 human aims through manager 1)");
    crole[0].pad = 0, crole[1].pad = 1, c_own_on = 0, c_own_slot = 0;
    passes(2);

    /* the load recreates the cockpit */
    make_cockpit();
    passes(1);
    CHECK(hud.cp == 0 || hud.cp != built, "cockpit recreated: old set dropped");
    unsigned wait_t = GetTickCount();
    while (!hud.cp && GetTickCount() - wait_t < 1500)
        Sleep(100), passes(1);
    CHECK(hud.cp && hud.cp != built && hud.stock_cp == R32(g_ck + 0x28), "new second set built next to the new stock set");

    /* the episode ends: camera managers freed (state 0) */
    W32(g_mgr1 + 0xC, 0);
    passes(1);
    CHECK(R8(v2 + 0x10) == 0 && !hud.cp && !active, "camera manager freed (title / loading): split ended, HUD set retired, bytes restored");
    W32(g_mgr1 + 0xC, 2);
    passes(3);
    CHECK(R8(v2 + 0x10) == 1 && active, "camera managers back: split resumes by itself");

    /* a solo part of the story: the second character does not exist */
    W32(n0 + 8, nend);
    passes(2);
    CHECK(suspended == SPLIT_SUSP_SOLO && R8(v2 + 0x10) == 0, "no character for the other human: whole view");
    CHECK(ar1_g.snap.on == 0, "fix023: solo part: the aim view is off");
    W32(n0 + 8, n1);
    passes(3);
    CHECK(!suspended && R8(v2 + 0x10) == 1, "the second character is back: split resumes");

    /* fix023: the engine's scene state (the movie player, the scene manager's cutscene slots, manager 0's event camera) */
    {
        unsigned fsmcell = (unsigned)(size_t)anywhere(0x10), pcscell = (unsigned)(size_t)anywhere(0x10);
        unsigned fsm = (unsigned)(size_t)anywhere(0x3000), pcs = (unsigned)(size_t)anywhere(0x400), recs = (unsigned)(size_t)anywhere(0x1370 * 16);
        unsigned res_demo = (unsigned)(size_t)anywhere(0x100), res_bg = (unsigned)(size_t)anywhere(0x100), arr_demo = (unsigned)(size_t)anywhere(0x100),
                 arr_bg = (unsigned)(size_t)anywhere(0x100), u_demo[4], u_bg = (unsigned)(size_t)anywhere(0x100);
        for (int k = 0; k < 4; k++)   /* the scene units: [unit+0x3C] = their entry array, its +8 = the resource (FUN_00A25480) */
            u_demo[k] = (unsigned)(size_t)anywhere(0x100), W32(u_demo[k] + 0x3C, arr_demo);
        W32(arr_demo + 8, res_demo), W32(u_bg + 0x3C, arr_bg), W32(arr_bg + 8, res_bg);
        unsigned camlist = (unsigned)(size_t)anywhere(0x40);
        g_tps = (unsigned)(size_t)anywhere(0x100), g_pcscam = (unsigned)(size_t)anywhere(0x100);
        split_test_set_scene_cells(fsmcell, pcscell);
        W32(fsmcell, fsm), W32(pcscell, pcs), W32(pcs + 0x23C, recs);
        strcpy((char *)(size_t)(res_demo + 8), "demo\\d0\\11_020_0\\11_020_0_main");
        strcpy((char *)(size_t)(res_bg + 8), "game\\scene\\11\\11_00\\11_00_s100_wavemain");
        W32(camlist + 0, g_tps), W32(camlist + 4, 0x7777), W32(camlist + 8, 0x7778), W32(camlist + 12, 0x7779), W32(camlist + 16, g_pcscam);
        W32(g_mgr0 + 0xD8, 6), W32(g_mgr0 + 0xE4, camlist), W32(g_mgr0 + 0xCC, 0);
        passes(3);
        CHECK(!suspended && R8(v2 + 0x10) == 1, "fix023 scene: nothing runs: split");
        unsigned before_hud = hud.cp;
        W32(fsm + 0x2040, 5);
        passes(1);
        CHECK(suspended == SPLIT_SUSP_SCENE && R8(v2 + 0x10) == 0 && R8(v0 + 0x13) == 0, "fix023 scene: a movie plays (player state 5) with manager 0 on viewport 0: whole view");
        CHECK(!hud.cp && (!before_hud || (R32(before_hud + 0xC) & 7) == 3), "fix023 scene: the movie retires the second HUD set like a cutscene camera");
        CHECK(ar1_g.snap.on == 0, "fix023 scene: the aim view is off during the movie");
        W32(fsm + 0x2040, 0);
        passes(1);
        CHECK(suspended == SPLIT_SUSP_SCENE, "fix023 scene: the movie ended: the whole view holds for a moment (no flicker between scenes)");
        Sleep(450);
        passes(1);
        CHECK(!suspended && R8(v2 + 0x10) == 1, "fix023 scene: 0.45 s after the movie: split again");
        /* an in-engine cutscene: a "demo\" slot running, manager 0 on its event camera */
        W32(pcs + 0x284 + 3 * 4, u_demo[0]), W8(pcs + 0x1EC + 3, 0);
        passes(1);
        CHECK(!suspended, "fix023 scene: a cutscene slot runs but manager 0 is still on its third-person camera: split (default rule = slot AND event camera)");
        W32(g_mgr0 + 0xCC, 4);
        passes(1);
        CHECK(suspended == SPLIT_SUSP_SCENE && R8(v2 + 0x10) == 0, "fix023 scene: the cutscene slot + manager 0 on cPcsCamera: whole view");
        W8(pcs + 0x1EC + 3, 1);
        passes(1);
        Sleep(450);
        passes(1);
        CHECK(!suspended, "fix023 scene: the slot says ended (START / its end): split again although the camera lags");
        W32(pcs + 0x284 + 3 * 4, 0);
        /* a background scene script with the event camera: not a cutscene */
        W32(pcs + 0x284 + 5 * 4, u_bg), W8(pcs + 0x1EC + 5, 0);
        passes(2);
        CHECK(!suspended, "fix023 scene: a game\\scene\\ background script (waves) never makes a whole view");
        W32(pcs + 0x284 + 5 * 4, 0);
        /* the cap: a cutscene slot that keeps running */
        char so[400];
        split_cmd("scenerule 9 400 1", so, sizeof so);
        W32(pcs + 0x284 + 7 * 4, u_demo[1]), W8(pcs + 0x1EC + 7, 0);
        passes(1);
        CHECK(suspended == SPLIT_SUSP_SCENE, "fix023 scene: a new cutscene slot + event camera: whole view");
        Sleep(1100);
        passes(1);
        Sleep(450);
        passes(1);
        CHECK(!suspended && n_sc_capped >= 1, "fix023 scene: a cutscene slot older than the cap no longer holds the whole view (logged once)");
        split_cmd("scenerule 2 400 240", so, sizeof so);
        W32(pcs + 0x284 + 7 * 4, u_demo[2]);   /* a new scene in the slot */
        W32(g_mgr0 + 0xCC, 0);
        passes(1);
        CHECK(suspended == SPLIT_SUSP_SCENE, "fix023 scene: `split scenerule 2`: a cutscene slot alone makes the whole view (lab)");
        split_cmd("scenerule 0", so, sizeof so);
        Sleep(450);
        passes(1);
        CHECK(!suspended, "fix023 scene: `split scenerule 0`: the old rules only");
        split_cmd("scenerule 9 400 240", so, sizeof so);
        W32(pcs + 0x284 + 7 * 4, 0);
        W32(fsm + 0x2040, 5);
        W32(R32(g_ck + 0x34) + 0xC, R32(R32(g_ck + 0x34) + 0xC) | 0xC00);   /* a movie under the pause / load managers (the level start) */
        passes(1);
        CHECK(suspended == SPLIT_SUSP_GUI, "fix023 scene: the level-start movie with the load managers running: the GUI rule (as 0.2.2)");
        W32(R32(g_ck + 0x34) + 0xC, R32(R32(g_ck + 0x34) + 0xC) & ~0xC00u);
        passes(1);
        CHECK(suspended == SPLIT_SUSP_SCENE, "fix023 scene: the managers stop, the movie goes on: still whole view (the scene rule)");
        W32(fsm + 0x2040, 0);
        Sleep(450);
        passes(2);
        CHECK(!suspended && R8(v2 + 0x10) == 1, "fix023 scene: after the movie: split");
        split_status_text(so, sizeof so);
        CHECK(strstr(so, "split ON") != NULL, "fix023 scene: status text");
    }

    /* Raid */
    unsigned live_cp = hud.cp;
    W32(cm + 0xF4, 2);
    passes(1);
    CHECK(R8(v2 + 0x10) == 0 && blocked == SPLIT_BLOCK_RAID && !hud.cp && R8(v0 + 0x13) == 0, "Raid detected: split ended, HUD set retired, bytes restored");
    CHECK(ar1_g.snap.on == 0, "fix023: Raid: the aim view is off");
    CHECK(n_retired >= 2 && (R32(live_cp + 0xC) & 7) == 3, "the second set was retired through the unit's own removal call (state 3)");
    CHECK(((R32(R32(g_ck + 0x28) + 0xC) >> 16) & 0x3FF) == 0xF, "stock view masks restored");
    W32(cm + 0xF4, 0xFFFFFFFFu);
    passes(3);
    CHECK(R8(v2 + 0x10) == 1, "campaign again: split resumes (still requested)");
    split_cmd("off", out, sizeof out);
    passes(1);
    CHECK(R8(v2 + 0x10) == 0 && R32(v2 + 4) == 0 && R8(v0 + 0x13) == 0 && R8(v2 + 0x13) == 0 && !hud.cp && c_ext == 0, "off: restored");
    CHECK(ar1_g.snap.on == 0, "fix023: split off: the aim view is off");
    split_cmd("on v", out, sizeof out);
    passes(3);
    CHECK(R8(v2 + 0x10) == 1, "on again");
    split_emergency_off();
    CHECK(R8(v2 + 0x10) == 0 && R8(v0 + 0x13) == 0 && !want_on, "splitkill (any thread): viewport bytes back at once");
    passes(2);
    CHECK(!hud.cp, "splitkill: the game thread's next pass retires the HUD set");
    crole[0].en = crole[1].en = 0;
    split_cmd("on v", out, sizeof out);
    passes(3);
    CHECK(crole[0].en && crole[1].en && crole[0].pad == 0 && crole[1].pad == 1, "split on with no capture roles bound: default local layout bound (character 0 -> pad 0, 1 -> pad 1)");
    split_cmd("off", out, sizeof out);
    passes(2);
    split_status_text(out, sizeof out);
    puts(out);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails ? 1 : 0;
}
