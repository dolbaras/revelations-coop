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
    printf("%d checks, %d failed\n", checks, fails);
    remove(ini);
    printf(fails ? "FAILED\n" : "ALL OK\n");
    return fails ? 1 : 0;
}
