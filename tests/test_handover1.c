/*
 * test_handover1.c - fix021 (Rev1 0.2.1, notes/fix021.md): an enemy the host's engine sleeps while the guest's PC drives it continues from the guest's state
 * when the host wakes it (watch1.h rules 2z / 3z / 6, local1.h rules 4a / 5a), on a model of both PCs: the host's zone pass (sleep + the zone-leave reset),
 * both AIs, the host's batch, the client's local1 table, the client's records (CP_W1_RLOC), the reports (CP_W1_LOCAL), the guest's HITX, the host's DEATH.
 *  A  fr1b in 1-1: the host's Jill steps into the next zone (within 1500 u), the Ooze chases the guest, the guest's shots take 2100 -> 1404 on its own copy,
 *     then the host's Jill comes (a) to the zone where the enemy really is, (b) back to the zone it was reset in. 0.2.0 (MODEL BEFORE THE FIX FAILS): (a) the
 *     host's copy never wakes (2100, frozen at the reset point), (b) it wakes at the stale point (the guest's copy jumps there). 0.2.1: the host's copy
 *     followed the guest's, wakes where the enemy is with its hp, the guest's copy becomes a replica in place, the kill is one death on both PCs.
 *  B  the wake-up while shots fly: the guest's bursts and the host's own blows around the take-back / hand-back: every point of damage counts once.
 *  C  a level reset (game over + Continue, a section change) while handed over: nothing stale survives.
 *  D  several enemies at once (3 and 5: two record messages per round).
 *  E  an enemy asleep on BOTH PCs.
 *  F  the Ooze's grab at the wake-up: the hand-back waits for the release (rule 5a), at most LR_HOLD_MAX_MS.
 *  G  link loss: the records stop; the host keeps the last mirrored state.
 *  H  the host's blow kills the reverse replica: one death (rule 4a); 0.2.0 left the guest's copy alive.
 *  I  pure functions (rehold, mirror / hand-back hp, settle) and the glue in the engine files (source checks).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/watch1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

#define TICK 50u
#define NE 6
#define SPEED 60.0f   /* u/s, both AIs */

static int zone_of(float x) { return x < -500.0f ? 2 : x < 0.0f ? 0 : 1; }   /* C | A | B */

typedef struct {
    uint16_t key;
    float spawn, maxhp;
    /* host */
    int h_alive, h_kind, h_hidden, h_spent, h_death_sent;
    float h_pos, h_hp;
    /* client */
    int c_alive, c_kind, c_reported;
    float c_pos, c_hp, c_last_pos;
    /* counters */
    int host_deaths, client_deaths, death_reports, hp_reports, rev_made, taken_back, handed_back;
    float max_jump_after;   /* the client's copy: the largest move in one tick after the return of the host's human */
    float dmg_guest, dmg_host;
    float take_pos, take_hp;   /* the host's unit at its first take-back */
} en_t;

/* ---- the message line (in order, a fixed latency) */
enum { M_REC = 1, M_RLOC, M_LOCAL, M_HITX, M_DEATH };
typedef struct { int type; uint32_t at; uint16_t key; unsigned ai; uint32_t e1c; float pos, hp; unsigned rtype; uint8_t buf[WT_MSG_MAX]; unsigned len; } msg_t;
#define QN 4096
typedef struct { msg_t m[QN]; int h, t; } q_t;
static q_t to_client, to_host;
static void q_push(q_t *q, const msg_t *m) { int n = (q->h + 1) % QN; if (n != q->t) q->m[q->h] = *m, q->h = n; }

/* ---- the world */
typedef struct {
    int fixed;              /* 0 = the 0.2.0 rules */
    en_t e[NE];
    int ne;
    float hx, gx;           /* the host's human, the guest's human */
    int hzone_prev;
    lr_t L;                 /* the client's local1 table */
    wt_host W;              /* the host's watch table */
    wt_tx TX;
    uint32_t now, last_host_pass, last_client_pass, last_batch;
    int link;               /* 0 = the link is lost (nothing arrives) */
    int own_held;           /* the client's own human is held (a grab) */
    int g_asleep_too;       /* E: the guest's human far from the enemy's zone */
    uint32_t t_return;      /* the host's human came back */
    int back_while_held;
} world_t;
static world_t Wd;

static int host_hits_ok(const en_t *e) { return e->h_alive && !e->h_hidden; }

static void host_kill(world_t *w, en_t *e, int by_report)
{
    if (!e->h_alive) return;
    e->h_alive = 0, e->host_deaths++;
    if (!e->h_death_sent) {   /* the host's model sends DEATH once */
        e->h_death_sent = 1;
        msg_t m = {.type = M_DEATH, .at = w->now + 60, .key = e->key};
        q_push(&to_client, &m);
    }
    (void)by_report;
}

/* the host's engine: zone pass (sleep, the zone-leave reset), its AI, the batch */
static void host_engine(world_t *w)
{
    int hz = zone_of(w->hx);
    for (int i = 0; i < w->ne; i++) {
        en_t *e = &w->e[i];
        if (!e->h_alive) continue;
        int z = zone_of(e->h_pos);
        if (hz != w->hzone_prev && z == w->hzone_prev && z != hz) {   /* FUN_0044F510: the unit in the zone the own human just left is reset */
            int skip = w->fixed && e->h_kind == 3;                      /* fix021: never a reverse replica (its client drives it) */
            if (!skip) e->h_pos = e->spawn;
        }
        e->h_hidden = zone_of(e->h_pos) != hz;   /* FUN_004592E0: away from the own human's zone = asleep */
        if (e->h_kind == 2 && !e->h_hidden) {    /* the host's AI chases the guest's puppet */
            float d = w->gx - e->h_pos, s = SPEED * TICK / 1000.0f;
            e->h_pos += fabsf(d) < s ? d : (d > 0 ? s : -s);
        }
    }
    w->hzone_prev = hz;
    if (w->now - w->last_batch >= 100) {
        w->last_batch = w->now;
        for (int i = 0; i < w->ne; i++) {
            en_t *e = &w->e[i];
            if (!e->h_alive) continue;
            unsigned ai = wt_batch_ai(&w->W, e->key, 7u);
            msg_t m = {.type = M_REC, .at = w->now + 60, .key = e->key, .ai = ai, .e1c = 0x40000088u | (e->h_hidden ? LR_E1C_HIDDEN : 0), .pos = e->h_pos, .hp = e->h_hp};
            q_push(&to_client, &m);
        }
    }
}

/* the host's watch pass (every 250 ms, watch_pass) */
static void host_pass(world_t *w)
{
    if (w->now - w->last_host_pass < 250) return;
    w->last_host_pass = w->now;
    for (unsigned k = 0; k < WT_MAX; k++) {
        wt_ent *we = &w->W.e[k];
        if (!we->used || !(we->active || wt_fresh(we, w->now))) continue;
        en_t *e = 0;
        for (int i = 0; i < w->ne; i++) if (w->e[i].key == we->key) e = &w->e[i];
        if (!e) continue;
        unsigned kind = e->h_alive ? (unsigned)e->h_kind : 0;
        float own_dist = fabsf(w->hx - we->pos[0]);
        int a = w->fixed ? wt_host_step_zone(&w->W, we, kind, 1, own_dist, own_dist, e->h_hidden, w->now) : wt_host_step(&w->W, we, kind, 1, own_dist, own_dist, w->now);
        if (a == WT_MAKE_REPLICA) e->h_kind = 3, e->rev_made++;
        else if (a == WT_TAKE_BACK) {
            if (!e->taken_back) e->take_pos = e->h_pos, e->take_hp = e->h_hp;
            e->h_kind = 2, e->taken_back++;
            continue;
        }
        if (!we->active || !e->h_alive || e->h_kind != 3 || !wt_fresh(we, w->now)) continue;
        e->h_pos = we->pos[0];
        e->h_hp = w->fixed ? wt_mirror_hp(we->hp, we->hd) : we->hp;
    }
}

static void host_rx(world_t *w, msg_t *m)
{
    en_t *e = 0;
    for (int i = 0; i < w->ne; i++) if (w->e[i].key == m->key) e = &w->e[i];
    if (m->type == M_RLOC) {
        wt_rx(&w->W, m->buf, m->len, w->now);
        return;
    }
    if (!e) return;
    if (m->type == M_HITX) {   /* the guest's blow on a replica: the host's applier */
        if (e->h_alive) {
            e->h_hp -= m->hp;
            if (e->h_kind == 3 && w->fixed) wt_host_blow(&w->W, e->key, m->hp);   /* (cannot happen: the guest sends HITX only for a host-simulated enemy) */
            if (e->h_hp <= 0.0f) host_kill(w, e, 0);
        }
        return;
    }
    if (m->type == M_LOCAL) {
        int hidden = e->h_alive && wt_report_hidden(&w->W, e->key, e->h_hidden);
        if (m->rtype == LR_REC_DEATH) {
            int d = lr_host_decide(LR_REC_DEATH, e->h_alive, hidden, e->h_spent, e->h_hp, 0.0f);
            if (d == LR_H_SPEND || d == LR_H_SPEND_KILL) {
                e->h_spent = 1;
                if (e->h_alive) e->h_alive = 0, e->host_deaths++;
                if (!e->h_death_sent) { e->h_death_sent = 1; msg_t r = {.type = M_DEATH, .at = w->now + 60, .key = e->key}; q_push(&to_client, &r); }
                if (w->fixed) wt_settle(&w->W, e->key);
            }
        } else {
            float eff = w->fixed ? wt_handback_eff(&w->W, e->key, m->hp) : m->hp;
            int d = lr_host_decide(LR_REC_HP, e->h_alive, hidden, e->h_spent, e->h_hp, eff);
            if (d == LR_H_SET_HP) e->h_hp = eff;
            if (w->fixed) wt_settle(&w->W, e->key);
        }
    }
}

/* the client's engine: its local AI chases the guest (asleep away from the guest's zone), a replica follows the host's records */
static void client_engine(world_t *w)
{
    for (int i = 0; i < w->ne; i++) {
        en_t *e = &w->e[i];
        if (!e->c_alive) continue;
        if (e->c_kind == 2) {
            int asleep = w->g_asleep_too;   /* the stock rule: a zombie chasing this PC's own human stays awake across zones (zkeep1.h FACTS) */
            if (!asleep) {
                float d = w->gx - e->c_pos, s = SPEED * TICK / 1000.0f;
                e->c_pos += fabsf(d) < s ? d : (d > 0 ? s : -s);
            }
        }
        if (w->t_return && w->now > w->t_return) {
            float j = fabsf(e->c_pos - e->c_last_pos);
            if (j > e->max_jump_after) e->max_jump_after = j;
        }
        e->c_last_pos = e->c_pos;
    }
}

static void client_rx(world_t *w, msg_t *m)
{
    en_t *e = 0;
    for (int i = 0; i < w->ne; i++) if (w->e[i].key == m->key) e = &w->e[i];
    if (!e) return;
    if (m->type == M_REC) {
        lr_on_record(&w->L, e->key, m->ai, m->e1c, w->now);
        if (e->c_alive && e->c_kind == 3) e->c_pos = m->pos, e->c_hp = m->hp > 0.0f ? m->hp : 1.0f;
    } else if (m->type == M_DEATH) {
        if (lr_kill_allowed(&w->L, e->key)) {
            lr_on_death(&w->L, e->key);
            if (e->c_alive) e->c_alive = 0, e->client_deaths++;
        }
    }
}

/* the client's local1 pass (every 100 ms) and its records (every 125 ms) */
static void client_pass(world_t *w)
{
    if (w->now - w->last_client_pass >= 100) {
        w->last_client_pass = w->now;
        w->L.own_held = w->own_held;
        for (int i = 0; i < w->ne; i++) {
            en_t *e = &w->e[i];
            unsigned held0 = w->L.back_held;
            int a = lr_step(&w->L, e->key, 0x24000000u + e->key, (unsigned)e->c_kind, e->c_alive && e->c_hp > 0.0f, e->c_hp, fabsf(w->gx - e->c_pos), w->now);
            if (w->L.back_held != held0) w->back_while_held++;
            if (a == LR_MAKE_LOCAL) e->c_kind = 2;
            else if (a == LR_MAKE_REPLICA) e->c_kind = 3;
            else if (a == LR_HAND_BACK) {
                e->c_kind = 3, e->handed_back++, e->hp_reports++;
                msg_t r = {.type = M_LOCAL, .at = w->now + 60, .key = e->key, .rtype = LR_REC_HP, .hp = e->c_hp};
                q_push(&to_host, &r);
            } else if (a == LR_REPORT_DEATH) {
                e->death_reports++;
                msg_t r = {.type = M_LOCAL, .at = w->now + 60, .key = e->key, .rtype = LR_REC_DEATH};
                q_push(&to_host, &r);
            }
        }
    }
    if (wt_tx_due(&w->TX, w->now)) {
        wt_rec r[NE];
        unsigned n = 0;
        for (int i = 0; i < w->ne; i++) {
            en_t *e = &w->e[i];
            lr_ent *le = lr_find(&w->L, e->key, 0);
            if (!le || !wt_tx_wants(le->local, le->reported, e->c_alive && e->c_kind == 2)) continue;
            r[n].key = e->key, r[n].pos[0] = e->c_pos, r[n].pos[1] = 0.0f, r[n].pos[2] = 0.0f, r[n].hp = e->c_hp;
            n++;
        }
        for (unsigned k = 0; k < n; k += WT_PER_MSG) {
            msg_t m = {.type = M_RLOC, .at = w->now + 60};
            m.len = wt_enc(m.buf, ++w->TX.seq, r + k, n - k < WT_PER_MSG ? n - k : WT_PER_MSG);
            q_push(&to_host, &m);
        }
    }
}

/* the guest shoots the enemy: its own copy when it runs it, else a replica (HITX to the host); returns 1 when a blow happened */
static int guest_shot(world_t *w, en_t *e, float d)
{
    if (!e->c_alive) return 0;
    e->dmg_guest += d;
    if (e->c_kind == 2) {
        e->c_hp -= d;
        if (e->c_hp <= 0.0f) e->c_hp = 0.0f, e->c_alive = 0, e->client_deaths++;
    } else {
        float keep = e->c_hp - d;
        e->c_hp = keep < 1.0f ? 1.0f : keep;   /* a replica never dies from the client's own blow (world1 hook_onhit) */
        msg_t m = {.type = M_HITX, .at = w->now + 60, .key = e->key, .hp = d};
        q_push(&to_host, &m);
    }
    return 1;
}

/* the host's own human shoots: only an awake unit can be hit */
static int host_shot(world_t *w, en_t *e, float d)
{
    if (!host_hits_ok(e)) return 0;
    e->dmg_host += d;
    e->h_hp -= d;
    if (w->fixed) wt_host_blow(&w->W, e->key, d);   /* rule 6: summed while the key's sum is open (reverse replica .. the hand-back report) */
    if (e->h_hp <= 0.0f) host_kill(w, e, 0);
    return 1;
}

static void level_reset(world_t *w)   /* world1_game.c: a new level instance -> local_reset (lr_reset, wt_host_reset) */
{
    lr_reset(&w->L);
    wt_host_reset(&w->W);
    int fixed = w->fixed;
    w->L.v020 = !fixed;
    for (int i = 0; i < w->ne; i++) {
        en_t *e = &w->e[i];
        e->h_alive = e->c_alive = 1, e->h_kind = 2, e->c_kind = 3, e->h_spent = e->h_death_sent = 0;
        e->h_pos = e->c_pos = e->c_last_pos = e->spawn, e->h_hp = e->c_hp = e->maxhp;
    }
    to_client.h = to_client.t = to_host.h = to_host.t = 0;
}

static void world_init(world_t *w, int fixed, int ne, float hx, float gx)
{
    memset(w, 0, sizeof *w);
    w->fixed = fixed, w->ne = ne, w->hx = hx, w->gx = gx, w->link = 1, w->now = 1000;
    w->hzone_prev = zone_of(hx);
    for (int i = 0; i < ne; i++) w->e[i].key = (uint16_t)(0x0200 + i), w->e[i].spawn = -100.0f - 10.0f * (float)i, w->e[i].maxhp = 2100.0f;
    level_reset(w);
}

static void step(world_t *w)
{
    w->now += TICK;
    host_engine(w);
    while (to_host.t != to_host.h && to_host.m[to_host.t].at <= w->now) {
        msg_t m = to_host.m[to_host.t];
        to_host.t = (to_host.t + 1) % QN;
        if (w->link) host_rx(w, &m);
    }
    host_pass(w);
    client_engine(w);
    while (to_client.t != to_client.h && to_client.m[to_client.t].at <= w->now) {
        msg_t m = to_client.m[to_client.t];
        to_client.t = (to_client.t + 1) % QN;
        if (w->link) client_rx(w, &m);
    }
    client_pass(w);
}
static void run_until(world_t *w, uint32_t t) { while (w->now < t) step(w); }

/* ---- A: fr1b. ret: hp_ok (host = client = 2100 - guest damage), jump (the guest's copy jumped > 120 u after the return), deaths */
typedef struct { int hp_equal, awake_on_host, client_replica, jumped, one_death, guest_dmg_kept; float h_hp, c_hp, h_pos, c_pos; } res_t;
static res_t run_A(int fixed, float return_to)
{
    world_t *w = &Wd;
    world_init(w, fixed, 1, -300.0f, 300.0f);
    en_t *e = &w->e[0];
    run_until(w, 2000);
    w->hx = -700.0f;                                   /* the host's Jill steps into the next zone (C), 400-1000 u from the Ooze */
    for (uint32_t t = 6000; t <= 7500; t += 500) {     /* the guest's four bursts (2100 -> 1404) once it runs the Ooze */
        run_until(w, t);
        guest_shot(w, e, 174.0f);
    }
    run_until(w, 12000);
    w->hx = return_to, w->t_return = w->now;            /* the host's Jill comes back */
    run_until(w, 17000);
    res_t r;
    memset(&r, 0, sizeof r);
    r.h_hp = e->h_hp, r.c_hp = e->c_hp, r.h_pos = e->h_pos, r.c_pos = e->c_pos;
    r.hp_equal = fabsf(e->h_hp - e->c_hp) <= 174.0f;
    r.guest_dmg_kept = fabsf(e->h_hp - (2100.0f - e->dmg_guest)) <= 1.0f;
    r.awake_on_host = e->h_alive && !e->h_hidden && e->h_kind == 2;
    r.client_replica = e->c_kind == 3;
    r.jumped = e->max_jump_after > 120.0f;
    /* the kill: the guest shoots its replica until the host's copy dies */
    for (int k = 0; k < 20 && e->h_alive; k++) {
        guest_shot(w, e, 174.0f);
        run_until(w, w->now + 400);
    }
    run_until(w, w->now + 2000);
    r.one_death = e->host_deaths == 1 && e->client_deaths == 1 && e->h_death_sent == 1 && e->death_reports <= 1 && !e->h_alive && !e->c_alive;
    return r;
}

/* ---- B: shots around the wake-up; conservation of damage */
static int run_B(int fixed, float *h_hp_out, float *want_out)
{
    world_t *w = &Wd;
    world_init(w, fixed, 1, -300.0f, 300.0f);
    en_t *e = &w->e[0];
    run_until(w, 2000);
    w->hx = -700.0f;
    run_until(w, 6000);
    guest_shot(w, e, 300.0f);
    run_until(w, 11000);
    w->hx = 250.0f, w->t_return = w->now;
    for (uint32_t t = 11000; t < 15000; t += 100) {   /* the guest's and the host's blows through the whole wake-up window */
        run_until(w, t);
        guest_shot(w, e, 10.0f);
        if (t % 200 == 0) host_shot(w, e, 7.0f);
    }
    run_until(w, 18000);
    *h_hp_out = e->h_hp, *want_out = 2100.0f - e->dmg_guest - e->dmg_host;
    return fabsf(e->h_hp - *want_out) <= 1.0f && fabsf(e->c_hp - e->h_hp) <= 1.0f;
}

int main(void)
{
    /* A: fr1b, the host's Jill comes to the zone where the enemy is (B) / back to the zone it was reset in (A) */
    res_t a0 = run_A(0, 250.0f), a1 = run_A(1, 250.0f), b0 = run_A(0, -200.0f), b1 = run_A(1, -200.0f);
    printf("A(a) to the enemy's zone: 0.2.0 host hp %.0f client %.0f awake %d | 0.2.1 host hp %.0f client %.0f awake %d jump %d pos %.0f / %.0f\n", a0.h_hp, a0.c_hp,
           a0.awake_on_host, a1.h_hp, a1.c_hp, a1.awake_on_host, a1.jumped, a1.h_pos, a1.c_pos);
    printf("A(b) to the reset zone:   0.2.0 host hp %.0f client %.0f jump %d | 0.2.1 host hp %.0f client %.0f jump %d\n", b0.h_hp, b0.c_hp, b0.jumped, b1.h_hp, b1.c_hp, b1.jumped);
    CHECK(!(a0.hp_equal && a0.awake_on_host && a0.guest_dmg_kept), "MODEL BEFORE THE FIX FAILS: A(a) 0.2.0 - the host's copy stays asleep at its reset point with the old hp");
    CHECK(b0.jumped, "MODEL BEFORE THE FIX FAILS: A(b) 0.2.0 - the host's copy wakes at the stale reset point: the guest's copy jumps there");
    CHECK(a1.hp_equal && a1.guest_dmg_kept, "A(a) 0.2.1: the host's hp = the guest's (2100 - the guest's damage)");
    CHECK(a1.awake_on_host && a1.client_replica, "A(a) 0.2.1: the host runs it again (own AI, awake), the guest's copy is a replica");
    CHECK(!a1.jumped && fabsf(a1.h_pos - a1.c_pos) < 30.0f, "A(a) 0.2.1: no jump of the guest's copy, both copies in one place");
    CHECK(a1.one_death, "A(a) 0.2.1: the kill is one death on both PCs (one DEATH, no report)");
    CHECK(b1.hp_equal && b1.guest_dmg_kept && !b1.jumped, "A(b) 0.2.1: the host's Jill in the old zone: the copy is where the guest's was (no jump), hp kept");
    CHECK(b1.one_death, "A(b) 0.2.1: one death");
    CHECK(a1.awake_on_host == 1, "A(a) 0.2.1: awake on the host");

    /* B: every point of damage once */
    float hh, want;
    int b_ok1 = run_B(1, &hh, &want);
    printf("B shots around the wake-up: 0.2.1 host hp %.1f, the sum of all blows says %.1f\n", hh, want);
    CHECK(b_ok1, "B 0.2.1: the guest's bursts and the host's blows around the take-back / hand-back all count once (host = client = 2100 - all damage)");
    float hh0, want0;
    int b_ok0 = run_B(0, &hh0, &want0);
    printf("B 0.2.0: host hp %.1f, the sum says %.1f\n", hh0, want0);
    CHECK(!b_ok0, "MODEL BEFORE THE FIX FAILS: B 0.2.0 - damage lost or double around the wake-up");

    /* C: a level reset while handed over */
    {
        world_t *w = &Wd;
        world_init(w, 1, 1, -300.0f, 300.0f);
        run_until(w, 2000);
        w->hx = -700.0f;
        run_until(w, 7000);
        guest_shot(w, &w->e[0], 500.0f);
        run_until(w, 8000);
        int was = wt_reverse(&w->W, w->e[0].key) && lr_find(&w->L, w->e[0].key, 0) && lr_find(&w->L, w->e[0].key, 0)->local;
        level_reset(w);   /* game over + Continue / a section change */
        w->hx = -300.0f;
        run_until(w, 9000);
        en_t *e = &w->e[0];
        lr_ent *le = lr_find(&w->L, e->key, 0);
        CHECK(was, "C: handed over before the reset (the guest's AI, the host's reverse replica)");
        CHECK(!wt_reverse(&w->W, e->key) && !(le && le->local) && e->h_kind == 2 && e->c_kind == 3 && e->h_hp == 2100.0f && e->c_hp == 2100.0f,
              "C: after the reset nothing stale: the host runs the new unit, the guest's is a replica, full hp on both");
        wt_ent *we = wt_find(&w->W, e->key, 0);
        CHECK(!we || (!we->open && we->hd == 0.0f && !we->back_ms), "C: no open damage sum, no take-back hold left");
    }

    /* D: several enemies at once (3, then 5 = two record messages per round) */
    for (int n = 3; n <= 5; n += 2) {
        world_t *w = &Wd;
        world_init(w, 1, n, -300.0f, 300.0f);
        run_until(w, 2000);
        w->hx = -700.0f;
        run_until(w, 7000);
        for (int i = 0; i < n; i++) guest_shot(w, &w->e[i], 100.0f * (float)(i + 1));
        run_until(w, 11000);
        w->hx = 250.0f, w->t_return = w->now;
        run_until(w, 16000);
        int ok = 1;
        for (int i = 0; i < n; i++) {
            en_t *e = &w->e[i];
            if (fabsf(e->h_hp - (2100.0f - 100.0f * (float)(i + 1))) > 1.0f || fabsf(e->c_hp - e->h_hp) > 1.0f || e->h_kind != 2 || e->c_kind != 3 || fabsf(e->h_pos - e->c_pos) > 30.0f) ok = 0;
        }
        char msg[120];
        snprintf(msg, sizeof msg, "D %d enemies handed over at once: each continues on the host with its own hp and place", n);
        CHECK(ok, msg);
    }

    /* E: asleep on BOTH PCs (the guest's human away too): the host's copy follows a still one; the host's Jill enters its zone */
    {
        world_t *w = &Wd;
        world_init(w, 1, 1, -300.0f, 300.0f);
        run_until(w, 2000);
        w->hx = -700.0f;
        run_until(w, 6000);
        guest_shot(w, &w->e[0], 250.0f);
        w->g_asleep_too = 1;   /* the guest's human walks off: its engine sleeps its local copy */
        float p = w->e[0].c_pos;
        run_until(w, 10000);
        w->hx = (zone_of(p) == 0 ? -200.0f : 250.0f), w->t_return = w->now;
        run_until(w, 15000);
        en_t *e = &w->e[0];
        printf("E asleep on both: p %.0f host hp %.0f pos %.0f kind %d hidden %d | client hp %.0f pos %.0f kind %d\n", p, e->h_hp, e->h_pos, e->h_kind, e->h_hidden, e->c_hp, e->c_pos, e->c_kind);
        CHECK(e->taken_back >= 1 && fabsf(e->take_pos - p) < 30.0f && fabsf(e->take_hp - 1850.0f) <= 1.0f, "E asleep on both PCs: the host wakes it where it stood, with the guest's hp");
        CHECK(fabsf(e->h_hp - 1850.0f) <= 1.0f && fabsf(e->c_hp - 1850.0f) <= 1.0f && e->h_alive && e->c_alive,
              "E: afterwards (its AI chases the far puppet out of the host's zone, the guest's PC drives it again) the hp stays one value on both");
    }

    /* F: the Ooze holds the guest at the wake-up: the hand-back waits for the release (at most LR_HOLD_MAX_MS) */
    {
        world_t *w = &Wd;
        world_init(w, 1, 1, -300.0f, 300.0f);
        run_until(w, 2000);
        w->hx = -700.0f;
        run_until(w, 6000);
        guest_shot(w, &w->e[0], 400.0f);
        run_until(w, 11000);
        w->own_held = 1;
        w->hx = 250.0f, w->t_return = w->now;
        run_until(w, 14000);
        int handed_while_held = w->e[0].handed_back;
        w->own_held = 0;
        run_until(w, 16000);
        en_t *e = &w->e[0];
        CHECK(handed_while_held == 0 && w->back_while_held > 0, "F: no hand-back while the guest is held (rule 5a)");
        CHECK(e->handed_back == 1 && fabsf(e->h_hp - 1700.0f) <= 1.0f && e->c_kind == 3, "F: after the release: handed back once, hp kept");
        world_init(w, 1, 1, -300.0f, 300.0f);   /* held for ever: the hand-back still comes after LR_HOLD_MAX_MS */
        run_until(w, 2000);
        w->hx = -700.0f;
        run_until(w, 11000);
        w->own_held = 1, w->hx = 250.0f;
        run_until(w, 11000 + LR_HOLD_MAX_MS + 2000);
        CHECK(w->e[0].handed_back == 1, "F: a hold never blocks the hand-back longer than LR_HOLD_MAX_MS");
    }

    /* G: link loss while handed over: the host keeps the last mirrored state (0.2.0: the frozen old one) */
    for (int fx = 0; fx <= 1; fx++) {
        world_t *w = &Wd;
        world_init(w, fx, 1, -300.0f, 300.0f);
        run_until(w, 2000);
        w->hx = -700.0f;
        run_until(w, 6000);
        guest_shot(w, &w->e[0], 600.0f);
        run_until(w, 9000);
        w->link = 0;
        run_until(w, 14000);
        en_t *e = &w->e[0];
        if (fx) CHECK(fabsf(e->h_hp - 1500.0f) <= 1.0f && e->h_kind == 2, "G 0.2.1: link lost: the host keeps the guest's last hp (taken back as own AI)");
        else CHECK(e->h_hp == 2100.0f, "MODEL BEFORE THE FIX FAILS: G 0.2.0 - link lost: the host's copy keeps its old hp");
    }

    /* H: the host's blow kills the reverse replica (visible before the take-back pass): one death on both PCs */
    for (int fx = 0; fx <= 1; fx++) {
        world_t *w = &Wd;
        world_init(w, fx, 1, -300.0f, 300.0f);
        en_t *e = &w->e[0];
        run_until(w, 2000);
        w->hx = -700.0f;
        run_until(w, 11000);
        if (!fx) e->h_kind = 3;   /* 0.2.0 made no reverse replica here (the distance gate): force one to show its death rule */
        e->h_hidden = 0;          /* the frame the zone pass woke it, before the 250-ms pass takes it back */
        host_shot(w, e, 5000.0f);
        run_until(w, 14000);
        int one = e->host_deaths == 1 && e->client_deaths == 1 && !e->c_alive;
        if (fx) CHECK(one, "H 0.2.1: the host killed its reverse replica: the guest's copy dies by the DEATH (rule 4a), one death");
        else CHECK(!one, "MODEL BEFORE THE FIX FAILS: H 0.2.0 - the host's DEATH left the guest's local copy alive");
    }

    /* I: pure functions */
    {
        wt_host h;
        wt_host_reset(&h);
        wt_ent *e = wt_find(&h, 7, 1);
        e->rx_ms = 1000;
        CHECK(wt_host_step_zone(&h, e, 2, 1, 200.0f, 200.0f, 1, 1100) == WT_MAKE_REPLICA, "I a fresh record + the host's unit asleep: a reverse replica, whatever the distance");
        CHECK(wt_host_step(&h, wt_find(&h, 8, 1), 2, 1, 200.0f, 200.0f, 1100) == WT_KEEP, "I 0.2.0: within WT_TAKEBACK no reverse replica (the distance gate)");
        CHECK(wt_host_step_zone(&h, e, 3, 1, 200.0f, 200.0f, 0, 1200) == WT_TAKE_BACK && e->back_ms == 1200, "I awake: taken back, the hold starts");
        e->rx_ms = 1300;
        CHECK(wt_host_step_zone(&h, e, 2, 1, 200.0f, 200.0f, 1, 1400) == WT_KEEP && !e->active, "I the hold: no new reverse replica at once");
        e->rx_ms = 1200 + WT_REHOLD_MS;
        CHECK(wt_host_step_zone(&h, e, 2, 1, 200.0f, 200.0f, 1, 1200 + WT_REHOLD_MS + 10) == WT_MAKE_REPLICA, "I after the hold: a reverse replica again");
        wt_host_blow(&h, 7, 40.0f), wt_host_blow(&h, 7, 60.0f), wt_host_blow(&h, 9, 50.0f);
        CHECK(fabsf(e->hd - 100.0f) < 0.01f, "I the host's blows are summed per key");
        CHECK(fabsf(wt_mirror_hp(1500.0f, e->hd) - 1400.0f) < 0.01f && wt_mirror_hp(50.0f, 100.0f) == 1.0f, "I mirrored hp = record - sum, never below 1");
        CHECK(fabsf(wt_handback_eff(&h, 7, 1200.0f) - 1100.0f) < 0.01f && wt_handback_eff(&h, 9, 1200.0f) == 1200.0f, "I hand-back hp - the sum (an unknown key: as it came)");
        CHECK(wt_handback_eff(&h, 7, 80.0f) == 1.0f, "I a hand-back never kills");
        wt_settle(&h, 7);
        CHECK(!e->open && e->hd == 0.0f && wt_handback_eff(&h, 7, 1200.0f) == 1200.0f, "I settled");
        CHECK(wt_host_step_zone(&h, wt_find(&h, 10, 1), 2, 0, 9999.0f, 100.0f, 0, 5000) == WT_KEEP, "I watching: wt_host_step decides (no record: keep)");
        lr_t t;
        lr_reset(&t);
        lr_ent *le = lr_find(&t, 3, 1);
        le->local = 1;
        CHECK(lr_kill_allowed(&t, 3), "I rule 4a: a local copy not reported dies by the host's DEATH");
        t.v020 = 1;
        CHECK(!lr_kill_allowed(&t, 3), "I 0.2.0: a local copy was left alone");
        t.v020 = 0, le->reported = 1;
        CHECK(!lr_kill_allowed(&t, 3), "I a copy this PC reported keeps its own death (no second one)");
    }

    /* I: the glue (source checks) */
    {
        FILE *f = fopen("../native/watch1_game.inc", "rb");
        static char src[400000];
        size_t n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(strstr(src, "wt_host_step_zone(") && strstr(src, "wt_mirror_hp("), "I watch1_game.inc decides by the host's zone and writes the mirrored hp");
        f = fopen("../native/local1_game.inc", "rb");
        n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(strstr(src, "wt_handback_eff(") && strstr(src, "wt_settle(") && strstr(src, "LR.own_held"), "I local1_game.inc: the hand-back hp with the host's blows, settled; the grab input");
        f = fopen("../native/world1_game.c", "rb");
        n = f ? fread(src, 1, sizeof src - 1, f) : 0;
        if (f) fclose(f);
        src[n] = 0;
        CHECK(strstr(src, "wt_host_blow_note(") != NULL, "I world1_game.c: the host's own blows on a handed-over enemy are summed");
    }

    printf("test_handover1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
