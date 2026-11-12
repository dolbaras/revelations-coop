/*
 * scan1.c - the engine glue of the Genesis scanner for the second player (Revelations 1). The rules: scan1.h; design, static RE and live results:
 * notes/scan1.md.
 *
 * Engine side (build 13298487, every stock call byte-checked at the first pass; one mismatch keeps the module off with a logged reason):
 *   - G: every 500 ms on the logic thread (lt.c pass, outside the unit loop): a human unit of this PC without a Genesis gets one through the story's own
 *     action FUN_00772500 (PcsMainCharacterAddWeapon: class 0x80051B30, slot -1, num 1) as soon as any player unit has one. Split: both humans; network: the
 *     own human and the partner's puppet (the puppet then shows the scanner in the hand like its owner); no co-op: nothing.
 *   - X (split screen): split.c's menu exchange runs a third kind, "the second human uses the Genesis" (scan1_split_hook): pads swapped at the XInput level,
 *     capture roles swapped, own slot = his character, whole view; the engine's one scanner pipeline then runs for him. His analysis rate (outside 1-2) and his
 *     one-herb latch of 1-2 are swapped into the game for that time.
 *   - R (network): the E2 mirror of story1_game.inc asks scan1_rate_shared_now(): only in 1-2 (0x91010200) the rate is one team value.
 *   - V (network): cGenesisInvisibleItemParam's vtable slot 5 (0x6DE990, the scan result of a hidden item) is wrapped (data write of the slot, no code patch):
 *     after the stock call the item's world key (sArrange item list) goes to the partner as a reliable session event of kind SC_EV_KIND; the partner's PC
 *     runs the same stock reveal on its copy (the param's vf5 when the param is found, else FUN_005DAAB0(item, 1)).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "r1.h"
#include "scan1.h"
#include "scan1_game.h"
#include "split.h"
#include "capture.h"
#include "net1.h"
#include "lobby_link.h"
#include "coop_session.h"
#include "coop_proto.h"
#include "world1_hooks.h"

#define TC __attribute__((thiscall))
#define ALIGNED __attribute__((force_align_arg_pointer))

/* ---------------------------------------------------------------- addresses (build 13298487) */
#define SCA_ADDWEAPON      0x772500u   /* thiscall(action; param, x, unit) ret 0xC: PcsMainCharacterAddWeapon's engine call; `this` unread when param+8 == 0x10 */
#define SCA_REVEAL_ITEM    0x5DAAB0u   /* thiscall(item; char on) ret 4: item+0xF19 = on; the key 0x80041500 also SetScenarioFlag(4865) */
#define SCA_PARAM_VF5      0x6DE990u   /* thiscall(param): cGenesisInvisibleItemParam's scan result (+0x20 = 2, FUN_005DAAB0([+8], 1)) */
#define SCA_PARAM_VT       0x106AD20u  /* cGenesisInvisibleItemParam vtable; slot 5 = SCA_PARAM_VF5 */
#define SCA_PARAM_SLOT5    (SCA_PARAM_VT + 5u * 4u)
#define SCA_SECTION        0x720320u   /* thiscall([0x116A340]) ret 0: the running section 0x91EESS00 */
#define SCA_EPISODE_PTR    0x116A340u
#define SCA_CSYSTEM_PTR    0x116A84Cu  /* [..]+0x74 = the analysis rate */
#define SCA_COCKPIT_PTR    0x1169120u  /* sIDCockpit: [+0x28] = uCockpitManagerMain, its +0x48 = uGUI_GadgetScanner */
#define SCA_CAMROOT_PTR    0x11E00B8u  /* sGameCamera: viewport 0 camera at +0x30+4, managers +0xCE0 / +0xCE4 */
#define SCA_SARRANGE_PTR   0x11695C4u
#define SCA_GENESIS_STATE  0x1064F54u  /* Upper::GenesisAimState vtable */
#define SCA_BAG_VT         0x10186B0u  /* cBioItemPack */
#define SC_U_SLOT 0xE2Cu
#define SC_U_BAG 0xFE8u
#define SC_U_UPPER 0xF80u              /* the current upper-body state object (stage 3) */
#define SC_U_F44 0xF44u                /* bit 0x20000: the scan view is open (FUN_006A85A0) */
#define SC_BAG_CUR 0xD4u
#define SC_GUI_RUN 0x148u              /* bit 0: the GUI runs */
#define SC_GUI_STORY 0x320u            /* 1 = story mode (1-2): the rate stays at 100 after the one herb */
#define SC_GUI_LATCH 0x321u            /* story mode: the herb was given */
#define SC_PAD_DATA 0x628u             /* sGamePad + this + 0x90 * slot: PadData; +8 bit 4 "Genesis allowed", +0x10 control scheme */
#define SC_ITEM_VISIBLE 0xF19u
#define SCA_CHGSTATE       0x49CD20u   /* cStateManager::ChangeState thiscall(mgr; id, a, b) ret 0xC: the state array is mgr+0x30 (+0 ptr, +4 count) */
#define SCA_PLACE_CALL1    0x6BF0DBu   /* FUN_006BEF50 (a unit's place status): call ChangeState([unit+0xE80]; 5, 0, 0) */
#define SCA_PLACE_CALL2    0x6BF16Cu

static const struct { uint32_t va; uint8_t b[12]; const char *what; } SIG[] = {
    {SCA_ADDWEAPON, {0x56, 0x57, 0x8b, 0x7c, 0x24, 0x0c, 0x83, 0x7f, 0x08, 0x10, 0x8b, 0xf1}, "AddWeapon action"},
    {SCA_REVEAL_ITEM, {0x8a, 0x44, 0x24, 0x04, 0x88, 0x81, 0x19, 0x0f, 0x00, 0x00, 0x3c, 0x01}, "item reveal"},
    {SCA_PARAM_VF5, {0xc7, 0x41, 0x20, 0x02, 0x00, 0x00, 0x00, 0x8b, 0x49, 0x08, 0x6a, 0x01}, "hidden item scan result"},
    {SCA_SECTION, {0x53, 0x8b, 0xd9, 0x56, 0x85, 0xdb, 0x74, 0x05, 0xe8, 0x93, 0x56, 0x2b}, "running section"},
};

typedef unsigned (TC *fsm4_fn)(void *self, void *param, void *x, void *unit);   /* ret 0xC */
typedef void (TC *reveal_fn)(void *item, int on);                              /* ret 4 */
typedef void (TC *param5_fn)(void *param);                                     /* ret */
typedef uint32_t (TC *sect_fn)(void *episode);                                 /* ret */
typedef unsigned (TC *chg_fn)(void *mgr, unsigned id, unsigned a, unsigned b);  /* ret 0xC */

/* ---------------------------------------------------------------- memory */
static uint32_t rd32(uint32_t a)
{
    uint32_t v = 0;
    if (a < 0x10000u || !r1_safe_read((const void *)(uintptr_t)a, &v, 4)) return 0;
    return v;
}
static uint8_t rd8(uint32_t a)
{
    uint8_t v = 0;
    if (a < 0x10000u || !r1_safe_read((const void *)(uintptr_t)a, &v, 1)) return 0;
    return v;
}
static int wr32(uint32_t a, uint32_t v)
{
    SIZE_T got = 0;
    return a >= 0x10000u && WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, &v, 4, &got) && got == 4;
}
static int wr8(uint32_t a, uint8_t v)
{
    SIZE_T got = 0;
    return a >= 0x10000u && WriteProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)a, &v, 1, &got) && got == 1;
}
static int wr_code32(uint32_t a, uint32_t v)   /* a vtable slot in .rdata */
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, 4, PAGE_READWRITE, &old)) return 0;
    *(volatile uint32_t *)(uintptr_t)a = v;
    VirtualProtect((void *)(uintptr_t)a, 4, old, &old);
    return 1;
}
/* world1_hooks' list walkers read through a table */
static int m_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    return a >= 0x10000u && r1_safe_read((const void *)(uintptr_t)a, out, n);
}
static int m_write(void *u, uint32_t a, const void *p, uint32_t n)
{
    (void)u, (void)a, (void)p, (void)n;
    return 0;   /* this module never writes through the table */
}
static uint32_t m_alloc(void *u, uint32_t n)
{
    (void)u, (void)n;
    return 0;
}
static const wh_mem MEM = {m_read, m_write, m_alloc, NULL, NULL};

static void slog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int wr_code(uint32_t a, const void *p, unsigned n)   /* a few code bytes */
{
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)a, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)(uintptr_t)a, p, n);
    VirtualProtect((void *)(uintptr_t)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)a, n);
    return 1;
}
static void slog(const char *fmt, ...)
{
    char b[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    r1_log("scan: %s", b);
}

/* ---------------------------------------------------------------- state */
static int g_inited, g_checked, g_ok, g_on = 1, g_x_on = 1, g_hooked;
static int g_fmask = 1;                /* split: the scanner's screen-filter units of the first human's scan are kept out of view 2 (the second human's half) */
static uint32_t g_fm_unit[3], g_fm_bits[3];   /* units whose view bit 18 was cleared, and that bit's old value */
static unsigned g_fm_sets;
static char g_why_off[160];
static sc_give_t G;
static sc_x_t X;
static sc_gauge_t GA;
static sc_seen_t SEEN;
static sc_have_t HV;
static volatile int g_local_source;    /* rule G saw a Genesis on a human of this PC (the last pass) */
static uint8_t g_src_epoch;            /* the level instance of that pass (a value of an older instance is not told) */
static int g_x_slot = -1;              /* the character of the exchange */
static uint32_t g_x_last_end;
static int g_x_ended;
static uint32_t g_last_give;
static struct { unsigned x_enters, x_leaves, x_refused_busy, reveal_hooked, reveal_sent, reveal_send_fail, reveal_dup_local, rx, rx_bad, rx_applied, rx_param, rx_wait, rx_drop[8];
                unsigned give_tries, give_passes, rate_asks, rate_shared; } S;
static uint32_t g_last_item_seen;      /* the item of the last local reveal (status) */
static volatile uint32_t g_sect;       /* the running section, read in the pass while a player unit exists (a level runs); E2 asks it from its own handlers */

/* the queue of received REVEALs (session thread -> logic thread) */
#define SQ 32
static CRITICAL_SECTION g_qcs;
static struct { uint8_t b[SC_MSG_LEN]; uint32_t t0; } g_q[SQ];
static int g_qh, g_qt;
static unsigned g_qdrop;
/* the local reveals to send (the GUI's update -> the pass) */
static struct { uint32_t k[4], cls, sect; uint8_t epoch; } g_tx[SQ];
static volatile int g_txh, g_txt;
static lk_t *g_reg_link;

/* ---------------------------------------------------------------- engine reads */
static uint32_t unit_class(uint32_t u)   /* [[vtable+0x10] = mov eax, DTI; ret] -> DTI +0x1C */
{
    uint32_t vt = rd32(u), f = vt ? rd32(vt + 0x10) : 0;
    if (!f || rd8(f) != 0xB8 || rd8(f + 5) != 0xC3) return 0;
    uint32_t dti = rd32(f + 1);
    return dti ? rd32(dti + 0x1C) : 0;
}
static uint32_t unit_of_slot(unsigned slot)   /* sPlayer list 0: +0xE2C == slot, (+0xC & 7) in {1, 2} */
{
    uint32_t sp = rd32(R1_SPLAYER_PTR);
    if (!sp) return 0;
    uint32_t node = rd32(sp + 0x30), end = rd32(sp + 0x34);
    for (int guard = 0; node && node != end && guard < 64; guard++) {
        uint32_t u = rd32(node + 0x10), k = u ? (rd32(u + 0xC) & 7u) : 0;
        if (u && rd32(u + SC_U_SLOT) == slot && (k == 1 || k == 2)) return u;
        node = rd32(node + 8);
    }
    return 0;
}
static uint32_t bag_of(uint32_t u)
{
    uint32_t b = u ? rd32(u + SC_U_BAG) : 0;
    return b && rd32(b) == SCA_BAG_VT ? b : 0;
}
static int bag_has_genesis(uint32_t b)
{
    uint32_t w = b ? rd32(b + SC_BAG_GENESIS) : 0;
    return w && unit_class(w) == SC_CLASS_SCANNER;
}
static float rdf(uint32_t a)
{
    float v = 0;
    if (a < 0x10000u || !r1_safe_read((const void *)(uintptr_t)a, &v, 4)) return 0;
    return v;
}
static int unit_in_play(uint32_t u) { return u && (rd32(u + 0xC) & 0xC00u) == 0xC00u; }
static int unit_in_genesis(uint32_t u)   /* the Genesis aim state, or the scanner in the hand */
{
    uint32_t b = bag_of(u), st = u ? rd32(u + SC_U_UPPER) : 0;
    return (b && rd32(b + SC_BAG_CUR) == SC_GENESIS_INDEX) || (st && rd32(st) == SCA_GENESIS_STATE);
}
static uint32_t scanner_gui(void)
{
    uint32_t ck = rd32(SCA_COCKPIT_PTR), cp = ck ? rd32(ck + 0x28) : 0;
    return cp ? rd32(cp + 0x48) : 0;
}
static int gui_running(uint32_t gui) { return gui && (rd8(gui + SC_GUI_RUN) & 1); }
static int rate_read(void)
{
    uint32_t cs = rd32(SCA_CSYSTEM_PTR);
    return cs ? (int)rd32(cs + 0x74) : -1;
}
static int rate_write(int v)
{
    uint32_t cs = rd32(SCA_CSYSTEM_PTR);
    return cs && wr32(cs + 0x74, (uint32_t)sc_clamp_rate(v));
}
static uint32_t current_section(void)
{
    if (!g_ok) return 0;
    uint32_t ep = rd32(SCA_EPISODE_PTR);
    return ep ? ((sect_fn)(uintptr_t)SCA_SECTION)((void *)(uintptr_t)ep) : 0;
}
static uint32_t pad_data(unsigned slot)
{
    uint32_t gp = rd32(R1_GAMEPAD_PTR);
    return gp && slot < 2 ? gp + SC_PAD_DATA + 0x90u * slot : 0;
}

/* ---------------------------------------------------------------- mode */
enum { SC_MODE_NONE = 0, SC_MODE_SPLIT, SC_MODE_NET };
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
        if (!cs || cs_phase(cs) != CP_PHASE_PLAYING || in.role < 0 || in.role > 1) return SC_MODE_NONE;
        if (out_cs) *out_cs = cs;
        if (me) *me = in.role;
        return SC_MODE_NET;
    }
    return menu_split_armed() ? SC_MODE_SPLIT : SC_MODE_NONE;
}

/* ---------------------------------------------------------------- G: a Genesis for everybody */
static int add_genesis(uint32_t unit)
{
    uint32_t param[8] __attribute__((aligned(4)));
    memset(param, 0, sizeof param);
    param[2] = 0x10;                 /* +8: the unit is given (4th argument) */
    param[3] = SC_CLASS_SCANNER;     /* +0xC: weapon id */
    param[4] = 0xFFFFFFFFu;          /* +0x10: slot -1 = the bag's own place for the class */
    param[5] = 1;                    /* +0x14: num */
    ((fsm4_fn)(uintptr_t)SCA_ADDWEAPON)(NULL, param, NULL, (void *)(uintptr_t)unit);
    return bag_has_genesis(bag_of(unit));
}
static uint8_t net_epoch(void);
static void give_pass(int mode, uint32_t now, int force)
{
    if (!force && now - g_last_give < SC_GIVE_POLL_MS) return;
    g_last_give = now;
    S.give_passes++;
    if (mode == SC_MODE_NONE) return;   /* per human: present, in play (not loading / not switched off) - no camera test (sc1n1: the guest's rule G never ran) */
    sc_human h[2];
    int allowed[2] = {1, 1};   /* split: both humans; network: the own human and the puppet */
    uint32_t u[2];
    for (int i = 0; i < 2; i++) {
        u[i] = unit_of_slot((unsigned)i);
        uint32_t b = bag_of(u[i]);
        memset(&h[i], 0, sizeof h[i]);
        h[i].present = u[i] != 0 && b != 0, h[i].in_play = unit_in_play(u[i]), h[i].bag = b;
        h[i].has = bag_has_genesis(b), h[i].empty = b && rd32(b + SC_BAG_GENESIS) == 0;
    }
    g_local_source = (h[0].present && h[0].has) || (h[1].present && h[1].has), g_src_epoch = mode == SC_MODE_NET ? net_epoch() : 0;
    int remote = mode == SC_MODE_NET && sc_have_source(&HV, net_epoch());   /* the partner's PC has one in this level instance (HAVE) */
    unsigned t = sc_give_targets_src(&G, h, allowed, remote);
    for (int i = 0; i < 2; i++) {
        if (!(t & (1u << i))) continue;
        S.give_tries++;
        int done = add_genesis(u[i]);
        sc_give_result(&G, i, h[i].bag, done);
        slog("the human of slot %d (unit %08x, bag %08x) %s the Genesis (%s; stock AddWeapon 0x80051B30)%s", i, (unsigned)u[i], (unsigned)h[i].bag, done ? "now has" : "did NOT get",
             g_local_source ? "the other human has one" : "a human on the partner's PC has one", mode == SC_MODE_NET ? " [network]" : " [split]");
    }
}

/* ---------------------------------------------------------------- V: hidden items (network) */
static int item_key(uint32_t item, uint32_t k[4])
{
    uint32_t sa = rd32(SCA_SARRANGE_PTR);
    wh_entry e;
    if (!sa || !wh_find_unit(&MEM, sa, WH_LIST_ITEMS, item, &e)) return 0;
    memcpy(k, e.k, 16);
    return 1;
}
static uint8_t net_epoch(void)
{
    lk_t *k = net1_link_peek();
    cs_t *cs = k ? lk_session(k) : NULL;
    return cs ? (uint8_t)(cs_epoch(cs) & 0xFFu) : 0;
}
static void TC ALIGNED reveal_hook(void *param)   /* cGenesisInvisibleItemParam slot 5: the scan of a hidden item finished on this PC */
{
    ((param5_fn)(uintptr_t)SCA_PARAM_VF5)(param);
    S.reveal_hooked++;
    if (!g_on || !net1_session_active()) return;
    uint32_t item = rd32((uint32_t)(uintptr_t)param + 8), k[4];
    if (!item || !item_key(item, k)) return;
    g_last_item_seen = item;
    int next = (g_txh + 1) % SQ;
    if (next == g_txt) return;
    memcpy(g_tx[g_txh].k, k, 16), g_tx[g_txh].cls = unit_class(item), g_tx[g_txh].sect = 0, g_tx[g_txh].epoch = 0;
    g_txh = next;
}
static int hook_set(int on)
{
    uint32_t cur = rd32(SCA_PARAM_SLOT5), mine = (uint32_t)(uintptr_t)reveal_hook;
    if (on) {
        if (cur == mine) return g_hooked = 1;
        if (cur != SCA_PARAM_VF5) {
            snprintf(g_why_off, sizeof g_why_off, "cGenesisInvisibleItemParam slot 5 holds %08x (not 0x6DE990): hidden items are not mirrored", (unsigned)cur);
            slog("%s", g_why_off);
            return 0;
        }
        if (!wr_code32(SCA_PARAM_SLOT5, mine)) return 0;
        g_hooked = 1;
        slog("the hidden item scan result (cGenesisInvisibleItemParam slot 5, 0x6DE990) is wrapped: a reveal goes to the partner's PC in a network game");
        return 1;
    }
    if (cur == mine && wr_code32(SCA_PARAM_SLOT5, SCA_PARAM_VF5)) slog("the hidden item scan result is the stock one again");
    g_hooked = 0;
    return 1;
}
static void on_event(void *user, const cp_event *e)   /* the session's thread: only queued */
{
    (void)user;
    if (!e || e->kind != SC_EV_KIND || e->len != SC_MSG_LEN) return;
    EnterCriticalSection(&g_qcs);
    int next = (g_qh + 1) % SQ;
    if (next != g_qt) {
        memcpy(g_q[g_qh].b, e->body, SC_MSG_LEN), g_q[g_qh].t0 = GetTickCount();
        g_qh = next;
    } else g_qdrop++;
    LeaveCriticalSection(&g_qcs);
}
/* the param of a hidden item: a pointer in the item's own words whose object has the param vtable and points back at the item */
static uint32_t find_param(uint32_t item)
{
    for (uint32_t off = 0x30; off < 0x1400; off += 4) {
        uint32_t p = rd32(item + off);
        if (p >= 0x10000u && (p & 3) == 0 && rd32(p) == SCA_PARAM_VT && rd32(p + 8) == item) return p;
    }
    return 0;
}
static void net_pass(int mode, cs_t *cs, int me, uint32_t now)
{
    lk_t *k = net1_link_peek();
    if (k && g_reg_link != k) {
        lk_add_event_cb(k, on_event, NULL);
        g_reg_link = k;
    }
    if (!net1_session_active()) {
        EnterCriticalSection(&g_qcs);
        g_qh = g_qt = 0;
        LeaveCriticalSection(&g_qcs);
        g_txt = g_txh;
        sc_have_reset(&HV);
        return;
    }
    uint8_t ep = net_epoch();
    sc_seen_epoch(&SEEN, ep);
    uint32_t own = me == 0 || me == 1 ? unit_of_slot((unsigned)me) : 0;
    int level_ok = mode == SC_MODE_NET && own && unit_in_play(own) && bag_of(own);   /* this PC's level runs: its own human is placed and updated */
    uint32_t sect = level_ok ? current_section() : 0;
    if (!sect) level_ok = 0;
    /* HAVE: a human of this PC holds a Genesis in this level instance -> the partner's rule G may give its humans one */
    if (cs && sc_have_should_send(&HV, ep, g_local_source && g_src_epoch == ep, level_ok)) {
        sc_msg m;
        memset(&m, 0, sizeof m);
        m.type = SC_M_HAVE, m.epoch = ep, m.sect = sect, m.cls = SC_CLASS_SCANNER, m.from_slot = (uint8_t)(me == 1);
        uint8_t b[SC_MSG_LEN];
        unsigned n = sc_enc(b, sizeof b, &m);
        if (n && cs_send_event(cs, SC_EV_KIND, b, n)) {
            sc_have_sent(&HV, ep);
            slog("a human of this PC has the Genesis: told the partner (epoch %u, section %08x)", ep, (unsigned)sect);
        }
    }
    /* local reveals -> the partner */
    while (g_txt != g_txh) {
        int i = g_txt;
        g_txt = (g_txt + 1) % SQ;
        if (!sc_seen_add(&SEEN, g_tx[i].k)) {
            S.reveal_dup_local++;
            continue;
        }
        sc_msg m;
        memset(&m, 0, sizeof m);
        m.type = SC_M_REVEAL, m.epoch = ep, m.sect = sect, m.cls = g_tx[i].cls, m.from_slot = (uint8_t)(me == 1);
        memcpy(m.k, g_tx[i].k, 16);
        uint8_t b[SC_MSG_LEN];
        unsigned n = sc_enc(b, sizeof b, &m);
        if (cs && n && cs_send_event(cs, SC_EV_KIND, b, n)) {
            S.reveal_sent++;
            slog("hidden item class %08x key %08x/%08x/%u/%u revealed by this PC's scan: sent to the partner (epoch %u, section %08x)", (unsigned)m.cls, (unsigned)m.k[0],
                 (unsigned)m.k[1], (unsigned)m.k[2], (unsigned)m.k[3], ep, (unsigned)sect);
        } else {
            S.reveal_send_fail++;
            slog("hidden item reveal NOT sent (no session)");
        }
    }
    /* the partner's reveals -> here */
    EnterCriticalSection(&g_qcs);
    int count = (g_qh - g_qt + SQ) % SQ;
    LeaveCriticalSection(&g_qcs);
    for (int i = 0; i < count; i++) {
        uint8_t b[SC_MSG_LEN];
        uint32_t t0 = 0;
        EnterCriticalSection(&g_qcs);
        int have = g_qt != g_qh;
        if (have) memcpy(b, g_q[g_qt].b, SC_MSG_LEN), t0 = g_q[g_qt].t0, g_qt = (g_qt + 1) % SQ;
        LeaveCriticalSection(&g_qcs);
        if (!have) break;
        sc_msg m;
        S.rx++;
        if (!sc_dec(b, SC_MSG_LEN, &m)) {
            S.rx_bad++;
            continue;
        }
        if (m.type == SC_M_HAVE) {
            int8_t age = (int8_t)(uint8_t)(m.epoch - ep);
            if (age > 0 && now - t0 < 60000u) {   /* the partner is a level ahead: kept until this PC is there */
                EnterCriticalSection(&g_qcs);
                int next = (g_qh + 1) % SQ;
                if (next != g_qt) memcpy(g_q[g_qh].b, b, SC_MSG_LEN), g_q[g_qh].t0 = t0, g_qh = next;
                LeaveCriticalSection(&g_qcs);
            } else if (age == 0) {
                int was = sc_have_source(&HV, ep);
                sc_have_remote(&HV, m.epoch);
                if (!was) slog("the partner's PC has the Genesis in this level instance (epoch %u): this PC's humans get one too", m.epoch);
                g_last_give = 0;   /* rule G at the next pass */
            }
            continue;
        }
        uint32_t sa = rd32(SCA_SARRANGE_PTR), item = 0;
        wh_entry e;
        int found = level_ok && sa && wh_find_key4(&MEM, sa, WH_LIST_ITEMS, m.k, &e) && e.unit && unit_class(e.unit) == m.cls;
        if (found) item = e.unit;
        int newer = (int8_t)(uint8_t)(m.epoch - ep) > 0;
        int d = sc_rx_decide(&SEEN, &m, level_ok && !newer, ep, sect, found, found && rd8(item + SC_ITEM_VISIBLE));
        if (d == SC_RX_WAIT) {
            if (now - t0 < 60000u) {   /* this PC's level is not in play yet: the message waits (at most a minute) */
                EnterCriticalSection(&g_qcs);
                int next = (g_qh + 1) % SQ;
                if (next != g_qt) memcpy(g_q[g_qh].b, b, SC_MSG_LEN), g_q[g_qh].t0 = t0, g_qh = next;
                LeaveCriticalSection(&g_qcs);
                S.rx_wait++;
            } else S.rx_drop[SC_RX_WAIT]++;
            continue;
        }
        if (d != SC_RX_APPLY) {
            S.rx_drop[d & 7]++;
            if (d != SC_RX_DROP_DUP) slog("the partner's reveal of class %08x key %08x/%08x/%u/%u not applied here (%s)", (unsigned)m.cls, (unsigned)m.k[0], (unsigned)m.k[1],
                                          (unsigned)m.k[2], (unsigned)m.k[3], d == SC_RX_DROP_EPOCH ? "an older level instance" : d == SC_RX_DROP_SECT ? "another section" :
                                          d == SC_RX_DROP_MISSING ? "no such item here (taken / not placed)" : "already visible here");
            sc_seen_add(&SEEN, m.k);
            continue;
        }
        sc_seen_add(&SEEN, m.k);
        uint32_t p = find_param(item);
        if (p) {
            ((param5_fn)(uintptr_t)SCA_PARAM_VF5)((void *)(uintptr_t)p);   /* the stock result itself (no hook: no echo) */
            S.rx_param++;
        } else {
            ((reveal_fn)(uintptr_t)SCA_REVEAL_ITEM)((void *)(uintptr_t)item, 1);
        }
        S.rx_applied++;
        slog("the partner's scan revealed item %08x class %08x here (%s; visible %u)", (unsigned)item, (unsigned)m.cls, p ? "its param's stock result" : "FUN_005DAAB0(item, 1)",
             rd8(item + SC_ITEM_VISIBLE));
    }
}

/* ---------------------------------------------------------------- X: the split screen exchange */
static int second_human_slot(void)   /* the character played on pad slot 1 (-1 = not bound) */
{
    capture_state_t st;
    capture_get_state(&st);
    if (!st.role[0].enabled || !st.role[1].enabled || st.role[0].pad == st.role[1].pad) return -1;
    return st.role[0].pad == 1 ? 0 : st.role[1].pad == 1 ? 1 : -1;
}
static int p2_holds_genesis_button(void)
{
    uint32_t pd = pad_data(1);
    unsigned scheme = pd ? rd32(pd + 0x10) : 99, mask = scheme == 0 ? 0x0100u : scheme == 1 ? 0x0200u : 0;   /* FUN_0088BA50: LB scheme 0, RB scheme 1 */
    unsigned char gp[12];
    memset(gp, 0, sizeof gp);
    if (!mask || !testmode_pad_peek(1, gp)) return 0;
    return (*(unsigned short *)gp & mask) != 0;
}
int ALIGNED scan1_split_hook(int op)
{
    uint32_t now = GetTickCount();
    if (op == SPLIT_XG_WANT_ENTER) {
        if (!g_ok || !g_on || !g_x_on) return 0;
        int s2 = second_human_slot();
        if (s2 < 0) return 0;
        uint32_t u2 = unit_of_slot((unsigned)s2), u1 = unit_of_slot((unsigned)(1 - s2)), pd = pad_data(1);
        sc_enter_in in;
        memset(&in, 0, sizeof in);
        in.split_ok = 1;
        in.p2_present = u2 && unit_in_play(u2);
        in.p2_has = bag_has_genesis(bag_of(u2));
        in.p2_allowed = pd && (rd32(pd + 8) & 0x10u);
        /* the hold button of control schemes 0 / 1 (FUN_0088BA50), or the Genesis already in his hand (schemes 2 / 3 / 5 toggle it like a weapon with D-pad up,
         * FUN_00888780 -> FUN_006AF2D0 slot 0xE) */
        uint32_t b2 = bag_of(u2);
        in.p2_hold = p2_holds_genesis_button() || (b2 && rd32(b2 + SC_BAG_CUR) == SC_GENESIS_INDEX);
        in.p1_busy = (u1 && unit_in_genesis(u1)) || gui_running(scanner_gui());
        in.now = now, in.last_end = g_x_last_end, in.ever_ended = g_x_ended;
        int want = sc_want_enter(&in);
        if (!want && in.p2_present && in.p2_has && in.p2_allowed && in.p2_hold && in.p1_busy) {
            static uint32_t last_busy_log;
            S.x_refused_busy++;
            if (now - last_busy_log > 3000u) last_busy_log = now, slog("the second human holds the Genesis button, but the first human is using the scanner (one scanner): he waits");
        }
        if (want) g_x_slot = s2;
        return want;
    }
    if (op == SPLIT_XG_BEGIN) {
        sc_x_begin(&X, now);
        uint32_t gui = scanner_gui();
        int story = gui ? rd8(gui + SC_GUI_STORY) == 1 : sc_rate_shared(current_section());
        int rate = rate_read(), latch = gui ? rd8(gui + SC_GUI_LATCH) : 0, nr = rate, nl = latch;
        if (rate >= 0) {
            sc_gauge_begin(&GA, story, gui, rate, latch, &nr, &nl);
            if (nr != rate) rate_write(nr);
            if (gui && nl != latch) wr8(gui + SC_GUI_LATCH, (uint8_t)nl);
        }
        S.x_enters++;
        slog("split: the second human (character %d) uses the Genesis: whole view, he is the own human until he stops (rate %d -> %d%s, herb latch %d -> %d)", g_x_slot, rate, nr,
             story ? ", 1-2: the team's rate" : ", his own rate", latch, nl);
        return 1;
    }
    if (op == SPLIT_XG_WANT_LEAVE) {
        uint32_t u2 = g_x_slot >= 0 ? unit_of_slot((unsigned)g_x_slot) : 0;
        sc_leave_in in;
        memset(&in, 0, sizeof in);
        in.p2_present = u2 != 0;
        in.p2_genesis = u2 && unit_in_genesis(u2);
        in.p2_view = u2 && (rd32(u2 + SC_U_F44) & 0x20000u);
        in.gui_open = gui_running(scanner_gui());
        uint32_t root = rd32(SCA_CAMROOT_PTR), m0 = root ? rd32(root + 0xCE0) : 0, v0 = root ? rd32(root + 0x30 + 4) : 0;
        in.story_camera = root && m0 && v0 && v0 != m0;
        if (in.story_camera) slog("split: a cutscene / event camera took viewport 0 during the second human's Genesis use: the exchange ends now");
        return sc_x_want_leave(&X, &in, now);
    }
    if (op == SPLIT_XG_END) {
        uint32_t gui = scanner_gui();
        int rate = rate_read(), latch = gui ? rd8(gui + SC_GUI_LATCH) : 0, nr = rate, nl = latch;
        if (rate >= 0) {
            sc_gauge_end(&GA, gui, rate, latch, &nr, &nl);
            if (nr != rate) rate_write(nr);
            if (gui && nl != latch) wr8(gui + SC_GUI_LATCH, (uint8_t)nl);
        }
        /* FUN_006AEF60 hides the human from view 0 (unit +0xC bit 16) while his scan view is open and shows him again when it closes; an exchange that ended
         * with the view still open (the guard) would leave him invisible in the first human's half */
        uint32_t u2 = g_x_slot >= 0 ? unit_of_slot((unsigned)g_x_slot) : 0, f2 = u2 ? rd32(u2 + 0xC) : 0;
        if (u2 && !gui_running(gui) && !(f2 & 0x10000u) && wr32(u2 + 0xC, f2 | 0x10000u)) slog("split: the second human was still hidden from view 0 (scan view): shown again");
        X.on = 0, g_x_last_end = now, g_x_ended = 1;
        S.x_leaves++;
        slog("split: the second human's Genesis use is over: split view again (rate %d -> %d, his own kept %d; latch %d -> %d)", rate, nr, GA.p2_rate, latch, nl);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- R */
int ALIGNED scan1_rate_shared_now(void)
{
    S.rate_asks++;
    if (!g_inited || !g_on || !g_ok) return 1;   /* the module is off: the old behaviour (one value everywhere) */
    int sh = sc_rate_shared(g_sect);
    S.rate_shared += sh;
    return sh;
}

/* ---------------------------------------------------------------- pass */
static int check_bytes(char *why, int size)
{
    for (unsigned i = 0; i < sizeof SIG / sizeof SIG[0]; i++) {
        uint8_t cur[12];
        if (!r1_safe_read((const void *)(uintptr_t)SIG[i].va, cur, 12) || memcmp(cur, SIG[i].b, 12)) {
            snprintf(why, (size_t)size, "the bytes of %s at %08x differ from build 13298487", SIG[i].what, (unsigned)SIG[i].va);
            return 0;
        }
    }
    return 1;
}
/* LAB ONLY (default off, `scan guard 1`): sc1s3 / sc1s4 live: the lab console's `tp` of a 1-2 sample (uOMWaif, a 3-state unit) runs FUN_006BEF50 (a unit's
 * place status) whose ChangeState(5) is beyond that unit's state array ([mgr+0x34]) -> the engine's assert (sc1s3) or a broken sample (sc1s4). The two
 * ChangeState calls of FUN_006BEF50 can go through this guard: a request beyond the array is dropped and logged; every other one is the stock call. The
 * product never teleports such units, so the product build leaves the calls stock. */
static volatile LONG g_chg_dropped;
static int g_chg_on, g_chg_req;
static unsigned TC ALIGNED chg_guard(void *mgr, unsigned id, unsigned a, unsigned b)
{
    uint32_t m = (uint32_t)(uintptr_t)mgr, arr = m ? rd32(m + 0x30) : 0, n = m ? rd32(m + 0x34) : 0;
    if (m && arr && n && id >= n) {
        LONG k = InterlockedIncrement(&g_chg_dropped);
        if (k <= 20) slog("a place-status state change to %u was beyond the unit's %u states (manager %08x, exchange %s): dropped (the engine would assert)", id, n, (unsigned)m,
                          X.on ? "on" : "off");
        return 0;
    }
    return ((chg_fn)(uintptr_t)SCA_CHGSTATE)(mgr, id, a, b);
}
static int chg_site_set(uint32_t site, int on)
{
    uint8_t cur[5], want[5];
    if (!r1_safe_read((const void *)(uintptr_t)site, cur, 5) || cur[0] != 0xE8) return 0;
    uint32_t to = on ? (uint32_t)(uintptr_t)chg_guard : SCA_CHGSTATE, rel = to - (site + 5u);
    uint32_t now = site + 5u + (uint32_t)(cur[1] | cur[2] << 8 | cur[3] << 16 | (uint32_t)cur[4] << 24);
    if (now != SCA_CHGSTATE && now != (uint32_t)(uintptr_t)chg_guard) return 0;   /* someone else's call there: left alone */
    want[0] = 0xE8, want[1] = (uint8_t)rel, want[2] = (uint8_t)(rel >> 8), want[3] = (uint8_t)(rel >> 16), want[4] = (uint8_t)(rel >> 24);
    return memcmp(cur, want, 5) == 0 || wr_code(site, want, 5);
}
static void chg_guard_set(int on)
{
    int a = chg_site_set(SCA_PLACE_CALL1, on), b = chg_site_set(SCA_PLACE_CALL2, on);
    g_chg_on = on && a && b;
    slog("the place-status ChangeState calls (0x6BF0DB, 0x6BF16C) %s", on ? (a && b ? "go through the range guard" : "could NOT be guarded (bytes differ)") : "are the stock calls again");
}

/* split: while the FIRST human scans in his half (no exchange), the GUI's screen-filter units (+0x2D0 "game\filter\scanner", +0x318, +0x31C: unit flags 0xC00 |
 * 0x4000 while the scan view is open) tint the whole screen (sc1s1 live: the second human's half turned green). The view bits of a unit are +0xC bits 16..25
 * (bit 16 + k = drawn in viewport k): bit 18 (the second half) is cleared while that scan runs and put back after it. */
static void fmask_pass(int mode)
{
    uint32_t gui = scanner_gui();
    int want = g_fmask && mode == SC_MODE_SPLIT && !X.on && gui && gui_running(gui) && split_is_active();
    static const uint32_t offs[3] = {0x2D0u, 0x318u, 0x31Cu};
    for (int i = 0; i < 3; i++) {
        uint32_t u = gui ? rd32(gui + offs[i]) : 0;
        if (want && u && !g_fm_unit[i]) {
            uint32_t f = rd32(u + 0xC);
            if (f & 0x40000u) {
                g_fm_unit[i] = u, g_fm_bits[i] = 1;
                wr32(u + 0xC, f & ~0x40000u);
                g_fm_sets++;
            }
        } else if (!want && g_fm_unit[i]) {
            uint32_t v = g_fm_unit[i];
            if (v == u) wr32(v + 0xC, rd32(v + 0xC) | (g_fm_bits[i] ? 0x40000u : 0));   /* the same unit still: its bit back (a new GUI after a load has its own) */
            g_fm_unit[i] = 0;
        }
    }
}
static void ALIGNED scan_frame(void)   /* lt.c calls it from its stub without a realign */
{
    if (!g_inited) return;
    if (!g_checked) {
        g_checked = 1;
        g_ok = check_bytes(g_why_off, sizeof g_why_off);
        if (!g_ok) slog("OFF: %s", g_why_off);
        else if (g_on) hook_set(1);
        if (g_ok) split_set_genesis_hook(scan1_split_hook);
    }
    if (!g_ok || !g_on) return;
    uint32_t now = GetTickCount();
    cs_t *cs = NULL;
    int me = -1, mode = coop_mode(&cs, &me);
    if (unit_of_slot(0) || unit_of_slot(1)) {
        uint32_t sc = current_section();
        if (sc) g_sect = sc;
    }
    give_pass(mode, now, 0);
    net_pass(mode, cs, me, now);
    fmask_pass(mode);
}

int scan1_init(void)
{
    if (g_inited) return g_on;
    char v[8];
    InitializeCriticalSection(&g_qcs);
    sc_give_init(&G);
    memset(&X, 0, sizeof X), memset(&GA, 0, sizeof GA), memset(&SEEN, 0, sizeof SEEN), memset(&HV, 0, sizeof HV);
    if (r1_env("REV1_SCAN", v, sizeof v) && v[0] == '0') g_on = 0;
    g_inited = 1;
    lt_add_frame_cb(scan_frame);
    r1_log("scan: ready (%s); the Genesis for the second player: give, split exchange, hidden items in the network game", g_on ? "on" : "off: REV1_SCAN=0");
    return g_on;
}

/* ---------------------------------------------------------------- console */
static int who_line(char *out, int size, int slot)
{
    uint32_t u = unit_of_slot((unsigned)slot), b = bag_of(u), st = u ? rd32(u + SC_U_UPPER) : 0, pd = pad_data((unsigned)slot), w = b ? rd32(b + SC_BAG_GENESIS) : 0;
    char cn[96] = "-";
    if (st) classes_object_name(st, cn, sizeof cn);
    return snprintf(out, (size_t)size, "\n  slot %d unit %08x in play %d bag %08x +3c %08x (class %08x) cur %d upper %s (vt %08x) F44 %08x | pad slot %d: data+8 %08x scheme %d",
                    slot, (unsigned)u, unit_in_play(u), (unsigned)b, (unsigned)w, (unsigned)(w ? unit_class(w) : 0), b ? (int)rd32(b + SC_BAG_CUR) : -1, cn,
                    (unsigned)(st ? rd32(st) : 0), (unsigned)(u ? rd32(u + SC_U_F44) : 0), slot, (unsigned)(pd ? rd32(pd + 8) : 0), pd ? (int)rd32(pd + 0x10) : -1);
}
void ALIGNED scan_cmd(const char *arg, char *out, int size)
{
    char w[16] = "";
    unsigned a = 0;
    int n = arg ? sscanf(arg, "%15s %x", w, &a) : 0;
    if (n < 1 || !strcmp(w, "status")) {
        cs_t *cs = NULL;
        int me = -1, mode = coop_mode(&cs, &me);
        uint32_t gui = scanner_gui();
        snprintf(out, (size_t)size,
                 "scan: %s, %s%s | mode %s (me %d) | give passes %u tries %u: given %u refused %u skipped %u, HAVE sent %u received %u (source here %d) | split exchange %s: %s (character %d), enters %u leaves %u, refused (scanner busy) %u, "
                 "second human's own rate %d | hidden items: hook %s, reveals here %u, sent %u (fail %u, dup %u), received %u (bad %u): applied %u (by the param %u), waited %u, "
                 "dropped epoch %u section %u dup %u missing %u visible %u stale %u | place-status guard %s, dropped %ld | rate shared asks %u (yes %u) | GUI %08x running %d story %d latch %d rate %d section %08x",
                 g_on ? "on" : "off", g_ok ? "bytes ok" : "OFF: ", g_ok ? "" : g_why_off, mode == SC_MODE_NET ? "network" : mode == SC_MODE_SPLIT ? "split" : "none", me, S.give_passes, S.give_tries,
                 G.gives, G.refused, G.skipped, HV.sent, HV.received, sc_have_source(&HV, net_epoch()), g_x_on ? "on" : "off", X.on ? "RUNNING" : "idle", g_x_slot, S.x_enters, S.x_leaves, S.x_refused_busy, GA.p2_rate,
                 g_hooked ? "on" : "off", S.reveal_hooked, S.reveal_sent, S.reveal_send_fail, S.reveal_dup_local, S.rx, S.rx_bad, S.rx_applied, S.rx_param, S.rx_wait,
                 S.rx_drop[SC_RX_DROP_EPOCH], S.rx_drop[SC_RX_DROP_SECT], S.rx_drop[SC_RX_DROP_DUP], S.rx_drop[SC_RX_DROP_MISSING], S.rx_drop[SC_RX_DROP_VISIBLE],
                 S.rx_drop[SC_RX_WAIT], g_chg_on ? "on" : "off", (long)g_chg_dropped, S.rate_asks, S.rate_shared, (unsigned)gui, gui_running(gui), gui ? rd8(gui + SC_GUI_STORY) : -1, gui ? rd8(gui + SC_GUI_LATCH) : -1,
                 rate_read(), (unsigned)current_section());
        return;
    }
    if (!strcmp(w, "who")) {
        uint32_t gui = scanner_gui(), gp = rd32(R1_GAMEPAD_PTR);
        int o = snprintf(out, (size_t)size, "scan who: own slot %d, second human (pad slot 1) = character %d, cur pad slot %d, GUI %08x running %d, rate %d", capture_own_effective(),
                         second_human_slot(), gp ? (int)rd32(gp + 0x870) : -1, (unsigned)gui, gui_running(gui), rate_read());
        for (int s = 0; s < 2 && o < size - 200; s++) o += who_line(out + o, size - o, s);
        if (gui && o < size - 200) {   /* the GUI's state machine (+0x280 state, +0x2C4 next), its targets (+0x2BC list) and hits (+0x310 list), the progress (+0x28C) */
            int nt = 0, nh = 0;
            for (uint32_t it = rd32(gui + 0x2BC), e = rd32(gui + 0x2C0); it && it != e && nt < 64; it = rd32(it + 8)) nt++;
            for (uint32_t it = rd32(gui + 0x310), e = rd32(gui + 0x314); it && it != e && nh < 64; it = rd32(it + 8)) nh++;
            o += snprintf(out + o, (size_t)(size - o), "\n  GUI state %d next %d targets %d hits %d progress %.2f gauge %.2f story %d latch %d weapon %08x", (int)rd32(gui + 0x280),
                          (int)rd32(gui + 0x2C4), nt, nh, (double)rdf(gui + 0x28C), (double)rdf(gui + 0x284), rd8(gui + SC_GUI_STORY), rd8(gui + SC_GUI_LATCH), (unsigned)rd32(gui + 0x2DC));
            static const uint32_t offs[3] = {0x2D0u, 0x318u, 0x31Cu};
            for (int i = 0; i < 3 && o < size - 120; i++) {
                uint32_t u = rd32(gui + offs[i]);
                char cn[64] = "-";
                if (u) classes_object_name(u, cn, sizeof cn);
                o += snprintf(out + o, (size_t)(size - o), " | filter +%x %08x %s flags %08x", (unsigned)offs[i], (unsigned)u, cn, (unsigned)(u ? rd32(u + 0xC) : 0));
            }
        }
        {   /* camera manager 0: eye +0x40, target +0x60 (the lab puts a target ahead of the scanning human's view) */
            uint32_t root = rd32(SCA_CAMROOT_PTR), m0 = root ? rd32(root + 0xCE0) : 0;
            if (m0 && o < size - 120)
                o += snprintf(out + o, (size_t)(size - o), "\n  camera0 eye (%.0f,%.0f,%.0f) target (%.0f,%.0f,%.0f) | filter mask %s (%u sets)", (double)rdf(m0 + 0x40), (double)rdf(m0 + 0x44),
                              (double)rdf(m0 + 0x48), (double)rdf(m0 + 0x60), (double)rdf(m0 + 0x64), (double)rdf(m0 + 0x68), g_fmask ? "on" : "off", g_fm_sets);
        }
        return;
    }
    if (!strcmp(w, "items")) {   /* the hidden-item candidates of this PC's item list: class 0x8004xxxx with +0xF19 */
        uint32_t sa = rd32(SCA_SARRANGE_PTR);
        unsigned cnt = sa ? wh_list_count(&MEM, sa, WH_LIST_ITEMS) : 0;
        int o = snprintf(out, (size_t)size, "scan items: %u entries", cnt), shown = 0;
        for (unsigned i = 0; i < cnt && o < size - 160; i++) {
            wh_entry e;
            if (!wh_list_at(&MEM, sa, WH_LIST_ITEMS, i, &e) || !e.unit) continue;
            uint32_t cls = unit_class(e.unit);
            if (!sc_item_class(cls)) continue;
            uint32_t p = find_param(e.unit);
            if (!p && cls != 0x80041203u && rd8(e.unit + SC_ITEM_VISIBLE)) continue;   /* plain visible items are not listed (the Genesis-hidden class always is) */
            o += snprintf(out + o, (size_t)(size - o), "\n  %08x class %08x key %08x/%08x/%u/%u visible %u param %08x (state %d) pos (%.0f,%.0f,%.0f)", (unsigned)e.unit, (unsigned)cls,
                          (unsigned)e.k[0], (unsigned)e.k[1], (unsigned)e.k[2], (unsigned)e.k[3], rd8(e.unit + SC_ITEM_VISIBLE), (unsigned)p, p ? (int)rd32(p + 0x20) : -1,
                          (double)rdf(e.unit + 0x40), (double)rdf(e.unit + 0x44), (double)rdf(e.unit + 0x48));
            shown++;
        }
        if (!shown) snprintf(out + o, (size_t)(size - o), " (no hidden item)");
        return;
    }
    if (!strcmp(w, "on") || !strcmp(w, "off")) {
        g_on = !strcmp(w, "on");
        if (g_ok) hook_set(g_on);
        if (g_ok && !g_on && g_chg_on) chg_guard_set(0);
        snprintf(out, (size_t)size, "scan: %s", g_on ? "on" : "off (no give, no exchange, the rate shared everywhere as before, no reveal mirror)");
        return;
    }
    if (!strcmp(w, "guard")) {   /* LAB: the place-status range guard (for the lab's tp of odd units) */
        g_chg_req = n >= 2 ? a != 0 : !g_chg_req;
        if (g_ok) chg_guard_set(g_chg_req);
        snprintf(out, (size_t)size, "scan: LAB place-status guard %s", g_chg_on ? "on" : "off");
        return;
    }
    if (!strcmp(w, "fmask")) {   /* LAB: the first human's scan filter kept out of the second half (default on) */
        g_fmask = n >= 2 ? a != 0 : !g_fmask;
        snprintf(out, (size_t)size, "scan: the first human's scan filter %s", g_fmask ? "kept in his half (view bit 18 cleared while it runs)" : "stock (both halves)");
        return;
    }
    if (!strcmp(w, "x")) {
        g_x_on = n >= 2 ? a != 0 : !g_x_on;
        snprintf(out, (size_t)size, "scan: split exchange of the second human's Genesis %s", g_x_on ? "on" : "off (stock: no overlay for him)");
        return;
    }
    if (!strcmp(w, "give")) {
        cs_t *cs = NULL;
        int me = -1, mode = coop_mode(&cs, &me);
        G.tried_bag[0] = G.tried_bag[1] = 0;
        give_pass(mode, GetTickCount(), 1);
        snprintf(out, (size_t)size, "scan give: mode %d, given %u refused %u (see the log)", mode, G.gives, G.refused);
        return;
    }
    if (!strcmp(w, "p2rate")) {   /* LAB: the second split human's own rate (decimal) */
        int v = 0;
        sscanf(arg, "%*s %d", &v);
        GA.p2_rate = sc_clamp_rate(v);
        snprintf(out, (size_t)size, "scan: LAB the second human's own rate = %d", GA.p2_rate);
        return;
    }
    if (!strcmp(w, "reveal")) {   /* LAB: the stock scan result of a hidden item on this PC (as a finished scan would call it) */
        uint32_t item = a, p = item ? find_param(item) : 0;
        if (!item || !sc_item_class(unit_class(item))) {
            snprintf(out, (size_t)size, "scan reveal: REFUSED (%08x is not an item)", (unsigned)item);
            return;
        }
        int queued = 0;
        if (p) {
            void (TC *f)(void *) = (void (TC *)(void *))(uintptr_t)rd32(rd32(p) + 5u * 4u);   /* through the vtable: our hook sends it like a real scan */
            f((void *)(uintptr_t)p);
        } else {   /* no param found from the item: the stock reveal, then the hook's own queueing (what reveal_hook does after the stock result) */
            uint32_t k[4];
            ((reveal_fn)(uintptr_t)SCA_REVEAL_ITEM)((void *)(uintptr_t)item, 1);
            int next = (g_txh + 1) % SQ;
            if (net1_session_active() && item_key(item, k) && next != g_txt) {
                memcpy(g_tx[g_txh].k, k, 16), g_tx[g_txh].cls = unit_class(item), g_txh = next;
                queued = 1;
            }
        }
        snprintf(out, (size_t)size, "scan reveal: LAB item %08x %s (visible %u)", (unsigned)item,
                 p ? "param slot 5 called (hooked)" : queued ? "FUN_005DAAB0(item, 1) + queued like the hook" : "FUN_005DAAB0(item, 1) (not queued: no session / no key)", rd8(item + SC_ITEM_VISIBLE));
        r1_log("scan: %s", out);
        return;
    }
    snprintf(out, (size_t)size, "scan [status] | who | items | on | off | x 0|1 | give | p2rate <n> (lab) | reveal <item hex> (lab)");
}
