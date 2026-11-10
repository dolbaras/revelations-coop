/*
 * zone1.h - round 9, PUPPET AUTHORITY (lead's decision, notes\ep1-flow.md round 9): each PC is authoritative over its OWN human only, and the story of a PC
 * never advances because of the partner's PUPPET. Pure logic (no Windows, no game); the glue is zone1_game.inc, the tests tests/test_zone1.c.
 *
 * Why (l8d, 2026-11-08 11:57:13): the host's story ran 13_060_0 (the END of Episode 1) because the guest's puppet (a pad replay mapped through the host's
 * camera; 68 correction teleports, errors up to 267 u) stood in cc_must00[9] while the guest's real Jill stood outside the tied man's door.
 *
 * The engine (FACT, static, notes\ep1-flow.md round 9): the FSM processes EventCollisionHitCheck 0x74BBF0 and EventCollisionArrivalCharaState 0x74BFA0 call
 * cEventCollision::HitCheck 0x6D5980(this, record) (return addresses 0x74BCB1 / 0x74C289). It clears every hitter slot of the object, tests the record
 * through 0x6D4750 / 0x6D34D0 (unit lists, class mask +0xBC, block gate, facing gate, swept shape) and writes up to 4 hitting units into the record's
 * slots ([this+0x18] + record * 8 -> 4 dwords); the FSM then takes the class bit of each hitter (0x80011000 -> 1, 0x80011010 Jill -> 2, 0x80011020 Parker
 * -> 4, ...). Gimmicks (doors, lifts) call the same function from other places: they are NOT touched here.
 *
 * The rule, after the stock test of a story record (record type +0xB8 bit 0 = player classes, +0xC8 = 0 "any"; the one "all" record of Episode 1,
 * 12_00_s200_obrevent[0], stays stock):
 *   - the partner's puppet is removed from the hitters (it never counts by its own position on this PC);
 *   - the own human's hit is reported to the partner's PC (CP_W1_ZONE: key = FNV-1a of the ECD resource path, record index, in / out): "in" on the first
 *     hit and again every Z1_REFRESH_MS while the own tests keep seeing it, "out" when a test sees it gone;
 *   - a report from the partner's PC counts for Z1_LEASE_MS after its last "in" (an "out" ends it): while it counts, the puppet is put into a free hitter
 *     slot of that record (it is "that slot's hit"), so the FSM sees the partner's class bit exactly as if his unit stood there.
 * Both PCs run the same code: the host's puppet on the guest is handled the same way (the reverse direction).
 */
#ifndef ZONE1_H
#define ZONE1_H

#include <stdint.h>
#include <string.h>

#define Z1_MAX 256u
#define Z1_REFRESH_MS 400u   /* the owner repeats "in" this often while its own tests see its human inside */
#define Z1_LEASE_MS 1500u    /* a partner's "in" counts this long after it arrived (a lost "out", an FSM that stopped testing on the owner's PC) */
#define Z1_WIRE_VER 1u

enum { Z1_OUT = 0, Z1_IN = 1 };
enum { Z1_P_NONE = 0, Z1_P_IN = 1, Z1_P_OUT = 2 };
/* decision bits of z1_after_test */
enum {
    Z1_D_REPORT = 1,        /* the partner's PC reports its human in this record now: count the puppet */
    Z1_D_DROP = 2,          /* the puppet hit it here but no report: it is removed */
    Z1_D_LOG_OWN = 0x10,    /* first own hit of this record (log once) */
    Z1_D_LOG_DROP = 0x20,   /* first dropped puppet hit of this record (log once) */
    Z1_D_LOG_REP = 0x40,    /* first counted report of this record (log once) */
};

typedef struct {
    uint32_t key;
    uint16_t idx;
    uint8_t own_in, pend, rem_on, logged;
    uint32_t own_sent_ms, seen_ms, rem_until;
} z1_ent;

typedef struct {
    uint32_t tests, own_hits, sent_in, sent_out, rx_in, rx_out, rx_bad, counted, dropped, evicted;
} z1_stats;

typedef struct {
    z1_ent e[Z1_MAX];
    unsigned n, rr;
    z1_stats st;
} z1_t;

static inline void z1_init(z1_t *z) { memset(z, 0, sizeof *z); }
/* a new level instance / the model unbound: nothing of the old level counts */
static inline void z1_reset(z1_t *z)
{
    z1_stats keep = z->st;
    memset(z, 0, sizeof *z);
    z->st = keep;
}

/* the key of an ECD resource: FNV-1a over its path (lower case, '/' as '\'), at most 64 bytes */
static inline uint32_t z1_key(const char *path, unsigned max)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < max && path[i]; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (c == '/') c = '\\';
        h = (h ^ c) * 16777619u;
    }
    return h ? h : 1u;
}

/* only "any of these player classes" records (type bit 0, all-mode 0) are mirrored */
static inline int z1_rec_ok(uint32_t type, uint32_t all) { return (type & 1u) && all == 0; }
/* the class bit of a player unit as 0x6D34D0 / 0x74BBF0 compute it (0 = not a player class) */
static inline uint32_t z1_class_bit(uint32_t cls) { return (cls & 0xF00FF000u) == 0x80011000u ? 1u << ((cls >> 4) & 0xFu) : 0u; }
/* the stock unit test: state 2 and +0xC bit 0x400 (in play), and the record's class mask */
static inline int z1_unit_counts(uint32_t c_flags, uint32_t cls, uint32_t mask) { return (c_flags & 7u) == 2u && (c_flags & 0x400u) && (z1_class_bit(cls) & mask & 0xFFFu); }

static inline z1_ent *z1_find(z1_t *z, uint32_t key, unsigned idx, int create, uint32_t now)
{
    for (unsigned i = 0; i < z->n; i++)
        if (z->e[i].key == key && z->e[i].idx == (uint16_t)idx) return &z->e[i];
    if (!create) return NULL;
    z1_ent *e;
    if (z->n < Z1_MAX) e = &z->e[z->n++];
    else {   /* full: the record tested longest ago gives its place (never one with a pending send or a live report) */
        e = NULL;
        for (unsigned i = 0; i < z->n; i++) {
            z1_ent *c = &z->e[i];
            if (c->pend || c->rem_on) continue;
            if (!e || (int32_t)(c->seen_ms - e->seen_ms) < 0) e = c;
        }
        if (!e) e = &z->e[0];
        z->st.evicted++;
    }
    memset(e, 0, sizeof *e);
    e->key = key, e->idx = (uint16_t)idx, e->seen_ms = now;
    return e;
}

/* after the stock test of record (key, idx) on this PC: own_hit / pup_hit = the own human / the partner's puppet among the stock hitters */
static inline int z1_after_test(z1_t *z, uint32_t key, unsigned idx, uint32_t now, int own_hit, int pup_hit)
{
    z1_ent *e = z1_find(z, key, idx, 1, now);
    int d = 0;
    z->st.tests++;
    e->seen_ms = now;
    if (own_hit) {
        z->st.own_hits++;
        if (!e->own_in || (uint32_t)(now - e->own_sent_ms) >= Z1_REFRESH_MS) e->pend = Z1_P_IN, e->own_sent_ms = now;
        if (!(e->logged & 1)) e->logged |= 1, d |= Z1_D_LOG_OWN;
        e->own_in = 1;
    } else if (e->own_in) {
        e->own_in = 0;
        e->pend = Z1_P_OUT;
    }
    if (e->rem_on && (int32_t)(e->rem_until - now) <= 0) e->rem_on = 0;   /* the lease is over */
    if (e->rem_on) {
        d |= Z1_D_REPORT;
        z->st.counted++;
        if (!(e->logged & 4)) e->logged |= 4, d |= Z1_D_LOG_REP;
    } else if (pup_hit) {
        d |= Z1_D_DROP;
        z->st.dropped++;
        if (!(e->logged & 2)) e->logged |= 2, d |= Z1_D_LOG_DROP;
    }
    return d;
}

/* the next report to send (round robin over the table); 1 = *key / *idx / *state are set */
static inline int z1_next_send(z1_t *z, uint32_t *key, unsigned *idx, unsigned *state)
{
    for (unsigned k = 0; k < z->n; k++) {
        z1_ent *e = &z->e[(z->rr + k) % z->n];
        if (!e->pend) continue;
        *key = e->key, *idx = e->idx, *state = e->pend == Z1_P_IN ? Z1_IN : Z1_OUT;
        if (e->pend == Z1_P_IN) z->st.sent_in++;
        else z->st.sent_out++;
        e->pend = Z1_P_NONE;
        z->rr = (z->rr + k + 1) % z->n;
        return 1;
    }
    return 0;
}

/* a report of the partner's PC arrived */
static inline void z1_remote(z1_t *z, uint32_t key, unsigned idx, unsigned state, uint32_t now)
{
    z1_ent *e = z1_find(z, key, idx, 1, now);
    if (state == Z1_IN) {
        e->rem_on = 1, e->rem_until = now + Z1_LEASE_MS;
        z->st.rx_in++;
    } else {
        e->rem_on = 0;
        z->st.rx_out++;
    }
}

/* the hitter slots of the record after the rule: the puppet removed; when `count` it is put into the first free slot (once). *changed = slots were written.
 * Returns 1 when any hitter is left (the record's result). pup 0 = no puppet on this PC. */
static inline int z1_slots(uint32_t s[4], uint32_t pup, int count, int *changed)
{
    int any = 0, placed = 0;
    *changed = 0;
    for (int j = 0; j < 4; j++) {
        if (pup && s[j] == pup) {
            if (count && !placed) placed = 1;   /* it stays: the report counts it in its place */
            else s[j] = 0, *changed = 1;
        }
    }
    if (count && pup && !placed)
        for (int j = 0; j < 4; j++)
            if (!s[j]) {
                s[j] = pup, placed = 1, *changed = 1;
                break;
            }
    for (int j = 0; j < 4; j++) any |= s[j] != 0;
    return any;
}

/* the whole rule for one stock test of a mirrored record (the glue and the tests call exactly this): s = the record's hitter slots after the stock test (edited in
 * place), r = the stock result (0 / 1), own / pup = the own human's unit and the partner's puppet unit on this PC (0 = none); pup_counts(u, pup) answers
 * whether the puppet would pass the stock unit test (in play, the record's class), asked only while a report counts. Returns the record's result after the
 * rule; *d = the decision bits (Z1_D_*), *changed = the slots were edited. */
static inline int z1_rule(z1_t *z, uint32_t key, unsigned idx, uint32_t now, uint32_t s[4], uint32_t own, uint32_t pup, int r, int (*pup_counts)(void *u, uint32_t pup), void *u,
                          int *d, int *changed)
{
    int own_hit = 0, pup_hit = 0;
    for (int j = 0; j < 4; j++) own_hit |= own && s[j] == own, pup_hit |= pup && s[j] == pup;
    *changed = 0;
    *d = z1_after_test(z, key, idx, now, own_hit, pup_hit);
    if (!(*d & (Z1_D_REPORT | Z1_D_DROP))) return r ? 1 : 0;
    int count = (*d & Z1_D_REPORT) && pup && pup_counts && pup_counts(u, pup);
    return z1_slots(s, pup, count, changed);
}

/* wire: u32 key, u16 record, u8 state, u8 version */
static inline unsigned z1_enc(uint8_t *b, unsigned cap, uint32_t key, unsigned idx, unsigned state)
{
    if (cap < 8) return 0;
    b[0] = (uint8_t)key, b[1] = (uint8_t)(key >> 8), b[2] = (uint8_t)(key >> 16), b[3] = (uint8_t)(key >> 24);
    b[4] = (uint8_t)idx, b[5] = (uint8_t)(idx >> 8), b[6] = (uint8_t)(state ? Z1_IN : Z1_OUT), b[7] = Z1_WIRE_VER;
    return 8;
}
static inline int z1_dec(const uint8_t *b, unsigned len, uint32_t *key, unsigned *idx, unsigned *state)
{
    if (len < 8 || b[7] != Z1_WIRE_VER || b[6] > 1) return 0;
    *key = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    *idx = (unsigned)b[4] | (unsigned)b[5] << 8;
    *state = b[6];
    return *key != 0;
}

#endif
