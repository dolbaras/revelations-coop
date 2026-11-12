/*
 * Offline test of the second human's aim gate (native/aimgate1.h, used by swap_call of capture.c).
 *
 * A model of one frame of the engine: the own human's phase writes the global byte sGamePad+0x883 from HIS "may aim" (FUN_006B3100 -> FUN_00883C70(FUN_006AD740(own))),
 * then every human's sections read "aim pressed and allowed" (FUN_00888590: +0x882 && +0x883 && the aim button of the current pad slot). Stock: the second human aims only
 * while the own human may (the live finding of 2026-11-11). With the gate: inside his sections the byte is his own value, and the game's value is back after them.
 * When the executable image of the supported build is at hand (REV1_IMAGE, default ../../image/rerev-image.bin) the facts the gate relies on are compared with its bytes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/aimgate1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the model ---- */
typedef struct {
    unsigned slot;     /* unit+0xE2C */
    int taken, lost;   /* capture state */
    int bag;           /* unit+0xFE8 != NULL */
    int can;           /* what FUN_006AD740 answers about him (gun in the hand, no scanner, no forbidding state) */
    int aim_button;    /* LT held on his pad slot */
    int cache;         /* capu_t.aim_ok */
    int aimed;         /* result of the frame: unit+0x1151 */
    int weapon_saw;    /* what his weapon section read from FUN_00888590 */
} human;
static uint8_t en = 1, flag = 0;   /* sGamePad+0x882, +0x883 */

static int aim_pressed(const human *h) { return en && flag && h->aim_button; }   /* FUN_00888590 for the current slot = h's pad */

/* one section of h (the character's phase or his weapon's), with or without the gate; returns whether the byte was the game's again afterwards */
static void section(human *h, int own, int gate, int char_section)
{
    int set = 0;
    uint8_t old = 0;
    if (ag1_applies(gate, h->taken && !h->lost, h->slot, own)) {
        int v = ag1_value(char_section, char_section && h->bag, char_section && h->bag ? h->can : 0, &h->cache);
        if (v != AG1_UNKNOWN) {
            old = flag;
            flag = (uint8_t)v;
            set = 1;
        }
    }
    if (char_section) {
        if (h->slot == (unsigned)own)   /* FUN_006B3100: only the own human writes the byte */
            flag = (uint8_t)(h->bag && h->can);
        h->aimed = h->can && aim_pressed(h);   /* uPlayer_updatePad: +0x1151 = FUN_006AD740(0) && FUN_00888590(0) */
    } else
        h->weapon_saw = aim_pressed(h);
    if (set)
        flag = old;
}

static void frame(human *p0, human *p1, int own, int gate)
{
    section(p0, own, gate, 1);
    section(p1, own, gate, 1);
    section(p1, own, gate, 0);   /* his weapon on a worker: the cached value */
    section(p0, own, gate, 0);
}

static human mk(unsigned slot, int can, int lt)
{
    human h;
    memset(&h, 0, sizeof h);
    h.slot = slot, h.taken = 1, h.bag = 1, h.can = can, h.aim_button = lt, h.cache = AG1_UNKNOWN;
    return h;
}

/* 1. the live finding: own human unarmed, the second human holds LT: stock never aims, the gate aims */
static void t_live(void)
{
    human j = mk(0, 0, 0), c = mk(1, 1, 1);
    flag = 0;
    for (int f = 0; f < 3; f++) frame(&j, &c, 0, 0);
    CHECK(!c.aimed && !c.weapon_saw);
    CHECK(flag == 0);
    for (int f = 0; f < 3; f++) frame(&j, &c, 0, 1);
    CHECK(c.aimed && c.weapon_saw);
    CHECK(flag == 0);   /* outside his sections the byte is the own human's again */
    CHECK(!j.aimed);
    printf("ok   live case: own human unarmed, LT of the second human: stock no aim (byte 0), gate aims; the own human's byte is left as the game wrote it\n");
}

/* 2. the reverse: the own human may aim, the second human may not (scanner, unarmed): the gate must not let him aim (stock lets the weapon sections see "allowed") */
static void t_reverse(void)
{
    human j = mk(0, 1, 0), c = mk(1, 0, 1);
    flag = 0;
    frame(&j, &c, 0, 0);
    CHECK(!c.aimed);          /* his own +0x1151 term (FUN_006AD740 on him) already blocks */
    CHECK(c.weapon_saw);      /* but his weapon / upper-state sections saw the own human's "allowed" */
    frame(&j, &c, 0, 1);
    CHECK(!c.aimed && !c.weapon_saw);
    CHECK(flag == 1);
    printf("ok   reverse case: the own human may aim, the second may not: the gate gives his sections HIS 0, the byte is 1 again after them\n");
}

/* 3. both may aim, both hold LT: the same result with and without the gate */
static void t_both(void)
{
    human j = mk(0, 1, 1), c = mk(1, 1, 1);
    flag = 0;
    frame(&j, &c, 0, 0);
    CHECK(j.aimed && c.aimed);
    frame(&j, &c, 0, 1);
    CHECK(j.aimed && c.aimed && flag == 1);
    printf("ok   both may aim: unchanged by the gate\n");
}

/* 4. who is touched: the own human never, an untaken (AI) or lost capture never, a slot field outside 0/1 never, the gate off never */
static void t_applies(void)
{
    CHECK(!ag1_applies(1, 1, 0, 0));     /* the own human: the game writes his byte */
    CHECK(ag1_applies(1, 1, 1, 0));
    CHECK(ag1_applies(1, 1, 0, 1));      /* own slot 1 (the menu exchange, the guest's PC): slot 0 is the other human */
    CHECK(!ag1_applies(1, 0, 1, 0));     /* passive / AI partner */
    CHECK(!ag1_applies(0, 1, 1, 0));     /* `cap aimgate 0` */
    CHECK(!ag1_applies(1, 1, 2, 0));     /* not a player slot */
    CHECK(!ag1_applies(1, 1, 0xFFFFFFFFu, 0));
    printf("ok   applies: other human only, taken and not lost, slots 0/1, gate on\n");
}

/* 5. the cache: unknown before his first own phase (the byte left alone), no bag = may not aim (and FUN_006AD740 is not called), the weapon sections follow the last answer */
static void t_cache(void)
{
    int cache = AG1_UNKNOWN;
    CHECK(ag1_value(0, 0, 0, &cache) == AG1_UNKNOWN);
    CHECK(ag1_value(1, 1, 1, &cache) == 1 && cache == 1);
    CHECK(ag1_value(0, 0, 0, &cache) == 1);
    CHECK(ag1_value(1, 0, 1, &cache) == 0 && cache == 0);   /* no bag: 0 whatever `can` says */
    CHECK(ag1_value(1, 1, 7, &cache) == 1);                  /* any non-zero al = yes */
    human j = mk(0, 0, 0), c = mk(1, 1, 1);
    flag = 0;
    section(&c, 0, 1, 0);   /* weapon section before his first own phase */
    CHECK(!c.weapon_saw && flag == 0);
    (void)j;
    printf("ok   cache: unknown leaves the byte alone, no bag = 0 without the call, weapon sections follow the last answer\n");
}

/* 6. the own human changes during play (the menu exchange flips the own slot to 1): the gate follows the own slot of the moment */
static void t_own_flip(void)
{
    human j = mk(0, 0, 1), c = mk(1, 1, 1);
    flag = 0;
    frame(&j, &c, 1, 1);   /* own = 1: c writes the byte himself, j gets her own 0 */
    CHECK(c.aimed && !j.aimed);
    CHECK(flag == 1);
    printf("ok   own slot flipped: the gate covers the other human (slot 0), the game's byte is the new own human's\n");
}

/* 7. the facts of the build the gate relies on, in the real image */
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
static int callers_of(uint32_t fn)
{
    int k = 0;
    for (size_t i = 0; i + 5 <= img_n; i++)
        if (img[i] == 0xE8) {
            int32_t rel;
            memcpy(&rel, img + i + 1, 4);
            if ((uint32_t)(0x400000u + i + 5 + (uint32_t)rel) == fn) k++;
        }
    return k;
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
    CHECK(img_has(AG1_CANAIM_FN, "83 EC 0C 53 56 8B F1 8B 86"));            /* the prologue capture.c checks before calling it */
    CHECK(img_has(0x6AD8EAu, "C2 04 00") && img_has(0x6AD8F4u, "C2 04 00"));  /* thiscall, one stack argument */
    CHECK(img_has(0x883C70u, "8A 44 24 04 88 81 83 08 00 00"));             /* FUN_00883C70: mov al, [esp+4]; mov [ecx+0x883], al */
    CHECK(callers_of(0x883C70u) == 1);                                      /* written only by ... */
    CHECK(img_has(0x6B31BFu, "8B 0D 88 03 1E 01 50 E8"));                   /* FUN_006B3100: ecx = sGamePad, push the own human's answer, call */
    CHECK(img_has(0x88859Cu, "80 BE 83 08 00 00 00"));                      /* FUN_00888590: cmp byte [esi+0x883], 0 */
    CHECK(img_has(0x888590u, "56 8B F1 80 BE 82 08 00 00 00"));             /* ... after cmp byte [esi+0x882], 0 */
    printf("ok   image %s: FUN_006AD740 prologue + ret 4, the byte's only writer and its single caller in the own human's frame, the reader needs +0x882 and +0x883\n", path);
    free(b);
    img = NULL;
}

int main(void)
{
    t_live();
    t_reverse();
    t_both();
    t_applies();
    t_cache();
    t_own_flip();
    t_image();
    printf("test_aimgate1: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
