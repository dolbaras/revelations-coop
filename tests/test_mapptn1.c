/* Offline test of the minimap partner patch (native/mapptn1.c, mapptn1.h): the decision the stock game takes (no partner on the campaign minimap: the defect,
 * shown by the model of the stock code failing the co-op requirement) against the patched one, byte checks before every write, the exact bytes written,
 * the stub's jump targets, revert, refusal reasons, the switch rules, and the split's minimap cell swap (the second view's icons must come from the second
 * set's uGUI_Map). The fake memory holds the REAL bytes of the supported build (rerev.exe 13298487) around the patch site and the called function; when the
 * executable image is at hand (REV1_IMAGE, default ../../image/rerev-image.bin) every expected byte is also compared with it. Build + run: sh tests/build.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "../native/mapptn1.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } else printf("ok   %s\n", msg); } while (0)

/* ---- fake memory ---- */
#define R_SITE 0x7B7EC0u
#define R_FN   0x41BC10u
#define R_HEAP 0x30000000u
static uint8_t m_site[0x80], m_fn[0x10], m_heap[128];
static const uint8_t real_site[0x80] = {
    0x85, 0xc9, 0x75, 0x14, 0x68, 0xf8, 0xb0, 0xfe, 0x00, 0x6a, 0x3e, 0x68, 0x70, 0xb0, 0xfe, 0x00, 0xe8, 0x9b, 0x94, 0xc4, 0xff, 0x83, 0xc4, 0x0c, 0x8b, 0x15, 0x08, 0x91,
    0x16, 0x01, 0x8b, 0x42, 0x24, 0x83, 0x78, 0x20, 0x01, 0x75, 0x41, 0x8b, 0x0d, 0x94, 0x93, 0x16, 0x01, 0x85, 0xc9, 0x75, 0x14, 0x68, 0xf8, 0xb0, 0xfe, 0x00, 0x6a, 0x3e,
    0x68, 0x70, 0xb0, 0xfe, 0x00, 0xe8, 0x6e, 0x94, 0xc4, 0xff, 0x83, 0xc4, 0x0c, 0x8b, 0x0d, 0x94, 0x93, 0x16, 0x01, 0x8d, 0x54, 0x24, 0x13, 0x52, 0xe8, 0xfb, 0x3c, 0xc6,
    0xff, 0x89, 0x44, 0x24, 0x14, 0x3b, 0xc3, 0x74, 0x0f, 0x8b, 0xc8, 0xe8, 0xfc, 0xfc, 0xee, 0xff, 0x3c, 0x01, 0x75, 0x04, 0x89, 0x5c, 0x24, 0x14, 0x8b, 0x7c, 0x24, 0x1c,
    0x3b, 0xfb, 0x0f, 0x84, 0x73, 0x01, 0x00, 0x00, 0x8b, 0x4c, 0x24, 0x18, 0x83, 0xc7, 0x40, 0x57};
static const uint8_t real_fn[0x10] = {0x53, 0x55, 0x8b, 0xe9, 0x8b, 0x5d, 0x30, 0x56, 0x57, 0x3b, 0x5d, 0x34, 0x74, 0x5f, 0x8b, 0xff};
static int writes, fail_write, fail_alloc;
static char lastlog[240];
static int nlogs;

static uint8_t *at(uint32_t a, uint32_t n)
{
    if (a >= R_SITE && a + n <= R_SITE + sizeof m_site) return m_site + (a - R_SITE);
    if (a >= R_FN && a + n <= R_FN + sizeof m_fn) return m_fn + (a - R_FN);
    if (a >= R_HEAP && a + n <= R_HEAP + sizeof m_heap) return m_heap + (a - R_HEAP);
    return NULL;
}
static int f_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    uint8_t *p = at(a, n);
    if (!p) return 0;
    memcpy(out, p, n);
    return 1;
}
static int f_write(void *u, uint32_t a, const void *src, uint32_t n)
{
    (void)u;
    uint8_t *p = at(a, n);
    if (!p || fail_write) return 0;
    memcpy(p, src, n);
    writes++;
    return 1;
}
static uint32_t f_alloc(void *u, uint32_t n) { (void)u, (void)n; return fail_alloc ? 0 : R_HEAP; }
static void f_log(void *u, const char *l) { (void)u; snprintf(lastlog, sizeof lastlog, "%s", l); nlogs++; }
static const mp1_mem MEM = {f_read, f_write, f_alloc, f_log, NULL};

static void reset(void)
{
    memcpy(m_site, real_site, sizeof m_site), memcpy(m_fn, real_fn, sizeof m_fn), memset(m_heap, 0xCC, sizeof m_heap);
    writes = fail_write = fail_alloc = 0, lastlog[0] = 0, nlogs = 0;
}
static uint32_t rd32le(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* ---- 1. the decision: the co-op requirement against the stock code (the defect) and the patched code ---- */
typedef int (*decide_fn)(int mode, int exists, int role);
static int stock_d(int mode, int exists, int role) { return mp1_stock_partner_shown(mode, exists, role); }
static int fixed_d(int mode, int exists, int role) { return mp1_fixed_partner_shown(1, mode, exists, role); }
/* the requirement: in a campaign co-op game (mode 0) the other human is on the minimap whenever his unit exists, whatever role word the engine shows for
 * him at that moment (1 = a captured / local human, 2 = AI: the network puppet when its capture was lost for a moment, 3 = remote); Raid (mode 1) keeps the
 * stock rule (an AI partner hidden, a human shown) */
static int violations(decide_fn d)
{
    int bad = 0;
    for (int role = 1; role <= 3; role++) {
        if (!d(0, 1, role)) bad++;                      /* campaign co-op: shown */
        if (d(0, 0, role)) bad++;                       /* no partner unit: nothing */
        if (d(1, 1, role) != (role != 2)) bad++;        /* Raid: the stock rule */
    }
    return bad;
}
static void t_decision(void)
{
    int vs = violations(stock_d), vf = violations(fixed_d);
    printf("     (stock decision: %d violations of the co-op requirement, patched: %d)\n", vs, vf);
    CHECK(vs == 3, "MODEL BEFORE THE FIX FAILS: the stock uGUI_Map_vf9 never puts the co-op partner on the campaign minimap (3 of 3 campaign cases)");
    CHECK(vf == 0, "the patched decision: the campaign partner is shown in every role, Raid unchanged");
    CHECK(mp1_fixed_partner_shown(0, 0, 1, 1) == 0, "patch not written (single player): the stock campaign minimap, no partner");
    CHECK(mp1_fixed_partner_shown(1, 1, 1, 2) == 0 && mp1_fixed_partner_shown(1, 1, 1, 3) == 1, "patch written, Raid: the AI partner still hidden, a remote one shown");
}

/* ---- 2. the switch ---- */
static void t_want(void)
{
    mp1_t g;
    mp1_init(&g, -1);
    CHECK(!mp1_want(&g, 0, 0), "auto: a single-player game keeps the original bytes");
    CHECK(mp1_want(&g, 1, 0), "auto: a split screen (requested, campaign) writes the patch");
    CHECK(mp1_want(&g, 0, 1), "auto: a started network session writes the patch");
    mp1_init(&g, 0);
    CHECK(!mp1_want(&g, 1, 1), "REV1_MAPPTN=0: never (the stock minimap for a comparison)");
    mp1_init(&g, 1);
    CHECK(mp1_want(&g, 0, 0), "REV1_MAPPTN=1: always");
    mp1_init(&g, -1);
    g.mode = MP1_MODE_OFF;
    CHECK(!mp1_want(&g, 1, 1), "console revert wins over a co-op game");
    g.mode = MP1_MODE_ON;
    CHECK(mp1_want(&g, 0, 0), "console apply wins over single player");
}

/* ---- 3. the bytes ---- */
static void t_stub_bytes(void)
{
    uint8_t s[MP1_STUB_LEN + 4];
    mp1_targets t;
    mp1_targets_game(&t);
    int n = mp1_stub_build(s, R_HEAP, &t);
    CHECK(n == MP1_STUB_LEN && n == 46, "stub length 46");
    CHECK(memcmp(s, mp1_site_orig, 7) == 0, "the stub starts with the 7 bytes it replaces (mov eax,[edx+0x24]; cmp dword [eax+0x20],1)");
    CHECK(s[7] == 0x0f && s[8] == 0x84 && R_HEAP + 13 + rd32le(s + 9) == MP1_RAID_PATH, "je -> 0x7B7EE7 (Raid: the stock partner block)");
    CHECK(s[13] == 0x8b && s[14] == 0x0d && rd32le(s + 15) == MP1_SPLAYER, "mov ecx,[0x1169394] (sPlayer)");
    CHECK(s[21] == 0x0f && s[22] == 0x84 && R_HEAP + 27 + rd32le(s + 23) == MP1_NO_PARTNER, "je -> 0x7B7F28 when sPlayer is 0 (the stock no-partner store)");
    CHECK(memcmp(s + 27, "\x8d\x54\x24\x13\x52", 5) == 0, "lea edx,[esp+0x13]; push edx (the stock call's argument)");
    CHECK(s[32] == 0xe8 && R_HEAP + 37 + rd32le(s + 33) == MP1_PARTNER_FN, "call 0x41BC10 (the partner: first player whose slot is not the own slot)");
    CHECK(memcmp(s + 37, "\x89\x44\x24\x14", 4) == 0, "mov [esp+0x14],eax (vf9's partner slot)");
    CHECK(s[41] == 0xe9 && R_HEAP + 46 + rd32le(s + 42) == MP1_CONTINUE, "jmp 0x7B7F2C (the stock partner placement)");
    uint8_t site[MP1_SITE_LEN];
    mp1_site_build(site, R_HEAP);
    CHECK(site[0] == 0xe9 && MP1_SITE + 5 + rd32le(site + 1) == R_HEAP && memcmp(site + 5, "\x90\x90\x90\x90", 4) == 0, "site: jmp stub + 4 x nop");
    /* the stock code the site replaces, read off the real bytes: jnz lands on the no-partner store, the Raid block follows */
    const uint8_t *p = real_site + (MP1_SITE - R_SITE);
    CHECK(memcmp(p, mp1_site_orig, MP1_SITE_LEN) == 0 && MP1_SITE + 9 + p[8] == MP1_NO_PARTNER, "stock: 0x7B7EE5 jnz +0x41 = 0x7B7F28 (the campaign skips the partner)");
    CHECK(memcmp(real_site + (MP1_RAID_PATH - R_SITE), mp1_raid_path_bytes, 6) == 0 && memcmp(real_site + (MP1_NO_PARTNER - R_SITE), mp1_no_partner_bytes, 4) == 0 &&
              memcmp(real_site + (MP1_CONTINUE - R_SITE), mp1_continue_bytes, 4) == 0 && memcmp(real_fn, mp1_partner_fn_bytes, 6) == 0,
          "the expected bytes of every target are the real ones");
    /* the stock Raid block: call 0x41BC10 then the AI hide (cmp al,1 / jnz / mov [esp+0x14],ebx) */
    const uint8_t *call = real_site + (0x7B7F10u - R_SITE);
    CHECK(call[0] == 0xe8 && 0x7B7F15u + rd32le(call + 1) == MP1_PARTNER_FN, "stock Raid block calls 0x41BC10 at 0x7B7F10 (the stub calls the same function)");
}

static void t_apply_revert(void)
{
    mp1_t g;
    reset();
    mp1_init(&g, -1);
    CHECK(mp1_apply(&g, &MEM) == 1 && g.state == MP1_ON && g.stub == R_HEAP, "apply on the real bytes: ON");
    uint8_t want[MP1_SITE_LEN];
    mp1_site_build(want, R_HEAP);
    CHECK(memcmp(m_site + (MP1_SITE - R_SITE), want, MP1_SITE_LEN) == 0, "the 9 site bytes are jmp stub + nops");
    CHECK(m_heap[0] == 0x8b && m_heap[45] != 0xCC && m_heap[46] == 0xCC, "the stub (46 bytes) is in the executable block");
    CHECK(strstr(lastlog, "minimap partner patch written") != NULL, "logged: written");
    int w = writes;
    CHECK(mp1_apply(&g, &MEM) == 1 && writes == w, "apply again: ours already, nothing written");
    CHECK(mp1_revert(&g, &MEM) == 1 && g.state == MP1_OFF && memcmp(m_site, real_site, sizeof m_site) == 0, "revert: the original bytes back");
    CHECK(mp1_apply(&g, &MEM) == 1 && g.applies == 2, "apply after revert: the same stub again");
    mp1_revert(&g, &MEM);

    reset();
    mp1_init(&g, -1);
    m_site[MP1_SITE - R_SITE + 3] = 0x90;
    w = writes;
    CHECK(mp1_apply(&g, &MEM) == 0 && g.state == MP1_MISMATCH && writes == w, "foreign bytes at the site: refused, nothing written");
    CHECK(strstr(g.why, "bytes at 007b7ede are") != NULL, "the refusal names the bytes");
    int nl = nlogs;
    mp1_apply(&g, &MEM);
    CHECK(nlogs == nl && g.refusals == 1, "the same refusal again: logged and counted once");

    reset();
    mp1_init(&g, -1);
    m_site[MP1_CONTINUE - R_SITE] = 0x90;
    CHECK(mp1_apply(&g, &MEM) == 0 && g.state == MP1_MISMATCH && writes == 0, "a target of the stub differs (another build): refused, nothing written");

    reset();
    mp1_init(&g, -1);
    m_fn[2] = 0x90;
    CHECK(mp1_apply(&g, &MEM) == 0 && writes == 0, "the partner function differs: refused");

    reset();
    mp1_init(&g, -1);
    fail_alloc = 1;
    CHECK(mp1_apply(&g, &MEM) == 0 && g.state == MP1_ERROR && writes == 0, "no executable memory: refused, nothing written");

    reset();
    mp1_init(&g, -1);
    mp1_apply(&g, &MEM);
    m_site[MP1_SITE - R_SITE + 1] ^= 0xFF;   /* somebody else rewrote it */
    CHECK(mp1_revert(&g, &MEM) == 0 && g.state == MP1_MISMATCH, "revert over foreign bytes: left alone");
    char st[400];
    mp1_status(&g, &MEM, 1, 0, st, sizeof st);
    CHECK(strstr(st, "FOREIGN") != NULL && strstr(st, "split ON") != NULL, "status shows the live bytes and the switch inputs");
}

/* ---- 4. the split's second minimap: whose uGUI_Map the second view's icons read ---- */
/* model of the engine: uMapIcon_vf9 reads [cell] (= [sIDCockpit+0x2C]) -> manager -> +0x48 uGUI_Map; the second manager's update runs with or without the swap */
typedef struct { uint32_t map; } mgr_t;
static uint32_t icons_source(uint32_t *cell, uint32_t second, int with_swap, uint32_t stock)
{
    uint32_t back = with_swap ? mp1_swap_begin(*cell, stock, second) : 0;
    if (back) *cell = second;
    uint32_t seen = *cell;                     /* what FUN_00849780 answers inside the second set's uMapIcon_vf9 */
    if (mp1_swap_restore_ok(back, *cell, second)) *cell = back;
    return seen;
}
static void t_swap(void)
{
    uint32_t stock = 0x100, second = 0x200, cell = stock;
    CHECK(icons_source(&cell, second, 0, stock) == stock, "MODEL BEFORE THE FIX FAILS: without the swap the second view's icons come from the STOCK minimap (the first human's data)");
    CHECK(icons_source(&cell, second, 1, stock) == second && cell == stock, "with the swap: the second set's own uGUI_Map, and the cell is the stock manager again afterwards");
    cell = 0x300;   /* the engine rebuilt the cockpit (checkpoint load): the cell is not the stock manager we know */
    CHECK(mp1_swap_begin(cell, stock, second) == 0, "unknown stock manager in the cell: no swap");
    CHECK(mp1_swap_begin(0, stock, second) == 0 && mp1_swap_begin(stock, stock, 0) == 0, "no cockpit / no second set: no swap");
    CHECK(!mp1_swap_restore_ok(stock, 0x300, second), "the cell changed during the update (rebuilt): not put back, left to the engine");
    CHECK(mp1_swap_restore_ok(stock, second, second), "the cell still points at the second manager: put back");
}

/* ---- 5. the build facts in the image ---- */
static void t_image(void)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("(no executable image at %s: the build facts are not compared)\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)n);
    if (!img || fread(img, 1, (size_t)n, f) != (size_t)n) { fclose(f); CHECK(0, "image read"); free(img); return; }
    fclose(f);
#define IMG(a) (img + ((a) - 0x400000u))
    CHECK(memcmp(IMG(R_SITE), real_site, sizeof real_site) == 0, "image: the site region is the test's fake memory");
    CHECK(memcmp(IMG(R_FN), real_fn, sizeof real_fn) == 0, "image: FUN_0041BC10's first bytes");
    CHECK(rd32le(IMG(0x1082120u + 9 * 4)) == 0x7B7B30u, "image: uGUI_Map vtable slot 9 = 0x7B7B30 (the update that holds the site)");
    CHECK(rd32le(IMG(0x1082508u + 9 * 4)) == 0x7B95E0u, "image: uMapIcon vtable slot 9 = 0x7B95E0 (the icons read the uGUI_Map of [sIDCockpit+0x2C])");
    /* FUN_00849780 = mov eax,[ecx+0x2c]; ret (the minimap cell the swap acts on) */
    CHECK(memcmp(IMG(0x849780u), "\x8b\x41\x2c\xc3", 4) == 0, "image: FUN_00849780 returns [ecx+0x2C]");
    /* FUN_006A7C20 compares unit +0xE30 with 2 (the Raid AI hide the stub skips) */
    int found = 0;
    for (int i = 0; i < 36; i++)
        if (memcmp(IMG(0x6A7C20u) + i, "\x83\xbe\x30\x0e\x00\x00\x02", 7) == 0) found = 1;   /* cmp dword ptr [esi+0xe30],2 */
    CHECK(found, "image: FUN_006A7C20 tests unit +0xE30 == 2 (AI)");
    /* the map screen marker: the class vtable slot the wrapper replaces, the stock clone / add calls of the load, the player part slot, the placement */
    CHECK(rd32le(IMG(MP1_WM_VTABLE + 8 * 4)) == MP1_WM_VF8, "image: uGUI_WorldMapIcon vtable slot 8 = 0x7BED30 (the update the marker hangs on)");
    const uint8_t *c1 = IMG(0x7BE53Eu), *c2 = IMG(0x7BE548u);
    CHECK(c1[0] == 0xE8 && 0x7BE543u + rd32le(c1 + 1) == MP1_WM_CLONE && c2[0] == 0xE8 && 0x7BE54Du + rd32le(c2 + 1) == MP1_WM_ADD,
          "image: the stock load clones a part with 0xB9FC90 and adds it with 0xB9DBB0 (0x7BE53E / 0x7BE548)");
    CHECK(memcmp(IMG(0x7BE59Bu), "\x89\x86\x74\x07\x00\x00", 6) == 0, "image: part 7 (the player marker) is stored at +0x774 (0x7BE59B)");
    CHECK(memcmp(IMG(MP1_WM_PLACE), "\x55\x8b\xec\x83\xe4\xf0", 6) == 0, "image: the stock placement FUN_007BE8B0");
    const uint8_t *q = IMG(0x8D33D0u + 4);   /* FUN_008D33D0: push [esp+4]; call FUN_008D30D0 */
    found = 0;
    for (int i = 0; i < 8; i++)
        if (q[i] == 0xE8 && 0x8D33D0u + 4 + (uint32_t)i + 5 + rd32le(q + i + 1) == MP1_WM_POS) found = 1;
    CHECK(found, "image: FUN_008D33D0 (the player marker's position, FUN_007C10D0) calls FUN_008D30D0");
    /* FUN_008D30D0 is a METHOD of the world-map model: it saves ecx (0x8D30E4 mov [esp+0x1c],ecx); FUN_008D33D0 passes ecx on; FUN_007C10D0 loads
     * edi = [ebx+0x7c] (ebx = uSubMenuManager) and calls FUN_008D33D0 with ecx = edi (the first live try called it without ecx and crashed) */
    CHECK(memcmp(IMG(0x8D30E4u), "\x89\x4c\x24\x1c", 4) == 0, "image: FUN_008D30D0 keeps ecx (a thiscall on the world-map model)");
    CHECK(memcmp(IMG(0x7C10E1u), "\x8b\x7b\x7c", 3) == 0 && memcmp(IMG(0x7C1399u), "\x8b\xcf", 2) == 0 && IMG(0x7C13ABu)[0] == 0xE8 &&
              0x7C13B0u + rd32le(IMG(0x7C13ABu) + 1) == 0x8D33D0u, "image: the stock caller passes ecx = [uSubMenuManager+0x7C] (0x7C10E1 / 0x7C1399 / 0x7C13AB)");
    /* the split minimap's screen size: the 8 reads of [0x11E1B9C] (+0xB8 / +0xBC follow), the stock scale constants 1/1280 and 1/720 */
    int sites_ok = 0;
    for (int i = 0; i < MP1_N_SCREEN_SITES; i++) {
        int o = mp1_screen_imm_off(IMG(mp1_screen_site[i]));
        if (o > 0 && rd32le(IMG(mp1_screen_site[i]) + o) == MP1_SCREEN_PTR)
            sites_ok++;
    }
    CHECK(sites_ok == MP1_N_SCREEN_SITES, "image: the 8 minimap reads are mov reg,[0x11E1B9C] (0x7B7B39 .. 0x7BA0F3)");
    CHECK(memcmp(IMG(0x7B7B3Eu), "\x66\x0f\x6e\x80\xb8\x00\x00\x00", 8) == 0 && memcmp(IMG(0x7B7B67u), "\x8b\x88\xb8\x00\x00\x00\x66\x0f\x6e\x80\xbc\x00\x00\x00", 14) == 0,
          "image: uGUI_Map_vf9 reads the screen's +0xB8 (width) and +0xBC (height) right after (0x7B7B3E / 0x7B7B67)");
    float k1, k2;
    memcpy(&k1, IMG(0x108024Cu), 4);
    memcpy(&k2, IMG(0x1080250u), 4);
    CHECK(k1 > 0.000781f && k1 < 0.000782f && k2 > 0.001388f && k2 < 0.001389f, "image: the scale constants are 1/1280 (0x108024C) and 1/720 (0x1080250)");
    free(img);
}

/* ---- 5b. the split minimap in a half (mapptn1.h mp1_screen_site) ---- */
static void t_screen(void)
{
    /* live mm1s3 03:40: screen 1920 x 1080, uGUI_Map +0x40 = 1284.0 (= 0x44A08000), layout +0xED8 = 856.0 */
    float x_full = mp1_map_x(1920, 856.0f);
    CHECK(x_full > 1283.9f && x_full < 1284.1f, "the stock formula gives the live value 1284 for the whole 1920-wide screen");
    int hw, hh;
    mp1_half_size(1, 1920, 1080, &hw, &hh);
    CHECK(hw == 960 && hh == 1080, "side by side: a half is 960 x 1080");
    CHECK(x_full > (float)hw, "MODEL BEFORE THE FIX FAILS: with the whole screen's width the map content is at x 1284, outside the 960-wide half (only the frame shows)");
    float x_half = mp1_map_x(hw, 856.0f);
    CHECK(x_half > 641.9f && x_half < 642.1f && x_half < (float)hw && x_half / (float)hw == 856.0f / 1280.0f,
          "with the half's width: x 642, inside the half and at the same share of it as 856 of the 1280 layout");
    mp1_half_size(0, 1920, 1080, &hw, &hh);
    CHECK(hw == 1920 && hh == 540, "top / bottom: a half is 1920 x 540");
    CHECK(mp1_screen_imm_off((const uint8_t *)"\xa1\x9c\x1b\x1e\x01") == 1 && mp1_screen_imm_off((const uint8_t *)"\x8b\x0d\x9c\x1b") == 2 &&
              mp1_screen_imm_off((const uint8_t *)"\x8b\x15\x9c\x1b") == 2 && mp1_screen_imm_off((const uint8_t *)"\x8b\x88\xb8\x00") == -1,
          "the address's place in mov eax / ecx / edx,[abs]; any other instruction is refused");
}

/* ---- 6. the map screen marker decision ---- */
static void t_wm(void)
{
    float p[3] = {120.0f, 3.0f, -40.0f}, z[3] = {0, 0, 0};
    CHECK(sizeof(mp1_wm_slot) == 0x20 && offsetof(mp1_wm_slot, part) == 4 && offsetof(mp1_wm_slot, pos) == 0x10, "the marker slot is the stock 0x20-byte layout (on, part +4, pos +0x10)");
    CHECK(mp1_wm_show(1, 1, p), "co-op, the player marker on, the partner on the map: shown");
    CHECK(!mp1_wm_show(1, 0, p), "the player marker off (the map closing / another page): hidden");
    CHECK(!mp1_wm_show(1, 1, z), "the partner has no world-map position (another area / scene): hidden");
    CHECK(!mp1_wm_show(0, 1, p), "single player: hidden (the stock map)");
    /* live mm1s3 03:43: own and partner in one room -> both at world-map (171.5, -2.5, 0): the partner marker is drawn beside the player's */
    float a[3] = {171.5f, -2.5f, 0.0f}, b[3] = {171.5f, -2.5f, 0.0f}, c[3] = {140.0f, 12.0f, 0.0f};
    CHECK(mp1_wm_same_spot(a, b) && !mp1_wm_same_spot(a, c) && MP1_WM_SAME_DX > 8.0f,
          "one room (the same marker place): drawn beside the player's marker; another room: at its own place");
}

int main(void)
{
    t_decision();
    t_want();
    t_stub_bytes();
    t_apply_revert();
    t_swap();
    t_screen();
    t_image();
    t_wm();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
