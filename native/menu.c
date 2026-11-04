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
#define GM_NODES 0xAB4              /* seven shortcut GUI nodes */
#define GM_MSGS 0xAD4

static unsigned g_vtable = GM_VTABLE, g_expect_orig = GM_VF8_ORIG;   /* the offline harness replaces them */
void menu_test_set_addrs(unsigned vtable, unsigned expect_orig) { g_vtable = vtable, g_expect_orig = expect_orig; }

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
static vf8_t orig_vf8;

/* ---------------------------------------------------------------- the update wrapper */
static void __attribute__((fastcall)) hook_vf8(void *self)
{
    if (!g_open) orig_vf8(self);   /* lobby open: the stock menu is frozen under the overlay */
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
static int steam_lang(void)
{
    void *ctx = steam_context();
    if (!ctx) return -1;
    unsigned apps = 0, vt = 0;
    if (!r1_safe_read((char *)ctx + 0x18, &apps, 4) || apps < 0x10000) return -1;
    if (!r1_safe_read((void *)apps, &vt, 4) || vt < 0x10000) return -1;
    unsigned fn = 0;
    if (!r1_safe_read((void *)(vt + 4 * 4), &fn, 4) || fn < 0x10000) return -1;   /* ISteamApps::GetCurrentGameLanguage is slot 4 of v008 */
    HMODULE m = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m) || !m) return -1;   /* inside a loaded module */
    typedef const char *(__attribute__((thiscall)) * lang_t)(void *);
    const char *s = ((lang_t)fn)((void *)apps);
    if (!s) return -1;
    char buf[32] = {0};
    if (!r1_safe_read(s, buf, 31)) return -1;
    buf[31] = 0;
    return l1_lang_from_steam(buf);
}

static int detect_lang(void)
{
    char v[32];
    if (g_lang >= 0) return g_lang;
    int l = -1;
    if (r1_env("REV1_LANG", v, sizeof v) && (l = l1_lang_from_code(v)) >= 0) g_lang_src = 1;
    else if (g_lang_override >= 0) l = g_lang_override, g_lang_src = 2;
    else if ((l = steam_lang()) >= 0) g_lang_src = 3;
    else l = 0, g_lang_src = 0;
    g_lang = l;
    r1_log("menu: language %s (%s)", l1_lang_codes[l], g_lang_src == 1 ? "REV1_LANG" : g_lang_src == 2 ? "rev1_coop.ini" : g_lang_src == 3 ? "Steam game language" : "default");
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
    if (g_open) {
        if (!seen && now - g_gm_tick > 1500 && now - g_opened_ms > 1500) {   /* the title menu is gone (a test opened the lobby elsewhere is left alone for a while) */
            if (g_gm_this) close_lobby("title menu gone");
        }
        for (int i = 0; i < n; i++) {
            l1_event_in(g_lobby, &ev[i]);
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
    void *old = r1_patch_vtable(&fake, GM_VF8_SLOT, (void *)hook_vf8);
    if (!old) {
        r1_log("menu: cannot patch the vtable slot");
        g_disabled = 1;
        return;
    }
    orig_vf8 = (vf8_t)old;
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
        snprintf(out, (size_t)size, "menu status | open | close | view | ev <up|down|left|right|ok|back|click n> | set armed=1 role=lead|partner layout=side|top pads=normal|swapped | apply | lang <code> | type <n> | idle <state> | hint x y w h | gm");
}
