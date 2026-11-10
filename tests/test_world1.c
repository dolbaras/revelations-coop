/*
 * Tests of the Revelations 1 world model (native/world1.c): host-authoritative enemies keyed by (block, index), HIT / DEATH / DESPAWN, kill credit by slot,
 * PICKUP / DOOR / FLAG events, epochs. Two models (host and client) are connected by a simulated line with loss, duplicates, reordering and a virtual clock;
 * a small mock "engine" on each side stands for the game (replicas, hp, bags). Deterministic (seeded).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/coop_proto.h"
#include "../native/world1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---------------------------------------------------------------- simulated line */
typedef struct { int lane; unsigned kind; uint8_t body[CP_WORLD_MAX]; unsigned len; uint32_t due; int used; } Pkt;
typedef struct {
    Pkt q[1024];
    unsigned rng;
    int loss, dup, jitter, base;          /* percent, percent, ms, ms */
    int black_events;                      /* drop the whole event lane */
    int (*drop)(int lane, unsigned kind, const uint8_t *b, unsigned len);
    uint32_t sent, dropped;
} Link;

static unsigned rnd(unsigned *s) { unsigned x = *s ? *s : 2463534242u; x ^= x << 13, x ^= x >> 17, x ^= x << 5; return *s = x; }

static uint32_t g_now;

static void link_put(Link *l, int lane, unsigned kind, const uint8_t *b, unsigned len)
{
    l->sent++;
    if (l->black_events && lane == W1_LANE_EVENT) { l->dropped++; return; }
    if (l->drop && l->drop(lane, kind, b, len)) { l->dropped++; return; }
    if ((int)(rnd(&l->rng) % 100) < l->loss) { l->dropped++; return; }
    int copies = (int)(rnd(&l->rng) % 100) < l->dup ? 2 : 1;
    for (int c = 0; c < copies; c++) {
        for (int i = 0; i < 1024; i++)
            if (!l->q[i].used) {
                Pkt *p = &l->q[i];
                p->used = 1, p->lane = lane, p->kind = kind, p->len = len, memcpy(p->body, b, len);
                p->due = g_now + (uint32_t)l->base + (l->jitter ? rnd(&l->rng) % (unsigned)(l->jitter + 1) : 0);
                break;
            }
    }
}

/* ---------------------------------------------------------------- mock engines */
typedef struct { w1_key key; float hp; int alive; float pos[3]; uint32_t cls; int has_rec; } MEnemy;
typedef struct {
    w1_t w;
    Link *tx;
    MEnemy en[160];
    int nen;
    double hp_lost;                       /* host: damage the "engine" took from the client's hits */
    double sent_dmg;                      /* client: damage handed to w1_client_hit that was announced */
    int kills_own, kills_other, kills_unknown;
    int spawn_calls, despawn_calls, kill_calls;
    int items_gone, taker_last;
    int door_calls, flag_calls;
    uint32_t door_vtbl, flag_id; unsigned door_phase; int32_t flag_val;
    int update_calls;
    w1_key last_hit_key;
    int pchits, pchit_seen[64], pchit_bad;
    w1_key pchit_att;
} Node;

static MEnemy *m_find(Node *n, w1_key k)
{
    for (int i = 0; i < n->nen; i++) if (n->en[i].key == k) return &n->en[i];
    return NULL;
}

static void cb_send(void *u, int lane, unsigned kind, const uint8_t *b, unsigned len) { link_put(((Node *)u)->tx, lane, kind, b, len); }
static void cb_spawn(void *u, const w1_enemy_rec *r)
{
    Node *n = u;
    n->spawn_calls++;
    MEnemy *e = &n->en[n->nen++];
    memset(e, 0, sizeof *e);
    e->key = r->key, e->hp = r->hp, e->alive = 1, e->cls = r->cls;
    memcpy(e->pos, r->pos, sizeof e->pos);
}
static void cb_update(void *u, const w1_enemy_rec *r)
{
    Node *n = u;
    n->update_calls++;
    MEnemy *e = m_find(n, r->key);
    if (e) memcpy(e->pos, r->pos, sizeof e->pos), e->hp = r->hp, e->has_rec = 1;
}
static void cb_kill(void *u, w1_key key, int killer, int credit_own, uint32_t fl, const float pt[3])
{
    Node *n = u;
    (void)fl, (void)pt;
    n->kill_calls++;
    MEnemy *e = m_find(n, key);
    if (e) e->alive = 0, e->hp = 0;
    if (credit_own) n->kills_own++; else if (killer >= 0) n->kills_other++; else n->kills_unknown++;
}
static void cb_despawn(void *u, w1_key key)
{
    Node *n = u;
    n->despawn_calls++;
    MEnemy *e = m_find(n, key);
    if (e) e->alive = 0, e->pos[1] = -99999.0f;
}
static void cb_apply_hit(void *u, w1_key key, int slot, float dmg, uint32_t fl, const float pt[3])
{
    Node *n = u;
    (void)slot, (void)fl, (void)pt;
    MEnemy *e = m_find(n, key);
    if (!e || !e->alive) return;
    n->last_hit_key = key;
    n->hp_lost += dmg;
    e->hp -= dmg;
    if (e->hp <= 0) {   /* the engine kills inside OnHit: no hint, the model credits the slot of the hit being applied */
        e->alive = 0;
        w1_host_death(&n->w, key, W1_NO_SLOT, 0x80, pt);
    }
}
static void cb_item(void *u, const w1_key4 *k, uint32_t cls, int taker) { Node *n = u; (void)k, (void)cls; n->items_gone++, n->taker_last = taker; }
static void cb_door(void *u, const w1_key4 *k, uint32_t vtbl, uint8_t ph) { Node *n = u; (void)k; n->door_calls++, n->door_vtbl = vtbl, n->door_phase = ph; }
static void cb_flag(void *u, uint32_t id, int32_t v) { Node *n = u; n->flag_calls++, n->flag_id = id, n->flag_val = v; }
static void cb_pchit(void *u, w1_key att, const uint8_t *p, unsigned len)
{
    Node *n = u;
    n->pchits++, n->pchit_att = att;
    int ok = len == W1_PCHIT_MAX && p[0] < 64;
    for (unsigned i = 1; ok && i < len; i++) ok = p[i] == (uint8_t)(p[0] * 7 + i);   /* the payload arrives byte for byte */
    if (!ok) n->pchit_bad++; else n->pchit_seen[p[0]]++;
}

static void node_init(Node *n, int role, int slot, Link *tx)
{
    memset(n, 0, sizeof *n);
    n->tx = tx;
    w1_ops o;
    memset(&o, 0, sizeof o);
    o.user = n, o.send = cb_send, o.spawn = cb_spawn, o.update = cb_update, o.kill = cb_kill, o.despawn = cb_despawn, o.apply_hit = cb_apply_hit, o.item_gone = cb_item, o.door_set = cb_door, o.flag_set = cb_flag;
    o.pc_hit = cb_pchit;
    w1_init(&n->w, role, slot, &o);
}

typedef struct { Node h, c; Link hc, ch; } Sim;

static void sim_init(Sim *s, unsigned seed, int loss, int dup, int jitter, int base)
{
    memset(&s->hc, 0, sizeof s->hc), memset(&s->ch, 0, sizeof s->ch);
    s->hc.rng = seed * 2654435761u + 1, s->ch.rng = seed * 40503u + 7;
    s->hc.loss = s->ch.loss = loss, s->hc.dup = s->ch.dup = dup, s->hc.jitter = s->ch.jitter = jitter, s->hc.base = s->ch.base = base;
    g_now = 1000;
    node_init(&s->h, W1_HOST, 0, &s->hc);
    node_init(&s->c, W1_CLIENT, 1, &s->ch);
    w1_tick(&s->h.w, g_now), w1_tick(&s->c.w, g_now);
    w1_set_epoch(&s->h.w, 5), w1_set_epoch(&s->c.w, 5);
}

static void pump(Link *l, Node *dst)
{
    for (;;) {
        int best = -1;
        for (int i = 0; i < 1024; i++)
            if (l->q[i].used && (int32_t)(l->q[i].due - g_now) <= 0 && (best < 0 || (int32_t)(l->q[i].due - l->q[best].due) < 0)) best = i;
        if (best < 0) break;
        Pkt p = l->q[best];
        l->q[best].used = 0;
        w1_recv(&dst->w, p.lane, p.kind, p.body, p.len);
    }
}

static void sim_step(Sim *s, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        g_now += 10;
        pump(&s->hc, &s->c), pump(&s->ch, &s->h);
        w1_tick(&s->h.w, g_now), w1_tick(&s->c.w, g_now);
    }
}

/* the host engine moves its live enemies and reports a sample */
static void host_move(Sim *s)
{
    for (int i = 0; i < s->h.nen; i++) {
        MEnemy *e = &s->h.en[i];
        if (!e->alive) continue;
        e->pos[0] += 7.0f, e->pos[2] -= 3.0f;
        w1_host_sample(&s->h.w, e->key, e->pos, 0.5f + 0.01f * (float)i, e->hp, 3, 0x40001088u);
    }
}

static MEnemy *host_add(Sim *s, int block, int index, uint32_t cls, float hp)
{
    MEnemy *e = &s->h.en[s->h.nen++];
    memset(e, 0, sizeof *e);
    e->key = W1_KEY(block, index), e->hp = hp, e->alive = 1, e->cls = cls;
    e->pos[0] = (float)(index * 100), e->pos[1] = -600, e->pos[2] = (float)(block * 50);
    w1_host_spawn(&s->h.w, e->key, cls, 0, 0, e->pos, hp);
    return e;
}

static void run_move(Sim *s, int ms)
{
    for (int t = 0; t < ms; t += 50) {
        host_move(s);
        sim_step(s, 50);
    }
}

/* ---------------------------------------------------------------- 1. codec and envelope sizes */
static void t_envelope(void)
{
    /* the model's messages travel in the existing envelopes and fit them (cp_encode / cp_decode round trip) */
    Sim s;
    sim_init(&s, 1, 0, 0, 0, 0);
    int seen_ev = 0, seen_batch = 0, seen_roster = 0, seen_ack = 0;
    for (int i = 0; i < 40; i++) host_add(&s, 3, i, 0x80031000, 100);
    float pos[3] = { 1, 2, 3 };
    w1_host_death(&s.h.w, W1_KEY(3, 0), 1, 0x80, pos);
    w1_host_despawn(&s.h.w, W1_KEY(3, 1));
    w1_key4 k4 = { { 1, 2, 3, 4 } };
    w1_local_pickup(&s.h.w, &k4, 0x80040100);
    w1_local_door(&s.h.w, &k4, 0x1032100, 2);
    w1_local_flag(&s.h.w, 4704, 1);
    w1_local_flag(&s.c.w, 4705, 1);
    for (int step = 0; step < 100; step++) {
        host_move(&s);
        sim_step(&s, 50);
    }
    /* now capture the wire */
    for (int d = 0; d < 2; d++) {
        Link *l = d ? &s.ch : &s.hc;
        l->base = 5000;   /* hold everything in flight so that the queue can be read */
    }
    host_add(&s, 4, 0, 0x80031000, 50);
    w1_local_flag(&s.h.w, 4900, 1);
    w1_local_flag(&s.c.w, 4901, 1);
    host_move(&s);
    sim_step(&s, 700);
    for (int d = 0; d < 2; d++) {
        Link *l = d ? &s.ch : &s.hc;
        for (int i = 0; i < 1024; i++) {
            Pkt *p = &l->q[i];
            if (!p->used) continue;
            cp_msg m;
            memset(&m, 0, sizeof m);
            uint8_t buf[CP_MAX_PACKET];
            if (p->lane == W1_LANE_EVENT) {
                CHECK(p->kind == CP_EV_W1 && p->len <= CP_EVENT_MAX && p->len >= 4);
                m.type = CP_EVENT, m.u.event.pid = 0, m.u.event.kind = (uint16_t)p->kind, m.u.event.eseq = 1, m.u.event.epoch = 5, m.u.event.len = (uint8_t)p->len;
                memcpy(m.u.event.body, p->body, p->len);
                seen_ev++;
            } else {
                CHECK(p->kind == CP_WK_W1_ENEMIES || p->kind == CP_WK_W1_ROSTER || p->kind == CP_WK_W1_ACK);
                CHECK(p->len <= CP_WORLD_MAX);
                m.type = CP_WORLD, m.u.world.pid = 0, m.u.world.kind = (uint8_t)p->kind, m.u.world.seq = 1, m.u.world.epoch = 5, m.u.world.len = (uint16_t)p->len;
                memcpy(m.u.world.body, p->body, p->len);
                seen_batch += p->kind == CP_WK_W1_ENEMIES, seen_roster += p->kind == CP_WK_W1_ROSTER, seen_ack += p->kind == CP_WK_W1_ACK;
            }
            size_t n = cp_encode(&m, buf, sizeof buf);
            cp_msg back;
            CHECK(n > 0 && cp_decode(buf, n, &back) && back.type == m.type);
            if (p->lane == W1_LANE_EVENT) CHECK(back.u.event.len == p->len && !memcmp(back.u.event.body, p->body, p->len));
            else CHECK(back.u.world.len == p->len && !memcmp(back.u.world.body, p->body, p->len));
        }
    }
    CHECK(seen_ev > 0 && seen_batch > 0 && seen_roster > 0 && seen_ack > 0);
    /* the kind numbers stay clear of the top-level types taken by the other branches */
    CHECK(CP_WORLD == 0x51 && CP_EV_W1 == 0x140 && CP_WK_W1_ENEMIES != 1);
    CHECK(W1_BATCH_RECS * 26 + 11 <= CP_WORLD_MAX);
}

/* ---------------------------------------------------------------- 2. spawn and state replication on a clean line */
static void t_replicate(void)
{
    Sim s;
    sim_init(&s, 2, 0, 0, 0, 20);
    for (int b = 0; b < 3; b++)
        for (int i = 0; i < 6; i++) host_add(&s, 10 + b, i, 0x80031000u + (unsigned)b, 80.0f + (float)i);
    run_move(&s, 3000);
    CHECK(w1_count(&s.h.w, W1_ALIVE) == 18 && w1_count(&s.c.w, W1_ALIVE) == 18);
    CHECK(s.c.spawn_calls == 18 && s.c.nen == 18);
    int ok = 1;
    for (int i = 0; i < s.h.nen; i++) {
        MEnemy *h = &s.h.en[i], *c = m_find(&s.c, h->key);
        if (!c || c->cls != h->cls || fabsf(c->pos[0] - h->pos[0]) > 30.0f || fabsf(c->pos[2] - h->pos[2]) > 15.0f) ok = 0;
        w1_enemy_rec r;
        if (!w1_enemy_get(&s.c.w, h->key, &r, NULL) || r.cls != h->cls || r.owner != 0) ok = 0;
    }
    CHECK(ok);
    CHECK(w1_pending(&s.h.w) == 0 && w1_pending(&s.c.w) == 0);   /* everything acknowledged */
    CHECK(s.c.w.st.rec_applied > 100 && s.h.w.st.retx == 0);
    /* the key is (block, index): two different blocks with the same index are two enemies */
    CHECK(w1_enemy_state(&s.c.w, W1_KEY(10, 0)) == W1_ALIVE && w1_enemy_state(&s.c.w, W1_KEY(11, 0)) == W1_ALIVE && w1_enemy_state(&s.c.w, W1_KEY(12, 9)) == W1_UNKNOWN);
    /* a second spawn of the same key is ignored; the client side cannot spawn */
    float p[3] = { 0, 0, 0 };
    CHECK(w1_host_spawn(&s.h.w, W1_KEY(10, 0), 0x80031000, 0, 0, p, 1) == 0);
    CHECK(w1_host_spawn(&s.c.w, W1_KEY(40, 0), 0x80031000, 0, 0, p, 1) == 0);
}

/* ---------------------------------------------------------------- 3. hits, death and kill credit */
static void t_credit(void)
{
    Sim s;
    sim_init(&s, 3, 0, 0, 0, 20);
    MEnemy *a = host_add(&s, 11, 0, 0x80031000, 100);   /* killed by the client's human */
    MEnemy *b = host_add(&s, 11, 1, 0x80031000, 100);   /* killed by the host's human */
    MEnemy *c = host_add(&s, 11, 2, 0x80031000, 100);   /* both hit it, the client lands the last blow */
    MEnemy *d = host_add(&s, 11, 3, 0x80031000, 100);   /* untouched */
    run_move(&s, 1000);
    CHECK(s.c.nen == 4);
    /* client hits A in three strokes */
    float pt[3] = { 5, 6, 7 };
    float allowed = 0;
    allowed += w1_client_hit(&s.c.w, a->key, 40, 0x01, pt);
    allowed += w1_client_hit(&s.c.w, a->key, 40, 0x01, pt);
    CHECK(fabsf(allowed - 80.0f) < 0.01f);
    run_move(&s, 500);
    CHECK(a->alive && fabs(s.h.hp_lost - 80.0) < 0.01);
    /* the third stroke would kill: the replica only goes down to 1 hp locally, the host decides */
    float al3 = w1_client_hit(&s.c.w, a->key, 40, 0x80, pt);
    CHECK(al3 <= 19.0f + 0.01f);
    MEnemy *ca = m_find(&s.c, a->key);
    CHECK(ca->hp >= 0.99f);
    run_move(&s, 800);
    CHECK(!a->alive && w1_enemy_state(&s.h.w, a->key) == W1_DEAD && w1_enemy_state(&s.c.w, a->key) == W1_DEAD);
    int k = -1;
    w1_enemy_get(&s.h.w, a->key, NULL, &k);
    CHECK(k == 1);
    w1_enemy_get(&s.c.w, a->key, NULL, &k);
    CHECK(k == 1);
    CHECK(s.c.kills_own == 1 && s.c.kills_other == 0 && s.c.kill_calls == 1);
    /* the host's human kills B: the client gets a kill without own credit */
    w1_host_hit_note(&s.h.w, b->key, 0);
    b->hp = 0, b->alive = 0;
    CHECK(w1_host_death(&s.h.w, b->key, W1_NO_SLOT, 0x01, pt) == 0);   /* the last damager is slot 0 */
    CHECK(w1_host_death(&s.h.w, b->key, W1_NO_SLOT, 0x01, pt) == 0);   /* repeat: idempotent */
    run_move(&s, 800);
    CHECK(s.c.kills_own == 1 && s.c.kills_other == 1 && s.c.kill_calls == 2);
    /* shared kill: the host hits first, the client's hit finishes it */
    w1_host_hit_note(&s.h.w, c->key, 0);
    c->hp = 10;
    w1_client_hit(&s.c.w, c->key, 30, 0x01, pt);
    run_move(&s, 800);
    CHECK(w1_enemy_state(&s.c.w, c->key) == W1_DEAD);
    w1_enemy_get(&s.h.w, c->key, NULL, &k);
    CHECK(k == 1);
    /* counters agree on both PCs and add up per slot */
    CHECK(w1_kills(&s.h.w, 0) == 1 && w1_kills(&s.h.w, 1) == 2 && w1_kills(&s.c.w, 0) == 1 && w1_kills(&s.c.w, 1) == 2);
    CHECK(s.c.kills_own == 2 && s.c.kills_other == 1);
    /* a hit on a dead enemy and a hit on an unknown key are not announced / are counted as late */
    CHECK(w1_client_hit(&s.c.w, a->key, 5, 1, pt) == 0 && w1_client_hit(&s.c.w, W1_KEY(99, 9), 5, 1, pt) == 0);
    CHECK(w1_enemy_state(&s.c.w, d->key) == W1_ALIVE && d->alive);
    /* despawn removes a live enemy and a corpse */
    CHECK(w1_host_despawn(&s.h.w, d->key) == 1 && w1_host_despawn(&s.h.w, a->key) == 1 && w1_host_despawn(&s.h.w, d->key) == 0);
    run_move(&s, 800);
    CHECK(w1_enemy_state(&s.c.w, d->key) == W1_GONE && w1_enemy_state(&s.c.w, a->key) == W1_GONE && s.c.despawn_calls == 2);
}

/* the client's HIT reaches the host after the host killed the enemy */
static void t_late_hit(void)
{
    Sim s;
    sim_init(&s, 4, 0, 0, 0, 100);   /* slow line: 100 ms */
    MEnemy *a = host_add(&s, 12, 0, 0x80031310, 50);
    run_move(&s, 1000);
    float pt[3] = { 0, 0, 0 };
    w1_client_hit(&s.c.w, a->key, 10, 1, pt);
    a->alive = 0;
    w1_host_hit_note(&s.h.w, a->key, 0);
    w1_host_death(&s.h.w, a->key, 0, 1, pt);   /* killed by the host's human a moment earlier */
    run_move(&s, 1000);
    CHECK(s.h.w.st.late_hits == 1 && s.h.w.st.hits_applied == 0 && s.h.hp_lost == 0);
    CHECK(w1_kills(&s.h.w, 0) == 1 && w1_kills(&s.c.w, 0) == 1 && w1_kills(&s.c.w, 1) == 0 && s.c.kills_own == 0 && s.c.kills_other == 1);
}

/* ---------------------------------------------------------------- 4. order independence (the absorbing states) */
typedef struct { uint8_t b[96]; unsigned len; int lane; unsigned kind; } Tap;
static Tap g_tap[64];
static int g_ntap;
static void tap_send(void *u, int lane, unsigned kind, const uint8_t *b, unsigned len)
{
    (void)u;
    if (g_ntap < 64) memcpy(g_tap[g_ntap].b, b, len), g_tap[g_ntap].len = len, g_tap[g_ntap].lane = lane, g_tap[g_ntap].kind = kind, g_ntap++;
}

static void t_order(void)
{
    /* host with a tap instead of a line: SPAWN k, DEATH k, SPAWN j, DESPAWN j */
    w1_t h;
    w1_ops o;
    memset(&o, 0, sizeof o);
    o.send = tap_send;
    w1_init(&h, W1_HOST, 0, &o);
    w1_set_epoch(&h, 9);
    g_ntap = 0;
    float p[3] = { 1, 2, 3 };
    w1_host_spawn(&h, W1_KEY(20, 1), 0x80031000, 0, 0, p, 10);
    w1_host_death(&h, W1_KEY(20, 1), 1, 0x80, p);
    w1_host_spawn(&h, W1_KEY(20, 2), 0x80031000, 0, 0, p, 10);
    w1_host_despawn(&h, W1_KEY(20, 2));
    CHECK(g_ntap == 4);
    Tap ev[4];
    memcpy(ev, g_tap, sizeof ev);
    /* every permutation of the four events (24) reaches the same end state on a fresh client */
    int perm[24][4], np = 0;
    for (int a = 0; a < 4; a++)
        for (int b = 0; b < 4; b++)
            for (int c = 0; c < 4; c++)
                for (int d = 0; d < 4; d++) {
                    if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
                    perm[np][0] = a, perm[np][1] = b, perm[np][2] = c, perm[np][3] = d, np++;
                }
    CHECK(np == 24);
    for (int pi = 0; pi < np; pi++) {
        Node c;
        Link dummy;
        memset(&dummy, 0, sizeof dummy);
        node_init(&c, W1_CLIENT, 1, &dummy);
        w1_set_epoch(&c.w, 9);
        for (int i = 0; i < 4; i++) {
            /* twice each: duplicates */
            CHECK(w1_recv(&c.w, W1_LANE_EVENT, CP_EV_W1, ev[perm[pi][i]].b, ev[perm[pi][i]].len) == 1);
            w1_recv(&c.w, W1_LANE_EVENT, CP_EV_W1, ev[perm[pi][i]].b, ev[perm[pi][i]].len);
        }
        CHECK(w1_enemy_state(&c.w, W1_KEY(20, 1)) == W1_DEAD && w1_enemy_state(&c.w, W1_KEY(20, 2)) == W1_GONE);
        CHECK(w1_kills(&c.w, 1) == 1 && w1_kills(&c.w, 0) == 0);
        CHECK(c.w.st.dup >= 4);
        CHECK(w1_count(&c.w, W1_ALIVE) == 0);
    }
}

/* ---------------------------------------------------------------- 5. loss, duplicates, reordering: everything converges, exactly once */
typedef struct { int loss, dup, jitter, base; } Prof;

static void run_chaos(unsigned seed, Prof pr, int verbose)
{
    Sim s;
    sim_init(&s, seed, pr.loss, pr.dup, pr.jitter, pr.base);
    enum { N = 40 };
    MEnemy *e[N];
    for (int i = 0; i < N; i++) e[i] = host_add(&s, 11 + i / 14, i % 14, i % 7 == 0 ? 0x80031310u : 0x80031000u, i % 5 == 0 ? 1000.0f : 60.0f);
    run_move(&s, 4000);
    float pt[3] = { 1, 1, 1 };
    double sent = 0;
    int client_kills = 0, host_kills = 0;
    /* the client hits the tough ones (hp 1000: never die) many times and kills some weak ones */
    for (int round = 0; round < 30; round++) {
        for (int i = 0; i < N; i += 5) {
            float a = w1_client_hit(&s.c.w, e[i]->key, 7.0f, 0x01, pt);
            (void)a;
            sent += 7.0;
        }
        run_move(&s, 100);
    }
    for (int i = 1; i < N; i += 4) {   /* weak enemies 1, 5, 9 ... killed by the client's human */
        if (i % 5 == 0) continue;
        w1_client_hit(&s.c.w, e[i]->key, 100.0f, 0x01, pt);
        client_kills++;
        run_move(&s, 50);
    }
    for (int i = 2; i < N; i += 4) {   /* 2, 6, 10 ... killed by the host's human */
        if (i % 5 == 0) continue;
        w1_host_hit_note(&s.h.w, e[i]->key, 0);
        e[i]->hp = 0, e[i]->alive = 0;
        w1_host_death(&s.h.w, e[i]->key, W1_NO_SLOT, 0x01, pt);
        host_kills++;
        run_move(&s, 50);
    }
    for (int i = 3; i < N; i += 8) {   /* a few are removed by the engine */
        if (i % 5 == 0) continue;
        w1_host_despawn(&s.h.w, e[i]->key);
        e[i]->alive = 0;
    }
    /* doors, flags, pickups from both sides, some at the same time */
    w1_key4 dk = { { 7, 8, 9, 1 } }, pk1 = { { 1, 1, 1, 1 } }, pk2 = { { 2, 2, 2, 2 } };
    w1_local_door(&s.h.w, &dk, 0x1032100, 1);
    w1_local_door(&s.c.w, &dk, 0x1032100, 2);   /* concurrent: the host's wins */
    w1_local_flag(&s.h.w, 4704, 1);
    w1_local_flag(&s.c.w, 4704, 7);
    w1_local_flag(&s.c.w, 4710, 1);
    w1_local_pickup(&s.h.w, &pk1, 0x80040100);
    w1_local_pickup(&s.c.w, &pk1, 0x80040100);   /* the same item taken on both PCs */
    w1_local_pickup(&s.c.w, &pk2, 0x80040200);
    run_move(&s, 12000);
    /* convergence */
    int diff = 0;
    for (int i = 0; i < N; i++) {
        int sh = w1_enemy_state(&s.h.w, e[i]->key), sc = w1_enemy_state(&s.c.w, e[i]->key);
        if (sh != sc) diff++;
    }
    if (verbose || diff) printf("  seed %u loss %d: diff %d  %s\n", seed, pr.loss, diff, "");
    CHECK(diff == 0);
    CHECK(w1_pending(&s.h.w) == 0 && w1_pending(&s.c.w) == 0);
    /* exactly-once damage: the host applied every announced stroke once (strokes on enemies that died meanwhile are the late ones) */
    (void)sent;
    if (s.h.w.st.hits_applied + s.h.w.st.late_hits != (unsigned)(30 * 8 + client_kills)) { char t[400]; w1_summary(&s.c.w, t, sizeof t); printf("  seed %u: %s | hit_unknown %u q_full %u\n", seed, t, s.c.w.st.hit_unknown, s.c.w.st.q_full); }
    CHECK(s.h.w.st.hits_applied + s.h.w.st.late_hits == (unsigned)(30 * 8 + client_kills));
    CHECK(s.h.w.st.hits_applied >= 30u * 8u);   /* the tough ones never die: all their strokes apply */
    CHECK(w1_kills(&s.h.w, 0) == w1_kills(&s.c.w, 0) && w1_kills(&s.h.w, 1) == w1_kills(&s.c.w, 1));
    CHECK((int)w1_kills(&s.h.w, 1) == client_kills && (int)w1_kills(&s.h.w, 0) == host_kills);
    CHECK(s.c.kills_own == client_kills && s.c.kills_other == host_kills);
    CHECK(s.c.kill_calls == client_kills + host_kills);   /* one kill call per kill, never two */
    CHECK(s.c.w.st.roster_despawn == 0);
    /* hp of the tough ones agrees (the last record of the stream) */
    int hp_ok = 1;
    for (int i = 0; i < N; i += 5) {
        MEnemy *c = m_find(&s.c, e[i]->key);
        if (!c || fabsf(c->hp - e[i]->hp) > 0.01f) hp_ok = 0;
    }
    CHECK(hp_ok);
    /* items / doors / flags */
    uint32_t vt; unsigned ph, ver; int32_t fv;
    CHECK(w1_door_get(&s.h.w, &dk, &vt, &ph, &ver) && ph == 1 && w1_door_get(&s.c.w, &dk, &vt, &ph, &ver) && ph == 1);   /* host won the tie */
    CHECK(w1_flag_get(&s.h.w, 4704, &fv, &ver) && fv == 1 && w1_flag_get(&s.c.w, 4704, &fv, &ver) && fv == 1);
    CHECK(w1_flag_get(&s.h.w, 4710, &fv, &ver) && fv == 1 && w1_flag_get(&s.c.w, 4710, &fv, &ver) && fv == 1);
    int t1, t2;
    CHECK(w1_item_is_gone(&s.h.w, &pk1, &t1) && w1_item_is_gone(&s.c.w, &pk1, &t2) && t1 == t2 && t1 == 0);   /* the host's human got it on both PCs' books */
    CHECK(w1_item_is_gone(&s.h.w, &pk2, &t1) && w1_item_is_gone(&s.c.w, &pk2, &t2) && t1 == 1 && t2 == 1);
    CHECK(s.h.items_gone == 1 && s.c.items_gone == 0 && s.h.taker_last == 1);   /* pk2 reached the host once; pk1 was already gone locally on both PCs: no callback */
}

static void t_chaos(void)
{
    Prof profs[] = { { 0, 0, 0, 20 }, { 25, 0, 0, 40 }, { 25, 15, 80, 40 }, { 10, 30, 150, 60 }, { 40, 20, 100, 50 } };
    for (unsigned p = 0; p < sizeof profs / sizeof profs[0]; p++)
        for (unsigned seed = 1; seed <= 6; seed++) run_chaos(seed * 17 + p, profs[p], 0);
}

/* the exactly-once property in the worst line: every packet twice, plus loss */
static void t_exactly_once(void)
{
    Sim s;
    sim_init(&s, 77, 25, 100, 60, 30);
    MEnemy *a = host_add(&s, 11, 0, 0x80031000, 1000);
    run_move(&s, 3000);
    float pt[3] = { 0, 0, 0 };
    for (int i = 0; i < 50; i++) {
        w1_client_hit(&s.c.w, a->key, 3.0f, 1, pt);
        run_move(&s, 20);
    }
    run_move(&s, 6000);
    CHECK(s.h.w.st.hits_applied == 50);
    CHECK(fabs(s.h.hp_lost - 150.0) < 1e-6);
    CHECK(s.h.w.st.dup > 0 && s.c.w.st.dup > 0);
}

/* an enemy's blow on the client's human (PCHIT): host -> client only, exactly once, payload intact, over a bad line */
static void t_pchit(void)
{
    Sim s;
    sim_init(&s, 91, 30, 50, 80, 30);
    uint8_t pl[W1_PCHIT_MAX + 1];
    for (int k = 0; k < 40; k++) {
        pl[0] = (uint8_t)k;
        for (unsigned i = 1; i < W1_PCHIT_MAX; i++) pl[i] = (uint8_t)(k * 7 + i);
        CHECK(w1_host_pchit(&s.h.w, W1_KEY(11, k % 5), pl, W1_PCHIT_MAX) == 1);
        run_move(&s, 30);
    }
    run_move(&s, 8000);
    int once = 1;
    for (int k = 0; k < 40; k++) once &= s.c.pchit_seen[k] == 1;
    CHECK(once);
    CHECK(s.c.pchits == 40 && s.c.pchit_bad == 0);
    CHECK(s.h.w.st.pchit_tx == 40 && s.c.w.st.pchit_rx == 40);
    CHECK(s.h.pchits == 0);
    CHECK(w1_host_pchit(&s.c.w, W1_KEY(11, 0), pl, 10) == 0);            /* a client never sends one */
    CHECK(w1_host_pchit(&s.h.w, W1_KEY(11, 0), pl, W1_PCHIT_MAX + 1) == 0);   /* too long */
    CHECK(w1_host_pchit(&s.h.w, W1_NO_KEY, NULL, 0) == 1);               /* no attacker known, no payload: allowed */
    run_move(&s, 3000);
    CHECK(s.c.pchits == 41 && s.c.pchit_att == W1_NO_KEY);
    /* a forged PCHIT from the client side is refused by the host (counted as bad), and a truncated one by the client */
    uint8_t ev[8] = {5, 1, 0, CP_W1_PCHIT, 0x00, 0x0B, 4, 1};
    unsigned bad0 = s.h.w.st.bad;
    w1_recv(&s.h.w, W1_LANE_EVENT, CP_EV_W1, ev, sizeof ev);
    CHECK(s.h.w.st.bad == bad0 + 1 && s.h.pchits == 0);
    w1_set_epoch(&s.h.w, 6), w1_set_epoch(&s.c.w, 6);
    unsigned cbad = s.c.w.st.bad;
    uint8_t ev2[8] = {6, 1, 0, CP_W1_PCHIT, 0x00, 0x0B, 40, 1};   /* says 40 bytes, carries 1 */
    w1_recv(&s.c.w, W1_LANE_EVENT, CP_EV_W1, ev2, sizeof ev2);
    CHECK(s.c.w.st.bad == cbad + 1 && s.c.pchits == 41);
}

/* an enemy that only turns (same place, same hp) is sent too: the heading drives the replica's interpolator */
static void t_turn_only(void)
{
    Sim s;
    sim_init(&s, 5, 0, 0, 0, 20);
    MEnemy *a = host_add(&s, 11, 2, 0x80031000, 100);
    for (int t = 0; t < 20; t++) w1_host_sample(&s.h.w, a->key, a->pos, 0.2f, a->hp, 3, 0), sim_step(&s, 50);
    w1_enemy_rec r;
    CHECK(w1_enemy_get(&s.c.w, a->key, &r, NULL) && fabsf(r.yaw - 0.2f) < 0.01f);
    for (int t = 0; t < 6; t++) w1_host_sample(&s.h.w, a->key, a->pos, 1.3f, a->hp, 3, 0), sim_step(&s, 50);   /* 300 ms: well under the 1 s refresh */
    CHECK(w1_enemy_get(&s.c.w, a->key, &r, NULL) && fabsf(r.yaw - 1.3f) < 0.01f);
    unsigned rec0 = s.c.w.st.rec_applied;
    for (int t = 0; t < 6; t++) w1_host_sample(&s.h.w, a->key, a->pos, 1.31f, a->hp, 3, 0), sim_step(&s, 50);   /* under 2 degrees: no new record before the refresh */
    CHECK(s.c.w.st.rec_applied == rec0);
}

/* ---------------------------------------------------------------- 6. epochs */
static void t_epoch(void)
{
    Sim s;
    sim_init(&s, 8, 0, 0, 0, 200);   /* 200 ms in flight */
    host_add(&s, 11, 0, 0x80031000, 50);
    host_add(&s, 11, 1, 0x80031000, 50);
    run_move(&s, 1000);
    CHECK(s.c.nen == 2);
    /* things in flight when the level changes */
    float pt[3] = { 0, 0, 0 };
    w1_host_death(&s.h.w, W1_KEY(11, 0), 0, 1, pt);
    w1_local_flag(&s.h.w, 4800, 1);
    w1_client_hit(&s.c.w, W1_KEY(11, 1), 5, 1, pt);
    run_move(&s, 30);   /* sent, not delivered yet */
    CHECK(w1_pending(&s.h.w) > 0);
    /* the host starts the new level first; the client is still on the old one for a moment */
    w1_set_epoch(&s.h.w, 6);
    CHECK(w1_pending(&s.h.w) == 0 && w1_count(&s.h.w, W1_ALIVE) == 0 && w1_kills(&s.h.w, 0) == 0);
    host_add(&s, 30, 0, 0x80031000, 50);   /* new epoch: same engine key space is fresh */
    run_move(&s, 100);
    uint32_t stale_before = s.c.w.st.stale_epoch;
    run_move(&s, 400);
    CHECK(s.c.w.st.stale_epoch > stale_before);        /* the client refuses the host's epoch 6 packets while it is on 5 */
    CHECK(w1_enemy_state(&s.c.w, W1_KEY(30, 0)) == W1_UNKNOWN);
    /* the old epoch packets still in flight to the host are refused after its switch */
    CHECK(s.h.w.st.hits_applied == 0);
    w1_set_epoch(&s.c.w, 6);
    run_move(&s, 3000);
    CHECK(w1_enemy_state(&s.c.w, W1_KEY(30, 0)) == W1_ALIVE && w1_enemy_state(&s.c.w, W1_KEY(11, 0)) == W1_UNKNOWN);
    CHECK(w1_kills(&s.c.w, 0) == 0 && s.h.w.st.hits_applied == 0);
    int32_t fv; unsigned ver;
    CHECK(!w1_flag_get(&s.c.w, 4800, &fv, &ver));       /* the old level's flag event never reached the new level */
    CHECK(w1_pending(&s.h.w) == 0 && w1_pending(&s.c.w) == 0);
    /* same epoch again = no-op, a wrong epoch value is refused */
    w1_set_epoch(&s.c.w, 6);
    CHECK(w1_enemy_state(&s.c.w, W1_KEY(30, 0)) == W1_ALIVE);
    uint8_t junk[8] = { 9, 1, 0, CP_W1_DESPAWN, 30, 0 };
    CHECK(w1_recv(&s.c.w, W1_LANE_EVENT, CP_EV_W1, junk, 6) == 0);
    CHECK(w1_enemy_state(&s.c.w, W1_KEY(30, 0)) == W1_ALIVE);
    /* epoch counter wraps (u8) */
    w1_set_epoch(&s.c.w, 255), w1_set_epoch(&s.c.w, 0);
    CHECK(w1_count(&s.c.w, W1_ALIVE) == 0 && w1_epoch(&s.c.w) == 0);
    /* no epoch set yet: everything is refused */
    w1_t fresh;
    w1_init(&fresh, W1_CLIENT, 1, NULL);
    CHECK(w1_recv(&fresh, W1_LANE_EVENT, CP_EV_W1, junk, 6) == 0 && w1_epoch(&fresh) == 0xFFFFFFFFu);
}

/* ---------------------------------------------------------------- 7. roster repairs */
static int drop_despawn_events(int lane, unsigned kind, const uint8_t *b, unsigned len)
{
    (void)kind;
    return lane == W1_LANE_EVENT && len >= 4 && b[3] == CP_W1_DESPAWN;
}
static int drop_spawn_events(int lane, unsigned kind, const uint8_t *b, unsigned len)
{
    (void)kind;
    return lane == W1_LANE_EVENT && len >= 4 && b[3] == CP_W1_SPAWN;
}

static void t_roster(void)
{
    /* (a) the DESPAWN never arrives: the roster removes the replica after W1_ROSTER_MISS rosters */
    {
        Sim s;
        sim_init(&s, 9, 5, 0, 0, 20);
        MEnemy *a = host_add(&s, 11, 0, 0x80031000, 50);
        host_add(&s, 11, 1, 0x80031000, 50);
        run_move(&s, 1500);
        CHECK(s.c.nen == 2);
        s.hc.drop = drop_despawn_events;
        w1_host_despawn(&s.h.w, a->key);
        a->alive = 0;
        run_move(&s, 3000);
        CHECK(w1_enemy_state(&s.c.w, a->key) == W1_GONE && s.c.w.st.roster_despawn == 1 && w1_enemy_state(&s.c.w, W1_KEY(11, 1)) == W1_ALIVE);
        CHECK(s.c.despawn_calls == 1);
    }
    /* (b) the SPAWN never arrives (event lane to the client blocked for a spawn): the client notices the roster entry and asks again */
    {
        Sim s;
        sim_init(&s, 10, 0, 0, 0, 20);
        s.hc.drop = drop_spawn_events;
        host_add(&s, 11, 3, 0x80031000, 50);
        run_move(&s, 2500);
        CHECK(s.c.nen == 0 && s.c.w.st.req_sent >= 1 && s.h.w.st.req_served >= 1);
        CHECK(w1_enemy_state(&s.c.w, W1_KEY(11, 3)) == W1_UNKNOWN);
        s.hc.drop = NULL;
        run_move(&s, 3000);
        CHECK(w1_enemy_state(&s.c.w, W1_KEY(11, 3)) == W1_ALIVE && s.c.nen == 1);
    }
    /* (c) the roster despawn does not cost the kill credit when the DEATH arrives late */
    {
        Sim s;
        sim_init(&s, 11, 0, 0, 0, 20);
        MEnemy *a = host_add(&s, 11, 0, 0x80031000, 50);
        run_move(&s, 1000);
        s.hc.black_events = 1;
        float pt[3] = { 0, 0, 0 };
        a->alive = 0;
        w1_host_hit_note(&s.h.w, a->key, 1);
        w1_host_death(&s.h.w, a->key, 1, 1, pt);
        run_move(&s, 2500);   /* the roster no longer lists it: the replica goes away */
        CHECK(w1_enemy_state(&s.c.w, a->key) == W1_GONE && s.c.w.st.roster_despawn == 1);
        s.hc.black_events = 0;
        run_move(&s, 2000);
        CHECK(w1_kills(&s.c.w, 1) == 1 && w1_kills(&s.h.w, 1) == 1 && w1_pending(&s.h.w) == 0);
    }
}

/* ---------------------------------------------------------------- 8. items, doors, flags: rules */
static void t_events(void)
{
    Sim s;
    sim_init(&s, 12, 0, 0, 0, 20);
    w1_key4 k = { { 0x11, 0x22, 5, 3 } }, k2 = { { 0x11, 0x22, 5, 4 } };
    /* PICKUP: an event, never a bag content: the other PC just removes the world object */
    CHECK(w1_local_pickup(&s.c.w, &k, 0x80040200) == 1);
    CHECK(w1_local_pickup(&s.c.w, &k, 0x80040200) == 0);   /* already gone here: nothing announced, counted as a conflict */
    run_move(&s, 500);
    CHECK(s.h.items_gone == 1 && s.h.taker_last == 1 && w1_item_is_gone(&s.h.w, &k, NULL) && !w1_item_is_gone(&s.h.w, &k2, NULL));
    CHECK(s.c.items_gone == 0);   /* no echo on the taker's own PC */
    /* the host takes the same item later on its PC (before it saw the event): conflict resolved the same way on both */
    CHECK(w1_local_pickup(&s.h.w, &k, 0x80040200) == 0);
    /* doors: a change is announced, an echo (same state) is not; a stale version loses */
    CHECK(w1_local_door(&s.h.w, &k, 0x1032100, 1) == 1 && w1_local_door(&s.h.w, &k, 0x1032100, 1) == 0);
    run_move(&s, 300);
    CHECK(s.c.door_calls == 1 && s.c.door_phase == 1);
    CHECK(w1_local_door(&s.c.w, &k, 0x1032100, 3) == 1);
    run_move(&s, 300);
    CHECK(s.h.door_calls == 1 && s.h.door_phase == 3);
    unsigned ver;
    CHECK(w1_door_get(&s.h.w, &k, NULL, NULL, &ver) && ver == 2);
    /* a stale door event (version 1 again) is ignored */
    uint8_t stale[40];
    memset(stale, 0, sizeof stale);
    stale[0] = 5, stale[1] = (uint8_t)(s.c.w.tx_seq + 1), stale[3] = CP_W1_DOOR;   /* hand made: epoch 5, seq, sub */
    for (int i = 0; i < 4; i++) memcpy(stale + 4 + 4 * i, &k.k[i], 4);
    uint32_t vt = 0x1032100;
    memcpy(stale + 20, &vt, 4);
    stale[24] = 9, stale[25] = 1, stale[26] = 0;   /* phase 9, version 1 */
    { unsigned q = ++s.c.w.tx_seq; stale[1] = (uint8_t)q, stale[2] = (uint8_t)(q >> 8); }   /* the client 'sent' it: its stream continues after it */
    CHECK(w1_recv(&s.h.w, W1_LANE_EVENT, CP_EV_W1, stale, 27) == 1);
    CHECK(s.h.door_calls == 1 && s.h.w.st.door_lost == 1);
    /* flags */
    CHECK(w1_local_flag(&s.c.w, 4712, 1) == 1 && w1_local_flag(&s.c.w, 4712, 1) == 0);
    run_move(&s, 300);
    CHECK(s.h.flag_calls == 1 && s.h.flag_id == 4712 && s.h.flag_val == 1);
    CHECK(w1_local_flag(&s.h.w, 4712, 0) == 1);
    run_move(&s, 300);
    CHECK(s.c.flag_calls == 1 && s.c.flag_val == 0);
    int32_t fv;
    CHECK(w1_flag_get(&s.c.w, 4712, &fv, &ver) && fv == 0 && ver == 2);
    /* direction rules: the client cannot make the host spawn / kill, the host cannot send hits */
    uint8_t bad[16] = { 5, 0, 0, CP_W1_SPAWN };
    { unsigned q = ++s.c.w.tx_seq; bad[1] = (uint8_t)q, bad[2] = (uint8_t)(q >> 8); }
    unsigned badn = s.h.w.st.bad;
    CHECK(w1_recv(&s.h.w, W1_LANE_EVENT, CP_EV_W1, bad, 4 + 24) == 1 && s.h.w.st.bad == badn + 1);
    CHECK(w1_enemy_state(&s.h.w, 0x0101) == W1_UNKNOWN);
    /* a hit that claims the host's slot is refused */
    host_add(&s, 1, 1, 0x80031000, 50);
    run_move(&s, 500);
    uint8_t hit[27];
    memset(hit, 0, sizeof hit);
    hit[0] = 5, hit[3] = CP_W1_HIT, hit[4] = 1, hit[5] = 1, hit[6] = 0;   /* key (1,1)=0x0101: lo=1, hi=1; slot 0 = the host's own */
    float dmg = 10.0f;
    memcpy(hit + 7, &dmg, 4);
    unsigned seq = ++s.c.w.tx_seq;
    hit[1] = (uint8_t)seq, hit[2] = (uint8_t)(seq >> 8);
    unsigned bs = s.h.w.st.hit_bad_slot;
    CHECK(w1_recv(&s.h.w, W1_LANE_EVENT, CP_EV_W1, hit, 4 + 23) == 1 && s.h.w.st.hit_bad_slot == bs + 1 && s.h.hp_lost == 0);
}

/* ---------------------------------------------------------------- 9. robustness: garbage never breaks the model */
static void t_fuzz(void)
{
    Sim s;
    sim_init(&s, 13, 0, 0, 0, 20);
    for (int i = 0; i < 10; i++) host_add(&s, 11, i, 0x80031000, 50);
    run_move(&s, 1500);
    unsigned rng = 99;
    uint8_t buf[CP_WORLD_MAX];
    for (int iter = 0; iter < 20000; iter++) {
        unsigned len = 1 + rnd(&rng) % 80;
        for (unsigned i = 0; i < len; i++) buf[i] = (uint8_t)rnd(&rng);
        if (rnd(&rng) % 2) buf[0] = 5;   /* the right epoch about half of the time */
        int lane = rnd(&rng) % 2;
        unsigned kind = lane == W1_LANE_EVENT ? CP_EV_W1 : (rnd(&rng) % 3) + CP_WK_W1_ENEMIES;
        w1_recv(rnd(&rng) % 2 ? &s.c.w : &s.h.w, lane, kind, buf, len);
    }
    /* truncated valid messages of every kind */
    Node cl;
    Link dummy;
    memset(&dummy, 0, sizeof dummy);
    node_init(&cl, W1_CLIENT, 1, &dummy);
    w1_set_epoch(&cl.w, 5);
    CHECK(w1_recv(&cl.w, W1_LANE_EVENT, CP_EV_W1, (const uint8_t *)"", 0) == 0);
    unsigned bad0 = cl.w.st.bad;
    CHECK(w1_recv(&cl.w, W1_LANE_WORLD, CP_WK_W1_ENEMIES, (const uint8_t *)"\x05\x01", 2) == 1 && cl.w.st.bad == bad0 + 1);   /* truncated: counted, no effect */
    CHECK(w1_recv(&cl.w, W1_LANE_WORLD, 0x40, (const uint8_t *)"\x05\x01\x00", 3) == 0);
    CHECK(w1_recv(&cl.w, 7, 0, (const uint8_t *)"\x05", 1) == 0);
    /* NaN / inf floats are refused */
    uint8_t nanspawn[28] = { 5, 1, 0, CP_W1_SPAWN, 1, 1, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0xC0, 0x7F };
    unsigned badn = cl.w.st.bad;
    w1_recv(&cl.w, W1_LANE_EVENT, CP_EV_W1, nanspawn, 28);
    CHECK(cl.w.st.bad == badn + 1 && w1_count(&cl.w, W1_ALIVE) == 0);
    /* the models survived: a new level instance starts clean and replicates (forged windows of the old epoch are gone with it) */
    w1_set_epoch(&s.h.w, 6), w1_set_epoch(&s.c.w, 6);
    for (int i = 0; i < 3; i++) host_add(&s, 40, i, 0x80031000, 50);
    run_move(&s, 3000);
    int ok = 0;
    for (int i = 0; i < 3; i++) ok += w1_enemy_state(&s.c.w, W1_KEY(40, i)) == W1_ALIVE;
    CHECK(ok == 3);
}

/* ---------------------------------------------------------------- 10. table limits, key encoding, summary */
static void t_misc(void)
{
    CHECK(W1_KEY(0x91, 0x23) == 0x9123 && sizeof(w1_key) == 2);
    Sim s;
    sim_init(&s, 14, 0, 0, 0, 5);
    float p[3] = { 0, 0, 0 };
    int made = 0;
    for (int i = 0; i < 300; i++) made += w1_host_spawn(&s.h.w, W1_KEY(i / 200 + 1, i % 200), 0x80031000, 0, 0, p, 1);
    CHECK(made == W1_MAX_ENEMIES && s.h.w.st.table_full > 0);
    char txt[400];
    w1_summary(&s.h.w, txt, sizeof txt);
    CHECK(strstr(txt, "host") && strstr(txt, "alive 128"));
    /* stale batches: an older batch never overwrites a newer record */
    Sim t;
    sim_init(&t, 15, 0, 0, 0, 5);
    MEnemy *a = host_add(&t, 11, 0, 0x80031000, 50);
    run_move(&t, 600);
    t.hc.base = 400;
    a->pos[0] = 1111;
    host_move(&t);
    sim_step(&t, 70);   /* record A in flight, 400 ms */
    t.hc.base = 5;
    a->pos[0] = 2222;
    host_move(&t);
    sim_step(&t, 70);   /* record B overtakes it */
    sim_step(&t, 500);
    MEnemy *c = m_find(&t.c, a->key);
    CHECK(c && c->pos[0] > 2000.0f && t.c.w.st.rec_old >= 1);
}

int main(void)
{
    t_envelope();
    t_replicate();
    t_credit();
    t_late_hit();
    t_order();
    t_chaos();
    t_exactly_once();
    t_pchit();
    t_turn_only();
    t_epoch();
    t_roster();
    t_events();
    t_fuzz();
    t_misc();
    printf("world1: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
