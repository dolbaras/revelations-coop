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
int classes_object_name(unsigned obj, char *out, int size)
{
    (void)obj, (void)out, (void)size;
    return 0;
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
    CHECK(g_drawn.v.nrows == 7 && g_drawn.v.sel == 0);
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
    CHECK(g_drawn.kind == OV_HINT && strstr(g_drawn.hint, "Co-Op is ON") && strstr(g_drawn.hint, "Parker / Jill"));

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
        CHECK(!strcmp((const char *)(size_t)tab[11], "Co-Op Play") && !strcmp((const char *)(size_t)tab[15], "Two players, one PC (split screen)."));
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
        g_obj = old_obj;
        frames(3);
    }
    printf("%d checks, %d failed\n", checks, fails);
    remove(ini);
    printf(fails ? "FAILED\n" : "ALL OK\n");
    return fails ? 1 : 0;
}
