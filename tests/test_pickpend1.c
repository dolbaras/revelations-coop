/*
 * Offline test of native/pickpend1.h: a pickup whose world item leaves the list only after AddItem returned (wp9 live: the 9-2 herb 8/12 on the host) is sent once
 * when the item is gone; a level change sends nothing; an item still listed after the limit is dropped; duplicates, a full table, the tick wrap.
 *   sh tests/build.sh   ->  tests/build/test_pickpend1.exe
 */
#include <stdio.h>
#include <string.h>

#include "../native/pickpend1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* fake world: a set of listed units, a level flag */
static uint32_t listed_units[16];
static int nlisted, level = 1;
static int f_listed(void *u, uint32_t unit)
{
    (void)u;
    if (!level) return -1;
    for (int i = 0; i < nlisted; i++)
        if (listed_units[i] == unit) return 1;
    return 0;
}
static void unlist(uint32_t unit)
{
    for (int i = 0; i < nlisted; i++)
        if (listed_units[i] == unit) {
            listed_units[i] = listed_units[--nlisted];
            return;
        }
}
static pp_ent last_sent;
static uint32_t last_age;
static int nsent, nexp;
static void f_sent(void *u, const pp_ent *e, uint32_t age) { (void)u, last_sent = *e, last_age = age, nsent++; }
static void f_exp(void *u, const pp_ent *e, uint32_t age) { (void)u, (void)e, (void)age, nexp++; }

int main(void)
{
    pp_t p;
    pp_init(&p);
    const uint32_t k1[4] = {9, 2, 8, 12}, k2[4] = {9, 2, 8, 13};
    uint32_t herb = 0x24150410u, herb2 = 0x24160000u;

    /* 1. wp9: the herb added while still listed -> watched; still listed in the next passes -> nothing; gone -> sent once with its key and class */
    nlisted = 0, listed_units[nlisted++] = herb, listed_units[nlisted++] = herb2;
    CHECK(pp_add(&p, herb, 0x80040100u, k1, 1000) == 1 && pp_count(&p) == 1);
    CHECK(pp_pass(&p, 1033, 5000, f_listed, f_sent, f_exp, NULL) == 0 && nsent == 0);
    CHECK(pp_pass(&p, 1500, 5000, f_listed, f_sent, f_exp, NULL) == 0 && nsent == 0);
    unlist(herb);
    CHECK(pp_pass(&p, 1700, 5000, f_listed, f_sent, f_exp, NULL) == 1 && nsent == 1);
    CHECK(last_sent.unit == herb && last_sent.cls == 0x80040100u && !memcmp(last_sent.k, k1, sizeof k1) && last_age == 700);
    CHECK(pp_count(&p) == 0 && pp_pass(&p, 1800, 5000, f_listed, f_sent, f_exp, NULL) == 0 && nsent == 1);   /* once */

    /* 2. the same unit added twice (two AddItem calls for one pickup): watched once */
    CHECK(pp_add(&p, herb2, 0x80040100u, k2, 2000) == 1 && pp_add(&p, herb2, 0x80040100u, k2, 2010) == 0 && p.dup == 1 && pp_count(&p) == 1);

    /* 3. still listed after the limit: dropped (expired), never sent */
    CHECK(pp_pass(&p, 6999, 5000, f_listed, f_sent, f_exp, NULL) == 0 && nexp == 0 && pp_count(&p) == 1);
    CHECK(pp_pass(&p, 7011, 5000, f_listed, f_sent, f_exp, NULL) == 0 && nexp == 1 && pp_count(&p) == 0 && nsent == 1);

    /* 4. a level change (no sArrange) while watched: forgotten silently */
    CHECK(pp_add(&p, herb2, 0x80040100u, k2, 8000) == 1);
    level = 0;
    CHECK(pp_pass(&p, 8100, 5000, f_listed, f_sent, f_exp, NULL) == 0 && pp_count(&p) == 0 && p.dropped_level == 1 && nsent == 1 && nexp == 1);
    level = 1;

    /* 5. pp_forget (a new level instance / bind) */
    CHECK(pp_add(&p, herb2, 1, k2, 9000) == 1);
    pp_forget(&p);
    CHECK(pp_count(&p) == 0 && pp_pass(&p, 9100, 5000, f_listed, f_sent, f_exp, NULL) == 0);

    /* 6. a full table: the ninth is refused (counted), the others stay; a zero unit is refused */
    for (uint32_t i = 0; i < PP_N; i++) CHECK(pp_add(&p, 0x30000000u + i * 0x100u, 1, k1, 10000) == 1);
    CHECK(pp_add(&p, 0x31000000u, 1, k1, 10000) == 0 && p.full == 1 && pp_count(&p) == PP_N);
    CHECK(pp_add(&p, 0, 1, k1, 10000) == 0);
    /* none of them is listed: all sent in one pass */
    nsent = 0;
    CHECK(pp_pass(&p, 10050, 5000, f_listed, f_sent, f_exp, NULL) == PP_N && nsent == PP_N && pp_count(&p) == 0);

    /* 7. the tick wraps between the add and the pass */
    nlisted = 0, listed_units[nlisted++] = herb;
    CHECK(pp_add(&p, herb, 1, k1, 0xFFFFFF00u) == 1);
    CHECK(pp_pass(&p, 0x00000100u, 5000, f_listed, f_sent, f_exp, NULL) == 0 && pp_count(&p) == 1);   /* 512 ms old: still watched */
    unlist(herb);
    nsent = 0;
    CHECK(pp_pass(&p, 0x00000200u, 5000, f_listed, f_sent, f_exp, NULL) == 1 && last_age == 0x300u);

    /* 8. the wiring in the game glue (source facts of native/world1_game.c): the add that left the item listed is watched, the frame pass runs in PLAYING, the
     * watch is forgotten on a new bind and a new level instance, a gone item is sent as EV_PICKUP with its key */
    {
        static char src[1 << 20];
        FILE *f = fopen("../native/world1_game.c", "rb");
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(n > 10000);
        const char *add = strstr(src, "static unsigned TC hook_add(");
        const char *nr = add ? strstr(add, "S.pickups_not_removed++;") : NULL;
        CHECK(nr && strstr(nr, "pp_add(&g_pp, it, cls, e.k, GetTickCount())") && strstr(nr, "pp_add(&g_pp") - nr < 200);
        const char *wf = strstr(src, "static void world_frame(void)");
        const char *vp = wf ? strstr(wf, "vital_pass(now);") : NULL;
        CHECK(vp && strstr(vp, "pickpend_pass();") && strstr(vp, "pickpend_pass();") - vp < 60);
        const char *ep = wf ? strstr(wf, "ndoors = 0;") : NULL;
        CHECK(ep && strstr(ep, "pickpend_forget();") - ep < 40);
        const char *bind = strstr(src, "w1n_init(&N, bound_cs");
        CHECK(bind && strstr(bind, "pickpend_forget();") && strstr(bind, "pickpend_forget();") - bind < 200);
        const char *sc = strstr(src, "static void pp_sent_cb(");
        CHECK(strstr(src, "#define PP_MAX_MS 60000u") != NULL);   /* the glue's limit: wide (the removal delay is not known) */
        CHECK(sc && strstr(sc, "ev.type = EV_PICKUP") && strstr(sc, "memcpy(ev.k4, pe->k, sizeof ev.k4);") && strstr(sc, "ev_push(&ev);"));
    }

    printf("test_pickpend1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
