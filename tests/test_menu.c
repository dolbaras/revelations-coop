/*
 * Offline harness of the title menu glue (native/menu.c): a fake uGUI_GeneralMenu (vtable + object in plain memory), fake keyboard and pads, recording stubs for the capture / split
 * modules and the overlay. 32-bit native build:   sh tests/build_menu.sh && tests/build/test_menu.exe
 */
#include <windows.h>
#include <xinput.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/capture.h"
#include "../native/overlay.h"
#include "../native/r1.h"
#include "../native/split.h"
#include "../native/coop_net.h"
#include "../native/lobby_link.h"
#include "../native/net1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- stubs of the proxy / modules */
void r1_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (getenv("MENU_TEST_VERBOSE")) { printf("  log: "); vprintf(fmt, ap); printf("\n"); }
    va_end(ap);
}
int r1_env(const char *name, char *out, int size)
{
    DWORD n = GetEnvironmentVariableA(name, out, (DWORD)size);   /* (the C runtime's getenv does not see SetEnvironmentVariable) */
    return n > 0 && n < (DWORD)size;
}
int r1_safe_read(const void *addr, void *out, unsigned size)
{
    if (IsBadReadPtr(addr, size)) return 0;
    memcpy(out, addr, size);
    return 1;
}
void *r1_patch_vtable(void *obj, int index, void *hook)
{
    void **vt = *(void ***)obj;
    DWORD old;
    if (vt[index] == hook || !VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old)) return NULL;
    void *orig = vt[index];
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void *), old, &old);
    return orig;
}
static lt_frame_cb g_cb;
void lt_add_frame_cb(lt_frame_cb cb) { g_cb = cb; }

static int g_keys[256], g_pad[2];
static short g_lx[2], g_ly[2];
static int g_pad_present[2] = {1, 1};
int inject_vk_down(int vk) { return vk >= 0 && vk < 256 && g_keys[vk]; }
HWND testmode_window(void) { return NULL; }
int testmode_pad_peek(int user, void *out)
{
    if (user < 0 || user > 1 || !g_pad_present[user]) return 0;
    XINPUT_GAMEPAD *g = out;
    memset(g, 0, sizeof *g);
    g->wButtons = (WORD)g_pad[user], g->sThumbLX = g_lx[user], g->sThumbLY = g_ly[user];
    return 1;
}
static int g_swap_base;
void testmode_pad_swap_base(int on) { g_swap_base = on != 0; }
int testmode_pad_swap_base_get(void) { return g_swap_base; }
void *steam_context(void) { return NULL; }
const char *steam_persona_name(void) { return NULL; }   /* lobby-must: no Steam in the harness (the name falls back to "Player 1" / "Player 2") */
unsigned rolebarrier_ep_block_ms(void) { return 0; }
/* allep1: the solo / non-section block and the end kind as rolebarrier1_game.c reports them (the test sets them) */
static unsigned g_t_secblock;
static int g_t_secblock_solo, g_t_endkind;
unsigned rolebarrier_sec_block(int *solo)
{
    if (solo) *solo = g_t_secblock ? g_t_secblock_solo : 0;
    return g_t_secblock;
}
int rolebarrier_end_kind(void) { return g_t_endkind; }
static int g_t_spec;
int rolebarrier_spectating(unsigned *watched)
{
    if (watched) *watched = 0;
    return g_t_spec;
}
int coopsave_rec_role1(void) { return 0; }   /* rev1 live 16: no co-op save in the harness */
unsigned coopsave_rec_area(void) { return 0; }   /* fix021: the record's section (none in the harness) */
/* ep1-flow E5: the end of Episode 1 as rolebarrier1_game.c reports it (the test sets it) */
static unsigned g_t_ep1, g_t_ep1_closed;
static int g_t_ep1_local;
unsigned rolebarrier_ep1_end(int *local, unsigned *closed)
{
    if (local) *local = g_t_ep1 ? g_t_ep1_local : 0;
    if (closed) *closed = g_t_ep1 ? g_t_ep1_closed : 0;
    return g_t_ep1;
}
/* ep1-solo E3: the watch of the solo part as world1_game.c reports it (the test sets it) */
static unsigned g_t_watch_unit, g_t_watch_enters;
static int g_t_watch_on;
int solo1_watch_now(unsigned *watched, unsigned *enters)
{
    if (watched) *watched = g_t_watch_unit;
    if (enters) *enters = g_t_watch_enters;
    return g_t_watch_on;
}
static unsigned g_fake_unit;   /* lobby-must: the one "player unit" of the fake level (its class name answers uPl<...>) */
int classes_object_name(unsigned obj, char *out, int size)
{
    if (g_fake_unit && obj == g_fake_unit) {
        snprintf(out, (size_t)size, "uPl< Jill >");
        return 1;
    }
    (void)obj, (void)out, (void)size;
    return 0;
}
/* a fake level at the game's own address: [0x1169394] = sPlayer, +0x30 / +0x34 = the list, node +8 next, node +0x10 unit (menu.c level_present). 0 = not mapped here */
void menu_test_set_splayer(unsigned cell);
void menu_test_force_apply_on_close(void);   /* the next close_lobby applies the settings (as a changed setting does) */
static unsigned g_splayer_cell;   /* stands for [0x1169394] */
static int fake_level_map(void)
{
    menu_test_set_splayer((unsigned)(size_t)&g_splayer_cell);
    return 1;
}
static void fake_level(int on)
{
    static unsigned sp[16], node[8], unit[4];
    unsigned *slot = &g_splayer_cell;
    if (!on) { *slot = 0; g_fake_unit = 0; return; }
    memset(sp, 0, sizeof sp), memset(node, 0, sizeof node);
    g_fake_unit = (unsigned)(size_t)unit;
    node[0x10 / 4] = g_fake_unit, node[8 / 4] = 0;   /* one node, next = 0 (the end) */
    sp[0x30 / 4] = (unsigned)(size_t)node, sp[0x34 / 4] = 0;
    *slot = (unsigned)(size_t)sp;
}

static struct { int unbinds, own, binds, layout, enable; int bind_unit[8], bind_pad[8]; } rec;
static capture_state_t g_cs;
static split_state_t g_ss;
void capture_unbind(int u) { (void)u; rec.unbinds++; }
void capture_set_own(int s) { rec.own = s; }
void capture_bind(int unit, int pad) { if (rec.binds < 8) rec.bind_unit[rec.binds] = unit, rec.bind_pad[rec.binds] = pad; rec.binds++; }
void capture_get_state(capture_state_t *st) { *st = g_cs; }
void split_set_layout(int l) { rec.layout = l; }
void split_enable(int on) { rec.enable = on; }
void split_get_state(split_state_t *st) { *st = g_ss; }

static ov_scene g_drawn;
static int g_draws;
void overlay_draw(void *dev, const ov_scene *sc) { (void)dev; g_drawn = *sc; g_draws++; }
void overlay_release(void) {}
int overlay_row_at_client(int px, int py, int cw, int ch, int *vx, int *vy) { (void)px, (void)py, (void)cw, (void)ch; if (vx) *vx = 0; if (vy) *vy = 0; return -1; }
void overlay_set_hint_rect(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; }
void overlay_stats(char *out, int size) { snprintf(out, (size_t)size, "overlay stub"); }

/* ---------------------------------------------------------------- fake uGUI_GeneralMenu */
static int g_orig_calls, g_mgr_orig_calls;
static void __attribute__((fastcall)) fake_orig(void *self) { (void)self; g_orig_calls++; }
static void __attribute__((fastcall)) fake_mgr_orig(void *self) { (void)self; g_mgr_orig_calls++; }
static void **g_mvt;
static void **g_vt;
static int g_mgr_installed = 1;
static unsigned char *g_obj;
static void set_u32(unsigned off, unsigned v) { memcpy(g_obj + off, &v, 4); }

static void game_frame(void)   /* what the game does in one pass: the object's update through its vtable, then the logic-thread callbacks */
{
    ((void(__attribute__((fastcall)) *)(void *))g_vt[8])(g_obj);
    if (g_mvt[8] != (void *)fake_mgr_orig || g_mgr_installed) ((void(__attribute__((fastcall)) *)(void *))g_mvt[8])(g_obj);   /* the manager's update (the object is a dummy for it) */
    if (g_cb) g_cb();
}

static void frames(int n)
{
    for (int i = 0; i < n; i++) {
        game_frame();
        Sleep(8);
    }
}

static void press(int vk, int ms_frames)
{
    g_keys[vk] = 1;
    frames(ms_frames);
    g_keys[vk] = 0;
    frames(2);
}

static void status(char *out, int size) { menu_cmd("status", out, size); }


/* ---------------------------------------------------------------- the native row: fake singleton with the Title message table, fake AddItem, fake decide handler */
extern void menu_test_set_native(unsigned sgui_ptr_va, void *additem);
extern void menu_test_set_smain(unsigned smain_ptr_va);
extern unsigned menu_test_act(void *page, unsigned cancel, unsigned mouse, void *orig);
static unsigned g_sgui_ptr;
static unsigned g_add_calls, g_add_args[9];
static unsigned g_act_orig_calls;
static void __attribute__((thiscall)) fake_additem(void *page, unsigned p2, unsigned p3, unsigned p4, unsigned p5, unsigned p6, unsigned p7, unsigned p8, unsigned p9)
{
    unsigned char *pg = page;
    unsigned cnt;
    memcpy(&cnt, pg + 0x410, 4);
    g_add_calls++;
    g_add_args[0] = p2, g_add_args[1] = p3, g_add_args[2] = p4, g_add_args[3] = p5, g_add_args[4] = p6, g_add_args[5] = p7, g_add_args[6] = p8, g_add_args[7] = p9;
    memcpy(pg + 0x41C + 4 * cnt, &p2, 4);
    unsigned rowptr = (unsigned)(size_t)(pg + 0xA64 + 8 * cnt);
    memcpy(pg + 0x9B8 + 4 * cnt, &rowptr, 4);
    cnt++;
    memcpy(pg + 0x410, &cnt, 4);
}
static unsigned __attribute__((fastcall)) fake_act_orig(void *page, void *edx, unsigned cancel, unsigned mouse) { (void)page; (void)edx; (void)cancel; (void)mouse; g_act_orig_calls++; return 7; }
static unsigned char *mk_page(unsigned type, unsigned count, unsigned id0, unsigned id1, unsigned id2)
{
    unsigned char *p = calloc(1, 0xC00);
    memcpy(p + 0x9A0, &type, 4);
    memcpy(p + 0x410, &count, 4);
    memcpy(p + 0x41C, &id0, 4);
    memcpy(p + 0x420, &id1, 4);
    memcpy(p + 0x424, &id2, 4);
    unsigned one = 1;
    memcpy(p + 0x3FC, &one, 4);
    for (int i = 0; i < 4; i++) {   /* the row nodes (records at +0xA64 + 8 * i): the first three are placed by the page, the fourth stands at (0, 0) */
        unsigned char *nd = calloc(1, 0x100);
        float x = i < 3 ? 640.0f : 0.0f, y = i < 3 ? 411.0f + 30.0f * (float)i : 0.0f;
        memcpy(nd + 0x40, &x, 4), memcpy(nd + 0x44, &y, 4), memcpy(nd + 0xA0, &x, 4), memcpy(nd + 0xA4, &y, 4);
        unsigned np = (unsigned)(size_t)nd;
        memcpy(p + 0xA64 + 8 * i, &np, 4);
    }
    return p;
}

int main(void)
{
    char ini[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    snprintf(ini, sizeof ini, "%stest_menu_%lu.ini", tmp, GetCurrentProcessId());
    remove(ini);
    SetEnvironmentVariableA("REV1_COOP_INI", ini);
    SetEnvironmentVariableA("REV1_LANG", "eng");
    g_vt = calloc(32, sizeof(void *));
    g_vt[8] = (void *)fake_orig;
    g_mvt = calloc(32, sizeof(void *));
    g_mvt[8] = (void *)fake_mgr_orig;
    menu_test_set_mgr((unsigned)g_mvt, (unsigned)fake_mgr_orig);
    g_obj = calloc(1, 0xC00);
    set_u32(0x3FC, 1), set_u32(0x9A0, 3), set_u32(0x410, 3);

    /* wrong build: the vtable slot is not the expected one */
    menu_test_set_addrs((unsigned)g_vt, 0x12345678);
    menu_install();
    char st[1500];
    status(st, sizeof st);
    if (!strstr(st, "test_menu_")) {   /* never write the settings of a test into the game's real config folder (this happened once: a stray rev1_coop.ini with armed=1) */
        printf("ABORT: the settings path is not the temporary test file: %s\n", st);
        return 2;
    }
    CHECK(strstr(st, "disabled"));
    CHECK(g_vt[8] == (void *)fake_orig);   /* nothing patched */
    menu_cmd("open", st, sizeof st);
    CHECK(strstr(st, "disabled") || strstr(st, "not installed"));

    /* the right build */
    menu_test_set_addrs((unsigned)g_vt, (unsigned)fake_orig);
    menu_install();
    CHECK(g_vt[8] != (void *)fake_orig);
    CHECK(g_cb != NULL);
    g_orig_calls = 0;
    g_mgr_orig_calls = 0;
    frames(3);
    CHECK(g_orig_calls == 3 && g_mgr_orig_calls == 3);   /* the stock update and the page manager run */
    menu_present(NULL);
    CHECK(g_draws == 0);        /* menu in state 1: nothing is drawn */
    status(st, sizeof st);
    CHECK(strstr(st, "state 1 type 3"));

    /* the menu waits for input: the hint appears */
    set_u32(0x3FC, 4);
    frames(3);
    menu_present(NULL);
    CHECK(g_draws == 1 && g_drawn.kind == OV_HINT);
    CHECK(strstr(g_drawn.hint, "F6") && strstr(g_drawn.hint, "Co-Op Play"));

    /* F6 opens the lobby; the stock update is frozen */
    g_keys[VK_F6] = 1;
    frames(2);
    status(st, sizeof st);
    CHECK(strstr(st, "lobby OPEN"));
    g_keys[VK_F6] = 0;
    frames(2);
    int before = g_orig_calls, mbefore = g_mgr_orig_calls;
    frames(4);
    CHECK(g_orig_calls == before && g_mgr_orig_calls == mbefore);   /* frozen while the lobby is open: page update AND page manager (cursor / back keys) */
    menu_present(NULL);
    CHECK(g_drawn.kind == OV_LOBBY && !strcmp(g_drawn.v.title, "Co-Op Play"));
    CHECK(g_drawn.v.nrows == 2 && g_drawn.v.sel == 0);   /* the product screen (lobby-must): Co-Op < Off > and Done; Right gives the split rows */
    CHECK(rec.enable == 0 && rec.binds == 0);   /* nothing applied yet */

    /* keys: Right arms the co-op (left alone for the keyboard), Down + Right = player 1 plays the partner, Esc closes */
    press(VK_RIGHT, 2);
    menu_present(NULL);
    CHECK(g_drawn.v.cards_on);
    press(VK_DOWN, 2);
    press(VK_RIGHT, 2);
    menu_cmd("view", st, sizeof st);
    CHECK(strstr(st, "Parker (partner)"));
    g_keys[VK_ESCAPE] = 1;
    frames(2);                       /* the first poll closes the lobby */
    int oc = g_orig_calls, moc = g_mgr_orig_calls;
    frames(4);                       /* Esc is still held: the stock menu must stay frozen (Esc would take it back to the title screen) */
    CHECK(g_orig_calls == oc && g_mgr_orig_calls == moc);
    menu_present(NULL);
    int d0 = g_draws;
    menu_present(NULL);
    CHECK(g_draws == d0);            /* no overlay while the closing key is held */
    g_keys[VK_ESCAPE] = 0;
    frames(6);
    status(st, sizeof st);
    CHECK(strstr(st, "lobby closed"));
    CHECK(rec.enable == 1 && rec.own == 1 && rec.binds == 2);
    CHECK(rec.bind_unit[0] == 1 && rec.bind_pad[0] == 0 && rec.bind_unit[1] == 0 && rec.bind_pad[1] == 1);
    CHECK(rec.layout == SPLIT_SIDE_BY_SIDE);
    CHECK(g_swap_base == 0);
    /* the settings file */
    {
        FILE *f = fopen(ini, "rb");
        CHECK(f != NULL);
        char buf[400] = "";
        if (f) fread(buf, 1, sizeof buf - 1, f), fclose(f);
        CHECK(strstr(buf, "armed=1") && strstr(buf, "role=partner") && strstr(buf, "layout=side") && strstr(buf, "pads=normal"));
    }
    g_orig_calls = 0;
    frames(3);
    CHECK(g_orig_calls == 3);   /* the stock menu runs again */
    menu_present(NULL);
    CHECK(g_drawn.kind == OV_HINT && strstr(g_drawn.hint, "Co-op is ON") && strstr(g_drawn.hint, "Parker / Jill"));

    /* the gamepad BACK button opens it, B closes; layout and pad exchange by pad */
    g_pad[1] = 0x0020;
    frames(2);
    g_pad[1] = 0;
    frames(2);
    status(st, sizeof st);
    CHECK(strstr(st, "lobby OPEN"));
    g_pad[0] = 0x0001; frames(2); g_pad[0] = 0; frames(2);   /* up: wraps to Done */
    g_pad[0] = 0x0001; frames(2); g_pad[0] = 0; frames(2);   /* up: pads row */
    g_pad[1] = 0x1000; frames(2); g_pad[1] = 0; frames(2);   /* A: exchange pads */
    g_pad[0] = 0x0001; frames(2); g_pad[0] = 0; frames(2);   /* up: layout */
    g_pad[0] = 0x1000; frames(2); g_pad[0] = 0; frames(2);   /* A: top / bottom */
    g_pad[0] = 0x2000; frames(2); g_pad[0] = 0; frames(2);   /* B: close */
    status(st, sizeof st);
    CHECK(strstr(st, "lobby closed"));
    CHECK(rec.layout == SPLIT_TOP_BOTTOM && g_swap_base == 1);
    CHECK(rec.bind_pad[2] == 0 && rec.bind_pad[3] == 1);   /* player 1 stays on pad slot 0: the exchange is physical */

    /* Raid or any other block: the permanent pad exchange is taken off, back when the campaign is back */
    g_cs.blocked = CAP_BLOCK_RAID;
    frames(2);
    CHECK(g_swap_base == 0);
    g_cs.blocked = 0;
    frames(2);
    CHECK(g_swap_base == 1);

    /* console set / not idle states: the hint disappears while the menu animates, F6 does nothing then */
    set_u32(0x3FC, 6);
    frames(3);
    menu_present(NULL);
    int d = g_draws;
    menu_present(NULL);
    CHECK(g_draws == d || g_draws == d + 1);
    g_keys[VK_F6] = 1; frames(3); g_keys[VK_F6] = 0; frames(2);
    status(st, sizeof st);
    CHECK(strstr(st, "lobby closed"));
    set_u32(0x3FC, 4);
    frames(2);
    menu_cmd("set armed=0", st, sizeof st);
    CHECK(rec.enable == 0 && g_swap_base == 0);
    menu_cmd("open", st, sizeof st);
    CHECK(strstr(st, "lobby open"));
    menu_cmd("ev click 0", st, sizeof st);
    CHECK(strstr(st, "Co-Op"));
    menu_cmd("ev back", st, sizeof st);
    status(st, sizeof st);
    CHECK(strstr(st, "lobby closed"));

    /* ---- the native row: the main page (type 0, three rows with the label ids 8 / 9 / 10) gets a fourth row from the dead "menu_gamestart" message; its decide opens the lobby */
    {
        static const char invalid[] = "Invalid Message";
        unsigned *tab = calloc(46, 4), *res = calloc(0x100 / 4, 4), *sg = calloc(0x800 / 4, 4);
        for (int i = 0; i < 46; i++) tab[i] = (unsigned)(size_t)invalid;
        res[0x88 / 4] = 46;
        res[0x98 / 4] = (unsigned)(size_t)tab;
        sg[(0x108 + 4 * 6) / 4] = (unsigned)(size_t)res;
        g_sgui_ptr = (unsigned)(size_t)sg;
        menu_test_set_native((unsigned)(size_t)&g_sgui_ptr, fake_additem);
        unsigned char *other = mk_page(0, 3, 18, 19, 16);      /* the Campaign page: three rows of another kind */
        unsigned char *old_obj = g_obj;
        g_obj = other;
        frames(2);
        CHECK(g_add_calls == 0);                                 /* only the main page gets the row */
        unsigned char *main_page = mk_page(0, 3, 8, 9, 10);
        g_obj = main_page;
        frames(2);
        CHECK(g_add_calls == 1);                                 /* once per page object, before the stock update of that frame */
        CHECK(g_add_args[0] == 11 && g_add_args[1] == 6 && g_add_args[2] == 15 && g_add_args[3] == 6 && g_add_args[4] == 0 && g_add_args[5] == 1 && g_add_args[6] == 0 && g_add_args[7] == 0);
        CHECK(!strcmp((const char *)(size_t)tab[11], "Co-Op Play") && !strcmp((const char *)(size_t)tab[15], "Two players: split screen on one PC, or over the network."));
        CHECK(tab[10] == (unsigned)(size_t)invalid && tab[12] == (unsigned)(size_t)invalid);   /* nothing else of the table touched */
        unsigned cnt;
        memcpy(&cnt, main_page + 0x410, 4);
        CHECK(cnt == 4);
        {   /* the new row's node got the next place: (640, 501) */
            unsigned np;
            float x, y, ty;
            memcpy(&np, main_page + 0xA64 + 8 * 3, 4);
            memcpy(&x, (unsigned char *)(size_t)np + 0x40, 4), memcpy(&y, (unsigned char *)(size_t)np + 0x44, 4), memcpy(&ty, (unsigned char *)(size_t)np + 0xA4, 4);
            CHECK(x == 640.0f && y == 501.0f && ty == 501.0f);
        }
        frames(5);
        CHECK(g_add_calls == 1);                                 /* not again while the count is four */
        {   /* the page is rebuilt (its builder resets the count to three): the row is added again, same object */
            unsigned three = 3;
            memcpy(main_page + 0x410, &three, 4);
            frames(2);
            CHECK(g_add_calls == 2);
            unsigned c4;
            memcpy(&c4, main_page + 0x410, 4);
            CHECK(c4 == 4);
        }
        {   /* lang (owner 2026-11-08): without REV1_LANG the texts follow the game's own sMain.mLanguage (the Russian row in capitals like the game's rows of that language);
               the languages the game has no texts of (Korean, Finnish ... "oth") read English there, so does the mod */
            static unsigned char smain[0x2600];
            static unsigned smain_ptr;
            static const struct { unsigned m; const char *label; const char *help; } lc[] = {
                {10, "СОВМЕСТНАЯ ИГРА", "Два игрока: разделённый экран на одном ПК или по сети."}, {8, "协力游玩", NULL}, {7, "協力遊玩", NULL}, {21, "JOGO COOPERATIVO", NULL},
                {9, "JOGO COOPERATIVO", NULL}, {11, "TRYB WSPÓŁPRACY", NULL}, {12, "CO-OP SPELEN", NULL}, {0, "協力プレイ", NULL}, {4, "Koop-Spiel", NULL},
                {6, "Co-Op Play", NULL}, {20, "Co-Op Play", NULL}, {1, "Co-Op Play", "Two players: split screen on one PC, or over the network."}};
            unsigned three = 3, before = g_add_calls;
            SetEnvironmentVariableA("REV1_LANG", NULL);
            smain_ptr = (unsigned)(size_t)smain;
            for (unsigned i = 0; i < sizeof lc / sizeof lc[0]; i++) {
                memcpy(smain + 0x254C, &lc[i].m, 4);
                menu_test_set_smain((unsigned)(size_t)&smain_ptr);
                memcpy(main_page + 0x410, &three, 4);   /* the page is rebuilt: the row (and its texts) again */
                frames(2);
                CHECK(g_add_calls == before + i + 1);
                CHECK(!strcmp((const char *)(size_t)tab[11], lc[i].label));
                CHECK(!lc[i].help || !strcmp((const char *)(size_t)tab[15], lc[i].help));
            }
            SetEnvironmentVariableA("REV1_LANG", "eng");   /* back to the harness default */
            menu_test_set_smain(0);
            memcpy(main_page + 0x410, &three, 4);
            frames(2);
            CHECK(!strcmp((const char *)(size_t)tab[11], "Co-Op Play"));
        }
        /* decide handler: the new row opens the lobby, other rows / cancel go to the stock code */
        unsigned st4 = 4, cur;
        memcpy(main_page + 0x3FC, &st4, 4);
        cur = 1; memcpy(main_page + 0x408, &cur, 4);
        g_act_orig_calls = 0;
        CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 7 && g_act_orig_calls == 1);   /* row 1: stock */
        cur = 3; memcpy(main_page + 0x408, &cur, 4);
        CHECK(menu_test_act(main_page, 1, 0, fake_act_orig) == 7 && g_act_orig_calls == 2);   /* cancel on row 3: stock */
        CHECK(menu_test_act(other, 0, 0, fake_act_orig) == 7 && g_act_orig_calls == 3);       /* another page: stock */
        status(st, sizeof st);
        CHECK(strstr(st, "lobby closed"));
        menu_cmd("set armed=0", st, sizeof st);
        frames(60);   /* (the stock menu is frozen for a moment after settings were applied) */
        g_keys[VK_RETURN] = 1;   /* the decide key is still down when the lobby opens: it must not act inside the lobby (live: the A that opened it toggled the first row) */
        CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 1 && g_act_orig_calls == 3);   /* decide on the Co-Op row: handled, lobby open, stock code NOT run */
        frames(4);
        menu_present(NULL);
        CHECK(g_drawn.kind == OV_LOBBY);
        CHECK(!g_drawn.v.cards_on);   /* co-op still off */
        g_keys[VK_RETURN] = 0;
        frames(3);
        status(st, sizeof st);
        CHECK(strstr(st, "lobby OPEN"));
        menu_cmd("ev back", st, sizeof st);
        status(st, sizeof st);
        CHECK(strstr(st, "lobby closed"));

        /* ---- ready1 (2026-11-09): the ready gate. While a network link runs and the session has not started, the main page's Campaign row (the story start:
         * Continue / New Game / Load Game) is held on both PCs and the strip says what is missing; the other rows and Back stay stock; un-ready and a partner who
         * leaves keep the gate; after the START the host's Campaign is the stock one. The glue before ready1 passed the decide through: a host started the story
         * alone with the partner in the room, and the partner's ready later started the session in the middle of the host's level. */
        {
            cn_sim_real_time();
            net1_cfg gcfg;
            memset(&gcfg, 0, sizeof gcfg);
            gcfg.transport = "sim", gcfg.threaded = 0, gcfg.name = "Hosty";
            net1_configure(&gcfg);
            lk_config jc;
            memset(&jc, 0, sizeof jc);
            jc.name = "Joiny", jc.mod_version = R1_VERSION;
            lk_t *jk = lk_new_cfg(NULL, "sim", 0, &jc);
            char own2[48], err2[160];
            cur = 0; memcpy(main_page + 0x408, &cur, 4);   /* the cursor on Campaign (row 0, Title message 8) */
            int a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 7 && g_act_orig_calls == a0 + 1);   /* no link: the stock Campaign */
            CHECK(net1_start(L1N_HOST, "", 28440, err2, sizeof err2) == 1);
            for (int i = 0; i < 20; i++) net1_frame(), frames(1), Sleep(2);
            a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 1 && g_act_orig_calls == a0);   /* hosting, nobody joined yet: held */
            frames(2);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && !strcmp(g_drawn.hint, "Waiting for the other player to join...  F6 / BACK button: Co-Op Play") && g_drawn.strip_kind == L1S_WARN);
            if (g_drawn.kind != OV_STRIP) printf("  (gate, host alone: drawn kind %d: %s)\n", g_drawn.kind, g_drawn.hint);
            CHECK(menu_test_act(main_page, 1, 0, fake_act_orig) == 7 && g_act_orig_calls == a0 + 1);   /* Back: stock */
            cur = 1; memcpy(main_page + 0x408, &cur, 4);
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 7 && g_act_orig_calls == a0 + 2);   /* another row (Raid Mode): stock */
            CHECK(menu_test_act(other, 0, 0, fake_act_orig) == 7);   /* another page: stock */
            cur = 0; memcpy(main_page + 0x408, &cur, 4);
            CHECK(lk_start(jk, L1N_JOIN, "sim:28440", 0, own2, sizeof own2, err2, sizeof err2) == 1);
            for (int i = 0; i < 100; i++) net1_frame(), lk_poll(jk), frames(1), Sleep(2);
            CHECK(lk_state(net1_link_peek()) == L1NS_CONNECTED && !net1_session_active());
            a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 1 && g_act_orig_calls == a0);   /* the partner is in the room, nobody ready: held */
            frames(2);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && !strcmp(g_drawn.hint, "Choose 'I am ready' when you are set.  F6 / BACK button: Co-Op Play"));
            lk_set_ready(jk, 1);   /* the partner marks ready: the strip follows while it shows */
            for (int i = 0; i < 60; i++) net1_frame(), lk_poll(jk), frames(1), Sleep(2);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && !strcmp(g_drawn.hint, "Joiny is ready.  Choose 'I am ready' when you are set.  F6 / BACK button: Co-Op Play"));
            if (!strstr(g_drawn.hint, "Joiny is ready")) printf("  (gate, partner ready: drawn kind %d: %s)\n", g_drawn.kind, g_drawn.hint);
            lk_set_ready(jk, 0);   /* ... and un-ready */
            for (int i = 0; i < 60; i++) net1_frame(), lk_poll(jk), frames(1), Sleep(2);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && !strcmp(g_drawn.hint, "Choose 'I am ready' when you are set.  F6 / BACK button: Co-Op Play"));
            net1_cmd("ready 1", st, sizeof st);   /* the host is ready, the partner not: still held */
            for (int i = 0; i < 60; i++) net1_frame(), lk_poll(jk), frames(1), Sleep(2);
            CHECK(!net1_session_active());
            a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 1 && g_act_orig_calls == a0);
            frames(2);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && !strcmp(g_drawn.hint, "You are ready. Waiting for the other player..."));
            lk_set_ready(jk, 1);   /* both ready: the host's link starts the session; the Campaign row is the stock one again (the host starts the story) */
            for (int i = 0; i < 100; i++) net1_frame(), lk_poll(jk), frames(1), Sleep(2);
            CHECK(net1_session_active());
            a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 7 && g_act_orig_calls == a0 + 1);
            frames(2);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, "Start the story") != NULL);   /* the gate's line is gone: the host's "what now" */
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "strip 0 ") != NULL);   /* the gate's event strip is cleared (the running HOST_GO line is drawn above) */
            if (!strstr(st, "strip 0 ")) printf("  (gate, after START: %s)\n", st);
            /* rev1 live 16: the host's title Continue put the co-op save's roles back (lk_force_roles after the START, before the first level): both links follow,
             * the line names this PC's character (the glue before it: no role change after the START, no line) */
            lk_info hi, ji;
            lk_get_info(net1_link_peek(), &hi);
            int was = hi.role, want = 1 - hi.role;   /* (this harness's room left the host as the partner) */
            CHECK(hi.started && (was == 0 || was == 1));
            CHECK(lk_force_roles(net1_link_peek(), want) == 1);
            for (int i = 0; i < 40; i++) net1_frame(), lk_poll(jk), frames(1), Sleep(2);
            lk_get_info(jk, &ji);
            lk_get_info(net1_link_peek(), &hi);
            CHECK(hi.role == want && ji.role == was);   /* both links follow */
            menu_present(NULL);
            char wl[120];
            snprintf(wl, sizeof wl, "'Continue' keeps the characters of the co-op save. Your character: %s.", want ? "partner" : "lead character");
            CHECK(g_drawn.kind == OV_STRIP && !strcmp(g_drawn.hint, wl));
            if (!strstr(g_drawn.hint, "co-op save")) printf("  (role lock strip: drawn kind %d: %s)\n", g_drawn.kind, g_drawn.hint);
            CHECK(lk_force_roles(jk, was) == -1 && lk_force_roles(net1_link_peek(), want) == 0);   /* the guest never; already so */
            lk_stop(jk);
            net1_stop();
            lk_free(jk);
            for (int i = 0; i < 20; i++) net1_frame(), frames(1), Sleep(2);
            a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 7 && g_act_orig_calls == a0 + 1);   /* no link any more: stock */
            /* the guest: connected and not ready -> held too; a partner (host) who leaves ends the gate with the link */
            gcfg.name = "Guesty";
            net1_configure(&gcfg);
            lk_config hc2;
            memset(&hc2, 0, sizeof hc2);
            hc2.name = "Hosty", hc2.mod_version = R1_VERSION;
            lk_t *hk2 = lk_new_cfg(NULL, "sim", 0, &hc2);
            CHECK(lk_start(hk2, L1N_HOST, "", 28441, own2, sizeof own2, err2, sizeof err2) == 1);
            CHECK(net1_start(L1N_JOIN, "sim:28441", 0, err2, sizeof err2) == 1);
            for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk2), frames(1), Sleep(2);
            CHECK(lk_state(net1_link_peek()) == L1NS_CONNECTED);
            lk_set_ready(hk2, 1);
            for (int i = 0; i < 60; i++) net1_frame(), lk_poll(hk2), frames(1), Sleep(2);
            a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 1 && g_act_orig_calls == a0);
            frames(2);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && !strcmp(g_drawn.hint, "Hosty is ready.  Choose 'I am ready' when you are set.  F6 / BACK button: Co-Op Play"));
            if (!strstr(g_drawn.hint, "Hosty is ready")) printf("  (gate, guest: drawn kind %d: %s)\n", g_drawn.kind, g_drawn.hint);
            lk_stop(hk2);   /* the host closes the room: the link ends (ERROR), the gate with it */
            for (int i = 0; i < 100; i++) net1_frame(), frames(1), Sleep(2);
            a0 = g_act_orig_calls;
            CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 7 && g_act_orig_calls == a0 + 1);
            net1_stop();
            lk_free(hk2);
            for (int i = 0; i < 20; i++) net1_frame(), frames(1), Sleep(2);
            menu_cmd("status", st, sizeof st);
        }

        /* ---- lobby-must: the guest after START (sim transport, the partner host in this process): the title menu takes no decide, the strip says what happens,
         * the host's end of the session is shown with its reason */
        cn_sim_real_time();
        net1_cfg nc;
        memset(&nc, 0, sizeof nc);
        nc.transport = "sim", nc.threaded = 0, nc.name = "Guesty";
        net1_configure(&nc);
        lk_config hc;
        memset(&hc, 0, sizeof hc);
        hc.name = "Hosty", hc.mod_version = R1_VERSION;   /* the glue sends this mod's version (net1_set_identity in menu_install): the same on both */
        lk_t *hk = lk_new_cfg(NULL, "sim", 0, &hc);
        char own[48], err[160];
        CHECK(lk_start(hk, L1N_HOST, "", 28420, own, sizeof own, err, sizeof err) == 1);
        {   /* ep1-flow E12: this harness host is a bare link (no net1): its session flags byte is set by hand, as the game's net1 does (Downed / revive 120 s = 5) */
            lk_settings hs;
            memset(&hs, 0, sizeof hs);
            hs.episode = 1, hs.difficulty = 1, hs.flags = 5;
            CHECK(lk_set_settings(hk, &hs) == 1);
        }
        CHECK(net1_start(L1N_JOIN, "sim:28420", 0, err, sizeof err) == 1);
        for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
        CHECK(lk_state(net1_link_peek()) == L1NS_CONNECTED);
        net1_cmd("ready 1", st, sizeof st);
        lk_set_ready(hk, 1);
        for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
        CHECK(net1_session_active());
        menu_cmd("net", st, sizeof st);
        CHECK(strstr(st, "hold 1") && strstr(st, "played 0"));
        menu_present(NULL);
        CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, "Waiting for Hosty to start the story"));
        cur = 1; memcpy(main_page + 0x408, &cur, 4);
        int before_act = g_act_orig_calls;
        CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 1 && g_act_orig_calls == before_act);   /* the guest's own Continue / New Game: swallowed */
        CHECK(menu_test_act(main_page, 1, 0, fake_act_orig) == 7 && g_act_orig_calls == before_act + 1);   /* Back still works */
        cur = 3; memcpy(main_page + 0x408, &cur, 4);
        CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 1);   /* the native row still opens the lobby (to disconnect) */
        frames(4);
        status(st, sizeof st);
        CHECK(strstr(st, "lobby OPEN"));
        menu_cmd("view", st, sizeof st);
        CHECK(strstr(st, "Network: join") && strstr(st, "You play | Parker (partner)"));
        CHECK(strstr(st, "Downed / revive | 120 s") != NULL);   /* ep1-flow E12: the guest's row shows the host's choice from the session record */
        if (!strstr(st, "Downed / revive | 120 s")) printf("  (view: %s)\n", st);
        /* wp12 (2026-11-08 02:52:53, the guest): closing the lobby after the START ran menu_apply's "co-op OFF" path, whose capture_unbind(-1) + capture_set_own(-1)
         * took back the role barrier's capture_set_own(1) (the guard's pickup own slot 1 -> 0: the guest's human could pick up nothing, open no door). In a started
         * session the role barrier owns the own slot and the capture roles: the lobby's close must leave them (rec.own = 1 stands for the barrier's call at START) */
        rec.own = 1;
        int unbinds0 = rec.unbinds;
        menu_test_force_apply_on_close();   /* live, the close saved changed settings (the guest's Mode / address rows), so close_lobby applied them: the same here */
        menu_cmd("ev back", st, sizeof st);
        frames(60);
        CHECK(rec.own == 1 && rec.unbinds == unbinds0);   /* the barrier's own slot and roles stay */
        if (rec.own != 1 || rec.unbinds != unbinds0) printf("  (lobby close in a session: own %d, unbinds %d -> %d)\n", rec.own, unbinds0, rec.unbinds);
        status(st, sizeof st);
        CHECK(strstr(st, "network session's roles stay") != NULL);   /* the close did apply (not skipped as unchanged) and said why the roles stay */
        /* the host ends the session: the guest's strip names the reason; nothing to return from at the title */
        lk_stop(hk);
        for (int i = 0; i < 100; i++) net1_frame(), frames(1), Sleep(2);
        menu_cmd("net", st, sizeof st);
        CHECK(strstr(st, "hold 0") && strstr(st, "quit at +0"));
        menu_present(NULL);
        CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, "The host has ended the session.") && g_drawn.strip_kind == L1S_WARN);
        cur = 1; memcpy(main_page + 0x408, &cur, 4);
        CHECK(menu_test_act(main_page, 0, 0, fake_act_orig) == 7);   /* the menu is the player's again */
        net1_stop();
        lk_free(hk);

        /* ---- the end at the title: a played session (a level since START) ends 1.5 s after the title menu waits for input again; the partner is told */
        if (fake_level_map()) {
            hk = lk_new_cfg(NULL, "sim", 0, &hc);
            CHECK(lk_start(hk, L1N_HOST, "", 28421, own, sizeof own, err, sizeof err) == 1);
            CHECK(net1_start(L1N_JOIN, "sim:28421", 0, err, sizeof err) == 1);
            for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
            net1_cmd("ready 1", st, sizeof st);
            lk_set_ready(hk, 1);
            for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
            CHECK(net1_session_active());
            unsigned st0 = 6;
            memcpy(main_page + 0x3FC, &st0, 4);   /* the title menu is not waiting (a level runs) */
            fake_level(1);
            for (int i = 0; i < 20; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "played 1") && strstr(st, "level 1") && strstr(st, "hold 0"));   /* the guest has its level: no hold */
            fake_level(0);
            st0 = 4;
            memcpy(main_page + 0x3FC, &st0, 4);   /* back at the title, waiting for input */
            /* lm2 live: the host's session ended 25 ms after its return to the title (since = now | 1 is now + 1 for an even tick: the unsigned now - since of a
             * pass in the same tick was 0xFFFFFFFF > 1500). The 1.5 s hold: still running a few passes later */
            for (int i = 0; i < 40; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(1);
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "ended by the title 0") && net1_session_active());
            /* (rev1 live 10: the section follow no longer goes through the title, so a played session at the title always ends) */
            for (int i = 0; i < 220; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(10);
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "ended by the title 1") && !net1_session_active());
            for (int i = 0; i < 50; i++) lk_poll(hk), Sleep(2);
            lk_info hi;
            lk_get_info(hk, &hi);
            CHECK(hi.peer_gone == 1 && hi.peer_gone_kind == L1RS_PARTNER_LEFT);   /* the host reads "<guest> has left" */
            lk_stop(hk);
            lk_free(hk);
            /* the guest whose host is gone while it plays: the strip, and the return to the title is planned 5 s later */
            hk = lk_new_cfg(NULL, "sim", 0, &hc);
            CHECK(lk_start(hk, L1N_HOST, "", 28422, own, sizeof own, err, sizeof err) == 1);
            CHECK(net1_start(L1N_JOIN, "sim:28422", 0, err, sizeof err) == 1);
            for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
            net1_cmd("ready 1", st, sizeof st);
            lk_set_ready(hk, 1);
            for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
            st0 = 6;
            memcpy(main_page + 0x3FC, &st0, 4);
            fake_level(1);
            for (int i = 0; i < 20; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
            lk_stop(hk);
            for (int i = 0; i < 60; i++) net1_frame(), frames(1), Sleep(2);
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "strip 5") && !strstr(st, "quit at +0 ms"));   /* "The host has ended the session.  Returning to the title screen..." + the return planned */
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, "Returning to the title screen"));
            /* lm5 live: back at the title the end at the title closes the ended link; its own "ended" event must not put a second strip ("connection lost")
             * over the host's reason */
            fake_level(0);
            st0 = 4;
            memcpy(main_page + 0x3FC, &st0, 4);
            for (int i = 0; i < 220; i++) net1_frame(), frames(1), Sleep(10);
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "ended by the title 2") && !strstr(st, "strip 6 ") && !strstr(st, "strip 7 "));   /* (the host's strip itself has run out by now) */
            if (!strstr(st, "ended by the title 2") || strstr(st, "strip 6 ") || strstr(st, "strip 7 ")) printf("  (menu net: %s)\n", st);
            lk_free(hk);
            /* ep1-flow E5: the end of Episode 1 closes the session on purpose. Variant 0: this guest passed the end itself (its own end flow runs): the host's close is
             * no loss: the Episode 1 strip, no return to the title. Variant 1: the guest was behind (the host's EPDONE only): the strip says so, back to the title */
            for (int variant = 0; variant < 2; variant++) {
                char tgt[24];
                snprintf(tgt, sizeof tgt, "sim:%d", 28423 + variant);
                hk = lk_new_cfg(NULL, "sim", 0, &hc);
                CHECK(lk_start(hk, L1N_HOST, "", 28423 + variant, own, sizeof own, err, sizeof err) == 1);
                CHECK(net1_start(L1N_JOIN, tgt, 0, err, sizeof err) == 1);
                for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
                net1_cmd("ready 1", st, sizeof st);
                lk_set_ready(hk, 1);
                for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
                CHECK(net1_session_active());
                st0 = 6;
                memcpy(main_page + 0x3FC, &st0, 4);
                fake_level(1);
                for (int i = 0; i < 20; i++) net1_frame(), lk_poll(hk), frames(1), Sleep(2);
                g_t_ep1 = GetTickCount() | 1, g_t_ep1_local = variant == 0, g_t_ep1_closed = 0;
                lk_stop(hk);
                for (int i = 0; i < 60; i++) net1_frame(), frames(1), Sleep(2);
                menu_cmd("net", st, sizeof st);
                if (variant == 0) CHECK(strstr(st, "strip 13 ") && strstr(st, "quit at +0 ms"));
                else CHECK(strstr(st, "strip 14 ") && !strstr(st, "quit at +0 ms"));
                if (!strstr(st, variant ? "strip 14 " : "strip 13 ")) printf("  (menu net, variant %d: %s)\n", variant, st);
                menu_present(NULL);
                if (variant == 0) CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, "Campaign complete! The co-op session has ended") && g_drawn.strip_kind == L1S_INFO);
                else CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, "The host has reached the end of the campaign.") && strstr(g_drawn.hint, "Returning to the title screen") && g_drawn.strip_kind == L1S_WARN);
                fake_level(0);
                st0 = 4;
                memcpy(main_page + 0x3FC, &st0, 4);
                for (int i = 0; i < 220; i++) net1_frame(), frames(1), Sleep(10);   /* back at the title: the ended link is closed quietly */
                net1_stop();
                lk_free(hk);
            }
            /* this PC closed the session itself after its own end flow: the strip comes at once */
            g_t_ep1 = GetTickCount() | 1, g_t_ep1_local = 1, g_t_ep1_closed = GetTickCount() | 1;
            frames(2);
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "strip 13 ") != NULL);
            /* allep1: the strip of an end of co-op by its kind (the campaign over, the solo part, not a section); and the host's solo / non-section block */
            {
                static const struct { int kind, strip; const char *text; } sc[] = {{1, 13, "complete! The co-op session has ended"}, {3, 15, "not a story section"}};
                for (unsigned i = 0; i < 2; i++) {
                    g_t_endkind = sc[i].kind;
                    Sleep(20);
                    g_t_ep1 = GetTickCount() | 1, g_t_ep1_local = 1, g_t_ep1_closed = GetTickCount() | 1;
                    frames(2);
                    menu_cmd("net", st, sizeof st);
                    char want[16];
                    snprintf(want, sizeof want, "strip %d ", sc[i].strip);
                    CHECK(strstr(st, want) != NULL);
                    menu_present(NULL);
                    CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, sc[i].text) && g_drawn.strip_kind == (sc[i].kind == 1 ? L1S_INFO : L1S_WARN));
                    if (!strstr(g_drawn.hint, sc[i].text)) printf("  (allep strip %d: %s / %s)\n", sc[i].kind, st, g_drawn.hint);
                }
                g_t_endkind = 0, g_t_ep1 = 0, g_t_ep1_local = 0, g_t_ep1_closed = 0;
                Sleep(20);
                g_t_secblock = GetTickCount() | 1, g_t_secblock_solo = 0;
                frames(2);
                menu_cmd("net", st, sizeof st);
                CHECK(strstr(st, "strip 15 ") != NULL);
                g_t_secblock = 0, g_t_secblock_solo = 0;
                frames(2);
            }
            /* ep1-solo E3: the story switched this PC's character off: once per watch the strip names the character this PC watches (the lead's name
             * when the class cannot be read: "Jill"); the same watch does not show it again */
            g_t_ep1 = 0, g_t_ep1_local = 0, g_t_ep1_closed = 0;
            g_t_watch_on = 1, g_t_watch_unit = 0x4321u, g_t_watch_enters = 1;
            frames(2);
            menu_cmd("net", st, sizeof st);
            CHECK(strstr(st, "strip 10 ") != NULL);
            menu_present(NULL);
            CHECK(g_drawn.kind == OV_STRIP && strstr(g_drawn.hint, "For now Jill goes on without you: you are watching until you meet again."));
            if (!strstr(g_drawn.hint, "goes on without you")) printf("  (menu net, watch: %s / drawn kind %d: %s)\n", st, g_drawn.kind, g_drawn.hint);
            g_t_watch_on = 0, g_t_watch_unit = 0;
            frames(2);
            /* the HOST passed the end: the partner's close is the end of Episode 1, not "<name> has left"; E12: the game's host publishes its flags byte by itself
             * (the lobby default on, 60 s = 1), a console `net set flags` wins for that link */
            g_t_ep1 = 0, g_t_ep1_local = 0, g_t_ep1_closed = 0;
            {
                lk_config gc;
                memset(&gc, 0, sizeof gc);
                gc.name = "Guesty2", gc.mod_version = R1_VERSION;
                lk_t *gk = lk_new_cfg(NULL, "sim", 0, &gc);
                CHECK(net1_start(L1N_HOST, "", 28430, err, sizeof err) == 1);
                CHECK(lk_start(gk, L1N_JOIN, "sim:28430", 0, own, sizeof own, err, sizeof err) == 1);
                for (int i = 0; i < 100; i++) net1_frame(), lk_poll(gk), frames(1), Sleep(2);
                lk_info gi;
                lk_get_info(gk, &gi);
                CHECK(lk_state(gk) == L1NS_CONNECTED && gi.set.flags == 1);
                CHECK(net1_session_flags() == 1);
                net1_cmd("set flags 7", st, sizeof st);
                for (int i = 0; i < 60; i++) net1_frame(), lk_poll(gk), frames(1), Sleep(2);
                lk_get_info(gk, &gi);
                CHECK(gi.set.flags == 7 && net1_session_flags() == 7);
                net1_cmd("set flags 15", st, sizeof st);   /* allep1: bit 3 (the room option) travels in the same byte, the guest reads it from the session record */
                for (int i = 0; i < 60; i++) net1_frame(), lk_poll(gk), frames(1), Sleep(2);
                lk_get_info(gk, &gi);
                CHECK(gi.set.flags == 15 && (gi.set.flags & 8) && net1_session_flags() == 15);
                net1_cmd("set flags 7", st, sizeof st);
                for (int i = 0; i < 60; i++) net1_frame(), lk_poll(gk), frames(1), Sleep(2);
                lk_get_info(gk, &gi);
                CHECK(gi.set.flags == 7 && net1_session_flags() == 7);
                net1_cmd("ready 1", st, sizeof st);
                lk_set_ready(gk, 1);
                for (int i = 0; i < 100; i++) net1_frame(), lk_poll(gk), frames(1), Sleep(2);
                CHECK(net1_session_active());
                lk_get_info(gk, &gi);
                CHECK(gi.started && gi.set.flags == 7);   /* final at the START: the guest's downed1 reads it there */
                st0 = 6;
                memcpy(main_page + 0x3FC, &st0, 4);
                fake_level(1);
                for (int i = 0; i < 20; i++) net1_frame(), lk_poll(gk), frames(1), Sleep(2);
                g_t_ep1 = GetTickCount() | 1, g_t_ep1_local = 1;
                lk_stop(gk);
                for (int i = 0; i < 100; i++) net1_frame(), frames(1), Sleep(2);
                menu_cmd("net", st, sizeof st);
                CHECK(strstr(st, "strip 13 ") && !strstr(st, "strip 3 "));
                if (!strstr(st, "strip 13 ")) printf("  (menu net, host: %s)\n", st);
                fake_level(0);
                st0 = 4;
                memcpy(main_page + 0x3FC, &st0, 4);
                net1_stop();
                lk_free(gk);
                g_t_ep1 = 0, g_t_ep1_local = 0;
                CHECK(net1_session_flags() == 7);   /* the console byte lasts until the next start of a link */
            }
        } else
            printf("(the fake level page at 0x01160000 could not be mapped: the end-at-the-title checks are skipped)\n");
        net1_shutdown();
        g_obj = old_obj;
        frames(3);
    }
    /* round 2: without a Steam persona the name is "Player 1" / "Player 2" in this PC's language (all 13 have both texts) */
    {
        char nm[64], st[200];
        net1_cfg nc0;
        memset(&nc0, 0, sizeof nc0);
        nc0.transport = "sim";   /* no configured name: the persona (none in the harness) or the fallback */
        net1_configure(&nc0);
        menu_cmd("lang eng", st, sizeof st);
        net1_name_for(L1N_HOST, nm, sizeof nm);
        CHECK(!strcmp(nm, "Player 1"));
        menu_cmd("lang rus", st, sizeof st);
        net1_name_for(L1N_JOIN, nm, sizeof nm);
        CHECK(!strcmp(nm, "Игрок 2"));
        menu_cmd("lang jpn", st, sizeof st);
        net1_name_for(L1N_HOST, nm, sizeof nm);
        CHECK(!strcmp(nm, "プレイヤー1"));
        int all = 1;
        for (int l = 0; l < L1_LANGS; l++) all &= l1_text(L1T_NAME_P1, l)[0] && l1_text(L1T_NAME_P2, l)[0] && strcmp(l1_text(L1T_NAME_P1, l), l1_text(L1T_NAME_P2, l));
        CHECK(all);
        menu_cmd("lang eng", st, sizeof st);
        /* round 2, the Steam way: the transport of the next start; coop_steam's auto-join line goes through net1_run_line */
        char o[300];
        CHECK(net1_set_way("steam") == 1 && !strcmp(net1_way(), "steam"));
        net1_cmd("way", o, sizeof o);
        CHECK(strstr(o, "Steam way") != NULL);
        net1_cmd("way ip", o, sizeof o);
        CHECK(!strcmp(net1_way(), "ip") && strstr(o, "direct IP"));
        CHECK(net1_set_way("carrier pigeon") == 0 && !strcmp(net1_way(), "ip"));
        /* lm6 live: `net join lobby:<id>` was refused as an address; the Steam way's targets */
        CHECK(net1_steam_target_ok("76561198000000042") && net1_steam_target_ok("lobby:109775243776000042") && net1_steam_target_ok("pending"));
        CHECK(!net1_steam_target_ok("") && !net1_steam_target_ok("lobby:") && !net1_steam_target_ok("lobby:12x") && !net1_steam_target_ok("192.168.0.1:28410") &&
              !net1_steam_target_ok("123456789012345678901"));
        net1_set_way("steam");
        net1_cmd("join lobby:109775243776000502x", o, sizeof o);
        CHECK(strstr(o, "Steam way: a Steam ID") != NULL);
        net1_set_way("ip");
        CHECK(net1_run_line("leave now", o, sizeof o) == 0 && strstr(o, "not understood"));
        CHECK(net1_run_line("join lobby:1 owl", o, sizeof o) == 0 && strstr(o, "unknown way"));
        CHECK(net1_take_autojoin() == 0);   /* rel010: a refused auto-join line opens nothing */
        lk_config hc;
        memset(&hc, 0, sizeof hc);
        hc.name = "Hosty", hc.mod_version = R1_VERSION;
        char own[48], err[160];
        lk_t *hk2 = lk_new_cfg(NULL, "sim", 0, &hc);
        CHECK(lk_start(hk2, L1N_HOST, "", 28433, own, sizeof own, err, sizeof err) == 1);
        CHECK(net1_run_line("join sim:28433 sim", o, sizeof o) == 1 && !strcmp(o, "joining"));   /* what the auto-join does, here over the sim transport */
        CHECK(net1_take_autojoin() == 1 && net1_take_autojoin() == 0);   /* rel010: the menu learns it once (it opens the co-op screen at the title) */
        for (int i = 0; i < 100; i++) net1_frame(), lk_poll(hk2), frames(1), Sleep(2);
        lk_t *gk = net1_link_peek();
        CHECK(gk && lk_mode(gk) == L1N_JOIN && !strcmp(lk_transport(gk), "sim") && lk_state(gk) == L1NS_CONNECTED);
        net1_stop();
        lk_stop(hk2);
        lk_free(hk2);
        net1_set_way("ip");
        net1_shutdown();
    }
    printf("%d checks, %d failed\n", checks, fails);
    remove(ini);
    printf(fails ? "FAILED\n" : "ALL OK\n");
    return fails ? 1 : 0;
}
