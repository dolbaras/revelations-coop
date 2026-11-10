/*
 * menu.c - the "Co-Op Play" entry of the title menu and the co-op lobby (stage 6 of the Revelations 1 mod). Game glue around the pure model (lobby1.c), the drawing (overlay.c)
 * and the two engines that do the work (capture.c: who plays which character; split.c: two views).
 *
 * How the entry works (notes\stage6.md section 2, stage A):
 *   - the title menu is one class, uGUI_GeneralMenu (vtable 0x108BA68). Its update (vtable slot 8, 0x82FDC0) is wrapped by patching that one vtable slot: every frame the wrapper
 *     records the instance, its state machine (state [+0x3FC], 4 = waiting for input), menu type [+0x9A0], cursor and item count. No code is patched.
 *   - while the menu waits for input a one-line hint (overlay) says "F6 / Back button: Co-Op Play"; F6 or the BACK button of a gamepad opens the lobby.
 *   - while the lobby is open the wrapper does NOT call the stock update: the stock menu is frozen under the opaque lobby overlay, so no key or button of the lobby reaches it.
 *     (Input is read by this module: the keyboard like the Revelations 2 mod did, gamepads through the proxy's XInput view.)
 *   - closing the lobby stores the settings (rev1_coop.ini next to the game's config.ini) and applies them: capture roles, own slot, split layout, split on / off, pad exchange.
 *     The player then chooses Continue / Load Game / New Game in the stock menu: the save decides the section and its pair of characters.
 * Raid is never touched: capture.c and split.c stop by themselves in a Raid scene, and the lobby is only reachable from the title menu.
 */
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture.h"
#include "gmdtext.h"
#include "lobby1.h"
#include "lobby_link.h"
#include "net1.h"
#include "overlay.h"
#include "r1.h"
#include "rolebarrier1.h"
#include "split.h"
#include "world1_game.h"   /* ep1-solo: solo1_watch_now */
#include "coop_steam.h"   /* rel010: the friend row */

/* ---------------------------------------------------------------- addresses (build 13298487) */
#define GM_VTABLE 0x108BA68u        /* uGUI_GeneralMenu vtable */
#define GM_VF8_SLOT 8
#define GM_VF8_ORIG 0x82FDC0u       /* uGUI_GeneralMenu::vf8 (update) */
#define GM_STATE 0x3FC              /* state machine; 4 = waiting for input */
#define GM_SUB 0x400
#define GM_CURSOR 0x408
#define GM_SHORTCUT 0x40C
#define GM_COUNT 0x410
#define GM_TYPE 0x9A0               /* menu type: 2 = campaign sub menu, 3 = main menu, 4 = single item */
#define MGR_VTABLE 0x108CA30u       /* uTitleMenuManager vtable: the manager drives the pages (cursor, activation, back); its update is slot 8 */
#define MGR_VF8_ORIG 0x841440u
#define GM_NODES 0xAB4              /* seven shortcut GUI nodes */
#define GM_MSGS 0xAD4

static unsigned g_vtable = GM_VTABLE, g_expect_orig = GM_VF8_ORIG;   /* the offline harness replaces them */
static unsigned g_mgr_vtable = MGR_VTABLE, g_mgr_expect = MGR_VF8_ORIG;
void menu_test_set_addrs(unsigned vtable, unsigned expect_orig) { g_vtable = vtable, g_expect_orig = expect_orig; }
void menu_test_set_mgr(unsigned vtable, unsigned expect_orig) { g_mgr_vtable = vtable, g_mgr_expect = expect_orig; }

static unsigned rd32(unsigned a)
{
    unsigned v = 0;
    r1_safe_read((const void *)a, &v, 4);
    return v;
}

/* ---------------------------------------------------------------- state */
static int g_installed, g_disabled;
static volatile unsigned g_gm_this, g_gm_tick;
static volatile int g_gm_state, g_gm_type = -1, g_gm_sub, g_gm_cursor, g_gm_count;
static unsigned g_gm_calls;
static int g_idle_state = 4;               /* [+0x3FC] value that means "waiting for input" (confirmed or corrected by the first live run: menu idle <n>) */
static int g_type_filter = -1;             /* menu type the hint / F6 are limited to (-1 = any title menu page) */
static volatile int g_open;                /* lobby open: the stock update is skipped */
static volatile int g_closing;             /* the lobby was just closed: the stock update stays frozen until every key / button is up (the closing key must not reach the stock menu: Esc went back to the title screen) */
static unsigned g_close_ms, g_quiet;
static l1_t *g_lobby;
static unsigned g_autoopen_wait;   /* rel010: an auto-join is waiting for the title to open the co-op screen (tick | 1, 0 none) */
static l1_settings g_set = {0, L1_LEAD, L1_LAYOUT_SIDE, 0};
static int g_lang = -1, g_lang_override = -1, g_lang_src;   /* g_lang_src: 0 default, 1 env REV1_LANG, 2 ini / console, 3 system language, 4 the game's own (detect_lang) */
static int g_stock_loaded = -1;
static l1_input g_in;
static unsigned g_opened_ms;
static char g_ini_path[MAX_PATH];
static unsigned g_applies, g_opens, g_events;
static int g_applied_armed = -1;
static char g_last_apply[160];

static CRITICAL_SECTION g_lock;
/* network rows (the product default since lobby-must; REV1_NETROWS=0 = the old local screen): the Mode row offers Network: host / join, then address / Start / Disconnect / ready.
 * The link is lobby_link (the coop session on the "enet" or "steam" transport), created at the first Start. */
static int g_net_rows;                     /* env.net_rows: set in menu_install, 1 unless REV1_NETROWS=0 */
static CRITICAL_SECTION g_chl;             /* g_typed: the window thread (WM_CHAR) and the game thread */
static l1_typed g_typed;                   /* characters typed in the game window, taken by menu_frame */
static WNDPROC g_chr_prev;                 /* the game window procedure that the WM_CHAR watch calls next */
static HWND g_chr_hwnd;                    /* the window the WM_CHAR watch is installed on (once per window) */
static ov_scene g_scene;
static unsigned g_scene_tick;
/* lobby-must: the network game around the lobby (menu_frame "session watch"): the guest's title menu is held after START until the host's story start
 * arrives (decides are swallowed, F6 / the native row still open the lobby), the in-game strip, the end of the session back at the title, the guest's
 * return to the title when the host is gone */
static volatile int g_hold;                /* guest after START, no level yet: the stock title menu takes no decide */
static unsigned g_hold_logged;
static int g_played;                       /* this PC has been in a level since the session started */
static int g_role_seen = -1;                /* rev1 live 16: this PC's role seen since the START (before its first level): a change = the co-op save's roles put back */
static unsigned g_title_idle_since;        /* the title menu waits for input since (0 = not) */
static int g_seen_ended = -1, g_seen_gone = -1;   /* the link's counters already shown */
static unsigned g_ep_seen;
static unsigned g_ep1_seen;                /* ep1-flow E5: the last close at the end of Episode 1 already shown */
static int g_strip_kind;                   /* L1G_* shown now, until g_strip_until */
static int g_strip_flags;                  /* L1GF_* of an L1G_READY_GATE strip (refreshed while it shows) */
static unsigned g_gate_holds, g_gate_logged;   /* ready1: Campaign decides held by the ready gate */
static char g_strip_name[40];
static unsigned g_strip_until;
static unsigned g_quit_at;                 /* the guest returns to the title at this time (the host is gone; 0 = not planned) */
static int g_quit_auto = 1;                /* REV1_LOSTQUIT=0: the strip only, no automatic return to the title */
static unsigned g_alone_since;             /* host: waiting for a partner since */
static unsigned g_stop_by_title;           /* the session was ended by the return to the title (count) */
static unsigned g_own_stop_until;          /* our own stop at the title: the link's "ended" / "gone" events until then are not shown (lm5: a second strip "lost") */

typedef void(__attribute__((fastcall)) * vf8_t)(void *self);
static vf8_t orig_vf8, orig_mgr_vf8;
static unsigned g_mgr_calls;

/* The title menu MANAGER (page logic: cursor movement, activation, back) reads the keys / buttons itself: freezing only the page's own update let the cursor moves and the closing Esc through
 * (live: the cursor ran to the last row, Esc went back to the title screen). Its update is frozen too while the lobby is open or just closed. */
static void *g_mgr_this;                   /* rev1 live 15: the title menu manager object (its [+0x68] = the uSaveFlowHD runner of the title's save reads) */
static unsigned g_mgr_tick;
static void __attribute__((fastcall)) hook_mgr_vf8(void *self)
{
    g_mgr_calls++;
    g_mgr_this = self, g_mgr_tick = GetTickCount();
    if (!g_open && !g_closing) orig_mgr_vf8(self);
}
/* rev1 live 15 (rolebarrier1_game.c, the guest's title save read): the title menu manager updated within the last 2 s, else 0 */
unsigned menu_title_mgr(void) { return g_mgr_this && GetTickCount() - g_mgr_tick < 2000u ? (unsigned)(uintptr_t)g_mgr_this : 0; }

/* ---------------------------------------------------------------- the native row (stage 6c)
 * The main page is built by the title menu manager with three calls of the page's AddItem (0x82A590: label message, bank, help message, bank, flags; the labels are the Title
 * messages 8 / 9 / 10 = menu_campaign / menu_coopmode / menu_option, bank 6) and keeps its rows in `row[count] = &record[count]` (+0x9B8), the text ids at +0x41C / +0x480. The 3DS
 * build had a fourth row "menu_gamestart" (Title message 11, help 15 "memo_gamestart"): both strings are "Invalid Message" in every language. This code (1) puts our label / help text
 * into the message table of the Title bank (the string pointers at [[res + 0x98] + 4 * id]) and (2) appends the row with the page's own AddItem, once per page object, before the
 * page lays its list out; (3) detours the page's decide handler 0x82BE80: the decide of that row opens the lobby, everything else is the stock code. (Writing count = 4 alone crashed in
 * stage 6: the row pointer of the fourth slot was never filled in.) REV1_NATIVEROW=0 switches it off. */
#define SGUI_PTR_VA 0x1182CA4u      /* the GUI singleton: [+0x108 + 4 * bank] = the message resource of a bank, bank 6 = Title */
#define TITLE_BANK 6
#define MSG_LABEL 11                /* Title message "menu_gamestart" */
#define MSG_HELP 15                 /* Title message "memo_gamestart" */
#define ADDITEM_MAIN_VA 0x82A590u   /* AddItem variant of the main page (record set 2 at +0xA64) */
#define ACT_VA 0x82BE80u            /* the page's decide handler: thiscall(page, cancel, mouse), ret 8, returns 1 when handled */
#define ACT_STEAL 6                 /* 53 56 8B F1 33 DB = push ebx; push esi; mov esi,ecx; xor ebx,ebx */
typedef void(__attribute__((thiscall)) * additem_t)(void *page, unsigned p2, unsigned p3, unsigned p4, unsigned p5, unsigned p6, unsigned p7, unsigned p8, unsigned p9);
typedef unsigned(__attribute__((fastcall)) * act_t)(void *page, void *edx, unsigned cancel, unsigned mouse);
static unsigned g_sgui_ptr_va = SGUI_PTR_VA;
static additem_t g_additem = (additem_t)ADDITEM_MAIN_VA;
static act_t g_act_tramp;
static int g_row_enabled = 1;
static volatile unsigned g_row_page;   /* the page object that got the row */
static int g_row_index = -1;           /* its index (3) */
static unsigned g_row_adds, g_row_acts;
static char g_row_label[128], g_row_help[192];

void menu_test_set_native(unsigned sgui_ptr_va, void *additem) { g_sgui_ptr_va = sgui_ptr_va, g_additem = (additem_t)additem, g_row_enabled = 1, g_row_page = 0; }

static void open_lobby(void);
static void close_lobby(const char *why);
static int detect_lang(void);

static int wr32m(unsigned a, unsigned v)
{
    if (IsBadWritePtr((void *)a, 4)) return 0;
    *(volatile unsigned *)a = v;
    return 1;
}
static float rdfm(unsigned a)
{
    unsigned v = rd32(a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static int wrfm(unsigned a, float f)
{
    unsigned v;
    memcpy(&v, &f, 4);
    return wr32m(a, v);
}

/* The page's rows are laid out by its own build code for the rows that exist when it runs: the node of a row added later stands at (0, 0) (live: the page had four items and showed three).
 * Rows are 30 units apart (y = 411 / 441 / 471 at x = 640 in the 1280x720 GUI space; the node keeps its position at +0x40 / +0x44 and the target of its animation at +0xA0 / +0xA4):
 * the new row gets the next place. */
static void native_row_place(unsigned s, int idx)
{
    unsigned n0 = rd32(s + 0xA64), n1 = rd32(s + 0xA64 + 8), nn = rd32(s + 0xA64 + 8 * (unsigned)idx);
    if (!n0 || !n1 || !nn) {
        r1_log("menu: native row: the row nodes are not there (%08x %08x %08x): not placed", n0, n1, nn);
        return;
    }
    float x = rdfm(n0 + 0x40), y0 = rdfm(n0 + 0x44), y1 = rdfm(n1 + 0x44), y = y0 + (y1 - y0) * (float)idx;
    wrfm(nn + 0x40, x), wrfm(nn + 0x44, y), wrfm(nn + 0xA0, x), wrfm(nn + 0xA4, y);
    r1_log("menu: native row placed at (%.0f, %.0f) (first row (%.0f, %.0f), spacing %.0f)", x, y, x, y0, y1 - y0);
}

/* the main page, every time it was (re)built: three rows with the label ids 8 / 9 / 10 (live: the page object stays, but its builder runs again after a visit to another page and resets
 * the count to three, so this is not once per object: the count of three IS the condition, four means the row is there) */
static void native_row_try(unsigned s)
{
    if (!g_row_enabled) return;
    if ((int)rd32(s + GM_TYPE) != 0 || (int)rd32(s + GM_COUNT) != 3) return;
    if (rd32(s + 0x41C) != 8 || rd32(s + 0x420) != 9 || rd32(s + 0x424) != 10) return;
    unsigned sg = rd32(g_sgui_ptr_va), res = sg ? rd32(sg + 0x108 + 4 * TITLE_BANK) : 0, tab = res ? rd32(res + 0x98) : 0, n = res ? rd32(res + 0x88) : 0;
    if (!tab || n <= MSG_HELP) {
        static int said;
        if (!said++) r1_log("menu: native row: the Title message table is not there yet (singleton %08x resource %08x table %08x count %u): later", sg, res, tab, n);
        return;
    }
    int lang = detect_lang();
    snprintf(g_row_label, sizeof g_row_label, "%s", l1_text(L1T_ROW_LABEL, lang));
    snprintf(g_row_help, sizeof g_row_help, "%s", l1_text(L1T_ROW_HELP, lang));
    if (!wr32m(tab + 4 * MSG_LABEL, (unsigned)(size_t)g_row_label) || !wr32m(tab + 4 * MSG_HELP, (unsigned)(size_t)g_row_help)) {
        r1_log("menu: native row: the message table at %08x is not writable", tab);
        g_row_page = s;
        return;
    }
    g_row_index = (int)rd32(s + GM_COUNT);
    int state = (int)rd32(s + GM_STATE);
    g_additem((void *)s, MSG_LABEL, TITLE_BANK, MSG_HELP, TITLE_BANK, 0, 1, 0, 0);
    native_row_place(s, g_row_index);
    g_row_page = s;
    g_row_adds++;
    r1_log("menu: native row added to the main page %08x (state %d, items now %d): label \"%s\", help \"%s\"", s, state, (int)rd32(s + GM_COUNT), g_row_label, g_row_help);
}

static void strip_show(int kind, const char *name, unsigned ms);

/* ready1 (2026-11-09): the ready gate (the owner: "Ready" as in Jericho, where the host's Start needs every seat ready). Here the story start is the title menu's Campaign
 * (Continue / New Game / Load Game). While a network link runs (the host waits or has a partner, the guest connects or is connected) and the session has not started (it
 * starts when both players are ready), the decide on the main page's Campaign row is held on both PCs and the strip says what is missing. Before this, a host could start the
 * story alone with the partner in the room, and the partner's ready later started the session in the middle of the host's level (the guest then waited for a story start
 * that had already happened). Returns 1 while the gate holds; *flags = L1GF_*, peer = the partner's name ("" unknown). */
static int ready_gate(int *flags, char *peer, size_t psize)
{
    lk_t *k = net1_link_peek();
    if (flags) *flags = 0;
    if (peer && psize) peer[0] = 0;
    if (!g_net_rows || !k || lk_mode(k) == L1N_OFF || !lk_active(k)) return 0;
    lk_info in;
    lk_get_info(k, &in);
    if (in.started) return 0;
    int f = (in.host ? L1GF_HOST : 0) | (lk_state(k) == L1NS_CONNECTED ? L1GF_CONNECTED : 0) | (in.ready ? L1GF_READY : 0) | (in.peer_ready ? L1GF_PEER_READY : 0);
    if (flags) *flags = f;
    if (peer && psize) snprintf(peer, psize, "%s", in.peer_name);
    return 1;
}
#define LABEL_CAMPAIGN 8            /* Title message "menu_campaign": the main page's first row */

static unsigned __attribute__((fastcall)) hook_act(void *page, void *edx, unsigned cancel, unsigned mouse)
{
    unsigned s = (unsigned)(size_t)page;
    if (g_row_enabled && s == g_row_page && (cancel & 0xFF) == 0 && (int)rd32(s + GM_STATE) == g_idle_state && (int)rd32(s + GM_CURSOR) == g_row_index && !g_open && !g_closing) {
        g_row_acts++;
        r1_log("menu: native row decided (%s): lobby opens", (mouse & 0xFF) ? "mouse" : "key / pad");
        open_lobby();
        return 1;   /* handled: the page stays in its waiting state, the stock manager sees nothing */
    }
    /* ready1: the ready gate on the Campaign row of the main page (its rows are the Title messages 8 / 9 / 10, with or without the native row) */
    if (!g_hold && (cancel & 0xFF) == 0 && (int)rd32(s + GM_STATE) == g_idle_state && rd32(s + 0x41C) == LABEL_CAMPAIGN && rd32(s + 0x420) == 9 && rd32(s + 0x424) == 10) {
        int cur = (int)rd32(s + GM_CURSOR), f = 0;
        char peer[40];
        if (cur >= 0 && cur < 8 && rd32(s + 0x41C + 4 * (unsigned)cur) == LABEL_CAMPAIGN && ready_gate(&f, peer, sizeof peer)) {
            unsigned now = GetTickCount();
            g_gate_holds++;
            g_strip_flags = f;
            strip_show(L1G_READY_GATE, peer, 7000);
            if (now - g_gate_logged > 2000)
                g_gate_logged = now, r1_log("menu: ready gate: Campaign is held until both players are ready (%s, %s, this PC %s, partner %s)", (f & L1GF_HOST) ? "host" : "guest",
                                            (f & L1GF_CONNECTED) ? "connected" : "no partner yet", (f & L1GF_READY) ? "ready" : "not ready", (f & L1GF_PEER_READY) ? "ready" : "not ready");
            return 1;
        }
    }
    if (g_hold && (cancel & 0xFF) == 0) {   /* the guest after START: the host's story start loads this PC; its own Continue / New Game / Load must not */
        unsigned now = GetTickCount();
        if (now - g_hold_logged > 2000) g_hold_logged = now, r1_log("menu: guest hold: a decide of the title menu (cursor %d) is ignored until the host starts the story", (int)rd32(s + GM_CURSOR));
        return 1;
    }
    return g_act_tramp ? g_act_tramp(page, edx, cancel, mouse) : 0;
}
unsigned menu_test_act(void *page, unsigned cancel, unsigned mouse, void *orig) { g_act_tramp = (act_t)orig; return hook_act(page, NULL, cancel, mouse); }

static int install_act_detour(void)
{
    static const unsigned char expect[ACT_STEAL] = {0x53, 0x56, 0x8B, 0xF1, 0x33, 0xDB};
    unsigned char cur[ACT_STEAL];
    if (!r1_safe_read((void *)(uintptr_t)ACT_VA, cur, ACT_STEAL) || memcmp(cur, expect, ACT_STEAL)) {
        r1_log("menu: native row: the decide handler's first bytes are not the expected ones: the row stays off");
        return 0;
    }
    unsigned char *t = (unsigned char *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return 0;
    memcpy(t, expect, ACT_STEAL);
    t[ACT_STEAL] = 0xE9;
    *(int *)(t + ACT_STEAL + 1) = (int)((ACT_VA + ACT_STEAL) - (unsigned)(uintptr_t)(t + ACT_STEAL + 5));
    g_act_tramp = (act_t)(uintptr_t)t;
    unsigned char patch[ACT_STEAL];
    patch[0] = 0xE9;
    *(int *)(patch + 1) = (int)((unsigned)(uintptr_t)hook_act - (ACT_VA + 5));
    patch[5] = 0x90;
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)ACT_VA, ACT_STEAL, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void *)(uintptr_t)ACT_VA, patch, ACT_STEAL);   /* one copy of whole instructions; the handler runs from the title menu's update only */
    VirtualProtect((void *)(uintptr_t)ACT_VA, ACT_STEAL, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)(uintptr_t)ACT_VA, ACT_STEAL);
    r1_log("menu: native row: decide handler %08x detoured (trampoline %p)", ACT_VA, (void *)t);
    return 1;
}

/* ---------------------------------------------------------------- the update wrapper */
static void __attribute__((fastcall)) hook_vf8(void *self)
{
    native_row_try((unsigned)self);   /* before the stock update: the row must exist when the page lays its list out */
    if (!g_open && !g_closing && !(g_hold && !g_act_tramp)) orig_vf8(self);   /* lobby open (or just closed, keys still down): frozen; the guest hold without the decide detour freezes too */
    unsigned s = (unsigned)self;
    g_gm_this = s;
    g_gm_state = (int)rd32(s + GM_STATE);
    g_gm_type = (int)rd32(s + GM_TYPE);
    g_gm_sub = (int)rd32(s + GM_SUB);
    g_gm_cursor = (int)rd32(s + GM_CURSOR);
    g_gm_count = (int)rd32(s + GM_COUNT);
    g_gm_calls++;
    g_gm_tick = GetTickCount();
}

/* ---------------------------------------------------------------- language, stock words
 * The texts follow the language the game itself shows: its sMain.mLanguage ([[0x11E0314] + 0x254C], set at 0x9BC7B5 from GetUserDefaultLangID; lang owner report 2026-11-08: a
 * Russian game with an English Co-Op Play row was a lab run with REV1_LANG=eng). Before the game has set it: the same rule on GetUserDefaultLangID, asked again later. */
#define SMAIN_PTR_VA 0x11E0314u
#define SMAIN_LANGUAGE 0x254C
static unsigned g_smain_ptr_va = SMAIN_PTR_VA;   /* the offline harness replaces it */
void menu_test_set_smain(unsigned smain_ptr_va) { g_smain_ptr_va = smain_ptr_va, g_lang = -1, g_lang_src = 0; }
static int detect_lang(void)
{
    static unsigned last_try;
    char v[32];
    if (g_lang >= 0 && (g_lang_src == 1 || g_lang_src == 2 || g_lang_src == 4)) return g_lang;
    if (g_lang >= 0 && GetTickCount() - last_try < 1000) return g_lang;   /* (not the game's own value yet: asked again once a second) */
    last_try = GetTickCount();
    int l = -1, src = 0;
    unsigned sm = g_smain_ptr_va ? rd32(g_smain_ptr_va) : 0, m = sm ? rd32(sm + SMAIN_LANGUAGE) : 0xFFFFFFFFu;
    if (r1_env("REV1_LANG", v, sizeof v) && (l = l1_lang_from_code(v)) >= 0) src = 1;   /* LAB: the mod's texts in that language, the game's stay as they are */
    else if (g_lang_override >= 0) l = g_lang_override, src = 2;
    else if (sm && (l = l1_lang_from_game(m)) >= 0) src = 4;
    else if ((l = l1_lang_from_langid(GetUserDefaultLangID())) >= 0) src = 3;
    else l = 0, src = 0;
    if (l != g_lang || src != g_lang_src)
        r1_log("menu: language %s (%s; the game's mLanguage %d)", l1_lang_codes[l], src == 1 ? "REV1_LANG, LAB: the game's own texts may differ" : src == 2 ? "rev1_coop.ini"
               : src == 4 ? "the game's own" : src == 3 ? "system language (GetUserDefaultLangID), the game's not set yet" : "default", sm ? (int)m : -1);
    g_lang = l, g_lang_src = src;
    return l;
}

/* the name this PC sends without a Steam persona, in its language (net1_name_for) */
static const char *fallback_name(int side) { return l1_text(side ? L1T_NAME_P2 : L1T_NAME_P1, detect_lang()); }

static void exe_dir(char *out, size_t size)
{
    char exe[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(out, size, "%s", exe);
}

static void load_stock(int lang)
{
    if (g_stock_loaded == lang) return;
    g_stock_loaded = lang;
    char dir[MAX_PATH], path[MAX_PATH + 64];
    exe_dir(dir, sizeof dir);
    char names[7][48];
    char words[4][64];
    memset(names, 0, sizeof names), memset(words, 0, sizeof words);
    int got_names = 0, got_words = 0;
    snprintf(path, sizeof path, "%s\\nativePC\\msg\\Coop_%s.gmd", dir, l1_lang_codes[lang]);
    gmd_t *g = gmd_load(path);
    if (g) {
        for (int i = 0; i < 7; i++) {
            char lab[24];
            snprintf(lab, sizeof lab, "char_name_%02d", i);
            const char *t = gmd_by_label(g, lab);
            if (t && t[0]) snprintf(names[i], sizeof names[i], "%s", t), got_names++;
        }
        gmd_free(g);
    }
    snprintf(path, sizeof path, "%s\\nativePC\\msg\\Title_%s.gmd", dir, l1_lang_codes[lang]);
    g = gmd_load(path);
    if (g) {
        static const char *const labs[4] = {"menu_continue", "menu_loadgame", "menu_newgame", "menu_campaign"};
        for (int i = 0; i < 4; i++) {
            const char *t = gmd_by_label(g, labs[i]);
            if (t && t[0] && !strchr(t, '<')) snprintf(words[i], sizeof words[i], "%s", t), got_words++;
        }
        gmd_free(g);
    }
    l1_set_stock_names((const char(*)[48])names);
    l1_set_stock_words((const char(*)[64])words);
    /* the Raid mode's online texts of the game (session full, connection terminated, <USER> has left / joined / is ready, removed, kick question): the network lobby and the strip quote them */
    /* ep1-flow: also the options menu's "Off" (option bank) and "Episode 1" (campaign bank): each id names its bank, the banks come in runs */
    int got_net = 0;
    const char *bank = NULL;
    g = NULL;
    for (int i = 0; i < L1K_COUNT; i++) {
        if (!bank || strcmp(bank, l1_stock_banks[i])) {
            if (g) gmd_free(g);
            bank = l1_stock_banks[i];
            snprintf(path, sizeof path, "%s\\nativePC\\msg\\%s_%s.gmd", dir, bank, l1_lang_codes[lang]);
            g = gmd_load(path);
        }
        const char *t = g ? gmd_by_label(g, l1_stock_labels[i]) : NULL;
        char clean[256];
        clean[0] = 0;
        if (t && t[0] && strcmp(t, "HARDUMMY") && strcmp(t, "Invalid Message")) l1_stock_clean(t, lang, clean, sizeof clean);
        l1_set_stock_text(i, clean);
        if (clean[0] && !strcmp(l1_stock(i), clean)) got_net++;
    }
    if (g) gmd_free(g);
    r1_log("menu: stock texts of the game (%s): %d names, %d menu words, %d of %d network texts", l1_lang_codes[lang], got_names, got_words, got_net, L1K_COUNT);
}

/* ---------------------------------------------------------------- settings file */
static void ini_path_init(void)
{
    if (r1_env("REV1_COOP_INI", g_ini_path, sizeof g_ini_path) && g_ini_path[0]) return;
    char la[MAX_PATH] = "";
    if (GetEnvironmentVariableA("LOCALAPPDATA", la, sizeof la))
        snprintf(g_ini_path, sizeof g_ini_path, "%s\\CAPCOM\\RESIDENT EVIL REVELATIONS\\rev1_coop.ini", la);
}

static int ini_enabled(void)
{
    FILE *f = fopen(g_ini_path, "rb");
    if (!f) return 1;
    char line[200];
    int on = 1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "enabled", 7)) {
            char *e = strchr(line, '=');
            if (e && atoi(e + 1) == 0) on = 0;
        }
    fclose(f);
    return on;
}

/* ---------------------------------------------------------------- applying the settings to the engines */
static void menu_apply(const char *why)
{
    l1_assign a;
    l1_assign_get(&g_set, &a);
    g_applies++;
    if (g_set.armed && (net1_session_active() || (net1_link_peek() && lk_mode(net1_link_peek()) != L1N_OFF))) {
        /* one mode: a network game never arms the local split (check 9 of net-role-barrier.md); the role barrier owns the capture roles then */
        split_enable(0);
        testmode_pad_swap_base(0);
        g_applied_armed = 0;
        snprintf(g_last_apply, sizeof g_last_apply, "split kept off (%s): a network link runs", why);
        r1_log("menu: co-op split NOT armed (%s): a network link is running (one human per PC)", why);
        return;
    }
    if (!g_set.armed && net1_session_active()) {
        /* wp12 (2026-11-08 02:52:53, the guest): the lobby closed after the START came here and its capture_unbind(-1) + capture_set_own(-1) took back the role
         * barrier's capture_set_own(1) (`guards: pickup own slot 1 -> 0`): the guest's human could pick up no item and open no door. In a started session the role
         * barrier owns the own slot and the capture roles (as in the armed branch above): only the split is kept off */
        split_enable(0);
        testmode_pad_swap_base(0);
        g_applied_armed = 0;
        snprintf(g_last_apply, sizeof g_last_apply, "off (%s): the network session's roles stay", why);
        r1_log("menu: co-op split off (%s): a network session runs, its own slot and capture roles stay as the role barrier set them", why);
        return;
    }
    if (!g_set.armed) {
        capture_unbind(-1);
        capture_set_own(-1);
        split_enable(0);
        testmode_pad_swap_base(0);
        g_applied_armed = 0;
        snprintf(g_last_apply, sizeof g_last_apply, "off (%s)", why);
        r1_log("menu: co-op OFF (%s)", why);
        return;
    }
    capture_unbind(-1);
    capture_set_own(a.unit1 == 0 ? -1 : a.unit1);   /* lead = the engine's own default (no own-slot hook needed), partner = own slot 1 before the level loads */
    capture_bind(a.unit1, a.pad1);
    capture_bind(a.unit2, a.pad2);
    split_set_layout(g_set.layout == L1_LAYOUT_TOPBOTTOM ? SPLIT_TOP_BOTTOM : SPLIT_SIDE_BY_SIDE);
    split_enable(1);
    testmode_pad_swap_base(a.swap);
    g_applied_armed = 1;
    snprintf(g_last_apply, sizeof g_last_apply, "on (%s): player 1 = unit slot %d on pad slot %d, player 2 = unit slot %d on pad slot %d, layout %s, pads %s", why, a.unit1, a.pad1, a.unit2,
             a.pad2, g_set.layout == L1_LAYOUT_TOPBOTTOM ? "top/bottom" : "side by side", a.swap ? "exchanged" : "normal");
    r1_log("menu: co-op %s", g_last_apply);
}

/* ---------------------------------------------------------------- input */
static int in_foreground(void)
{
    HWND w = testmode_window();
    return w && GetForegroundWindow() == w;
}

static int key_down(int vk, int fg)
{
    if (inject_vk_down(vk)) return 1;
    return fg && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void read_raw(l1_raw *raw, int *pads, int want_pads)
{
    memset(raw, 0, sizeof *raw);
    int fg = in_foreground();
    raw->up = (unsigned char)key_down(VK_UP, fg), raw->down = (unsigned char)key_down(VK_DOWN, fg), raw->left = (unsigned char)key_down(VK_LEFT, fg);
    raw->right = (unsigned char)key_down(VK_RIGHT, fg), raw->enter = (unsigned char)key_down(VK_RETURN, fg), raw->space = (unsigned char)key_down(VK_SPACE, fg);
    raw->esc = (unsigned char)key_down(VK_ESCAPE, fg), raw->f6 = (unsigned char)key_down(VK_F6, fg);
    int n = 0;
    for (int u = 0; u < 2 && want_pads; u++) {   /* polling an unplugged XInput user is slow: only while the title menu waits or the lobby is open */
        XINPUT_GAMEPAD gp;
        memset(&gp, 0, sizeof gp);
        if (testmode_pad_peek(u, &gp)) {
            raw->pad_buttons[u] = gp.wButtons, raw->lx[u] = gp.sThumbLX, raw->ly[u] = gp.sThumbLY;
            n++;
        }
    }
    *pads = n;
}

/* mouse: hover and click on the rows of the lobby (only while the game window is in front; the pointer is drawn by the overlay while the mouse moves) */
static int m_hover = -2, m_prev_btn, m_vx, m_vy;
static unsigned m_last_move;
static int mouse_events(l1_event *ev, int max, unsigned now)
{
    int n = 0;
    if (!in_foreground()) return 0;
    HWND w = testmode_window();
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !ScreenToClient(w, &pt) || !GetClientRect(w, &rc)) return 0;
    static POINT last_pt = {-1, -1};
    int row = overlay_row_at_client(pt.x, pt.y, rc.right, rc.bottom, &m_vx, &m_vy);
    if (pt.x != last_pt.x || pt.y != last_pt.y) m_last_move = now, last_pt = pt;
    int btn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (row != m_hover && now - m_last_move < 200) {
        m_hover = row;
        if (row >= 0 && n < max) ev[n].kind = L1E_HOVER, ev[n].row = row, n++;
    }
    if (btn && !m_prev_btn && row >= 0 && n < max) ev[n].kind = L1E_CLICK, ev[n].row = row, n++;
    m_prev_btn = btn;
    return n;
}

/* ---------------------------------------------------------------- typed characters and the network rows
 * The join address is typed: the game window gets WM_CHAR, which this module reads with a window procedure in front of the game's (once per window, network rows only).
 * The window procedure only queues the character (l1_typed_push: printable ASCII), the game thread takes it in menu_frame and hands it to the model as raw.typed. */
static LRESULT CALLBACK chr_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CHAR) {
        EnterCriticalSection(&g_chl);
        l1_typed_push(&g_typed, (unsigned)wp);
        LeaveCriticalSection(&g_chl);
    }
    return CallWindowProc(g_chr_prev, w, msg, wp, lp);
}

static void chars_watch(void)
{
    HWND hw = testmode_window();
    if (!g_net_rows || !hw || hw == g_chr_hwnd) return;
    WNDPROC prev = IsWindowUnicode(hw) ? (WNDPROC)SetWindowLongW(hw, GWL_WNDPROC, (LONG)chr_proc) : (WNDPROC)SetWindowLongA(hw, GWL_WNDPROC, (LONG)chr_proc);
    if (!prev) return;
    g_chr_prev = prev, g_chr_hwnd = hw;
    r1_log("menu: network rows: WM_CHAR of window %p is read for the join address", (void *)hw);
}

static int chars_take(unsigned char *out)
{
    EnterCriticalSection(&g_chl);
    int n = l1_typed_take(&g_typed, out);
    LeaveCriticalSection(&g_chl);
    return n;
}

/* the network requests of the lobby (L1R_NET_START / L1R_NET_STOP): Start hosts or joins with the settings (mode, typed address),
 * Disconnect closes the link. The link itself is the shared one of net1.c (the console `net` command drives the same link); the state the player
 * sees is the model's, set by the link (lobby_link.c) through the session's notifications, which net1_frame polls once a frame (lobby open or not). */
static void net_request(int req)
{
    if (!g_lobby) return;
    net1_attach_lobby(g_lobby);   /* creates nothing; a link that exists shows itself in the model */
    if (req == L1R_NET_START) {
        l1_settings s;
        l1_settings_get(g_lobby, &s);
        char err[160];
        if (s.way) net1_set_way("steam");   /* round 2: the Way row; a lab transport (sim) stays unless the Steam way is chosen */
        else if (!strcmp(net1_way(), "steam")) net1_set_way("ip");
        int ok = net1_start(s.net_mode, s.join_addr, LK_DEFAULT_PORT, err, sizeof err);
        r1_log("menu: network %s %s%s", s.net_mode == L1N_HOST ? "host" : "join", ok ? "started" : "failed: ", ok ? "" : err);
    } else if (req == L1R_NET_STOP) {
        net1_stop();
        r1_log("menu: network stopped");
    } else if (req == L1R_NET_KICK) {
        int ok = net1_kick();
        r1_log("menu: Remove partner: %s", ok ? "the partner was removed" : "nobody to remove");
    } else if (req == L1R_NET_INVITE) {   /* round 2: the Steam way's "Invite a friend" (coop_steam: the overlay's invite dialog for our lobby) */
        char out[300];
        net1_cmd("steaminvite", out, sizeof out);
        r1_log("menu: Invite a friend: %s", out);
    }
}

/* the lobby's request, once per call: close, or the network start / stop */
static void lobby_request(const char *why)
{
    int req = l1_take_request(g_lobby);
    if (req == L1R_CLOSE) close_lobby(why);
    else if (req == L1R_NET_START || req == L1R_NET_STOP || req == L1R_NET_KICK || req == L1R_NET_INVITE) net_request(req);
}

/* ---------------------------------------------------------------- scene for the overlay */
static void publish(const ov_scene *sc)
{
    EnterCriticalSection(&g_lock);
    if (sc) {
        g_scene = *sc;
        g_scene_tick = GetTickCount();
    } else
        g_scene.kind = OV_NONE;
    LeaveCriticalSection(&g_lock);
}

void menu_present(void *dev)
{
    if (!g_installed) return;
    static ov_scene sc;
    EnterCriticalSection(&g_lock);
    int ok = g_scene.kind != OV_NONE && GetTickCount() - g_scene_tick < 500;
    if (ok) sc = g_scene;
    LeaveCriticalSection(&g_lock);
    if (!ok) return;
    sc.tick_ms = GetTickCount();
    overlay_draw(dev, &sc);
}

void menu_before_reset(void)
{
    if (g_installed) overlay_release();
}

/* ---------------------------------------------------------------- environment of the lobby */
/* the area of this PC's save: the campaign checkpoint record of sCheckPoint ([0x116A334] + 0x60 + mode * 0x24, mode = [[[0x1169108]+0x24]+0x20]), +0x14 = the area Continue
 * loads (notes\net-story-re.md section 2, FUN_006D0FF0); 0 when the objects are not there */
static unsigned save_area(void)
{
    unsigned cp = rd32(0x116A334u), g = rd32(0x1169108u), o = g ? rd32(g + 0x24) : 0;
    if (!cp || !o) return 0;
    unsigned mode = rd32(o + 0x20);
    if (mode > 1) return 0;
    unsigned a = rd32(cp + 0x60 + mode * 0x24 + 0x14), ep = (a >> 16) & 0xFFu;
    /* lm1 live (main PC, a save the title showed as "Episode 1"): the record held 0x91FF0000 at the title = not filled yet (the save is read at Continue):
     * an episode byte 0 / 0xFF is "unknown", never a warning */
    return (a >> 24) == 0x91u && ep && ep != 0xFFu ? a : 0;
}

/* rel010: the friend row's list (Join + Steam friends): asked from Steam at most once a second, on the game thread */
static unsigned g_fr_at;
static int g_fr_n;
static char g_fr_name[L1_FRIENDS][40], g_fr_id[L1_FRIENDS][24];
static void friends_refresh(const l1_settings *s)
{
    unsigned now = GetTickCount();
    if (!s || s->net_mode != L1N_JOIN || !s->way) { g_fr_n = 0; return; }
    if (g_fr_at && now - g_fr_at < 1000) return;
    g_fr_at = now | 1;
    uint64_t ids[L1_FRIENDS];
    int n = cst_friends_in_game(ids, g_fr_name, L1_FRIENDS);
    for (int i = 0; i < n; i++) snprintf(g_fr_id[i], sizeof g_fr_id[i], "%llu", (unsigned long long)ids[i]);
    if (n != g_fr_n) r1_log("menu: friend row: %d friend(s) in the game with a lobby%s%s", n, n ? ", first " : "", n ? g_fr_name[0] : "");
    g_fr_n = n;
}

static void fill_env(l1_env *env, int pads)
{
    memset(env, 0, sizeof *env);
    env->lang = detect_lang();
    env->pads_connected = pads;
    env->net_rows = g_net_rows;
    net1_own_addr(env->own_addr, sizeof env->own_addr);
    lk_t *k = net1_link_peek();
    unsigned now = GetTickCount();
    if (k && lk_mode(k) == L1N_HOST && lk_state(k) == L1NS_WAIT) {
        if (!g_alone_since) g_alone_since = now | 1;
        int d = (int)(now - g_alone_since);   /* signed: since = now | 1 can be now + 1 */
        env->alone_s = d > 0 ? d / 1000 : 0;
    } else
        g_alone_since = 0;
    env->save_area = save_area();
    env->coop_role1 = coopsave_rec_role1();   /* rev1 live 16: the host's room says that Continue plays the co-op save's characters */
    l1_settings ls;
    if (g_lobby) l1_settings_get(g_lobby, &ls);
    friends_refresh(g_lobby ? &ls : NULL);
    env->n_friends = g_fr_n;
    for (int i = 0; i < g_fr_n && i < L1_FRIENDS; i++) {
        snprintf(env->friend_name[i], sizeof env->friend_name[i], "%s", g_fr_name[i]);
        snprintf(env->friend_id[i], sizeof env->friend_id[i], "%s", g_fr_id[i]);
    }
    capture_state_t cs;
    capture_get_state(&cs);
    char cn[2][80];
    int person[2] = {-1, -1};
    for (int i = 0; i < 2; i++) {
        cn[i][0] = 0;
        if (cs.role[i].unit) {
            if (classes_object_name(cs.role[i].unit, cn[i], sizeof cn[i])) person[i] = l1_person_from_class(cn[i]);
        }
    }
    if (person[0] >= 0 && person[1] >= 0) {   /* a live section: its real pair */
        snprintf(env->lead, sizeof env->lead, "%s", l1_person_name(person[0], env->lang));
        snprintf(env->partner, sizeof env->partner, "%s", l1_person_name(person[1], env->lang));
        env->section_known = 1;
    }
}

static void open_lobby(void)
{
    if (g_open) return;
    l1_env env;
    l1_raw raw;
    int pads;
    read_raw(&raw, &pads, 1);
    fill_env(&env, pads);
    load_stock(env.lang);
    if (!g_lobby) g_lobby = l1_new();
    l1_open(g_lobby, &g_set, &env);
    if (g_net_rows) net1_attach_lobby(g_lobby);   /* a link that runs already (started from the console, or before the lobby was closed) shows its state, mode and roles */
    l1_input_reset(&g_in);   /* the first poll inside the lobby only learns what is held (the decide of the native row, A / Enter / F6, is usually still down): live, the A that opened it toggled the first row */
    g_opened_ms = GetTickCount();
    g_open = 1;
    g_opens++;
    r1_log("menu: lobby opened (menu state %d type %d cursor %d count %d)", g_gm_state, g_gm_type, g_gm_cursor, g_gm_count);
}

static void close_lobby(const char *why)
{
    if (!g_open) return;
    l1_settings s;
    l1_settings_get(g_lobby, &s);
    int changed = memcmp(&s, &g_set, sizeof s) != 0;
    g_set = s;
    g_closing = 1;
    g_close_ms = GetTickCount();
    g_quiet = 0;
    g_open = 0;
    publish(NULL);
    if (changed || g_applied_armed < 0 || g_applied_armed != g_set.armed) {
        l1_ini_save(g_ini_path, &g_set, g_lang_override);
        menu_apply(why);
    }
    r1_log("menu: lobby closed (%s)%s", why, changed ? ", settings changed and saved" : "");
}

/* ---------------------------------------------------------------- the network game around the lobby (lobby-must) */
static int is_player_unit(unsigned unit)
{
    char cn[96];
    return unit > 0x10000 && classes_object_name(unit, cn, sizeof cn) && !strncmp(cn, "uPl<", 4);
}
/* a level is loaded on this PC: the sPlayer list holds a player unit (the title screen has none) */
static unsigned g_splayer_ptr = R1_SPLAYER_PTR;   /* the offline harness points it at its own fake list */
void menu_test_set_splayer(unsigned cell) { g_splayer_ptr = cell; }
void menu_test_force_apply_on_close(void) { g_applied_armed = -1; }   /* test: the next close_lobby applies (as a changed setting does) */
static int level_present(void)
{
    unsigned sp = rd32(g_splayer_ptr);
    if (!sp) return 0;
    unsigned head = rd32(sp + 0x30), endp = rd32(sp + 0x34);
    for (int guard = 0; head && head != endp && guard < 40; guard++, head = rd32(head + 8))
        if (is_player_unit(rd32(head + 0x10))) return 1;
    return 0;
}

unsigned menu_title_age_ms(void) { return g_gm_this ? GetTickCount() - g_gm_tick : 0xFFFFFFFFu; }
int menu_split_armed(void) { return g_applied_armed == 1; }   /* rev1 live 16: the local split screen is applied (coopsave1_game.c: a split game uses the co-op save) */

/* the class of the running area: sGameArea [0x11E01E8], current = [+0x3828 + 4 * [+0x3824]] (FUN_0043C590). lm3 live: after the return to the title the game shows
 * the press-start screen (Start / Exit) and the title MENU object does not run until a key: the area class aRevelationsTitle tells "at the title" there too */
static int area_name(char *out, int size)
{
    unsigned ga = rd32(0x11E01E8u), idx = ga ? rd32(ga + 0x3824) : 0, area = idx ? rd32(ga + 0x3828 + 4 * idx) : 0;
    out[0] = 0;
    return area && classes_object_name(area, out, size);
}
static int title_area(void)
{
    char cn[64];
    return area_name(cn, sizeof cn) && strstr(cn, "aRevelationsTitle") != NULL;
}

/* back to the title screen from a level. lm1 live (2026-11-07): the area change [0x11E01E8] vf+0x28 (0x43C740) with the DTI "aTitle" 0x1168E00 is NOT the title: it
 * is a sample area of the engine ("nativePC/sample/title/title.arc" missing -> fatal box -> ExitProcess). The release title is aRevelationsTitle 0x116B2FC; the
 * stock way there from a running game is FUN_008B1660 of the save module (no arguments, plain ret; its only caller 0x8B3A5A runs it after a save error message
 * was closed): fade object ([0x1169110] FUN_0078EE80)->vf+0x18(4), area change to aRevelationsTitle, cGameSystemData_setMode(0), save state 2.
 * Logic thread only; the first bytes are checked. UNVERIFIED live until `menu quit yes` passed on the bench (lobby-must notes section 7). 1 = asked. */
#define QUIT_FN 0x8B1660u
static int quit_to_title(const char *why)
{
    static const unsigned char expect[9] = {0xA1, 0x04, 0x91, 0x16, 0x01, 0x85, 0xC0, 0x75, 0x14};   /* mov eax,[0x1169104]; test eax,eax; jne +0x14 */
    unsigned char cur[9];
    unsigned save = rd32(0x1169104u), fade = rd32(0x1169110u), area = rd32(0x11E01E8u), gs = rd32(0x1169108u);
    if (!r1_safe_read((const void *)(uintptr_t)QUIT_FN, cur, sizeof cur) || memcmp(cur, expect, sizeof cur) || !save || !fade || !area || !gs) {
        r1_log("menu: return to the title (%s) NOT done: bytes at %08x %02x %02x %02x, save %08x fade %08x area %08x system %08x", why, QUIT_FN, cur[0], cur[1], cur[2], save, fade, area, gs);
        return 0;
    }
    r1_log("menu: return to the title (%s): FUN_008B1660 (the save module's return to aRevelationsTitle)", why);
    ((void (*)(void))(uintptr_t)QUIT_FN)();
    return 1;
}

static void strip_show(int kind, const char *name, unsigned ms)
{
    g_strip_kind = kind;
    snprintf(g_strip_name, sizeof g_strip_name, "%s", name ? name : "");
    g_strip_until = (GetTickCount() + ms) | 1;
}

/* once per pass: what the network session means for the title menu and the levels: the guest hold, the strip, the automatic end at the title, the guest's
 * return to the title after the host is gone. Returns the strip to draw (L1G_NONE = none) in *st. */
static void session_watch(int idle, l1_strip_in *st)
{
    unsigned now = GetTickCount();
    memset(st, 0, sizeof *st);
    lk_t *k = net1_link_peek();
    lk_info in;
    memset(&in, 0, sizeof in);
    int running = k && lk_mode(k) != L1N_OFF;
    if (k) lk_get_info(k, &in);
    int started = running && in.started && lk_state(k) == L1NS_CONNECTED;
    int insession = running && in.started;   /* also a host whose partner has left (the link waits again): back at the title the session ends all the same */
    int level = level_present();
    if (insession && level) g_played = 1;
    if (!insession) g_played = 0, g_title_idle_since = 0, g_role_seen = -1;
    /* rev1 live 16: the host's title Continue of a co-op save put the save's roles back (lk_force_roles): the line on both PCs, this PC's character named */
    if (started && !g_played && (in.role == 0 || in.role == 1)) {
        if (g_role_seen >= 0 && in.role != g_role_seen) {
            const char *who = l1_person_name(in.role == 1 ? 2 : 0, detect_lang());   /* Episode 1: Jill / Parker (the game's own names) */
            strip_show(L1G_CONT_ROLES, who, 12000);
            r1_log("menu: strip: Continue keeps the characters of the co-op save: this PC plays the %s (%s) now (it was the %s)", in.role ? "partner" : "lead", who,
                   g_role_seen ? "partner" : "lead");
        }
        g_role_seen = in.role;
    }
    /* the guest's hold: after START and before its first level the host's story start loads this PC (rolebarrier1_game.c follows it) */
    int hold = started && !in.host && !g_played;   /* (a guest whose host is gone is no longer started: the link went to ERROR) */
    if (hold != g_hold) r1_log("menu: guest hold %s", hold ? "ON (the title menu takes no decide; the host's story start loads this PC)" : "off");
    g_hold = hold;
    /* back at the title with a played session: the session ends by itself (the partner is told) */
    if (insession && g_played && !level && (idle || title_area())) {   /* the title menu waits, or the press-start screen of the title area */
        if (!g_title_idle_since) g_title_idle_since = now | 1;
        if ((int)(now - g_title_idle_since) > 1500) {   /* signed: since = now | 1 can be now + 1 (lm2 live: ended 25 ms after the return) */
            int over = !in.host && lk_state(k) != L1NS_CONNECTED;   /* the guest's session had ended already (host ended / removed / lost): only the link is closed */
            r1_log("menu: back at the title menu after a network game: %s", over ? "the ended session's link is closed" : in.host ? "the session ends (the partner is told the host ended it)"
                                                                                                                               : "the session ends (the host is told the partner left)");
            g_stop_by_title++;
            g_own_stop_until = (now + 2000) | 1;
            net1_stop();
            g_played = 0, g_title_idle_since = 0, g_hold = 0;
            return;
        }
    } else
        g_title_idle_since = 0;
    /* events of the link: a guest whose session ended, a host whose partner left */
    if (k && g_seen_ended < 0) g_seen_ended = in.ended, g_seen_gone = in.peer_gone;
    int own_stop = g_own_stop_until && (int)(now - g_own_stop_until) < 0;
    if (!own_stop) g_own_stop_until = 0;
    if (k && own_stop && (in.ended != g_seen_ended || in.peer_gone != g_seen_gone)) {   /* the events of our own stop at the title: nothing to tell */
        g_seen_ended = in.ended, g_seen_gone = in.peer_gone;
        r1_log("menu: the link closed at the title (no strip)");
    }
    /* ep1-flow E5: the end of Episode 1 closes the session on purpose: the partner's close is no loss (no loss strip, no return to the title for a PC whose own end
     * flow runs); a guest that was behind (the host completed it) goes back to the title like a guest whose host ended the session */
    int ep1_local = 0;
    unsigned ep1_closed = 0, ep1 = rolebarrier_ep1_end(&ep1_local, &ep1_closed);
    if (k && in.ended != g_seen_ended) {
        g_seen_ended = in.ended;
        if (ep1) {
            strip_show(ep1_local ? L1G_EP1_DONE : L1G_EP1_BEHIND, in.last_peer, 12000);
            if (!ep1_local && level_present() && g_quit_auto) g_quit_at = (now + 6000) | 1;
            r1_log("menu: the session ended on this guest at the END OF EPISODE 1 (%s)%s", ep1_local ? "this PC passed it too: its own end flow goes on alone" : "this PC was behind the host",
                   g_quit_at ? ": back to the title in 6 s" : "");
        } else {
            int kind = in.ended_reason == L1RS_HOST_ENDED ? L1G_HOST_ENDED : in.ended_reason == L1RS_REMOVED ? L1G_REMOVED : L1G_HOST_LOST;
            strip_show(kind, in.last_peer, 7000);
            if (level_present() && g_quit_auto) g_quit_at = (now + 5000) | 1;   /* the guest's world was the host's: back to the title after the strip was read */
            r1_log("menu: the session ended on this guest (%s)%s", kind == L1G_HOST_ENDED ? "the host ended it" : kind == L1G_REMOVED ? "removed by the host" : "connection lost",
                   g_quit_at ? ": back to the title in 5 s" : "");
        }
    }
    if (k && in.peer_gone != g_seen_gone) {
        g_seen_gone = in.peer_gone;
        if (ep1 && ep1_local) {   /* the host passed the end too: its own end flow goes on alone */
            strip_show(L1G_EP1_DONE, in.last_peer, 12000);
            r1_log("menu: the partner closed the session at the END OF EPISODE 1 (this PC passed it too)");
        } else
            strip_show(in.peer_gone_kind == L1RS_PARTNER_LOST ? L1G_PARTNER_LOST : L1G_PARTNER_LEFT, in.last_peer, 8000);
    }
    if (ep1_closed && ep1_closed != g_ep1_seen) {   /* this PC closed the session itself after its stock end flow */
        g_ep1_seen = ep1_closed;
        strip_show(L1G_EP1_DONE, in.last_peer, 12000);
        r1_log("menu: strip: Episode 1 complete (this PC closed the session after its end flow)");
    }
    {   /* ep1-solo E3: the story switched this PC's character off (1-3 solo part): once per watch, the strip names the character this PC watches now */
        static unsigned seen_enters;
        unsigned wu = 0, en = 0;
        if (solo1_watch_now(&wu, &en) && en != seen_enters) {
            seen_enters = en;
            char cn[96] = "";
            int person = wu && classes_object_name(wu, cn, sizeof cn) ? l1_person_from_class(cn) : -1;
            const char *who = l1_person_name(person < 0 ? 0 : person, detect_lang());
            strip_show(L1G_SOLO_WATCH, who, 12000);
            r1_log("menu: strip: this PC watches %s (%s) while the story goes on without its own character", who, cn[0] ? cn : "?");
        }
    }
    unsigned eb = rolebarrier_ep_block_ms();
    if (eb && eb != g_ep_seen) g_ep_seen = eb, strip_show(L1G_EP_ENDED, "", 10000);
    if (g_quit_at && (int)(now - g_quit_at) >= 0) {
        g_quit_at = 0;
        if (level_present()) quit_to_title("the host is gone");
    }
    /* what to show: a fresh event first, then the running states */
    if (g_strip_kind == L1G_READY_GATE && g_strip_until) {   /* ready1: the gate's line follows the ready flags while it shows, and goes when the gate is gone */
        char pn[40];
        if (ready_gate(&g_strip_flags, pn, sizeof pn)) {
            if (pn[0]) snprintf(g_strip_name, sizeof g_strip_name, "%s", pn);
        } else
            g_strip_until = 0;
    }
    if (g_strip_kind && g_strip_until && (int)(now - g_strip_until) < 0) {
        st->kind = g_strip_kind;
        st->flags = g_strip_flags;
        snprintf(st->name, sizeof st->name, "%s", g_strip_name);
        return;
    }
    g_strip_kind = 0;
    if (!started) return;
    const char *peer = in.last_peer[0] ? in.last_peer : in.peer_name;
    if (in.silent_s > 0 && in.silent_age_ms < 15000) {
        st->kind = L1G_SILENT, st->seconds = in.silent_s + (int)(in.silent_age_ms / 1000u);
        snprintf(st->name, sizeof st->name, "%s", peer);
    } else if (in.wait_s > 0 && in.wait_age_ms < 35000 && in.phase == CP_PHASE_LOADING) {
        st->kind = L1G_WAIT_LOAD;
        snprintf(st->name, sizeof st->name, "%s", in.wait_name[0] ? in.wait_name : peer);
    } else if (!level && !g_played) {
        st->kind = in.host ? L1G_HOST_GO : L1G_GUEST_HOLD;
        snprintf(st->name, sizeof st->name, "%s", peer);
    }
}

/* ---------------------------------------------------------------- per pass (game thread) */
static void menu_frame(void)
{
    if (!g_installed || g_disabled) return;
    unsigned now = GetTickCount();
    int seen = g_gm_this && now - g_gm_tick < 300;
    int idle = seen && g_gm_state == g_idle_state && (g_type_filter < 0 || g_gm_type == g_type_filter);
    l1_raw raw;
    int pads = 0;
    read_raw(&raw, &pads, idle || g_open);
    if (g_net_rows) {   /* the characters typed since the last pass (dropped by the model while the lobby is closed) */
        chars_watch();
        chars_take(raw.typed);
    }
    l1_event ev[8];
    int want_open = 0;
    int n = l1_input_poll(&g_in, &raw, now, g_open, ev, 8, &want_open);
    /* rel010: "Join game" in the Steam friends list or an invite joined in the background (coop_steam's auto-join): at the title the co-op screen opens by itself,
     * the room with the host and the Ready row (Revelations 2 does the same). A test console run (REV1_CONSOLE) keeps it closed unless REV1_MENU_AUTOOPEN=1. */
    if (net1_take_autojoin()) {
        static int autoopen = -1;
        if (autoopen < 0) {
            char v[8];
            autoopen = r1_env("REV1_MENU_AUTOOPEN", v, sizeof v) ? v[0] == '1' : !r1_env("REV1_CONSOLE", v, sizeof v);
        }
        if (autoopen && !g_open && g_net_rows) {
            g_autoopen_wait = now | 1;
            r1_log("menu: a Steam join (Join game / invite) started: the co-op screen opens at the title");
            if (!strcmp(net1_way(), "steam")) g_set.way = 1;   /* the room shows the way the link really runs (b11b: it said Direct IP) */
        }
    }
    if (g_autoopen_wait && !g_open) {
        if (idle) want_open = 1, g_autoopen_wait = 0;
        else if (now - g_autoopen_wait > 30000) g_autoopen_wait = 0;   /* not at the title within 30 s (in a level, a menu of its own): the link runs on, the screen stays shut */
    } else if (g_open)
        g_autoopen_wait = 0;
    if (g_closing) {   /* wait until nothing is held (3 quiet polls) or 1.5 s, then give the menu back */
        int any = raw.up | raw.down | raw.left | raw.right | raw.enter | raw.space | raw.esc | raw.f6 | raw.pad_buttons[0] | raw.pad_buttons[1];
        any |= raw.ly[0] > 22000 || raw.ly[0] < -22000 || raw.lx[0] > 22000 || raw.lx[0] < -22000 || raw.ly[1] > 22000 || raw.ly[1] < -22000 || raw.lx[1] > 22000 || raw.lx[1] < -22000;
        g_quiet = any ? 0 : g_quiet + 1;
        if (g_quiet >= 3 || now - g_close_ms > 1500) g_closing = 0;
        publish(NULL);
        return;
    }
    if (g_open) {
        if (!seen && now - g_gm_tick > 1500 && now - g_opened_ms > 1500) {   /* the title menu is gone (a test opened the lobby elsewhere is left alone for a while) */
            if (g_gm_this) close_lobby("title menu gone");
        }
        l1_event mev[4];
        int mn = mouse_events(mev, 4, now);
        for (int i = 0; i < n; i++) {
            l1_event_in(g_lobby, &ev[i]);
            g_events++;
        }
        for (int i = 0; i < mn; i++) {
            l1_event_in(g_lobby, &mev[i]);
            g_events++;
        }
        l1_env env;
        fill_env(&env, pads);
        l1_set_env(g_lobby, &env);
        lobby_request("closed by the player");
    } else if (want_open && idle) {
        open_lobby();
    }
    /* ep1-flow E12: the Downed / revive rule. The host publishes its choice in the session record (net1 writes it before the START; the default is on, 60 s); a guest's
     * row shows the host's choice */
    {
        l1_settings cur = g_set;
        if (g_open && g_lobby) l1_settings_get(g_lobby, &cur);
        net1_set_session_flags((int)l1_downed_flags(cur.downed));
        if (g_lobby) {
            lk_t *k = net1_link_peek();
            int hf = -1;
            if (k && lk_mode(k) == L1N_JOIN && lk_state(k) == L1NS_CONNECTED) {
                lk_info in;
                lk_get_info(k, &in);
                hf = in.set.flags & 0xFF;
            }
            l1_net_set_host_flags(g_lobby, hf);
        }
    }
    /* what to draw */
    l1_strip_in strip;
    session_watch(idle, &strip);
    if (net1_session_active() && g_applied_armed == 1) {   /* a network game never runs the local split as well (an old armed=1, a console start) */
        r1_log("menu: a network session runs while the split screen was armed: the split is switched off");
        menu_apply("network session");
    }
    if (g_open) {
        static ov_scene sc;
        memset(&sc, 0, sizeof sc);
        sc.kind = OV_LOBBY;
        sc.lang = g_lang;
        l1_view_get(g_lobby, &sc.v);
        sc.cursor_on = now - m_last_move < 3000 && in_foreground();
        sc.cursor_x = m_vx, sc.cursor_y = m_vy;
        publish(&sc);
    } else if (strip.kind != L1G_NONE) {   /* the in-game strip (levels and the title): the partner, the session */
        static ov_scene sc;
        memset(&sc, 0, sizeof sc);
        sc.kind = OV_STRIP;
        sc.lang = detect_lang();
        load_stock(sc.lang);
        l1_strip_text(&strip, sc.lang, sc.hint, sizeof sc.hint, &sc.strip_kind);
        publish(&sc);
    } else if (idle) {
        static ov_scene sc;
        l1_env env;
        fill_env(&env, pads);
        load_stock(env.lang);
        memset(&sc, 0, sizeof sc);
        sc.kind = OV_HINT;
        sc.lang = env.lang;
        l1_hint_text(&g_set, &env, sc.hint, sizeof sc.hint);
        publish(&sc);
    } else
        publish(NULL);
    /* the physical pad exchange stays off outside the campaign (Raid is never touched) */
    capture_state_t cs;
    capture_get_state(&cs);
    split_state_t ss;
    split_get_state(&ss);
    if (g_set.armed && g_set.pads_swapped && (cs.blocked || ss.blocked)) testmode_pad_swap_base(0);
    else if (g_set.armed && g_set.pads_swapped && !testmode_pad_swap_base_get()) testmode_pad_swap_base(1);
}

/* ---------------------------------------------------------------- install */
void menu_install(void)
{
    char v[16];
    if (g_installed) return;
    g_disabled = 0;
    static int lock_init;
    if (!lock_init) InitializeCriticalSection(&g_lock), InitializeCriticalSection(&g_chl), lock_init = 1;
    g_net_rows = !(r1_env("REV1_NETROWS", v, sizeof v) && v[0] == '0');   /* the product: the Mode row offers the network; REV1_NETROWS=0 = the old local screen */
    r1_log("menu: network rows %s (the Mode row: Off / Split screen / Network: host / Network: join; the link is the coop session, enet port %d)", g_net_rows ? "ON" : "OFF (REV1_NETROWS=0)",
           LK_DEFAULT_PORT);
    {   /* the identity in the network: the Steam persona name (never the computer or Windows user name), this mod's version, no rejoin after the start (REV1_REJOIN=1: the lab's rejoin) */
        int rejoin = r1_env("REV1_REJOIN", v, sizeof v) && v[0] == '1';
        net1_set_identity(steam_persona_name, R1_VERSION, !rejoin);
        net1_set_fallback(fallback_name);
        g_quit_auto = !(r1_env("REV1_LOSTQUIT", v, sizeof v) && v[0] == '0');   /* (the way back to the title: quit_to_title, see there) */
        r1_log("menu: network identity: mod %s, rejoin after the start %s, the guest returns to the title when the host is gone: %s", R1_VERSION, rejoin ? "allowed (REV1_REJOIN=1)" : "refused",
               g_quit_auto ? "yes" : "no (REV1_LOSTQUIT=0)");
    }
    if (r1_env("REV1_MENU", v, sizeof v) && v[0] == '0') {
        r1_log("menu: disabled by REV1_MENU=0");
        g_disabled = 1;
        return;
    }
    ini_path_init();
    if (!ini_enabled()) {
        r1_log("menu: disabled by enabled=0 in %s", g_ini_path);
        g_disabled = 1;
        return;
    }
    int ini_found = l1_ini_load(g_ini_path, &g_set, &g_lang_override);
    l1_input_reset(&g_in);
    /* wrap the title menu's update: patch one slot of the class vtable (checked first) */
    unsigned cur = rd32(g_vtable + 4 * GM_VF8_SLOT);
    if (cur != g_expect_orig) {
        r1_log("menu: uGUI_GeneralMenu vtable slot 8 is %08x, expected %08x: not this build, the menu module stays off", cur, g_expect_orig);
        g_disabled = 1;
        return;
    }
    void *fake = (void *)g_vtable;
    unsigned mcur = rd32(g_mgr_vtable + 4 * GM_VF8_SLOT);
    if (mcur != g_mgr_expect) {
        r1_log("menu: uTitleMenuManager vtable slot 8 is %08x, expected %08x: not this build, the menu module stays off", mcur, g_mgr_expect);
        g_disabled = 1;
        return;
    }
    orig_vf8 = (vf8_t)cur;   /* set BEFORE the patch: the menu's update may run on another thread right after it */
    orig_mgr_vf8 = (vf8_t)mcur;
    void *old = r1_patch_vtable(&fake, GM_VF8_SLOT, (void *)hook_vf8);
    if (!old) {
        r1_log("menu: cannot patch the vtable slot");
        g_disabled = 1;
        return;
    }
    void *fake2 = (void *)g_mgr_vtable;
    if (!r1_patch_vtable(&fake2, GM_VF8_SLOT, (void *)hook_mgr_vf8)) {
        r1_log("menu: cannot patch the manager's vtable slot (the page's own update is wrapped already: the entry still works, keys may leak into the stock menu)");
    }
    if (r1_env("REV1_NATIVEROW", v, sizeof v) && v[0] == '0') {
        g_row_enabled = 0;
        r1_log("menu: native row disabled by REV1_NATIVEROW=0 (hint + F6 / BACK only)");
    } else if (!install_act_detour()) {
        g_row_enabled = 0;   /* never add a row whose decide nobody catches */
    }
    lt_add_frame_cb(menu_frame);
    g_installed = 1;
    r1_log("menu: ready (settings file %s%s; armed %d role %s layout %s pads %s)", g_ini_path, ini_found ? "" : " - not there yet", g_set.armed, g_set.role == L1_PARTNER ? "partner" : "lead",
           g_set.layout == L1_LAYOUT_TOPBOTTOM ? "top/bottom" : "side by side", g_set.pads_swapped ? "exchanged" : "normal");
    if (g_set.armed) menu_apply("settings file");
}

/* ---------------------------------------------------------------- console */
static void view_text(char *out, int size)
{
    if (!g_lobby) {
        snprintf(out, (size_t)size, "no lobby object yet (menu open)");
        return;
    }
    l1_view v;
    l1_view_get(g_lobby, &v);
    int o = snprintf(out, (size_t)size, "%s\n%s\n", v.title, v.subtitle);
    for (int i = 0; i < v.nrows && o < size - 200; i++) o += snprintf(out + o, (size_t)(size - o), "%s %s | %s%s\n", i == v.sel ? ">" : " ", v.rows[i].label, v.rows[i].value, v.rows[i].kind == L1V_GREY ? " (grey)" : "");
    if (v.cards_on && o < size - 200)
        o += snprintf(out + o, (size_t)(size - o), "cards: [%s: %s, %s] [%s: %s, %s]\n", v.card[0].who, v.card[0].chr, v.card[0].pad, v.card[1].who, v.card[1].chr, v.card[1].pad);
    if (o < size - 100) o += snprintf(out + o, (size_t)(size - o), "status(%d): %s\nnote: %s\nnote2: %s\nhint: %s", v.status_kind, v.status, v.note, v.note2, v.hint);
}

void menu_cmd(const char *arg, char *out, int size)
{
    char sub[32] = "", rest[200] = "";
    sscanf(arg, "%31s %199[^\r\n]", sub, rest);
    if (!g_installed && strcmp(sub, "status")) {
        snprintf(out, (size_t)size, "menu module %s", g_disabled ? "disabled (REV1_MENU=0, enabled=0, or another build)" : "not installed yet");
        return;
    }
    if (!sub[0] || !strcmp(sub, "status")) {
        char ov[200];
        overlay_stats(ov, sizeof ov);
        unsigned now = GetTickCount();
        snprintf(out, (size_t)size,
                 "menu: %s; title menu object %08x state %d type %d sub %d cursor %d items %d (last update %d ms ago, %u calls, idle state %d, type filter %d); lobby %s; settings armed %d role %s layout %s pads %s; "
                 "language %s (src %d); last apply: %s\nini %s\n%s",
                 g_installed ? "installed" : (g_disabled ? "disabled" : "not installed"), g_gm_this, g_gm_state, g_gm_type, g_gm_sub, g_gm_cursor, g_gm_count,
                 g_gm_this ? (int)(now - g_gm_tick) : -1, g_gm_calls, g_idle_state, g_type_filter, g_open ? "OPEN" : "closed", g_set.armed, g_set.role == L1_PARTNER ? "partner" : "lead",
                 g_set.layout == L1_LAYOUT_TOPBOTTOM ? "top/bottom" : "side by side", g_set.pads_swapped ? "exchanged" : "normal", g_lang >= 0 ? l1_lang_codes[g_lang] : "?", g_lang_src,
                 g_last_apply[0] ? g_last_apply : "-", g_ini_path, ov);
    } else if (!strcmp(sub, "open")) {
        open_lobby();
        snprintf(out, (size_t)size, "lobby %s", g_open ? "open" : "not open");
    } else if (!strcmp(sub, "close")) {
        close_lobby("console");
        snprintf(out, (size_t)size, "lobby closed");
    } else if (!strcmp(sub, "view")) {
        view_text(out, size);
    } else if (!strcmp(sub, "ev")) {
        static const struct { const char *n; int k; } map[] = {{"up", L1E_UP}, {"down", L1E_DOWN}, {"left", L1E_LEFT}, {"right", L1E_RIGHT}, {"ok", L1E_OK}, {"back", L1E_BACK}};
        char w[16] = "";
        int row = 0;
        sscanf(rest, "%15s %d", w, &row);
        int k = 0;
        if (!strcmp(w, "click")) k = L1E_CLICK;
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(map[i].n, w)) k = map[i].k;
        if (!k || !g_open || !g_lobby) {
            snprintf(out, (size_t)size, "usage: menu ev up|down|left|right|ok|back|click <row> (lobby must be open)");
            return;
        }
        l1_event e = {k, row};
        l1_event_in(g_lobby, &e);
        lobby_request("closed by event");
        view_text(out, size);
    } else if (!strcmp(sub, "set")) {
        char *tok = strtok(rest, " ");
        int n = 0;
        while (tok) {
            char *eq = strchr(tok, '=');
            if (eq) {
                *eq = 0;
                const char *k = tok, *v = eq + 1;
                if (!strcmp(k, "armed")) g_set.armed = atoi(v) != 0, n++;
                else if (!strcmp(k, "role")) g_set.role = !strcmp(v, "partner") ? L1_PARTNER : L1_LEAD, n++;
                else if (!strcmp(k, "layout")) g_set.layout = !strcmp(v, "top") ? L1_LAYOUT_TOPBOTTOM : L1_LAYOUT_SIDE, n++;
                else if (!strcmp(k, "pads")) g_set.pads_swapped = !strcmp(v, "swapped"), n++;
            }
            tok = strtok(NULL, " ");
        }
        l1_ini_save(g_ini_path, &g_set, g_lang_override);
        menu_apply("console");
        snprintf(out, (size_t)size, "%d settings changed, saved and applied: %s", n, g_last_apply);
    } else if (!strcmp(sub, "apply")) {
        menu_apply("console");
        snprintf(out, (size_t)size, "%s", g_last_apply);
    } else if (!strcmp(sub, "quit")) {   /* lab: menu quit yes = the return to the title the guest makes when the host is gone (changes the game for real) */
        if (strcmp(rest, "yes")) snprintf(out, (size_t)size, "menu quit yes: back to the title screen now (FUN_008B1660, the save module's return to aRevelationsTitle; REAL)");
        else snprintf(out, (size_t)size, "return to the title %s", quit_to_title("console") ? "asked" : "NOT possible (see the log)");
    } else if (!strcmp(sub, "strip")) {   /* lab: menu strip <kind 1..10> [name] [seconds]: show an in-game strip (screenshots of every kind and language) */
        int kind = 0, secs = 15;
        char nm[40] = "";
        sscanf(rest, "%d %39s %d", &kind, nm, &secs);
        if (kind <= L1G_NONE || kind >= L1G_COUNT) {
            snprintf(out, (size_t)size, "menu strip <1..%d> [name] [seconds]: 1 wait load, 2 silent, 3 partner left, 4 partner lost, 5 host ended, 6 host lost, 7 removed, 8 guest hold, 9 host go, 10 Episode 1 gate, 11 Episode 1 done, 12 Episode 1 behind, 13 solo watch",
                     L1G_COUNT - 1);
            return;
        }
        strip_show(kind, nm, (unsigned)secs * 1000u);
        l1_strip_in in = {kind, "", secs};
        snprintf(in.name, sizeof in.name, "%s", nm);
        char t[400];
        l1_strip_text(&in, detect_lang(), t, sizeof t, NULL);
        snprintf(out, (size_t)size, "strip %d for %d s: %s", kind, secs, t);
    } else if (!strcmp(sub, "net")) {   /* the session watch: hold, strip, played, counters */
        lk_t *k = net1_link_peek();
        lk_info in;
        memset(&in, 0, sizeof in);
        if (k) lk_get_info(k, &in);
        char an[64];
        area_name(an, sizeof an);
        snprintf(out, (size_t)size, "menu net: area %s, rows %d, hold %d, played %d, level %d, title age %u ms, strip %d (%s) until %+d ms, quit at %+d ms (auto %d), save area %08x, link: started %d host %d "
                 "reason %d ended %d (%d) gone %d (%d) silent %d s (%u ms ago) wait %d s, ended by the title %u",
                 an[0] ? an : "-", g_net_rows, g_hold, g_played, level_present(), menu_title_age_ms(), g_strip_kind, g_strip_name, g_strip_until ? (int)(g_strip_until - GetTickCount()) : 0,
                 g_quit_at ? (int)(g_quit_at - GetTickCount()) : 0, g_quit_auto, save_area(), in.started, in.host, in.reason, in.ended, in.ended_reason, in.peer_gone, in.peer_gone_kind, in.silent_s,
                 in.silent_age_ms, in.wait_s, g_stop_by_title);
    } else if (!strcmp(sub, "lang")) {
        int l = l1_lang_from_code(rest);
        if (l < 0) {
            snprintf(out, (size_t)size, "usage: menu lang eng|fre|ita|ger|spa|jpn|rus|pol|dut|por|ptB|chS|chT");
            return;
        }
        g_lang = l, g_lang_src = 2;
        g_stock_loaded = -1;
        load_stock(l);
        snprintf(out, (size_t)size, "language %s", l1_lang_codes[l]);
    } else if (!strcmp(sub, "idle")) {
        g_idle_state = atoi(rest);
        snprintf(out, (size_t)size, "the menu counts as waiting for input in state %d", g_idle_state);
    } else if (!strcmp(sub, "type")) {
        g_type_filter = atoi(rest);
        snprintf(out, (size_t)size, "the hint and F6 work on title menu type %d (-1 = any page)", g_type_filter);
    } else if (!strcmp(sub, "hint")) {   /* menu hint x y w h: where the hint strip is drawn (virtual 1920x1080), to keep clear of the stock menu's own help line */
        float x, y, w, h;
        if (sscanf(rest, "%f %f %f %f", &x, &y, &w, &h) == 4) {
            overlay_set_hint_rect(x, y, w, h);
            snprintf(out, (size_t)size, "hint strip at %.0f,%.0f size %.0fx%.0f", x, y, w, h);
        } else
            snprintf(out, (size_t)size, "usage: menu hint <x> <y> <w> <h> (virtual 1920x1080)");
    } else if (!strcmp(sub, "items")) {   /* the list item records of the current page (research for the native row): pointer and 32 bytes each */
        unsigned s = g_gm_this;
        if (!s) {
            snprintf(out, (size_t)size, "no title menu object seen");
            return;
        }
        int cnt = (int)rd32(s + GM_COUNT), o = snprintf(out, (size_t)size, "page %08x: %d items, array +0x9B8:\n", s, cnt);
        for (int k = 0; k < 8 && k < cnt + 2 && o < size - 200; k++) {
            unsigned p = rd32(s + 0x9B8 + 4 * k), w[8] = {0};
            if (p) r1_safe_read((void *)p, w, sizeof w);
            o += snprintf(out + o, (size_t)(size - o), "[%d] %08x: %08x %08x %08x %08x %08x %08x %08x %08x\n", k, p, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    } else if (!strcmp(sub, "gm")) {   /* dump of the title menu object for the live research of the native row (stage B) */
        unsigned s = g_gm_this;
        if (!s) {
            snprintf(out, (size_t)size, "no title menu object seen");
            return;
        }
        int o = snprintf(out, (size_t)size, "uGUI_GeneralMenu %08x: flags %08x state %d sub %d cursor %d shortcut %d items %d type %d\nnodes (+0xAB4): ", s, rd32(s + 0xC), (int)rd32(s + GM_STATE),
                         (int)rd32(s + GM_SUB), (int)rd32(s + GM_CURSOR), (int)rd32(s + GM_SHORTCUT), (int)rd32(s + GM_COUNT), (int)rd32(s + GM_TYPE));
        for (int i = 0; i < 7 && o < size - 200; i++) {
            unsigned node = rd32(s + GM_NODES + 4 * i);
            o += snprintf(out + o, (size_t)(size - o), "[%d] %08x flags54 %08x msg %08x; ", i, node, node ? rd32(node + 0x54) : 0, rd32(s + GM_MSGS + 4 * i));
        }
        o += snprintf(out + o, (size_t)(size - o), "\nitem flags +0x464: ");
        for (int i = 0; i < 16 && o < size - 100; i++) {
            unsigned char b = 0;
            r1_safe_read((void *)(s + 0x464 + i), &b, 1);
            o += snprintf(out + o, (size_t)(size - o), "%02x ", b);
        }
    } else
        snprintf(out, (size_t)size, "menu status | open | close | view | ev <up|down|left|right|ok|back|click n> | set armed=1 role=lead|partner layout=side|top pads=normal|swapped | apply | lang <code> | type <n> | idle <state> | hint x y w h | gm | items | net | strip <kind> [name] [s] | quit yes");
}
