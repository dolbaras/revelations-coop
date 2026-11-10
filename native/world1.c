/*
 * world1.c: the shared world model of Revelations 1 (see world1.h). Pure C, no Windows and no game dependencies.
 *
 * Wire (all little endian, floats binary32), carried in the envelopes named in coop_proto.h:
 *   CP_EV_W1 (reliable lane, retransmitted by the model until acked):  epoch u8 | seq u16 | sub u8 | payload
 *     SPAWN   host -> client  key u16, cls u32, owner u8, flags u8, pos f32 x3, hp f32                     (24)
 *     HIT     client -> host  key u16, attacker slot u8, damage f32, hit flags u32, point f32 x3          (23)
 *     DEATH   host -> client  key u16, killer slot u8 (0xFF none), hit flags u32, point f32 x3            (19)
 *     DESPAWN host -> client  key u16                                                                      (2)
 *     PICKUP  both            key4 u32 x4, cls u32, taker slot u8                                         (21)
 *     DOOR    both            key4 u32 x4, vtbl u32, phase u8, ver u16                                    (23)
 *     FLAG    both            id u32, value i32, ver u16                                                  (10)
 *     REQ     client -> host  n u8, key u16 x n (n <= 16): "I do not know these live enemies, send SPAWN again"
 *   CP_WK_W1_ENEMIES (host -> client, unreliable, ~15 Hz): epoch u8 | bseq u16 | ack_lo u16 | ack_bits u32 | n u8 | n x record
 *     record (26): key u16, pos f32 x3, yaw i16 (rad * 32767 / pi), hp f32, ai u16, e1c u32
 *   CP_WK_W1_ROSTER (host -> client, unreliable, ~3 Hz): epoch u8 | rseq u16 | ack_lo u16 | ack_bits u32 | n u8 | n x key u16 (live enemies)
 *   CP_WK_W1_ACK (both, unreliable): epoch u8 | ack_lo u16 | ack_bits u32      (ack_bits bit i = event seq ack_lo + 1 + i received)
 */
#include "world1.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "coop_proto.h"

/* ---------------------------------------------------------------- byte io */
typedef struct { uint8_t *p; unsigned n, cap; int err; } WB;
typedef struct { const uint8_t *p; unsigned n, pos; int err; } RB;

static void wb_u8(WB *w, unsigned v) { if (w->n + 1 > w->cap) { w->err = 1; return; } w->p[w->n++] = (uint8_t)v; }
static void wb_u16(WB *w, unsigned v) { wb_u8(w, v & 0xFF), wb_u8(w, (v >> 8) & 0xFF); }
static void wb_u32(WB *w, uint32_t v) { wb_u16(w, v & 0xFFFF), wb_u16(w, v >> 16); }
static void wb_f32(WB *w, float f) { uint32_t u; memcpy(&u, &f, 4); wb_u32(w, u); }
static void wb_key4(WB *w, const w1_key4 *k) { for (int i = 0; i < 4; i++) wb_u32(w, k->k[i]); }

static unsigned rb_u8(RB *r) { if (r->pos + 1 > r->n) { r->err = 1; return 0; } return r->p[r->pos++]; }
static unsigned rb_u16(RB *r) { unsigned a = rb_u8(r); unsigned b = rb_u8(r); return a | (b << 8); }
static uint32_t rb_u32(RB *r) { uint32_t a = rb_u16(r); uint32_t b = rb_u16(r); return a | (b << 16); }
static float rb_f32(RB *r) { uint32_t u = rb_u32(r); float f; memcpy(&f, &u, 4); if (!isfinite(f)) { r->err = 1; return 0; } return f; }
static void rb_key4(RB *r, w1_key4 *k) { for (int i = 0; i < 4; i++) k->k[i] = rb_u32(r); }

static int newer16(unsigned a, unsigned b) { return (int16_t)(uint16_t)(a - b) > 0; }
static int key4_eq(const w1_key4 *a, const w1_key4 *b) { return !memcmp(a, b, sizeof *a); }
static int slot_ok(unsigned s) { return s <= 1; }

/* ---------------------------------------------------------------- setup */
static void reset_state(w1_t *w)
{
    w1_stats keep = w->st;
    memset(w->q, 0, sizeof w->q);
    memset(w->e, 0, sizeof w->e);
    memset(w->it, 0, sizeof w->it);
    memset(w->dr, 0, sizeof w->dr);
    memset(w->fl, 0, sizeof w->fl);
    w->ne = w->ni = w->nd = w->nf = 0;
    w->tx_seq = 0, w->peer_ack_lo = 0, w->rx_lo = 0, w->rx_bits = 0, w->ack_dirty = 0;
    w->bseq = w->rseq = 0, w->cursor = 0, w->last_batch_ms = w->last_roster_ms = w->last_ack_ms = w->last_req_ms = w->now;
    w->have_rseq = 0, w->last_rseq = 0, w->n_prev_missing = 0, w->req_valid = 0;
    w->kills[0] = w->kills[1] = 0;
    w->in_hit = 0, w->hit_slot = W1_NO_SLOT, w->hit_key = 0;
    w->st = keep;
}

void w1_init(w1_t *w, int role, int own_slot, const w1_ops *ops)
{
    memset(w, 0, sizeof *w);
    w->role = role, w->own_slot = own_slot & 1, w->peer_slot = 1 - (own_slot & 1);
    if (ops) w->ops = *ops;
    w->hit_slot = W1_NO_SLOT;
}

void w1_set_epoch(w1_t *w, unsigned epoch)
{
    if (w->epoch_valid && w->epoch == (uint8_t)epoch) return;
    reset_state(w);
    w->epoch = (uint8_t)epoch, w->epoch_valid = 1;
}

unsigned w1_epoch(const w1_t *w) { return w->epoch_valid ? w->epoch : 0xFFFFFFFFu; }

static void out(w1_t *w, int lane, unsigned kind, const uint8_t *b, unsigned n)
{
    if (lane == W1_LANE_WORLD) w->st.tx_world++;
    if (w->ops.send) w->ops.send(w->ops.send_user ? w->ops.send_user : w->ops.user, lane, kind, b, n);
}

/* ---------------------------------------------------------------- reliable layer */
static int in_window(const w1_t *w, unsigned seq)
{
    return (uint16_t)(seq - w->peer_ack_lo) <= W1_WINDOW;   /* also false for "behind" (>= 0x8000) */
}

static void ev_send_one(w1_t *w, w1_txev *e, int retry)
{
    out(w, W1_LANE_EVENT, CP_EV_W1, e->body, e->len);
    e->last_ms = w->now, e->sent = 1;
    if (retry) w->st.retx++; else w->st.tx_ev++;
}

static int ev_push(w1_t *w, unsigned sub, const uint8_t *payload, unsigned plen)
{
    if (!w->epoch_valid || plen + 4 > sizeof ((w1_txev *)0)->body || plen + 4 > CP_EVENT_MAX) return 0;
    w1_txev *slot = NULL;
    for (int i = 0; i < W1_QMAX; i++) if (!w->q[i].used) { slot = &w->q[i]; break; }
    if (!slot) { w->st.q_full++; return 0; }
    memset(slot, 0, sizeof *slot);
    slot->used = 1, slot->seq = ++w->tx_seq;
    WB b = { slot->body, 0, sizeof slot->body, 0 };
    wb_u8(&b, w->epoch), wb_u16(&b, slot->seq), wb_u8(&b, sub);
    for (unsigned i = 0; i < plen; i++) wb_u8(&b, payload[i]);
    slot->len = (uint8_t)b.n;
    if (in_window(w, slot->seq)) ev_send_one(w, slot, 0);
    return 1;
}

static void tx_ack(w1_t *w, unsigned ack_lo, uint32_t bits)
{
    if (newer16(ack_lo, w->tx_seq)) return;   /* acknowledges events we never sent: ignore */
    if (newer16(ack_lo, w->peer_ack_lo)) w->peer_ack_lo = (uint16_t)ack_lo;
    for (int i = 0; i < W1_QMAX; i++) {
        w1_txev *e = &w->q[i];
        if (!e->used) continue;
        unsigned d = (uint16_t)(e->seq - ack_lo);
        if (d == 0 || d >= 0x8000u) e->used = 0;
        else if (d <= W1_WINDOW && ((bits >> (d - 1)) & 1u)) e->used = 0;
    }
}

static void send_ack(w1_t *w)
{
    uint8_t buf[16];
    WB b = { buf, 0, sizeof buf, 0 };
    wb_u8(&b, w->epoch), wb_u16(&b, w->rx_lo), wb_u32(&b, w->rx_bits);
    out(w, W1_LANE_WORLD, CP_WK_W1_ACK, buf, b.n);
    w->ack_dirty = 0, w->last_ack_ms = w->now;
}

unsigned w1_pending(const w1_t *w)
{
    unsigned n = 0;
    for (int i = 0; i < W1_QMAX; i++) n += w->q[i].used;
    return n;
}

/* ---------------------------------------------------------------- tables */
static w1_enemy *e_find(w1_t *w, w1_key key)
{
    for (unsigned i = 0; i < w->ne; i++) if (w->e[i].key == key) return &w->e[i];
    return NULL;
}
static const w1_enemy *e_findc(const w1_t *w, w1_key key) { return e_find((w1_t *)w, key); }

static w1_enemy *e_new(w1_t *w, w1_key key, int st)
{
    if (w->ne >= W1_MAX_ENEMIES) { w->st.table_full++; return NULL; }
    w1_enemy *e = &w->e[w->ne++];
    memset(e, 0, sizeof *e);
    e->key = key, e->st = (uint8_t)st, e->killer = W1_NO_SLOT, e->last_hit = W1_NO_SLOT;
    return e;
}

static void e_fill_rec(const w1_enemy *e, w1_enemy_rec *r)
{
    memset(r, 0, sizeof *r);
    r->key = e->key, r->cls = e->cls, r->owner = e->owner, r->spawn_flags = e->spawn_flags;
    memcpy(r->pos, e->pos, sizeof r->pos);
    r->yaw = e->yaw, r->hp = e->hp, r->ai = e->ai, r->e1c = e->e1c;
}

/* ---------------------------------------------------------------- host side: reports from the engine */
static void send_spawn(w1_t *w, const w1_enemy *e)
{
    uint8_t buf[32];
    WB b = { buf, 0, sizeof buf, 0 };
    wb_u16(&b, e->key), wb_u32(&b, e->cls), wb_u8(&b, e->owner), wb_u8(&b, e->spawn_flags);
    for (int i = 0; i < 3; i++) wb_f32(&b, e->pos[i]);
    wb_f32(&b, e->hp);
    ev_push(w, CP_W1_SPAWN, buf, b.n);
}

int w1_host_spawn(w1_t *w, w1_key key, uint32_t cls, int owner, int spawn_flags, const float pos[3], float hp)
{
    if (w->role != W1_HOST || !w->epoch_valid || e_find(w, key)) return 0;
    w1_enemy *e = e_new(w, key, W1_ALIVE);
    if (!e) return 0;
    e->cls = cls, e->owner = (uint8_t)owner, e->spawn_flags = (uint8_t)spawn_flags, e->hp = e->hpmax = hp, e->dirty = 1;
    if (pos) memcpy(e->pos, pos, sizeof e->pos);
    memcpy(e->lpos, e->pos, sizeof e->lpos), e->lhp = hp, e->lyaw = 0.0f;
    w->st.spawns++;
    send_spawn(w, e);
    return 1;
}

void w1_host_sample(w1_t *w, w1_key key, const float pos[3], float yaw, float hp, unsigned ai, uint32_t e1c)
{
    w1_enemy *e = e_find(w, key);
    if (w->role != W1_HOST || !e || e->st != W1_ALIVE) return;
    int ch = fabsf(pos[0] - e->lpos[0]) > 1.0f || fabsf(pos[1] - e->lpos[1]) > 1.0f || fabsf(pos[2] - e->lpos[2]) > 1.0f || hp != e->lhp || (uint16_t)ai != e->lai || e1c != e->le1c ||
             fabsf(yaw - e->lyaw) > 0.035f;   /* about 2 degrees: a replica that only turns is sent too (heading for the interpolator) */
    memcpy(e->pos, pos, sizeof e->pos);
    e->yaw = yaw, e->hp = hp, e->ai = (uint16_t)ai, e->e1c = e1c;
    if (ch) e->dirty = 1;
}

void w1_host_hit_note(w1_t *w, w1_key key, int attacker_slot)
{
    w1_enemy *e = e_find(w, key);
    if (e && e->st == W1_ALIVE && slot_ok((unsigned)attacker_slot)) e->last_hit = (uint8_t)attacker_slot;
}

int w1_host_death(w1_t *w, w1_key key, int killer_hint, uint32_t hit_flags, const float pt[3])
{
    w1_enemy *e = e_find(w, key);
    if (w->role != W1_HOST || !e) return -1;
    if (e->st != W1_ALIVE) return e->st == W1_DEAD ? (int)e->killer : -1;
    unsigned k = W1_NO_SLOT;
    if (slot_ok((unsigned)killer_hint)) k = (unsigned)killer_hint;
    else if (w->in_hit && w->hit_key == key && slot_ok((unsigned)w->hit_slot)) k = (unsigned)w->hit_slot;
    else if (slot_ok(e->last_hit)) k = e->last_hit;
    e->st = W1_DEAD, e->killer = (uint8_t)k, e->hp = 0;
    if (slot_ok(k)) w->kills[k]++;
    w->st.deaths++;
    uint8_t buf[24];
    WB b = { buf, 0, sizeof buf, 0 };
    float z[3] = { 0, 0, 0 };
    if (!pt) pt = z;
    wb_u16(&b, key), wb_u8(&b, k), wb_u32(&b, hit_flags);
    for (int i = 0; i < 3; i++) wb_f32(&b, pt[i]);
    ev_push(w, CP_W1_DEATH, buf, b.n);
    return (int)k;
}

int w1_host_despawn(w1_t *w, w1_key key)
{
    w1_enemy *e = e_find(w, key);
    if (w->role != W1_HOST || !e || (e->st != W1_ALIVE && e->st != W1_DEAD)) return 0;
    e->st = W1_GONE;
    w->st.despawns++;
    uint8_t buf[2];
    WB b = { buf, 0, sizeof buf, 0 };
    wb_u16(&b, key);
    ev_push(w, CP_W1_DESPAWN, buf, b.n);
    return 1;
}

/* ---------------------------------------------------------------- client side: local hit */
float w1_client_hit(w1_t *w, w1_key key, float damage, uint32_t hit_flags, const float pt[3])
{
    w1_enemy *e = e_find(w, key);
    if (w->role != W1_CLIENT || !w->epoch_valid) return 0;
    if (!e || e->st != W1_ALIVE || !(damage > 0)) { w->st.hit_unknown++; return 0; }
    uint8_t buf[32];
    WB b = { buf, 0, sizeof buf, 0 };
    float z[3] = { 0, 0, 0 };
    if (!pt) pt = z;
    wb_u16(&b, key), wb_u8(&b, w->own_slot), wb_f32(&b, damage), wb_u32(&b, hit_flags);
    for (int i = 0; i < 3; i++) wb_f32(&b, pt[i]);
    if (!ev_push(w, CP_W1_HIT, buf, b.n)) return 0;
    /* the replica may lose hp locally but never die by itself: the host decides death (net-world-enemies-re.md, W_HP) */
    float allowed = damage;
    if (allowed > e->hp - 1.0f) allowed = e->hp - 1.0f;
    if (allowed < 0) allowed = 0;
    e->hp -= allowed;
    return allowed;
}

/* ---------------------------------------------------------------- events of both sides */
int w1_local_pickup(w1_t *w, const w1_key4 *key, uint32_t cls)
{
    if (!w->epoch_valid) return 0;
    for (unsigned i = 0; i < w->ni; i++)
        if (key4_eq(&w->it[i].key, key)) { w->st.pickup_conflict++; return 0; }
    if (w->ni < W1_MAX_ITEMS) {
        w1_item *it = &w->it[w->ni++];
        it->key = *key, it->cls = cls, it->taker = (uint8_t)w->own_slot, it->used = 1;
    } else w->st.table_full++;
    uint8_t buf[24];
    WB b = { buf, 0, sizeof buf, 0 };
    wb_key4(&b, key), wb_u32(&b, cls), wb_u8(&b, w->own_slot);
    return ev_push(w, CP_W1_PICKUP, buf, b.n);
}

static w1_door *d_find(w1_t *w, const w1_key4 *k)
{
    for (unsigned i = 0; i < w->nd; i++) if (key4_eq(&w->dr[i].key, k)) return &w->dr[i];
    return NULL;
}
static w1_flag *f_find(w1_t *w, uint32_t id)
{
    for (unsigned i = 0; i < w->nf; i++) if (w->fl[i].id == id) return &w->fl[i];
    return NULL;
}

int w1_local_door(w1_t *w, const w1_key4 *key, uint32_t vtbl, unsigned phase)
{
    if (!w->epoch_valid) return 0;
    w1_door *d = d_find(w, key);
    uint16_t ver = 1;
    if (d) {
        if (d->vtbl == vtbl && d->phase == (uint8_t)phase) return 0;
        ver = (uint16_t)(d->ver + 1);
    } else if (w->nd < W1_MAX_DOORS) d = &w->dr[w->nd++];
    else w->st.table_full++;
    if (d) d->key = *key, d->vtbl = vtbl, d->phase = (uint8_t)phase, d->ver = ver, d->by_host = w->role == W1_HOST, d->used = 1;
    uint8_t buf[32];
    WB b = { buf, 0, sizeof buf, 0 };
    wb_key4(&b, key), wb_u32(&b, vtbl), wb_u8(&b, phase), wb_u16(&b, ver);
    return ev_push(w, CP_W1_DOOR, buf, b.n);
}

int w1_local_flag(w1_t *w, uint32_t id, int32_t value)
{
    if (!w->epoch_valid) return 0;
    w1_flag *f = f_find(w, id);
    uint16_t ver = 1;
    if (f) {
        if (f->value == value) return 0;
        ver = (uint16_t)(f->ver + 1);
    } else if (w->nf < W1_MAX_FLAGS) f = &w->fl[w->nf++];
    else w->st.table_full++;
    if (f) f->id = id, f->value = value, f->ver = ver, f->by_host = w->role == W1_HOST, f->used = 1;
    uint8_t buf[16];
    WB b = { buf, 0, sizeof buf, 0 };
    wb_u32(&b, id), wb_u32(&b, (uint32_t)value), wb_u16(&b, ver);
    return ev_push(w, CP_W1_FLAG, buf, b.n);
}

/* ---------------------------------------------------------------- receive: events */
static int host_slot(const w1_t *w) { return w->role == W1_HOST ? w->own_slot : w->peer_slot; }

static int rx_spawn(w1_t *w, RB *r)
{
    if (w->role != W1_CLIENT) return 0;
    w1_enemy_rec rec;
    memset(&rec, 0, sizeof rec);
    rec.key = (w1_key)rb_u16(r), rec.cls = rb_u32(r), rec.owner = (uint8_t)rb_u8(r), rec.spawn_flags = (uint8_t)rb_u8(r);
    for (int i = 0; i < 3; i++) rec.pos[i] = rb_f32(r);
    rec.hp = rb_f32(r);
    if (r->err || !slot_ok(rec.owner)) return 0;
    if (e_find(w, rec.key)) return 1;   /* known (duplicate, or a DEATH / DESPAWN came first): nothing to do, a dead key is never resurrected */
    w1_enemy *e = e_new(w, rec.key, W1_ALIVE);
    if (!e) return 1;
    e->cls = rec.cls, e->owner = rec.owner, e->spawn_flags = rec.spawn_flags, e->hp = e->hpmax = rec.hp;
    memcpy(e->pos, rec.pos, sizeof e->pos);
    w->st.spawns++;
    if (w->ops.spawn) w->ops.spawn(w->ops.user, &rec);
    return 1;
}

static int rx_hit(w1_t *w, RB *r)
{
    if (w->role != W1_HOST) return 0;
    w1_key key = (w1_key)rb_u16(r);
    unsigned slot = rb_u8(r);
    float dmg = rb_f32(r);
    uint32_t fl = rb_u32(r);
    float pt[3];
    for (int i = 0; i < 3; i++) pt[i] = rb_f32(r);
    if (r->err) return 0;
    if (slot != (unsigned)w->peer_slot || !(dmg > 0) || dmg > 1.0e6f) { w->st.hit_bad_slot++; return 1; }
    w1_enemy *e = e_find(w, key);
    if (!e || e->st != W1_ALIVE) { w->st.late_hits++; return 1; }   /* it died / was removed first: the hit is moot */
    e->last_hit = (uint8_t)slot;
    w->in_hit = 1, w->hit_key = key, w->hit_slot = (int)slot;
    w->st.hits_applied++;
    if (w->ops.apply_hit) w->ops.apply_hit(w->ops.user, key, (int)slot, dmg, fl, pt);
    w->in_hit = 0, w->hit_slot = W1_NO_SLOT;
    return 1;
}

static int rx_death(w1_t *w, RB *r)
{
    if (w->role != W1_CLIENT) return 0;
    w1_key key = (w1_key)rb_u16(r);
    unsigned killer = rb_u8(r);
    uint32_t fl = rb_u32(r);
    float pt[3];
    for (int i = 0; i < 3; i++) pt[i] = rb_f32(r);
    if (r->err || (killer != W1_NO_SLOT && !slot_ok(killer))) return 0;
    w1_enemy *e = e_find(w, key);
    if (!e) {   /* the DEATH overtook the SPAWN: remember it, the SPAWN will find a dead key */
        e = e_new(w, key, W1_DEAD);
        if (!e) return 1;
        e->killer = (uint8_t)killer;
        if (slot_ok(killer)) w->kills[killer]++;
        w->st.deaths++;
        return 1;
    }
    if (e->st == W1_ALIVE) {
        e->st = W1_DEAD, e->killer = (uint8_t)killer, e->hp = 0;
        if (slot_ok(killer)) w->kills[killer]++;
        w->st.deaths++;
        if (w->ops.kill) w->ops.kill(w->ops.user, key, (int)killer == W1_NO_SLOT ? -1 : (int)killer, killer == (unsigned)w->own_slot, fl, pt);
    } else if (e->st == W1_GONE && e->miss == 0xFF) {   /* removed by the roster before the DEATH arrived: the credit still counts, the corpse is gone */
        e->miss = 0, e->killer = (uint8_t)killer;
        if (slot_ok(killer)) w->kills[killer]++;
        w->st.deaths++;
    }
    return 1;
}

static int rx_despawn(w1_t *w, RB *r)
{
    if (w->role != W1_CLIENT) return 0;
    w1_key key = (w1_key)rb_u16(r);
    if (r->err) return 0;
    w1_enemy *e = e_find(w, key);
    if (!e) { e_new(w, key, W1_GONE); return 1; }
    if (e->st == W1_ALIVE || e->st == W1_DEAD) {
        e->st = W1_GONE;
        w->st.despawns++;
        if (w->ops.despawn) w->ops.despawn(w->ops.user, key);
    }
    return 1;
}

static int rx_pickup(w1_t *w, RB *r)
{
    w1_key4 k;
    rb_key4(r, &k);
    uint32_t cls = rb_u32(r);
    unsigned slot = rb_u8(r);
    if (r->err || !slot_ok(slot)) return 0;
    for (unsigned i = 0; i < w->ni; i++)
        if (key4_eq(&w->it[i].key, &k)) {
            if (w->it[i].taker != slot) {   /* both humans took it at the same moment: the host's human is the one who got it, the same verdict on both PCs */
                w->it[i].taker = (uint8_t)host_slot(w);
                w->st.pickup_conflict++;
            }
            return 1;
        }
    if (w->ni < W1_MAX_ITEMS) {
        w1_item *it = &w->it[w->ni++];
        it->key = k, it->cls = cls, it->taker = (uint8_t)slot, it->used = 1;
    } else w->st.table_full++;
    if (w->ops.item_gone) w->ops.item_gone(w->ops.user, &k, cls, (int)slot);
    return 1;
}

/* "newer" for door / flag versions: a higher version wins, an equal one is won by the host (the same verdict on both PCs) */
static int ver_wins(unsigned ver, int sender_is_host, unsigned cur, int cur_by_host)
{
    if (newer16(ver, cur)) return 1;
    return ver == cur && sender_is_host && !cur_by_host;
}

static int rx_door(w1_t *w, RB *r)
{
    w1_key4 k;
    rb_key4(r, &k);
    uint32_t vtbl = rb_u32(r);
    unsigned phase = rb_u8(r), ver = rb_u16(r);
    if (r->err) return 0;
    int from_host = w->role == W1_CLIENT;
    w1_door *d = d_find(w, &k);
    if (d && !ver_wins(ver, from_host, d->ver, d->by_host)) { w->st.door_lost++; return 1; }
    if (!d && w->nd < W1_MAX_DOORS) d = &w->dr[w->nd++];
    else if (!d) w->st.table_full++;
    int same = d && d->used && d->vtbl == vtbl && d->phase == (uint8_t)phase;
    if (d) d->key = k, d->vtbl = vtbl, d->phase = (uint8_t)phase, d->ver = (uint16_t)ver, d->by_host = (uint8_t)from_host, d->used = 1;
    if (!same && w->ops.door_set) w->ops.door_set(w->ops.user, &k, vtbl, (uint8_t)phase);
    return 1;
}

int w1_host_pchit(w1_t *w, w1_key attacker, const uint8_t *payload, unsigned len)
{
    if (w->role != W1_HOST || !w->epoch_valid || len > W1_PCHIT_MAX || (len && !payload)) return 0;
    uint8_t buf[3 + W1_PCHIT_MAX];
    WB b = { buf, 0, sizeof buf, 0 };
    wb_u16(&b, attacker), wb_u8(&b, len);
    for (unsigned i = 0; i < len; i++) wb_u8(&b, payload[i]);
    if (!ev_push(w, CP_W1_PCHIT, buf, b.n)) return 0;
    w->st.pchit_tx++;
    return 1;
}

static int rx_pchit(w1_t *w, RB *r)
{
    if (w->role != W1_CLIENT) return 0;   /* only the host decides enemy blows */
    w1_key att = (w1_key)rb_u16(r);
    unsigned n = rb_u8(r);
    if (r->err || n > W1_PCHIT_MAX || r->pos + n > r->n) return 0;
    const uint8_t *p = r->p + r->pos;
    r->pos += n;
    w->st.pchit_rx++;
    if (w->ops.pc_hit) w->ops.pc_hit(w->ops.user, att, p, n);
    return 1;
}

static int rx_flag(w1_t *w, RB *r)
{
    uint32_t id = rb_u32(r);
    int32_t value = (int32_t)rb_u32(r);
    unsigned ver = rb_u16(r);
    if (r->err) return 0;
    int from_host = w->role == W1_CLIENT;
    w1_flag *f = f_find(w, id);
    if (f && !ver_wins(ver, from_host, f->ver, f->by_host)) { w->st.flag_lost++; return 1; }
    int same = f && f->used && f->value == value && !w->flag_raw;
    if (!f && w->nf < W1_MAX_FLAGS) f = &w->fl[w->nf++];
    else if (!f) w->st.table_full++;
    if (f) f->id = id, f->value = value, f->ver = (uint16_t)ver, f->by_host = (uint8_t)from_host, f->used = 1;
    if (!same && w->ops.flag_set) {
        if (w->flag_raw) w->st.flag_raw_calls++;
        w->ops.flag_set(w->ops.user, id, value);
    }
    return 1;
}

void w1_set_flag_raw(w1_t *w, int on) { w->flag_raw = on ? 1 : 0; }

int w1_note_flag(w1_t *w, uint32_t id, int32_t value)
{
    if (!w->epoch_valid) return 0;
    w1_flag *f = f_find(w, id);
    if (f && f->value == value) return 0;
    if (!f && w->nf < W1_MAX_FLAGS) f = &w->fl[w->nf++];
    else if (!f) {
        w->st.table_full++;
        return 0;
    }
    f->ver = (uint16_t)(f->used ? f->ver + 1 : 1);
    f->id = id, f->value = value, f->by_host = w->role == W1_HOST, f->used = 1;
    return 1;
}

int w1_send_ext(w1_t *w, unsigned sub, const uint8_t *body, unsigned len)
{
    if (sub < CP_W1_EXT_FIRST || sub > CP_W1_EXT_LAST || len > W1_EXT_MAX || (len && !body)) return 0;
    if (!ev_push(w, sub, body, len)) return 0;
    w->st.ext_tx++;
    return 1;
}

static int rx_ext(w1_t *w, unsigned sub, RB *r)
{
    unsigned n = r->n - r->pos;
    const uint8_t *p = r->p + r->pos;
    r->pos = r->n;
    w->st.ext_rx++;
    if (w->ops.ext) w->ops.ext(w->ops.user, sub, p, n);
    return 1;
}

static int rx_req(w1_t *w, RB *r)
{
    if (w->role != W1_HOST) return 0;
    unsigned n = rb_u8(r);
    if (n > 16) return 0;
    w1_key keys[16];
    for (unsigned i = 0; i < n; i++) keys[i] = (w1_key)rb_u16(r);
    if (r->err) return 0;
    for (unsigned i = 0; i < n; i++) {
        w1_enemy *e = e_find(w, keys[i]);
        if (e && e->st == W1_ALIVE) send_spawn(w, e), w->st.req_served++;
    }
    return 1;
}

static int rx_event(w1_t *w, const uint8_t *body, unsigned len)
{
    RB r = { body, len, 0, 0 };
    unsigned ep = rb_u8(&r), seq = rb_u16(&r), sub = rb_u8(&r);
    if (r.err) { w->st.bad++; return 0; }
    if (!w->epoch_valid || ep != w->epoch) { w->st.stale_epoch++; return 0; }
    unsigned d = (uint16_t)(seq - w->rx_lo);
    if (d == 0 || d >= 0x8000u) { w->st.dup++, w->ack_dirty = 1; return 1; }
    if (d > W1_WINDOW) { w->st.ahead++; return 0; }   /* too far ahead for the window: not acked, the sender repeats it later */
    if ((w->rx_bits >> (d - 1)) & 1u) { w->st.dup++, w->ack_dirty = 1; return 1; }
    int ok;
    switch (sub) {
    case CP_W1_SPAWN: ok = rx_spawn(w, &r); break;
    case CP_W1_HIT: ok = rx_hit(w, &r); break;
    case CP_W1_DEATH: ok = rx_death(w, &r); break;
    case CP_W1_DESPAWN: ok = rx_despawn(w, &r); break;
    case CP_W1_PICKUP: ok = rx_pickup(w, &r); break;
    case CP_W1_DOOR: ok = rx_door(w, &r); break;
    case CP_W1_FLAG: ok = rx_flag(w, &r); break;
    case CP_W1_REQ: ok = rx_req(w, &r); break;
    case CP_W1_PCHIT: ok = rx_pchit(w, &r); break;
    default: ok = sub >= CP_W1_EXT_FIRST && sub <= CP_W1_EXT_LAST ? rx_ext(w, sub, &r) : 0; break;   /* unknown sub type of a newer peer: acknowledged (so it is not repeated), otherwise ignored */
    }
    if (!ok && ((sub >= CP_W1_SPAWN && sub <= CP_W1_REQ) || sub == CP_W1_PCHIT)) w->st.bad++;
    w->rx_bits |= 1u << (d - 1);
    while (w->rx_bits & 1u) w->rx_lo++, w->rx_bits >>= 1;
    w->ack_dirty = 1;
    w->st.rx_ev++;
    return 1;
}

/* ---------------------------------------------------------------- receive: unreliable world lane */
static void rx_enemies(w1_t *w, RB *r)
{
    unsigned bseq = rb_u16(r), ack_lo = rb_u16(r);
    uint32_t bits = rb_u32(r);
    unsigned n = rb_u8(r);
    if (r->err || n > W1_BATCH_RECS) { w->st.bad++; return; }
    if (w->role != W1_CLIENT) { w->st.bad++; return; }
    tx_ack(w, ack_lo, bits);
    for (unsigned i = 0; i < n; i++) {
        w1_enemy_rec rec;
        memset(&rec, 0, sizeof rec);
        rec.key = (w1_key)rb_u16(r);
        for (int j = 0; j < 3; j++) rec.pos[j] = rb_f32(r);
        int yaw = (int16_t)rb_u16(r);
        rec.hp = rb_f32(r), rec.ai = (uint16_t)rb_u16(r), rec.e1c = rb_u32(r);
        if (r->err) { w->st.bad++; return; }
        rec.yaw = (float)yaw * 3.14159265f / 32767.0f;
        w1_enemy *e = e_find(w, rec.key);
        if (!e) { w->st.rec_unknown++; continue; }
        if (e->st != W1_ALIVE) continue;
        if (e->seen_rec && !newer16(bseq, e->rec_bseq)) { w->st.rec_old++; continue; }
        e->seen_rec = 1, e->rec_bseq = (uint16_t)bseq;
        memcpy(e->pos, rec.pos, sizeof e->pos);
        e->yaw = rec.yaw, e->hp = rec.hp, e->ai = rec.ai, e->e1c = rec.e1c;
        rec.cls = e->cls, rec.owner = e->owner, rec.spawn_flags = e->spawn_flags;
        w->st.rec_applied++;
        if (w->ops.update) w->ops.update(w->ops.user, &rec);
    }
}

static void rx_roster(w1_t *w, RB *r)
{
    unsigned rseq = rb_u16(r), ack_lo = rb_u16(r);
    uint32_t bits = rb_u32(r);
    unsigned n = rb_u8(r);
    if (r->err || n > W1_MAX_ENEMIES || w->role != W1_CLIENT) { w->st.bad++; return; }
    w1_key keys[W1_MAX_ENEMIES];
    for (unsigned i = 0; i < n; i++) keys[i] = (w1_key)rb_u16(r);
    if (r->err) { w->st.bad++; return; }
    tx_ack(w, ack_lo, bits);
    if (w->have_rseq && !newer16(rseq, w->last_rseq)) return;
    w->have_rseq = 1, w->last_rseq = (uint16_t)rseq;
    static const uint8_t bit[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    uint8_t in[8192];
    memset(in, 0, sizeof in);
    for (unsigned i = 0; i < n; i++) in[keys[i] >> 3] |= bit[keys[i] & 7];
    for (unsigned i = 0; i < w->ne; i++) {
        w1_enemy *e = &w->e[i];
        if (e->st != W1_ALIVE) continue;
        if (in[e->key >> 3] & bit[e->key & 7]) { e->miss = 0; continue; }
        if (++e->miss >= W1_ROSTER_MISS) {   /* the host has not listed it for a while: last-resort cleanup (its DESPAWN / DEATH never came) */
            e->st = W1_GONE, e->miss = 0xFF;
            w->st.roster_despawn++;
            if (w->ops.despawn) w->ops.despawn(w->ops.user, e->key);
        }
    }
    w1_key miss[16];
    unsigned nm = 0;
    for (unsigned i = 0; i < n && nm < 16; i++) if (!e_find(w, keys[i])) miss[nm++] = keys[i];
    w1_key want[16];
    unsigned nw = 0;
    for (unsigned i = 0; i < nm; i++)
        for (unsigned j = 0; j < w->n_prev_missing; j++) if (miss[i] == w->prev_missing[j]) { want[nw++] = miss[i]; break; }
    memcpy(w->prev_missing, miss, nm * sizeof(w1_key)), w->n_prev_missing = nm;
    if (nw && (!w->req_valid || (int32_t)(w->now - w->last_req_ms) >= (int32_t)W1_REQ_MS)) {   /* a live enemy of the host that we have not heard of for two rosters */
        uint8_t buf[40];
        WB b = { buf, 0, sizeof buf, 0 };
        wb_u8(&b, nw);
        for (unsigned i = 0; i < nw; i++) wb_u16(&b, want[i]);
        if (ev_push(w, CP_W1_REQ, buf, b.n)) w->st.req_sent++;
        w->last_req_ms = w->now, w->req_valid = 1;
    }
}

int w1_recv(w1_t *w, int lane, unsigned kind, const uint8_t *body, unsigned len)
{
    if (!body || !len) { w->st.bad++; return 0; }
    if (lane == W1_LANE_EVENT) return kind == CP_EV_W1 ? rx_event(w, body, len) : 0;
    if (lane != W1_LANE_WORLD) return 0;
    RB r = { body, len, 0, 0 };
    unsigned ep = rb_u8(&r);
    if (kind != CP_WK_W1_ENEMIES && kind != CP_WK_W1_ROSTER && kind != CP_WK_W1_ACK) return 0;
    if (!w->epoch_valid || ep != w->epoch) { w->st.stale_epoch++; return 0; }
    w->st.rx_world++;
    if (kind == CP_WK_W1_ENEMIES) rx_enemies(w, &r);
    else if (kind == CP_WK_W1_ROSTER) rx_roster(w, &r);
    else {
        unsigned ack_lo = rb_u16(&r);
        uint32_t bits = rb_u32(&r);
        if (r.err) { w->st.bad++; return 0; }
        tx_ack(w, ack_lo, bits);
    }
    return 1;
}

/* ---------------------------------------------------------------- timers */
static void host_batch(w1_t *w)
{
    uint8_t buf[CP_WORLD_MAX];
    WB b = { buf, 0, sizeof buf, 0 };
    unsigned picked[W1_BATCH_RECS], np = 0;
    unsigned start = w->ne ? w->cursor % w->ne : 0, last = start;
    for (unsigned k = 0; k < w->ne && np < W1_BATCH_RECS; k++) {
        unsigned i = (start + k) % w->ne;
        w1_enemy *e = &w->e[i];
        if (e->st != W1_ALIVE) continue;
        if (!e->dirty && (int32_t)(w->now - e->sent_ms) < 1000) continue;
        picked[np++] = i, last = i;
    }
    if (!np) return;
    wb_u8(&b, w->epoch), wb_u16(&b, ++w->bseq), wb_u16(&b, w->rx_lo), wb_u32(&b, w->rx_bits), wb_u8(&b, np);
    for (unsigned k = 0; k < np; k++) {
        w1_enemy *e = &w->e[picked[k]];
        wb_u16(&b, e->key);
        for (int j = 0; j < 3; j++) wb_f32(&b, e->pos[j]);
        float y = e->yaw;
        if (y > 3.14159265f) y = 3.14159265f;
        if (y < -3.14159265f) y = -3.14159265f;
        wb_u16(&b, (uint16_t)(int16_t)lrintf(y * 32767.0f / 3.14159265f));
        wb_f32(&b, e->hp), wb_u16(&b, e->ai), wb_u32(&b, e->e1c);
        memcpy(e->lpos, e->pos, sizeof e->lpos), e->lhp = e->hp, e->lyaw = e->yaw, e->lai = e->ai, e->le1c = e->e1c, e->dirty = 0, e->sent_ms = w->now;
    }
    w->cursor = last + 1;
    out(w, W1_LANE_WORLD, CP_WK_W1_ENEMIES, buf, b.n);
    w->ack_dirty = 0, w->last_ack_ms = w->now;   /* the batch carries the ack */
}

static void host_roster(w1_t *w)
{
    uint8_t buf[CP_WORLD_MAX];
    WB b = { buf, 0, sizeof buf, 0 };
    uint16_t keys[W1_MAX_ENEMIES];
    unsigned n = 0;
    for (unsigned i = 0; i < w->ne; i++) if (w->e[i].st == W1_ALIVE) keys[n++] = w->e[i].key;
    wb_u8(&b, w->epoch), wb_u16(&b, ++w->rseq), wb_u16(&b, w->rx_lo), wb_u32(&b, w->rx_bits), wb_u8(&b, n);
    for (unsigned i = 0; i < n; i++) wb_u16(&b, keys[i]);
    out(w, W1_LANE_WORLD, CP_WK_W1_ROSTER, buf, b.n);
    w->ack_dirty = 0, w->last_ack_ms = w->now;
}

void w1_tick(w1_t *w, uint32_t now)
{
    w->now = now;
    if (!w->epoch_valid) return;
    for (int i = 0; i < W1_QMAX; i++) {
        w1_txev *e = &w->q[i];
        if (!e->used || !in_window(w, e->seq)) continue;
        if (!e->sent) ev_send_one(w, e, 0);
        else if ((int32_t)(now - e->last_ms) >= (int32_t)W1_RTO_MS) ev_send_one(w, e, 1);
    }
    if (w->role == W1_HOST) {
        if ((int32_t)(now - w->last_batch_ms) >= (int32_t)W1_BATCH_MS) w->last_batch_ms = now, host_batch(w);
        if ((int32_t)(now - w->last_roster_ms) >= (int32_t)W1_ROSTER_MS) w->last_roster_ms = now, host_roster(w);
    }
    int32_t since = (int32_t)(now - w->last_ack_ms);
    if ((w->ack_dirty && since >= (int32_t)W1_ACK_MS) || since >= (int32_t)W1_ACK_KEEP_MS) send_ack(w);
}

/* ---------------------------------------------------------------- inspection */
int w1_enemy_state(const w1_t *w, w1_key key)
{
    const w1_enemy *e = e_findc(w, key);
    return e ? e->st : W1_UNKNOWN;
}

int w1_enemy_get(const w1_t *w, w1_key key, w1_enemy_rec *out_rec, int *killer)
{
    const w1_enemy *e = e_findc(w, key);
    if (!e) return 0;
    if (out_rec) e_fill_rec(e, out_rec);
    if (killer) *killer = e->killer == W1_NO_SLOT ? -1 : (int)e->killer;
    return 1;
}

unsigned w1_count(const w1_t *w, int est)
{
    unsigned n = 0;
    for (unsigned i = 0; i < w->ne; i++) n += w->e[i].st == est;
    return n;
}

uint32_t w1_kills(const w1_t *w, int slot) { return slot_ok((unsigned)slot) ? w->kills[slot] : 0; }

int w1_item_is_gone(const w1_t *w, const w1_key4 *key, int *taker)
{
    for (unsigned i = 0; i < w->ni; i++)
        if (key4_eq(&w->it[i].key, key)) { if (taker) *taker = w->it[i].taker; return 1; }
    return 0;
}

int w1_door_get(const w1_t *w, const w1_key4 *key, uint32_t *vtbl, unsigned *phase, unsigned *ver)
{
    for (unsigned i = 0; i < w->nd; i++)
        if (key4_eq(&w->dr[i].key, key)) {
            if (vtbl) *vtbl = w->dr[i].vtbl;
            if (phase) *phase = w->dr[i].phase;
            if (ver) *ver = w->dr[i].ver;
            return 1;
        }
    return 0;
}

int w1_flag_get(const w1_t *w, uint32_t id, int32_t *value, unsigned *ver)
{
    for (unsigned i = 0; i < w->nf; i++)
        if (w->fl[i].id == id) {
            if (value) *value = w->fl[i].value;
            if (ver) *ver = w->fl[i].ver;
            return 1;
        }
    return 0;
}

const w1_stats *w1_get_stats(const w1_t *w) { return &w->st; }

void w1_summary(const w1_t *w, char *buf, size_t size)
{
    snprintf(buf, size, "world1 %s slot %d epoch %d: enemies alive %u dead %u gone %u, kills %u/%u, items gone %u, doors %u, flags %u, pending %u; tx ev %u (+%u retx) world %u, rx ev %u dup %u stale %u bad %u, hits %u late %u, rec %u old %u, conflicts %u",
             w->role == W1_HOST ? "host" : "client", w->own_slot, w->epoch_valid ? w->epoch : -1, w1_count(w, W1_ALIVE), w1_count(w, W1_DEAD), w1_count(w, W1_GONE), w->kills[0], w->kills[1], w->ni, w->nd, w->nf,
             w1_pending(w), w->st.tx_ev, w->st.retx, w->st.tx_world, w->st.rx_ev, w->st.dup, w->st.stale_epoch, w->st.bad, w->st.hits_applied, w->st.late_hits, w->st.rec_applied, w->st.rec_old, w->st.pickup_conflict);
}
