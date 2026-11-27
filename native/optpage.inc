/* optpage.inc - the NATIVE Co-Op page (rel027m; notes\rel027m.md), included by menu.c after the pause row.
 *
 * What the player sees is the game's own Options page (uGUI_OptionMenu, option_menu.gui): the category name, up to six rows "LABEL  < VALUE >" with the stock cursor ring, arrows, sounds
 * and animations, the help line and the legend. Nothing is drawn by the mod. The page is the one of the stock Options flow:
 *   - the pause panel's first row (our "Co-Op Play", menu.c pz_*) starts the Options flow (pause state 0x14, the row's kind = 1 for one frame), exactly as the row "Options" does;
 *   - the Options manager (uOptionManager, vtable 0x1089530, update = slot 8 0x80C060) is wrapped: at its first state (0 = show the category list) the list gets ONE row, ours
 *     (the others have empty texts, the cursor stands on it), and the handler slot of the page kind is taken (kind 2 = the Game page: four rows; kind 0 = the Controls page: six rows);
 *     the list is the stock one: A on the row opens the page with the stock call FUN_00806300(kind), B leaves, as in the Options;
 *   - the page object has one handler function per kind in its own data (+0x270 + 4 * kind, called by uGUI_OptionMenu's update 0x800930 while +0x294 is 1): ours replaces the one
 *     of the open kind for the length of the page. It is the stock Game handler (0x803950) with the data operations replaced: the pad / mouse input is turned into the events of the
 *     lobby model (lobby1.c, the same model as the overlay page: the rows, the rules, the requests) and the model's rows are written into the page's label / value text nodes;
 *   - B on the page = the stock exit calls (sound 0x78, +0x295, FUN_008475a0) and the list comes back; B on the list ends the Options flow (manager state 8: the model closes and is saved).
 *     Restart / Leave / End co-op close the model: the page leaves and the manager goes to its exit state 7 at once; the pause panel then opens the stock popup (pz_goto).
 * REV1_NATIVEPAGE=0: the overlay page of pausenative (menu.c open_pause_page) as before. A step that does not find the expected objects logs it and falls back to the overlay page. */
#define OM_VTABLE 0x1089530u
#define OM_VF8_ORIG 0x80C060u
#define OM_STATE 0x38
#define OM_TOP 0x26C
#define OM_MENU 0x270
#define OP_MGR 0x290
#define OP_ACTIVE 0x294
#define OP_EXITFLAG 0x295
#define OP_KIND 0x298
#define OP_CURSOR 0x2A0
#define OP_COUNT 0x2A4
#define OP_MOUSE 0x2B0
#define OP_ROWS 0x2E8              /* six row nodes (4 bytes each) */
#define OP_LABELS 0x33C            /* six text binders {node, message id, bank}, 12 bytes each */
#define OP_VALUES 0x384            /* six value binders */
#define NP_SGP 0x11E0388u          /* the pad manager pointer cell (g_sGamePad) */
#define NP_SND 0x116910Cu          /* the sound / system object cell */
#define NP_INP 0x11E057Cu          /* the stick / mouse input object cell */
#define NP_SAVE 0x1169118u         /* the save system cell (busy check) */
#define NP_HELP 0x1181EE4u         /* the help line owner cell: its +0x29C is the help text node */
#define NP_ROWS_MAX 6

typedef void(__attribute__((thiscall)) * np_v0_t)(void *self);
typedef void(__attribute__((thiscall)) * np_v1_t)(void *self, unsigned a);
typedef void(__attribute__((thiscall)) * np_v2_t)(void *self, unsigned a, unsigned b);
typedef int(__attribute__((thiscall)) * np_i0_t)(void *self);
typedef int(__attribute__((thiscall)) * np_i1_t)(void *self, unsigned a);
static np_v1_t np_open_page = (np_v1_t)0x806300;       /* the stock "open the sub page of kind k" (manager state 2) */
static np_v1_t np_place_cursor = (np_v1_t)0x800BA0;    /* cursor ring for the page's cursor / count (+ the row's help animation when the flag is 1) */
static np_i1_t np_hover_row = (np_i1_t)0x801850;       /* (page, 0xD) the row under the mouse, -1 none */
static np_i1_t np_click = (np_i1_t)0x801910;           /* (page, 0xB right arrow / 0xC left arrow) 1 = decide pressed on that arrow */
static np_i0_t np_stick = (np_i0_t)0x873640;           /* (input object) 1 = stick up, 2 = down */
static np_v0_t np_stick_a = (np_v0_t)0x8736A0, np_stick_b = (np_v0_t)0x8736E0;   /* (input object) the stick repeat reset */
static np_v2_t np_sound = (np_v2_t)0x8C0100;           /* (sound object, id, page) 3 move, 0x26 change */
static np_v1_t np_sound1 = (np_v1_t)0x8BF860;          /* (sound object, id) 0x78 back */
static np_v1_t np_anim = (np_v1_t)0xB92CB0;            /* (row node, id) 0x2763 plus / 0x2777 minus */
static np_i0_t np_close_page = (np_i0_t)0x8475A0;      /* (page) the stock exit of a page (+0x295 = 1 first) */
static np_v0_t np_pad_changed = (np_v0_t)0x800B50;     /* (page) the legend icons after a device change */
static np_i0_t np_busy = (np_i0_t)0x79C2D0;            /* (save system) 1 = the save system is busy */
static np_v2_t np_settext = (np_v2_t)0xB8C140;         /* (text node, string, length) */

static volatile int g_np_skip;
static int g_np_enabled = 1, g_np_kind = 0;
static unsigned g_np_req_ms, g_np_age, g_np_calls, g_np_mgr_calls, g_np_opens, g_np_closes, g_np_last_menu, g_np_last_mgr, g_np_slot, g_np_old_fn;
static int g_np_leaving, g_np_kind0_fix, g_np_kind0_saved, g_np_hover = -1, g_np_prev_sel = -1;
static unsigned g_np_pause;                 /* the pause object that started the flow */
static char g_np_lab[NP_ROWS_MAX][160], g_np_val[NP_ROWS_MAX][160], g_np_help[240];
static char g_np_set_lab[NP_ROWS_MAX][160], g_np_set_val[NP_ROWS_MAX][160], g_np_set_help[240];
static vf8_t orig_om_vf8;
static int g_np_installed;

static int rd8m(unsigned a)
{
    unsigned char v = 0;
    r1_safe_read((const void *)(uintptr_t)a, &v, 1);
    return v;
}
static int wr8m(unsigned a, int v)
{
    if (IsBadWritePtr((void *)(uintptr_t)a, 1)) return 0;
    *(volatile unsigned char *)(uintptr_t)a = (unsigned char)v;
    return 1;
}

/* mixed case -> the menus' capitals (Latin, Cyrillic, Greek; the Asian scripts are unchanged) */
static void np_upper(const char *in, char *out, int size)
{
    wchar_t w[400];
    int n = MultiByteToWideChar(CP_UTF8, 0, in, -1, w, 399);
    if (n <= 1) { out[0] = 0; return; }
    CharUpperBuffW(w, (DWORD)(n - 1));
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL)) out[0] = 0;
}
/* "< value >" (the overlay's arrows) -> "value" (the page has its own arrows) */
static void np_strip(const char *in, char *out, int size)
{
    size_t n = strlen(in);
    const char *p = in;
    if (n >= 4 && in[0] == '<' && in[1] == ' ' && in[n - 1] == '>' && in[n - 2] == ' ') {
        p = in + 2;
        n -= 4;
    }
    if ((int)n >= size) n = (size_t)size - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static unsigned np_menu_of(unsigned mgr) { return mgr ? rd32(mgr + OM_MENU) : 0; }

/* the model's rows -> the page's text nodes (only what changed; the first 40 frames everything, the stock texts of the page's own opening come later) */
static void np_set_text(unsigned node, const char *s)
{
    if (!node) return;
    np_settext((void *)(uintptr_t)node, (unsigned)(uintptr_t)s, (unsigned)strlen(s));
}
static void np_texts(unsigned m, const l1_view *v)
{
    int force = g_np_age < 40;
    for (int k = 0; k < NP_ROWS_MAX; k++) {
        char tmp[160];
        g_np_lab[k][0] = g_np_val[k][0] = 0;
        if (k < v->nrows) {
            np_upper(v->rows[k].label, g_np_lab[k], sizeof g_np_lab[k]);
            np_strip(v->rows[k].value, tmp, sizeof tmp);
            np_upper(tmp, g_np_val[k], sizeof g_np_val[k]);
        }
        if (force || strcmp(g_np_lab[k], g_np_set_lab[k])) {
            memcpy(g_np_set_lab[k], g_np_lab[k], sizeof g_np_set_lab[k]);
            np_set_text(rd32(m + OP_LABELS + 12 * (unsigned)k), g_np_set_lab[k]);
        }
        if (force || strcmp(g_np_val[k], g_np_set_val[k])) {
            memcpy(g_np_set_val[k], g_np_val[k], sizeof g_np_set_val[k]);
            np_set_text(rd32(m + OP_VALUES + 12 * (unsigned)k), g_np_set_val[k]);
        }
    }
    /* the help line under the page (a global node): the version row says where to report problems, the others the page's line */
    int sel = v->sel >= 0 && v->sel < v->nrows ? v->sel : 0;
    if (v->nrows && v->rows[sel].kind == L1V_INFO) snprintf(g_np_help, sizeof g_np_help, "%s", R1_URL);
    else snprintf(g_np_help, sizeof g_np_help, "%s", v->status[0] ? v->status : v->subtitle);
    if (force || strcmp(g_np_help, g_np_set_help)) {
        memcpy(g_np_set_help, g_np_help, sizeof g_np_set_help);
        unsigned ho = rd32(NP_HELP);
        np_set_text(ho ? rd32(ho + 0x29C) : 0, g_np_set_help);
    }
}

/* the page leaves like the stock handler's B: the sound, +0x295, the stock exit animation (the list comes back when it is over) */
static void np_exit_page(unsigned m, int user_back)
{
    if (g_np_leaving) return;
    g_np_leaving = 1;
    if (user_back) np_sound1((void *)(uintptr_t)rd32(NP_SND), 0x78);
    wr8m(m + OP_EXITFLAG, 1);
    int r = np_close_page((void *)(uintptr_t)m) & 0xFF;
    np_stick_a((void *)(uintptr_t)rd32(NP_INP));
    g_np_closes++;
    r1_log("np: the page leaves (%s, exit animation %s)", user_back ? "B" : "closed by the model", r ? "started" : "NOT started");
}

/* the page handler: the stock Game handler's order (0x803950) with the data operations replaced */
static void __attribute__((fastcall)) np_handler(void *self)
{
    unsigned m = (unsigned)(size_t)self;
    g_np_calls++;
    unsigned sg = rd32(NP_SGP), inp = rd32(NP_INP), snd = rd32(NP_SND), mgr = rd32(m + OP_MGR);
    if (!sg || !g_lobby || !mgr) return;
    if (g_np_leaving) {
        if (rd8m(m + OP_EXITFLAG)) return;   /* the exit animation runs */
        g_np_leaving = 0;                    /* the page was opened again (the stock open clears +0x295) */
        g_np_age = 0, g_np_prev_sel = -1, g_np_hover = -1;
        r1_log("np: the page is open again");
    }
    g_np_age++;
    if (rd32(sg + 0x874) != rd32(sg + 0x878)) np_pad_changed(self);
    if (rd8m(mgr + 0x30) == 1) {   /* the manager is closing (the pause key): the stock exit */
        wr8m(m + OP_EXITFLAG, 1);
        np_close_page(self);
        np_stick_a((void *)(uintptr_t)inp);
        g_np_leaving = 1;
        r1_log("np: the manager closes: the page leaves");
        return;
    }
    if (rd8m(mgr + 0x33) == 1) return;
    unsigned save = rd32(NP_SAVE);
    if (save && (np_busy((void *)(uintptr_t)save) & 0xFF)) return;
    unsigned dev = rd32(sg + 0x870) * 0x2CCu;
    unsigned pressed = rd32(sg + dev + 0x1A0), rep = rd32(sg + dev + 0x1AC);
    int cancel = (rd32(sg + 0x5D8) & pressed) != 0, decide = (rd32(sg + 0x5D4) & pressed) != 0;
    if (!g_open) {   /* the model closed (Restart, Leave, End co-op): the page leaves, the manager follows (hook_om_vf8) */
        np_exit_page(m, 0);
        return;
    }
    l1_view v0;
    l1_view_get(g_lobby, &v0);
    int sel0 = v0.sel;
    l1_event ev[6];
    int n = 0, hover = -1;
    int up = (rep & 0x10010) != 0, down = (rep & 0x40040) != 0;
    int stk = np_stick((void *)(uintptr_t)inp);
    if (!up && !down) up = stk == 1, down = stk == 2;
    hover = np_hover_row((void *)(uintptr_t)m, 0xD);
    if (cancel) ev[n++] = (l1_event){L1E_BACK, 0, 0};
    else if (up) ev[n++] = (l1_event){L1E_UP, 0, 0};
    else if (down) ev[n++] = (l1_event){L1E_DOWN, 0, 0};
    else {
        if (hover >= 0 && hover != g_np_hover && hover < v0.nrows) ev[n++] = (l1_event){L1E_HOVER, hover, 0};
        int cl = np_click((void *)(uintptr_t)m, 0xC) & 0xFF, cr = np_click((void *)(uintptr_t)m, 0xB) & 0xFF;
        if ((rep & 0x80080) || cl) ev[n++] = (l1_event){L1E_LEFT, 0, 0};
        else if ((rep & 0x20020) || cr) ev[n++] = (l1_event){L1E_RIGHT, 0, 0};
        else if (decide) ev[n++] = (l1_event){L1E_OK, 0, 0};
    }
    g_np_hover = hover;
    int lastkind = n ? ev[n - 1].kind : 0;
    for (int i = 0; i < n; i++) {
        l1_event_in(g_lobby, &ev[i]);
        g_events++;
    }
    int req = l1_take_request(g_lobby);
    l1_view v;
    l1_view_get(g_lobby, &v);
    int moved = v.sel != sel0, changed = 0;
    for (int k = 0; k < v.nrows && k < v0.nrows; k++)
        if (strcmp(v.rows[k].value, v0.rows[k].value)) changed = 1;
    if (moved) np_sound((void *)(uintptr_t)snd, 3, m);
    if (changed && !cancel) {
        np_sound((void *)(uintptr_t)snd, 0x26, m);
        unsigned node = rd32(m + OP_ROWS + 4 * (unsigned)(v.sel < NP_ROWS_MAX && v.sel >= 0 ? v.sel : 0));
        if (node) np_anim((void *)(uintptr_t)node, lastkind == L1E_LEFT ? 0x2777 : 0x2763);
    }
    wr32m(m + OP_COUNT, (unsigned)v.nrows);
    wr32m(m + OP_CURSOR, (unsigned)(v.sel < 0 ? 0 : v.sel));
    if (moved || g_np_prev_sel < 0) np_place_cursor((void *)(uintptr_t)m, 1);
    g_np_prev_sel = v.sel;
    np_texts(m, &v);
    if (req == L1R_CLOSE) np_exit_page(m, cancel);   /* B: back to the list (the model stays open until the Options flow ends) */
    else if (req == L1R_P_RESTART || req == L1R_P_END || req == L1R_P_LEAVE || req == L1R_P_P2) pz_request(req, "native page");
    else if (req == L1R_NET_START || req == L1R_NET_STOP || req == L1R_NET_KICK || req == L1R_NET_INVITE) net_request(req);
}

static int np_begin(unsigned pause)   /* 1 = the native flow was started (the caller must not open the overlay page) */
{
    if (!g_np_enabled || !g_np_installed || g_open) return 0;
    l1_env env;
    l1_raw raw;
    int pads;
    g_np_req = 1;   /* (before fill_env: the model's rows are the native ones) */
    g_pg = 1;
    read_raw(&raw, &pads, 1);
    fill_env(&env, pads);
    load_stock(env.lang);
    if (!g_lobby) g_lobby = l1_new();
    l1_open(g_lobby, &g_set, &env);
    if (g_net_rows) net1_attach_lobby(g_lobby);
    l1_input_reset(&g_in);
    g_opened_ms = GetTickCount();
    g_open = 1;
    g_pz_opens++;
    g_np_req_ms = GetTickCount();
    g_np_pause = pause;
    g_np_leaving = 0;
    g_np_age = 0;
    g_np_prev_sel = -1;
    g_np_hover = -1;
    memset(g_np_set_lab, 0, sizeof g_np_set_lab), memset(g_np_set_val, 0, sizeof g_np_set_val), g_np_set_help[0] = 0;
    /* the stock Options flow: the row's kind is Options (1) while the panel's state 0x15 asks for it; the sub-state of the flow is reset (the stock does it at state 7) */
    g_np_kind0_saved = (int)rd32(pause + PZ_KINDS);
    g_np_kind0_fix = 1;
    wr32m(pause + PZ_KINDS, 1);
    wr32m(pause + 0x2A8, 0);
    wr32m(pause + PZ_STATE, 0x14);
    r1_log("np: the native page is requested (pause object %08x, row kind %d -> 1, page kind %d)", pause, g_np_kind0_saved, g_np_kind);
    return 1;
}

static void np_abort(const char *why)
{
    r1_log("np: native page given up (%s): the pause menu goes on", why);
    g_np_req = 0;
    if (g_np_kind0_fix && g_np_pause) wr32m(g_np_pause + PZ_KINDS, (unsigned)g_np_kind0_saved);
    g_np_kind0_fix = 0;
    if (g_open) close_lobby(why);
}

/* the Options flow is over (manager state 8, or the manager is gone): the model closes (its changes are saved), the page slot is given back */
static void np_end(const char *why)
{
    if (!g_np_active) return;
    if (g_np_slot && rd32(g_np_slot) == (unsigned)(uintptr_t)np_handler) wr32m(g_np_slot, g_np_old_fn);
    g_np_active = 0;
    g_np_leaving = 0;
    r1_log("np: the Options flow is over (%s)", why);
    if (g_open) close_lobby("native page: options closed");
}

/* the category list shows ONE row, ours: its text, the others empty, the cursor on it; its help line is the page's */
static char g_np_row_label[160], g_np_row_help[240], g_np_row_help_set[240];
static int g_np_row_idx;
/* (the stock update writes the list's texts again in its first states: ours are set before and after every update of the manager while the list may be on screen) */
static void np_list_texts(unsigned top)
{
    static char empty[2] = "";
    for (unsigned k = 0; k < 9; k++) {
        unsigned node = rd32(top + 0x34C + 12 * k);
        if (node) np_set_text(node, (int)k == g_np_row_idx ? g_np_row_label : empty);
    }
}
static void np_relabel(unsigned mgr, unsigned top)
{
    int lang = detect_lang(), idx = 0;
    unsigned count = rd32(top + 0x2E8);
    for (unsigned i = 0; i < count && i < 9; i++)
        if (rd32(top + 0x2C4 + 4 * i) == (unsigned)g_np_kind) idx = (int)i;
    np_upper(l1_text(L1T_ROW_LABEL, lang), g_np_row_label, sizeof g_np_row_label);
    g_np_row_idx = idx;
    np_list_texts(top);
    wr32m(top + 0x2C0, (unsigned)idx);
    wr32m(mgr + 0x40, (unsigned)idx);
    snprintf(g_np_row_help, sizeof g_np_row_help, "%s", l1_text(L1T_PA_ROWHELP, lang));
    g_np_row_help_set[0] = 0;
    r1_log("np: the category list shows one row \"%s\" (row %d of %u, kind %d)", g_np_row_label, idx, count, g_np_kind);
}

/* the Options manager's update */
static void __attribute__((fastcall)) hook_om_vf8(void *self)
{
    unsigned mgr = (unsigned)(size_t)self;
    g_np_mgr_calls++;
    g_np_last_mgr = mgr;
    g_np_mgr_tick = GetTickCount();
    if (g_np_req || g_np_active) {
        if (g_np_kind0_fix && g_np_pause) {   /* the Options flow started: the pause row is Restart-kind again */
            wr32m(g_np_pause + PZ_KINDS, (unsigned)g_np_kind0_saved);
            g_np_kind0_fix = 0;
        }
        unsigned menu = np_menu_of(mgr), top = rd32(mgr + OM_TOP);
        int st = (int)rd32(mgr + OM_STATE);
        g_np_last_menu = menu;
        if (g_np_req && !g_np_active) {
            if (st == 0 && menu && top && rd32(menu + OP_ROWS) && rd32(menu + OP_MGR) == mgr && rd32(menu + OP_LABELS)) {
                g_np_slot = menu + 0x270 + 4 * (unsigned)g_np_kind;
                g_np_old_fn = rd32(g_np_slot);
                wr32m(g_np_slot, (unsigned)(uintptr_t)np_handler);
                np_relabel(mgr, top);
                g_np_active = 1;
                g_np_req = 0;
                g_np_opens++;
                g_np_age = 0;
                r1_log("np: the Options flow is ours (manager %08x menu %08x top %08x, page kind %d, handler slot %08x was %08x)", mgr, menu, top, g_np_kind, g_np_slot, g_np_old_fn);
            } else if (GetTickCount() - g_np_req_ms > 8000)
                np_abort("the options manager did not reach its first state in 8 s");
        } else if (g_np_active) {
            if (st <= 2 && top) np_list_texts(top);
            if (!g_open && st == 1) {   /* the page was closed by an action of the model (Restart, Leave ...): the Options flow ends, no second B */
                wr32m(mgr + OM_STATE, 7);
                r1_log("np: the model closed the page: the Options flow exits");
            }
            if (st == 8) np_end("manager state 8");
        }
    }
    orig_om_vf8(self);
    if (g_np_active && g_np_last_mgr == mgr) {
        unsigned top = rd32(mgr + OM_TOP);
        int st = (int)rd32(mgr + OM_STATE);
        if (st <= 2 && top) np_list_texts(top);
        if (st == 2 && g_open) {   /* the list is idle: its help line says what the page is (set after the stock update: it writes its own help text on a cursor move) */
            unsigned ho = rd32(NP_HELP);
            if (ho) np_set_text(rd32(ho + 0x29C), g_np_row_help);
        }
    }
}

static int np_install(void)
{
    char v[16];
    if (r1_env("REV1_NATIVEPAGE", v, sizeof v) && v[0] == '0') {
        g_np_enabled = 0;
        r1_log("np: native page disabled by REV1_NATIVEPAGE=0 (the overlay page)");
        return 0;
    }
    if (r1_env("REV1_NATIVE_KIND", v, sizeof v) && (v[0] == '0' || v[0] == '2')) g_np_kind = v[0] - '0';
    unsigned cur = rd32(OM_VTABLE + 4 * GM_VF8_SLOT);
    unsigned char b[8];
    if (cur != OM_VF8_ORIG || !r1_safe_read((void *)(uintptr_t)cur, b, sizeof b)) {
        r1_log("np: uOptionManager vtable slot 8 is %08x, expected %08x: the native page stays off", cur, OM_VF8_ORIG);
        g_np_enabled = 0;
        return 0;
    }
    orig_om_vf8 = (vf8_t)(uintptr_t)cur;
    void *fake = (void *)(uintptr_t)OM_VTABLE;
    if (!r1_patch_vtable(&fake, GM_VF8_SLOT, (void *)hook_om_vf8)) {
        r1_log("np: cannot patch the options manager's vtable slot");
        g_np_enabled = 0;
        return 0;
    }
    g_np_installed = 1;
    r1_log("np: native page ready (uOptionManager update %08x wrapped, page kind %d)", cur, g_np_kind);
    return 1;
}
