/*
 * watch1.h - rev1 live round 13, task 3: the watching PC shows the enemies around the human it watches. Pure (no Windows, no engine): watch1_game.inc
 * (inside world1_game.c) feeds it, tests/test_watch1.c drives it on a model of both PCs in both role orders.
 *
 * FACTS (notes/ep1-flow.md "RESUME HERE (round 13"):
 *  - FUN_004592E0 (called by FUN_00460250 in EVERY frame) clears E1C 0x1000 of the units in the OWN human's zone and sets it (hidden / asleep) on every other
 *    unit that has a zone and is not hidden yet (zombies 0x80031000/10/40/60 only while their vf 0x194 says "not awake"); a unit away from the own human is
 *    therefore hidden again in every frame, whatever was written in between.
 *  - In a solo part the watching PC's own human is switched off far away (FsmSetEnablePartner): the enemies around the WATCHED human are hidden there
 *    (l12a / l12e R7 host: the key zombies 0/0, 0/1 E1C 4000100c / 40001008; l12b lead order: the guest's replicas kind 3, hidden), so the watcher drew the
 *    room (stage1.h, r29) but no enemy.
 *  - R7: those enemies run on the CLIENT's AI (local1.h rule 3) and the client sent nothing about them: the host's units stood hidden where they were made.
 *  - HUD minimap (minimap1 notes): uGUI_Map_vf9 places the OWN human (FUN_0040EEF0 = the unit whose +0xE2C is the own slot FUN_0087E1B0); the manager's
 *    update slots 8 / 9 run every child's update, so a scope of the own slot around them makes the map follow another human (split.c does it for the second set).
 * THE RULE:
 *  1. the client sends the records of its LOCAL enemies (key, position, hp) every WT_TX_MS while they are alive (ext CP_W1_RLOC: a sequence number and up to
 *     WT_PER_MSG records; an older sequence is dropped on arrival);
 *  2. the host keeps a REVERSE REPLICA of such a key while its records are fresh (WT_STALE_MS) and its own human is not active near it (watching, or farther than
 *     WT_TAKEBACK): kind 3 with the client's slot as owner (the engine's kind rule: lr_set_kind), position / hp from the records; its own batch says "no unit
 *     here" for that key (ai LR_AI_NOUNIT), so the client keeps it (local1.h rule 3: no hand-back loop that would wake the host's AI next to the puppet); the
 *     client's death report kills it there (local1.h rule 4, decided as for a hidden unit);
 *  3. when the host's own human is active and near (WT_TAKEBACK), or the records stop with the unit alive here, the host takes it back: own AI (kind 2), its batch
 *     carries the real ai again and the client hands it back after LR_BACK_MS with its hp (local1.h rule 5);
 *  4. the WATCHING PC keeps every replica (kind 3) within WT_NEAR of the watched human visible: right after its zone pass the hidden bit is cleared again (so the
 *     lead order's guest sees the host's enemies, R7's host the client's); a PC that does not watch changes nothing;
 *  5. the watcher's HUD minimap follows the watched human: its update runs with the own slot answering the watched human's slot (with minimap1's partner marker
 *     the watching PC's own human becomes the partner marker there).
 * 0.2.1 (fix021, notes/fix021.md) rules 2 / 3 for an ACTIVE host human (wt_host_step_zone; watching keeps wt_host_step):
 *  2z. the host keeps a reverse replica of a key the client runs while the client's records are fresh and the host's unit is ASLEEP (E1C 0x1000: its own
 *      zone pass, outside the host human's zone) - no distance gate (0.2.0: only farther than WT_TAKEBACK, so a host human in the next zone left the unit
 *      frozen where its zone pass had reset it, with its old hp: fr1b);
 *  3z. the host takes it back (own AI, from the mirrored position and hp) on the pass that sees it AWAKE (in the host human's zone); then no new reverse
 *      replica of the key for WT_REHOLD_MS (a zone border does not flap);
 *  6.  the host's own blows on the key while the client drives it and until the client's hand-back report are summed (hd): a record's hp is written as
 *      hp - hd, the hand-back's hp is min(host hp, client hp - hd), never below 1 by a report (no kill, no resurrection), then hd is settled.
 */
#ifndef WATCH1_H
#define WATCH1_H

#include <stdint.h>
#include <string.h>

#include "local1.h"

#define WT_MAX 32
#define WT_TX_MS 125u           /* the client's record rate (8 Hz) */
#define WT_PER_MSG 4u
#define WT_REC_LEN 18u          /* u16 key, f32 x, y, z, f32 hp */
#define WT_HDR_LEN 3u           /* u8 count, u16 sequence */
#define WT_MSG_MAX (WT_HDR_LEN + WT_PER_MSG * WT_REC_LEN)   /* 75 <= W1_EXT_MAX (92) */
#define WT_STALE_MS 1500u       /* no record for this long: the client does not run it any more */
#define WT_NEAR 3000.0f         /* the watcher shows replicas this close to the watched human */
#define WT_TAKEBACK 1500.0f     /* the host's active own human this close takes the enemy back (0.2.0 rule 3; 0.2.1: watching only) */
#define WT_REHOLD_MS 3000u      /* 0.2.1 rule 3z: after a take-back no new reverse replica of the key before this */

typedef struct {
    uint16_t key;
    float pos[3], hp;
} wt_rec;

/* ---- the client's sender */
typedef struct {
    uint16_t seq;
    uint32_t last_ms;
    int started;
    unsigned msgs, recs;
} wt_tx;

/* 1 = a send round is due now (the first call is due at once) */
static inline int wt_tx_due(wt_tx *t, uint32_t now)
{
    if (t->started && (uint32_t)(now - t->last_ms) < WT_TX_MS) return 0;
    t->started = 1, t->last_ms = now;
    return 1;
}

/* the client's own enemy may be sent: local (its AI here), not reported dead, alive */
static inline int wt_tx_wants(int local, int reported, int alive) { return local && !reported && alive; }

static inline unsigned wt_enc(uint8_t *b, uint16_t seq, const wt_rec *r, unsigned n)
{
    if (n > WT_PER_MSG) n = WT_PER_MSG;
    b[0] = (uint8_t)n, b[1] = (uint8_t)(seq & 0xFF), b[2] = (uint8_t)(seq >> 8);
    for (unsigned i = 0; i < n; i++) {
        uint8_t *p = b + WT_HDR_LEN + i * WT_REC_LEN;
        p[0] = (uint8_t)(r[i].key & 0xFF), p[1] = (uint8_t)(r[i].key >> 8);
        memcpy(p + 2, r[i].pos, 12);
        memcpy(p + 14, &r[i].hp, 4);
    }
    return WT_HDR_LEN + n * WT_REC_LEN;
}

static inline int wt_dec(const uint8_t *b, unsigned len, uint16_t *seq, wt_rec *r, unsigned *n)
{
    if (len < WT_HDR_LEN || b[0] == 0 || b[0] > WT_PER_MSG || len < WT_HDR_LEN + b[0] * WT_REC_LEN) return 0;
    *n = b[0], *seq = (uint16_t)(b[1] | (b[2] << 8));
    for (unsigned i = 0; i < *n; i++) {
        const uint8_t *p = b + WT_HDR_LEN + i * WT_REC_LEN;
        r[i].key = (uint16_t)(p[0] | (p[1] << 8));
        memcpy(r[i].pos, p + 2, 12);
        memcpy(&r[i].hp, p + 14, 4);
    }
    return 1;
}

/* ---- the host's table of the client's local enemies */
typedef struct {
    uint16_t key;
    uint8_t used, active;
    uint32_t rx_ms;
    float pos[3], hp;
    uint8_t open;      /* 0.2.1 rule 6: the host's blows on the key are summed (from the reverse replica until the client's hand-back report) */
    float hd;          /* 0.2.1 rule 6: their sum */
    uint32_t back_ms;  /* 0.2.1 rule 3z: the last take-back */
} wt_ent;

typedef struct {
    wt_ent e[WT_MAX];
    uint16_t last_seq;
    int have_seq;
    unsigned rx_msgs, rx_old, rx_bad, rx_recs, made, taken_back, ended, full;
} wt_host;

static inline void wt_host_reset(wt_host *h) { memset(h, 0, sizeof *h); }

static inline wt_ent *wt_find(wt_host *h, uint16_t key, int create)
{
    wt_ent *fr = 0;
    for (unsigned i = 0; i < WT_MAX; i++) {
        if (h->e[i].used && h->e[i].key == key) return &h->e[i];
        if (!h->e[i].used && !fr) fr = &h->e[i];
    }
    if (!create || !fr) return 0;
    memset(fr, 0, sizeof *fr);
    fr->used = 1, fr->key = key;
    return fr;
}

/* a message of the client: 1 = taken (records stored), 0 = bad or older than the last one */
static inline int wt_rx(wt_host *h, const uint8_t *b, unsigned len, uint32_t now)
{
    wt_rec r[WT_PER_MSG];
    unsigned n = 0;
    uint16_t seq = 0;
    if (!wt_dec(b, len, &seq, r, &n)) {
        h->rx_bad++;
        return 0;
    }
    if (h->have_seq && (int16_t)(uint16_t)(seq - h->last_seq) <= 0) {
        h->rx_old++;
        return 0;
    }
    h->have_seq = 1, h->last_seq = seq, h->rx_msgs++;
    for (unsigned i = 0; i < n; i++) {
        wt_ent *e = wt_find(h, r[i].key, 1);
        if (!e) {
            h->full++;
            continue;
        }
        e->rx_ms = now ? now : 1;
        memcpy(e->pos, r[i].pos, sizeof e->pos);
        e->hp = r[i].hp;
        h->rx_recs++;
    }
    return 1;
}

static inline int wt_fresh(const wt_ent *e, uint32_t now) { return e->rx_ms && (uint32_t)(now - e->rx_ms) <= WT_STALE_MS; }

enum { WT_KEEP = 0, WT_MAKE_REPLICA = 1, WT_TAKE_BACK = 2 };

/* host, per pass, for a key of the table. unit_kind = the kind of the host's live unit (2 own AI, 3 replica; 0 = no live unit); own_active = the host's own
 * human is active (this PC does not watch); own_dist = from it to the enemy; view_dist = from the host's view point (the watched human while it watches, else
 * its own human) to the record's position: only there does the host stream the stage (m13c live: 0/2, 3500 u from the watched Jill, was made a replica and
 * placed where the host had no floor; the host's unit died there and its DEATH reached the client). Returns what the glue does to the unit. */
static inline int wt_host_step(wt_host *h, wt_ent *e, unsigned unit_kind, int own_active, float own_dist, float view_dist, uint32_t now)
{
    int want = wt_fresh(e, now) && !(own_active && own_dist <= WT_TAKEBACK) && view_dist <= WT_NEAR;
    if (want) {
        if (!e->active) e->active = 1, h->made++;
        return unit_kind && unit_kind != 3 ? WT_MAKE_REPLICA : WT_KEEP;
    }
    if (e->active) {
        e->active = 0, h->ended++;
        if (unit_kind == 3) {
            h->taken_back++;
            return WT_TAKE_BACK;
        }
    }
    return WT_KEEP;
}

/* 0.2.1 rules 2z / 3z (an active host human; watching: wt_host_step). host_hidden = the host's unit has E1C 0x1000 now. */
static inline int wt_host_step_zone(wt_host *h, wt_ent *e, unsigned unit_kind, int own_active, float own_dist, float view_dist, int host_hidden, uint32_t now)
{
    if (!own_active) return wt_host_step(h, e, unit_kind, own_active, own_dist, view_dist, now);
    int hold = e->back_ms && (uint32_t)(now - e->back_ms) < WT_REHOLD_MS;
    int want = wt_fresh(e, now) && view_dist <= WT_NEAR && unit_kind && host_hidden && (e->active || !hold);
    if (want) {
        if (!e->active) e->active = 1, e->open = 1, e->hd = 0.0f, h->made++;
        return unit_kind != 3 ? WT_MAKE_REPLICA : WT_KEEP;
    }
    if (e->active) {
        e->active = 0, h->ended++;
        if (unit_kind == 3) {
            h->taken_back++, e->back_ms = now ? now : 1;
            return WT_TAKE_BACK;
        }
    }
    return WT_KEEP;
}

/* 0.2.1 rule 6: a blow of the host's own human on the key (dealt = the hp it took here) */
static inline void wt_host_blow(wt_host *h, uint16_t key, float dealt)
{
    wt_ent *e = wt_find(h, key, 0);
    if (e && e->open && dealt > 0.0f) e->hd += dealt;
}

/* 0.2.1 rule 6: the hp a record gives the reverse replica */
static inline float wt_mirror_hp(float rec_hp, float hd)
{
    float v = rec_hp - (hd > 0.0f ? hd : 0.0f);
    return v < 1.0f ? 1.0f : v;
}

/* 0.2.1 rule 6: the client's hand-back hp as the host's value (the host's own blows since the client drove it taken off); 0.2.0 = the client's hp */
static inline float wt_handback_eff(const wt_host *h, uint16_t key, float client_hp)
{
    for (unsigned i = 0; i < WT_MAX; i++)
        if (h->e[i].used && h->e[i].key == key && h->e[i].open) {
            float v = client_hp - (h->e[i].hd > 0.0f ? h->e[i].hd : 0.0f);
            return v < 1.0f ? 1.0f : v;
        }
    return client_hp;
}

/* 0.2.1 rule 6: the hand-back report arrived (or the enemy died): the sum is settled */
static inline void wt_settle(wt_host *h, uint16_t key)
{
    wt_ent *e = wt_find(h, key, 0);
    if (e) e->open = 0, e->hd = 0.0f;
}

/* is the key a reverse replica on the host now */
static inline int wt_reverse(const wt_host *h, uint16_t key)
{
    for (unsigned i = 0; i < WT_MAX; i++)
        if (h->e[i].used && h->e[i].key == key) return h->e[i].active;
    return 0;
}

/* the ai the host's batch carries for a key (rule 2: "no unit here" for a reverse replica, so the client keeps it) */
static inline unsigned wt_batch_ai(const wt_host *h, uint16_t key, unsigned ai) { return wt_reverse(h, key) ? LR_AI_NOUNIT : ai; }

/* the host decides a death report of the client as for a hidden unit when the key is a reverse replica (rule 2) */
static inline int wt_report_hidden(const wt_host *h, uint16_t key, int hidden) { return hidden || wt_reverse(h, key); }

/* rule 4: keep this unit visible on this PC right after its zone pass */
static inline int wt_keep_visible(int on, int watching, unsigned kind, int alive, float dist_watched)
{
    return on && watching && kind == 3 && alive && dist_watched <= WT_NEAR;
}

/* rule 6 (m13b live: the watching guest took 0/2 at 2023 u from its SWITCHED-OFF Parker as its own AI, and the host made it a reverse replica): a PC whose
 * own human is switched off by the story (it watches) takes no enemy for its own AI; such an enemy sleeps on the host as in the stock solo part. The distance
 * local1.h's take rule sees: */
static inline float wt_local_dist(int watching, float dist) { return watching ? 1e9f : dist; }

/* rule 5: the slot the HUD minimap's update must see as the own slot: the watched human's while this PC watches, else -1 (no scope) */
static inline int wt_map_scope(int on, int watching, int own_slot) { return on && watching && (own_slot == 0 || own_slot == 1) ? 1 - own_slot : -1; }

#endif
