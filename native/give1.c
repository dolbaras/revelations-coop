/*
 * give1.c - the engine glue of the RE5-style "give to partner" (Revelations 1, 0.2.0). The rule, the wire and the texts: give1.h / give1_text.h; design, static RE
 * and live results: notes/give1.md.
 *
 * Engine side (build 13298487, every stock call byte-checked at the first pass; one mismatch keeps the module off with a logged reason):
 *   - the item screen (BACK) = uGUI_SubMenu (vtable 0x1082698): its update, slot 8 = 0x7BCD30, runs inline on the logic thread. Slot 8 is replaced by a wrapper that
 *     calls the stock update first, then (a co-op session only) puts "<B_DE> Give to partner  <B_CA> Close" into the screen's memo bar on a herb / grenade / ammo
 *     cell while the partner may take it (FUN_00848E50, the stock "Read  Close" call), and notes a decide press on such a cell (a plain item's decide does nothing
 *     in the stock code). The give itself runs in the per-pass callback (lt.c, same thread, outside the unit loop); the next screen update rebuilds the list
 *     (FUN_007BC120) and the memo (FUN_007BB710).
 *   - counts only: herbs bag+0xC8, ammo vf11 / vf13, grenades vf18 / FUN_005C7B10 / FUN_005C8230; a grenade type gets its weapon archive loaded first on the PC
 *     whose human receives it (the stock sResource load, as bag1 does for guns).
 *   - split screen (menu_split_armed): both humans are local: the pass moves the units from one bag to the other. Network (net1_session_active): this PC writes only
 *     its own human's bag; GIVE / ACK / CANCEL / CANCELACK travel as reliable session events of kind GV_EV_KIND (0x144).
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "r1.h"
#include "give1.h"
#include "give1_text.h"
#include "give1_game.h"
#include "lobby1.h"
#include "net1.h"
#include "lobby_link.h"
#include "coop_session.h"
#include "coop_proto.h"
#include "downed1.h"
#include "rolebarrier1.h"   /* rolebarrier_shielded: a restart came, the next level instance has not opened */

int puppet_partner_flags(unsigned *flags);   /* puppet_game.c: the partner's newest pad / STATE flags (CP_SF_*), 1 = fresh */
int puppet_scene_now(unsigned *flags);       /* puppet_game.c: this PC's own scene flags of the last pass (CP_SF_LOADING / CUTSCENE / PAUSED), 1 = fresh */

#define TC __attribute__((thiscall))

/* ---------------------------------------------------------------- addresses (build 13298487) */
#define GVA_SUBMENU_VT     0x1082698u   /* uGUI_SubMenu vtable */
#define GVA_SUBMENU_SLOT8  (GVA_SUBMENU_VT + 8u * 4u)
#define GVA_SUBMENU_UPDATE 0x7BCD30u   /* its slot 8: the screen's update, thiscall(gui) */
#define GVA_LIST           0x7BC120u   /* thiscall(gui): the cells of the tab from the local player's bag */
#define GVA_MEMO           0x7BB710u   /* thiscall(gui): the stock memo of the cell under the cursor */
#define GVA_MEMO2          0x848E50u   /* thiscall(gui; text object, s1, s2) ret 0xC: "%s  %s" */
#define GVA_BANKSTR        0xA10F10u   /* thiscall(bank; id) ret 4: a message of a bank */
#define GVA_SOUND          0x8C0100u   /* thiscall([0x116910C]; index, gui) ret 8 */
#define GVA_HOVER          0x848CA0u   /* thiscall(gui; cell object, -1) ret 8: the mouse is over the cell (jmp 0x848AD0) */
#define GVA_LOCAL          0x40EEF0u   /* thiscall(sPlayer): the unit of the own slot (the screen shows its bag) */
#define GVA_HERB_ADD       0x5C4300u   /* thiscall(bag; n) ret 4 */
#define GVA_GREN_ADD       0x5C7B10u   /* thiscall(bag; class, n) ret 8 */
#define GVA_GREN_REFRESH   0x5C8230u   /* thiscall(bag; force) ret 4 */
#define GVA_AMMO_GOT       0x84D9C0u   /* thiscall(item log; class) ret 4: the ammo type is shown */
#define GVA_AMMO_MARK      0x84D890u   /* thiscall(item log; class, 1) ret 8 */
#define GVA_RES_EXISTS     0x9B12C0u   /* thiscall(sResource; DTI, path) ret 8 -> al */
#define GVA_SECTION        0x720320u   /* thiscall([0x116A340]) ret 0: the running section 0x91EESS00 */
#define GVA_EPISODE_PTR    0x116A340u
#define GVA_BAG_VT         0x10186B0u  /* cBioItemPack */
#define GVA_BAG_MAX        0x5C5B90u   /* vf10 thiscall(bag; class) ret 4 */
#define GVA_BAG_AMMO_SET   0x5C2CE0u   /* vf11 thiscall(bag; index, n) ret 8 */
#define GVA_BAG_AMMO_ADD   0x5C2D20u   /* vf13 thiscall(bag; class, n) ret 8 (capped) */
#define GVA_BAG_GREN_SET   0x5C2E70u   /* vf18 thiscall(bag; class, n) */
#define GVA_SOUND_PTR      0x116910Cu
#define GVA_ITEMLOG_PTR    0x116A7D8u
#define GVA_MSGROOT_PTR    0x1182CA4u   /* +0x1A4 loaded, +0x108 + 4k bank k (3 = cockpit) */
#define GVA_SMAIN_PTR      0x11E0314u   /* +0x254C mLanguage */
#define GVA_SRES_PTR       0x11E00FCu
#define GVA_DTI_ARCHIVE    0x11E0274u
#define GV_U_HP 0xE38u
#define GV_U_SLOT 0xE2Cu
#define GV_U_POS 0x40u
#define GV_U_BAG 0xFE8u

static const struct { uint32_t va; uint8_t b[12]; const char *what; } SIG[] = {
    {GVA_SUBMENU_UPDATE, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf0, 0x83, 0xec, 0x54, 0x53, 0x56, 0x57}, "uGUI_SubMenu update"},
    {GVA_LIST, {0xa1, 0x94, 0x93, 0x16, 0x01, 0x83, 0xec, 0x20, 0x56, 0x57, 0x8b, 0xf1}, "item list builder"},
    {GVA_MEMO, {0x83, 0xec, 0x08, 0x56, 0x8b, 0xf1, 0x83, 0xbe, 0x98, 0x02, 0x00, 0x00}, "item memo"},
    {GVA_MEMO2, {0x56, 0x8b, 0x74, 0x24, 0x08, 0x8b, 0x06, 0x8b, 0x50, 0x10, 0x57, 0x8b}, "memo print"},
    {GVA_BANKSTR, {0x8b, 0x44, 0x24, 0x04, 0x39, 0x81, 0x88, 0x00, 0x00, 0x00, 0x77, 0x08}, "bank string"},
    {GVA_SOUND, {0x56, 0x57, 0x8b, 0x3d, 0x34, 0x0a, 0x1e, 0x01, 0x8b, 0xf1, 0x85, 0xff}, "GUI sound"},
    {GVA_HOVER, {0xe9, 0x2b, 0xfe, 0xff, 0xff, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc}, "cell hover"},
    {GVA_LOCAL, {0x53, 0x55, 0x8b, 0xe9, 0x8b, 0x5d, 0x30, 0x56, 0x57, 0x3b, 0x5d, 0x34}, "local player"},
    {GVA_HERB_ADD, {0x56, 0x8b, 0xf1, 0x8b, 0x06, 0x8b, 0x50, 0x28, 0x68, 0x01, 0x01, 0x04}, "herb add"},
    {GVA_GREN_ADD, {0x56, 0x8b, 0xb1, 0xd0, 0x00, 0x00, 0x00, 0x85, 0xf6, 0x0f, 0x84, 0xf7}, "grenade add"},
    {GVA_GREN_REFRESH, {0x83, 0xec, 0x10, 0x56, 0x8b, 0xf1, 0x8b, 0x4e, 0x14, 0x57, 0x85, 0xc9}, "grenade slot refresh"},
    {GVA_AMMO_GOT, {0x56, 0x8b, 0xf1, 0x85, 0xf6, 0x74, 0x05, 0xe8, 0xf4, 0x7f, 0x18, 0x00}, "ammo type shown"},
    {GVA_AMMO_MARK, {0x56, 0x8b, 0xf1, 0x85, 0xf6, 0x74, 0x05, 0xe8, 0x24, 0x81, 0x18, 0x00}, "ammo type mark"},
    {GVA_RES_EXISTS, {0x81, 0xec, 0x04, 0x01, 0x00, 0x00, 0x53, 0x8b, 0x9c, 0x24, 0x0c, 0x01}, "archive exists"},
    {GVA_SECTION, {0x53, 0x8b, 0xd9, 0x56, 0x85, 0xdb, 0x74, 0x05, 0xe8, 0x93, 0x56, 0x2b}, "running section"},
    {GVA_BAG_MAX, {0x83, 0xec, 0x0c, 0x0f, 0x57, 0xc0, 0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x8e}, "bag max"},
    {GVA_BAG_AMMO_SET, {0x8b, 0x44, 0x24, 0x04, 0x83, 0xf8, 0x0c, 0x73, 0x08, 0x8b, 0x54, 0x24}, "bag ammo set"},
    {GVA_BAG_AMMO_ADD, {0x56, 0x8b, 0xf1, 0x8b, 0x4c, 0x24, 0x08, 0x8d, 0x81, 0x00, 0xfe, 0xfb}, "bag ammo add"},
    {GVA_BAG_GREN_SET, {0x8b, 0x54, 0x24, 0x04, 0x81, 0xc2, 0x00, 0xe7, 0xfa, 0x7f, 0xb8, 0x05}, "bag grenade set"},
};
/* the bag's vtable slots the rule uses (cBioItemPack 0x10186B0): 10 max, 11 ammo set, 13 ammo add, 18 grenade set */
static const struct { unsigned slot; uint32_t fn; } BAGVT[] = {{10, GVA_BAG_MAX}, {11, GVA_BAG_AMMO_SET}, {13, GVA_BAG_AMMO_ADD}, {18, GVA_BAG_GREN_SET}};

typedef void (TC *gui_fn)(void *gui);
typedef void (TC *memo2_fn)(void *gui, void *text, const char *s1, const char *s2);
typedef const char *(TC *bankstr_fn)(void *bank, unsigned id);
typedef void (TC *sound_fn)(void *snd, unsigned index, void *gui);
typedef unsigned (TC *hover_fn)(void *gui, void *cell, int k);
typedef uint32_t (TC *local_fn)(void *splayer, void *unused);   /* ret 4 */
typedef void (TC *bag1_fn)(void *bag, unsigned a);
typedef void (TC *bag2_fn)(void *bag, unsigned a, unsigned b);
typedef unsigned (TC *bagmax_fn)(void *bag, unsigned cls);
typedef unsigned (TC *log1_fn)(void *log, unsigned cls);
typedef void (TC *log2_fn)(void *log, unsigned cls, unsigned v);
typedef unsigned (TC *exists_fn)(void *sres, void *dti, const char *path);
typedef uint32_t (TC *load_fn)(void *sres, void *dti, const char *path, int flags);
typedef uint32_t (TC *sect_fn)(void *episode);

/* ---------------------------------------------------------------- memory */
static uint32_t rd32(uint32_t a)
{
    uint32_t v = 0;
    if (a < 0x10000u || !r1_safe_read((const void *)(uintptr_t)a, &v, 4)) return 0;
    return v;
}
static unsigned rd8(uint32_t a)
{
    uint8_t v = 0;
    if (a < 0x10000u || !r1_safe_read((const void *)(uintptr_t)a, &v, 1)) return 0;
    return v;
}
static float rdf(uint32_t a)
{
    float v = 0;
    if (a < 0x10000u || !r1_safe_read((const void *)(uintptr_t)a, &v, 4)) return 0;
    return v;
}
static int wr32(uint32_t a, uint32_t v)
{
    SIZE_T got = 0;
    return a >= 0x10000u && WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, &v, 4, &got) && got == 4;
}
static int wr_code32(uint32_t a, uint32_t v)   /* a vtable slot in .rdata */
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, 4, PAGE_READWRITE, &old)) return 0;
    *(volatile uint32_t *)(uintptr_t)a = v;
    VirtualProtect((void *)(uintptr_t)a, 4, old, &old);
    return 1;
}

static void glog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void glog(const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    r1_log("give: %s", b);
}

/* ---------------------------------------------------------------- state */
static int g_inited, g_checked, g_ok, g_on = 1, g_hooked;
static char g_why_off[160];
static gui_fn orig_update;
static float g_reach = GV_REACH;
static unsigned g_line_ms = 2500;      /* how long a refusal line stays in the memo bar (LAB `give set line <ms>` for a screenshot) */
static volatile int g_lab_hold;         /* LAB `give set hold 1`: incoming GIVEs wait (a section change can be put between a give and its arrival) */
static unsigned g_snd_ok = 0x26, g_snd_err = 2;
static CRITICAL_SECTION g_qcs;
#define GQ 32
static struct { uint8_t b[GV_MSG_LEN]; uint8_t len; uint32_t t0; } g_q[GQ];   /* t0: the first pass that had to keep it waiting (0 = never) */
static int g_qh, g_qt;
static unsigned g_qdrop;
static lk_t *g_reg_link;
static gv_giver G;
static gv_recv R;
static int g_net_me = -1;
static struct { int want; uint32_t gui, giver, partner; int giver_slot, tab, id, mode; } REQ;
static uint32_t g_refresh_gui;
static int g_line_text = -1;
static uint32_t g_line_until;
static struct { uint32_t gui, seen; int tab, pos, id, shown, lang, dev, f290; } M;
static int g_last_why = -1;
static struct { unsigned decides, split_gives, split_full, net_sent, net_send_fail, refunds, refund_units, recv_applied, recv_refused, answers_tx, memo_sets, memo_stock,
                arch_loads, arch_fail, verify_fail, rx, rx_bad, lines; } S;
#define GV_ARCH_MAX 4
static uint32_t g_arch_cls[GV_ARCH_MAX], g_arch_res[GV_ARCH_MAX];

/* ---------------------------------------------------------------- engine reads */
static uint32_t unit_class(uint32_t u)   /* the class code of a unit: [[vtable+0x10] = mov eax, DTI; ret] -> DTI +0x1C */
{
    uint32_t vt = rd32(u), f = vt ? rd32(vt + 0x10) : 0;
    if (!f || rd8(f) != 0xB8 || rd8(f + 5) != 0xC3) return 0;
    uint32_t dti = rd32(f + 1);
    return dti ? rd32(dti + 0x1C) : 0;
}
static uint32_t unit_of_slot(unsigned slot)   /* sPlayer list 0 (FUN_0040EEF0's walk): +0xE2C == slot, (+0xC & 7) in {1, 2} */
{
    uint32_t sp = rd32(R1_SPLAYER_PTR);
    if (!sp) return 0;
    uint32_t node = rd32(sp + 0x30), end = rd32(sp + 0x34);
    for (int guard = 0; node && node != end && guard < 64; guard++) {
        uint32_t u = rd32(node + 0x10), k = u ? (rd32(u + 0xC) & 7u) : 0;
        if (u && rd32(u + GV_U_SLOT) == slot && (k == 1 || k == 2)) return u;
        node = rd32(node + 8);
    }
    return 0;
}
static uint32_t local_player(void)
{
    /* FUN_0040EEF0 takes ONE stack word (its callers pass a byte pointer; it never reads it) and returns with `ret 4` (0x40EF53). g1n1 / g1n2 live: called
     * without it, the callee popped 4 bytes of our frame: esp off by 4, the next SSE store `movaps [esp+0x100]` faulted (#GP) and the host died at BACK. */
    uint32_t sp = rd32(R1_SPLAYER_PTR);
    uint8_t unused = 0;
    return sp ? ((local_fn)(uintptr_t)GVA_LOCAL)((void *)(uintptr_t)sp, &unused) : 0;
}
static uint32_t bag_of(uint32_t u)
{
    uint32_t b = u ? rd32(u + GV_U_BAG) : 0;
    return b && rd32(b) == GVA_BAG_VT ? b : 0;
}
static int bag_max(uint32_t b, uint32_t cls) { return (int)((bagmax_fn)(uintptr_t)GVA_BAG_MAX)((void *)(uintptr_t)b, cls); }
static void bag_read(uint32_t b, gv_bag *o)
{
    memset(o, 0, sizeof *o);
    if (!b) return;
    o->herb = (int)rd32(b + 0xC8), o->herb_max = bag_max(b, 0x80040101u);
    for (int i = 0; i < GV_NITEMS; i++) {
        const gv_item *it = &GV_ITEMS[i];
        if (it->kind == GV_K_AMMO) o->ammo[it->index] = (int)rd32(b + 0x40 + 4u * (unsigned)it->index), o->ammo_max[it->index] = bag_max(b, it->cls);
        if (it->kind == GV_K_GRENADE) o->gren[it->index] = (int)rd32(b + 0xA0 + 4u * (unsigned)it->index), o->gren_max[it->index] = bag_max(b, it->cls);
    }
}
static int bag_count(uint32_t b, const gv_item *it)
{
    if (!b || !it) return 0;
    return (int)rd32(it->kind == GV_K_HERB ? b + 0xC8 : it->kind == GV_K_AMMO ? b + 0x40 + 4u * (unsigned)it->index : b + 0xA0 + 4u * (unsigned)it->index);
}
static void bag_line(uint32_t b, char *out, int size)
{
    gv_bag g;
    bag_read(b, &g);
    snprintf(out, (size_t)size, "herb %d/%d | ammo hg %d/%d sg %d/%d mg %d/%d rf %d/%d mag %d/%d | grenade hand %d/%d shock %d/%d decoy %d/%d pulse %d/%d | slot4 %08x", g.herb,
             g.herb_max, g.ammo[0], g.ammo_max[0], g.ammo[1], g.ammo_max[1], g.ammo[2], g.ammo_max[2], g.ammo[3], g.ammo_max[3], g.ammo[4], g.ammo_max[4], g.gren[0],
             g.gren_max[0], g.gren[1], g.gren_max[1], g.gren[2], g.gren_max[2], g.gren[3], g.gren_max[3], b ? unit_class(rd32(b + 0x14)) : 0u);
}
static int game_lang(void)
{
    uint32_t sm = rd32(GVA_SMAIN_PTR);
    int l = sm ? l1_lang_from_game(rd32(sm + 0x254C)) : -1;
    return l >= 0 && l < GV_LANGS ? l : 0;
}
static int unit_up(uint32_t u) { return u && rdf(u + GV_U_HP) > 1.0f; }   /* a downed human sits at hp 1 (downed1.c); 0 = dead */

/* ---------------------------------------------------------------- the stock bag operations (counts only) */
static int arch_ensure(uint32_t cls)   /* a grenade type's weapon archive on THIS PC (bag1_game.inc bg_load_archive, the stock check first) */
{
    for (int i = 0; i < GV_ARCH_MAX; i++)
        if (g_arch_cls[i] == cls) return 1;
    char path[64];
    snprintf(path, sizeof path, "archive\\game\\chara\\wp\\wp%04x\\wp%04x", (unsigned)(cls & 0xFFFFu), (unsigned)(cls & 0xFFFFu));
    uint32_t sres = rd32(GVA_SRES_PTR), vt = sres ? rd32(sres) : 0, fn = vt ? rd32(vt + 0x30) : 0;
    if (!sres || !fn) {
        S.arch_fail++;
        return 0;
    }
    if (!(((exists_fn)(uintptr_t)GVA_RES_EXISTS)((void *)(uintptr_t)sres, (void *)(uintptr_t)GVA_DTI_ARCHIVE, path) & 0xFFu)) {
        S.arch_fail++;
        glog("grenade %08x: the archive %s is not there (stock check): not given", (unsigned)cls, path);
        return 0;
    }
    uint32_t res = 1;   /* fix021 (0.2.1): no own load: the stock grenade add (FUN_005C7B10 -> FUN_005C7060 -> FUN_005D0C60 -> FUN_005CE180) loads the archive
                        * itself, refcounted per item and released at the level end; r48 showed that references the mod holds for the session are not safe */
    for (int i = 0; i < GV_ARCH_MAX; i++)
        if (!g_arch_cls[i]) {
            g_arch_cls[i] = cls, g_arch_res[i] = res;
            break;
        }
    S.arch_loads++;
    glog("grenade %08x: archive %s is there (the stock check): the stock add loads it", (unsigned)cls, path);
    return 1;
}
/* add n of an item to a bag with the stock adds. Returns the units really added (counts before / after). */
static int bag_add(uint32_t b, const gv_item *it, int n)
{
    if (!b || !it || n <= 0) return 0;
    gv_bag before, after;
    bag_read(b, &before);
    if (it->kind == GV_K_HERB) ((bag1_fn)(uintptr_t)GVA_HERB_ADD)((void *)(uintptr_t)b, (unsigned)n);
    else if (it->kind == GV_K_AMMO) {
        ((bag2_fn)(uintptr_t)GVA_BAG_AMMO_ADD)((void *)(uintptr_t)b, it->cls, (unsigned)n);
        uint32_t log = rd32(GVA_ITEMLOG_PTR);
        if (log && !(((log1_fn)(uintptr_t)GVA_AMMO_GOT)((void *)(uintptr_t)log, it->cls) & 0xFFu)) {   /* the Ammunition tab shows the type (as a weapon of that family would) */
            ((log2_fn)(uintptr_t)GVA_AMMO_MARK)((void *)(uintptr_t)log, it->cls, 1);
            glog("ammo type %08x marked as found (it was not on this PC): the Ammunition tab shows it", (unsigned)it->cls);
        }
    } else if (it->kind == GV_K_GRENADE) {
        if (!arch_ensure(it->cls)) return 0;
        ((bag2_fn)(uintptr_t)GVA_GREN_ADD)((void *)(uintptr_t)b, it->cls, (unsigned)n);
    }
    bag_read(b, &after);
    return gv_have(&after, it) - gv_have(&before, it);
}
/* take n away (the giver). Returns the units really removed. */
static int bag_remove(uint32_t b, const gv_item *it, int n)
{
    if (!b || !it || n <= 0) return 0;
    gv_bag before, after;
    bag_read(b, &before);
    int have = gv_have(&before, it), nv = have - n;
    if (nv < 0) return 0;
    if (it->kind == GV_K_HERB) wr32(b + 0xC8, (uint32_t)nv);
    else if (it->kind == GV_K_AMMO) ((bag2_fn)(uintptr_t)GVA_BAG_AMMO_SET)((void *)(uintptr_t)b, (unsigned)it->index, (unsigned)nv);
    else if (it->kind == GV_K_GRENADE) {
        ((bag2_fn)(uintptr_t)GVA_BAG_GREN_SET)((void *)(uintptr_t)b, it->cls, (unsigned)nv);
        uint32_t s4 = rd32(b + 0x14);
        if (s4 && unit_class(s4) == it->cls) wr32(s4 + 0xF7C, (uint32_t)nv);   /* the held type: its unit's clip is the count (FUN_005C7B10 keeps them equal) */
        ((bag1_fn)(uintptr_t)GVA_GREN_REFRESH)((void *)(uintptr_t)b, 0);   /* the stock refresh: next type / an emptied unit released the stock way */
    }
    bag_read(b, &after);
    return gv_have(&before, it) - gv_have(&after, it);
}

/* ---------------------------------------------------------------- session */
static int coop_mode(cs_t **out_cs, int *me)
{
    if (out_cs) *out_cs = NULL;
    if (me) *me = -1;
    if (net1_session_active()) {
        lk_t *k = net1_link_peek();
        cs_t *cs = k ? lk_session(k) : NULL;
        lk_info in;
        memset(&in, 0, sizeof in);
        in.role = -1;
        if (k) lk_get_info(k, &in);
        if (!cs || cs_phase(cs) != CP_PHASE_PLAYING || in.role < 0 || in.role > 1) return GV_MODE_NONE;
        if (out_cs) *out_cs = cs;
        if (me) *me = in.role;
        return GV_MODE_NET;
    }
    return menu_split_armed() ? GV_MODE_SPLIT : GV_MODE_NONE;
}
/* the running section 0x91EESS00 (FUN_00720320 thiscall([0x116A340]) ret 0: the running area, an aCut area mapped to its section); 0 = no level object */
static uint32_t current_section(void)
{
    uint32_t ep = rd32(GVA_EPISODE_PTR);
    return ep ? ((sect_fn)(uintptr_t)GVA_SECTION)((void *)(uintptr_t)ep) : 0;
}
/* a level change is under way on this PC: the barrier's shield after a restart, or this PC's own scene loading (puppet_game.c flags of the last pass) */
static int own_transit(void)
{
    unsigned f = 0;
    return rolebarrier_shielded() || (puppet_scene_now(&f) && (f & CP_SF_LOADING));
}
static int partner_up_net(void)
{
    unsigned f = 0;
    if (!puppet_partner_flags(&f)) return 0;   /* no fresh flags: the partner's state is not known */
    return !(f & (CP_SF_DOWN | CP_SF_DEAD));
}
static void send_msg(cs_t *cs, const gv_msg *m)
{
    uint8_t b[GV_MSG_LEN];
    unsigned n = gv_enc(b, m);
    if (cs && cs_send_event(cs, GV_EV_KIND, b, n)) S.answers_tx += m->type == GV_M_ACK || m->type == GV_M_CANCELACK;
    else glog("message %u (id %08x) NOT sent: no session", m->type, (unsigned)m->id);
}
static void on_event(void *user, const cp_event *e)   /* the session's thread: only queued */
{
    (void)user;
    if (!e || e->kind != GV_EV_KIND || e->len != GV_MSG_LEN) return;
    EnterCriticalSection(&g_qcs);
    int next = (g_qh + 1) % GQ;
    if (next != g_qt) {
        memcpy(g_q[g_qh].b, e->body, GV_MSG_LEN), g_q[g_qh].len = GV_MSG_LEN, g_q[g_qh].t0 = 0;
        g_qh = next;
    } else g_qdrop++;
    LeaveCriticalSection(&g_qcs);
}

/* ---------------------------------------------------------------- feedback */
static uint32_t live_gui(void)   /* the item screen object now: sIDCockpit [0x1169120] +0x3C uSubMenuManager +0x74 (a checkpoint load makes new ones) */
{
    uint32_t ck = rd32(0x1169120u), smm = ck ? rd32(ck + 0x3C) : 0;
    return smm ? rd32(smm + 0x74) : 0;
}
static void play(uint32_t gui, unsigned idx)   /* only for the live, open screen (the sound keeps the object as its owner) */
{
    uint32_t snd = rd32(GVA_SOUND_PTR);
    if (snd && gui && gui == live_gui() && gui == M.gui && (uint32_t)(GetTickCount() - M.seen) < 300u) ((sound_fn)(uintptr_t)GVA_SOUND)((void *)(uintptr_t)snd, idx, (void *)(uintptr_t)gui);
}
static void line(int text, uint32_t now)
{
    g_line_text = text, g_line_until = (now ? now : 1) + g_line_ms;
    S.lines++;
}

/* ---------------------------------------------------------------- the give (per-pass callback) */
static void do_request(uint32_t now)
{
    if (!REQ.want) return;
    REQ.want = 0;
    const gv_item *it = gv_item_by_id(REQ.tab, REQ.id);
    cs_t *cs = NULL;
    int me = -1, mode = coop_mode(&cs, &me);
    uint32_t giver = unit_of_slot((unsigned)REQ.giver_slot), partner = unit_of_slot((unsigned)(1 - REQ.giver_slot));
    uint32_t gb = bag_of(giver), pb = bag_of(partner);
    if (!it || mode != REQ.mode || giver != REQ.giver || partner != REQ.partner || !gb || (mode == GV_MODE_NET && me != REQ.giver_slot)) {
        glog("give request dropped: the scene changed (mode %d / %d, giver %08x / %08x, partner %08x / %08x)", mode, REQ.mode, (unsigned)giver, (unsigned)REQ.giver,
             (unsigned)partner, (unsigned)REQ.partner);
        return;
    }
    if (own_transit()) {
        glog("give request dropped: a level change is under way");
        return;
    }
    gv_bag g, p;
    bag_read(gb, &g);
    if (mode == GV_MODE_SPLIT) {
        if (!pb) return;
        bag_read(pb, &p);
        gv_plan pl = gv_split(it, &g, &p);
        if (pl.why == GV_WHY_FULL) {
            S.split_full++;
            line(GV_T_FULL, now);
            play(REQ.gui, g_snd_err);
            glog("split: slot %d -> slot %d item %02x (%08x): the partner's bag is full (%d/%d): nothing moved", REQ.giver_slot, 1 - REQ.giver_slot, (unsigned)it->id,
                 (unsigned)it->cls, gv_have(&p, it), gv_max(&p, it));
            return;
        }
        if (pl.why != GV_WHY_OK || pl.n <= 0) return;
        int gh = gv_have(&g, it), ph = gv_have(&p, it);
        int out = bag_remove(gb, it, pl.n), in = out > 0 ? bag_add(pb, it, out) : 0;
        if (in < out) {   /* the receiver took less than the room said: the rest goes back at once (never lost, never doubled) */
            int back = bag_add(gb, it, out - in);
            glog("split: the receiver took %d of %d: %d given back to the giver", in, out, back);
        }
        gv_bag g2, p2;
        bag_read(gb, &g2), bag_read(pb, &p2);
        int ok = gv_have(&g2, it) + gv_have(&p2, it) == gh + ph;
        if (!ok) S.verify_fail++;
        S.split_gives++;
        play(REQ.gui, g_snd_ok);
        g_refresh_gui = REQ.gui;
        glog("split: slot %d gave %d of item %02x (%08x) to slot %d: giver %d -> %d, partner %d -> %d%s", REQ.giver_slot, in, (unsigned)it->id, (unsigned)it->cls,
             1 - REQ.giver_slot, gh, gv_have(&g2, it), ph, gv_have(&p2, it), ok ? "" : " (SUM CHANGED)");
        return;
    }
    /* network: this PC's own human only */
    if (g_net_me != me) {   /* the first give of a session can come before the network pass saw the role */
        gv_giver_init(&G, me), gv_recv_init(&R, me);
        g_net_me = me;
    }
    int n = gv_offer(it->kind, gv_have(&g, it));
    if (n <= 0 || gv_giver_open(&G)) return;
    int out = bag_remove(gb, it, n);
    if (out <= 0) return;
    gv_msg m;
    uint8_t ep = (uint8_t)(cs_epoch(cs) & 0xFFu);
    uint32_t sect = current_section();
    if (!gv_giver_begin(&G, ep, sect, it->kind, it->cls, out, now, &m)) {
        bag_add(gb, it, out);
        return;
    }
    uint8_t b[GV_MSG_LEN];
    gv_enc(b, &m);
    if (!cs_send_event(cs, GV_EV_KIND, b, GV_MSG_LEN)) {   /* not sent: nothing left this PC: back into the own bag, the give is closed */
        S.net_send_fail++;
        int kind = 0, closed = 0, why = 0;
        uint32_t cls = 0;
        gv_msg a = m;
        a.type = GV_M_ACK, a.from = m.to, a.to = m.from, a.taken = 0, a.why = GV_WHY_NOW;
        int back = gv_giver_answer(&G, &a, ep, sect, &kind, &cls, &closed, &why);
        bag_add(gb, it, back);
        line(GV_T_NOW, now);
        play(REQ.gui, g_snd_err);
        glog("net: GIVE %08x NOT sent (session): %d back to the own human", (unsigned)m.id, back);
        g_refresh_gui = REQ.gui;
        return;
    }
    S.net_sent++;
    play(REQ.gui, g_snd_ok);
    g_refresh_gui = REQ.gui;
    glog("net: GIVE %08x sent: %d of item %02x (%08x) left the own human (slot %d, %d -> %d), level instance %u, section %08x", (unsigned)m.id, out, (unsigned)it->id,
         (unsigned)it->cls, me, gv_have(&g, it), gv_have(&g, it) - out, (unsigned)ep, (unsigned)sect);
}

#define GV_DEFER_GIVE_MS 30000u   /* a GIVE waits this long for this PC's level (a section load) before it is refused "not now" (the giver gets it back) */
static void q_push_back(const uint8_t *b, uint32_t t0)   /* a message that must wait goes to the tail (the others are not blocked) */
{
    EnterCriticalSection(&g_qcs);
    int next = (g_qh + 1) % GQ;
    if (next != g_qt) {
        memcpy(g_q[g_qh].b, b, GV_MSG_LEN), g_q[g_qh].len = GV_MSG_LEN, g_q[g_qh].t0 = t0;
        g_qh = next;
    } else g_qdrop++;
    LeaveCriticalSection(&g_qcs);
}
static void net_pass(uint32_t now)
{
    cs_t *cs = NULL;
    int me = -1, mode = coop_mode(&cs, &me);
    lk_t *k = net1_link_peek();
    if (k && g_reg_link != k) {
        lk_add_event_cb(k, on_event, NULL);
        g_reg_link = k;
    }
    if (!net1_session_active()) {
        if (g_net_me >= 0) {
            int lost = gv_giver_session_end(&G);
            if (lost) glog("net: the session ended with %d give(s) open: dropped without refund (the partner's bag is not known)", lost);
            g_net_me = -1;
        }
        EnterCriticalSection(&g_qcs);
        g_qh = g_qt = 0;
        LeaveCriticalSection(&g_qcs);
        return;
    }
    if (mode != GV_MODE_NET && !cs) {   /* loading: messages wait where they need this PC's level */
        lk_info in;
        memset(&in, 0, sizeof in);
        if (!k) return;
        cs = lk_session(k);
        lk_get_info(k, &in);
        me = in.role;
    }
    if (me != 0 && me != 1) return;
    if (g_net_me != me) {
        gv_giver_init(&G, me), gv_recv_init(&R, me);
        g_net_me = me;
        glog("net: this PC's human is slot %d: gives travel as session events kind 0x%x", me, GV_EV_KIND);
    }
    uint8_t ep = cs ? (uint8_t)(cs_epoch(cs) & 0xFFu) : 0;
    uint32_t own = unit_of_slot((unsigned)me), ob = bag_of(own);
    int level_ok = mode == GV_MODE_NET && ob && !own_transit();   /* this PC's level is in play: its instance and section are valid, its own bag exists */
    uint32_t sect = level_ok ? current_section() : 0;
    if (level_ok && !sect) level_ok = 0;
    EnterCriticalSection(&g_qcs);
    int count = (g_qh - g_qt + GQ) % GQ;   /* this pass looks at what is queued now; what has to wait goes to the tail for the next pass */
    LeaveCriticalSection(&g_qcs);
    int blocked = 0;   /* once a message waits, every later one of this pass waits behind it: the order of the lane is kept (a CANCEL never overtakes its GIVE) */
    for (int i = 0; i < count; i++) {
        uint8_t b[GV_MSG_LEN];
        uint32_t t0 = 0;
        EnterCriticalSection(&g_qcs);
        int have = g_qt != g_qh;
        if (have) memcpy(b, g_q[g_qt].b, GV_MSG_LEN), t0 = g_q[g_qt].t0, g_qt = (g_qt + 1) % GQ;
        LeaveCriticalSection(&g_qcs);
        if (!have) break;
        if (blocked) {
            q_push_back(b, t0);
            continue;
        }
        gv_msg m, a;
        if (!gv_dec(b, GV_MSG_LEN, &m)) {
            S.rx++, S.rx_bad++;
            continue;
        }
        const gv_item *it = gv_item_by_cls(m.kind, m.cls);
        if (m.type == GV_M_GIVE) {
            int wait_level = g_lab_hold || (!level_ok && (!t0 || (uint32_t)(now - t0) < GV_DEFER_GIVE_MS));
            if (wait_level) {   /* this PC loads / is in a level change: the GIVE waits (a section change carries the bags: it is applied after the load) */
                if (!t0) glog("net: GIVE %08x from slot %u waits: this PC's level is not in play (loading / level change)", (unsigned)m.id, (unsigned)m.from);
                q_push_back(b, t0 ? t0 : now), blocked = 1;
                continue;
            }
            int room = -1;
            if (level_ok && unit_up(own) && !downed1_own_down()) {
                gv_bag ib;
                bag_read(ob, &ib);
                room = gv_room(&ib, it);
            }
            int take = gv_recv_give(&R, &m, ep, sect, room, &a);
            if (take == GV_DEFER) {   /* the giver is already in a newer level instance than this PC */
                q_push_back(b, t0 ? t0 : now), blocked = 1;
                continue;
            }
            S.rx++;
            int got = take > 0 ? bag_add(ob, it, take) : 0;
            if (got != take) {   /* the stock add took less (a grenade archive missing ...): the answer says what really arrived */
                glog("net: GIVE %08x: %d planned, %d added", (unsigned)m.id, take, got);
                for (int j = 0; j < GV_RING; j++)
                    if (R.r[j].id == m.id && R.r[j].state == GV_SEEN_APPLIED) R.r[j].taken = (uint16_t)(got > 0 ? got : 0);
                a.taken = (uint16_t)(got > 0 ? got : 0);
                if (!a.taken) a.why = GV_WHY_NOW;
            }
            if (a.type) send_msg(cs, &a);
            if (got > 0) S.recv_applied++, g_refresh_gui = M.gui;
            else S.recv_refused++;
            glog("net: GIVE %08x from slot %u: item %08x x%u (instance %u section %08x, here %u / %08x), room %d -> took %u (%s)", (unsigned)m.id, (unsigned)m.from, (unsigned)m.cls,
                 (unsigned)m.n, (unsigned)m.epoch, (unsigned)m.sect, (unsigned)ep, (unsigned)sect, room, (unsigned)a.taken, gv_why_name(a.why));
        } else if (m.type == GV_M_CANCEL) {   /* answered from the session's record: no level needed */
            S.rx++;
            gv_recv_cancel(&R, &m, &a);
            if (a.type) send_msg(cs, &a);
            glog("net: CANCEL %08x from slot %u: answered %u taken (%s)", (unsigned)m.id, (unsigned)m.from, (unsigned)a.taken, gv_why_name(a.why));
        } else {
            if (!level_ok) {   /* an answer waits for this PC's level: a refund needs the own bag, and a reload / section change must be known */
                q_push_back(b, t0 ? t0 : now), blocked = 1;
                continue;
            }
            S.rx++;
            int kind = 0, closed = 0, why = 0;
            uint32_t cls = 0;
            int back = gv_giver_answer(&G, &m, ep, sect, &kind, &cls, &closed, &why);
            const gv_item *bi = gv_item_by_cls(kind, cls);
            int got = back > 0 ? bag_add(ob, bi, back) : 0;
            if (back > 0) S.refunds++, S.refund_units += (unsigned)got, g_refresh_gui = M.gui;
            if (closed && m.taken == 0 && gv_level_change(m.epoch, m.sect, ep, sect) == GV_LC_NONE) {
                line(why == GV_WHY_FULL ? GV_T_FULL : GV_T_NOW, now);
                play(M.gui, g_snd_err);
            }
            glog("net: %s %08x: the partner took %u of %u (%s)%s; %d back to the own human (added %d)%s", m.type == GV_M_ACK ? "ACK" : "CANCELACK", (unsigned)m.id,
                 (unsigned)m.taken, (unsigned)m.n, gv_why_name(why), closed ? "" : " (not open here: nothing)", back, got,
                 !closed ? "" : gv_level_change(m.epoch, m.sect, ep, sect) == GV_LC_RELOAD ? " [a reload since the give: the checkpoint gave it back]"
                 : gv_level_change(m.epoch, m.sect, ep, sect) == GV_LC_SECTION ? " [after a section change: the bags were carried]" : "");
        }
    }
    gv_msg c[GV_PEND_MAX];
    unsigned dr0 = G.dropped_reload;
    int nc = gv_giver_tick(&G, now, level_ok, ep, sect, c, GV_PEND_MAX);
    if (G.dropped_reload != dr0) glog("net: %u open give(s) dropped: a reload of this section since (the checkpoint restored both bags)", G.dropped_reload - dr0);
    for (int i = 0; i < nc; i++) {
        send_msg(cs, &c[i]);
        glog("net: GIVE %08x: asked again (CANCEL: applied or not?): a section change or no answer for %u ms", (unsigned)c[i].id, GV_TIMEOUT_MS);
    }
}

/* ---------------------------------------------------------------- the item screen (inside its update, logic thread) */
static const char *close_string(void)
{
    uint32_t root = rd32(GVA_MSGROOT_PTR), bank = root ? rd32(root + 0x114) : 0;
    if (!root || !rd32(root + 0x1A4) || !bank || !(rd8(bank + 0x50) & 1) || ((rd32(bank + 0x50) >> 4) & 1)) return "";
    const char *s = ((bankstr_fn)(uintptr_t)GVA_BANKSTR)((void *)(uintptr_t)bank, 0x22);
    return s ? s : "";
}
static void memo_set(uint32_t gui, const char *s1)
{
    uint32_t text = rd32(gui + 0x89C);
    if (!text) return;
    ((memo2_fn)(uintptr_t)GVA_MEMO2)((void *)(uintptr_t)gui, (void *)(uintptr_t)text, s1, close_string());
    S.memo_sets++;
}
static int decide_pressed(uint32_t gui, int cur)
{
    uint32_t gp = rd32(R1_GAMEPAD_PTR);
    if (!gp) return 0;
    uint32_t slot = rd32(gp + 0x870);
    if (slot > 1) return 0;
    uint32_t hit = rd32(gp + 0x1A0 + 0x2CCu * slot) & rd32(gp + 0x5D4);
    if (!hit) return 0;
    if (hit & ~0x1000000u) return 1;   /* pad A / the decide key */
    uint32_t cell = cur >= 0 && cur < 12 ? rd32(gui + 0x7F0 + 4u * (unsigned)cur) : 0;   /* a click: on the cell under the cursor */
    return cell && (((hover_fn)(uintptr_t)GVA_HOVER)((void *)(uintptr_t)gui, (void *)(uintptr_t)cell, -1) & 0xFFu);
}
static void ui_pass(uint32_t gui)
{
    uint32_t now = GetTickCount();
    if (!(rd8(gui + 0x148) & 1)) {
        if (M.gui == gui) M.gui = 0;
        return;
    }
    M.seen = now;
    if (g_refresh_gui == gui) {   /* a give / refund changed this bag: the stock list and memo again (the stock order of vf8) */
        g_refresh_gui = 0;
        ((gui_fn)(uintptr_t)GVA_LIST)((void *)(uintptr_t)gui);
        ((gui_fn)(uintptr_t)GVA_MEMO)((void *)(uintptr_t)gui);
        M.shown = -1;
    }
    uint32_t st = rd32(gui + 0x240);
    if (st != 0xD && st != 5) return;   /* the screen takes no input now (opening / closing / tab animation) */
    int tab = (int)rd32(gui + 0x298), scroll = (int)rd32(gui + 0x2A0), cur = (int)rd32(gui + 0x2A4), id = -1;
    if (tab >= 0 && tab <= 2 && scroll >= 0 && scroll < 0x51 - 12 && cur >= 0 && cur < 12) id = (int)rd32(gui + 0x2B4 + 4u * (unsigned)(tab * 0x51 + scroll + cur));
    cs_t *cs = NULL;
    int me = -1;
    gv_ctx c;
    memset(&c, 0, sizeof c);
    c.mode = coop_mode(&cs, &me), c.tab = tab, c.item_id = id;
    uint32_t giver = local_player(), gb = bag_of(giver);
    int gslot = giver ? (int)rd32(giver + GV_U_SLOT) : -1;
    uint32_t partner = gslot == 0 || gslot == 1 ? unit_of_slot((unsigned)(1 - gslot)) : 0;
    const gv_item *it = NULL;
    if (c.mode == GV_MODE_NET && gslot != me) c.mode = GV_MODE_NONE;   /* the screen shows another human than this PC's own: never */
    c.have = bag_count(gb, gv_item_by_id(tab, id));   /* the count only (no capacity call every frame) */
    c.giver_up = unit_up(giver) && !(c.mode == GV_MODE_NET && downed1_own_down());
    c.partner_present = partner && bag_of(partner);
    c.partner_up = c.mode == GV_MODE_NET ? partner_up_net() && rdf(partner + GV_U_HP) > 0.0f : unit_up(partner);
    if (giver && partner) {
        c.dx = rdf(partner + GV_U_POS) - rdf(giver + GV_U_POS), c.dy = rdf(partner + GV_U_POS + 4) - rdf(giver + GV_U_POS + 4);
        c.dz = rdf(partner + GV_U_POS + 8) - rdf(giver + GV_U_POS + 8);
        if (g_reach != GV_REACH) {   /* lab `give set reach`: scale the offsets so gv_check's fixed reach applies */
            float s = GV_REACH / (g_reach > 1.0f ? g_reach : 1.0f);
            c.dx *= s, c.dz *= s;
        }
    } else c.dx = c.dy = c.dz = NAN;
    c.busy = c.mode == GV_MODE_NET && gv_giver_open(&G) > 0;
    {   /* a level change under way: the barrier's shield, this PC loading, or (network) the partner loading */
        unsigned pf = 0;
        c.transit = own_transit() || (c.mode == GV_MODE_NET && puppet_partner_flags(&pf) && (pf & CP_SF_LOADING));
    }
    int why = gv_check(&c, &it);
    if (why != g_last_why) {
        if (it || g_last_why == GV_WHY_OK) glog("item screen: tab %d cell %02x: %s", tab, (unsigned)id, gv_why_name(why));
        g_last_why = why;
    }
    int line_on = g_line_text >= 0 && (int32_t)(g_line_until - now) > 0;
    if (!line_on) g_line_text = -1;
    int want = line_on ? 2 : why == GV_WHY_OK ? 1 : 0, lang = game_lang();
    uint32_t gp = rd32(R1_GAMEPAD_PTR);
    int dev = gp ? (int)rd32(gp + 0x874) : 0, f290 = (int)rd8(gui + 0x290), pos = scroll * 16 + cur;
    if (gui != M.gui || tab != M.tab || pos != M.pos || id != M.id || want != M.shown || lang != M.lang || dev != M.dev || f290 != M.f290) {
        if (want) {
            char s[160];
            if (want == 2) {
                snprintf(s, sizeof s, "%s", gv_text(g_line_text, lang));
                glog("item screen: the refusal line is in the memo bar (%u ms): %s", g_line_ms, s);
            }
            else gv_memo_action(s, sizeof s, lang);
            memo_set(gui, s);
        } else if (M.gui == gui && M.shown > 0) {
            ((gui_fn)(uintptr_t)GVA_MEMO)((void *)(uintptr_t)gui);   /* our line is gone: the stock memo of that cell */
            S.memo_stock++;
        }
        M.gui = gui, M.tab = tab, M.pos = pos, M.id = id, M.shown = want, M.lang = lang, M.dev = dev, M.f290 = f290;
    }
    if (why == GV_WHY_OK && !REQ.want && decide_pressed(gui, cur)) {
        S.decides++;
        REQ.want = 1, REQ.gui = gui, REQ.giver = giver, REQ.partner = partner, REQ.giver_slot = gslot, REQ.tab = tab, REQ.id = id, REQ.mode = c.mode;
    }
}

/* force_align_arg_pointer: the game calls its vtable slots with a 4-byte aligned stack; our code (SSE) assumes 16 (g1n1 live 22:30:38: `movaps [esp+0x100]` in
 * ui_pass's distance code = #GP, reported as AccessViolation read FFFFFFFF at DINPUT8.dll+0x40B0F, the host's game died at the first BACK). Every entry the
 * game calls realigns (as capture.c hook_kb, rolebarrier1_game.c cb_*). */
static void TC __attribute__((force_align_arg_pointer)) update_hook(void *gui)
{
    orig_update(gui);
    if (g_on && g_ok) ui_pass((uint32_t)(uintptr_t)gui);
}

/* ---------------------------------------------------------------- install */
static int check_bytes(char *why, int size)
{
    for (unsigned i = 0; i < sizeof SIG / sizeof SIG[0]; i++) {
        uint8_t cur[12];
        if (!r1_safe_read((const void *)(uintptr_t)SIG[i].va, cur, 12) || memcmp(cur, SIG[i].b, 12)) {
            snprintf(why, (size_t)size, "the bytes of %s at %08x differ from build 13298487", SIG[i].what, (unsigned)SIG[i].va);
            return 0;
        }
    }
    for (unsigned i = 0; i < sizeof BAGVT / sizeof BAGVT[0]; i++)
        if (rd32(GVA_BAG_VT + 4u * BAGVT[i].slot) != BAGVT[i].fn) {
            snprintf(why, (size_t)size, "the bag vtable slot %u is not %08x", BAGVT[i].slot, (unsigned)BAGVT[i].fn);
            return 0;
        }
    return 1;
}
static int hook_set(int on)
{
    uint32_t cur = rd32(GVA_SUBMENU_SLOT8), mine = (uint32_t)(uintptr_t)update_hook;
    if (on) {
        if (cur == mine) return 1;
        if (cur != GVA_SUBMENU_UPDATE) {
            snprintf(g_why_off, sizeof g_why_off, "uGUI_SubMenu slot 8 holds %08x (not the stock update): not wrapped", (unsigned)cur);
            return 0;
        }
        orig_update = (gui_fn)(uintptr_t)GVA_SUBMENU_UPDATE;
        if (!wr_code32(GVA_SUBMENU_SLOT8, mine)) return 0;
        g_hooked = 1;
        glog("the item screen's update (uGUI_SubMenu slot 8, 0x7BCD30) is wrapped: the give entry shows in a co-op session");
        return 1;
    }
    if (cur == mine && wr_code32(GVA_SUBMENU_SLOT8, GVA_SUBMENU_UPDATE)) glog("the item screen's update is the stock one again");
    g_hooked = 0;
    return 1;
}

static void __attribute__((force_align_arg_pointer)) give_frame(void)   /* lt.c calls it from its stub without a realign */
{
    if (!g_inited) return;
    if (!g_checked) {
        g_checked = 1;
        g_ok = check_bytes(g_why_off, sizeof g_why_off);
        if (!g_ok) glog("OFF: %s", g_why_off);
        else if (g_on) hook_set(1);
    }
    if (!g_ok) return;
    uint32_t now = GetTickCount();
    do_request(now);
    net_pass(now);
}

int give1_init(void)
{
    if (g_inited) return g_on;
    char v[8];
    InitializeCriticalSection(&g_qcs);
    if (r1_env("REV1_GIVE", v, sizeof v) && v[0] == '0') g_on = 0;
    g_inited = 1;
    lt_add_frame_cb(give_frame);
    r1_log("give: ready (%s); the item screen is wrapped at the first logic pass when the stock bytes match", g_on ? "on" : "off: REV1_GIVE=0");
    return g_on;
}

/* ---------------------------------------------------------------- console */
static const gv_item *item_arg(const char *kind, int index)
{
    int k = !strcmp(kind, "herb") ? GV_K_HERB : !strcmp(kind, "ammo") ? GV_K_AMMO : !strcmp(kind, "gren") ? GV_K_GRENADE : 0;
    for (int i = 0; i < GV_NITEMS; i++)
        if (GV_ITEMS[i].kind == k && GV_ITEMS[i].index == index) return &GV_ITEMS[i];
    return NULL;
}
void __attribute__((force_align_arg_pointer)) give_cmd(const char *arg, char *out, int size)
{
    char w[16] = "", a2[16] = "";
    int x = 0, y = 0, z = 0;
    int n = arg ? sscanf(arg, "%15s", w) : 0;
    if (n < 1 || !strcmp(w, "status")) {
        cs_t *cs = NULL;
        int me = -1, mode = coop_mode(&cs, &me);
        snprintf(out, (size_t)size,
                 "give: %s, %s%s | mode %s (me %d) | reach %.0f | decides %u, split gives %u (full %u, verify fail %u) | net sent %u (send fail %u), open %d, refunds %u (%u units), "
                 "dropped by a reload %u, asked after a section change %u, lost at the session end %u | received %u (bad %u, dropped %u): applied %u refused %u (reload %u), "
                 "waited %u, answers %u | memo %u / stock %u, lines %u | archives %u (fail %u) | last: %s",
                 g_on ? "on" : "off", g_ok ? (g_hooked ? "item screen wrapped" : "not wrapped") : "OFF", g_ok ? "" : g_why_off,
                 mode == GV_MODE_NET ? "network" : mode == GV_MODE_SPLIT ? "split" : "none", me, (double)g_reach, S.decides, S.split_gives, S.split_full, S.verify_fail, S.net_sent,
                 S.net_send_fail, gv_giver_open(&G), S.refunds, S.refund_units, G.dropped_reload, G.asked_after_section, G.lost_session, S.rx, S.rx_bad, g_qdrop, S.recv_applied,
                 S.recv_refused, R.refused_reload, R.deferred, S.answers_tx,
                 S.memo_sets, S.memo_stock, S.lines, S.arch_loads, S.arch_fail, gv_why_name(g_last_why));
        return;
    }
    if (!strcmp(w, "on") || !strcmp(w, "off")) {
        g_on = w[1] == 'n';
        if (g_ok) hook_set(g_on);
        snprintf(out, (size_t)size, "give: %s", g_on ? "on" : "off (the stock item screen)");
        return;
    }
    if (!strcmp(w, "bag")) {
        int slot = -1;
        sscanf(arg, "%*s %d", &slot);
        uint32_t u = slot == 0 || slot == 1 ? unit_of_slot((unsigned)slot) : local_player(), b = bag_of(u);
        char l[300];
        bag_line(b, l, sizeof l);
        snprintf(out, (size_t)size, "give bag: unit %08x (slot %d) bag %08x: %s", (unsigned)u, u ? (int)rd32(u + GV_U_SLOT) : -1, (unsigned)b, b ? l : "-");
        return;
    }
    if (!strcmp(w, "gui")) {
        uint32_t ck = rd32(0x1169120u), smm = ck ? rd32(ck + 0x3C) : 0, gui = live_gui();
        snprintf(out, (size_t)size, "give gui: sub menu manager %08x flags %08x, screen %08x visible %u state %u tab %d scroll %d cursor %d cell %02x | last check: %s | line %d",
                 (unsigned)smm, smm ? rd32(smm + 0xC) : 0u, (unsigned)gui, gui ? rd8(gui + 0x148) & 1 : 0u, gui ? rd32(gui + 0x240) : 0u, gui ? (int)rd32(gui + 0x298) : -1,
                 gui ? (int)rd32(gui + 0x2A0) : -1, gui ? (int)rd32(gui + 0x2A4) : -1,
                 gui ? (unsigned)rd32(gui + 0x2B4 + 4u * (rd32(gui + 0x298) * 0x51u + rd32(gui + 0x2A0) + rd32(gui + 0x2A4))) : 0u, gv_why_name(g_last_why), g_line_text);
        return;
    }
    if (!strcmp(w, "lab")) {   /* LAB: the give of the screen's decide without the screen: giver slot, tab, cell id (the same request path) */
        if (sscanf(arg, "%*s %d %d %x", &x, &y, &z) != 3 || (x != 0 && x != 1)) {
            snprintf(out, (size_t)size, "give lab <giver slot> <tab 0|1> <cell id hex>");
            return;
        }
        cs_t *cs = NULL;
        int me = -1, mode = coop_mode(&cs, &me);
        uint32_t g = unit_of_slot((unsigned)x), p = unit_of_slot((unsigned)(1 - x));
        if (mode == GV_MODE_NONE || !g || !p || (mode == GV_MODE_NET && me != x) || !gv_item_by_id(y, z)) {
            snprintf(out, (size_t)size, "give lab: REFUSED (mode %d, me %d, giver %08x, partner %08x, item %d/%02x)", mode, me, (unsigned)g, (unsigned)p, y, (unsigned)z);
            return;
        }
        REQ.want = 1, REQ.gui = 0, REQ.giver = g, REQ.partner = p, REQ.giver_slot = x, REQ.tab = y, REQ.id = z, REQ.mode = mode;
        snprintf(out, (size_t)size, "give lab: slot %d gives item %02x (tab %d) at the next pass (no distance / downed check: LAB)", x, (unsigned)z, y);
        return;
    }
    if (!strcmp(w, "fill")) {   /* LAB: the stock add on a human's bag (test setup) */
        if (sscanf(arg, "%*s %d %15s %d %d", &x, a2, &y, &z) != 4 || (x != 0 && x != 1) || z <= 0) {
            snprintf(out, (size_t)size, "give fill <slot> herb|ammo|gren <index> <n>");
            return;
        }
        const gv_item *it = item_arg(a2, y);
        uint32_t b = bag_of(unit_of_slot((unsigned)x));
        int got = b && it ? bag_add(b, it, z) : 0;
        char l[300];
        bag_line(b, l, sizeof l);
        snprintf(out, (size_t)size, "give fill: slot %d %s %d +%d -> added %d (LAB, the stock add): %s", x, a2, y, z, got, l);
        r1_log("%s", out);
        return;
    }
    if (!strcmp(w, "set") && sscanf(arg, "%*s %15s %d", a2, &x) == 2 && !strcmp(a2, "reach") && x > 0) {
        g_reach = (float)x;
        snprintf(out, (size_t)size, "give: reach %d units (LAB; product 300)", x);
        return;
    }
    if (!strcmp(w, "set") && sscanf(arg, "%*s %15s %d", a2, &x) == 2 && !strcmp(a2, "line") && x >= 500 && x <= 20000) {
        g_line_ms = (unsigned)x;
        snprintf(out, (size_t)size, "give: a refusal line stays %d ms (LAB; product 2500)", x);
        return;
    }
    if (!strcmp(w, "set") && sscanf(arg, "%*s %15s %d", a2, &x) == 2 && !strcmp(a2, "hold")) {
        g_lab_hold = x != 0;
        snprintf(out, (size_t)size, "give: incoming GIVEs %s (LAB)", g_lab_hold ? "WAIT until `give set hold 0`" : "are handled again");
        r1_log("%s", out);
        return;
    }
    if (!strcmp(w, "sound") && sscanf(arg, "%*s %x %x", &x, &y) == 2) {
        g_snd_ok = (unsigned)x, g_snd_err = (unsigned)y;
        snprintf(out, (size_t)size, "give: sounds ok %x refusal %x", x, y);
        return;
    }
    snprintf(out, (size_t)size, "give [status] | on | off | gui | bag [slot] | lab <slot> <tab> <id hex> | fill <slot> herb|ammo|gren <index> <n> | set reach <u> | set line <ms> | set hold 0|1 | sound <ok> <err>");
}
