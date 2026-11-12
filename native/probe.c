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
#include "capture.h"
#include "downed1.h"
#include "split.h"
#include "net1.h"
#include "guards1.h"
#include "mapptn1.h"
#include "rolebarrier1.h"
#include "world1_game.h"
#include "packcopy1.h"
#include "ach1_game.h"
#include "give1_game.h"   /* give1: RE5-style give to partner from the item screen */
#include "scan1_game.h"   /* scan1: the Genesis scanner for the second player */

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

/* LAB ONLY: packcopy <from slot> <to slot> - lab/owner_give.py in one logic-thread call. A section entered by `warp` gives the human that is local at the load the
 * profile's bag of that character (empty in the lab saves: only uWpUnarmed, stage6 row 84, wp1 2026-11-07 on BOTH PCs) while the AI partner gets the level's set.
 * The words +4..+0x140 of the bag (unit+0xFE8, cBioItemPack) of the unit of <from slot> are copied into the bag of <to slot>, and every weapon object found there
 * (class uWp*, not uWpUnarmed / uWpGndHand / uWpGndPulse) gets the receiver as owner (+0x30). Known side effect (stage6 row 87): a later death + Continue crashed
 * twice after such a transfer (wp6: the weapons sit in both bags). `packcopy undo` puts the receiver's words and the owners back; since rev1-int it also runs by
 * itself right before a reload through the restart hook (packcopy_before_reload). Other teardowns (section change, return to the title, warp) are NOT covered. */
static unsigned unit_of_slot(int slot)
{
    ulist_t l[40];
    int n = sp_units(l, 40, NULL, NULL, 0);
    for (int i = 0; i < n; i++) {
        char cn[96];
        if (l[i].list == 0 && classes_object_name(l[i].unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4) && (int)rd32(l[i].unit + 0xE2C) == slot)
            return l[i].unit;
    }
    return 0;
}

static pc1_t g_pc1;   /* what the copies overwrote (packcopy undo) */
static int pc1_rd(void *u, uint32_t a, uint32_t *v) { (void)u; return r1_safe_read((void *)(uintptr_t)a, v, 4); }
static int pc1_wr(void *u, uint32_t a, uint32_t v) { (void)u; return wr32(a, v); }
static int pc1_isweapon(void *u, uint32_t obj)
{
    (void)u;
    char cn[96];
    return classes_object_name(obj, cn, sizeof cn) && !strncmp(cn, "uWp", 3) && strncmp(cn, "uWpUnarmed", 10) && strncmp(cn, "uWpGndHand", 10) && strncmp(cn, "uWpGndPulse", 11);
}
static int pc1_bagof(void *u, uint32_t unit, uint32_t *bag)
{
    (void)u;
    char cn[96];
    if (!unit || !classes_object_name(unit, cn, sizeof cn) || strncmp(cn, "uPl<", 4)) return 0;
    *bag = rd32(unit + 0xFE8);
    return *bag != 0;
}
static const pc1_mem PC1M = {NULL, pc1_rd, pc1_wr, pc1_isweapon, pc1_bagof};

/* rolebarrier_on_restart (registered in probe_install): the game thread, right before FUN_00720870 reloads the level (host Continue / game-over "Yes", the guest's
 * own, the follow): a lab packcopy is undone before the teardown frees the shared weapons twice (wp6 exe+2c9700). Silent when nothing is pending. */
static void packcopy_before_reload(void)
{
    char line[220];
    if (pc1_before_reload(&g_pc1, &PC1M, line, sizeof line) >= 0) r1_log("probe: %s", line);
}

/* packcopy <from> <to> | packcopy undo | packcopy status */
static void packcopy_cmd(const char *arg, char *out, int size)
{
    int fs = -1, ts = -1;
    char w[16] = "";
    sscanf(arg, "%15s", w);
    if (!strcmp(w, "undo")) {   /* before any death / Continue / retry step of a lab run (the double free of the shared weapons on a reload, lobby-must c77a623) */
        int words = 0, owners = 0, stale = 0, bags = pc1_undo(&g_pc1, &PC1M, &words, &owners, &stale);
        snprintf(out, size, "packcopy undo: %d bag(s) put back (%d words, %d weapon owners), %d record(s) dropped (unit gone / other bag); pending now %d", bags, words, owners, stale,
                 pc1_pending(&g_pc1));
        r1_log("probe: %s", out);
        return;
    }
    if (!strcmp(w, "status")) {
        snprintf(out, size, "packcopy: %d copy(ies) pending an undo (run `packcopy undo` before any death / Continue / retry)", pc1_pending(&g_pc1));
        return;
    }
    sscanf(arg, "%d %d", &fs, &ts);
    unsigned uf = fs >= 0 ? unit_of_slot(fs) : 0, ut = ts >= 0 ? unit_of_slot(ts) : 0;
    unsigned pf = uf ? rd32(uf + 0xFE8) : 0, pt = ut ? rd32(ut + 0xFE8) : 0;
    if (!pf || !pt || fs == ts) {
        snprintf(out, size, "usage: packcopy <from slot> <to slot> | undo | status (units %08x / %08x, bags %08x / %08x)", uf, ut, pf, pt);
        return;
    }
    int words = 0, owners = 0;
    if (!pc1_copy(&g_pc1, &PC1M, pf, pt, ut, &words, &owners)) {
        snprintf(out, size, "packcopy: REFUSED (bags %08x -> %08x unreadable or no free undo record: run `packcopy undo` first)", pf, pt);
        return;
    }
    int o = snprintf(out, size, "packcopy: bag %08x (unit %08x, slot %d) -> bag %08x (unit %08x, slot %d):", pf, uf, fs, pt, ut, ts);
    for (unsigned off = PC1_FIRST; off < PC1_END && o + 80 < size; off += 4) {
        unsigned v = rd32(pt + off);
        char cn[96];
        if (v > 0x10000 && pc1_isweapon(NULL, v) && classes_object_name(v, cn, sizeof cn)) o += snprintf(out + o, size - o, " +%x %s owner %08x;", off, cn, rd32(v + PC1_OWNER_OFF));
    }
    snprintf(out + o, size - o, " %d words written, %d weapon owners set; current slot %u; UNDO before any death / Continue (`packcopy undo`)", words, owners, rd32(pt + 0xD4));
    r1_log("probe: %s", out);
}

/* LAB (read only), rev1 live 10: bag <slot> - the carry-over check of a section change: the human's hp (+0xE38), held (+0xE34: bitten) and the words +4..+0x140 of its bag (unit+0xFE8,
 * cBioItemPack: +4.. weapon slots, +0x40 reserve ammo, +0xD4 current slot): a word that is an object prints its class (a weapon also its clip +0xF7C), any
 * other non-zero word its value. Objects are new after a load; the classes, clips and values are what must carry over. */
/* LAB ONLY (rev1 live 11, task 3: the lab bags of a title start hold no weapon and the Genesis does not equip by LB / RB / Y, so no combat check could run).
 * The stock FSM actions' own engine calls, never the product path (console only):
 *   bag <slot> weapon <class hex> [num]  = PcsMainCharacterAddWeapon (f1 0x779EB0 -> FUN_00772500(action, param{+8 = 0x10 "the unit is given", +0xC weapon id,
 *                                          +0x10 slot -1 = the bag's free slot (FUN_005C7A70), +0x14 num}, x, unit), thiscall ret 0xC; `this` is read only when +8 != 0x10).
 *                                          Handguns: uWpHdgPPK 80051020, uWpHdgM92F 80051000, uWpHdgGovernment 80051010; the Genesis uWpScanner 80051B30.
 *   bag <slot> equip <index>             = PcsMainNpcChangeEquipSlot (f1 0x77A550 -> FUN_00775830(action, param{+8 = 0x10, +0xC slot}, x, unit) -> FUN_006AF0A0(slot)):
 *                                          the bag word +4 + 4 * index (13_012_0_1 equips the NPC partner's Genesis with index 14 = bag +0x3C).
 *   bag <slot> clip <bag offset hex> <n> = the clip (+0xF7C) of the weapon object in that bag word (a lab refill; the stock reload takes the reserve ammo). */
static const uint8_t addweapon_sig[] = {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x83, 0x7f, 0x08, 0x10, 0x8b, 0xf1, 0x75, 0x06, 0x8b, 0x74, 0x24};
typedef unsigned (__attribute__((thiscall)) *fsm4_fn)(void *self, void *param, void *x, void *unit);
static int bag_lab(int slot, unsigned u, unsigned p, const char *arg, char *out, int size)
{
    char w[16] = "";
    unsigned a = 0, b = 0;
    if (sscanf(arg, "%*d %15s %x %x", w, &a, &b) < 2) return 0;
    if (!strcmp(w, "clip")) {
        unsigned obj = (a >= 4 && a < 0x144 && !(a & 3)) ? rd32(p + a) : 0;
        char cn[96];
        int n = 0;
        sscanf(arg, "%*d %*s %*x %d", &n);
        if (!obj || !classes_object_name(obj, cn, sizeof cn) || strncmp(cn, "uWp", 3) || n < 0 || n > 999) {
            snprintf(out, size, "bag clip: REFUSED (bag +%x = %08x is not a weapon object, or n %d)", a, obj, n);
            return 1;
        }
        int old = (int)rd32(obj + 0xF7C);
        wr32(obj + 0xF7C, (unsigned)n);
        snprintf(out, size, "bag slot %d +%x %s: LAB clip %d -> %d", slot, a, cn, old, (int)rd32(obj + 0xF7C));
        r1_log("probe: %s", out);
        return 1;
    }
    if (strcmp(w, "weapon") && strcmp(w, "equip")) return 0;
    uint32_t fn = !strcmp(w, "weapon") ? 0x772500u : 0x775830u;
    uint8_t cur[sizeof addweapon_sig];
    for (unsigned i = 0; i < sizeof cur; i++) cur[i] = (uint8_t)rd8(fn + i);
    if (memcmp(cur, addweapon_sig, sizeof cur)) {   /* both functions share this prologue in the Steam build 13298487 */
        snprintf(out, size, "bag %s: REFUSED: the bytes at %08x are not the build's FSM action prologue", w, fn);
        return 1;
    }
    uint32_t param[8] __attribute__((aligned(4)));
    memset(param, 0, sizeof param);
    param[2] = 0x10;                           /* +8: "the unit is given" (the 4th argument) */
    param[3] = a;                              /* +0xC: weapon id / equip slot */
    param[4] = 0xFFFFFFFFu;                    /* +0x10: slot -1 = the bag's free slot (AddWeapon) */
    param[5] = b ? b : 1;                      /* +0x14: num */
    ((fsm4_fn)(uintptr_t)fn)(NULL, param, NULL, (void *)(uintptr_t)u);
    int o = snprintf(out, size, "bag slot %d: LAB %s %08x done (FUN_%08X); now cur %u:", slot, w, a, fn, rd32(p + 0xD4));
    for (unsigned off = 4; off < 0x144 && o + 60 < size; off += 4) {
        unsigned v = rd32(p + off);
        char cn[96];
        if (v > 0x10000 && classes_object_name(v, cn, sizeof cn) && !strncmp(cn, "uWp", 3)) o += snprintf(out + o, size - o, " +%x=%s(clip %d)", off, cn, (int)rd32(v + 0xF7C));
    }
    r1_log("probe: %s", out);
    return 1;
}

static void bag_cmd(const char *arg, char *out, int size)
{
    int slot = -1;
    sscanf(arg, "%d", &slot);
    unsigned u = slot >= 0 ? unit_of_slot(slot) : 0, p = u ? rd32(u + 0xFE8) : 0;
    if (!p) {
        snprintf(out, size, "usage: bag <slot> [weapon <class hex> [num] | equip <index> | clip <bag offset hex> <n>] (LAB) (unit %08x bag %08x)", u, p);
        return;
    }
    if (bag_lab(slot, u, p, arg, out, size)) return;
    int o = snprintf(out, size, "bag slot %d unit %08x hp %.0f held %u bag %08x cur %u:", slot, u, rdf(u + 0xE38), rd32(u + 0xE34), p, rd32(p + 0xD4));
    for (unsigned off = PC1_FIRST; off < PC1_END && o + 60 < size; off += 4) {
        unsigned v = rd32(p + off);
        char cn[96];
        if (!v) continue;
        if (v > 0x10000 && classes_object_name(v, cn, sizeof cn)) {
            if (!strncmp(cn, "uWp", 3)) o += snprintf(out + o, size - o, " +%x=%s(clip %d)", off, cn, (int)rd32(v + 0xF7C));
            else o += snprintf(out + o, size - o, " +%x=%s", off, cn);
        } else
            o += snprintf(out + o, size - o, " +%x=%x", off, v);
    }
}

/* the game's global unit list ([0x11E0184]+0x11C, next at +0x14; FUN_00882670 walks it): every live unit, weapons included. units [name filter] */
static void units_cmd(const char *flt, char *out, int size)
{
    unsigned root = rd32(0x11E0184), p = root ? rd32(root + 0x11C) : 0;
    int o = snprintf(out, size, "unit list root %08x head %08x\n", root, p), n = 0;
    for (int guard = 0; p && guard < 600 && o + 150 < size; guard++, p = rd32(p + 0x14)) {
        char cn[96] = "?";
        classes_object_name(p, cn, sizeof cn);
        if (flt[0] && !strstr(cn, flt))
            continue;
        o += snprintf(out + o, size - o, "%08x %-28s E2C=%d E30=%d pos=(%.0f,%.0f,%.0f) +10C=%08x hp=%.0f E1C=%08x\n", p, cn, (int)rd32(p + 0xE2C), (int)rd32(p + 0xE30), rdf(p + 0x40), rdf(p + 0x44),
                      rdf(p + 0x48), rd32(p + 0x10C), rdf(p + 0xE38), rd32(p + 0xE1C));
        n++;
    }
    snprintf(out + o, size - o, "%d units shown", n);
}

/* ulist [g<k> | name filter]: the game's whole unit table (sUnit [0x11E0184]: +0x620 = group count, group k = first unit at +0x2C+0x18k, next at unit+0x14). `units` only walks group 10
 * (players, NPCs); enemies are in other groups (uZombieBase = group 13), found by the stage 6c census. No argument: one line per group (units, first class); g<k>: the units of group k;
 * a name: the units of any group whose class name contains it. Unit line: group, address, class, pos, hp/max, flags (E1C), state (F08). */
static void ulist_cmd(const char *flt, char *out, int size)
{
    unsigned su = rd32(0x11E0184), ng = su ? rd32(su + 0x620) : 0, only = 0xFFFFFFFFu;
    int o = snprintf(out, size, "sUnit %08x, %u groups\n", su, ng), shown = 0, summary = !flt[0];
    if (flt[0] == 'g' && flt[1] >= '0' && flt[1] <= '9')
        only = (unsigned)atoi(flt + 1), flt = "";
    for (unsigned k = 0; k < ng && k < 40 && o + 200 < size; k++) {
        if (only != 0xFFFFFFFFu && k != only)
            continue;
        unsigned u = rd32(su + 0x2C + 0x18 * k);
        int n = 0, match = 0;
        char first[96] = "";
        for (int guard = 0; u && guard < 3000 && o + 200 < size; guard++, u = rd32(u + 0x14)) {
            char cn[96] = "?";
            n++;
            classes_object_name(u, cn, sizeof cn);
            if (!first[0])
                snprintf(first, sizeof first, "%s", cn);
            if (summary || (flt[0] && !strstr(cn, flt)))
                continue;
            match++;
            if (shown < 120) {
                o += snprintf(out + o, size - o, "g%u %08x %-26s pos=(%.0f,%.0f,%.0f) hp=%.0f/%.0f E1C=%08x F08=%d\n", k, u, cn, rdf(u + 0x40), rdf(u + 0x44), rdf(u + 0x48), rdf(u + 0xE38),
                              rdf(u + 0xE40), rd32(u + 0xE1C), (int)rd32(u + 0xF08));
                shown++;
            }
        }
        if (n && summary)
            o += snprintf(out + o, size - o, "group %u: %d units, first %s\n", k, n, first);
        else if (n && (flt[0] || only != 0xFFFFFFFFu))
            o += snprintf(out + o, size - o, "  group %u: %d units, %d shown\n", k, n, only != 0xFFFFFFFFu ? n : match);
    }
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
    unsigned mgr = rd32(root + 0xCE0);   /* a pointer to the uCameraManage */
    char mn[96] = "?";
    classes_object_name(mgr, mn, sizeof mn);
    snprintf(out, size, "camera root [0x11E00B8] = %08x (%s)\nmanager [root+0xCE0] = %08x (%s): target +0x74=%08x +0x78=%08x camera index(+0xCC)=%d cams(+0xE4)=%08x fov(+0x3C)=%.3f", root, cn, mgr, mn, rd32(mgr + 0x74),
             rd32(mgr + 0x78), (int)rd32(mgr + 0xCC), rd32(mgr + 0xE4), rdf(mgr + 0x3C));
}

/* ---- hot-loadable plug-in (console "plug load <dll>" / "plug unload" / "plug status"; commands for it: "P <name> <args>") ---- */
static HMODULE plug_mod;
static r1_plug_t plug;
static char plug_path[260];

static const r1_api_t api = {3, r1_log, r1_safe_read, testmode_pad_peek, classes_object_name, d3d_frames, testmode_pad_set, testmode_pad_hold, d3d_request_shot, capture_busy};

static void plug_unload(char *out, int size)
{
    if (!plug_mod) {
        snprintf(out, size, "no plug-in loaded");
        return;
    }
    if (plug.on_exit)
        plug.on_exit();
    memset(&plug, 0, sizeof plug);
    FreeLibrary(plug_mod);
    plug_mod = NULL;
    snprintf(out, size, "plug-in %s unloaded", plug_path);
}

static void plug_load(const char *path, char *out, int size)
{
    if (plug_mod)
        plug_unload(out, size);
    HMODULE m = LoadLibraryA(path);
    if (!m) {
        snprintf(out, size, "LoadLibrary failed (%lu): %s", GetLastError(), path);
        return;
    }
    int (*init)(const r1_api_t *, r1_plug_t *) = (int (*)(const r1_api_t *, r1_plug_t *))GetProcAddress(m, "plug_init");
    if (!init) {
        FreeLibrary(m);
        snprintf(out, size, "no plug_init export in %s", path);
        return;
    }
    memset(&plug, 0, sizeof plug);
    int rc = init(&api, &plug);
    if (rc) {
        memset(&plug, 0, sizeof plug);
        FreeLibrary(m);
        snprintf(out, size, "plug_init returned %d", rc);
        return;
    }
    plug_mod = m;
    snprintf(plug_path, sizeof plug_path, "%s", path);
    r1_log("plug: %s loaded", path);
    snprintf(out, size, "plug-in %s loaded", path);
}

/* ---- black box: one line per interval in the log (REV1_PROBELOG=<seconds> or console probelog) ---- */
static volatile unsigned probelog_ms;
static DWORD probelog_last;
static unsigned last_mode = 0xFFFFFFFE;

static void probe_frame(void)
{
    net1_frame();   /* the co-op link (lobby or console): session notifications, ready flags, the automatic start */
    unsigned holder = rd32(R1_GAMESYS_PTR), data = holder ? rd32(holder + 0x24) : 0, mode = data ? rd32(data + 0x20) : 0xFFFFFFFF;
    if (mode != last_mode) {
        r1_log("probe: game mode [[0x1169108]+0x24]+0x20 changed %d -> %d (pass %u)", (int)last_mode, (int)mode, lt_frames());
        last_mode = mode;
    }
    if (plug.on_frame)
        plug.on_frame();
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
    r1_log("bb: sGamePad cur=%d en=%u", (int)rd32(rd32(R1_GAMEPAD_PTR) + 0x870), rd8(rd32(R1_GAMEPAD_PTR) + 0x882));
}

static void net_log_sink(const char *line) { r1_log("%s", line); }

static void probe_frame(void);
void probe_install(void)
{
    net1_cfg nc;
    memset(&nc, 0, sizeof nc);
    nc.transport = "enet", nc.threaded = 1, nc.exe_sha = r1_exe_ok() ? r1_exe_sha() : NULL, nc.log = net_log_sink;   /* the exe tag is the SHA-256 of the supported build: a host refuses a join from another build */
    net1_configure(&nc);
    lt_add_frame_cb(probe_frame);
    capture_init();
    split_init();
    downed1_init();   /* the downed / revive rule: off unless REV1_DOWNED=1 */
    menu_install();
    puppet_install();
    warp_install();
    guards_init();
    mapptn_init();   /* the co-op partner on the HUD minimap (mapptn1_game.c): patched only while a split screen or a network session runs */
    rolebarrier_init();
    coopsave_install();   /* rev1 live 15, task 5: the co-op game's own save (coopsave1.h) */
    give1_init();   /* give1: the item screen's "give to partner" (a co-op session only; REV1_GIVE=0 = off) */
    scan1_init();   /* scan1: a Genesis for the second player, his split-screen scans, hidden items on both PCs (REV1_SCAN=0 = off) */
    rolebarrier_on_restart(packcopy_before_reload);   /* LAB packcopy: undone before any reload through the restart hook */
    world_init();
    ach1_game_init();   /* B10: only this PC's human moves its achievement counters in a network game */
    lt_add_frame_cb(ach1_game_frame);
    r1_log("probe: ready (no inline hook until the capture module is asked for a role; experiments are a hot-loadable plug-in, see plug_cap.c)");
}

/* ---- commands ---- */
int probe_is_game_cmd(const char *n)
{
    static const char *const list[] = {"players", "unit", "mode", "gamepad", "camera", "units", "ulist", "plug", "P", "ctl", "tp", "call", "w32", "w8", "wf", "rf", "probelog", "ltinfo", "pokesw", "cap", "split", "menu", "warp", "net", "guards", "mapptn", "barrier", "world", "packcopy", "bag", "r32", "downed", "ach", "coopsave", "scan", "give", NULL};
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
    static int env_done;
    if (!env_done) {
        env_done = 1;
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
    } else if (!strcmp(name, "units")) {
        units_cmd(arg, out, size);
    } else if (!strcmp(name, "ulist")) {
        ulist_cmd(arg, out, size);
    } else if (!strcmp(name, "mode")) {
        mode_cmd(out, size);
    } else if (!strcmp(name, "gamepad")) {
        gamepad_cmd(out, size);
    } else if (!strcmp(name, "camera")) {
        camera_cmd(out, size);
    } else if (!strcmp(name, "cap")) {   /* the capture module (capture.c): cap status | bind | unbind | own | src | rec | trk | timeout | raid */
        capture_cmd(arg, out, size);
    } else if (!strcmp(name, "split")) {   /* the split screen module (split.c): split status | on [v|h] | off | lay | hud | menu | probe */
        split_cmd(arg, out, size);
    } else if (!strcmp(name, "menu")) {   /* the co-op entry / lobby (menu.c): menu status | open | close | ev <...> | set <key>=<value> | lang <code> | view */
        menu_cmd(arg, out, size);
    } else if (!strcmp(name, "packcopy")) {   /* LAB ONLY: copy one human's bag into another's (see packcopy_cmd) */
        packcopy_cmd(arg, out, size);
    } else if (!strcmp(name, "bag")) {   /* LAB: bag <slot> (the carry-over of a section change); weapon / equip / clip (rev1 live 11) */
        bag_cmd(arg, out, size);
    } else if (!strcmp(name, "world")) {   /* the engine hooks of the shared world (world1_game.c): world status | apply | revert | auto | list | flags | set */
        world_cmd(arg, out, size);
    } else if (!strcmp(name, "ach")) {   /* the achievement gate (ach1_game.c): ach [status] | gate 0|1 | taint 0|1 | profile */
        ach1_game_cmd(arg, out, size);
    } else if (!strcmp(name, "downed")) {   /* the downed / revive rule (downed1.c): downed [status] | down <k> | pose <k> down|keep|up|die */
        downed_cmd(arg, out, size);
    } else if (!strcmp(name, "guards")) {   /* the network guards (guards1_game.c): guards status | apply | revert | auto */
        guards_cmd(arg, out, size);
    } else if (!strcmp(name, "mapptn")) {   /* the co-op partner on the HUD minimap (mapptn1_game.c): mapptn status | apply | revert | auto | log 0|1 */
        mapptn_cmd(arg, out, size);
    } else if (!strcmp(name, "give")) {   /* give1: give [status] | on | off | gui | bag [slot] | lab | fill | set reach | sound */
        give_cmd(arg, out, size);
    } else if (!strcmp(name, "scan")) {   /* scan1: scan [status] | who | items | on | off | x 0|1 | give | p2rate | reveal */
        scan_cmd(arg, out, size);
    } else if (!strcmp(name, "coopsave")) {   /* rev1 live 15: the co-op save (coopsave1_game.c): coopsave status | delete | force on|off|auto */
        coopsave_cmd(arg, out, size);
    } else if (!strcmp(name, "barrier")) {   /* role / load + cutscene barrier / Retry-Continue triple (rolebarrier1_game.c): barrier status | apply | revert | auto | restart | set */
        rolebarrier_cmd(arg, out, size);
    } else if (!strcmp(name, "net")) {   /* the co-op link (net1.c): net host [port] | join <ip[:port]> | status | stop | ready | role | set | start | auto | load | done */
        if (!strncmp(arg, "pad", 3) && (arg[3] == 0 || arg[3] == ' ')) puppet_cmd(arg[3] ? arg + 4 : "", out, size);   /* net pad ...: the puppet of the other PC's human (puppet_game.c) */
        else net1_cmd(arg, out, (size_t)size);
    } else if (!strcmp(name, "warp")) {   /* developer tool (warp.c): warp <hex scene id> | off | status */
        warp_cmd(arg, out, size);
    } else if (!strcmp(name, "plug")) {   /* plug load <dll path> | plug unload | plug status */
        if (!strncmp(arg, "load ", 5))
            plug_load(arg + 5, out, size);
        else if (!strcmp(arg, "unload"))
            plug_unload(out, size);
        else
            snprintf(out, size, "plug-in: %s", plug_mod ? plug_path : "none loaded (plug load <dll>)");
    } else if (!strcmp(name, "P")) {   /* P <name> <args>: a command of the plug-in */
        char pn[32] = "", pa[300] = "";
        sscanf(arg, "%31s %299[^\r\n]", pn, pa);
        if (!plug.on_cmd || !plug.on_cmd(pn, pa, out, size))
            snprintf(out, size, plug.on_cmd ? "plug-in: unknown command %s" : "no plug-in loaded", pn);
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
    } else if (!strcmp(name, "tp")) {   /* tp <jill|parker|hexaddr> <x> <y> <z>: the story's own "set character position" (op PcsMainCharacterSetPos, 0x771AF0) in ONE frame: the move object's
                                           * "teleport" byte [[unit+0xE88]+0x2B4] = 1, the position +0x40/+0x44/+0x48 (+0x4C = 0), then the unit's state refresh 0x6BEF50(unit, [unit+0xF08], 1) for players.
                                           * Plain writes of the position are swept against the collision and stop at the first wall (stage 6c live), the byte must be set in the same frame. */
        char w[40] = "", xs[24] = "", ys[24] = "", zs[24] = "";
        sscanf(arg, "%39s %23s %23s %23s", w, xs, ys, zs);
        unsigned unit = find_unit(w), mo = unit ? rd32(unit + 0xE88) : 0;
        if (!unit || !mo || !zs[0]) {
            snprintf(out, size, "usage: tp <jill|parker|hexaddr> <x> <y> <z>");
        } else {
            wr8(mo + 0x2B4, 1);
            wrf(unit + 0x40, arg_float(xs));
            wrf(unit + 0x44, arg_float(ys));
            wrf(unit + 0x48, arg_float(zs));
            wr32(unit + 0x4C, 0);
            unsigned a[3] = {unit, rd32(unit + 0xF08), 1};
            do_call(2, 0x6BEF50, a, 3);
            snprintf(out, size, "unit %08x set to (%s, %s, %s) with the teleport byte, now (%.1f, %.1f, %.1f)", unit, xs, ys, zs, rdf(unit + 0x40), rdf(unit + 0x44), rdf(unit + 0x48));
        }
    } else if (!strcmp(name, "probelog")) {
        int sec = atoi(arg);
        probelog_ms = sec > 0 ? (unsigned)sec * 1000 : 0;
        snprintf(out, size, "black box every %d s in the log (0 = off)", sec);
    } else if (!strcmp(name, "ltinfo")) {
        snprintf(out, size, "logic hook installed, passes %u, thread %lu", lt_frames(), GetCurrentThreadId());
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
    } else if (!strcmp(name, "r32")) {   /* LAB, read only (rev1 live 10): r32 <hex addr> [n words, <= 64] */
        char as[20];
        int n = 4;
        sscanf(arg, "%19s %d", as, &n);
        int o = 0;
        for (int i = 0; i < n && i < 64 && o + 12 < size; i++)
            o += snprintf(out + o, size - o, "%08x ", rd32(hx(as) + 4 * (unsigned)i));
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
