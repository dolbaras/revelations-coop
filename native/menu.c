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
#include "overlay.h"
#include "r1.h"
#include "split.h"

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
static l1_settings g_set = {0, L1_LEAD, L1_LAYOUT_SIDE, 0};
static int g_lang = -1, g_lang_override = -1, g_lang_src;   /* g_lang_src: 0 default, 1 env, 2 ini, 3 steam */
static int g_stock_loaded = -1;
static l1_input g_in;
static unsigned g_opened_ms;
static char g_ini_path[MAX_PATH];
static unsigned g_applies, g_opens, g_events;
static int g_applied_armed = -1;
static char g_last_apply[160];

static CRITICAL_SECTION g_lock;
static ov_scene g_scene;
static unsigned g_scene_tick;

typedef void(__attribute__((fastcall)) * vf8_t)(void *self);
static vf8_t orig_vf8, orig_mgr_vf8;
static unsigned g_mgr_calls;

/* The title menu MANAGER (page logic: cursor movement, activation, back) reads the keys / buttons itself: freezing only the page's own update let the cursor moves and the closing Esc through
 * (live: the cursor ran to the last row, Esc went back to the title screen). Its update is frozen too while the lobby is open or just closed. */
static void __attribute__((fastcall)) hook_mgr_vf8(void *self)
{
    g_mgr_calls++;
    if (!g_open && !g_closing) orig_mgr_vf8(self);
}

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

static unsigned __attribute__((fastcall)) hook_act(void *page, void *edx, unsigned cancel, unsigned mouse)
{
    unsigned s = (unsigned)(size_t)page;
    if (g_row_enabled && s == g_row_page && (cancel & 0xFF) == 0 && (int)rd32(s + GM_STATE) == g_idle_state && (int)rd32(s + GM_CURSOR) == g_row_index && !g_open && !g_closing) {
        g_row_acts++;
        r1_log("menu: native row decided (%s): lobby opens", (mouse & 0xFF) ? "mouse" : "key / pad");
        open_lobby();
        return 1;   /* handled: the page stays in its waiting state, the stock manager sees nothing */
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
    if (!g_open && !g_closing) orig_vf8(self);   /* lobby open (or just closed, keys still down): the stock menu is frozen */
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

/* ---------------------------------------------------------------- language, stock words */
static int detect_lang(void)
{
    char v[32];
    if (g_lang >= 0) return g_lang;
    int l = -1;
    if (r1_env("REV1_LANG", v, sizeof v) && (l = l1_lang_from_code(v)) >= 0) g_lang_src = 1;
    else if (g_lang_override >= 0) l = g_lang_override, g_lang_src = 2;
    else if ((l = l1_lang_from_langid(GetUserDefaultLangID())) >= 0) g_lang_src = 3;   /* the game picks its language from this too (its import: GetUserDefaultLangID), not from Steam */
    else l = 0, g_lang_src = 0;
    g_lang = l;
    r1_log("menu: language %s (%s)", l1_lang_codes[l], g_lang_src == 1 ? "REV1_LANG" : g_lang_src == 2 ? "rev1_coop.ini" : g_lang_src == 3 ? "system language (GetUserDefaultLangID)" : "default");
    return l;
}

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
    r1_log("menu: stock texts of the game (%s): %d names, %d menu words", l1_lang_codes[lang], got_names, got_words);
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
static void fill_env(l1_env *env, int pads)
{
    memset(env, 0, sizeof *env);
    env->lang = detect_lang();
    env->pads_connected = pads;
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
    l1_event ev[8];
    int want_open = 0;
    int n = l1_input_poll(&g_in, &raw, now, g_open, ev, 8, &want_open);
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
        if (l1_take_request(g_lobby) == L1R_CLOSE) close_lobby("closed by the player");
    } else if (want_open && idle) {
        open_lobby();
    }
    /* what to draw */
    if (g_open) {
        static ov_scene sc;
        memset(&sc, 0, sizeof sc);
        sc.kind = OV_LOBBY;
        sc.lang = g_lang;
        l1_view_get(g_lobby, &sc.v);
        sc.cursor_on = now - m_last_move < 3000 && in_foreground();
        sc.cursor_x = m_vx, sc.cursor_y = m_vy;
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
    if (!lock_init) InitializeCriticalSection(&g_lock), lock_init = 1;
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
        if (l1_take_request(g_lobby) == L1R_CLOSE) close_lobby("closed by event");
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
        snprintf(out, (size_t)size, "menu status | open | close | view | ev <up|down|left|right|ok|back|click n> | set armed=1 role=lead|partner layout=side|top pads=normal|swapped | apply | lang <code> | type <n> | idle <state> | hint x y w h | gm | items");
}
