/*
 * test_zkeep1.c - rev1 live round 17: a replica the host simulates stays visible and hittable on the client across the client's own zone passes
 * (native/zkeep1.h rule 7), on a model of the client's engine: FUN_004592E0's per-frame hide (E1C 0x1000 outside the own human's zone, the reset FUN_0044F510
 * kill 1 on the frame the own human leaves the unit's zone, a corpse killed there), the logic thread's list every 250 ms (two buffers), the hook's un-hide
 * right after the pass, the client's shots (a hidden unit takes none) and local1.h's take rule.
 *  A  m16a1 (R7: the guest = Jill): the host's Parker and the Ooze in 256/7 (the host simulates it); the guest's Jill fights, then walks into 257/1 while the
 *     Ooze stays in 256/7, 200 u away, and keeps shooting. The model before the fix: hidden from the crossing on, every shot goes through, one reset. Fixed:
 *     every shot lands, no reset, the unit is never hidden when the frame is drawn.
 *  B  m16a3 (lead order: the guest = Parker) + the guest walks back into the Ooze's zone and out again: the same on the other slot; no reset either way.
 *  C  an enemy the host does NOT simulate (its human left: its record says hidden): the client's engine sleeps it exactly as before (pre-fix = fixed), and
 *     local1.h still takes it near the client's human (LR_MAKE_LOCAL after LR_TAKE_MS); far away it stays asleep; the rule never lists a kind 2 unit.
 *  D  after the host's DEATH (lr_on_death), with stale records, far away (> ZK_NEAR), or while this PC watches: not kept (the stock hide).
 *  E  a corpse (state type 12): the zone pass's kill is never skipped; outside the pass or with kill 0 nothing is skipped.
 *  F  the glue in the engine files (source checks).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/local1.h"
#include "../native/zkeep1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

#define Z(b, u) (((b) << 8) | (u))
#define KEY 0x0200u   /* 2/0: the 1-1 Ooze */

/* ---- the client's unit of the enemy */
typedef struct {
    int zone, was_in_zone, hidden, resets, skipped, killed, kind, state_type, awake;
    float hp;
} cu_t;

/* the logic thread's list (watch_visible_list): two buffers, the engine reads cur */
typedef struct { int cnt[2], keep[2], cur; } list_t;

/* FUN_004592E0 for this unit (own_now / own_before = the zones of the client's own human now and in the frame before), then watch_after_zpass */
static void zone_pass(cu_t *u, int own_now, int own_before, const list_t *L, int zk_on)
{
    int listed = L->cnt[L->cur] > 0, keep = listed && L->keep[L->cur];
    if (u->killed) return;
    if (u->zone == own_now) {
        u->hidden = 0, u->was_in_zone = 1;
    } else if (u->was_in_zone && !u->hidden) {
        if (!u->awake) {   /* a zombie whose vf101 says "awake" stays visible (stock); a replica never is */
            u->hidden = 1;
            if (u->zone == own_before) {   /* FUN_0044F510(unit, 1) through the hook */
                if (zk_skip_reset(zk_on, keep, 1, 1, (unsigned)u->state_type)) u->skipped++;
                else {
                    u->resets++;
                    if (u->state_type == (int)ZK_STATE_CORPSE) u->killed = 1;
                }
            }
        }
    }
    if (listed && u->hidden) u->hidden = 0;   /* watch_after_zpass */
}

/* the client's per-250-ms pass: local1.h's rule and the zone-keep list */
static void logic_pass(lr_t *T, cu_t *u, list_t *L, int zk_on, int watching, float dist, uint32_t now)
{
    int alive = u->hp > 0.0f && !u->killed;
    int a = lr_step(T, KEY, 0x24bf8660u, (unsigned)u->kind, alive, u->hp, dist, now);
    if (a == LR_MAKE_LOCAL) u->kind = 2;
    if (a == LR_MAKE_REPLICA || a == LR_HAND_BACK) u->kind = 3;
    lr_ent *e = lr_find(T, KEY, 0);
    int keep = e && zk_keep(zk_on, 1, watching, (unsigned)u->kind, alive, lr_host_simulates(e, now), e->reported, dist);
    int nb = 1 - L->cur;
    L->cnt[nb] = keep, L->keep[nb] = keep, L->cur = nb;
}

typedef struct {
    int shots, hits, resets, skipped, hidden_frames, took_local, kind_end;
} res_t;

/* one fight: frames of 50 ms for `secs`; the host's records every 100 ms (host_sim: its unit awake = the record's E1C not hidden; records stop at
 * rec_stop_ms); the client's human: zone by the schedule, distance `dist`; shots every 500 ms from shoot_from to shoot_to */
typedef struct {
    int zk_on, watching, host_sim, death_at_ms, rec_stop_ms, enemy_zone, state_type, kind0, back_in_ms, back_out_ms;
    int cross_ms, zone_before, zone_after, shoot_from, shoot_to, secs;
    float dist;
} scen_t;

static res_t run(const scen_t *s)
{
    lr_t T;
    lr_reset(&T);
    cu_t u = {0};
    list_t L = {{0, 0}, {0, 0}, 0};
    res_t r = {0};
    u.zone = s->enemy_zone, u.kind = s->kind0 ? s->kind0 : 3, u.hp = 2100.0f, u.state_type = s->state_type ? s->state_type : 1;
    int own_prev = s->zone_before;
    for (uint32_t t = 1; t <= (uint32_t)s->secs * 1000u; t += 50) {
        int own = (int)t < s->cross_ms ? s->zone_before : s->zone_after;
        if (s->back_in_ms && (int)t >= s->back_in_ms && (!s->back_out_ms || (int)t < s->back_out_ms)) own = s->zone_before;
        if (t % 100 < 50 && (!s->rec_stop_ms || (int)t < s->rec_stop_ms))
            lr_on_record(&T, KEY, 7u, 0x40000088u | (s->host_sim ? 0u : LR_E1C_HIDDEN), t);
        if (s->death_at_ms && (int)t >= s->death_at_ms) lr_on_death(&T, KEY);
        if (t % 250 < 50) logic_pass(&T, &u, &L, s->zk_on, s->watching, s->dist, t);
        zone_pass(&u, own, own_prev, &L, s->zk_on);
        own_prev = own;
        if (u.hidden) r.hidden_frames++;
        if ((int)t >= s->shoot_from && (int)t < s->shoot_to && t % 500 < 50) {
            r.shots++;
            if (!u.hidden && !u.killed && u.hp > 0.0f) r.hits++;   /* HITX sent: the shot met the replica */
        }
    }
    r.resets = u.resets, r.skipped = u.skipped, r.took_local = (int)T.took, r.kind_end = u.kind;
    return r;
}

static scen_t fight(int zk_on, int guest_zone_after)
{
    scen_t s = {0};
    s.zk_on = zk_on, s.host_sim = 1, s.enemy_zone = Z(256, 7), s.zone_before = Z(256, 7), s.zone_after = guest_zone_after;
    s.cross_ms = 5000, s.shoot_from = 6000, s.shoot_to = 16000, s.secs = 20, s.dist = 200.0f;
    return s;
}

static void run_a(void)
{
    scen_t s0 = fight(0, Z(257, 1));
    res_t p = run(&s0);
    printf("A before the fix (m16a1 R7): shots %d, hits %d, resets %d, hidden frames %d\n", p.shots, p.hits, p.resets, p.hidden_frames);
    CHECK(p.shots == 20 && p.hits == 0, "A the model before the fix: every shot after the crossing goes through the hidden replica (m16a1: HITX sent 0)");
    CHECK(p.resets == 1, "A the model before the fix: the zone pass resets the replica once (zone leave kill 1)");
    CHECK(!(p.hits == p.shots), "A the fixed-rule check (every shot lands) FAILS for the pre-fix model");
    scen_t s1 = fight(1, Z(257, 1));
    res_t f = run(&s1);
    printf("A fixed: shots %d, hits %d, resets %d, skipped %d, hidden frames %d\n", f.shots, f.hits, f.resets, f.skipped, f.hidden_frames);
    CHECK(f.shots == 20 && f.hits == 20, "A fixed: every shot after the crossing lands (the replica stays hittable)");
    CHECK(f.resets == 0 && f.skipped == 1, "A fixed: the zone pass's reset is skipped once, never run");
    CHECK(f.hidden_frames == 0, "A fixed: the replica is never hidden when the frame is drawn");
    CHECK(f.took_local == 0 && f.kind_end == 3, "A fixed: local1 never takes it (the host simulates it): one owner");
}

static void run_b(void)
{
    /* lead order: the same rule on the other slot (the guest = Parker; the rule has no slot); then the guest walks back in at 9 s and out again at 12 s */
    scen_t s = fight(1, Z(256, 6));
    s.back_in_ms = 9000, s.back_out_ms = 12000;
    res_t f = run(&s);
    printf("B fixed (m16a3 lead order, out / in / out): hits %d of %d, resets %d, skipped %d, hidden frames %d\n", f.hits, f.shots, f.resets, f.skipped, f.hidden_frames);
    CHECK(f.hits == f.shots && f.resets == 0 && f.skipped == 2 && f.hidden_frames == 0, "B fixed: out, in and out again: every shot lands, both resets skipped");
    s.zk_on = 0;
    res_t p = run(&s);
    printf("B before the fix: hits %d of %d, resets %d\n", p.hits, p.shots, p.resets);
    CHECK(p.hits < p.shots && p.resets == 2, "B before the fix: the shots outside the Ooze's zone go through, two resets");
}

static void run_c(void)
{
    /* the host does not simulate it (its own human left; its record says hidden): the client's human 200 u away crosses too */
    scen_t s = fight(1, Z(257, 1));
    s.host_sim = 0;
    res_t f = run(&s);
    s.zk_on = 0;
    res_t p = run(&s);
    printf("C host does not simulate, near: fixed took %d kind %d resets %d | before took %d kind %d resets %d\n", f.took_local, f.kind_end, f.resets, p.took_local, p.kind_end,
           p.resets);
    CHECK(f.took_local == 1 && f.kind_end == 2, "C fixed: local1 still takes the enemy the host does not simulate (rule 3)");
    CHECK(f.took_local == p.took_local && f.kind_end == p.kind_end && f.resets == p.resets && f.skipped == 0, "C the fix changes nothing for it (same as before the fix)");
    CHECK(!zk_keep(1, 1, 0, 2, 1, 0, 0, 200.0f) && !zk_keep(1, 1, 0, 2, 1, 1, 0, 200.0f), "C a kind 2 unit (own AI) is never listed");
    s.dist = 3500.0f, s.zk_on = 1;
    res_t far = run(&s);
    CHECK(far.took_local == 0 && far.kind_end == 3 && far.resets == 1 && far.hidden_frames > 0, "C far away: asleep on the client as before, not taken");
}

static void run_d(void)
{
    scen_t s = fight(1, Z(257, 1));
    s.death_at_ms = 4000;
    res_t d = run(&s);
    CHECK(d.skipped == 0 && d.resets == 1 && d.hidden_frames > 0, "D after the host's DEATH: not kept (the stock hide)");
    s = fight(1, Z(257, 1));
    s.rec_stop_ms = 2000;
    res_t st = run(&s);
    CHECK(st.skipped == 0 && st.resets == 1, "D stale records (the host's batch stopped): not kept");
    s = fight(1, Z(257, 1));
    s.dist = ZK_NEAR + 50.0f;
    res_t far = run(&s);
    CHECK(far.skipped == 0 && far.hidden_frames > 0, "D farther than ZK_NEAR: not kept");
    s = fight(1, Z(257, 1));
    s.watching = 1;
    res_t w = run(&s);
    CHECK(w.skipped == 0 && w.hidden_frames > 0, "D while this PC watches: rule 7 lists nothing (rule 4 of watch1.h decides)");
    CHECK(!zk_keep(1, 0, 0, 3, 1, 1, 0, 100.0f), "D the host never uses rule 7");
    CHECK(!zk_keep(0, 1, 0, 3, 1, 1, 0, 100.0f) && zk_keep(1, 1, 0, 3, 1, 1, 0, 100.0f), "D the switch");
}

static void run_e(void)
{
    scen_t s = fight(1, Z(257, 1));
    s.state_type = (int)ZK_STATE_CORPSE;
    res_t c = run(&s);
    CHECK(c.resets == 1 && c.skipped == 0, "E a corpse's zone-pass kill is never skipped");
    CHECK(!zk_skip_reset(1, 1, 0, 1, 1), "E outside the zone pass nothing is skipped");
    CHECK(!zk_skip_reset(1, 1, 1, 0, 1), "E kill 0 (FUN_004592E0 never calls it so; other callers) is not skipped");
    CHECK(!zk_skip_reset(1, 0, 1, 1, 1), "E a unit not listed by rule 7 (rule 4 or none) is not skipped");
    CHECK(zk_skip_reset(1, 1, 1, 0x101, 1), "E only the low byte of kill counts (a char argument)");
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

static void run_f(void)
{
    char *w = slurp("../native/world1_game.c"), *l = slurp("../native/local1_game.inc"), *g = slurp("../native/watch1_game.inc");
    CHECK(w && strstr(w, "{\"zkeep\", &f_zkeep, 1}"), "F `world set zkeep` exists");
    CHECK(l && strstr(l, "#include \"zkeep1.h\"") && strstr(l, "zk_skip_reset(f_zkeep && g_client, zk_listed_keep(u), zk_pass_tid == GetCurrentThreadId(), kill,"),
          "F the zone-leave hook skips the reset only for a listed rule-7 replica inside the pass");
    CHECK(l && strstr(l, "zk_pass_tid = GetCurrentThreadId();") && strstr(l, "zk_pass_tid = 0;"), "F the zone-pass hook marks its thread around the stock pass");
    CHECK(l && strstr(l, "rd32(wv_entry[c & 1][i] + 4) == u"), "F the zone-leave hook checks that the entry still points at the unit");
    CHECK(g && strstr(g, "zk_keep(f_zkeep, g_client, g_solo_watch, kind, alive, lr_host_simulates(le, now), le->reported,"), "F the list uses the rule with local1's host-simulates");
    CHECK(g && strstr(g, "InterlockedExchange(&wv_cnt[nb], (LONG)k);") && strstr(g, "InterlockedExchange(&wv_cur, nb);"), "F the list is filled into the other buffer, then switched");
    CHECK(g && strstr(g, "watch_visible_list(now);") && strstr(g, "& ~LR_E1C_HIDDEN"), "F the frame builds the list; the hook un-hides");
    free(w), free(l), free(g);
}

int main(void)
{
    run_a();
    run_b();
    run_c();
    run_d();
    run_e();
    run_f();
    printf("test_zkeep1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
