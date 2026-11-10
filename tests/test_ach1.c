/*
 * Offline tests of native/ach1.h (B10): the table of the 50 ids, the game's condition as a pure function (read from cAchievementInfo_CheckCondition 0x86D220), the gate
 * (who may credit what), the dedupe / rate / section limits, and a model of two PCs that applies scripted co-op play and checks that every PC credits exactly its own human.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/ach1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_table(void)
{
    int nr[6] = {0}, ep1_yes = 0, with_setter = 0;
    for (int i = 0; i < ACH1_N; i++) {
        const ach1_entry_t *e = &ach1_table[i];
        CHECK(e->id == i + 1);
        CHECK(ach1_by_id(e->id) == e);
        CHECK(e->rule >= A1R_ACTOR && e->rule <= A1R_NA);
        nr[e->rule]++;
        CHECK(e->setter < A1S_N);
        if (e->setter) {
            with_setter++;
            CHECK(e->rule == A1R_ACTOR || e->rule == A1R_SCAN);
            CHECK(ach1_setters[e->setter].sid == e->setter);
        }
        if (e->id >= 25) CHECK(e->rule == A1R_NA && e->ep1 == A1X_NO);
        if (e->id <= 4) CHECK(e->rule == A1R_CAMPAIGN && e->ep1 == A1X_NO);   /* three episodes each: Episode 1 alone cannot do it */
        if (e->ep1 == A1X_YES) ep1_yes++;
        CHECK(e->cond && e->cond[0]);
    }
    CHECK(ach1_by_id(0) == 0 && ach1_by_id(51) == 0);
    CHECK(nr[A1R_NA] == 26 && nr[A1R_CAMPAIGN] == 8 && nr[A1R_FLAGS] == 5 && nr[A1R_SCAN] == 2 && nr[A1R_ACTOR] == 9);
    CHECK(ep1_yes == 2);                       /* ids 14 and 17: enemy deaths and zombie hits exist in Episode 1 */
    CHECK(with_setter == 11);
    /* setters: every one has a distinct id, a function in the profile class range and sane limits */
    for (unsigned s = A1S_C70; s < A1S_N; s++) {
        const ach1_setter_t *t = &ach1_setters[s];
        CHECK(t->sid == s && t->func >= 0x499200 && t->func <= 0x4999F0 && t->caller > 0x400000 && t->rate > 0 && t->sect_cap >= t->rate);
        CHECK(t->bound == A1B_ACTOR || t->bound == A1B_KILLER);
    }
    CHECK(ach1_setters[A1S_C72].bound == A1B_KILLER && ach1_setters[A1S_C72].cap == 150);
    CHECK(ach1_setters[A1S_C70].cap == 100 && ach1_setters[A1S_C71].cap == 100 && ach1_setters[A1S_C74].cap == 100);
}

static void prof_init(ach1_profile_t *p)
{
    memset(p, 0, sizeof *p);
    p->active = 0xFFFFFFu;
    p->ep_system = 1;
    for (unsigned id = 9; id <= 24; id++) p->target[id] = 10;
    p->target[10] = 20;
    p->target[11] = 30;
    p->target[14] = 150;
}

static void test_check(void)
{
    ach1_profile_t p;
    prof_init(&p);
    /* a fresh profile unlocks nothing but id 21 (condition inverted: true while +0x44 bit 0 is clear; the game only polls it once active) */
    CHECK(ach1_check(&p, 21) == 1);
    for (unsigned id = 1; id <= 50; id++) if (id != 21) CHECK(ach1_check(&p, id) == 0);
    p.active &= ~(1u << 20);
    CHECK(ach1_poll(&p) == 0);
    p.active |= 1u << 20;
    CHECK(ach1_poll(&p) == (1u << 20));
    CHECK(ach1_poll(&p) == 0);                 /* sent once per profile */
    p.b44 |= 1;
    CHECK(ach1_check(&p, 21) == 0);
    /* ids 1-4: three episodes each, any difficulty */
    p.ep_clear[0][0] = 1; p.ep_clear[1][2] = 1;
    CHECK(ach1_check(&p, 1) == 0);
    p.ep_clear[2][1] = 1;
    CHECK(ach1_check(&p, 1) == 1 && ach1_check(&p, 2) == 0);
    for (int e = 3; e < 6; e++) p.ep_clear[e][0] = 1;
    CHECK(ach1_check(&p, 2) == 1 && ach1_check(&p, 3) == 0 && ach1_check(&p, 4) == 0);
    for (int e = 6; e < 12; e++) p.ep_clear[e][1] = 1;
    CHECK(ach1_check(&p, 3) == 1 && ach1_check(&p, 4) == 1);
    /* ids 5-7: bits 0, 1, 2 of +0x3C (id 5 any, id 6 bit 1 or 2, id 7 bit 2 only); id 8 needs the episode system and counter 0 */
    p.b3c = 1;
    CHECK(ach1_check(&p, 5) == 1 && ach1_check(&p, 6) == 0 && ach1_check(&p, 7) == 0 && ach1_check(&p, 8) == 0);
    p.b3c = 2;
    CHECK(ach1_check(&p, 5) == 1 && ach1_check(&p, 6) == 1 && ach1_check(&p, 7) == 0 && ach1_check(&p, 8) == 1);
    p.deaths_counter = 3;
    CHECK(ach1_check(&p, 8) == 0);
    p.deaths_counter = 0; p.ep_system = 0;
    CHECK(ach1_check(&p, 8) == 0);
    p.ep_system = 1; p.b3c = 4;
    CHECK(ach1_check(&p, 7) == 1 && ach1_check(&p, 6) == 1);
    /* ids 9-11: how many of the 30 flags; the 31st+ bits do not count */
    p.b3c = 0;
    p.flags30 = 0x3FF;
    CHECK(ach1_check(&p, 9) == 1 && ach1_check(&p, 10) == 0);
    p.flags30 = 0xFFFFFFFFu;
    CHECK(ach1_check(&p, 11) == 1);
    p.flags30 = 0xC0000000u;
    CHECK(ach1_check(&p, 9) == 0);
    /* ids 12/13: scan mask of 17 kinds */
    p.scan6c = 0x3FF;
    CHECK(ach1_check(&p, 12) == 1 && ach1_check(&p, 13) == 0);
    p.scan6c = 0x1FFFF;
    CHECK(ach1_check(&p, 13) == 1);
    p.scan6c = 0xFFFE0000u;                    /* bits above the 17 kinds never count */
    CHECK(ach1_check(&p, 12) == 0 && ach1_check(&p, 13) == 0);
    /* counters with targets; a target of 0 (unknown) never reports done */
    p.c72 = 149; CHECK(ach1_check(&p, 14) == 0);
    p.c72 = 150; CHECK(ach1_check(&p, 14) == 1);
    p.target[14] = 0; CHECK(ach1_check(&p, 14) == 0); p.target[14] = 150;
    p.c74 = 10; CHECK(ach1_check(&p, 17) == 1);
    p.c70 = 9; CHECK(ach1_check(&p, 23) == 0); p.c70 = 10; CHECK(ach1_check(&p, 23) == 1);
    p.c71 = 10; CHECK(ach1_check(&p, 24) == 1);
    /* the single bits and flags */
    p.b3c = 0x10; CHECK(ach1_check(&p, 15) == 1 && ach1_check(&p, 18) == 0);
    p.b3c = 0x40; CHECK(ach1_check(&p, 18) == 1);
    p.b3c = 0x80; CHECK(ach1_check(&p, 19) == 1 && ach1_check(&p, 20) == 0);
    p.b3c = 0x100; CHECK(ach1_check(&p, 20) == 1);
    p.f16b = 1; CHECK(ach1_check(&p, 16) == 1 && ach1_check(&p, 22) == 0);
    p.f16a = 1; CHECK(ach1_check(&p, 22) == 1);
    /* Raid ids stay false even with the Raid flag: the mod never evaluates them */
    p.coop_flag = 1;
    for (unsigned id = 25; id <= 50; id++) CHECK(ach1_check(&p, id) == 0);
    CHECK(ach1_check(&p, 0) == 0 && ach1_check(&p, 51) == 0);
}

static void test_apply_caps(void)
{
    ach1_profile_t p;
    prof_init(&p);
    for (int i = 0; i < 200; i++) { ach1_apply(&p, A1S_C70, 0); ach1_apply(&p, A1S_C71, 0); ach1_apply(&p, A1S_C72, 0); ach1_apply(&p, A1S_C74, 0); }
    CHECK(p.c70 == 100 && p.c71 == 100 && p.c72 == 150 && p.c74 == 100);
    ach1_apply(&p, A1S_SCAN, 3); ach1_apply(&p, A1S_SCAN, 16); ach1_apply(&p, A1S_SCAN, 17);
    CHECK(p.scan6c == ((1u << 3) | (1u << 16)));
    ach1_apply(&p, A1S_B3C_4, 0); ach1_apply(&p, A1S_B3C_6, 0); ach1_apply(&p, A1S_B3C_7, 0); ach1_apply(&p, A1S_B3C_8, 0);
    CHECK(p.b3c == (0x10 | 0x40 | 0x80 | 0x100));
    ach1_apply(&p, A1S_B44_0, 0);
    CHECK(p.b44 == 1);
}

static void test_gate_basic(void)
{
    ach1_cfg_t c = {1, 1, 0};
    ach1_state_t s;
    ach1_state_init(&s);
    /* a session off / gate off: the game is untouched whoever acts */
    ach1_cfg_t off = {0, 1, 0}, nogate = {1, 0, 0};
    CHECK(ach1_gate(&off, &s, A1S_C70, A1A_REMOTE, 0, 1000) == A1G_ALLOW);
    CHECK(ach1_gate(&nogate, &s, A1S_C72, A1A_REMOTE, 5, 1000) == A1G_ALLOW);
    CHECK(ach1_gate(&c, &s, A1S_NONE, A1A_REMOTE, 0, 1000) == A1G_ALLOW);   /* unknown setter: not ours */
    CHECK(ach1_gate(&c, &s, 99, A1A_REMOTE, 0, 1000) == A1G_ALLOW);
    /* actor-bound: only a local human */
    for (unsigned sid = A1S_C70; sid < A1S_N; sid++) {
        const ach1_setter_t *t = &ach1_setters[sid];
        ach1_state_t g;
        ach1_state_init(&g);
        CHECK(ach1_gate(&c, &g, sid, A1A_REMOTE, 0, 1000) == A1G_UNDO_REMOTE);   /* the partner's puppet never credits this profile */
        CHECK(ach1_gate(&c, &g, sid, A1A_LOCAL, 0, 1000) == A1G_ALLOW);
        if (t->bound == A1B_ACTOR) {
            CHECK(ach1_gate(&c, &g, sid, A1A_NONE, 0, 1000) == A1G_UNDO_NOACTOR);
            CHECK(ach1_gate(&c, &g, sid, A1A_AI, 0, 1000) == A1G_UNDO_NOACTOR);
        } else {
            CHECK(ach1_gate(&c, &g, sid, A1A_NONE, 0, 1000) == A1G_ALLOW);       /* a trap kill counts, as in single player */
            CHECK(ach1_gate(&c, &g, sid, A1A_AI, 0, 1000) == A1G_ALLOW);
        }
    }
    /* tainted: nothing is credited */
    ach1_cfg_t t = {1, 1, 1};
    ach1_state_init(&s);
    CHECK(ach1_gate(&t, &s, A1S_C72, A1A_LOCAL, 7, 1000) == A1G_UNDO_TAINT);
    CHECK(s.refused[A1G_UNDO_TAINT] == 1);
    /* a refusal leaves no trace: the same cause is credited later when the actor is right */
    ach1_state_init(&s);
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_REMOTE, 7, 1000) == A1G_UNDO_REMOTE);
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 7, 1001) == A1G_ALLOW);
    CHECK(s.sect_n[A1S_C72] == 1);
}

static void test_gate_limits(void)
{
    ach1_cfg_t c = {1, 1, 0};
    ach1_state_t s;
    /* dedupe by key inside a section instance, other keys and other setters are independent, a new section starts again */
    ach1_state_init(&s);
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 0x10001, 100) == A1G_ALLOW);
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 0x10001, 200) == A1G_UNDO_DUP);    /* the local death and the replayed W_DEATH of the same enemy */
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 0x10002, 200) == A1G_ALLOW);
    CHECK(ach1_gate(&c, &s, A1S_C74, A1A_LOCAL, 0x10001, 200) == A1G_ALLOW);
    ach1_new_section(&s);
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 0x10001, 300) == A1G_ALLOW);       /* a retry respawns the enemy: a fresh cause, as in single player */
    /* key 0 never dedupes (ladders repeat) */
    ach1_state_init(&s);
    CHECK(ach1_gate(&c, &s, A1S_C70, A1A_LOCAL, 0, 100) == A1G_ALLOW);
    CHECK(ach1_gate(&c, &s, A1S_C70, A1A_LOCAL, 0, 100) == A1G_ALLOW);
    /* rate: ladder 3 per second */
    ach1_state_init(&s);
    int ok = 0, rate = 0;
    for (int i = 0; i < 10; i++) {
        int r = ach1_gate(&c, &s, A1S_C70, A1A_LOCAL, 0, 5000 + i * 10);
        if (r == A1G_ALLOW) ok++; else if (r == A1G_UNDO_RATE) rate++;
    }
    CHECK(ok == 3 && rate == 7);
    CHECK(ach1_gate(&c, &s, A1S_C70, A1A_LOCAL, 0, 6100) == A1G_ALLOW);          /* a new window */
    CHECK(s.refused[A1G_UNDO_RATE] == 7);
    /* the rate is per setter, the clock may wrap */
    CHECK(ach1_gate(&c, &s, A1S_C71, A1A_LOCAL, 0, 6100) == A1G_ALLOW);
    ach1_state_init(&s);
    CHECK(ach1_gate(&c, &s, A1S_C70, A1A_LOCAL, 0, 0xFFFFFF00u) == A1G_ALLOW);
    CHECK(ach1_gate(&c, &s, A1S_C70, A1A_LOCAL, 0, 0x00000100u) == A1G_ALLOW);   /* 512 ms across the wrap: same window */
    /* section cap */
    ach1_state_init(&s);
    int allowed = 0;
    for (int i = 0; i < 1000; i++)
        if (ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 0x20000 + i, 10000 + i * 100) == A1G_ALLOW) allowed++;
    CHECK(allowed == ach1_setters[A1S_C72].sect_cap);
    CHECK(s.refused[A1G_UNDO_SECT] == 1000 - allowed);
    ach1_new_section(&s);
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 0x20000, 900000) == A1G_ALLOW);
    /* a full ledger refuses nothing by itself */
    ach1_state_init(&s);
    for (int i = 0; i < 300; i++) ach1_ledger_add(&s, ((unsigned long long)A1S_C72 << 56) | (0x30000 + i));
    CHECK(ach1_gate(&c, &s, A1S_C72, A1A_LOCAL, 0x40000, 1000) == A1G_ALLOW);
}

static void test_claims_and_clear(void)
{
    ach1_cfg_t c = {1, 1, 0}, t = {1, 1, 1}, off = {0, 1, 0};
    CHECK(ach1_claim_ok(1, 1) && !ach1_claim_ok(1, 0) && !ach1_claim_ok(0, 1) && !ach1_claim_ok(-1, -1));
    CHECK(ach1_clear_ok(&c, 1, 1, 1) == 1);
    CHECK(ach1_clear_ok(&c, 0, 1, 1) == 0);    /* the local menu difficulty differs from the session's: no clear under a difficulty that was not played */
    CHECK(ach1_clear_ok(&c, 1, 1, 0) == 0);    /* never reached the end (left the session / warp) */
    CHECK(ach1_clear_ok(&t, 1, 1, 1) == 0);
    CHECK(ach1_clear_ok(&off, 0, 2, 0) == 1);  /* no session: the game untouched */
    CHECK(ach1_block_default(1, '1', 0) == 1 && ach1_block_default(1, '0', 1) == 0 && ach1_block_default(0, 0, 1) == 1 && ach1_block_default(0, 0, 0) == 0);
}

/* ---- two PCs ---------------------------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { ach1_profile_t prof; ach1_state_t st; ach1_cfg_t cfg; int unlocked_log[8], nlog; } pc_t;

static void pc_init(pc_t *p)
{
    memset(p, 0, sizeof *p);
    prof_init(&p->prof);
    p->cfg.session = 1; p->cfg.gate_on = 1;
    ach1_state_init(&p->st);
}

/* the game on PC `p` runs setter `sid` for `actor`; the glue asks the gate first and runs the game's write only on ALLOW; then the game polls */
static int pc_event(pc_t *p, unsigned sid, int actor, unsigned key, unsigned now, unsigned arg)
{
    int r = ach1_gate(&p->cfg, &p->st, sid, actor, key, now);
    if (r == A1G_ALLOW) ach1_apply(&p->prof, sid, arg);
    unsigned u = ach1_poll(&p->prof);
    for (unsigned id = 1; id <= 24; id++) if (u & (1u << (id - 1))) p->unlocked_log[p->nlog++ % 8] = (int)id;
    return r;
}

static void test_two_pcs(void)
{
    pc_t host, cl;
    pc_init(&host); pc_init(&cl);
    /* both profiles are one kill short of ids 14 (150) and 17 (target 10), one ladder short of 23: the lab setup of the live test (H13) */
    host.prof.c72 = cl.prof.c72 = 149;
    host.prof.c74 = cl.prof.c74 = 9;
    host.prof.c70 = cl.prof.c70 = 9;
    host.prof.active &= ~(1u << 20); cl.prof.active &= ~(1u << 20);          /* id 21 is not polled here */
    /* the HOST's human kills enemy 1: the host PC sees a local killer, the client PC runs the replayed death with the host's human as killer = REMOTE for the client */
    CHECK(pc_event(&host, A1S_C72, A1A_LOCAL, 0x10001, 1000, 0) == A1G_ALLOW);
    CHECK(pc_event(&cl, A1S_C72, A1A_REMOTE, 0x10001, 1010, 0) == A1G_UNDO_REMOTE);
    CHECK(host.prof.c72 == 150 && cl.prof.c72 == 149);
    CHECK((host.prof.sent & (1u << 13)) && !(cl.prof.sent & (1u << 13)));      /* id 14 only on the host */
    /* the CLIENT's human kills enemy 2 (the host sees a remote human killer) */
    CHECK(pc_event(&host, A1S_C72, A1A_REMOTE, 0x10002, 2000, 0) == A1G_UNDO_REMOTE);
    CHECK(pc_event(&cl, A1S_C72, A1A_LOCAL, 0x10002, 2010, 0) == A1G_ALLOW);
    CHECK(cl.prof.c72 == 150 && (cl.prof.sent & (1u << 13)));
    /* the zombie hit by the client's human on the host: host side is the partner's action, client side own (the replica runs the same path) */
    CHECK(pc_event(&host, A1S_C74, A1A_REMOTE, 0, 3000, 0) == A1G_UNDO_REMOTE);
    CHECK(host.prof.c74 == 9 && !(host.prof.sent & (1u << 16)));
    CHECK(pc_event(&cl, A1S_C74, A1A_LOCAL, 0, 3010, 0) == A1G_ALLOW);
    CHECK(cl.prof.c74 == 10 && (cl.prof.sent & (1u << 16)) && !(host.prof.sent & (1u << 16)));
    /* the client's puppet on the host climbs a ladder: not credited to the host */
    CHECK(pc_event(&host, A1S_C70, A1A_REMOTE, 0, 4000, 0) == A1G_UNDO_REMOTE);
    CHECK(host.prof.c70 == 9 && !(host.prof.sent & (1u << 22)));
    CHECK(pc_event(&host, A1S_C70, A1A_LOCAL, 0, 4500, 0) == A1G_ALLOW);
    CHECK(host.prof.sent & (1u << 22));
    CHECK(!(cl.prof.sent & (1u << 22)) && cl.prof.c70 == 9);
    /* the environment kills an enemy on both PCs: each profile counts it once (as in single player) */
    host.prof.c72 = cl.prof.c72 = 10;
    CHECK(pc_event(&host, A1S_C72, A1A_NONE, 0x10003, 5000, 0) == A1G_ALLOW);
    CHECK(pc_event(&cl, A1S_C72, A1A_NONE, 0x10003, 5005, 0) == A1G_ALLOW);
    CHECK(host.prof.c72 == 11 && cl.prof.c72 == 11);
    /* a duplicated message (the same death twice on the client) credits once */
    CHECK(pc_event(&cl, A1S_C72, A1A_LOCAL, 0x10004, 6000, 0) == A1G_ALLOW);
    CHECK(pc_event(&cl, A1S_C72, A1A_LOCAL, 0x10004, 6010, 0) == A1G_UNDO_DUP);
    CHECK(cl.prof.c72 == 12);
    /* scanning: a scan by the partner never lands in this profile's mask */
    CHECK(pc_event(&host, A1S_SCAN, A1A_LOCAL, 3, 7000, 3) == A1G_ALLOW);
    CHECK(pc_event(&cl, A1S_SCAN, A1A_REMOTE, 3, 7010, 3) == A1G_UNDO_REMOTE);
    CHECK(host.prof.scan6c == (1u << 3) && cl.prof.scan6c == 0);
    /* a forged flood (a client sending kills of 50 enemies in a second) is cut by the rate limit, the profile moves by at most the rate */
    pc_t victim; pc_init(&victim);
    int allowed = 0;
    for (int i = 0; i < 50; i++) if (pc_event(&victim, A1S_C72, A1A_LOCAL, 0x50000 + i, 100000 + i * 5, 0) == A1G_ALLOW) allowed++;
    CHECK(allowed == ach1_setters[A1S_C72].rate && victim.prof.c72 == ach1_setters[A1S_C72].rate);
    /* a partner cannot award this PC anything: claims for another slot are dropped */
    CHECK(!ach1_claim_ok(1, 0));
}

static void test_flag_idempotent(void)
{
    /* rule A1R_FLAGS: flags set by pickups / scenario FSMs on every PC through its own world: a replay cannot overcount (set-bits) */
    ach1_profile_t a, b;
    prof_init(&a); prof_init(&b);
    for (int i = 0; i < 12; i++) a.flags30 |= 1u << i;
    for (int i = 0; i < 12; i++) a.flags30 |= 1u << i;      /* the pickup replayed twice */
    b.flags30 = a.flags30;                                   /* the partner's replayed pickups, same world */
    CHECK(ach1_pop(a.flags30) == 12 && ach1_check(&a, 9) == 1 && ach1_check(&b, 9) == 1 && ach1_check(&a, 10) == 0);
}

/* rel010: the glue (native/ach1_game.c). (1) who acted, from the unit at the call; (2) the six call sites are what the glue patches (the image of build
 * 13298487, REV1_IMAGE or ../../image/rerev-image.bin: `mov ecx, [0x116B3A4]` + E8 rel32 to the setter, and the setter increments the counter the stub
 * returns on a skip); (3) the host / client model: the partner's kill, zombie hit, ladder and tech action on this PC (his puppet; on the host his PCHIT) are
 * not credited here, the own ones are; the model before the gate (the setter always runs) gives the host the partner's kill (it must fail). */
static int g_model_gate = 1;
static int glue_event(pc_t *p, unsigned sid, int slot, int kind, int own, unsigned now)
{
    int actor = ach1_actor_class(slot, kind, own);
    int r = g_model_gate ? ach1_gate(&p->cfg, &p->st, sid, actor, 0, now) : A1G_ALLOW;
    if (r == A1G_ALLOW) ach1_apply(&p->prof, sid, 0);
    return r;
}

static int glue_model(int gate)
{
    int bad = 0;
    pc_t host, cl;
    g_model_gate = gate;
    pc_init(&host); pc_init(&cl);
    host.cfg.session = cl.cfg.session = 1;
    host.prof.c72 = cl.prof.c72 = 149;
    host.prof.c74 = cl.prof.c74 = 9;
    host.prof.c70 = cl.prof.c70 = 9;
    host.prof.c71 = cl.prof.c71 = 9;
    /* the host plays slot 0, the client slot 1. The client's blow kills a host enemy: the host's rank hook sees the client's puppet (slot 1, kind 1) as the attacker */
    glue_event(&host, A1S_C72, 1, 1, 0, 1000);
    if (host.prof.c72 != 149) bad++;
    /* the host's own kill on the host: credited */
    glue_event(&host, A1S_C72, 0, 1, 0, 2000);
    if (host.prof.c72 != 150) bad++;
    /* the host's puppet on the client hits a zombie replica, climbs a ladder, makes a charge tech: the client's profile stays */
    glue_event(&cl, A1S_C74, 0, 1, 1, 3000);
    glue_event(&cl, A1S_C70, 0, 3, 1, 3100);
    glue_event(&cl, A1S_C71, 0, 1, 1, 3200);
    if (cl.prof.c74 != 9 || cl.prof.c70 != 9 || cl.prof.c71 != 9) bad++;
    /* the client's own zombie hit and ladder: credited on the client */
    glue_event(&cl, A1S_C74, 1, 1, 1, 4000);
    glue_event(&cl, A1S_C70, 1, 1, 1, 4100);
    if (cl.prof.c74 != 10 || cl.prof.c70 != 10) bad++;
    /* the partner left, the game's AI drives his character (kind 2): its kill counts as in single player, its ladder does not move this profile */
    host.prof.c72 = 10;
    glue_event(&host, A1S_C72, 1, 2, 0, 5000);
    glue_event(&host, A1S_C70, 1, 2, 0, 5100);
    if (host.prof.c72 != 11 || host.prof.c70 != 9) bad++;
    /* a trap kill (no player attacker) counts on each PC */
    glue_event(&cl, A1S_C72, -1, -1, 1, 6000);
    if (cl.prof.c72 != 150) bad++;
    /* outside a session every call stands (single player, split screen) */
    pc_t solo; pc_init(&solo);
    solo.cfg.session = 0;
    solo.prof.c72 = 5;
    glue_event(&solo, A1S_C72, 1, 1, 0, 7000);
    if (solo.prof.c72 != 6) bad++;
    g_model_gate = 1;
    return bad;
}

static void test_glue(void)
{
    /* (1) the actor class */
    CHECK(ach1_actor_class(0, 1, 0) == A1A_LOCAL && ach1_actor_class(1, 1, 1) == A1A_LOCAL);
    CHECK(ach1_actor_class(1, 1, 0) == A1A_REMOTE && ach1_actor_class(1, 3, 0) == A1A_REMOTE && ach1_actor_class(0, 1, 1) == A1A_REMOTE);
    CHECK(ach1_actor_class(1, 2, 0) == A1A_AI && ach1_actor_class(-1, -1, 0) == A1A_NONE);
    CHECK(ach1_actor_class(0, 2, 0) == A1A_LOCAL);   /* the own slot is never someone else's */
    for (int r = A1G_ALLOW; r <= A1G_UNDO_TAINT; r++) CHECK(strcmp(ach1_result_name(r), "?") != 0);
    /* (2) the sites */
    CHECK(ACH1_NSITES == 6);
    for (int i = 0; i < ACH1_NSITES; i++) {
        const ach1_site_t *s = &ach1_sites[i];
        CHECK(s->sid > A1S_NONE && s->sid < A1S_N && ach1_setters[s->sid].func == s->setter);
        CHECK(s->off == 0x70 + (s->sid == A1S_C70 ? 0 : s->sid == A1S_C71 ? 1 : s->sid == A1S_C72 ? 2 : 4));
        CHECK(s->src <= A1C_HIT_STACK && s->what && s->what[0]);
    }
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    FILE *f = fopen(path, "rb");
    if (f) {
        static unsigned char img[0x1000000];
        size_t n = fread(img, 1, sizeof img, f);
        fclose(f);
        int ok_sites = 0, ok_setters = 0;
        for (int i = 0; i < ACH1_NSITES; i++) {
            const ach1_site_t *s = &ach1_sites[i];
            size_t o = s->site - 0x400000u;
            static const unsigned char mov_ecx[6] = {0x8B, 0x0D, 0xA4, 0xB3, 0x16, 0x01};
            if (o + 5 <= n && o >= 6 && !memcmp(img + o - 6, mov_ecx, 6) && img[o] == 0xE8) {
                int rel;
                memcpy(&rel, img + o + 1, 4);
                if (s->site + 5u + (unsigned)rel == s->setter) ok_sites++;
            }
            /* the setter increments the counter the stub returns: `inc byte ptr [esi + off]` (FE 46 off) after the campaign check, about 0x6D bytes in */
            size_t so = s->setter - 0x400000u;
            int seen = 0;
            for (size_t k = so; k + 3 < n && k < so + 0xA0; k++)
                if (img[k] == 0xFE && img[k + 1] == 0x46 && img[k + 2] == s->off) { seen = 1; break; }
            ok_setters += seen;
        }
        CHECK(ok_sites == ACH1_NSITES);
        CHECK(ok_setters == ACH1_NSITES);
        printf("image %s: %d of %d call sites `mov ecx,[0x116B3A4]; call <setter>`, %d setters touch their counter\n", path, ok_sites, ACH1_NSITES, ok_setters);
    } else
        printf("skip image checks (%s not found)\n", path);
    /* (3) the model, with the gate and before it */
    int with = glue_model(1), without = glue_model(0);
    CHECK(with == 0);
    CHECK(without > 0);
    printf("  the model before the gate (every setter call stands): %d of 7 checks fail (the partner's kill / hit / ladder / tech land on this profile)\n", without);
}

int main(void)
{
    test_table();
    test_check();
    test_apply_caps();
    test_gate_basic();
    test_gate_limits();
    test_claims_and_clear();
    test_two_pcs();
    test_flag_idempotent();
    test_glue();
    printf("ach1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
