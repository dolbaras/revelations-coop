/*
 * The world model over the real session code (world1_net.c + coop_session.c) on the in-process "sim" transport with a virtual clock and a lossy line:
 * the envelopes (CP_EVENT kind CP_EV_W1, CP_WORLD kinds), the host's relay, the epoch of the level instance and the load barrier.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/world1_net.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t vclock = 1000;

typedef struct {
    cs_t *s;
    w1n_t n;
    uint32_t offset;
    float hp[200];                 /* mock engine: hp by key index (block 11) */
    int alive[200], spawned[200];
    int kills_own, kills_other, kill_calls;
    int pickups;
    double hp_lost;
} Node;

static uint32_t clk(void *u) { return vclock + ((Node *)u)->offset; }
static void on_note(void *u, const cs_note *n) { w1n_note(&((Node *)u)->n, n); }

static void cb_spawn(void *u, const w1_enemy_rec *r) { Node *n = u; n->spawned[r->key & 0xFF] = 1, n->alive[r->key & 0xFF] = 1, n->hp[r->key & 0xFF] = r->hp; }
static void cb_kill(void *u, w1_key key, int killer, int own, uint32_t fl, const float pt[3])
{
    Node *n = u;
    (void)fl, (void)pt;
    n->kill_calls++, n->alive[key & 0xFF] = 0;
    if (own) n->kills_own++; else if (killer >= 0) n->kills_other++;
}
static void cb_hit(void *u, w1_key key, int slot, float dmg, uint32_t fl, const float pt[3])
{
    Node *n = u;
    (void)slot;
    int i = key & 0xFF;
    if (!n->alive[i]) return;
    n->hp_lost += dmg, n->hp[i] -= dmg;
    if (n->hp[i] <= 0) n->alive[i] = 0, w1_host_death(&n->n.w, key, W1_NO_SLOT, fl, pt);
}
static void cb_item(void *u, const w1_key4 *k, uint32_t cls, int taker) { (void)k, (void)cls, (void)taker; ((Node *)u)->pickups++; }

static void node_init(Node *nd, int role, int slot, int want, uint32_t offset)
{
    memset(nd, 0, sizeof *nd);
    nd->offset = offset;
    cs_config c;
    memset(&c, 0, sizeof c);
    c.name = role == W1_HOST ? "Host" : "Client", c.want_char = want, c.mod_version = "test", c.is_bot = 1, c.clock = clk, c.clock_user = nd, c.on_note = on_note, c.user = nd;
    nd->s = cs_new(&c);
    w1_ops o;
    memset(&o, 0, sizeof o);
    o.user = nd, o.spawn = cb_spawn, o.kill = cb_kill, o.apply_hit = cb_hit, o.item_gone = cb_item;
    w1n_init(&nd->n, nd->s, role, slot, &o);
}

static void faults(int loss, int dup, int reorder, int delay, int jitter, uint32_t seed)
{
    cn_sim_params p;
    memset(&p, 0, sizeof p);
    p.loss_pct = loss, p.dup_pct = dup, p.reorder_pct = reorder, p.delay_ms = delay, p.jitter_ms = jitter, p.reorder_extra_ms = 60, p.seed = seed;
    cn_sim_set(&p);
}

static void step(Node *h, Node *c, int ms)
{
    for (int t = 0; t < ms; t += 5) {
        vclock += 5;
        cn_sim_set_time(vclock);
        cs_poll(h->s), cs_poll(c->s);
        if ((t % 15) == 0) w1n_frame(&h->n), w1n_frame(&c->n);
    }
}

static void add_enemy(Node *h, int idx, float hp)
{
    float pos[3] = { (float)idx, 0, 0 };
    h->alive[idx] = 1, h->hp[idx] = hp;
    w1_host_spawn(&h->n.w, W1_KEY(11, idx), 0x80031000, 0, 0, pos, hp);
}

static void scenario(int loss, int dup, int reorder, int delay, int jitter, uint32_t seed)
{
    Node h, c;
    node_init(&h, W1_HOST, 0, CP_CHAR_LEAD, 0);
    node_init(&c, W1_CLIENT, 1, CP_CHAR_NONE, 777777);
    cn_sim_set_time(vclock);
    faults(0, 0, 0, 0, 0, seed);
    char err[200];
    CHECK(cs_host(h.s, "sim", 31 + (int)seed, 0, err, sizeof err));
    char target[32];
    snprintf(target, sizeof target, "sim:%d", 31 + (int)seed);
    CHECK(cs_join(c.s, "sim", target, 0, err, sizeof err));
    step(&h, &c, 100);
    cs_ready(c.s, 1);
    step(&h, &c, 100);
    err[0] = 0;
    CHECK(cs_start(h.s, 0, err, sizeof err) || printf("start refused: %s\n", err));
    step(&h, &c, 100);
    cs_load_done(h.s), cs_load_done(c.s);
    step(&h, &c, 100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    faults(loss, dup, reorder, delay, jitter, seed);
    unsigned ep1 = cs_epoch(h.s);

    for (int i = 0; i < 12; i++) add_enemy(&h, i, i < 6 ? 1000.0f : 50.0f);
    step(&h, &c, 3000);
    int got = 0;
    for (int i = 0; i < 12; i++) got += c.spawned[i];
    CHECK(got == 12);
    CHECK(w1_epoch(&h.n.w) == ep1 && w1_epoch(&c.n.w) == ep1);

    float pt[3] = { 0, 0, 0 };
    for (int r = 0; r < 20; r++) {
        for (int i = 0; i < 6; i++) w1_client_hit(&c.n.w, W1_KEY(11, i), 5.0f, 1, pt);
        step(&h, &c, 60);
    }
    for (int i = 6; i < 9; i++) w1_client_hit(&c.n.w, W1_KEY(11, i), 80.0f, 0x80, pt);        /* client kills 6, 7, 8 */
    for (int i = 9; i < 12; i++) {                                                            /* host kills 9, 10, 11 */
        w1_host_hit_note(&h.n.w, W1_KEY(11, i), 0);
        h.alive[i] = 0;
        w1_host_death(&h.n.w, W1_KEY(11, i), W1_NO_SLOT, 1, pt);
    }
    w1_key4 k = { { 1, 2, 3, 4 } };
    w1_local_pickup(&c.n.w, &k, 0x80040100);
    step(&h, &c, 6000);
    CHECK(fabs(h.hp_lost - (20.0 * 6 * 5.0 + 3 * 80.0)) < 1e-6);   /* every stroke exactly once: 120 x 5 on the tough ones + 3 killing strokes of 80 */
    CHECK(h.n.w.st.hits_applied == 20u * 6u + 3u);
    CHECK(c.kills_own == 3 && c.kills_other == 3 && c.kill_calls == 6);
    CHECK(w1_kills(&h.n.w, 0) == 3 && w1_kills(&h.n.w, 1) == 3 && w1_kills(&c.n.w, 0) == 3 && w1_kills(&c.n.w, 1) == 3);
    CHECK(h.pickups == 1 && c.pickups == 0);
    CHECK(w1_pending(&h.n.w) == 0 && w1_pending(&c.n.w) == 0);

    /* the client starts the next level: epoch + 1. Late packets of the old level never reach the new one. */
    float pos[3] = { 1, 1, 1 };
    w1_host_hit_note(&h.n.w, W1_KEY(11, 0), 0);
    w1_host_death(&h.n.w, W1_KEY(11, 0), 0, 1, pos);   /* an event of the old level, in flight when the load starts */
    cs_load_begin(c.s, 0x91010200);
    step(&h, &c, 80);
    cs_load_begin(h.s, 0x91010200);
    cs_load_done(c.s), cs_load_done(h.s);
    step(&h, &c, 300);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_epoch(h.s) == ep1 + 1);
    step(&h, &c, 100);
    CHECK(w1_epoch(&h.n.w) == ep1 + 1 && w1_epoch(&c.n.w) == ep1 + 1);
    CHECK(w1_count(&h.n.w, W1_ALIVE) == 0 && w1_count(&c.n.w, W1_ALIVE) == 0 && w1_kills(&c.n.w, 0) == 0 && w1_kills(&c.n.w, 1) == 0);
    add_enemy(&h, 50, 40.0f);
    step(&h, &c, 3000);
    CHECK(c.spawned[50] && w1_enemy_state(&c.n.w, W1_KEY(11, 50)) == W1_ALIVE && w1_enemy_state(&c.n.w, W1_KEY(11, 0)) == W1_UNKNOWN);
    cs_free(h.s), cs_free(c.s);
}

int main(void)
{
    printf("-- world over the session: clean line\n");
    scenario(0, 0, 0, 5, 0, 1);
    printf("-- 20 %% loss, 10 %% duplicates, 10 %% reordering, 40 ms +- 20\n");
    scenario(20, 10, 10, 40, 20, 2);
    scenario(25, 15, 10, 30, 30, 3);
    printf("world1_net: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
