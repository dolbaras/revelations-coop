/*
 * test_hitx1.c - rev1 live round 14 (native/hitx1.h + local1.h's dead guard): an enemy the CLIENT kills dies on the host by the game's own death, its replica
 * dies on the client the same way, local1 never takes a replica the host's DEATH came for, and a dead enemy no stock death reached is finished.
 *
 * The ENGINE MODEL is the static chain (hitx1.h header): a hit reaches an enemy's state machine only through its HIT QUEUE (FUN_0049D3E0, written by
 * ApplyHit and by the Raid applier 0x49AA50); the zombie's update (uZombieBase_vf43 0x5BAC70) walks the queue: hp > 0 -> a reaction, hp <= 0 -> a death
 * request (+0x59F), which the NEXT update starts only when the unit is local (FUN_006C9FE0: kind != 0 && kind != 3); the dying state ends in DeadState
 * (E1C bit 31 after its timer), and only then FUN_00457370 marks the record used up (+0xD8), which the story's dead checks read (FUN_00459040).
 *
 *  A  wire: HITX = key, damage, the PCHIT packing of the real HitInfo (92 bytes = W1_EXT_MAX); the point and the flags survive.
 *  B  HOST, the m13f kill (the Ooze at hp 30, the client's 117-damage blow): the model before the fix (Damage + OnHit, no queue) leaves it hp 0 without the dead
 *     bit and the record not used up (the 1-1 story waits); the fix (the applier) gives the death state, the dead bit and the used-up record.
 *  C  CLIENT replica on the host's DEATH: before the fix (Damage + OnHit on a kind-3 replica) it never dies; the later rounds of the burst put it back to hp 1,
 *     local1 (before the guard) takes it to the guest's AI and it bites; the fix: local + hp 0 + the queued HitInfo -> the stock death, never taken.
 *  D  the finish (rule 3): only a dead, hp <= 0, not-dead-bit, not-used-up enemy, once, after the limit; a record without a unit too.
 *  E  the per-key last HitInfo table; the path decisions; the glue's wiring in the sources.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/hitx1.h"
#include "../native/local1.h"
#include "../native/world1_hooks.h"
#include "../native/world1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* ---------------------------------------------------------------- the engine model of one zombie */
typedef struct {
    float hp;
    int kind;            /* 1 human, 2 own AI, 3 replica */
    int queue;           /* HitInfos in [unit+0xEA0] */
    int req;             /* +0x59F: 0 none, 1 reaction, 2 death */
    int state;           /* 0 alive / reacting, 1 dying, 2 dead (DeadState reached) */
    int dying_frames;
    unsigned e1c;
    int spent;           /* the record's +0xD8 */
    int reactions;
    int bites;           /* the AI attacked (own AI with hp > 0 next to a human) */
} zom;

static int local_unit(const zom *z) { return z->kind != 0 && z->kind != 3; }   /* FUN_006C9FE0 */

/* one update (uZombieBase_vf43): the request first, then the queue, then the AI / the dying timer */
static void think(zom *z)
{
    if (z->req && local_unit(z)) {
        if (z->req == 2 && z->state == 0) z->state = 1, z->dying_frames = 0;
        else if (z->req == 1) z->reactions++;
        z->req = 0;
    } else if (!local_unit(z)) {
        z->req = 0;   /* a replica: the stored request is dropped (the replica's own machine never starts it) */
    }
    while (z->queue > 0) {
        z->queue--;
        z->req = z->hp > 0.0f ? 1 : 2;   /* FUN_005B8530 */
    }
    if (z->state == 1 && ++z->dying_frames >= 90) {   /* DeadState_vf10: the dead bit after its timer, then FUN_00457370 */
        z->state = 2, z->e1c |= HX_E1C_DEAD;
        z->spent = 1;
    }
    if (z->state == 0 && local_unit(z) && z->kind == 2 && z->hp > 0.0f) z->bites++;
}
static void frames(zom *z, int n) { for (int i = 0; i < n; i++) think(z); }

/* the host's ways to apply the client's blow */
static void host_before_fix(zom *z, float dmg) { z->hp -= dmg; if (z->hp < 0) z->hp = 0; }                        /* op_apply_hit: Damage + OnHit */
static void applier(zom *z, float dmg) { z->queue++; z->hp -= dmg; if (z->hp < 0) z->hp = 0; }                     /* 0x49AA50: queue + vf46 + Damage + OnHit */

/* ---------------------------------------------------------------- A */
static void t_wire(void)
{
    uint8_t hit[WH_HIT_SIZE], msg[WH_DMGMSG_SIZE], pay[HX_PAY], b[HX_LEN];
    float pt[3] = {-210.5f, -597.0f, 13710.25f};
    wh_hit_build(hit, WH_HIT_VTBL, 0x24000000u, 0x24000000u, 0x01000400u, pt);
    hit[0x48] = 3, hit[0x5C] = 7;   /* part, weapon kind */
    float pw = 117.0f;
    memcpy(hit + 0x64, &pw, 4);
    wh_dmgmsg_from_hit(hit, 0, 0, msg);
    wh_pchit_pack(msg, pay);
    CHECK(HX_PAY == WH_PCHIT_LEN && HX_LEN == W1_EXT_MAX, "A the HITX body is exactly the ext room (92) with the 88-byte PCHIT packing");
    CHECK(hx_enc(b, 0x0200, 117.4f, pay) == HX_LEN, "A encoded length");
    uint16_t key;
    float dmg;
    const uint8_t *p;
    CHECK(hx_dec(b, HX_LEN, &key, &dmg, &p) && key == 0x0200 && dmg == 117.0f && !memcmp(p, pay, HX_PAY), "A decode: key, the rounded damage, the payload");
    CHECK(!hx_dec(b, HX_LEN - 1, &key, &dmg, &p), "A a short body is refused");
    float q[3];
    uint32_t fl;
    memcpy(q, p, 12), memcpy(&fl, p + 36 + (0x4C - 0x44), 4);
    CHECK(q[0] == pt[0] && q[2] == pt[2] && fl == 0x01000400u, "A the point and the flags (HitInfo +0x54 = msg +0x4C) are where hitx_rx reads them");
    uint8_t m2[WH_DMGMSG_SIZE];
    wh_pchit_unpack(p, 1, 1, m2);
    uint16_t as, os;
    memcpy(&as, m2 + 0x40, 2), memcpy(&os, m2 + 0x42, 2);
    CHECK(as == 1 && os == 1 && m2[0x44] == 3 && m2[0x51] == 7 && !memcmp(m2 + 0x54, &pw, 4), "A the applier's message: the puppet's serial for attacker / owner, part, weapon kind, damage numbers");
    hx_enc(b, 1, 0.2f, pay), hx_dec(b, HX_LEN, &key, &dmg, &p);
    CHECK(dmg == 1.0f, "A a tiny damage still says 1 (the old path never sends 0)");
    hx_enc(b, 1, 1.0e7f, pay), hx_dec(b, HX_LEN, &key, &dmg, &p);
    CHECK(dmg == 65535.0f, "A a kill intent above 65535 is clamped");
}

/* ---------------------------------------------------------------- B */
static int host_run(int fixed)
{
    zom z = {30.0f, 2, 0, 0, 0, 0, 0x40000088u, 0, 0, 0};
    int path = hx_host_path(fixed, 1, 1, 1, 1, 1);
    if (path == HX_P_APPLIER) applier(&z, 117.0f);
    else host_before_fix(&z, 117.0f);
    frames(&z, 54 * 30);   /* the 54 s of m13f */
    return z.spent && (z.e1c & HX_E1C_DEAD) && z.state == 2;
}
static void t_host(void)
{
    int before = host_run(0), after = host_run(1);
    CHECK(!before, "B before the fix: the host's Ooze stays hp 0 without the dead bit, its record not used up (the m13f defect: the 1-1 story waits)");
    printf("B before the fix: stock death %d (0 = the defect reproduced)\n", before);
    CHECK(after, "B the fix: the client's blow through the applier -> death state -> DeadState -> dead bit -> the record used up (the story's dead check passes)");
    /* the same holds when the blow is not lethal: a reaction, no death */
    zom z = {2100.0f, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    applier(&z, 117.0f), frames(&z, 30);
    CHECK(z.reactions == 1 && z.state == 0 && z.hp == 1983.0f, "B a non-lethal client blow: the host's Ooze reacts (the stock reaction), hp 2100 -> 1983");
    zom z2 = {2100.0f, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    host_before_fix(&z2, 117.0f), frames(&z2, 30);
    CHECK(z2.reactions == 0, "B before the fix the host's Ooze never reacted to a client blow");
}

/* ---------------------------------------------------------------- C */
typedef struct { int died, taken, bites; } crun;
static crun client_run(int fixed)
{
    lr_t T;
    lr_reset(&T);
    T.no_dead_guard = !fixed;
    zom r = {30.0f, 3, 0, 0, 0, 0, 0x40000008u, 0, 0, 0};
    uint16_t key = 0x0200;
    uint32_t unit = 0x24bcb750u, now = 1000;
    crun out = {0, 0, 0};
    /* records of the host while it is alive */
    for (int i = 0; i < 10; i++, now += 100) lr_on_record(&T, key, 7, 0x40000008u, now), lr_step(&T, key, unit, 3, 1, r.hp, 160.0f, now);
    /* the host's DEATH arrives (op_kill) */
    lr_on_death(&T, key);
    if (hx_replica_death(fixed, r.kind, 1, 1) == HX_D_STOCK) {
        r.kind = 2, r.hp = 0.0f, r.queue++;   /* lr_set_kind(.., 2), hp 0, the applier */
    } else {
        r.hp = 0.0f;   /* Damage(hp) + OnHit */
    }
    /* the later rounds of the burst: the client's own blows on the replica (the replica branch of OnHit keeps it at hp 1) */
    for (int i = 0; i < 3; i++)
        if (r.kind == 3) r.queue++, r.hp = 1.0f;
    /* no more records from the host (its model has it dead): local1 per pass, 10 s */
    for (int i = 0; i < 100; i++, now += 100) {
        think(&r);
        int alive = r.hp > 0.0f && r.state == 0 && !(r.e1c & HX_E1C_DEAD);
        int a = lr_step(&T, key, unit, (unsigned)r.kind, alive, r.hp, 160.0f, now);
        if (a == LR_MAKE_LOCAL) r.kind = 2, out.taken = 1;
    }
    frames(&r, 200);
    out.died = r.spent && (r.e1c & HX_E1C_DEAD);
    out.bites = r.bites;
    return out;
}
static void t_client(void)
{
    crun b = client_run(0), a = client_run(1);
    CHECK(!b.died && b.taken && b.bites > 0, "C before the fix: the replica never dies, local1 takes the hp-1 replica and its AI bites (m13f: `runs on THIS PC's AI now (kind 3 -> 2)` -> Parker dead)");
    printf("C before the fix: died %d taken %d bites %d (the defect reproduced)\n", b.died, b.taken, b.bites);
    CHECK(a.died && !a.taken && a.bites == 0, "C the fix: the replica dies by its own update (dead bit, used up), never taken, never bites");
    /* the guard alone (the old death, the guard on) keeps it from being taken */
    lr_t T;
    lr_reset(&T);
    lr_on_record(&T, 1, 7, 0, 100);
    lr_step(&T, 1, 0x100, 3, 1, 30.0f, 100.0f, 100);
    lr_on_death(&T, 1);
    int took = 0;
    for (uint32_t t = 200; t < 20000; t += 100) took |= lr_step(&T, 1, 0x100, 3, 1, 1.0f, 100.0f, t) == LR_MAKE_LOCAL;
    CHECK(!took && T.dead_refused > 0, "C the dead guard: a replica the host's DEATH came for is never taken (counted)");
    lr_reset(&T);
    lr_on_record(&T, 2, LR_AI_NOUNIT, 0, 100);
    took = 0;
    for (uint32_t t = 200; t < 8000; t += 100) took |= lr_step(&T, 2, 0x200, 3, 1, 900.0f, 100.0f, t) == LR_MAKE_LOCAL;
    CHECK(took, "C the guard leaves the local1 rule alone for a live enemy the host does not simulate (rule 3 still takes it)");
}

/* ---------------------------------------------------------------- D */
static void t_finish(void)
{
    hx_wd_t W;
    hx_wd_reset(&W);
    uint32_t t = 1000;
    int a = 0, n = 0;
    for (; t < 1000 + HX_FINISH_MS + 1000; t += 250) {
        a = hx_wd_step(&W, 5, 1, 0x100, 0.0f, 0x40000088u, 0, t, HX_FINISH_MS);
        if (a) n++;
        if (a) break;
    }
    CHECK(a == HX_WD_FINISH_UNIT && n == 1 && t >= 1000 + HX_FINISH_MS, "D a dead hp-0 unit without the dead bit is finished once, after the limit");
    CHECK(hx_wd_step(&W, 5, 1, 0x100, 0.0f, 0x40000088u, 0, t + 5000, HX_FINISH_MS) == HX_WD_NONE, "D only once");
    hx_wd_reset(&W);
    int any = 0;
    for (t = 1000; t < 40000; t += 250) any |= hx_wd_step(&W, 6, 1, 0x100, 0.0f, 0xC0000088u, 0, t, HX_FINISH_MS);
    CHECK(!any, "D the stock death came (dead bit): never finished");
    for (t = 1000; t < 40000; t += 250) any |= hx_wd_step(&W, 7, 1, 0x100, 0.0f, 0x40000088u, 1, t, HX_FINISH_MS);
    CHECK(!any, "D the record is used up: never finished");
    for (t = 1000; t < 40000; t += 250) any |= hx_wd_step(&W, 8, 1, 0x100, 300.0f, 0x40000088u, 0, t, HX_FINISH_MS);
    CHECK(!any, "D a unit with hp (a local enemy the host's DEATH did not touch) is never finished");
    for (t = 1000; t < 40000; t += 250) any |= hx_wd_step(&W, 9, 0, 0x100, 0.0f, 0x40000088u, 0, t, HX_FINISH_MS);
    CHECK(!any, "D the model does not say dead: nothing");
    a = 0;
    for (t = 1000; t < 40000 && !a; t += 250) a = hx_wd_step(&W, 10, 1, 0, 0.0f, 0, 0, t, HX_FINISH_MS);
    CHECK(a == HX_WD_FINISH_RECORD, "D dead in the model, no unit here, the record not used up: the record is marked");
    for (t = 1000; t < 40000; t += 250) any |= hx_wd_step(&W, 11, 1, 0x100, 0.0f, 0x40000088u, 0, t, 0);
    CHECK(!any, "D limit 0 = off");
    /* the engine model: the host's Ooze of the defect state is finished by rule 3 even before the fix */
    zom z = {30.0f, 2, 0, 0, 0, 0, 0x40000088u, 0, 0, 0};
    host_before_fix(&z, 117.0f);
    hx_wd_reset(&W);
    for (t = 0; t < 20000; t += 250) {
        frames(&z, 8);
        if (hx_wd_step(&W, 0x200, 1, 0x100, z.hp, z.e1c, z.spent, t + 1, HX_FINISH_MS) == HX_WD_FINISH_UNIT) z.spent = 1;
    }
    CHECK(z.spent, "D the safety net: an hp-0 enemy no death state reached gets its record used up (the story goes on)");
}

/* ---------------------------------------------------------------- E */
static void t_tables(void)
{
    hx_last_t L;
    hx_last_reset(&L);
    uint8_t p1[HX_PAY], p2[HX_PAY];
    memset(p1, 1, sizeof p1), memset(p2, 2, sizeof p2);
    CHECK(!hx_last_get(&L, 5), "E nothing yet");
    hx_last_put(&L, 5, p1), hx_last_put(&L, 5, p2);
    CHECK(hx_last_get(&L, 5) && hx_last_get(&L, 5)[0] == 2, "E the last HitInfo per key (overwritten)");
    for (int k = 100; k < 100 + HX_MAXL + 4; k++) hx_last_put(&L, (uint16_t)k, p1);
    CHECK(!hx_last_get(&L, 5) && !hx_last_get(&L, 100) && hx_last_get(&L, 100 + HX_MAXL + 3), "E full: the oldest keys go first");
    CHECK(hx_use_last(1, 1, 1) && !hx_use_last(0, 1, 1) && !hx_use_last(1, 1, 0) && !hx_use_last(W1_NO_SLOT, 1, 1), "E the own last HitInfo only when this PC's human killed");
    CHECK(hx_host_path(1, 0, 1, 1, 1, 1) == HX_P_LATE && hx_host_path(1, 1, 0, 1, 1, 1) == HX_P_NOUNIT, "E late / no unit");
    CHECK(hx_host_path(1, 1, 1, 1, 1, 1) == HX_P_APPLIER, "E the applier when everything is verified");
    CHECK(hx_host_path(1, 1, 1, 1, 1, 0) == HX_P_DIRECT && hx_host_path(0, 1, 1, 1, 1, 1) == HX_P_DIRECT && hx_host_path(1, 1, 1, 0, 1, 1) == HX_P_DIRECT,
          "E the old Damage + OnHit when the puppet's serial resolves elsewhere / the switch is off / the applier is not verified");
    CHECK(hx_replica_death(1, 3, 1, 1) == HX_D_STOCK && hx_replica_death(1, 2, 1, 1) == HX_D_OLD && hx_replica_death(0, 3, 1, 1) == HX_D_OLD && hx_replica_death(1, 3, 1, 0) == HX_D_OLD,
          "E the replica's stock death only for a kind-3 replica with a hit queue, switch on");
}

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (s) s[n] = 0;
    fclose(f);
    return s;
}
static void t_sources(void)
{
    char *w = slurp("../native/world1_game.c"), *g = slurp("../native/hitx1_game.inc"), *s = slurp("../native/story1_game.inc"), *l = slurp("../native/local1_game.inc"),
         *p = slurp("../native/coop_proto.h");
    CHECK(w && strstr(w, "#include \"hitx1_game.inc\"") && strstr(w, "hitx_pass(now);"), "F the glue is included and runs in the frame");
    CHECK(w && strstr(w, "wh_pchit_pack(msg, ev.blob), ev.have_blob = 1"), "F hook_apply packs the own human's real HitInfo into the event");
    CHECK(w && strstr(w, "if (!(e.have_blob && hitx_send(e.key, e.dmg, e.blob))) w1_client_hit("), "F the drain sends HITX, the bare HIT only when HITX is not sent");
    CHECK(w && strstr(w, "if (unit && hitx_replica_die(key, unit, killer_slot, hit_flags, pt))"), "F op_kill gives the replica its stock death first");
    CHECK(w && strstr(w, "int via = hitx_lab_kill(e.unit, att, by, pt);"), "F the LAB kill goes through the hit queue too");
    CHECK(s && strstr(s, "sub == CP_W1_HITX") && strstr(s, "hitx_rx(b, len)"), "F op_ext routes CP_W1_HITX");
    CHECK(l && strstr(l, "hx_reset();"), "F the level reset clears the hitx tables");
    CHECK(p && strstr(p, "#define CP_W1_HITX 23"), "F CP_W1_HITX = 23 (inside CP_W1_EXT_FIRST..LAST)");
    CHECK(g && strstr(g, "hx_apply(unit, pay, (uint16_t)slot, (uint16_t)slot, slot);") && strstr(g, "su == pup"), "F the host plays the blow with the puppet's serial, checked to resolve to the puppet");
    CHECK(g && strstr(g, "hx_apply(unit, pay, 0xFFFFu, 0xFFFFu, 1 - g_own);") && strstr(g, "lr_set_kind(e.entry, unit, 2)"), "F the replica dies local and attacker-less (no second kill record)");
    CHECK(g && strstr(g, "LR.no_dead_guard = !f_deadguard;"), "F the dead guard follows its switch");
    free(w), free(g), free(s), free(l), free(p);
}

int main(void)
{
    t_wire();
    t_host();
    t_client();
    t_finish();
    t_tables();
    t_sources();
    printf("test_hitx1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
