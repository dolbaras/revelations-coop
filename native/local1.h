/*
 * local1.h - rev1 live round 11: the enemies the host does not simulate. Pure decisions (no Windows, no engine): world1_game.c feeds them per pass,
 * tests/test_local1.c drives them through the l10e timeline in both role orders.
 *
 * FACTS (notes/ep1-flow.md "RESUME HERE (round 11"):
 *  - l10e (R7: host = Parker, guest = Jill in the 1-3 solo part): the host's story placed the 13_048_0 zombies too (host SpawnChara 0/0, 0/1 at 20:10:59.542,
 *    10 ms after the guest's [8] report was counted), its model had them ALIVE (so it had units), but later its entries had NO unit; the guest's engine
 *    re-created 0/1 (new unit 24e53490 at 13_048_0) without a SpawnChara call, and its 0/0 ran as kind 2 (its own AI). The host's screen showed Jill's puppet
 *    in a void (the room around her is not drawn on the host). FUN_00456f20 clears an entry's unit when the unit's state (+0xC & 7) is not 1 / 2 (killed);
 *    FUN_004592e0 hides (E1C |= 0x1000) every unit outside the OWN human's zone; FUN_00464320 (ArgCharaRearrange) re-creates a missing unit of a record that
 *    is not used up, with the record's owner (no SpawnChara, so no replica owner).
 *  - The host's batch resends every model-ALIVE enemy at least once a second (world1.c host_batch), also a record whose unit is gone (the last values).
 *  So "the host owns every enemy" cannot hold where only the client's human is. THE RULE (every outcome the same on both PCs):
 *   1. the host simulates an enemy when it has a live unit that is not hidden (E1C 0x1000); its record then carries the real AI number; a record of an
 *      enemy the host keeps without a unit carries ai = LR_AI_NOUNIT (0xFFFF) (host_pass);
 *   2. the client shows a replica (kind 3) of an enemy the host simulates (a unit its engine re-created as own AI is turned into a replica);
 *   3. an enemy the host does not simulate (records with ai 0xFFFF or E1C 0x1000, or no record for LR_SILENT_MS) while the client's live unit is within
 *      LR_NEAR of the client's human, for LR_TAKE_MS without a break, runs on the client's own AI (kind 2): "local";
 *   4. the client reports a local enemy's death once (CP_W1_LOCAL, LR_REC_DEATH): the host marks its record used up (entry +0xD8), so its story's
 *      dead check (ArgCheckCharaState mCheckState 2 = record used up / dead bit) agrees, kills a hidden unit it may still have, and its model sends DEATH
 *      (the client's credit is its own kill record; the DEATH changes nothing there);
 *   5. when the host simulates it again for LR_BACK_MS, the client's unit becomes a replica and the client sends its hp (LR_REC_HP): the host takes the lower hp.
 */
#ifndef LOCAL1_H
#define LOCAL1_H

#include <stdint.h>
#include <string.h>

#define LR_MAX 64
#define LR_AI_NOUNIT 0xFFFFu        /* the record's ai of an enemy the host keeps without a unit */
#define LR_E1C_HIDDEN 0x1000u        /* unit +0xE1C: hidden / asleep (FUN_004592e0 outside the own human's zone) */
#define LR_SILENT_MS 3000u           /* no record at all for this long = the host does not simulate it */
#define LR_TAKE_MS 2500u             /* "not simulated by the host" must hold this long before the client takes it */
#define LR_BACK_MS 1000u             /* "simulated by the host" must hold this long before the client hands it back */
#define LR_FRESH_MS 2500u            /* a record younger than this counts (the host refreshes every ALIVE record at least once a second) */
#define LR_NEAR 3000.0f              /* the client takes only enemies near its own human */

enum lr_act { LR_NONE = 0, LR_MAKE_REPLICA, LR_MAKE_LOCAL, LR_REPORT_DEATH, LR_HAND_BACK };
enum lr_rec { LR_REC_DEATH = 1, LR_REC_HP = 2 };

typedef struct {
    uint16_t key;
    uint8_t used, local, reported, have_rec, host_sim;
    uint32_t unit, last_rec_ms, cond_since_ms;   /* cond_since: since when the current wish (take / give back) holds; 0 = none */
    float last_hp;
} lr_ent;

typedef struct {
    lr_ent e[LR_MAX];
    unsigned took, gave_back, reports, made_replica;
    unsigned dead_refused;   /* rev1 live 14: replicas not taken because the host's DEATH came for them */
    int no_dead_guard;       /* rev1 live 14 LAB (`world set deadguard 0`): 1 = the rule before the fix (a replica the host's model killed may still be taken) */
} lr_t;

static inline void lr_reset(lr_t *t) { memset(t, 0, sizeof *t); }

static inline lr_ent *lr_find(lr_t *t, uint16_t key, int create)
{
    lr_ent *free_e = 0;
    for (unsigned i = 0; i < LR_MAX; i++) {
        if (t->e[i].used && t->e[i].key == key) return &t->e[i];
        if (!t->e[i].used && !free_e) free_e = &t->e[i];
    }
    if (!create || !free_e) return 0;
    memset(free_e, 0, sizeof *free_e);
    free_e->used = 1, free_e->key = key;
    return free_e;
}

/* rev1 live 12 (l12c / l12b, FACT): the record's ai is the unit's state machine number (machine +0x3C, u16 on the wire); a live, awake host unit can read
 * 0xFFFF there (the lead-order Ooze 2/0 in 1-1, the key zombie 0/0 in 1-3: `record NOT simulated by the host` while the host ran it), which is the
 * "no unit here" sentinel: the guest took the host's Ooze 156 u from its Parker (`runs on THIS PC's AI now`, its shots then hit its own copy: `client blows
 * sent 0`). The host therefore never sends the sentinel for a real unit: lr_host_ai maps a real 0xFFFF to 0xFFFE. */
static inline unsigned lr_host_ai(uint32_t raw)
{
    unsigned a = (unsigned)(raw & 0xFFFFu);
    return a == LR_AI_NOUNIT ? LR_AI_NOUNIT - 1u : a;
}

/* is the enemy simulated by the host, from a record (rule 1) */
static inline int lr_record_sim(unsigned ai, uint32_t e1c) { return ai != LR_AI_NOUNIT && !(e1c & LR_E1C_HIDDEN); }

/* client: a host record of the key arrived (op_update, also when there is no unit here) */
static inline void lr_on_record(lr_t *t, uint16_t key, unsigned ai, uint32_t e1c, uint32_t now)
{
    lr_ent *e = lr_find(t, key, 1);
    if (!e) return;
    e->have_rec = 1, e->last_rec_ms = now, e->host_sim = (uint8_t)lr_record_sim(ai, e1c);
}

/* client: the host's model says the enemy died (DEATH): nothing is reported for it any more */
static inline void lr_on_death(lr_t *t, uint16_t key)
{
    lr_ent *e = lr_find(t, key, 0);
    if (e) e->reported = 1;
}

static inline int lr_host_simulates(const lr_ent *e, uint32_t now)
{
    return e->have_rec && (uint32_t)(now - e->last_rec_ms) <= LR_FRESH_MS && e->host_sim;
}

/* client, per pass, for an enemy record that has a unit here. kind = the unit's control kind (2 own AI, 3 replica); alive = hp > 0 and state 1 / 2;
 * dist = from the client's own human. Returns what the glue does (the glue changes the kind / sends the report and then calls nothing else). */
static inline int lr_step(lr_t *t, uint16_t key, uint32_t unit, unsigned kind, int alive, float hp, float dist, uint32_t now)
{
    lr_ent *e = lr_find(t, key, 1);
    if (!e || !unit) return LR_NONE;
    if (e->unit != unit) {   /* a new unit for the record (a rearrange, a reload): its own history */
        int was_local = e->local;
        e->unit = unit, e->cond_since_ms = 0, e->local = kind == 2 && was_local;
    }
    if (!alive) {
        if (e->local && !e->reported) {
            e->reported = 1, t->reports++;
            return LR_REPORT_DEATH;
        }
        return LR_NONE;
    }
    e->last_hp = hp;
    int sim = lr_host_simulates(e, now);
    int silent = !e->have_rec || (uint32_t)(now - e->last_rec_ms) > LR_SILENT_MS;
    int wish_local = !sim && (silent || (e->have_rec && !e->host_sim)) && dist <= LR_NEAR;
    if (kind == 2 && !e->local) {   /* own AI made by the engine itself (a rearrange): a replica as soon as the host simulates it */
        if (sim) {
            e->cond_since_ms = 0, t->made_replica++;
            return LR_MAKE_REPLICA;
        }
        if (!wish_local) return LR_NONE;   /* far away: left as the engine made it (its zone logic hides it) */
        if (!e->cond_since_ms) e->cond_since_ms = now ? now : 1;
        if ((uint32_t)(now - e->cond_since_ms) >= LR_TAKE_MS) {
            e->local = 1, e->cond_since_ms = 0, t->took++;
            return LR_NONE;   /* stays own AI: now counted as local (reported when it dies) */
        }
        return LR_NONE;
    }
    if (kind == 3) {   /* a replica */
        if (e->reported && !t->no_dead_guard) {   /* rev1 live 14 (m13f): the host's DEATH came for it (lr_on_death): never taken, whatever its hp says here (the
                                                      later rounds of a burst put a replica back to hp 1; local1 then took the hp-1 Ooze and it bit the guest's Parker) */
            if (e->cond_since_ms) e->cond_since_ms = 0;
            t->dead_refused++;
            return LR_NONE;
        }
        if (!wish_local) {
            e->cond_since_ms = 0;
            return LR_NONE;
        }
        if (!e->cond_since_ms) e->cond_since_ms = now ? now : 1;
        if ((uint32_t)(now - e->cond_since_ms) >= LR_TAKE_MS) {
            e->local = 1, e->cond_since_ms = 0, t->took++;
            return LR_MAKE_LOCAL;
        }
        return LR_NONE;
    }
    if (kind == 2 && e->local) {   /* local: back to the host when it simulates it again */
        if (!sim) {
            e->cond_since_ms = 0;
            return LR_NONE;
        }
        if (!e->cond_since_ms) e->cond_since_ms = now ? now : 1;
        if ((uint32_t)(now - e->cond_since_ms) >= LR_BACK_MS) {
            e->local = 0, e->cond_since_ms = 0, t->gave_back++;
            return LR_HAND_BACK;   /* the glue makes it a replica and sends LR_REC_HP */
        }
    }
    return LR_NONE;
}

/* client: may the host's DEATH / kill run on this unit? (a local enemy's corpse is the client's own; op_kill leaves it) */
static inline int lr_kill_allowed(lr_t *t, uint16_t key)
{
    lr_ent *e = lr_find(t, key, 0);
    return !(e && (e->local || e->reported));
}

/* ---- the report (ext CP_W1_LOCAL): u8 type, u16 key, f32 hp, u8 killer slot (0xFF = none) */
#define LR_MSG_LEN 8
static inline unsigned lr_enc(uint8_t *b, unsigned type, uint16_t key, float hp, unsigned killer)
{
    b[0] = (uint8_t)type, b[1] = (uint8_t)(key & 0xFF), b[2] = (uint8_t)(key >> 8);
    memcpy(b + 3, &hp, 4);
    b[7] = (uint8_t)killer;
    return LR_MSG_LEN;
}
static inline int lr_dec(const uint8_t *b, unsigned len, unsigned *type, uint16_t *key, float *hp, unsigned *killer)
{
    if (len < LR_MSG_LEN || (b[0] != LR_REC_DEATH && b[0] != LR_REC_HP)) return 0;
    *type = b[0], *key = (uint16_t)(b[1] | (b[2] << 8));
    memcpy(hp, b + 3, 4);
    *killer = b[7];
    return 1;
}

/* host: what a report does. has_unit = the host's record has a unit; hidden = that unit is hidden (E1C 0x1000); spent = record used up already.
 * DEATH: never against a unit the host simulates (that enemy is the host's: refused); otherwise the record is marked used up (and a hidden unit killed).
 * HP: only lowers the hp of a unit the host has (the client fought it while the host did not simulate it). */
enum { LR_H_REFUSE = 0, LR_H_SPEND = 1, LR_H_SPEND_KILL = 2, LR_H_SET_HP = 3, LR_H_NOTHING = 4 };
static inline int lr_host_decide(unsigned type, int has_unit, int hidden, int spent, float host_hp, float hp)
{
    if (type == LR_REC_DEATH) {
        if (spent) return LR_H_NOTHING;
        if (!has_unit) return LR_H_SPEND;
        return hidden ? LR_H_SPEND_KILL : LR_H_REFUSE;
    }
    if (type == LR_REC_HP) return has_unit && hp > 0.0f && hp < host_hp ? LR_H_SET_HP : LR_H_NOTHING;
    return LR_H_REFUSE;
}

/* THE ENGINE'S KIND RULE (rev1 live 12; FACT: l11a + static): every state change of a non-player unit (FUN_0049CD20 -> FUN_0049CB70) sets its control kind
 * from its owner unit+0xE7C (FUN_006C9FD0): the engine's own slot (FUN_00882D60) -> 2 (own AI), any other slot -> 3 (remote). l11a: the client's 0/1 (record
 * owner 1 = the host's slot after the rearrange, so +0xE7C 1) was made own AI by the kind alone and became a replica again at its next state change (its bites
 * on Jill), every 3-15 s ("runs on THIS PC's AI now" eight times, 01:20:12-01:20:40). So a kind change goes with the owner, the engine's own way
 * (FUN_00459880: FUN_006C9FC0(owner), SetKind, the record's +0xD0 = owner, so a later rearrange re-creates the unit with it). */
static inline int lr_engine_kind(int unit_owner, int engine_own) { return unit_owner == engine_own ? 2 : 3; }
static inline int lr_owner_for(int kind, int engine_own, int replica_owner) { return kind == 2 ? engine_own : replica_owner; }

static inline const char *lr_act_name(int a)
{
    switch (a) {
    case LR_MAKE_REPLICA: return "make replica";
    case LR_MAKE_LOCAL: return "make local";
    case LR_REPORT_DEATH: return "report death";
    case LR_HAND_BACK: return "hand back";
    default: return "none";
    }
}

#endif
