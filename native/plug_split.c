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

#define CAMROOT_PTR   0x11E00B8u
#define SPLAYER_PTR   0x1169394u
#define GAMESYS_PTR   0x1169108u
#define COOPMGR_PTR   0x1169398u
#define RENDER_PTR    0x11E1B9Cu
#define CS_FLAG_GLOB  0x11E04ECu
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
static unsigned n_pass, n_reasserted, n_nocam, n_raid;
static unsigned last_cam2, last_mgr1;

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
    if (src_mgr1 && follow) {
        unsigned u = unit_by_slot(1);
        if (u && rd32(mgr1 + 0x74) != u) {
            wr32(mgr1 + 0x74, u);
            wr32(mgr1 + 0x78, u);
        }
    }
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

static int on_cmd(const char *name, const char *arg, char *out, int size)
{
    if (strcmp(name, "split"))
        return 0;
    char sub[32] = "", a1[32] = "", a2[32] = "", a3[32] = "";
    sscanf(arg ? arg : "", "%31s %31s %31s %31s", sub, a1, a2, a3);
    if (!strcmp(sub, "") || !strcmp(sub, "status")) {
        snprintf(out, (size_t)size, "split %s: camera %s, layout %s, follow %s; passes %u, re-asserted %u, no-camera passes %u, raid stops %u; vp2 camera %08x (mgr1 %08x)", on ? "ON" : "off",
                 src_mgr1 ? "mgr1 (partner)" : "dup (same as vp0)", vertical ? "side by side" : "top/bottom", follow ? "on" : "off", n_pass, n_reasserted, n_nocam, n_raid, last_cam2, last_mgr1);
        return 1;
    }
    if (!strcmp(sub, "probe"))
        return probe(out, size);
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
        n_pass = n_reasserted = n_nocam = 0;
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
    snprintf(out, (size_t)size, "split: probe | on [dup|mgr1] [v|h] | off | lay v|h | cam dup|mgr1 | follow on|off | vp <i> <field> <v> | status");
    return 1;
}

static void on_frame_(void) { apply(); }

static void on_exit_(void)
{
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
