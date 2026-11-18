/* fix023: the second human's aim ray (native/aimray1.c), built for the 32-bit target (the decision reads 32-bit object addresses).
 *   1. the decision table (ar1_second_view)
 *   2. ar1_choose on fake objects: the first human, the second human, the AI partner, the whole view, a stale manager, a weapon with a holder
 *   3. apply / revert on a fake code map holding the bytes of build 13298487: all three sites or none, refusals, idempotence, the stub's register
 *   4. the build facts in the image (REV1_IMAGE, default ../../image/rerev-image.bin)
 * Build + run: sh tests/build.sh. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../native/aimray1.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } else printf("ok   %s\n", msg); } while (0)

static uint32_t A(const void *p) { return (uint32_t)(uintptr_t)p; }
static void W(uint8_t *o, uint32_t off, uint32_t v) { memcpy(o + off, &v, 4); }
static uint32_t R(const void *p, uint32_t off) { uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v; }

/* ---------------------------------------------------------------- 1. the decision table */
static void t_table(void)
{
    ar1_snap s = {1, 0x2000, 0, 960, 1080};
    CHECK(ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 1, 1), "a human (kind 1) on slot 1, own slot 0, split view up: the second view");
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 1, 0), "the own slot's human (the first human): stock");
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 2, 1), "the AI partner (kind 2): stock");
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 3, 1), "a remote puppet (kind 3): stock");
    CHECK(!ar1_second_view(&s, 0, 0x1000, 0x2000, 0x3000, 1, 1), "the lab switch off: stock");
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2004, 0x3000, 1, 1), "the camera root holds another manager 1 than the split saw: stock");
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 1, 2), "a slot above 1: stock");
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2000, 0, 1, 1), "no owner: stock");
    s.on = 0;
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 1, 1), "whole view (cutscene, menu, solo part, network): stock");
    s.on = 1, s.own = 1;
    CHECK(ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 1, 0), "own slot 1 (the lobby's swapped layout): the slot-0 human gets the second view");
    CHECK(!ar1_second_view(&s, 1, 0x1000, 0x2000, 0x3000, 1, 1), "own slot 1: the slot-1 human keeps the stock root");
}

/* ---------------------------------------------------------------- 2. ar1_choose on fake objects */
static uint8_t cam_real[AR1_FAKE_SIZE], mgr0[0x200], mgr1[0x200], u0[0x1000], u1[0x1000], uai[0x1000], hold[0x40];
static uint8_t w0[0x1400], w1[0x1400], wai[0x1400], w1h[0x1400];
static uint32_t cell;

static void t_choose(void)
{
    W(cam_real, AR1_OFF_MGR0, A(mgr0)), W(cam_real, AR1_OFF_MGR1, A(mgr1));
    W(cam_real, 0x48, 0), W(cam_real, 0x4C, 0), W(cam_real, 0x50, 960), W(cam_real, 0x54, 1080);
    cell = A(cam_real);
    W(u0, AR1_U_KIND, 1), W(u0, AR1_U_SLOT, 0);
    W(u1, AR1_U_KIND, 1), W(u1, AR1_U_SLOT, 1);
    W(uai, AR1_U_KIND, 2), W(uai, AR1_U_SLOT, 1);
    W(w0, AR1_W_OWNER, A(u0)), W(w1, AR1_W_OWNER, A(u1)), W(wai, AR1_W_OWNER, A(uai));
    W(w1h, AR1_W_OWNER, 0), W(w1h, AR1_W_HOLDER, A(hold)), W(hold, AR1_HOLDER_OWNER, A(u1));

    ar1_init(&ar1_g, A(&cell), 1);
    CHECK(ar1_choose(A(w1)) == A(cam_real), "no split view yet: the second human's weapon gets the real camera root (0.2.2 behaviour)");
    ar1_set_view(&ar1_g, 1, A(mgr1), 0, 960, 1080);
    CHECK(ar1_choose(A(w0)) == A(cam_real), "split view up: the first human's weapon keeps the real root (manager 0, viewport 0)");
    uint32_t f = ar1_choose(A(w1));
    CHECK(f == A(ar1_g.fake), "split view up: the second human's weapon gets the stand-in root");
    CHECK(R(ar1_g.fake, AR1_OFF_MGR0) == A(mgr1), "the stand-in's +0xCE0 = manager 1 (his own camera)");
    CHECK(R(ar1_g.fake, 0x48) == 0 && R(ar1_g.fake, 0x4C) == 0 && R(ar1_g.fake, 0x50) == 960 && R(ar1_g.fake, 0x54) == 1080,
          "the stand-in's rect = viewport 2's size at the origin (the half sizes the ray uses; the screen point stays the first human's 640 / 360)");
    CHECK(ar1_choose(A(w1h)) == A(ar1_g.fake), "an owner found through the holder (+0x12D0 -> +0x30): the second view too");
    CHECK(ar1_choose(A(wai)) == A(cam_real), "the AI partner's weapon: the real root (the muzzle ray is the stock AI path anyway)");
    CHECK(ar1_choose(0) == A(cam_real), "no weapon: the real root");
    W(cam_real, AR1_OFF_MGR1, A(mgr0));
    CHECK(ar1_choose(A(w1)) == A(cam_real), "the camera root's manager 1 changed (a load): the real root until the split's next pass");
    W(cam_real, AR1_OFF_MGR1, A(mgr1));
    ar1_set_view(&ar1_g, 0, 0, 0, 0, 0);
    CHECK(ar1_choose(A(w1)) == A(cam_real), "whole view again (cutscene / menu): the real root");
    ar1_set_view(&ar1_g, 1, A(mgr1), 0, 0, 1080);
    CHECK(ar1_choose(A(w1)) == A(cam_real), "a viewport 2 without a size yet (the first pass): the real root");
    ar1_set_view(&ar1_g, 1, A(mgr1), 0, 960, 1080);
    ar1_g.enabled = 0;
    CHECK(ar1_choose(A(w1)) == A(cam_real), "`split aim 0` (lab A / B): the real root for everybody");
    ar1_g.enabled = 1;
    CHECK(ar1_g.n_second >= 2 && ar1_g.last_owner == A(u1), "counters: the second-view answers and the last owner");
}

/* ---------------------------------------------------------------- 3. apply / revert on a fake code map */
#define MAP_LO 0x5EB000u
#define MAP_HI 0x5F0000u
static uint8_t map[MAP_HI - MAP_LO];
static uint8_t stubs[4096];
static uint32_t stub_used;
static int map_ok(uint32_t a, uint32_t n) { return a >= MAP_LO && a + n <= MAP_HI; }
static int stub_ok(uint32_t a, uint32_t n) { return a >= A(stubs) && a + n <= A(stubs) + sizeof stubs; }
static int f_read(void *u, uint32_t a, void *out, uint32_t n)
{
    (void)u;
    if (map_ok(a, n)) { memcpy(out, map + (a - MAP_LO), n); return 1; }
    if (stub_ok(a, n)) { memcpy(out, stubs + (a - A(stubs)), n); return 1; }
    return 0;
}
static int f_write(void *u, uint32_t a, const void *p, uint32_t n)
{
    (void)u;
    if (map_ok(a, n)) { memcpy(map + (a - MAP_LO), p, n); return 1; }
    if (stub_ok(a, n)) { memcpy(stubs + (a - A(stubs)), p, n); return 1; }
    return 0;
}
static uint32_t f_alloc(void *u, uint32_t n)
{
    (void)u;
    if (stub_used + n > sizeof stubs) return 0;
    uint32_t a = A(stubs) + stub_used;
    stub_used += (n + 15u) & ~15u;
    return a;
}
static int n_logs;
static void f_log(void *u, const char *line) { (void)u; n_logs++; printf("     [log] %s\n", line); }
static const ar1_mem M = {f_read, f_write, f_alloc, f_log, NULL};
static const uint8_t call_ebc10[5] = {0xE8, 0xFA, 0xD7, 0xFF, 0xFF};

static void fill_map(void)
{
    memset(map, 0x90, sizeof map);
    for (int i = 0; i < AR1_N_SITES; i++) {
        const ar1_site *s = &ar1_sites[i];
        memcpy(map + (s->addr - MAP_LO), ar1_site_orig, AR1_SITE_LEN);
        memcpy(map + (s->addr + AR1_SITE_LEN - MAP_LO), s->next, sizeof s->next);
        memcpy(map + (s->reg_addr - MAP_LO), s->reg_bytes, (size_t)s->reg_len);
    }
    memcpy(map + (0x5EE411u - MAP_LO), call_ebc10, 5);
}
static int sites_orig(void)
{
    for (int i = 0; i < AR1_N_SITES; i++)
        if (memcmp(map + (ar1_sites[i].addr - MAP_LO), ar1_site_orig, AR1_SITE_LEN)) return 0;
    return 1;
}
static int sites_call_stubs(const ar1_t *g)
{
    for (int i = 0; i < AR1_N_SITES; i++) {
        const uint8_t *p = map + (ar1_sites[i].addr - MAP_LO);
        if (p[0] != 0xE8 || ar1_sites[i].addr + 5 + R(p, 1) != g->stub[i]) return 0;
    }
    return 1;
}

static void t_apply(void)
{
    uint32_t fn = 0x10203040u;
    ar1_t g;
    fill_map();
    ar1_init(&g, 0x11E00B8u, 1);
    CHECK(ar1_apply(&g, &M, fn) && g.state == AR1_ON, "apply on the stock bytes: on");
    CHECK(sites_call_stubs(&g), "each of the three loads now calls its own stub");
    for (int i = 0; i < AR1_N_SITES; i++) {
        uint8_t code[AR1_STUB_LEN];
        f_read(NULL, g.stub[i], code, AR1_STUB_LEN);
        uint8_t push = code[48];
        uint32_t target = g.stub[i] + 54 + R(code, 50);
        char msg[160];
        snprintf(msg, sizeof msg, "stub %d: pushes %s (the weapon at %08x), calls the decision, ends with ret", i, ar1_sites[i].reg == AR1_REG_ESI ? "esi" : "ecx", ar1_sites[i].addr);
        CHECK(push == (ar1_sites[i].reg == AR1_REG_ESI ? 0x56 : 0x51) && code[49] == 0xE8 && target == fn && code[105] == 0xC3, msg);
    }
    CHECK(ar1_sites[0].reg == AR1_REG_ECX && ar1_sites[1].reg == AR1_REG_ESI && ar1_sites[2].reg == AR1_REG_ESI, "the weapon registers: ecx at 0x5EBC16, esi at 0x5EE3BB / 0x5EFB1F");
    const char *dump = getenv("REV1_AR1_DUMP");   /* (a disassembler check of the stub bytes: lab/f23/stubdis.py) */
    if (dump && *dump) {
        FILE *df = fopen(dump, "wb");
        uint8_t code[AR1_STUB_LEN];
        f_read(NULL, g.stub[1], code, AR1_STUB_LEN);
        if (df) fwrite(code, 1, AR1_STUB_LEN, df), fclose(df);
        printf("     (stub 1 bytes written to %s, base %08x)\n", dump, g.stub[1]);
    }
    uint32_t s0 = g.stub[0];
    CHECK(ar1_apply(&g, &M, fn) && g.stub[0] == s0 && sites_call_stubs(&g), "apply again: already ours, nothing new allocated");
    CHECK(ar1_revert(&g, &M) && sites_orig() && g.state == AR1_OFF, "revert: the stock bytes are back");
    CHECK(ar1_revert(&g, &M), "revert again: nothing to do");

    fill_map();
    ar1_init(&g, 0x11E00B8u, 1);
    map[0x5EFB24u - MAP_LO] ^= 0xFF;
    CHECK(!ar1_apply(&g, &M, fn) && g.state == AR1_MISMATCH && sites_orig(), "a context byte of the third site differs (another build): refused, NO site written");
    fill_map();
    ar1_init(&g, 0x11E00B8u, 1);
    map[0x5EE35Du - MAP_LO] = 0x89;
    CHECK(!ar1_apply(&g, &M, fn) && g.state == AR1_MISMATCH && sites_orig(), "the instruction that loads esi is not the expected one: refused, nothing written");
    fill_map();
    ar1_init(&g, 0x11E00B8u, 1);
    map[0x5EE412u - MAP_LO] ^= 1;
    CHECK(!ar1_apply(&g, &M, fn) && sites_orig(), "FUN_005EE350 does not call FUN_005EBC10 as expected: refused");
    int logs = n_logs;
    CHECK(!ar1_apply(&g, &M, fn) && n_logs == logs, "the same refusal again: logged once");
    fill_map();
    ar1_init(&g, 0x11E00B8u, 1);
    CHECK(ar1_apply(&g, &M, fn), "apply");
    map[ar1_sites[1].addr - MAP_LO] = 0xA1, W(map, ar1_sites[1].addr - MAP_LO + 1, 0x12345678u);
    CHECK(!ar1_revert(&g, &M) && g.state == AR1_MISMATCH, "revert while a site holds bytes that are neither ours nor stock: refused for that site");
    CHECK(map[ar1_sites[0].addr - MAP_LO] == 0xA1 && !memcmp(map + (ar1_sites[0].addr - MAP_LO), ar1_site_orig, AR1_SITE_LEN), "... the other sites are put back");
}

/* ---------------------------------------------------------------- 4. the build facts in the image */
static void t_image(void)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("(no executable image at %s: the build facts are not compared)\n", path); return; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)n);
    if (!img || fread(img, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); CHECK(0, "image read"); free(img); return; }
    fclose(fp);
#define IMG(va) (img + ((va) - 0x400000u))
    for (int i = 0; i < AR1_N_SITES; i++) {
        const ar1_site *s = &ar1_sites[i];
        char msg[200];
        snprintf(msg, sizeof msg, "image: %08x is `mov eax,[0x11E00B8]` with its context and the weapon register's load (%s)", s->addr, s->what);
        CHECK(!memcmp(IMG(s->addr), ar1_site_orig, AR1_SITE_LEN) && !memcmp(IMG(s->addr + AR1_SITE_LEN), s->next, sizeof s->next) &&
                  !memcmp(IMG(s->reg_addr), s->reg_bytes, (size_t)s->reg_len), msg);
    }
    CHECK(!memcmp(IMG(0x5EE411u), call_ebc10, 5), "image: FUN_005EE350 calls FUN_005EBC10 at 0x5EE411 (ecx = esi = the weapon, set at 0x5EE409)");
    int callers = 0;
    for (uint32_t va = 0x401000u; va + 5 < 0xF00000u && va + 5 - 0x400000u < (uint32_t)n; va++)
        if (*IMG(va) == 0xE8 && va + 5 + R(IMG(va), 1) == 0x5EBC10u) callers++;
    CHECK(callers == 1, "image: exactly one call of FUN_005EBC10 in the code (so its ecx is always the weapon)");
    CHECK(!memcmp(IMG(0x5EBC23u), "\x8b\xb0\xe0\x0c\x00\x00", 6), "image: FUN_005EBC10 takes the manager from [eax+0xCE0] (= manager 0 in the stock root)");
    CHECK(!memcmp(IMG(0x5EBCF1u), "\x8b\x48\x48\x8b\x50\x4c", 6) && !memcmp(IMG(0x5EBCFFu), "\x8b\x78\x54", 3) && !memcmp(IMG(0x5EBD17u), "\x8b\x48\x50", 3),
          "image: ... and the rect +0x48 / +0x4C / +0x54 / +0x50 through the same eax");
    CHECK(!memcmp(IMG(0x5EBD4Au), "\x8d\x84\x24", 3), "image: eax is overwritten at 0x5EBD4A (no other field of the root is read there)");
    CHECK(!memcmp(IMG(0x5EE3C9u), "\x8b\x40\x54", 3) && !memcmp(IMG(0x5EFB27u), "\x8b\x40\x4c", 3), "image: the other two sites read the rect and drop the pointer at once");
    CHECK(!memcmp(IMG(0x5EE3AEu), "\x83\xbf\x30\x0e\x00\x00\x01", 7) && !memcmp(IMG(0x5EFB12u), "\x83\xbf\x30\x0e\x00\x00\x01", 7),
          "image: both ray functions run their camera part only for an owner of kind 1 (a human): [edi+0xE30] == 1");
    free(img);
}

int main(void)
{
    t_table();
    t_choose();
    t_apply();
    t_image();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ALL OK", checks, fails);
    return fails ? 1 : 0;
}
