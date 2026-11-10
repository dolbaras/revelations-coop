/*
 * Offline test of the lab `packcopy` and its undo (native/packcopy1.h) on fake memory: two humans with bags, weapon objects, a second copy into the same bag,
 * the undo (owners and words back, newest first), a unit that a load replaced (its record is dropped, its old memory never written), refusals; the restart
 * callback pc1_before_reload on the wp6 layout (rb_bag_shared of rolebarrier1.c 4 -> 0) and its wiring in probe.c / rolebarrier1_game.c (source facts).
 *   sh tests/build.sh   ->  tests/build/test_packcopy1.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/packcopy1.h"
#include "../native/rolebarrier1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* fake memory: 64 KB at base 0x20000 */
#define BASE 0x20000u
#define SIZE 0x10000u
static uint8_t M[SIZE];
static int writes_outside, writes;
static uint32_t W[8];   /* weapon objects */
static int nw;

static int f_rd(void *u, uint32_t a, uint32_t *v)
{
    (void)u;
    if (a < BASE || a + 4 > BASE + SIZE) return 0;
    memcpy(v, M + (a - BASE), 4);
    return 1;
}
static int f_wr(void *u, uint32_t a, uint32_t v)
{
    (void)u;
    if (a < BASE || a + 4 > BASE + SIZE) { writes_outside++; return 0; }
    memcpy(M + (a - BASE), &v, 4);
    writes++;
    return 1;
}
static int f_isweapon(void *u, uint32_t obj)
{
    (void)u;
    for (int i = 0; i < nw; i++) if (W[i] == obj) return 1;
    return 0;
}
static int f_bagof(void *u, uint32_t unit, uint32_t *bag) { return f_rd(u, unit + 0xFE8, bag) && *bag != 0; }
static const pc1_mem FM = {NULL, f_rd, f_wr, f_isweapon, f_bagof};

static uint32_t r32(uint32_t a) { uint32_t v = 0; f_rd(NULL, a, &v); return v; }
/* the bag check of the restart glue (rolebarrier1.c rb_bag_shared) on the same fake memory */
static uint32_t t_rd(void *u, uint32_t a) { uint32_t v = 0; f_rd(u, a, &v); return v; }
static int t_isweapon(void *u, uint32_t v) { return f_isweapon(u, v); }
static void w32(uint32_t a, uint32_t v) { f_wr(NULL, a, v); }

int main(void)
{
    /* units at 0x21000 (Jill, slot 0) and 0x23000 (Chris, slot 1), bags at 0x26000 / 0x27000, weapons at 0x28000.. */
    uint32_t uj = BASE + 0x1000, uc = BASE + 0x3000, bj = BASE + 0x6000, bc = BASE + 0x7000;
    memset(M, 0, sizeof M);
    w32(uj + 0xFE8, bj), w32(uc + 0xFE8, bc);
    nw = 4;
    for (int i = 0; i < nw; i++) W[i] = BASE + 0x8000 + 0x100u * (uint32_t)i, w32(W[i] + PC1_OWNER_OFF, uc);   /* Chris's (AI partner's) level set */
    w32(bc + 4, W[0]), w32(bc + 8, W[1]), w32(bc + 0x2C, W[2]), w32(bc + 0x40, 30), w32(bc + 0x70, 0x80051070);
    w32(bj + 4, 0x11111111), w32(bj + 0x2C, 0), w32(bj + 0x40, 7), w32(bj + 0xD4, 0);   /* Jill: an empty lab bag (unarmed id, ammo 7) */
    uint8_t before_j[0x150];
    memcpy(before_j, M + (bj - BASE), sizeof before_j);
    uint32_t owners_before[4];
    for (int i = 0; i < nw; i++) owners_before[i] = r32(W[i] + PC1_OWNER_OFF);

    pc1_t s;
    pc1_init(&s);
    int words = 0, owners = 0, stale = 0;

    /* 1. copy Chris -> Jill: words copied, the three weapons owned by Jill, one undo record */
    CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1);
    CHECK(r32(bj + 4) == W[0] && r32(bj + 8) == W[1] && r32(bj + 0x2C) == W[2] && r32(bj + 0x40) == 30 && r32(bj + 0x70) == 0x80051070);
    CHECK(owners == 3 && r32(W[0] + PC1_OWNER_OFF) == uj && r32(W[2] + PC1_OWNER_OFF) == uj && r32(W[3] + PC1_OWNER_OFF) == uc);
    CHECK(words >= 5 && pc1_pending(&s) == 1);
    CHECK(r32(bj + 0xD4) == 0 && r32(bj + 0x144) == 0);   /* nothing outside +4..+0x140 */

    /* 2. a second copy into the same bag (after a change in the source): the save keeps the ORIGINAL words */
    w32(bc + 0x40, 99);
    CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1 && r32(bj + 0x40) == 99 && pc1_pending(&s) == 1);

    /* 3. undo: owners back to Chris, Jill's words exactly as before the first copy */
    CHECK(pc1_undo(&s, &FM, &words, &owners, &stale) == 1 && stale == 0);
    CHECK(memcmp(before_j, M + (bj - BASE), sizeof before_j) == 0);
    int ok = 1;
    for (int i = 0; i < nw; i++) ok &= r32(W[i] + PC1_OWNER_OFF) == owners_before[i];
    CHECK(ok && owners == 3);
    CHECK(pc1_pending(&s) == 0);
    /* a second undo does nothing */
    writes = 0;
    CHECK(pc1_undo(&s, &FM, &words, &owners, &stale) == 0 && writes == 0);

    /* 4. copies on both sides (Chris -> Jill, Jill -> Chris): undo newest first returns both bags and all owners */
    uint8_t before_c[0x150];
    memcpy(before_c, M + (bc - BASE), sizeof before_c);
    memcpy(before_j, M + (bj - BASE), sizeof before_j);
    w32(bj + 0x10, W[3]), w32(W[3] + PC1_OWNER_OFF, uj);   /* Jill has a gun of her own now */
    memcpy(before_j, M + (bj - BASE), sizeof before_j);
    CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1);
    CHECK(pc1_copy(&s, &FM, bj, bc, uc, &words, &owners) == 1 && pc1_pending(&s) == 2);
    CHECK(pc1_undo(&s, &FM, &words, &owners, &stale) == 2);
    CHECK(memcmp(before_j, M + (bj - BASE), sizeof before_j) == 0 && memcmp(before_c, M + (bc - BASE), sizeof before_c) == 0);
    CHECK(r32(W[0] + PC1_OWNER_OFF) == uc && r32(W[3] + PC1_OWNER_OFF) == uj);

    /* 5. a load replaced Jill's unit (new bag): the record is dropped, the old bag memory is not written */
    CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1);
    uint32_t newbag = BASE + 0x9000;
    w32(uj + 0xFE8, newbag);
    writes = 0;
    CHECK(pc1_undo(&s, &FM, &words, &owners, &stale) == 0 && stale == 1 && writes == 0 && pc1_pending(&s) == 0);
    w32(uj + 0xFE8, bj);
    /* ... and a unit that is gone (bag pointer 0) */
    CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1);
    w32(uj + 0xFE8, 0);
    CHECK(pc1_undo(&s, &FM, &words, &owners, &stale) == 0 && stale == 1);
    w32(uj + 0xFE8, bj);

    /* 6. refusals: same bag, a null bag, an unreadable source; no free record -> nothing written */
    CHECK(pc1_copy(&s, &FM, bj, bj, uj, &words, &owners) == 0);
    CHECK(pc1_copy(&s, &FM, 0, bj, uj, &words, &owners) == 0);
    writes = 0;
    CHECK(pc1_copy(&s, &FM, 0x100u, bj, uj, &words, &owners) == 0 && writes == 0);
    {
        uint32_t bags[PC1_MAX + 1];
        for (int i = 0; i <= PC1_MAX; i++) bags[i] = BASE + 0xA000 + 0x400u * (uint32_t)i;
        for (int i = 0; i < PC1_MAX; i++) CHECK(pc1_copy(&s, &FM, bc, bags[i], uj, &words, &owners) == 1);
        writes = 0;
        CHECK(pc1_copy(&s, &FM, bc, bags[PC1_MAX], uj, &words, &owners) == 0 && writes == 0);
        pc1_forget(&s);
        CHECK(pc1_pending(&s) == 0);
    }
    CHECK(writes_outside == 0);

    /* 7. the restart callback (probe.c packcopy_before_reload via rolebarrier_on_restart): the wp6 layout - Chris's level set copied into Jill's lab bag, so the
     * weapons sit in BOTH bags (rolebarrier1.c rb_bag_shared, the check that runs right after the callbacks, counts them); the callback undoes it, the check then
     * finds 0; a second reload with nothing pending writes nothing and gives no line */
    {
        memset(M, 0, sizeof M);
        w32(uj + 0xFE8, bj), w32(uc + 0xFE8, bc);
        for (int i = 0; i < nw; i++) w32(W[i] + PC1_OWNER_OFF, uc);
        w32(bc + 4, W[0]), w32(bc + 8, W[1]), w32(bc + 0x2C, W[2]), w32(bc + 0x30, W[3]), w32(bc + 0x40, 30);
        w32(bj + 4, 0x11111111), w32(bj + 0x40, 7);
        memcpy(before_j, M + (bj - BASE), sizeof before_j);
        pc1_init(&s);
        uint32_t sh[8];
        CHECK(rb_bag_shared(t_rd, t_isweapon, NULL, bj, bc, sh, 8) == 0);   /* clean before the copy */
        CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1 && owners == 4);
        CHECK(rb_bag_shared(t_rd, t_isweapon, NULL, bj, bc, sh, 8) == 4);   /* wp6: four weapon objects in two bags = the double free on the reload */
        char line[220] = "untouched";
        CHECK(pc1_before_reload(&s, &FM, line, sizeof line) == 1);
        CHECK(strstr(line, "packcopy undo by itself: 1 bag(s) put back") != NULL && strstr(line, "4 weapon owners") != NULL && strstr(line, "0 record(s) dropped") != NULL);
        CHECK(rb_bag_shared(t_rd, t_isweapon, NULL, bj, bc, sh, 8) == 0);   /* what before_restart checks next: nothing shared */
        CHECK(memcmp(before_j, M + (bj - BASE), sizeof before_j) == 0);
        int own_ok = 1;
        for (int i = 0; i < nw; i++) own_ok &= r32(W[i] + PC1_OWNER_OFF) == uc;
        CHECK(own_ok && pc1_pending(&s) == 0);
        /* nothing pending: -1, no write, the line untouched (the callback logs nothing) */
        strcpy(line, "untouched");
        writes = 0;
        CHECK(pc1_before_reload(&s, &FM, line, sizeof line) == -1 && writes == 0 && !strcmp(line, "untouched"));
        /* a copy whose receiver a load already replaced: dropped, never written, reported */
        CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1);
        w32(uj + 0xFE8, BASE + 0x9000);
        writes = 0;
        CHECK(pc1_before_reload(&s, &FM, line, sizeof line) == 0 && writes == 0 && strstr(line, "1 record(s) dropped") != NULL && pc1_pending(&s) == 0);
        w32(uj + 0xFE8, bj);
        /* NULL out is allowed */
        CHECK(pc1_copy(&s, &FM, bc, bj, uj, &words, &owners) == 1 && pc1_before_reload(&s, &FM, NULL, 0) == 1);
    }
    CHECK(writes_outside == 0);

    /* 8. the wiring in the game glue (source facts): probe_install registers the callback after rolebarrier_init, the callback calls pc1_before_reload, and
     * rolebarrier1_game.c runs the registered callbacks before its bag check, from cb_restart only when the call is not held */
    {
        static char src[1 << 20];
        const char *pi = NULL, *pr = NULL, *pcb = NULL;
        FILE *f = fopen("../native/probe.c", "rb");
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(n > 1000);
        const char *inst = strstr(src, "void probe_install(void)");
        if (inst) pi = strstr(inst, "rolebarrier_init();"), pr = strstr(inst, "rolebarrier_on_restart(packcopy_before_reload);");
        CHECK(inst && pi && pr && pi < pr);
        pcb = strstr(src, "static void packcopy_before_reload(void)");
        CHECK(pcb && strstr(pcb, "pc1_before_reload(&g_pc1, &PC1M") != NULL && pcb < inst);
        static char g[1 << 20];
        f = fopen("../native/rolebarrier1_game.c", "rb");
        n = f ? fread(g, 1, sizeof g - 1, f) : 0;
        if (f) fclose(f);
        g[n] = 0;
        const char *br = strstr(g, "static void before_restart(void)");
        const char *loop = br ? strstr(br, "g_restart_cbs[i]();") : NULL, *chk = br ? strstr(br, "rb_bag_shared(") : NULL;
        CHECK(br && loop && chk && loop < chk);
        const char *cr = strstr(g, "cb_restart(rbk_frame *f)");
        const char *hold = cr ? strstr(cr, "if (!hold) {") : NULL, *call = hold ? strstr(hold, "before_restart();") : NULL;
        CHECK(cr && hold && call && call - hold < 200);
    }

    printf("test_packcopy1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
