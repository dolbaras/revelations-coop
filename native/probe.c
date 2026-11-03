/*
 * Stage 1/2 live probes and the stage 3 partner capture for Revelations 1. Every command here runs on the game's logic thread (lt.c), so
 * reading and calling game code is as safe as the game's own code. Nothing happens unless a console command asks (or REV1_PROBELOG=1: a
 * one-line-per-interval black box in the log).
 *
 * Game structures used (build 13298487, notes\engine-map.md):
 *   sPlayer  [0x1169394]: four circular lists, list k has its sentinel at +0x28+0x14k, head pointer +0x30+0x14k, end pointer +0x34+0x14k;
 *                         a node has next at +8 and the unit at +0x10 (FUN_0067d850 walks list 0 like this)
 *   unit (uCharaBase/uNpc/uPlayer): +0x40 position, +0xE1C flags, +0xE2C slot/serial, +0xE30 kind (1 player, 2 AI, 3 remote), +0xE38 health,
 *         +0xE80 -> controller owner: [[+0xE80]+0x38]+0x34 = control type (1 local pad, 2 AI, 3 remote), +0xF08 character state,
 *         +0xF14 upper status, +0xF3C/+0xF44 flag words, +0x1138.. pad group (move xy, rotate xy, aim xy floats; run byte +0x1150, aim byte +0x1151),
 *         +0x1630 think manager (AI virtual pad at +0x50..0x64, action bits +0x158)
 *   updatePad 0x6AFBD0 __fastcall(unit): builds the pad group from the input manager (type 1), the think manager (type 2) or the net fields (type 3)
 *   sGamePad [0x11E0388]: +0x628+0x90*k pad slot data (k < 2; +0x10 = device kind, 6 = keyboard), +0x870 "current" slot index, +0x882 enabled
 *   game mode: [[0x1169108]+0x24]+0x20 (0 campaign, 1 Raid "coop")
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "r1.h"

#define TC __attribute__((thiscall))
#define FC __attribute__((fastcall))
#define SC __attribute__((stdcall))
#define CD __attribute__((cdecl))
typedef unsigned u;

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((void *)a, &v, 4);
    return v;
}
static float rdf(unsigned a)
{
    float v = 0;
    r1_safe_read((void *)a, &v, 4);
    return v;
}
static unsigned rd8(unsigned a)
{
    unsigned char v = 0;
    r1_safe_read((void *)a, &v, 1);
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
static int wrf(unsigned a, float v) { return wr(a, &v, 4); }
static unsigned hx(const char *s) { return (unsigned)strtoul(s, NULL, 16); }
static float arg_float(const char *s) { return (float)atof(s); }

static unsigned do_call(int conv, unsigned fn, const unsigned *a, int n)
{
    switch (conv) {
    case 0:
        switch (n) {
        case 0: return ((u(CD *)(void))fn)();
        case 1: return ((u(CD *)(u))fn)(a[0]);
        case 2: return ((u(CD *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(CD *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(CD *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(CD *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(CD *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    case 1:
        switch (n) {
        case 0: return ((u(SC *)(void))fn)();
        case 1: return ((u(SC *)(u))fn)(a[0]);
        case 2: return ((u(SC *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(SC *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(SC *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(SC *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(SC *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    case 2:
        switch (n) {
        case 0:
        case 1: return ((u(TC *)(u))fn)(a[0]);
        case 2: return ((u(TC *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(TC *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(TC *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(TC *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(TC *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    default:
        switch (n) {
        case 0:
        case 1: return ((u(FC *)(u))fn)(a[0]);
        case 2: return ((u(FC *)(u, u))fn)(a[0], a[1]);
        case 3: return ((u(FC *)(u, u, u))fn)(a[0], a[1], a[2]);
        case 4: return ((u(FC *)(u, u, u, u))fn)(a[0], a[1], a[2], a[3]);
        case 5: return ((u(FC *)(u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4]);
        default: return ((u(FC *)(u, u, u, u, u, u))fn)(a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    }
}

/* ---- units ---- */
static unsigned unit_ctl_obj(unsigned unit) { return rd32(rd32(unit + 0xE80) + 0x38); }   /* the object whose +0x34 is the control type */
static int unit_ctl(unsigned unit)
{
    unsigned c = unit_ctl_obj(unit);
    return c ? (int)rd32(c + 0x34) : -1;
}

static int is_unit(unsigned p, char *name, int size)
{
    return p > 0x10000 && classes_object_name(p, name, size);
}

/* the units of the four sPlayer lists */
typedef struct { unsigned unit, node; int list; } ulist_t;
static int sp_units(ulist_t *out, int max, int *dump_lists, char *dbg, int dbgsize)
{
    unsigned sp = rd32(R1_SPLAYER_PTR);
    int n = 0, o = 0;
    if (!sp)
        return 0;
    for (int k = 0; k < 4; k++) {
        unsigned base = sp + 0x28 + 0x14 * (unsigned)k, head = rd32(sp + 0x30 + 0x14 * (unsigned)k), endp = rd32(sp + 0x34 + 0x14 * (unsigned)k);
        if (dbg && dump_lists)
            o += snprintf(dbg + o, dbgsize - o, "  list %d: sentinel %08x head %08x end %08x\n", k, base, head, endp);
        unsigned node = head;
        for (int guard = 0; node && node != endp && node != base && guard < 40; guard++) {
            unsigned unit = rd32(node + 0x10);
            if (n < max)
                out[n].unit = unit, out[n].node = node, out[n].list = k, n++;
            node = rd32(node + 8);
        }
    }
    return n;
}

static void unit_line(unsigned unit, char *out, int size, int verbose)
{
    char cn[96] = "?";
    classes_object_name(unit, cn, sizeof cn);
    unsigned c = unit_ctl_obj(unit);
    int o = snprintf(out, size, "unit %08x %-22s slot(E2C)=%d kind(E30)=%d ctl=%d ctlobj=%08x flags(E1C)=%08x pos=(%.1f,%.1f,%.1f) hp(E38)=%.1f", unit, cn, (int)rd32(unit + 0xE2C),
                     (int)rd32(unit + 0xE30), unit_ctl(unit), c, rd32(unit + 0xE1C), rdf(unit + 0x40), rdf(unit + 0x44), rdf(unit + 0x48), rdf(unit + 0xE38));
    if (verbose)
        o += snprintf(out + o, size - o,
                      "\n   think(1630)=%08x net(E88)=%08x F08=%d F10=%d F14=%d F3C=%08x F44=%08x\n   pad group: move(1138)=(%.2f,%.2f) rot(1140)=(%.2f,%.2f) aim(1148)=(%.2f,%.2f) run(1150)=%u aimflag(1151)=%u",
                      rd32(unit + 0x1630), rd32(unit + 0xE88), (int)rd32(unit + 0xF08), (int)rd32(unit + 0xF10), (int)rd32(unit + 0xF14), rd32(unit + 0xF3C), rd32(unit + 0xF44),
                      rdf(unit + 0x1138), rdf(unit + 0x113C), rdf(unit + 0x1140), rdf(unit + 0x1144), rdf(unit + 0x1148), rdf(unit + 0x114C), rd8(unit + 0x1150), rd8(unit + 0x1151));
}

static void players_cmd(char *out, int size, int verbose)
{
    char dbg[600] = "";
    ulist_t l[40];
    int n = sp_units(l, 40, &(int){1}, dbg, sizeof dbg);
    unsigned sp = rd32(R1_SPLAYER_PTR);
    char cn[96] = "?";
    classes_object_name(sp, cn, sizeof cn);
    int o = snprintf(out, size, "sPlayer %08x (%s) %d nodes\n%s", sp, cn, n, dbg);
    for (int i = 0; i < n && o + 400 < size; i++) {
        o += snprintf(out + o, size - o, "[%d] list %d node %08x ", i, l[i].list, l[i].node);
        char line[900];
        unit_line(l[i].unit, line, sizeof line, verbose);
        o += snprintf(out + o, size - o, "%s\n", line);
    }
}

static unsigned find_unit(const char *what)
{
    ulist_t l[40];
    int n = sp_units(l, 40, NULL, NULL, 0);
    if (!strcmp(what, "jill") || !strcmp(what, "parker") || !strcmp(what, "p1") || !strcmp(what, "p2")) {
        int ctl_want = (!strcmp(what, "jill") || !strcmp(what, "p1")) ? 1 : 2;
        for (int i = 0; i < n; i++) {
            char cn[96];
            if (!classes_object_name(l[i].unit, cn, sizeof cn) || strncmp(cn, "uPl<", 4))
                continue;
            if (!strcmp(what, "jill") && strstr(cn, "Jill"))
                return l[i].unit;
            if (!strcmp(what, "parker") && strstr(cn, "Parker"))
                return l[i].unit;
            (void)ctl_want;
        }
        return 0;
    }
    return hx(what);
}

/* ---- globals / modes ---- */
static void mode_cmd(char *out, int size)
{
    unsigned holder = rd32(R1_GAMESYS_PTR), data = holder ? rd32(holder + 0x24) : 0, mode = data ? rd32(data + 0x20) : 0xFFFFFFFF;
    unsigned cm = rd32(R1_COOPMGR_PTR);
    char c1[96] = "?", c2[96] = "?";
    classes_object_name(holder, c1, sizeof c1);
    classes_object_name(cm, c2, sizeof c2);
    snprintf(out, size, "mode [[%08x]+0x24]+0x20 = %d  (holder %08x %s, data %08x)\nsCoopManager %08x (%s): +0xF4=%d +0xF8=%d +0x28=%08x +0x30=%08x +0x8F4=%d +0x8F0=%d", R1_GAMESYS_PTR, (int)mode, holder, c1,
             data, cm, c2, (int)rd32(cm + 0xF4), (int)rd32(cm + 0xF8), rd32(cm + 0x28), rd32(cm + 0x30), (int)rd32(cm + 0x8F4), (int)rd32(cm + 0x8F0));
}

static void gamepad_cmd(char *out, int size)
{
    unsigned gp = rd32(R1_GAMEPAD_PTR);
    char cn[96] = "?";
    classes_object_name(gp, cn, sizeof cn);
    int o = snprintf(out, size, "sGamePad %08x (%s): current slot(+870)=%d enabled(+882)=%u (+883)=%u\n", gp, cn, (int)rd32(gp + 0x870), rd8(gp + 0x882), rd8(gp + 0x883));
    for (int k = 0; k < 2; k++) {
        unsigned s = gp + 0x628 + 0x90 * (unsigned)k;
        o += snprintf(out + o, size - o, " slot %d at %08x: kind(+10)=%d stick(+28)=(%.2f,%.2f) data:", k, s, (int)rd32(s + 0x10), rdf(s + 0x28), rdf(s + 0x2C));
        for (int i = 0; i < 0x90 / 4 && o + 12 < size; i++)
            o += snprintf(out + o, size - o, "%s%08x", i % 9 == 0 ? "\n   " : " ", rd32(s + 4 * (unsigned)i));
        o += snprintf(out + o, size - o, "\n");
    }
    /* the sPad::Pad objects: Pad k = sGamePad+0x3C+0x2CC*k (sPad_ScanPad / sPad_PollPad): +4 active, +5 flag, +6 slot index, +7 kind (0 unassigned, 2 DirectInput, else XInput),
     * +0xC scan flags, +0x10 XInput user, +0x15C.. state (buttons held) */
    for (int k = 0; k < 2; k++) {
        unsigned pd = gp + 0x3C + 0x2CC * (unsigned)k;
        o += snprintf(out + o, size - o, " Pad[%d] %08x: active(+4)=%u (+5)=%u slotidx(+6)=%u kind(+7)=%u flags(+C)=%08x user(+10)=%d buttons(+15C)=%08x %08x\n", k, pd, rd8(pd + 4), rd8(pd + 5),
                      rd8(pd + 6), rd8(pd + 7), rd32(pd + 0xC), (int)rd32(pd + 0x10), rd32(pd + 0x15C), rd32(pd + 0x160));
    }
}

static void camera_cmd(char *out, int size)
{
    unsigned root = rd32(0x11E00B8);
    char cn[96] = "?";
    classes_object_name(root, cn, sizeof cn);
    unsigned mgr = root + 0xCE0;
    char mn[96] = "?";
    classes_object_name(mgr, mn, sizeof mn);
    snprintf(out, size, "camera root [0x11E00B8] = %08x (%s)\nmanager root+0xCE0 = %08x (%s): target +0x74=%08x +0x78=%08x camera index(+0xCC)=%d cams(+0xE4)=%08x fov(+0x3C)=%.3f", root, cn, mgr, mn, rd32(mgr + 0x74),
             rd32(mgr + 0x78), (int)rd32(mgr + 0xCC), rd32(mgr + 0xE4), rdf(mgr + 0x3C));
}

/* ---- inline hook machinery (as Revelations 2 trace.c) ---- */
static void *make_tramp(unsigned target, const unsigned char *expect, int n)
{
    unsigned char cur[16];
    if (n < 5 || n > 12 || !r1_safe_read((void *)target, cur, (unsigned)n) || memcmp(cur, expect, (size_t)n) != 0) {
        r1_log("probe: bytes at %08x differ from the expected build, not patching", target);
        return NULL;
    }
    unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t)
        return NULL;
    memcpy(t, cur, (size_t)n);
    t[n] = 0xE9;
    *(int *)(t + n + 1) = (int)((target + (unsigned)n) - (unsigned)(t + n + 5));
    return t;
}

static int patch_jmp(unsigned target, int n, void *hook)
{
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    unsigned char *p = (unsigned char *)target;
    for (int i = 5; i < n; i++)
        p[i] = 0x90;
    *(int *)(p + 1) = (int)((unsigned)hook - (target + 5));
    p[0] = 0xE9;
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    return 1;
}

/* ---- stage 3: partner capture ---- */
typedef void (FC *UpdatePad_t)(unsigned unit);
static UpdatePad_t orig_updatePad;
static volatile unsigned updatepad_calls;
static unsigned up_seen[8];
static int n_up_seen;

static struct {
    volatile int on;
    unsigned unit;
    int slot;          /* pad slot the unit reads while captured (sGamePad+0x870 during its updatePad) */
    int mode;          /* 0 = swap the slot, 1 = write the pad group from the console values after updatePad */
    int saved_ctl, saved_kind;
    unsigned ctl_obj;
    float mv[2], rot[2], aim[2];
    unsigned char run, aimflag;
    unsigned swaps;
} cap;

static void FC hook_updatePad(unsigned unit)
{
    updatepad_calls++;
    if (n_up_seen < 8) {
        int seen = 0;
        for (int i = 0; i < n_up_seen; i++)
            seen |= up_seen[i] == unit;
        if (!seen)
            up_seen[n_up_seen++] = unit;
    }
    if (cap.on && unit == cap.unit) {
        unsigned gp = rd32(R1_GAMEPAD_PTR);
        if (cap.mode == 0 && gp) {
            unsigned old = rd32(gp + 0x870);
            wr32(gp + 0x870, (unsigned)cap.slot);
            orig_updatePad(unit);
            wr32(gp + 0x870, old);
            cap.swaps++;
            return;
        }
        orig_updatePad(unit);
        if (cap.mode == 1) {
            wrf(unit + 0x1138, cap.mv[0]), wrf(unit + 0x113C, cap.mv[1]);
            wrf(unit + 0x1140, cap.rot[0]), wrf(unit + 0x1144, cap.rot[1]);
            wrf(unit + 0x1148, cap.aim[0]), wrf(unit + 0x114C, cap.aim[1]);
            wr8(unit + 0x1150, cap.run), wr8(unit + 0x1151, cap.aimflag);
        }
        return;
    }
    orig_updatePad(unit);
}

void probe_install(void)
{
    static int done;
    if (done || !r1_text_decrypted())
        return;
    done = 1;
    static const unsigned char b_up[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    void *t = make_tramp(R1_UPDATEPAD, b_up, 6);
    if (t) {
        orig_updatePad = (UpdatePad_t)t;
        patch_jmp(R1_UPDATEPAD, 6, (void *)hook_updatePad);
    }
    r1_log("probe: updatePad hook (passes through unless 'cap on') %s", orig_updatePad ? "installed" : "NOT installed");
}

static void cap_cmd(const char *arg, char *out, int size)
{
    char sub[16] = "", who[40] = "";
    int slot = 1, mode = 0;
    sscanf(arg, "%15s %39s %d %d", sub, who, &slot, &mode);
    if (!strcmp(sub, "on")) {
        unsigned unit = find_unit(who);
        char cn[96];
        if (!unit || !classes_object_name(unit, cn, sizeof cn)) {
            snprintf(out, size, "no such unit '%s' (cap on parker|jill|<hexaddr> [padslot=1] [mode 0 swap|1 write])", who);
            return;
        }
        if (!orig_updatePad) {
            snprintf(out, size, "updatePad hook not installed");
            return;
        }
        if (cap.on)
            snprintf(out, size, "capture already on for %08x (cap off first)", cap.unit);
        cap.unit = unit, cap.slot = slot, cap.mode = mode, cap.swaps = 0;
        cap.ctl_obj = unit_ctl_obj(unit);
        cap.saved_ctl = unit_ctl(unit), cap.saved_kind = (int)rd32(unit + 0xE30);
        if (!cap.ctl_obj) {
            snprintf(out, size, "unit has no controller object ([[unit+0xE80]+0x38] is null)");
            return;
        }
        wr32(cap.ctl_obj + 0x34, 1);   /* control type 1: updatePad reads the input manager */
        wr32(unit + 0xE30, 1);         /* kind 1: "player" (weapons, aiming tests) */
        cap.on = 1;
        snprintf(out, size, "capture ON: %s %08x reads pad slot %d (%s), control type %d -> 1, kind %d -> 1", cn, unit, slot, mode ? "console values written after updatePad" : "slot swap during updatePad",
                 cap.saved_ctl, cap.saved_kind);
    } else if (!strcmp(sub, "off")) {
        if (cap.on && cap.ctl_obj) {
            cap.on = 0;
            wr32(cap.ctl_obj + 0x34, (unsigned)cap.saved_ctl);
            wr32(cap.unit + 0xE30, (unsigned)cap.saved_kind);
            snprintf(out, size, "capture OFF: %08x control type back to %d, kind %d (%u slot swaps done)", cap.unit, cap.saved_ctl, cap.saved_kind, cap.swaps);
        } else
            snprintf(out, size, "capture was off");
    } else if (!strcmp(sub, "set")) {   /* cap set mx my rx ry ax ay run aim : mode 1 values */
        float v[6] = {0};
        int r = 0, a = 0;
        sscanf(arg, "%*s %f %f %f %f %f %f %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &r, &a);
        cap.mv[0] = v[0], cap.mv[1] = v[1], cap.rot[0] = v[2], cap.rot[1] = v[3], cap.aim[0] = v[4], cap.aim[1] = v[5], cap.run = (unsigned char)r, cap.aimflag = (unsigned char)a;
        snprintf(out, size, "mode-1 values set");
    } else {
        snprintf(out, size, "capture %s: unit %08x pad slot %d mode %d, saved ctl %d kind %d, ctl obj %08x, %u swaps; updatePad called %u times for units:", cap.on ? "ON" : "off", cap.unit, cap.slot,
                 cap.mode, cap.saved_ctl, cap.saved_kind, cap.ctl_obj, cap.swaps, updatepad_calls);
        int o = (int)strlen(out);
        for (int i = 0; i < n_up_seen; i++)
            o += snprintf(out + o, size - o, " %08x", up_seen[i]);
    }
}

/* ---- black box: one line per interval in the log (REV1_PROBELOG=<seconds> or console probelog) ---- */
static volatile unsigned probelog_ms;
static DWORD probelog_last;
static unsigned last_mode = 0xFFFFFFFE;

static void probe_frame(void)
{
    unsigned holder = rd32(R1_GAMESYS_PTR), data = holder ? rd32(holder + 0x24) : 0, mode = data ? rd32(data + 0x20) : 0xFFFFFFFF;
    if (mode != last_mode) {
        r1_log("probe: game mode [[0x1169108]+0x24]+0x20 changed %d -> %d (pass %u)", (int)last_mode, (int)mode, lt_frames());
        last_mode = mode;
    }
    if (!probelog_ms)
        return;
    DWORD now = GetTickCount();
    if (now - probelog_last < probelog_ms)
        return;
    probelog_last = now;
    ulist_t l[40];
    int n = sp_units(l, 40, NULL, NULL, 0);
    for (int i = 0; i < n && i < 8; i++) {
        char line[900];
        unit_line(l[i].unit, line, sizeof line, 0);
        r1_log("bb: list %d %s", l[i].list, line);
    }
    r1_log("bb: sGamePad cur=%d en=%u cap=%s", (int)rd32(rd32(R1_GAMEPAD_PTR) + 0x870), rd8(rd32(R1_GAMEPAD_PTR) + 0x882), cap.on ? "ON" : "off");
}

/* ---- commands ---- */
int probe_is_game_cmd(const char *n)
{
    static const char *const list[] = {"players", "unit", "mode", "gamepad", "camera", "cap", "ctl", "call", "w32", "w8", "wf", "rf", "probelog", "ltinfo", "pokesw", NULL};
    for (int i = 0; list[i]; i++)
        if (!strcmp(n, list[i]))
            return 1;
    return 0;
}

void probe_run(const char *cmd, char *out, int size)
{
    char name[32] = "", arg[360] = "";
    sscanf(cmd, "%31s %359[^\r\n]", name, arg);
    out[0] = 0;
    static int cb_added;
    if (!cb_added) {
        cb_added = 1;
        lt_add_frame_cb(probe_frame);
        char v[16];
        if (r1_env("REV1_PROBELOG", v, sizeof v) && atoi(v) > 0)
            probelog_ms = (unsigned)atoi(v) * 1000;
    }
    if (!strcmp(name, "players")) {
        players_cmd(out, size, !strcmp(arg, "v"));
    } else if (!strcmp(name, "unit")) {
        unsigned unit = find_unit(arg);
        if (!unit)
            snprintf(out, size, "no such unit (jill|parker|<hexaddr>)");
        else
            unit_line(unit, out, size, 1);
    } else if (!strcmp(name, "mode")) {
        mode_cmd(out, size);
    } else if (!strcmp(name, "gamepad")) {
        gamepad_cmd(out, size);
    } else if (!strcmp(name, "camera")) {
        camera_cmd(out, size);
    } else if (!strcmp(name, "cap")) {
        cap_cmd(arg, out, size);
    } else if (!strcmp(name, "ctl")) {   /* ctl <unit> <type> [kind]: write control type (and E30) */
        char w[40] = "";
        int type = -1, kind = -1;
        sscanf(arg, "%39s %d %d", w, &type, &kind);
        unsigned unit = find_unit(w);
        unsigned c = unit ? unit_ctl_obj(unit) : 0;
        if (!c || type < 0) {
            snprintf(out, size, "usage: ctl <jill|parker|hexaddr> <type> [kind]");
        } else {
            int was = unit_ctl(unit), wk = (int)rd32(unit + 0xE30);
            wr32(c + 0x34, (unsigned)type);
            if (kind >= 0)
                wr32(unit + 0xE30, (unsigned)kind);
            snprintf(out, size, "unit %08x control type %d -> %d, kind %d -> %d", unit, was, type, wk, kind >= 0 ? kind : wk);
        }
    } else if (!strcmp(name, "probelog")) {
        int sec = atoi(arg);
        probelog_ms = sec > 0 ? (unsigned)sec * 1000 : 0;
        snprintf(out, size, "black box every %d s in the log (0 = off)", sec);
    } else if (!strcmp(name, "ltinfo")) {
        snprintf(out, size, "logic hook installed, passes %u, thread %lu, updatePad calls %u", lt_frames(), GetCurrentThreadId(), updatepad_calls);
    } else if (!strcmp(name, "call")) {
        char cv[16], fa[16];
        unsigned a[8] = {0};
        int n = 0;
        char *rest = arg;
        int consumed = 0;
        if (sscanf(rest, "%15s %15s%n", cv, fa, &consumed) < 2) {
            snprintf(out, size, "usage: call <cdecl|stdcall|thiscall|fastcall> <hexaddr> [hex args or f:float...] (up to 6)");
            return;
        }
        rest += consumed;
        char tok[40];
        int c2;
        while (n < 6 && sscanf(rest, "%39s%n", tok, &c2) == 1) {
            if (tok[0] == 'f' && tok[1] == ':') {
                float f = arg_float(tok + 2);
                memcpy(&a[n++], &f, 4);
            } else
                a[n++] = hx(tok);
            rest += c2;
        }
        int conv = !strcmp(cv, "cdecl") ? 0 : !strcmp(cv, "stdcall") ? 1 : !strcmp(cv, "thiscall") ? 2 : 3;
        unsigned r = do_call(conv, hx(fa), a, n);
        snprintf(out, size, "returned %08x (%d)", r, (int)r);
    } else if (!strcmp(name, "w32") || !strcmp(name, "w8") || !strcmp(name, "wf")) {
        char as[20], vs[40];
        if (sscanf(arg, "%19s %39s", as, vs) != 2) {
            snprintf(out, size, "usage: %s <hexaddr> <value>", name);
            return;
        }
        int ok;
        if (!strcmp(name, "w32"))
            ok = wr32(hx(as), hx(vs));
        else if (!strcmp(name, "w8"))
            ok = wr8(hx(as), hx(vs));
        else
            ok = wrf(hx(as), arg_float(vs));
        snprintf(out, size, ok ? "written" : "write failed");
    } else if (!strcmp(name, "rf")) {
        char as[20];
        int n = 4;
        sscanf(arg, "%19s %d", as, &n);
        int o = 0;
        for (int i = 0; i < n && i < 64 && o + 24 < size; i++)
            o += snprintf(out + o, size - o, "%.3f ", rdf(hx(as) + 4 * (unsigned)i));
    } else {
        snprintf(out, size, "unknown probe command %s", name);
    }
}
