/* test_downed.c: offline model of the downed / revive rule (native/downed_logic.h). Two "PCs" each own a dl_t and a toy human (hp, lying flag); a link with latency carries the reliable events (DOWN, DEAD,
 * BEGIN, KEEPALIVE, CANCEL, DONE) and the unreliable 20 Hz STATE flags (which can be dropped); the glue's reactions to the logic's outputs are written the way native/downed.inc executes them.
 * Cases: one human, two humans, both down, timers, revive interrupted, retry, desync, packet loss, a stalled clock, a wrapping clock, three humans, a fuzz run with invariants. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/downed_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ the toy world */
#define MAXQ 256
typedef struct { uint32_t at; int kind, from, arg, is_state, down, dead; } msg_t;

typedef struct {
    dl_t d;
    int idx;
    int hp, lying, dead_screen, being_revived;
    int got_up, die_calls, game_over_outputs, puppet_down[DL_MAX], puppet_up[DL_MAX], revive_play, revive_stop, hold_reset;
    msg_t in[MAXQ];
    int nin;
    uint32_t last_state_send;
    int present;
} pc_t;

typedef struct {
    pc_t pc[DL_MAX];
    int n;
    uint32_t now, latency;
    int drop_state;         /* percent of STATE samples dropped */
    int drop_events;        /* percent of events dropped (the real channel is reliable: used only for the "lost event" desync cases) */
    int cut;                /* the link is cut: nothing is delivered (events are lost) */
    unsigned rng;
    int bad_event_order;
} world_t;

static unsigned rnd(world_t *w) { w->rng ^= w->rng << 13, w->rng ^= w->rng >> 17, w->rng ^= w->rng << 5; return w->rng; }

static void w_init(world_t *w, int n, const dl_cfg *cfg, uint32_t start)
{
    memset(w, 0, sizeof *w);
    w->n = n, w->now = start, w->latency = 40, w->rng = 2463534242u;
    for (int i = 0; i < n; i++) {
        pc_t *p = &w->pc[i];
        p->idx = i, p->hp = 100, p->present = 1;
        dl_init(&p->d, cfg, i, n, start);
    }
}

static void send(world_t *w, int from, int to, int kind, int arg, int is_state, int down, int dead)
{
    pc_t *t = &w->pc[to];
    if (w->cut) return;
    if (!is_state && w->drop_events && (int)(rnd(w) % 100) < w->drop_events) return;
    if (is_state && w->drop_state && (int)(rnd(w) % 100) < w->drop_state) return;
    if (t->nin >= MAXQ) return;
    msg_t *m = &t->in[t->nin++];
    m->at = w->now + w->latency, m->kind = kind, m->from = from, m->arg = arg, m->is_state = is_state, m->down = down, m->dead = dead;
}

/* execute what the logic asked for (the glue's job) */
static void run_outputs(world_t *w, pc_t *p)
{
    dl_out o;
    while (dl_take(&p->d, &o)) {
        switch (o.kind) {
        case DL_O_GO_DOWN: p->hp = 1, p->lying = 1; break;
        case DL_O_DIE: p->hp = 0, p->lying = 0, p->dead_screen = 1, p->die_calls++; break;
        case DL_O_GET_UP: p->hp = 60, p->lying = 0, p->being_revived = 0, p->got_up++; break;
        case DL_O_REVIVE_PLAY: p->being_revived = 1, p->revive_play++; break;
        case DL_O_REVIVE_STOP: p->being_revived = 0, p->revive_stop++; break;
        case DL_O_PUPPET_DOWN: p->puppet_down[o.who]++; break;
        case DL_O_PUPPET_UP: p->puppet_up[o.who]++; break;
        case DL_O_HOLD_RESET: p->hold_reset++; break;
        case DL_O_GAME_OVER: p->game_over_outputs++; break;
        case DL_O_SEND_DOWN: for (int k = 0; k < w->n; k++) if (k != p->idx) send(w, p->idx, k, DL_E_DOWN, 0, 0, 0, 0); break;
        case DL_O_SEND_DEAD: for (int k = 0; k < w->n; k++) if (k != p->idx) send(w, p->idx, k, DL_E_DEAD, 0, 0, 0, 0); break;
        case DL_O_SEND_BEGIN: for (int k = 0; k < w->n; k++) if (k != p->idx) send(w, p->idx, k, DL_E_BEGIN, o.arg, 0, 0, 0); break;
        case DL_O_SEND_KEEPALIVE: for (int k = 0; k < w->n; k++) if (k != p->idx) send(w, p->idx, k, DL_E_KEEPALIVE, o.arg, 0, 0, 0); break;
        case DL_O_SEND_CANCEL: for (int k = 0; k < w->n; k++) if (k != p->idx) send(w, p->idx, k, DL_E_CANCEL, o.arg, 0, 0, 0); break;
        case DL_O_SEND_DONE: for (int k = 0; k < w->n; k++) if (k != p->idx) send(w, p->idx, k, DL_E_DONE, o.arg, 0, 0, 0); break;
        default: break;
        }
    }
}

/* one 50 ms frame: deliver, STATE every frame (20 Hz), tick, outputs */
static void step(world_t *w)
{
    w->now += 50;
    for (int i = 0; i < w->n; i++) {
        pc_t *p = &w->pc[i];
        if (!p->present) continue;
        for (int k = 0; k < p->nin;) {
            msg_t *m = &p->in[k];
            if ((int32_t)(w->now - m->at) < 0) {
                k++;
                continue;
            }
            msg_t c = *m;
            memmove(m, m + 1, (size_t)(p->nin - k - 1) * sizeof *m);
            p->nin--;
            if (c.is_state) dl_remote_sample(&p->d, c.from, c.down, c.dead, w->now);
            else dl_remote_event(&p->d, c.from, c.kind, c.arg, w->now);
        }
    }
    for (int i = 0; i < w->n; i++) {
        pc_t *p = &w->pc[i];
        if (!p->present) continue;
        for (int k = 0; k < w->n; k++)
            if (k != i && w->pc[k].present) send(w, i, k, 0, 0, 1, p->lying, p->hp <= 0);
        dl_tick(&p->d, w->now);
        run_outputs(w, p);
    }
}

static void steps(world_t *w, int ms)
{
    for (int t = 0; t < ms; t += 50) step(w);
}

/* a lethal hit on human i: the hook asks the logic, then the human is clamped or killed */
static int hit(world_t *w, int i)
{
    pc_t *p = &w->pc[i];
    int absorbed = dl_local_lethal(&p->d, w->now);
    if (!absorbed) {
        p->hp = 0, p->dead_screen = 1;
        dl_local_dead(&p->d, w->now);
    }
    run_outputs(w, p);
    return absorbed;
}

/* the interact button of human i held on target for ms, in range */
static void hold(world_t *w, int i, int target, int ms, int in_range)
{
    for (int t = 0; t < ms; t += 50) {
        dl_local_hold(&w->pc[i].d, target, 1, in_range, w->now);
        run_outputs(w, &w->pc[i]);
        step(w);
    }
}
static void release(world_t *w, int i, int target)
{
    dl_local_hold(&w->pc[i].d, target, 0, 0, w->now);
    run_outputs(w, &w->pc[i]);
}

static dl_cfg cfg_on(void)
{
    dl_cfg c;
    memset(&c, 0, sizeof c);
    c.enabled = 1, c.bleed_ms = 60000, c.revive_ms = 3000, c.get_up_on_leave = 1;
    return c;
}

/* ------------------------------------------------------------------ cases */
static void t_off_and_single(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    c.enabled = 0;
    w_init(&w, 2, &c, 1000);
    CHECK(!hit(&w, 0));   /* rule off: an ordinary death */
    CHECK(w.pc[0].hp == 0 && dl_state(&w.pc[0].d, 0) == DL_DEAD && w.pc[0].die_calls == 0);
    steps(&w, 500);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_ALIVE);   /* the rule is off: the mirror is not even fed (the existing coupling does the work) */

    c = cfg_on();
    w_init(&w, 1, &c, 1000);   /* one human in the session */
    CHECK(!hit(&w, 0));
    CHECK(w.pc[0].hp == 0);
    CHECK(dl_count_present(&w.pc[0].d) == 1 && dl_others_up(&w.pc[0].d, 0) == 0);
}

static void t_two_humans_down_and_bleed(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 5000);
    CHECK(hit(&w, 0));   /* A is hit lethally, B is up: A goes down */
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DOWN && w.pc[0].hp == 1 && w.pc[0].lying);
    CHECK(w.pc[0].d.n_down == 1);
    steps(&w, 200);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_DOWN);   /* B's mirror follows (event) */
    CHECK(w.pc[1].puppet_down[0] == 1);
    CHECK(dl_state(&w.pc[1].d, 1) == DL_ALIVE);
    CHECK(!w.pc[0].d.over && !w.pc[1].d.over);   /* B is up: no game over */
    int32_t l0 = dl_bleed_left(&w.pc[0].d, 0);
    steps(&w, 10000);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) < l0 - 9000 && dl_bleed_left(&w.pc[0].d, 0) > l0 - 11000);   /* the timer runs ~10 s */
    CHECK(dl_bleed_left(&w.pc[1].d, 0) < 52000 && dl_bleed_left(&w.pc[1].d, 0) > 48000);          /* the mirror runs too */
    steps(&w, 51000);   /* total 61.4 s since the down: bled out */
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DEAD && w.pc[0].hp == 0 && w.pc[0].die_calls == 1);
    CHECK(w.pc[0].d.n_bleed == 1 && w.pc[0].d.over);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_DEAD);   /* DEAD event */
    CHECK(w.pc[1].d.over && w.pc[1].die_calls == 1 && w.pc[1].hp == 0);   /* a dead human = game over for both (the existing coupling would do it too) */
    CHECK(w.pc[0].game_over_outputs == 1 && w.pc[1].game_over_outputs == 1);
}

static void t_presets_and_infinite(void)
{
    unsigned flags[4] = {CP_SESS_DOWNED | (0u << 1), CP_SESS_DOWNED | (1u << 1), CP_SESS_DOWNED | (2u << 1), CP_SESS_DOWNED | (3u << 1)};
    uint32_t want[4] = {60000, 30000, 120000, 0};
    for (int i = 0; i < 4; i++) {
        dl_cfg c;
        dl_cfg_from_flags(&c, flags[i]);
        CHECK(c.enabled && c.bleed_ms == want[i] && c.revive_ms == DL_REVIVE_DEFAULT_MS);
    }
    dl_cfg off;
    dl_cfg_from_flags(&off, 0);
    CHECK(!off.enabled);
    world_t w;
    dl_cfg c;
    dl_cfg_from_flags(&c, flags[1]);   /* 30 s */
    w_init(&w, 2, &c, 100);
    hit(&w, 1);
    steps(&w, 29000);
    CHECK(dl_state(&w.pc[1].d, 1) == DL_DOWN);
    steps(&w, 2000);
    CHECK(dl_state(&w.pc[1].d, 1) == DL_DEAD);
    dl_cfg_from_flags(&c, flags[3]);   /* never */
    w_init(&w, 2, &c, 100);
    hit(&w, 0);
    steps(&w, 600000);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DOWN && dl_state(&w.pc[1].d, 0) == DL_DOWN && !w.pc[0].d.over);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) == 0x7FFFFFFF);
}

static void t_revive(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 9000);
    hit(&w, 0);
    steps(&w, 5000);
    int32_t before = dl_bleed_left(&w.pc[0].d, 0);
    hold(&w, 1, 0, 1500, 1);   /* B holds the button next to A, 1.5 s of 3 */
    CHECK(dl_being_revived(&w.pc[0].d, 0) && w.pc[0].being_revived && w.pc[0].revive_play >= 1);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) >= before - 100 && dl_bleed_left(&w.pc[0].d, 0) <= before);   /* the timer is frozen while he is being raised */
    hold(&w, 1, 0, 1700, 1);   /* past 3 s in total */
    steps(&w, 300);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_ALIVE && w.pc[0].got_up == 1 && w.pc[0].hp == 60 && !w.pc[0].lying);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_ALIVE);   /* the raiser's mirror too */
    CHECK(w.pc[1].d.n_revive == 1 && w.pc[0].d.n_revive == 1);
    CHECK(!w.pc[0].d.over && !w.pc[1].d.over);
    CHECK(w.pc[1].hold_reset >= 1);
    /* the first hold sends exactly one BEGIN and several keep-alives, no CANCEL */
    CHECK(w.pc[0].d.h[0].reviver == -1);
    /* a second down later starts a fresh timer */
    hit(&w, 0);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) == 60000);
}

static void t_revive_interrupted(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 9000);
    hit(&w, 0);
    steps(&w, 1000);
    hold(&w, 1, 0, 2900, 1);
    release(&w, 1, 0);   /* the button goes up 100 ms short */
    steps(&w, 300);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DOWN && !dl_being_revived(&w.pc[0].d, 0) && w.pc[0].revive_stop == 1);
    CHECK(w.pc[0].lying && w.pc[0].hp == 1);
    int32_t b = dl_bleed_left(&w.pc[0].d, 0);
    steps(&w, 2000);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) < b - 1500);   /* the timer runs again */
    hold(&w, 1, 0, 2900, 1);   /* the second try starts from zero: 2.9 s are not enough again */
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DOWN);
    hold(&w, 1, 0, 400, 1);
    steps(&w, 200);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_ALIVE);
    /* out of range counts as a release */
    hit(&w, 0);
    steps(&w, 200);
    hold(&w, 1, 0, 1000, 1);
    hold(&w, 1, 0, 100, 0);
    steps(&w, 200);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DOWN && !dl_being_revived(&w.pc[0].d, 0));
}

static void t_hold_preconditions(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 100);
    hold(&w, 1, 0, 500, 1);   /* target is up: nothing happens */
    CHECK(w.pc[1].d.h[1].hold_target == -1 && w.pc[0].revive_play == 0);
    hit(&w, 0);
    steps(&w, 200);
    hit(&w, 1);   /* B is lethal-hit while A is down: nobody is up: B dies */
    CHECK(dl_state(&w.pc[1].d, 1) == DL_DEAD);
    /* a down human cannot raise anybody */
    world_t v;
    w_init(&v, 2, &c, 100);
    hit(&v, 0);
    steps(&v, 200);
    dl_local_hold(&v.pc[0].d, 1, 1, 1, v.now);   /* A (down) "holds" on B (up): not a revive */
    CHECK(v.pc[0].d.h[0].hold_target == -1);
    dl_local_hold(&v.pc[1].d, 1, 1, 1, v.now);   /* B on himself */
    CHECK(v.pc[1].d.h[1].hold_target == -1);
    dl_local_hold(&v.pc[1].d, 7, 1, 1, v.now);   /* out of range index */
    CHECK(v.pc[1].d.h[1].hold_target == -1);
}

static void t_watchdog(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 100);
    hit(&w, 0);
    steps(&w, 500);
    hold(&w, 1, 0, 1000, 1);
    CHECK(dl_being_revived(&w.pc[0].d, 0));
    w.cut = 1;   /* the link dies while B holds: no keep-alive reaches A */
    hold(&w, 1, 0, 1500, 1);
    steps(&w, 1500);
    CHECK(!dl_being_revived(&w.pc[0].d, 0) && w.pc[0].revive_stop >= 1);   /* the watchdog ended the revive on A's side */
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DOWN);
    int32_t b = dl_bleed_left(&w.pc[0].d, 0);
    steps(&w, 3000);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) < b - 2500);
}

static void t_both_down(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    w.latency = 80;
    int a = hit(&w, 0), b = hit(&w, 1);   /* in the same millisecond: each sees the other still up */
    CHECK(a && b);
    steps(&w, 400);
    CHECK(w.pc[0].d.over && w.pc[1].d.over);   /* both PCs see everybody down: game over on both */
    CHECK(w.pc[0].die_calls == 1 && w.pc[1].die_calls == 1 && w.pc[0].hp == 0 && w.pc[1].hp == 0);
    CHECK(w.pc[0].game_over_outputs == 1 && w.pc[1].game_over_outputs == 1);
    steps(&w, 5000);
    CHECK(w.pc[0].die_calls == 1 && w.pc[1].die_calls == 1);   /* once */
}

static void t_down_then_partner_lethal(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    hit(&w, 0);
    steps(&w, 300);
    CHECK(!hit(&w, 1));   /* A is down and B takes the lethal hit: B dies at once (nobody is up to raise him) */
    CHECK(dl_state(&w.pc[1].d, 1) == DL_DEAD && w.pc[1].d.over);
    steps(&w, 300);
    CHECK(w.pc[0].d.over && dl_state(&w.pc[0].d, 0) == DL_DEAD && w.pc[0].hp == 0);
}

static void t_reviver_goes_down(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    hit(&w, 0);
    steps(&w, 300);
    hold(&w, 1, 0, 1200, 1);
    CHECK(dl_being_revived(&w.pc[0].d, 0));
    int ab = hit(&w, 1);   /* the reviver is hit while holding: A is down, so B has nobody to raise him: B dies */
    CHECK(!ab);
    steps(&w, 400);
    CHECK(w.pc[0].d.over && w.pc[1].d.over);
    /* variant: B is the second human of three, so he goes down instead and his hold is cancelled */
    world_t v;
    w_init(&v, 3, &c, 1000);
    hit(&v, 0);
    steps(&v, 300);
    hold(&v, 1, 0, 1200, 1);
    CHECK(dl_being_revived(&v.pc[0].d, 0));
    CHECK(hit(&v, 1));   /* human 2 is still up: 1 goes down */
    steps(&v, 300);
    CHECK(v.pc[1].d.h[1].hold_target == -1 && !dl_being_revived(&v.pc[0].d, 0));
    CHECK(dl_state(&v.pc[0].d, 0) == DL_DOWN && dl_state(&v.pc[0].d, 1) == DL_DOWN && !v.pc[0].d.over);
    hold(&v, 2, 0, 3300, 1);   /* human 2 raises human 0, then human 1 */
    steps(&v, 300);
    CHECK(dl_state(&v.pc[0].d, 0) == DL_ALIVE && dl_state(&v.pc[2].d, 0) == DL_ALIVE);
    hold(&v, 0, 1, 3300, 1);
    steps(&v, 300);
    CHECK(dl_state(&v.pc[1].d, 1) == DL_ALIVE && dl_state(&v.pc[0].d, 1) == DL_ALIVE && dl_state(&v.pc[2].d, 1) == DL_ALIVE);
    CHECK(!v.pc[0].d.over && !v.pc[1].d.over && !v.pc[2].d.over);
    /* all three down: game over */
    hit(&v, 0), hit(&v, 1);
    steps(&v, 300);
    CHECK(!v.pc[2].d.over);
    CHECK(!hit(&v, 2));   /* the last one up: nobody to raise him: dies */
    steps(&v, 400);
    CHECK(v.pc[0].d.over && v.pc[1].d.over && v.pc[2].d.over);
}

static void t_second_down_after_revive(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    hit(&w, 0);
    steps(&w, 200);
    hold(&w, 1, 0, 3200, 1);
    steps(&w, 200);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_ALIVE);
    CHECK(hit(&w, 1));   /* now B goes down and A raises him */
    steps(&w, 200);
    hold(&w, 0, 1, 3200, 1);
    steps(&w, 200);
    CHECK(dl_state(&w.pc[1].d, 1) == DL_ALIVE && dl_state(&w.pc[0].d, 1) == DL_ALIVE);
    CHECK(w.pc[0].d.n_down == 2 && w.pc[1].d.n_down == 2);   /* each PC counted its own down and the partner's */
    CHECK(!w.pc[0].d.over && !w.pc[1].d.over);
}

static void t_retry_reset(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    hit(&w, 0);
    hit(&w, 1);
    steps(&w, 400);
    CHECK(w.pc[0].d.over);
    for (int i = 0; i < 2; i++) {   /* a checkpoint load: both reset; the events in flight belong to the old epoch and are dropped (the glue compares the epoch) */
        dl_reset(&w.pc[i].d, w.now);
        w.pc[i].nin = 0;
        w.pc[i].hp = 100, w.pc[i].lying = 0, w.pc[i].dead_screen = 0, w.pc[i].die_calls = 0, w.pc[i].game_over_outputs = 0;
    }
    CHECK(!w.pc[0].d.over && dl_state(&w.pc[0].d, 0) == DL_ALIVE && dl_state(&w.pc[0].d, 1) == DL_ALIVE && w.pc[0].d.nq == 0);
    steps(&w, 1000);
    CHECK(!w.pc[0].d.over && !w.pc[1].d.over);
    CHECK(hit(&w, 0));   /* the rule works again */
    steps(&w, 200);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_DOWN);
    /* a reset mid-revive */
    hold(&w, 1, 0, 1000, 1);
    dl_reset(&w.pc[0].d, w.now), dl_reset(&w.pc[1].d, w.now);
    w.pc[0].nin = w.pc[1].nin = 0;
    CHECK(w.pc[1].d.h[1].hold_target == -1 && !dl_being_revived(&w.pc[0].d, 0));
    CHECK(dl_count_up(&w.pc[0].d) == 2);
}

static void t_desync_and_state(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    w.drop_events = 100;   /* every reliable event is lost (a model of a missed event, the real channel does not do that) */
    hit(&w, 0);
    steps(&w, 300);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_ALIVE);   /* B still thinks A is up */
    steps(&w, 800);   /* STATE says "down" for longer than DL_TRUST_MS */
    CHECK(dl_state(&w.pc[1].d, 0) == DL_DOWN && w.pc[1].d.n_stale_fix >= 1);
    /* A is raised by the engine (not by B's hold), the DONE event is lost: B's mirror follows the STATE flag */
    dl_local_up(&w.pc[0].d, w.now);
    w.pc[0].lying = 0, w.pc[0].hp = 60;
    run_outputs(&w, &w.pc[0]);
    steps(&w, 1200);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_ALIVE);
}

static void t_state_debounce(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    dl_t *d = &w.pc[1].d;
    dl_remote_sample(d, 0, 1, 0, 1000);   /* one stray sample saying down */
    dl_remote_sample(d, 0, 0, 0, 1050);
    dl_remote_sample(d, 0, 1, 0, 1100);
    dl_remote_sample(d, 0, 0, 0, 1150);
    CHECK(dl_state(d, 0) == DL_ALIVE);
    for (uint32_t t = 2000; t <= 2700; t += 50) dl_remote_sample(d, 0, 1, 0, t);
    CHECK(dl_state(d, 0) == DL_DOWN);
    dl_remote_sample(d, 0, 0, 0, 2750);
    CHECK(dl_state(d, 0) == DL_DOWN);   /* one sample is not enough to stand him up */
    dl_remote_sample(d, 0, 0, 1, 2800);   /* dead: believed at once */
    CHECK(dl_state(d, 0) == DL_DEAD && d->over);
    /* a dead mirror contradicted for long: a missed reload */
    dl_t e;
    dl_init(&e, &c, 1, 2, 0);
    dl_remote_event(&e, 0, DL_E_DEAD, 0, 10);
    CHECK(e.over);
    for (uint32_t t = 100; t <= 800; t += 50) dl_remote_sample(&e, 0, 0, 0, t);
    CHECK(dl_state(&e, 0) == DL_ALIVE);
}

static void t_partner_leaves(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    hit(&w, 0);
    steps(&w, 300);
    dl_set_present(&w.pc[0].d, 1, 0, w.now);   /* B leaves while A lies: A stands up (alone) */
    run_outputs(&w, &w.pc[0]);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_ALIVE && w.pc[0].got_up == 1);
    CHECK(!hit(&w, 0));   /* alone: a lethal hit is a death */
    /* the option off: A keeps lying (and bleeds out) */
    c.get_up_on_leave = 0;
    w_init(&w, 2, &c, 1000);
    hit(&w, 0);
    dl_set_present(&w.pc[0].d, 1, 0, w.now);
    run_outputs(&w, &w.pc[0]);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_DOWN);
    /* a leave while B holds: the hold is dropped */
    c.get_up_on_leave = 1;
    world_t v;
    w_init(&v, 3, &c, 1000);
    hit(&v, 0);
    steps(&v, 200);
    hold(&v, 1, 0, 800, 1);
    dl_set_present(&v.pc[1].d, 0, 0, v.now);
    CHECK(v.pc[1].d.h[1].hold_target == -1);
    /* a partner who joins again */
    dl_set_present(&v.pc[1].d, 0, 1, v.now);
    CHECK(dl_state(&v.pc[1].d, 0) == DL_ALIVE);
}

static void t_remote_bleed_prediction(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    w.cut = 0;
    hit(&w, 0);
    steps(&w, 200);
    w.cut = 1;   /* from now on B hears nothing: A's DEAD never arrives */
    w.pc[0].d.cfg.bleed_ms = 60000;
    steps(&w, 60500);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_DOWN);   /* grace period */
    steps(&w, 2500);
    CHECK(dl_state(&w.pc[1].d, 0) == DL_DEAD && w.pc[1].d.over && w.pc[1].die_calls == 1);
}

static void t_stalls_and_wrap(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 0xFFFFFF00u);   /* the clock wraps during the test */
    hit(&w, 0);
    int32_t b = dl_bleed_left(&w.pc[0].d, 0);
    steps(&w, 3000);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) <= b - 2900 && dl_bleed_left(&w.pc[0].d, 0) >= b - 3100);
    CHECK(w.now < 0xFFFFFF00u);   /* wrapped */
    hold(&w, 1, 0, 3300, 1);
    steps(&w, 300);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_ALIVE);
    /* a stall of the game thread (a load): ten seconds pass in one tick, the timer eats at most one second; the hold gets at most 250 ms of progress */
    hit(&w, 0);
    int32_t b2 = dl_bleed_left(&w.pc[0].d, 0);
    w.now += 10000;
    dl_tick(&w.pc[0].d, w.now);
    CHECK(dl_bleed_left(&w.pc[0].d, 0) >= b2 - 1000 && dl_bleed_left(&w.pc[0].d, 0) < b2);
    dl_local_hold(&w.pc[1].d, 0, 1, 1, w.now);
    w.now += 10000;
    dl_local_hold(&w.pc[1].d, 0, 1, 1, w.now);
    CHECK(w.pc[1].d.h[1].hold_ms <= 250);
}

static void t_absorb_repeated_hits(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    CHECK(hit(&w, 0));
    steps(&w, 5000);
    int32_t b = dl_bleed_left(&w.pc[0].d, 0);
    for (int i = 0; i < 20; i++) CHECK(hit(&w, 0));   /* enemies keep hitting the lying human: absorbed, no second DOWN, no timer change */
    CHECK(w.pc[0].d.n_down == 1 && dl_bleed_left(&w.pc[0].d, 0) == b && w.pc[0].hp == 1);
    steps(&w, 100);
    CHECK(w.pc[1].d.n_down == 1);
}

static void t_engine_driven_hold(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    hit(&w, 0);
    steps(&w, 300);
    dl_local_revive_begin(&w.pc[1].d, 0, w.now);   /* the engine's pl_act_rescue started on B */
    run_outputs(&w, &w.pc[1]);
    steps(&w, 100);
    CHECK(dl_being_revived(&w.pc[0].d, 0) && w.pc[0].revive_play == 1);
    for (int i = 0; i < 40; i++) {   /* 2 s of the engine's hold: keep-alives keep A's watchdog quiet */
        dl_local_revive_keep(&w.pc[1].d, w.now);
        run_outputs(&w, &w.pc[1]);
        step(&w);
    }
    CHECK(dl_being_revived(&w.pc[0].d, 0));
    dl_local_revive_cancel(&w.pc[1].d);   /* released */
    run_outputs(&w, &w.pc[1]);
    steps(&w, 200);
    CHECK(!dl_being_revived(&w.pc[0].d, 0) && w.pc[0].revive_stop == 1);
    dl_local_revive_begin(&w.pc[1].d, 0, w.now);
    dl_local_revive_done(&w.pc[1].d, w.now);   /* completed */
    run_outputs(&w, &w.pc[1]);
    steps(&w, 200);
    CHECK(dl_state(&w.pc[0].d, 0) == DL_ALIVE && w.pc[0].got_up == 1 && dl_state(&w.pc[1].d, 0) == DL_ALIVE);
    /* a begin on a human who is up does nothing */
    dl_local_revive_begin(&w.pc[1].d, 0, w.now);
    CHECK(w.pc[1].d.nq == 0 && w.pc[1].d.h[1].hold_target == -1);
}

static void t_external_up(void)
{
    world_t w;
    dl_cfg c = cfg_on();
    w_init(&w, 2, &c, 1000);
    hit(&w, 1);
    steps(&w, 200);
    dl_local_up(&w.pc[1].d, w.now);   /* the engine's own completion (or a herb) stood him up */
    run_outputs(&w, &w.pc[1]);
    steps(&w, 200);
    CHECK(dl_state(&w.pc[1].d, 1) == DL_ALIVE && dl_state(&w.pc[0].d, 1) == DL_ALIVE);
    dl_local_up(&w.pc[1].d, w.now);   /* up already: nothing */
    CHECK(w.pc[1].d.nq == 0);
}

static void t_event_edge_cases(void)
{
    dl_cfg c = cfg_on();
    dl_t d;
    dl_init(&d, &c, 0, 2, 100);
    dl_remote_event(&d, 1, DL_E_DOWN, 0, 200);
    dl_remote_event(&d, 1, DL_E_DOWN, 0, 210);   /* duplicate */
    CHECK(d.n_down == 1 && dl_state(&d, 1) == DL_DOWN);
    dl_remote_event(&d, 1, DL_E_CANCEL, 1, 220);   /* a cancel nobody started */
    CHECK(d.h[1].reviver == -1);
    dl_remote_event(&d, 1, DL_E_KEEPALIVE, 0, 230);   /* keep-alive for a human who is up (me): ignored */
    CHECK(d.h[0].reviver == -1);
    /* only the reviver can cancel */
    dl_t e;
    dl_init(&e, &c, 0, 3, 100);
    dl_local_lethal(&e, 150);
    dl_out o;
    while (dl_take(&e, &o)) {}
    dl_remote_event(&e, 1, DL_E_BEGIN, 0, 200);
    CHECK(e.h[0].reviver == 1);
    dl_remote_event(&e, 2, DL_E_CANCEL, 0, 210);   /* not the reviver */
    CHECK(e.h[0].reviver == 1);
    dl_remote_event(&e, 1, DL_E_CANCEL, 0, 220);
    CHECK(e.h[0].reviver == -1);
    /* a keep-alive without a begin counts as one (the begin was missed) and the downed side shows the pose */
    int plays = 0;
    while (dl_take(&e, &o)) {}
    dl_remote_event(&e, 2, DL_E_KEEPALIVE, 0, 300);
    while (dl_take(&e, &o)) plays += o.kind == DL_O_REVIVE_PLAY;
    CHECK(e.h[0].reviver == 2 && plays == 1);
    /* done for a target who is not down: ignored */
    dl_remote_event(&e, 1, DL_E_DONE, 1, 310);
    CHECK(dl_state(&e, 1) == DL_ALIVE);
    /* events from myself, unknown humans, bad kinds */
    dl_remote_event(&e, 0, DL_E_DEAD, 0, 320);
    dl_remote_event(&e, -1, DL_E_DEAD, 0, 320);
    dl_remote_event(&e, 9, DL_E_DEAD, 0, 320);
    dl_remote_event(&e, 1, 99, 0, 320);
    CHECK(dl_state(&e, 0) == DL_DOWN && dl_state(&e, 1) == DL_ALIVE);
    /* the rule off: events are ignored */
    c.enabled = 0;
    dl_t f;
    dl_init(&f, &c, 0, 2, 100);
    dl_remote_event(&f, 1, DL_E_DOWN, 0, 200);
    CHECK(dl_state(&f, 1) == DL_ALIVE);
    dl_tick(&f, 5000);
    CHECK(f.nq == 0);
}

static void t_queue_overflow_and_status(void)
{
    dl_cfg c = cfg_on();
    dl_t d;
    dl_init(&d, &c, 0, 2, 0);
    for (int i = 0; i < 100; i++) dl_push(&d, DL_O_SEND_DOWN, 0, 0);
    CHECK(d.nq == DL_QMAX && d.n_dropped == 100 - DL_QMAX);
    dl_out o;
    int n = 0;
    while (dl_take(&d, &o)) n++;
    CHECK(n == DL_QMAX && d.nq == 0);
    char buf[512], tiny[16];
    dl_local_lethal(&d, 10);
    int len = dl_status(&d, buf, sizeof buf);
    CHECK(len > 40 && strstr(buf, "ON") && strstr(buf, "DOWN") && strstr(buf, "me"));
    dl_status(&d, tiny, sizeof tiny);   /* a tiny buffer must not overflow or crash */
    CHECK(strlen(tiny) < sizeof tiny);
}

static void t_three_humans_generic(void)
{
    dl_cfg c = cfg_on();
    dl_t d;
    dl_init(&d, &c, 0, 3, 0);
    CHECK(dl_count_up(&d) == 3 && dl_count_present(&d) == 3);
    CHECK(dl_local_lethal(&d, 100));   /* 1 and 2 are up */
    dl_remote_event(&d, 1, DL_E_DOWN, 0, 200);
    CHECK(!d.over);   /* 2 is still up */
    dl_remote_event(&d, 2, DL_E_DOWN, 0, 300);
    CHECK(d.over);    /* nobody is up */
    CHECK(d.h[0].state == DL_DEAD);
}

/* ------------------------------------------------------------------ fuzz: random hits, holds, releases with latency and sample loss; invariants */
static void t_fuzz(void)
{
    unsigned total_over = 0, total_revive = 0, total_down = 0, total_bleed = 0;
    for (int seed = 1; seed <= 60; seed++) {
        world_t w;
        dl_cfg c = cfg_on();
        c.bleed_ms = 8000 + (uint32_t)(seed % 5) * 4000, c.revive_ms = 1500 + (uint32_t)(seed % 4) * 500;
        w_init(&w, 2, &c, 1000u * (unsigned)seed);
        w.rng ^= (unsigned)seed * 2654435761u;
        w.latency = 20 + (unsigned)(seed % 6) * 40, w.drop_state = seed % 3 * 20;
        int holder = -1, target = -1;
        int viol = 0;
        for (int t = 0; t < 4000 && !(w.pc[0].d.over && w.pc[1].d.over); t++) {
            unsigned r = rnd(&w) % 1000;
            if (r < 6) hit(&w, (int)(rnd(&w) & 1));
            if (holder < 0 && r > 960) {
                holder = (int)(rnd(&w) & 1), target = 1 - holder;
            } else if (holder >= 0 && r > 960) {
                release(&w, holder, target);
                holder = -1;
            }
            if (holder >= 0) {
                dl_local_hold(&w.pc[holder].d, target, 1, 1, w.now);
                run_outputs(&w, &w.pc[holder]);
            }
            step(&w);
            /* invariants */
            for (int i = 0; i < 2; i++) {
                pc_t *p = &w.pc[i];
                if (p->lying && dl_state(&p->d, i) != DL_DOWN && dl_state(&p->d, i) != DL_DEAD) viol++;   /* lying means DOWN (or just died) */
                if (p->hp > 1 && p->lying) viol++;
                if (p->d.over && dl_state(&p->d, i) == DL_ALIVE && p->hp > 0 && p->die_calls == 0) viol++;
                if (p->d.nq > DL_QMAX) viol++;
                if (p->die_calls > 1) viol++;
            }
        }
        CHECK(viol == 0);
        total_over += w.pc[0].d.over, total_revive += w.pc[0].d.n_revive + w.pc[1].d.n_revive, total_down += w.pc[0].d.n_down, total_bleed += w.pc[0].d.n_bleed + w.pc[1].d.n_bleed;
        if (w.pc[0].d.over) {   /* a game over on one PC arrives on the other one (events) within a second or two */
            steps(&w, 3000);
            CHECK(w.pc[1].d.over);
            CHECK(w.pc[0].hp == 0 && w.pc[1].hp == 0);
        }
    }
    CHECK(total_over > 10 && total_revive > 10 && total_down > 40 && total_bleed > 0);   /* the fuzz really exercised the paths */
    printf("fuzz: over %u, revives %u, downs %u, bleed-outs %u\n", total_over, total_revive, total_down, total_bleed);
}

int main(void)
{
    t_off_and_single();
    t_two_humans_down_and_bleed();
    t_presets_and_infinite();
    t_revive();
    t_revive_interrupted();
    t_hold_preconditions();
    t_watchdog();
    t_both_down();
    t_down_then_partner_lethal();
    t_reviver_goes_down();
    t_second_down_after_revive();
    t_retry_reset();
    t_desync_and_state();
    t_state_debounce();
    t_partner_leaves();
    t_remote_bleed_prediction();
    t_stalls_and_wrap();
    t_absorb_repeated_hits();
    t_engine_driven_hold();
    t_external_up();
    t_event_edge_cases();
    t_queue_overflow_and_status();
    t_three_humans_generic();
    t_fuzz();
    printf("test_downed: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
