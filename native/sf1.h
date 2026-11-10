/*
 * sf1: the story mirror of Revelations 1 Episode 1 as pure logic (no Windows, no game): which scenario flag changes cross between the PCs (E1),
 * the Genesis analysis rate (E2) and broken set pieces (E6). Design and evidence: notes\ep1-world.md.
 *
 * The glue (world1_game.c) calls it on the logic thread:
 *   sf1_epoch           a new level instance (the world model's epoch) began here
 *   sf1_unplaced        ... but this PC's human is not placed yet (e1w2 live: the 1-1 start movie runs ~110 s with the units at 0,0,0 after the barrier opened)
 *   sf1_placed          this PC's human is placed now: the quiet window starts here, not at the epoch start
 *   sf1_local           this PC's engine changed a flag (old -> new, read around SetScenarioFlag; never for our own applies)
 *   sf1_remote_stream   guest: a flag of the host's stream arrived (world1's versioned FLAG, already ordered)
 *   sf1_proposal        host: a guest proposal arrived
 *   sf1_tick            guest: held clears that are due
 *   sf1_digest_*        host: build the bitmap at the end of the quiet window; guest: apply it
 *   sf1_rate_*          the rate [cSystem+0x74]
 *   sf1_piece_*         set pieces (key4 + class)
 * Rules (the tests prove them):
 *   - only the Episode 1 whitelist crosses (sf1_index); the host is authoritative and sends sets and clears; the guest sends only sets ("proposals")
 *     of ids the host has not touched in this epoch, never inside the quiet window before the digest;
 *   - the host applies a proposal only for an untouched id whose value is 0 here, so a guest that is behind cannot re-arm what the host consumed;
 *   - a held clear keeps a pulse visible on the guest for SF1_HOLD_MS;
 *   - everything is per epoch: sf1_epoch forgets the old level.
 */
#ifndef SF1_H
#define SF1_H

#include <stddef.h>
#include <stdint.h>

#define SF1_N 380                 /* mirrored ids (the ranges in sf1.c) */
#define SF1_BYTES ((SF1_N + 7) / 8)
#define SF1_QUIET_MS 2500u        /* after this PC's human is placed in the epoch (else after the epoch began): no proposals, then the host's digest */
#define SF1_PLACE_MAX_MS 300000u  /* an epoch whose own human is still unplaced this long (a stuck movie / no unit): the window starts anyway */
#define SF1_DIGEST_WAIT_MS 12000u /* guest: no digest this long after the quiet window -> proposals go on without it (an older host) */
#define SF1_HOLD_MS 750u          /* guest: a clear right after a remote set of the same id waits until the set is this old */
#define SF1_MAXHOLD 32
#define SF1_MAXPIECES 64
#define SF1_RATE_MAX 100

enum sf1_role { SF1_HOST = 1, SF1_GUEST = 2 };

/* decisions */
enum {
    SF1_SKIP = 0,                 /* nothing to do (see the reason) */
    SF1_SEND = 1,                 /* host: send on the stream (w1_local_flag); guest: send a proposal */
    SF1_APPLY = 2,                /* apply here now */
    SF1_LATER = 3,                /* guest: apply at *due (a held clear) */
};
enum sf1_why {
    SF1_W_NONE = 0, SF1_W_RANGE, SF1_W_SAME, SF1_W_CLEAR, SF1_W_TOUCHED, SF1_W_QUIET, SF1_W_NOEPOCH, SF1_W_ROLE, SF1_W_HOLDFULL, SF1_W_N
};
const char *sf1_why_name(int why);

typedef struct {
    uint32_t local_seen, local_sent, prop_sent, prop_deferred, skip_range, skip_same, skip_clear, skip_touched;
    uint32_t stream_rx, stream_applied, held, held_applied, hold_full;
    uint32_t prop_rx, prop_applied, prop_refused;
    uint32_t digest_tx, digest_rx, digest_set, digest_clear, digest_prop, digest_bad, digest_timeout;
    uint32_t rate_tx, rate_rx, rate_applied;
    uint32_t piece_tx, piece_rx, piece_applied, piece_dup;
} sf1_stats;

typedef struct { uint32_t k[4]; uint32_t cls; uint8_t state; } sf1_piece;   /* state 1 = broken here (own), 2 = applied from the other PC, 3 = both */

typedef struct {
    int role;
    int epoch_valid;
    unsigned epoch;
    uint32_t epoch_ms;
    uint32_t quiet_ms;            /* the quiet window starts here: the epoch start, or the moment this PC's human was placed (sf1_placed) */
    uint8_t unplaced;             /* sf1_unplaced: the window waits for sf1_placed (at most SF1_PLACE_MAX_MS after the epoch start) */
    int wide;                     /* lab: every id >= 256 crosses (not only the whitelist); the digest still covers the whitelist */
    uint8_t touched[SF1_BYTES];   /* host: changed here or applied from a proposal in this epoch; guest: the host has spoken about it */
    uint8_t own[SF1_BYTES];       /* guest: set by its own engine in this epoch */
    uint8_t proposed[SF1_BYTES];  /* guest: a proposal was sent in this epoch */
    uint32_t rset_ms[SF1_N];      /* guest: when a remote SET of the id was applied (0 = never) */
    uint8_t digest_done;          /* host: sent; guest: applied (or given up) */
    uint8_t dig_kept;             /* guest: a digest arrived inside its own quiet window (a slower load): applied when the window ends */
    uint8_t dig[4 + SF1_BYTES];
    struct { uint32_t id; int32_t value; uint32_t due; } hold[SF1_MAXHOLD];
    unsigned nhold;
    /* rate */
    int rate_last;                /* the value this PC last saw / wrote (-1 = unknown) */
    /* pieces */
    sf1_piece pc[SF1_MAXPIECES];
    unsigned npc;
    sf1_stats st;
} sf1_t;

/* the whitelist: bit index of a mirrored id, -1 = not mirrored. sf1_id_at(i) is the inverse. sf1_layout() = a hash of the ranges (digest header). */
int sf1_index(uint32_t id);
uint32_t sf1_id_at(unsigned i);
uint16_t sf1_layout(void);
/* crosses at all (whitelist, or id >= 256 in the wide lab mode) */
int sf1_crosses(const sf1_t *s, uint32_t id);

void sf1_init(sf1_t *s, int role);
void sf1_epoch(sf1_t *s, unsigned epoch, uint32_t now);           /* same epoch again = no-op */
int sf1_in_quiet(const sf1_t *s, uint32_t now);
void sf1_unplaced(sf1_t *s);                                     /* right after sf1_epoch: the own human is not placed yet */
int sf1_placed(sf1_t *s, uint32_t now);                          /* the own human is placed: 1 = the window starts now (once per epoch) */

/* this PC's engine changed id from old to new (both read from the engine). Returns SF1_SEND or SF1_SKIP; *why = the reason (may be NULL). */
int sf1_local(sf1_t *s, uint32_t id, int32_t old, int32_t value, uint32_t now, int *why);
/* guest: the host's stream says id = value (a newer version). SF1_APPLY now, or SF1_LATER (held, sf1_tick hands it out), or SF1_SKIP. */
int sf1_remote_stream(sf1_t *s, uint32_t id, int32_t value, int32_t cur, uint32_t now, int *why);
/* host: a guest proposal id = value; cur = the host's value. SF1_APPLY or SF1_SKIP. */
int sf1_proposal(sf1_t *s, uint32_t id, int32_t value, int32_t cur, int *why);
/* guest: next due held clear (1 = *id / *value filled, call again until 0) */
int sf1_tick(sf1_t *s, uint32_t now, uint32_t *id, int32_t *value);

/* engine reader for the digest: value of a flag id on this PC */
typedef int (*sf1_get_fn)(void *u, uint32_t id);
/* host: 1 once per epoch when the quiet window has passed (then build and send) */
int sf1_digest_due(sf1_t *s, uint32_t now);
/* host: the digest body (layout u16, n u16, bitmap); returns the length (0 = no room) */
unsigned sf1_digest_build(sf1_t *s, sf1_get_fn get, void *u, uint8_t *out, unsigned cap);
/* guest: apply a digest: calls set(u, id, value) for each id to change here and prop(u, id) for each own set the host lacks (to be proposed).
 * Returns 1 = taken, 2 = kept (inside the guest's own quiet window: sf1_digest_kept applies it), 0 = refused (layout / length / role / epoch). */
typedef void (*sf1_set_fn)(void *u, uint32_t id, int32_t value);
typedef void (*sf1_prop_fn)(void *u, uint32_t id);
int sf1_digest_apply(sf1_t *s, const uint8_t *body, unsigned len, sf1_get_fn get, sf1_set_fn set, sf1_prop_fn prop, void *u, uint32_t now);
/* guest: a digest kept by sf1_digest_apply (return 2) is applied once the guest's own quiet window is over. 1 = applied now */
int sf1_digest_kept(sf1_t *s, uint32_t now, sf1_get_fn get, sf1_set_fn set, sf1_prop_fn prop, void *u);
/* guest: no digest came within SF1_DIGEST_WAIT_MS after the quiet window: give up waiting; prop(u, id) for own sets still 1 here and not touched. 1 = gave up now */
int sf1_digest_timeout(sf1_t *s, uint32_t now, sf1_get_fn get, sf1_prop_fn prop, void *u);
/* guest: may a deferred own set be proposed now (after the digest)? */
int sf1_may_propose(const sf1_t *s, uint32_t now);

/* rate. kinds of the wire: 0 = absolute from the host, 1 = delta from the guest */
enum { SF1_RATE_ABS = 0, SF1_RATE_DELTA = 1 };
/* poll of this PC's rate value cur: returns 1 when something is to be sent (*kind, *value), 0 otherwise */
int sf1_rate_poll(sf1_t *s, int cur, int *kind, int *value);
/* a rate message: returns 1 when this PC's rate must be written to *out (then sf1 counts it as seen); *send = 1 when the host must send its new absolute */
int sf1_rate_remote(sf1_t *s, int kind, int value, int cur, int *out, int *send);
/* host: the absolute to send with the digest */
int sf1_rate_value(const sf1_t *s);

/* set pieces. sf1_piece_class: the class may be mirrored (verified table). local: the piece broke here (1 = announce); remote: the other PC's piece broke (1 = apply here) */
int sf1_piece_class(uint32_t cls);
int sf1_piece_local(sf1_t *s, const uint32_t k[4], uint32_t cls);
int sf1_piece_remote(sf1_t *s, const uint32_t k[4], uint32_t cls);
int sf1_piece_known(const sf1_t *s, const uint32_t k[4]);         /* 0 none, else the state bits */

/* wire bodies of the ext sub types (world1 reliable lane): proposals, rate, pieces */
unsigned sf1_enc_prop(uint8_t *out, unsigned cap, uint32_t id, int32_t value);
int sf1_dec_prop(const uint8_t *b, unsigned n, uint32_t *id, int32_t *value);
unsigned sf1_enc_rate(uint8_t *out, unsigned cap, int kind, int value);
int sf1_dec_rate(const uint8_t *b, unsigned n, int *kind, int *value);
unsigned sf1_enc_piece(uint8_t *out, unsigned cap, const uint32_t k[4], uint32_t cls);
int sf1_dec_piece(const uint8_t *b, unsigned n, uint32_t k[4], uint32_t *cls);

void sf1_summary(const sf1_t *s, char *out, size_t size);

/* ep1-probe: the mirrored ids set here as one line (count returned, -1 = get refused; *hash = FNV-1a of the bitmap; list = the ids, "..." when cut) */
int sf1_set_summary(sf1_get_fn get, void *u, uint32_t *hash, char *list, unsigned cap);

/* ep1-probe: the PRE-LOAD digest. e1p1 live (SV probe): no restart deserializes the flag store (no save read at the host's restart call nor at the guest's
 * follow; the title's save read loads the store once), so at a host Continue / restart the guest's level would load with ITS store (its title / boot state, or
 * the old level's) until the post-load digest, 2.5 s after its human is placed. The host sends its mirrored bitmap at its restart PRE (session event
 * CP_EV_SFPRE, any phase: the world model does not send while loading); the guest writes it (sets and clears) at ITS restart PRE (right before its
 * FUN_00720870), so its load reads the host's world. Used once; older than SF1_PRE_MAX_MS = dropped (a later Continue must not get it); the newest seq wins. */
#define SF1_PRE_LEN (6 + SF1_BYTES)   /* layout u16, seq u32, bitmap */
#define SF1_PRE_MAX_MS 20000u
typedef struct { int have; uint32_t seq, at; uint8_t bm[SF1_BYTES]; uint32_t tx, rx, bad, stale, applied, set_n, clr_n; } sf1_pre_t;
unsigned sf1_pre_build(sf1_get_fn get, void *u, uint32_t seq, uint8_t *out, unsigned cap);   /* host: the body (0 = no room / no getter) */
int sf1_pre_take(sf1_pre_t *p, const uint8_t *b, unsigned n, uint32_t now);              /* guest: 1 = kept (newest), 0 = refused / older */
/* guest, at its restart PRE: writes every mirrored id whose value differs from the host's bitmap through set(); 1 = applied (fresh, unused), *set_n / *clr_n counts */
int sf1_pre_apply(sf1_pre_t *p, uint32_t now, sf1_get_fn get, sf1_set_fn set, void *u, unsigned *set_n, unsigned *clr_n);

#endif
