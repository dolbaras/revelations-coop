/*
 * Offline test of the narrow keyboard-mode fix (native/kbskip1.h, used by the kbskip hook of capture.c). Built twice by tests/build.sh: x86_64 and the 32-bit target (on x86 the hook
 * body calls the fake engine function through a real fastcall pointer, ecx = sGamePad, as the trampoline does in the game).
 *
 * A fake sGamePad is driven frame by frame in the order of sGamePad_vf6 (0x88E270): poll of both pads (FUN_009BDE20 layout: held with the stick-direction bits, previous held from the block
 * itself, pressed / released / changed edges, sticks), the keyboard-mode switch (FUN_00888FD0: a held bit on the CURRENT pad clears the mode, keyboard / mouse activity sets it), the
 * keyboard-mode function (a model of FUN_0088CB10: memset of both raw blocks, keyboard OR-ed into the current slot, mouse words +0x600.., PadData[current]+0x71 toggle), then the builder reads
 * both blocks. The partner's records reach the synthetic XInput user 1 at any time between frames (capture_push_remote -> testmode_pad_set); the poll of the next frame picks the latest.
 * When the executable image of the supported build is at hand (REV1_IMAGE, default ../../image/rerev-image.bin) the facts the fix relies on are compared with its bytes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/kbskip1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define GSIZE 0x900u
static uint8_t G[GSIZE];   /* the fake sGamePad */

static uint32_t rd(const uint8_t *g, uint32_t off) { return kb1_rd32(g, off); }
static void wr(uint8_t *g, uint32_t off, uint32_t v) { memcpy(g + off, &v, 4); }
static uint8_t *blk(uint8_t *g, unsigned k) { return g + KB1_RAW_OFF + KB1_RAW_STRIDE * k; }

/* raw block fields (offsets inside the block) */
enum { B_HELD = 0x00, B_PREV = 0x04, B_PRESSED = 0x08, B_RELEASED = 0x0C, B_CHANGED = 0x10, B_REPEAT = 0x14, B_RX = 0x18, B_RY = 0x1C, B_LX = 0x20, B_LY = 0x24 };

/* ---- the synthetic XInput users (what capture_push_remote / padhold leave there) ---- */
typedef struct { uint32_t bits; int lx, ly; } xpad;
static xpad user[2];

/* FUN_009BDE20: held = button bits | stick-direction bits (dead zone 7849 here), previous held from the block, edges */
static void poll(uint8_t *g, unsigned k)
{
    uint8_t *b = blk(g, k);
    uint32_t prev = rd(b, B_HELD), held = user[k].bits;
    if (user[k].ly > 7849) held |= 0x10000;
    if (user[k].ly < -7849) held |= 0x40000;
    if (user[k].lx > 7849) held |= 0x20000;
    if (user[k].lx < -7849) held |= 0x80000;
    uint32_t ch = held ^ prev;
    wr(b, B_HELD, held), wr(b, B_PREV, prev), wr(b, B_PRESSED, ch & held), wr(b, B_RELEASED, ~held & ch), wr(b, B_CHANGED, ch), wr(b, B_REPEAT, 0);
    wr(b, B_LX, (uint32_t)user[k].lx), wr(b, B_LY, (uint32_t)user[k].ly), wr(b, B_RX, 0), wr(b, B_RY, 0);
}

/* ---- the keyboard (what the keyboard object reports this frame) ---- */
#define K_ESC 0x1000u     /* the pause / start bit the keyboard maps Esc to (any bit: the model only ORs it) */
#define K_UP 0x10u
typedef struct { uint32_t held, prev, pressed, released; int up, activity; uint32_t mx, my; } kbd_t;
static kbd_t kbd;
static unsigned fake_calls;

/* FUN_00888FD0: a held bit on the current pad leaves keyboard mode, keyboard / mouse activity sets it */
static void kbswitch(uint8_t *g)
{
    uint32_t cur = rd(g, KB1_CUR_OFF);
    if (cur < 2 && rd(blk(g, cur), B_HELD) != 0)
        wr(g, KB1_MODE_OFF, 0);
    else if (kbd.activity)
        wr(g, KB1_MODE_OFF, 1);
}

/* a model of FUN_0088CB10 (thiscall ecx = sGamePad on x86) */
static void KB1_FC fake_88CB10(uintptr_t self)
{
    uint8_t *g = (uint8_t *)self;
    fake_calls++;
    if (!rd(g, KB1_MODE_OFF))
        return;
    memset(g + 0x198, 0, 0x2C);
    memset(g + 0x464, 0, 0x2C);
    uint32_t cur = rd(g, KB1_CUR_OFF);
    if (cur >= 2)
        return;
    uint8_t *b = blk(g, cur);
    wr(b, B_HELD, rd(b, B_HELD) | kbd.held | (kbd.up ? K_UP : 0));
    wr(b, B_PREV, rd(b, B_PREV) | kbd.prev);
    wr(b, B_PRESSED, rd(b, B_PRESSED) | kbd.pressed);
    wr(b, B_RELEASED, rd(b, B_RELEASED) | kbd.released);
    if (kbd.up)
        wr(b, B_LY, 0x7fff);
    wr(g, 0x600, kbd.mx), wr(g, 0x604, kbd.my);
    if (kbd.pressed & K_ESC)
        g[0x628 + 0x90 * cur + 0x71] ^= 1;   /* FUN_0088BB10(current slot): PadData[current]+0x71 */
}
static void KB1_FC skip_all(uintptr_t self) { (void)self; }   /* the FIRST kbskip: the whole function skipped */

static kb1_stats st;
typedef struct { uint32_t held[2], pressed[2], released[2]; int ly[2]; } seen_t;   /* what the builder reads */

/* one frame of sGamePad_vf6. fn = the trampoline (the model), mask = kept pads (0 = stock) */
static seen_t frame(uint8_t *g, kb1_fn fn, uint32_t mask)
{
    poll(g, 0), poll(g, 1);
    kbswitch(g);
    kb1_call(fn, (uintptr_t)g, mask, &st);
    seen_t s;
    for (unsigned k = 0; k < 2; k++)
        s.held[k] = rd(blk(g, k), B_HELD), s.pressed[k] = rd(blk(g, k), B_PRESSED), s.released[k] = rd(blk(g, k), B_RELEASED), s.ly[k] = (int)rd(blk(g, k), B_LY);
    return s;
}

static void reset(void)
{
    memset(G, 0, sizeof G);
    memset(user, 0, sizeof user);
    memset(&kbd, 0, sizeof kbd);
    memset(&st, 0, sizeof st);
    fake_calls = 0;
}

/* 1. no keyboard mode: the function returns at once; nothing is copied or put back */
static void t_no_kbmode(void)
{
    reset();
    user[1].ly = 20000, user[0].bits = 0;
    seen_t s = frame(G, fake_88CB10, KB1_DEFAULT_MASK);
    CHECK(fake_calls == 1);
    CHECK(s.ly[1] == 20000 && (s.held[1] & 0x10000));
    CHECK(st.calls == 1 && st.kbcalls == 0 && st.kept == 0);
    printf("ok   no keyboard mode: stock function, nothing kept\n");
}

/* 2 + 3. keyboard mode: stock wipes the partner's pad; narrow keeps it and everything else is byte-identical to stock */
static void t_narrow_vs_stock(void)
{
    static uint8_t A[GSIZE], B[GSIZE];
    reset();
    wr(G, KB1_MODE_OFF, 1);
    kbd.activity = 1, kbd.held = 0x200, kbd.pressed = 0x200, kbd.up = 1, kbd.mx = 640, kbd.my = 360;
    user[1].bits = 0x10, user[1].ly = -20000;
    poll(G, 0), poll(G, 1);
    kbswitch(G);
    CHECK(rd(G, KB1_MODE_OFF) == 1);   /* pad 0 has no held bit: keyboard mode stays */
    uint8_t polled1[KB1_RAW_SIZE];
    memcpy(polled1, blk(G, 1), KB1_RAW_SIZE);
    memcpy(A, G, GSIZE), memcpy(B, G, GSIZE);
    kb1_call(fake_88CB10, (uintptr_t)A, 0, &st);                  /* stock */
    kb1_call(fake_88CB10, (uintptr_t)B, KB1_DEFAULT_MASK, &st);   /* narrow kbskip */
    static const uint8_t zero[KB1_RAW_SIZE];
    CHECK(memcmp(blk(A, 1), zero, KB1_RAW_SIZE) == 0);            /* stock: the partner's pad is gone (the bug of sections 16-17) */
    CHECK(memcmp(blk(B, 1), polled1, KB1_RAW_SIZE) == 0);         /* narrow: exactly what the poll read */
    CHECK(memcmp(B, A, KB1_RAW_OFF + KB1_RAW_STRIDE) == 0);       /* up to the block of pad 1: identical (pad 0 = keyboard, Pad objects) */
    CHECK(memcmp(B + KB1_RAW_OFF + KB1_RAW_STRIDE + KB1_RAW_SIZE, A + KB1_RAW_OFF + KB1_RAW_STRIDE + KB1_RAW_SIZE, GSIZE - (KB1_RAW_OFF + KB1_RAW_STRIDE + KB1_RAW_SIZE)) == 0);   /* mouse words, PadData, mode */
    CHECK(rd(blk(B, 0), B_HELD) == (0x200u | K_UP) && rd(blk(B, 0), B_LY) == 0x7fff);   /* the keyboard drives pad 0 */
    CHECK(rd(B, 0x600) == 640 && rd(B, 0x604) == 360);
    CHECK(st.calls == 1 && st.kbcalls == 1 && st.kept == 1 && st.cur_hit == 0 && st.last_cur == 0);
    printf("ok   keyboard mode: stock wipes pad 1; narrow keeps pad 1 as polled, every other byte as stock (keyboard on pad 0, mouse, PadData)\n");
}

/* 4. the pause menu (duo6-9): Esc opens it and Esc closes it while kbskip is on; the first kbskip (whole function skipped) and `both` never see Esc */
typedef struct { int edges, open_mid, open_end, partner_frames; } menu_res;
static menu_res menu_run(kb1_fn fn, uint32_t mask)
{
    reset();
    wr(G, KB1_MODE_OFF, 1);
    user[1].ly = 20000;   /* the partner walks all the time */
    int menu = 0;
    menu_res r = {0, 0, 0, 0};
    for (int f = 0; f < 40; f++) {
        kbd.activity = 1;   /* keys / mouse: keyboard mode stays on all the time */
        kbd.held = (f == 5 || f == 6 || f == 20 || f == 21) ? K_ESC : 0;
        kbd.pressed = (f == 5 || f == 20) ? K_ESC : 0;   /* Esc down in frames 5 and 20: the keyboard reports the edge once */
        kbd.released = (f == 7 || f == 22) ? K_ESC : 0;
        seen_t s = frame(G, fn, mask);
        if (s.pressed[0] & K_ESC) {   /* the menu reads the pressed edge of the current slot */
            r.edges++;
            menu = !menu;
        }
        if (f == 12) r.open_mid = menu;
        r.partner_frames += s.ly[1] == 20000;
    }
    r.open_end = menu;
    return r;
}
static void t_pause_menu(void)
{
    menu_res r = menu_run(fake_88CB10, KB1_DEFAULT_MASK);
    CHECK(r.edges == 2 && r.open_mid == 1 && r.open_end == 0);   /* Esc opens the pause menu, Esc closes it */
    CHECK(r.partner_frames == 40);                               /* and the partner's stick was there in every frame */
    r = menu_run(fake_88CB10, 0);
    CHECK(r.edges == 2 && r.open_end == 0 && r.partner_frames == 0);   /* stock: the menu works, the partner is wiped (section 17) */
    r = menu_run(skip_all, 0);
    CHECK(r.edges == 0 && r.partner_frames == 40);   /* the first kbskip: Esc never reaches the game (duo7: title; duo6-9: the pause menu could not be closed) */
    r = menu_run(fake_88CB10, 3u);
    CHECK(r.edges == 0 && r.partner_frames == 40);   /* `cap kbskip 1 both` keeps pad 0 too: no keyboard on pad 0 (lab only, documented) */
    printf("ok   pause menu: with kbskip (pad 1 kept) Esc opens and closes it and the partner keeps walking; the whole-function skip and `both` drop Esc\n");
}

/* 5. the partner's button in keyboard mode: one pressed edge, held, one released edge (stock pad behaviour), stock = nothing */
static void t_partner_edges(void)
{
    for (int narrow = 0; narrow < 2; narrow++) {
        reset();
        wr(G, KB1_MODE_OFF, 1);
        int pressed = 0, released = 0, held = 0;
        for (int f = 0; f < 12; f++) {
            kbd.activity = 1;
            user[1].bits = (f >= 3 && f < 8) ? 0x1000u : 0;   /* the partner holds a button for 5 frames */
            seen_t s = frame(G, fake_88CB10, narrow ? KB1_DEFAULT_MASK : 0);
            pressed += !!(s.pressed[1] & 0x1000), released += !!(s.released[1] & 0x1000), held += !!(s.held[1] & 0x1000);
        }
        if (narrow)
            CHECK(pressed == 1 && released == 1 && held == 5);
        else
            CHECK(pressed == 0 && released == 0 && held == 0);
    }
    printf("ok   partner's button in keyboard mode: kept = 1 pressed edge, 5 held frames, 1 released edge; stock = none\n");
}

/* 6. the order of the partner's records against the frame: any number of records between two frames, the poll of the next frame takes the latest; never a wiped frame */
static void t_record_order(void)
{
    reset();
    wr(G, KB1_MODE_OFF, 1);
    unsigned seed = 12345;
    int bad = 0, last = 0;
    for (int f = 0; f < 300; f++) {
        kbd.activity = (f % 7) != 0;   /* mouse noise on most frames */
        int n = (int)((seed = seed * 1103515245u + 12345u) >> 16) % 4;   /* 0..3 records arrive before this frame's poll */
        for (int i = 0; i < n; i++) {
            last = 8000 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 20000;
            user[1].ly = last;   /* capture_push_remote -> testmode_pad_set(1, ...) */
        }
        seen_t s = frame(G, fake_88CB10, KB1_DEFAULT_MASK);
        if (f > 0 && last && s.ly[1] != last) bad++;
    }
    CHECK(bad == 0);
    CHECK(rd(G, KB1_MODE_OFF) == 1);   /* the partner's held bits do not clear keyboard mode: FUN_00888FD0 looks at the current slot only */
    printf("ok   records at any time between frames: the builder sees the latest record of the poll in all 300 frames\n");
}

/* 7. a stick-only synthetic pad 0 (lab `padhold 0 0 <ms> 0 20000 ...`) leaves keyboard mode by itself (stick-direction bit in held): no wipe, pad 0 drives Jill with narrow kbskip */
static void t_padhold0(void)
{
    reset();
    wr(G, KB1_MODE_OFF, 1);
    kbd.activity = 0;
    user[0].ly = 20000;
    seen_t s = frame(G, fake_88CB10, KB1_DEFAULT_MASK);
    CHECK(rd(G, KB1_MODE_OFF) == 0);
    CHECK(s.ly[0] == 20000 && (s.held[0] & 0x10000));
    CHECK(st.kbcalls == 0);
    printf("ok   stick-only pad 0 (lab padhold): keyboard mode is left before the wipe, pad 0 reaches the builder\n");
}

/* 8. the current slot is a kept pad (a swapped section of another thread): the puppet never takes this PC's keyboard; counted */
static void t_cur_is_kept(void)
{
    reset();
    wr(G, KB1_MODE_OFF, 1);
    wr(G, KB1_CUR_OFF, 1);
    kbd.activity = 1, kbd.held = 0x200, kbd.pressed = 0x200;
    user[1].bits = 0, user[1].ly = 15000;
    poll(G, 0), poll(G, 1);
    uint8_t polled1[KB1_RAW_SIZE];
    memcpy(polled1, blk(G, 1), KB1_RAW_SIZE);
    kb1_call(fake_88CB10, (uintptr_t)G, KB1_DEFAULT_MASK, &st);
    CHECK(memcmp(blk(G, 1), polled1, KB1_RAW_SIZE) == 0);
    CHECK(rd(blk(G, 0), B_HELD) == 0);   /* stock: pad 0 is wiped in such a frame anyway */
    CHECK(st.cur_hit == 1 && st.last_cur == 1);
    printf("ok   current slot = kept pad: pad 1 keeps the poll (no keyboard on the puppet), counted as cur_hit\n");
}

/* 9. masks and arguments */
static void t_masks(void)
{
    reset();
    wr(G, KB1_MODE_OFF, 1);
    user[1].ly = 20000;
    kbd.activity = 1;
    seen_t s = frame(G, fake_88CB10, 0xFCu);   /* no pad bit: stock */
    CHECK(s.ly[1] == 0 && st.calls == 0);
    kb1_keep k;
    CHECK(kb1_before(&k, NULL, 2u, &st) == 0 && k.mask == 0);
    kb1_after(&k, NULL, &st);
    CHECK(st.kept == 0);
    kbd.held = 0x200, kbd.up = 1;
    s = frame(G, fake_88CB10, 1u);   /* keep pad 0 only: pad 1 wiped (stock), pad 0 = as polled, the keyboard dropped */
    CHECK(s.ly[1] == 0 && s.ly[0] == 0 && s.held[0] == 0);
    s = frame(G, fake_88CB10, 2u);   /* the same frame with the default: the keyboard on pad 0, pad 1 kept */
    CHECK(s.ly[1] == 20000 && s.ly[0] == 0x7fff && s.held[0] == (0x200u | K_UP));
    CHECK(!strcmp(kb1_mask_name(2), "1") && !strcmp(kb1_mask_name(1), "0") && !strcmp(kb1_mask_name(3), "0+1") && !strcmp(kb1_mask_name(0), "none"));
    printf("ok   masks: bits outside the two pads = stock, NULL sGamePad, names\n");
}

/* 10. the facts of the build the fix relies on, in the real image */
static const uint8_t *img;
static size_t img_n;
static int img_has(uint32_t va, const char *hex)
{
    uint8_t want[32];
    int n = 0;
    for (const char *p = hex; *p && n < 32;) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) return 0;
        want[n++] = (uint8_t)v, p += 2;
    }
    size_t o = va - 0x400000u;
    return o + (size_t)n <= img_n && memcmp(img + o, want, (size_t)n) == 0;
}
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
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        CHECK(!"image read");
        return;
    }
    fclose(f);
    img = b, img_n = (size_t)n;
    CHECK(img_has(KB1_FN, "55 8B EC 83 E4 F8"));                        /* the prologue the hook steals (capture.c b_kb) */
    CHECK(img_has(0x88CB1Bu, "8B F1 83 BE 74 08 00 00 00"));             /* mov esi, ecx; cmp [esi+0x874], 0: keyboard mode */
    CHECK(img_has(0x88CB96u, "6A 2C 8D 96 98 01 00 00"));                /* push 0x2C; lea edx, [esi+0x198]: memset of pad 0 */
    CHECK(img_has(0x88CBB6u, "6A 2C 8D 86 64 04 00 00 6A 00 50 E8"));    /* push 0x2C; lea eax, [esi+0x464]; push 0; push eax; call memset: pad 1 */
    CHECK(KB1_RAW_OFF + KB1_RAW_STRIDE == 0x464u);
    CHECK(img_has(0x88DCB4u, "8B 8E 70 08 00 00 51"));                   /* FUN_0088BB10(current slot +0x870) at the end */
    CHECK(img_has(0x88DCC8u, "5F 5E 5B 8B E5 5D C3"));                   /* plain ret: no stack arguments */
    CHECK(img_has(0x88E458u, "8B CF E8 B1 E6 FF FF 33 F6"));             /* vf6: ecx = sGamePad, call 0x88CB10, the return value is not used */
    int callers = 0;
    for (size_t i = 0; i + 5 <= img_n; i++)
        if (img[i] == 0xE8 || img[i] == 0xE9) {
            int32_t rel;
            memcpy(&rel, img + i + 1, 4);
            if ((uint32_t)(0x400000u + i + 5 + (uint32_t)rel) == KB1_FN) callers++;
        }
    CHECK(callers == 1);
    printf("ok   image %s: prologue, keyboard-mode test, both memsets (0x198 / 0x464), current-slot tail, plain ret, the only caller (vf6) and its unused result\n", path);
    free(b);
    img = NULL;
}

int main(void)
{
    t_no_kbmode();
    t_narrow_vs_stock();
    t_pause_menu();
    t_partner_edges();
    t_record_order();
    t_padhold0();
    t_cur_is_kept();
    t_masks();
    t_image();
    printf("test_kbskip1 (%s): %d checks, %d failed\n", sizeof(void *) == 4 ? "x86" : "x86_64", checks, fails);
    return fails != 0;
}
