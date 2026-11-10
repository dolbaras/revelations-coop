/*
 * world1: the shared world of Resident Evil Revelations 1 (Episode 1) as a pure model: enemies, world items, doors, scenario flags.
 * No Windows, no game and no transport dependencies: the engine hooks (not written yet) call the w1_host_* / w1_client_* / w1_local_* functions, the
 * network glue calls w1_recv / w1_tick and sends what the model hands to ops.send, the engine side applies what the model decides through the other ops.
 * Design and evidence: notes\net-world-enemies-re.md, notes\net-items-doors-re.md; implementation notes: notes\net-world-impl.md.
 *
 * Rules (the model enforces them, the tests prove them):
 *  - the HOST is authoritative over enemies: it decides existence, hp, death and who gets the kill; the client only applies (replicas, view control kind 3);
 *  - an enemy is named by KEY = (block, index) of the sArrange record that SpawnChara (0x4688F0) placed, never by the unit serial +0xE2C (it differs per PC);
 *  - every message is idempotent: the per-key state only moves ALIVE -> DEAD -> GONE (a late SPAWN never resurrects what a DEATH / DESPAWN already ended,
 *    whatever the arrival order); duplicates change nothing; losses are repaired by retransmission until acknowledged and by the host's roster;
 *  - kill credit goes to the SLOT of the human who killed (HIT carries the attacker slot, DEATH the killer slot); each PC credits only its own human;
 *  - "whose bag": bags never cross. PICKUP / DOOR / FLAG are EVENTS (a world object was taken, a door changed, a flag was set), never inventory contents;
 *  - a message of another epoch (level instance, cs_epoch) is refused: after w1_set_epoch nothing of the old level can touch the new one.
 */
#ifndef WORLD1_H
#define WORLD1_H

#include <stddef.h>
#include <stdint.h>

#define W1_MAX_ENEMIES 128
#define W1_MAX_ITEMS 192
#define W1_MAX_DOORS 128
#define W1_MAX_FLAGS 512
#define W1_QMAX 192                /* unacknowledged reliable events a side can hold */
#define W1_WINDOW 32               /* receive window above the cumulative ack (bitmap) */
#define W1_BATCH_RECS 14           /* enemy records per ENEMIES packet (26 bytes each) */
#define W1_NO_SLOT 0xFF

#define W1_RTO_MS 200u             /* retransmit an unacknowledged event after this */
#define W1_ACK_MS 100u             /* standalone ack when something new arrived */
#define W1_ACK_KEEP_MS 500u        /* idle ack (liveness of the world channel) */
#define W1_BATCH_MS 66u            /* ENEMIES (host), ~15 Hz */
#define W1_ROSTER_MS 300u          /* roster of live keys (host) */
#define W1_ROSTER_MISS 4           /* a replica missing from this many consecutive rosters is removed (last resort) */
#define W1_REQ_MS 500u

typedef uint16_t w1_key;                         /* (block << 8) | index of the sArrange character record */
#define W1_KEY(block, index) ((w1_key)((((unsigned)(block)) << 8) | ((unsigned)(index) & 0xFF)))
typedef struct { uint32_t k[4]; } w1_key4;       /* item / object: (+0x10, +0x14, +0x38, +0x3C) of the sArrange list entry */

enum w1_role { W1_HOST = 1, W1_CLIENT = 2 };
enum w1_est { W1_UNKNOWN = 0, W1_ALIVE = 1, W1_DEAD = 2, W1_GONE = 3 };   /* per enemy key; DEAD and GONE are absorbing within an epoch */
enum w1_lane { W1_LANE_EVENT = 0, W1_LANE_WORLD = 1 };

typedef struct {
    w1_key key;
    uint32_t cls;                 /* class hash 0x8003xxxx (control value) */
    uint8_t owner;                /* slot that owned the spawn on the host (SpawnChara param_4) */
    uint8_t spawn_flags;          /* SpawnChara param_5 */
    float pos[3], yaw;
    float hp;
    uint16_t ai;                  /* state machine number (machine +0x3C) */
    uint32_t e1c;                 /* unit +0xE1C flag word (0x1000 = hidden / asleep) */
} w1_enemy_rec;

/* What the engine side provides. Every callback may be NULL (the call is then skipped). */
typedef struct {
    void *user;
    void *send_user;              /* handed to send instead of user when set (the network glue owns the sending side) */
    /* output: hand a ready body to the transport. lane 0 = cs_send_event(CP_EV_W1, ...), lane 1 = cs_send_world(kind, ...). */
    void (*send)(void *u, int lane, unsigned kind, const uint8_t *body, unsigned len);
    /* client: apply what the host decided */
    void (*spawn)(void *u, const w1_enemy_rec *r);                      /* create the replica: SpawnChara(block, index, owner, flags, fromHost = 1, 0) */
    void (*update)(void *u, const w1_enemy_rec *r);                     /* position / hp / state of a replica (newer record only) */
    void (*kill)(void *u, w1_key key, int killer_slot, int credit_own, uint32_t hit_flags, const float pt[3]);   /* credit_own = 1: run this PC's own kill record */
    void (*despawn)(void *u, w1_key key);                               /* remove a replica or a corpse */
    /* host: apply the client's hit to the real enemy (Damage + OnHit with the client's puppet as attacker) */
    void (*apply_hit)(void *u, w1_key key, int attacker_slot, float damage, uint32_t hit_flags, const float pt[3]);
    /* both: what the other PC's human did to the world */
    void (*item_gone)(void *u, const w1_key4 *key, uint32_t cls, int taker_slot);   /* take the world object away (FUN_005D7D00); do NOT put it in anybody's bag here */
    void (*door_set)(void *u, const w1_key4 *key, uint32_t vtbl, uint8_t phase);
    void (*flag_set)(void *u, uint32_t id, int32_t value);
    /* client: an enemy hit this PC's human on the host (PCHIT). attacker = key of the enemy (0xFFFF = none known), payload = the packed damage fields
     * (world1_hooks: wh_pchit_pack). Applied once per event (the reliable lane delivers exactly once). */
    void (*pc_hit)(void *u, w1_key attacker, const uint8_t *payload, unsigned len);
    /* both: an extension event (sub CP_W1_EXT_FIRST..LAST: the story mirror of sf1.c), delivered once, in the epoch it was sent in */
    void (*ext)(void *u, unsigned sub, const uint8_t *body, unsigned len);
} w1_ops;

typedef struct {
    uint32_t tx_ev, retx, rx_ev, dup, stale_epoch, bad, ahead, tx_world, rx_world;
    uint32_t hits_applied, late_hits, hit_unknown, hit_bad_slot;
    uint32_t rec_applied, rec_old, rec_unknown;
    uint32_t roster_despawn, req_sent, req_served;
    uint32_t pickup_conflict, door_lost, flag_lost, q_full, table_full;
    uint32_t spawns, deaths, despawns;
    uint32_t pchit_tx, pchit_rx;
    uint32_t ext_tx, ext_rx, flag_raw_calls;
} w1_stats;

typedef struct { uint16_t seq; uint8_t len; uint8_t used; uint32_t last_ms; uint8_t sent; uint8_t body[96]; } w1_txev;
typedef struct {
    w1_key key; uint8_t st; uint8_t owner, spawn_flags, killer, last_hit; uint8_t seen_rec;
    uint32_t cls;
    float pos[3], yaw, hp, hpmax;
    uint16_t ai; uint32_t e1c;
    uint16_t rec_bseq;            /* batch sequence of the newest record applied (client) */
    uint32_t sent_ms; uint8_t dirty; uint8_t miss;
    float lpos[3], lhp, lyaw; uint16_t lai; uint32_t le1c;   /* what the host last put in a batch (change detection) */
} w1_enemy;
typedef struct { w1_key4 key; uint32_t cls; uint8_t taker; uint8_t used; } w1_item;
typedef struct { w1_key4 key; uint32_t vtbl; uint8_t phase; uint16_t ver; uint8_t by_host; uint8_t used; } w1_door;
typedef struct { uint32_t id; int32_t value; uint16_t ver; uint8_t by_host; uint8_t used; } w1_flag;

typedef struct w1 {
    int role, own_slot, peer_slot;
    w1_ops ops;
    int epoch_valid; uint8_t epoch;
    uint32_t now;
    /* reliable layer */
    uint16_t tx_seq; w1_txev q[W1_QMAX]; uint16_t peer_ack_lo;
    uint16_t rx_lo; uint32_t rx_bits; uint8_t ack_dirty; uint32_t last_ack_ms;
    /* host: enemy stream */
    uint16_t bseq, rseq; uint32_t last_batch_ms, last_roster_ms; unsigned cursor;
    /* client: roster */
    uint16_t last_rseq; uint8_t have_rseq; w1_key prev_missing[16]; unsigned n_prev_missing; uint32_t last_req_ms; uint8_t req_valid;
    w1_enemy e[W1_MAX_ENEMIES]; unsigned ne;
    w1_item it[W1_MAX_ITEMS]; unsigned ni;
    w1_door dr[W1_MAX_DOORS]; unsigned nd;
    w1_flag fl[W1_MAX_FLAGS]; unsigned nf;
    uint32_t kills[2];
    int hit_slot;                 /* slot of the hit being applied by apply_hit (kill credit while the engine kills inside the callback) */
    w1_key hit_key; int in_hit;
    int flag_raw;                 /* w1_set_flag_raw: every FLAG of a newer version reaches ops.flag_set, also when the table already holds the value (the engine may differ) */
    w1_stats st;
} w1_t;

/* setup. role W1_HOST / W1_CLIENT; own_slot = the slot of the human on THIS PC (0 lead / 1 partner), the other human has 1 - own_slot. */
void w1_init(w1_t *w, int role, int own_slot, const w1_ops *ops);
/* a new level instance (cs_epoch): drops all state of the old one. Same epoch again = no-op. */
void w1_set_epoch(w1_t *w, unsigned epoch);
unsigned w1_epoch(const w1_t *w);
/* time + timers: retransmits, acks, enemy batches (host), roster (host), roster check and requests (client). Call every game frame or at least every ~30 ms. */
void w1_tick(w1_t *w, uint32_t now_ms);
/* a message of the transport. lane/kind as handed to ops.send on the other PC (CP_EVENT body for lane 0, CP_WORLD kind + body for lane 1). Returns 1 = accepted, 0 = refused. */
int w1_recv(w1_t *w, int lane, unsigned kind, const uint8_t *body, unsigned len);

/* ---- host: the engine reports (SpawnChara hook, per frame samples, OnHit hook, unit delete) ---- */
int w1_host_spawn(w1_t *w, w1_key key, uint32_t cls, int owner, int spawn_flags, const float pos[3], float hp);   /* 1 = new */
void w1_host_sample(w1_t *w, w1_key key, const float pos[3], float yaw, float hp, unsigned ai, uint32_t e1c);
void w1_host_hit_note(w1_t *w, w1_key key, int attacker_slot);                  /* the host's own human hit it (credit if the enemy dies without a better hint) */
int w1_host_death(w1_t *w, w1_key key, int killer_hint, uint32_t hit_flags, const float pt[3]);   /* returns the killer slot (W1_NO_SLOT if nobody is credited), -1 if there is nothing to do; killer_hint W1_NO_SLOT = auto: the slot of the client hit being applied, else the last damager */
int w1_host_despawn(w1_t *w, w1_key key);
#define W1_PCHIT_MAX 88
#define W1_NO_KEY 0xFFFFu
/* host: an enemy's blow on the OTHER PC's human: sent reliably (exactly once) to the client, which applies it to its own human. 1 = queued. */
int w1_host_pchit(w1_t *w, w1_key attacker, const uint8_t *payload, unsigned len);

/* ---- client: the local human damaged a replica. Returns the damage the replica may take locally (the host decides the rest). 0 = not a live replica. ---- */
float w1_client_hit(w1_t *w, w1_key key, float damage, uint32_t hit_flags, const float pt[3]);

/* ---- both: events of the local human ---- */
int w1_local_pickup(w1_t *w, const w1_key4 *key, uint32_t cls);                   /* 1 = announced, 0 = the object is already gone (conflict) */
int w1_local_door(w1_t *w, const w1_key4 *key, uint32_t vtbl, unsigned phase);    /* 1 = announced, 0 = unchanged (an echo) */
int w1_local_flag(w1_t *w, uint32_t id, int32_t value);                           /* 1 = announced, 0 = unchanged */

/* ---- both: an extension event (sub CP_W1_EXT_FIRST..CP_W1_EXT_LAST, body <= W1_EXT_MAX): reliable, exactly once, same epoch. 1 = queued ---- */
#define W1_EXT_MAX 92
int w1_send_ext(w1_t *w, unsigned sub, const uint8_t *body, unsigned len);
/* FLAG delivery: 0 (default) = ops.flag_set only when the value differs from the table; 1 = for every newer version (the glue compares with the engine) */
void w1_set_flag_raw(w1_t *w, int on);
/* record a value in the flag table (a new version) WITHOUT sending it: the host applied the guest's proposal, so the guest has it; the host's later change is then newer */
int w1_note_flag(w1_t *w, uint32_t id, int32_t value);

/* ---- inspection (tests, console) ---- */
int w1_enemy_state(const w1_t *w, w1_key key);                                    /* enum w1_est */
int w1_enemy_get(const w1_t *w, w1_key key, w1_enemy_rec *out, int *killer);      /* 1 = known */
unsigned w1_count(const w1_t *w, int est);
uint32_t w1_kills(const w1_t *w, int slot);
int w1_item_is_gone(const w1_t *w, const w1_key4 *key, int *taker);
int w1_door_get(const w1_t *w, const w1_key4 *key, uint32_t *vtbl, unsigned *phase, unsigned *ver);
int w1_flag_get(const w1_t *w, uint32_t id, int32_t *value, unsigned *ver);
unsigned w1_pending(const w1_t *w);                                               /* unacknowledged reliable events */
const w1_stats *w1_get_stats(const w1_t *w);
void w1_summary(const w1_t *w, char *out, size_t size);

#endif
