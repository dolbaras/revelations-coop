/*
 * test_local1.c - rev1 live round 11: the enemies the host does not simulate (native/local1.h) and a model of the two PCs around the 13_048_0 zombies.
 * The l10e timeline (R7: host = Parker far away, guest = Jill at the key): the host's units of 0/0, 0/1 go away after the spawn, the host keeps the records
 * ALIVE and refreshes them every second, the guest's engine re-creates its units as own AI (FUN_00464320), the guest's Jill kills them. Pass = both PCs end
 * with both records dead / used up, so the host's story (Rachel n7 dead_ck) runs on as the guest's does, and no enemy is simulated twice. The model before
 * the fix (the client keeps every enemy a replica and never reports) leaves the host's records alive: the check "host dead check" fails for it.
 * The lead order (host = Jill at the key) must not change: the host simulates, the client shows replicas, nothing is taken.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../native/local1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* ---- a model of one enemy record on both PCs */
typedef struct {
    /* host */
    int h_unit, h_hidden, h_spent, h_model_alive;
    float h_hp;
    /* client */
    int c_unit, c_kind;   /* 2 own AI, 3 replica */
    float c_hp;
    int c_spent;
    /* wire */
    int report_pending;
    unsigned rep_type;
    float rep_hp;
} rec_t;

static lr_t T;
static int fixed = 1;   /* 0 = the model before the fix */

/* host side of the batch: the record's ai / e1c as host_pass writes them */
static void host_record(rec_t *r, unsigned *ai, uint32_t *e1c)
{
    *ai = r->h_unit ? 7u : LR_AI_NOUNIT;
    *e1c = 0x40004000u | (r->h_hidden ? LR_E1C_HIDDEN : 0);
}

/* the client's per-pass step through the rule; returns the action */
static int client_pass(rec_t *r, uint16_t key, float dist, uint32_t now)
{
    if (!fixed || !r->c_unit) return LR_NONE;
    int a = lr_step(&T, key, 0x24000000u + key, (unsigned)r->c_kind, r->c_hp > 0.0f, r->c_hp, dist, now);
    switch (a) {
    case LR_MAKE_REPLICA: r->c_kind = 3; break;
    case LR_MAKE_LOCAL: r->c_kind = 2; break;
    case LR_REPORT_DEATH: r->report_pending = 1, r->rep_type = LR_REC_DEATH, r->rep_hp = 0.0f; break;
    case LR_HAND_BACK: r->c_kind = 3, r->report_pending = 1, r->rep_type = LR_REC_HP, r->rep_hp = r->c_hp; break;
    }
    return a;
}

/* the host applies a report */
static int host_apply(rec_t *r)
{
    if (!r->report_pending) return -1;
    r->report_pending = 0;
    uint8_t b[LR_MSG_LEN];
    unsigned t, k;
    uint16_t key;
    float hp;
    lr_enc(b, r->rep_type, 0x0001, r->rep_hp, 0);
    CHECK(lr_dec(b, sizeof b, &t, &key, &hp, &k) && t == r->rep_type && key == 1 && k == 0, "the report decodes");
    int d = lr_host_decide(t, r->h_unit, r->h_hidden, r->h_spent, r->h_hp, hp);
    if (d == LR_H_SPEND || d == LR_H_SPEND_KILL) {
        r->h_spent = 1, r->h_model_alive = 0;
        if (d == LR_H_SPEND_KILL) r->h_unit = 0;
    } else if (d == LR_H_SET_HP) r->h_hp = hp;
    return d;
}

static void t_codec(void)
{
    uint8_t b[LR_MSG_LEN];
    unsigned t, k;
    uint16_t key;
    float hp;
    CHECK(lr_enc(b, LR_REC_HP, 0x6403, 123.5f, 1) == LR_MSG_LEN, "enc length");
    CHECK(lr_dec(b, LR_MSG_LEN, &t, &key, &hp, &k) && t == LR_REC_HP && key == 0x6403 && hp == 123.5f && k == 1, "dec fields");
    CHECK(!lr_dec(b, LR_MSG_LEN - 1, &t, &key, &hp, &k), "dec refuses a short body");
    b[0] = 9;
    CHECK(!lr_dec(b, LR_MSG_LEN, &t, &key, &hp, &k), "dec refuses an unknown type");
    CHECK(lr_record_sim(7, 0x40004000u) && !lr_record_sim(LR_AI_NOUNIT, 0x40004000u) && !lr_record_sim(7, 0x40005000u), "record: simulated = a unit, not hidden");
}

static void t_host_decide(void)
{
    CHECK(lr_host_decide(LR_REC_DEATH, 0, 0, 0, 0, 0) == LR_H_SPEND, "death of a record without a unit on the host: used up");
    CHECK(lr_host_decide(LR_REC_DEATH, 1, 1, 0, 1100, 0) == LR_H_SPEND_KILL, "death of a hidden host unit: killed + used up");
    CHECK(lr_host_decide(LR_REC_DEATH, 1, 0, 0, 1100, 0) == LR_H_REFUSE, "death of an enemy the host simulates: refused (the host's enemy)");
    CHECK(lr_host_decide(LR_REC_DEATH, 0, 0, 1, 0, 0) == LR_H_NOTHING, "death of a used-up record: nothing");
    CHECK(lr_host_decide(LR_REC_HP, 1, 0, 0, 1100, 400) == LR_H_SET_HP, "hand-back hp lowers the host's hp");
    CHECK(lr_host_decide(LR_REC_HP, 1, 0, 0, 300, 400) == LR_H_NOTHING, "hand-back hp never raises it");
    CHECK(lr_host_decide(LR_REC_HP, 0, 0, 0, 0, 400) == LR_H_NOTHING, "hand-back hp without a host unit: nothing");
}

/* R7 l10e: host Parker far away (watching), guest Jill at the key */
static int run_r7(int fix)
{
    fixed = fix;
    lr_reset(&T);
    rec_t z[2];
    memset(z, 0, sizeof z);
    int bad = 0;
    uint32_t now = 1000;
    for (int i = 0; i < 2; i++) z[i].h_unit = 1, z[i].h_hp = 1100 + 100 * i, z[i].h_model_alive = 1, z[i].c_unit = 1, z[i].c_kind = 3, z[i].c_hp = z[i].h_hp;
    /* 20:10:59 the spawn on both: for 300 ms the host has units, records carry the real ai */
    for (int t = 0; t < 300; t += 33, now += 33)
        for (int i = 0; i < 2; i++) {
            unsigned ai; uint32_t e1c;
            host_record(&z[i], &ai, &e1c);
            lr_on_record(&T, (uint16_t)i, ai, e1c, now);
            if (client_pass(&z[i], (uint16_t)i, 2400.0f, now) == LR_MAKE_LOCAL) bad++;   /* never taken while the host simulates */
        }
    /* the host's units go away (no human of the host near): records keep coming every second with ai 0xFFFF */
    for (int i = 0; i < 2; i++) z[i].h_unit = 0;
    /* 20:11:15 13_048_0: the guest's engine re-creates the units as own AI (FUN_00464320) */
    int took = 0;
    for (int t = 0; t < 8000; t += 100, now += 100) {
        for (int i = 0; i < 2; i++) {
            if (t == 3000) z[i].c_kind = 2;   /* the rearrange */
            if (t % 1000 == 0) {
                unsigned ai; uint32_t e1c;
                host_record(&z[i], &ai, &e1c);
                lr_on_record(&T, (uint16_t)i, ai, e1c, now);
            }
            int a = client_pass(&z[i], (uint16_t)i, 300.0f, now);
            if (a == LR_MAKE_REPLICA) bad++;   /* the host does not simulate it: it must not become a frozen replica */
            if (a == LR_MAKE_LOCAL) took++;
        }
    }
    if (fix && took != 2) bad++;   /* both replicas became own AI before the rearrange came */
    if (fix) for (int i = 0; i < 2; i++) if (z[i].c_kind != 2) bad++;
    /* Jill kills both (client hp 0); the reports go to the host */
    for (int i = 0; i < 2; i++) {
        z[i].c_hp = 0.0f;
        int a = client_pass(&z[i], (uint16_t)i, 300.0f, now);
        if (fix && a != LR_REPORT_DEATH) bad++;
        if (a == LR_REPORT_DEATH && host_apply(&z[i]) != LR_H_SPEND) bad++;
        if (client_pass(&z[i], (uint16_t)i, 300.0f, now + 100) == LR_REPORT_DEATH) bad++;   /* once */
        if ((!lr_kill_allowed(&T, (uint16_t)i)) != fix) bad++;   /* the host's DEATH leaves the client's own corpse alone */
    }
    /* the host's dead check (Rachel n7): both records used up */
    int host_dead = z[0].h_spent && z[1].h_spent;
    CHECK(host_dead == fix, fix ? "R7: the host's records of the key zombies are used up (its dead check agrees with the guest's)"
                                : "the model before the fix: the host's records stay alive (its story waits forever at Rachel n7)");
    return bad;
}

/* lead order: host Jill at the key simulates, the client (Parker far away) has replicas */
static int run_lead(void)
{
    fixed = 1;
    lr_reset(&T);
    rec_t z;
    memset(&z, 0, sizeof z);
    z.h_unit = 1, z.h_hp = 1100, z.h_model_alive = 1, z.c_unit = 1, z.c_kind = 3, z.c_hp = 1100;
    int bad = 0;
    uint32_t now = 5000;
    for (int t = 0; t < 20000; t += 100, now += 100) {
        unsigned ai; uint32_t e1c;
        if (t % 1000 == 0 || t % 300 == 0) host_record(&z, &ai, &e1c), lr_on_record(&T, 0, ai, e1c, now);
        if (client_pass(&z, 0, 4000.0f, now) != LR_NONE) bad++;   /* far: nothing */
        if (client_pass(&z, 0, 200.0f, now) != LR_NONE) bad++;    /* near, but the host simulates it: nothing */
    }
    /* the client's engine re-creates it as own AI (a rearrange near the client's human): turned into a replica at once */
    z.c_kind = 2;
    { unsigned ai; uint32_t e1c; host_record(&z, &ai, &e1c); lr_on_record(&T, 0, ai, e1c, now); }
    if (client_pass(&z, 0, 200.0f, now) != LR_MAKE_REPLICA || z.c_kind != 3) bad++;
    /* the host kills it: DEATH allowed on the replica */
    if (!lr_kill_allowed(&T, 0)) bad++;
    z.c_hp = 0.0f;
    if (client_pass(&z, 0, 200.0f, now) == LR_REPORT_DEATH) bad++;   /* not local: no report */
    CHECK(T.took == 0 && T.reports == 0, "lead order: nothing taken, nothing reported");
    return bad;
}

/* a local enemy the host gets back (its human comes near): the client hands it back once with its hp */
static int run_handback(void)
{
    fixed = 1;
    lr_reset(&T);
    rec_t z;
    memset(&z, 0, sizeof z);
    z.h_unit = 0, z.h_hp = 1100, z.h_model_alive = 1, z.c_unit = 1, z.c_kind = 3, z.c_hp = 1100;
    int bad = 0;
    uint32_t now = 100;
    for (int t = 0; t < 4000; t += 100, now += 100) {
        if (t % 1000 == 0) { unsigned ai; uint32_t e1c; host_record(&z, &ai, &e1c); lr_on_record(&T, 3, ai, e1c, now); }
        client_pass(&z, 3, 500.0f, now);
    }
    if (z.c_kind != 2) bad++;
    z.c_hp = 450.0f;   /* hurt while local */
    z.h_unit = 1;      /* the host's rearrange: its unit is back, full hp */
    int backs = 0;
    for (int t = 0; t < 3000; t += 100, now += 100) {
        if (t % 300 == 0) { unsigned ai; uint32_t e1c; host_record(&z, &ai, &e1c); lr_on_record(&T, 3, ai, e1c, now); }
        if (client_pass(&z, 3, 500.0f, now) == LR_HAND_BACK) backs++;
    }
    if (backs != 1 || z.c_kind != 3) bad++;
    if (host_apply(&z) != LR_H_SET_HP || z.h_hp != 450.0f) bad++;
    /* a hidden host unit (asleep outside the host's human's zone) counts as not simulated: the client takes it again */
    z.h_hidden = 1;
    int took = 0;
    for (int t = 0; t < 4000; t += 100, now += 100) {
        if (t % 1000 == 0) { unsigned ai; uint32_t e1c; host_record(&z, &ai, &e1c); lr_on_record(&T, 3, ai, e1c, now); }
        if (client_pass(&z, 3, 500.0f, now) == LR_MAKE_LOCAL) took++;
    }
    if (took != 1) bad++;
    z.c_hp = 0.0f;
    if (client_pass(&z, 3, 500.0f, now) != LR_REPORT_DEATH) bad++;
    if (host_apply(&z) != LR_H_SPEND_KILL || z.h_unit || !z.h_spent) bad++;
    return bad;
}

/* no flapping: one record every second that alternates between simulated and not must never take the enemy */
static int run_flap(void)
{
    fixed = 1;
    lr_reset(&T);
    rec_t z;
    memset(&z, 0, sizeof z);
    z.h_unit = 1, z.h_hp = 1000, z.c_unit = 1, z.c_kind = 3, z.c_hp = 1000;
    int bad = 0;
    uint32_t now = 100;
    for (int t = 0; t < 20000; t += 100, now += 100) {
        if (t % 1000 == 0) {
            z.h_hidden = (t / 1000) % 2;
            unsigned ai; uint32_t e1c;
            host_record(&z, &ai, &e1c);
            lr_on_record(&T, 9, ai, e1c, now);
        }
        if (client_pass(&z, 9, 300.0f, now) == LR_MAKE_LOCAL) bad++;
    }
    return bad;
}

/* rev1 live 12: the engine's kind rule (local1.h): every state change of the unit sets kind = (owner == the engine's own slot) ? 2 : 3. l11a: the client's 0/1
 * (owner 1 = the host's slot) taken by the kind alone became a replica again at its next state change, eight times in 30 s, while it bit Jill; the client's
 * 0/0 re-created by its own rearrange with owner 0 stays own AI against a replica decision the same way. owner_fix 0 = the glue before (kind only), 1 = the
 * owner goes with the kind (FUN_00459880's way). Returns the number of wrong outcomes. */
static int run_engine_rule(int owner_fix, int *takes, int *double_ai)
{
    fixed = 1;
    lr_reset(&T);
    const int own = 0, host = 1;   /* R7 client: Jill = slot 0 */
    int bad = 0;
    *takes = 0, *double_ai = 0;
    /* 0/1: a replica (owner 1, kind 3) the host does not simulate (hidden there), 80 u from Jill, changing state every 400 ms (attacks) */
    int owner = host, kind = 3;
    uint32_t now = 1000;
    for (int t = 0; t < 30000; t += 100, now += 100) {
        if (t % 1000 == 0) lr_on_record(&T, 1, 7, 0x40005000u, now);   /* hidden on the host */
        int a = lr_step(&T, 1, 0x24e9c9e0u, (unsigned)kind, 1, 1200.0f, 80.0f, now);
        if (a == LR_MAKE_LOCAL) {
            (*takes)++;
            kind = 2;
            if (owner_fix) owner = lr_owner_for(2, own, host);
        }
        if (t % 400 == 0) kind = lr_engine_kind(owner, own);   /* FUN_0049CD20 -> FUN_0049CB70 */
    }
    if (*takes != 1 || kind != 2) bad++;
    /* 0/0: re-created by the client's rearrange with owner 0 (own AI); the host simulates it (a live, visible unit there): a replica, and it must stay one */
    lr_reset(&T);
    owner = own, kind = 2;
    for (int t = 0; t < 10000; t += 100, now += 100) {
        if (t % 300 == 0) lr_on_record(&T, 0, 7, 0x40004000u, now);   /* simulated by the host */
        int a = lr_step(&T, 0, 0x24e87930u, (unsigned)kind, 1, 1100.0f, 300.0f, now);
        if (a == LR_MAKE_REPLICA) {
            kind = 3;
            if (owner_fix) owner = lr_owner_for(3, own, host);
        }
        if (t % 400 == 0) kind = lr_engine_kind(owner, own);
        if (kind == 2 && t > 2000) (*double_ai)++;   /* own AI here while the host's AI runs it too */
    }
    if (*double_ai) bad++;
    return bad;
}

/* rev1 live 12 (l12c, lead order 1-1): the host runs the Ooze 2/0 (a live, awake unit) whose state machine number reads 0xFFFF; the guest's Parker stands
 * 156 u from it. fixed_ai = the host maps a real 0xFFFF to 0xFFFE (lr_host_ai); 0 = the sentinel goes out as it is. Returns the takes. */
static int run_ai_sentinel(int fixed_ai)
{
    lr_reset(&T);
    int took = 0;
    uint32_t now = 1000;
    for (int t = 0; t < 10000; t += 100, now += 100) {
        if (t % 300 == 0) lr_on_record(&T, 0x0200, fixed_ai ? lr_host_ai(0xFFFFFFFFu) : (0xFFFFFFFFu & 0xFFFFu), 0x40000088u, now);
        if (lr_step(&T, 0x0200, 0x24e30580u, 3, 1, 2100.0f, 156.0f, now) == LR_MAKE_LOCAL) took++;
    }
    return took;
}

/* allep step 4 (3-2 / 8-3, run allep3c): the client took the enemy (host record "hidden"), then the host's SPAWN made the unit a replica again (kind 2 -> 3, no HAND_BACK);
 * the host killed it later: its DEATH must reach the replica (the stale local flag made it "left alone", the replica stayed at hp 1000). Returns the bad count. */
static int run_spawn_after_take(void)
{
    lr_reset(&T);
    int bad = 0;
    uint32_t now = 1000;
    int took = 0;
    for (int t = 0; t < 8000 && !took; t += 250, now += 250) {
        if (t % 1000 == 0) lr_on_record(&T, 0x1901, LR_AI_NOUNIT, 0x40002088u, now);
        if (lr_step(&T, 0x1901, 0x24d23530u, 3, 1, 1000.0f, 573.0f, now) == LR_MAKE_LOCAL) took = 1;
    }
    if (!took || lr_kill_allowed(&T, 0x1901)) bad++;   /* local now: a host DEATH would be left alone */
    for (int t = 0; t < 2000; t += 250, now += 250) {   /* the host's SPAWN: kind 3 again, the host simulates it (live records) */
        lr_on_record(&T, 0x1901, 12, 0x40000088u, now);
        lr_step(&T, 0x1901, 0x24d23530u, 3, 1, 1000.0f, 573.0f, now);
    }
    if (!lr_kill_allowed(&T, 0x1901)) bad++;
    return bad;
}

int main(void)
{
    t_codec();
    t_host_decide();
    CHECK(run_r7(1) == 0, "R7 timeline with the fix: no wrong action, both taken, both reported once, the host's dead check agrees");
    int pre = run_r7(0);
    (void)pre;
    CHECK(run_lead() == 0, "lead order: the host simulates, the client turns a rearranged own-AI unit into a replica, nothing taken");
    CHECK(run_handback() == 0, "hand back with the hp; a hidden host unit taken again and killed by the report");
    CHECK(run_flap() == 0, "a record that alternates every second never makes the enemy local");
    {
        int tk, dbl;
        CHECK(run_engine_rule(1, &tk, &dbl) == 0 && tk == 1 && dbl == 0, "the owner with the kind: the taken 0/1 stays own AI through its state changes (one take), the 0/0 replica stays a replica");
        int pre = run_engine_rule(0, &tk, &dbl);
        CHECK(pre > 0 && tk > 1 && dbl > 0, "the glue before (kind only): the engine's rule undoes it (l11a: 0/1 taken again and again, 0/0 own AI on both PCs)");
        printf("engine kind rule: owner fix takes 1 / double 0; kind only takes %d / double AI passes %d\n", tk, dbl);
    }
    CHECK(lr_host_ai(0xFFFFFFFFu) == 0xFFFEu && lr_host_ai(0x12345u) == 0x2345u && lr_record_sim(lr_host_ai(0xFFFFu), 0x40000088u), "a real unit's state machine number 0xFFFF is never the no-unit sentinel");
    CHECK(run_ai_sentinel(1) == 0, "lead order: the host runs the Ooze (machine number 0xFFFF): the guest never takes it");
    CHECK(run_ai_sentinel(0) >= 1, "the host before the fix sent the sentinel for a live Ooze: the guest took it (l12c: runs on THIS PC's AI now ... 156 away)");
    CHECK(run_spawn_after_take() == 0, "a taken enemy turned into a replica by the host's SPAWN: the host's DEATH is applied (not left alone)");
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
