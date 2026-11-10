/* downed_logic.h: the pure state machine of the "downed / revive" rule (RE5 / RE6 "agony") for the co-op mod. No engine calls, no Windows, no network: tests/test_downed.c replays it, native/downed.inc (Rev2) or native/downed1_pair.h + native/downed1.c (Rev1)
 * (engine glue) feeds it and executes what it asks for. Design, wire events and live checks: docs/downed-revive.md.
 *
 * The rule (switchable, session flag CP_SESS_DOWNED): when a human's health would reach 0 and at least one OTHER human is still up (ALIVE), he goes DOWN instead of dying: hp stays 1, he lies on the floor
 * (the game's own wounded chain: dying_start -> wait_rescue), cannot act, and BLEEDS OUT on a timer (default 60 s). A partner who is up and holds the interact button next to him for the revive time raises him
 * (hp restored by the game's own completion). Game over (the death screen, retry from the checkpoint) when a human DIES (the timer ran out, a void fall, a finisher the rule cannot soften) or when no human
 * is up any more (everybody DOWN).
 *
 * Authority (network): every PC is the authority for ITS OWN human: it decides DOWN / DEAD / bleed-out for him, and it is the authority for the hold of ITS human on the partner (the progress is computed where
 * the input is). Everything else is a MIRROR driven by reliable events (DOWN, DEAD, BEGIN, KEEPALIVE, CANCEL, DONE) with the unreliable STATE flags as a debounced second opinion. Each PC evaluates the game-over
 * condition on its own mirror; the condition is stable (nobody can get up from "all down"), so both PCs reach it without arbitration. The one race that exists - both humans take their lethal hit within one
 * round trip - ends in "both DOWN" on both PCs, i.e. a game over on both, which is the right answer.
 * All times are milliseconds of a monotonic local clock (wrap-safe differences). */
#ifndef DOWNED_LOGIC_H
#define DOWNED_LOGIC_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DL_MAX 4                       /* the protocol knows four players, the game two */
#define DL_BLEED_DEFAULT_MS 60000u     /* agony before the bleed-out (0 in the config = never) */
#define DL_REVIVE_DEFAULT_MS 3000u     /* hold time of the mod-driven revive (the engine's own is g_sPlayer+0xCE4, read live) */
#define DL_KEEPALIVE_MS 500u           /* the reviver repeats "still holding" this often */
#define DL_WATCHDOG_MS 2500u           /* the downed side: no BEGIN / KEEPALIVE from the reviver for this long = the revive was cancelled (reviver gone, link stalled) */
#define DL_REMOTE_GRACE_MS 1500u       /* a remote human's predicted bleed-out is believed this long after the timer ran out without the DEAD event */
#define DL_TRUST_MS 600u               /* the unreliable STATE disagrees with the mirror for this long: it wins */
#define DL_QMAX 24

enum dl_state { DL_ALIVE = 0, DL_DOWN = 1, DL_DEAD = 2, DL_GONE = 3 /* not in the session (left / never was) */ };

/* what the glue must do (outputs) */
enum dl_out_kind {
    DL_O_NONE = 0,
    DL_O_GO_DOWN,        /* who = me: clamp hp to 1 and start the engine's dying chain (ChangeAction 0x79); bleed timer runs */
    DL_O_DIE,            /* who = me: let the human die now (hp := 0): the engine's game over, the existing partner coupling does the rest */
    DL_O_GET_UP,         /* who = me: revived (or auto-revived): the engine's completion (hp restore, get-up motion) */
    DL_O_REVIVE_PLAY,    /* who = me: somebody holds the button on me: the be_rescued pose (0x7D) */
    DL_O_REVIVE_STOP,    /* who = me: the hold ended without success: back to lying (0x7A) */
    DL_O_PUPPET_DOWN,    /* who = remote: show him lying (puppet ChangeAction 0x7A) so the stock rescue prompt can appear */
    DL_O_PUPPET_UP,      /* who = remote: he stood up again (puppet back to the normal drive) */
    DL_O_SEND_DOWN,      /* events to the partner: */
    DL_O_SEND_DEAD,
    DL_O_SEND_BEGIN,     /* arg = target human */
    DL_O_SEND_KEEPALIVE, /* arg = target human */
    DL_O_SEND_CANCEL,    /* arg = target human */
    DL_O_SEND_DONE,      /* arg = target human: I raised him */
    DL_O_GAME_OVER,      /* informational: the rule decided the run is over (followed by DL_O_DIE for me when I am still down or up) */
    DL_O_HOLD_RESET      /* who = me: my own hold ended (the glue clears its input latch / the engine's gauge) */
};

typedef struct { int kind, who, arg; } dl_out;

/* inputs from the other PC (events) */
enum dl_event_kind { DL_E_DOWN = 1, DL_E_DEAD, DL_E_BEGIN, DL_E_KEEPALIVE, DL_E_CANCEL, DL_E_DONE };

typedef struct {
    int enabled;               /* the session switch */
    uint32_t bleed_ms;         /* 0 = never bleeds out */
    uint32_t revive_ms;        /* mod-driven hold time (dl_local_hold) */
    int get_up_on_leave;       /* the partner left while I lie: stand up (otherwise I would lie for ever) */
} dl_cfg;

typedef struct {
    int state;
    uint32_t down_at, last_seen;
    int32_t bleed_left;        /* ms; frozen while somebody holds the button */
    int reviver;               /* human that holds the button on this one (as far as I know), -1 */
    uint32_t revive_last;      /* time of the last BEGIN / KEEPALIVE */
    int hold_target;           /* me only: the human whose revive I am holding, -1 */
    uint32_t hold_ms, hold_ka, hold_t; /* progress of my hold, time of my last keep-alive, time of the last hold sample */
    uint32_t mismatch_since;   /* STATE (unreliable) disagrees since */
    int mismatch_state;
} dl_human;

typedef struct {
    dl_cfg cfg;
    int me, n;
    dl_human h[DL_MAX];
    dl_out q[DL_QMAX];
    int nq;
    uint32_t last_tick;
    int over;                  /* the game-over decision was made (once per run) */
    uint32_t n_down, n_revive, n_bleed, n_over, n_dropped, n_stale_fix;
} dl_t;

static inline int32_t dl_diff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static inline void dl_push(dl_t *d, int kind, int who, int arg)
{
    if (d->nq >= DL_QMAX) {
        d->n_dropped++;
        return;
    }
    d->q[d->nq].kind = kind, d->q[d->nq].who = who, d->q[d->nq].arg = arg, d->nq++;
}

/* the glue empties the queue every pass; returns 1 and fills *o while something is left */
static inline int dl_take(dl_t *d, dl_out *o)
{
    if (!d->nq) return 0;
    *o = d->q[0];
    d->nq--;
    memmove(d->q, d->q + 1, (size_t)d->nq * sizeof d->q[0]);
    return 1;
}

static inline void dl_human_reset(dl_human *h, int state, uint32_t now)
{
    memset(h, 0, sizeof *h);
    h->state = state, h->reviver = -1, h->hold_target = -1, h->last_seen = now;
}

/* n humans in the session (2 today), me = my index (the slot of my character) */
static inline void dl_init(dl_t *d, const dl_cfg *cfg, int me, int n, uint32_t now)
{
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    if (n < 1) n = 1;
    if (n > DL_MAX) n = DL_MAX;
    d->me = me, d->n = n, d->last_tick = now;
    for (int i = 0; i < DL_MAX; i++) dl_human_reset(&d->h[i], i < n ? DL_ALIVE : DL_GONE, now);
}

/* a level load / retry: everybody is up again */
static inline void dl_reset(dl_t *d, uint32_t now)
{
    for (int i = 0; i < DL_MAX; i++)
        if (d->h[i].state != DL_GONE) dl_human_reset(&d->h[i], DL_ALIVE, now);
    d->nq = 0, d->over = 0, d->last_tick = now;
}

/* ---- queries ---- */
static inline int dl_state(const dl_t *d, int i) { return i >= 0 && i < DL_MAX ? d->h[i].state : DL_GONE; }
static inline int dl_present(const dl_t *d, int i) { return dl_state(d, i) != DL_GONE; }
static inline int dl_count_up(const dl_t *d)
{
    int c = 0;
    for (int i = 0; i < DL_MAX; i++) c += d->h[i].state == DL_ALIVE;
    return c;
}
static inline int dl_count_present(const dl_t *d)
{
    int c = 0;
    for (int i = 0; i < DL_MAX; i++) c += d->h[i].state != DL_GONE;
    return c;
}
static inline int dl_others_up(const dl_t *d, int i)
{
    int c = 0;
    for (int k = 0; k < DL_MAX; k++) c += k != i && d->h[k].state == DL_ALIVE;
    return c;
}
static inline int32_t dl_bleed_left(const dl_t *d, int i) { return i >= 0 && i < DL_MAX && d->h[i].state == DL_DOWN ? d->h[i].bleed_left : 0; }
static inline int dl_being_revived(const dl_t *d, int i) { return i >= 0 && i < DL_MAX && d->h[i].state == DL_DOWN && d->h[i].reviver >= 0; }

/* ---- the game-over decision: any human dead, or nobody up (with at least two humans in the session) ---- */
static inline void dl_check_over(dl_t *d)
{
    if (d->over || !d->cfg.enabled) return;
    int dead = 0, up = 0, down = 0;
    for (int i = 0; i < DL_MAX; i++) {
        dead += d->h[i].state == DL_DEAD;
        up += d->h[i].state == DL_ALIVE;
        down += d->h[i].state == DL_DOWN;
    }
    if (dead || (up == 0 && down > 0)) {
        d->over = 1, d->n_over++;
        dl_push(d, DL_O_GAME_OVER, d->me, dead);
        if (d->h[d->me].state == DL_DOWN || d->h[d->me].state == DL_ALIVE) {
            if (d->h[d->me].state == DL_DOWN) {
                d->h[d->me].state = DL_DEAD;
                dl_push(d, DL_O_SEND_DEAD, d->me, 0);
            }
            dl_push(d, DL_O_DIE, d->me, 0);
        }
    }
}

/* ---- my own human ---- */

/* The local human would die from this hit (hp would reach 0). Returns 1 = absorb it: clamp hp to 1, the human is DOWN now; 0 = let him die (the rule is off, nobody is up to raise him, he is down already
 * and the glue's clamp failed). A hit on a human who is DOWN already is absorbed too (he is at hp 1, the clamp keeps him there; hits do not change the timer). */
static inline int dl_local_lethal(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (!d->cfg.enabled) return 0;
    if (m->state == DL_DOWN) return 1;
    if (m->state != DL_ALIVE) return 0;
    if (dl_others_up(d, d->me) == 0) return 0;   /* nobody to raise me: an ordinary death (also the single-human game) */
    m->state = DL_DOWN, m->down_at = now, m->bleed_left = d->cfg.bleed_ms ? (int32_t)d->cfg.bleed_ms : 0x7FFFFFFF, m->reviver = -1, m->revive_last = 0;
    if (m->hold_target >= 0) {   /* I was holding a revive: it ends */
        dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target), dl_push(d, DL_O_HOLD_RESET, d->me, 0);
        m->hold_target = -1, m->hold_ms = 0;
    }
    d->n_down++;
    dl_push(d, DL_O_GO_DOWN, d->me, 0);
    dl_push(d, DL_O_SEND_DOWN, d->me, 0);
    return 1;
}

/* my human is dead by a path the rule does not soften (the engine says so): hp 0 for real */
static inline void dl_local_dead(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->state == DL_DEAD || m->state == DL_GONE) return;
    m->state = DL_DEAD, m->last_seen = now;
    if (m->hold_target >= 0) {
        dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
        m->hold_target = -1, m->hold_ms = 0;
    }
    dl_push(d, DL_O_SEND_DEAD, d->me, 0);
    dl_check_over(d);
}

/* my human is standing at full control again with hp > 1 although the mirror says DOWN (the engine's own rescue completed, a heal, a console command): follow the engine */
static inline void dl_local_up(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->state != DL_DOWN) return;
    int by = m->reviver;
    dl_human_reset(m, DL_ALIVE, now);
    d->n_revive++;
    dl_push(d, DL_O_SEND_DONE, d->me, d->me);   /* "I am up": the partner's mirror of me (arg = me) */
    (void)by;
}

/* ---- the hold of my human on a downed partner (mod-driven revive; the engine's own hold uses dl_local_revive_*) ----
 * holding = the interact button is down, in_range = close enough, called every pass with the time passed (dt ms) */
static inline void dl_local_hold(dl_t *d, int target, int holding, int in_range, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (!d->cfg.enabled) return;
    int ok = holding && in_range && m->state == DL_ALIVE && target >= 0 && target < DL_MAX && target != d->me && d->h[target].state == DL_DOWN;
    if (!ok) {
        if (m->hold_target >= 0) {
            dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
            dl_push(d, DL_O_HOLD_RESET, d->me, 0);
            if (d->h[m->hold_target].reviver == d->me) d->h[m->hold_target].reviver = -1;
            m->hold_target = -1, m->hold_ms = 0;
        }
        return;
    }
    if (m->hold_target != target) {   /* a new hold starts at zero */
        if (m->hold_target >= 0) dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
        m->hold_target = target, m->hold_ms = 0, m->hold_ka = now, m->hold_t = now;
        d->h[target].reviver = d->me, d->h[target].revive_last = now;
        dl_push(d, DL_O_SEND_BEGIN, d->me, target);
        return;
    }
    uint32_t dt = dl_diff(now, m->hold_t) > 0 ? (uint32_t)dl_diff(now, m->hold_t) : 0;
    m->hold_t = now;
    if (dt > 250) dt = 250;   /* a long stall of the game thread is not progress */
    m->hold_ms += dt;
    d->h[target].revive_last = now;
    if (dl_diff(now, m->hold_ka) >= (int32_t)DL_KEEPALIVE_MS) {
        m->hold_ka = now;
        dl_push(d, DL_O_SEND_KEEPALIVE, d->me, target);
    }
    if (m->hold_ms >= d->cfg.revive_ms) {   /* done: he is up */
        dl_human *t = &d->h[target];
        dl_human_reset(t, DL_ALIVE, now);
        m->hold_target = -1, m->hold_ms = 0;
        d->n_revive++;
        dl_push(d, DL_O_SEND_DONE, d->me, target);
        dl_push(d, DL_O_PUPPET_UP, target, 0);
        dl_push(d, DL_O_HOLD_RESET, d->me, 0);
    }
}

/* the ENGINE's own hold (pl_act_rescue 0x7C) began / ended on my side: same bookkeeping, the progress is the engine's */
static inline void dl_local_revive_begin(dl_t *d, int target, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (!d->cfg.enabled || target < 0 || target >= DL_MAX || target == d->me || d->h[target].state != DL_DOWN || m->state != DL_ALIVE) return;
    if (m->hold_target == target) return;
    m->hold_target = target, m->hold_ms = 0, m->hold_ka = now;
    d->h[target].reviver = d->me, d->h[target].revive_last = now;
    dl_push(d, DL_O_SEND_BEGIN, d->me, target);
}
/* the engine's hold goes on: keep the downed side's watchdog quiet (call every pass while the engine's rescue action runs) */
static inline void dl_local_revive_keep(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->hold_target < 0) return;
    d->h[m->hold_target].revive_last = now;
    if (dl_diff(now, m->hold_ka) >= (int32_t)DL_KEEPALIVE_MS) {
        m->hold_ka = now;
        dl_push(d, DL_O_SEND_KEEPALIVE, d->me, m->hold_target);
    }
}
static inline void dl_local_revive_cancel(dl_t *d)
{
    dl_human *m = &d->h[d->me];
    if (m->hold_target < 0) return;
    dl_push(d, DL_O_SEND_CANCEL, d->me, m->hold_target);
    if (d->h[m->hold_target].reviver == d->me) d->h[m->hold_target].reviver = -1;
    m->hold_target = -1, m->hold_ms = 0;
}
static inline void dl_local_revive_done(dl_t *d, uint32_t now)
{
    dl_human *m = &d->h[d->me];
    if (m->hold_target < 0) return;
    int t = m->hold_target;
    dl_human_reset(&d->h[t], DL_ALIVE, now);
    m->hold_target = -1, m->hold_ms = 0;
    d->n_revive++;
    dl_push(d, DL_O_SEND_DONE, d->me, t);
    dl_push(d, DL_O_PUPPET_UP, t, 0);
}

/* ---- the partner ---- */

/* a reliable event of human i (pid mapped to a human index by the glue); arg = target human for BEGIN / KEEPALIVE / CANCEL / DONE */
static inline void dl_remote_event(dl_t *d, int i, int kind, int arg, uint32_t now)
{
    if (i < 0 || i >= DL_MAX || i == d->me || !d->cfg.enabled) return;
    dl_human *r = &d->h[i];
    if (r->state == DL_GONE) return;
    r->last_seen = now, r->mismatch_since = 0;
    switch (kind) {
    case DL_E_DOWN:
        if (r->state == DL_ALIVE) {
            r->state = DL_DOWN, r->down_at = now, r->bleed_left = d->cfg.bleed_ms ? (int32_t)d->cfg.bleed_ms : 0x7FFFFFFF, r->reviver = -1;
            d->n_down++;
            dl_push(d, DL_O_PUPPET_DOWN, i, 0);
        }
        break;
    case DL_E_DEAD:
        if (r->state != DL_DEAD) {
            r->state = DL_DEAD, r->reviver = -1;
            dl_push(d, DL_O_PUPPET_UP, i, 0);   /* the puppet is not lying for the rescue any more (the death screen follows) */
        }
        break;
    case DL_E_BEGIN:   /* i holds the button on arg */
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN) {
            d->h[arg].reviver = i, d->h[arg].revive_last = now;
            if (arg == d->me) dl_push(d, DL_O_REVIVE_PLAY, d->me, i);
        }
        break;
    case DL_E_KEEPALIVE:
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN) {
            if (d->h[arg].reviver != i) {   /* a missed BEGIN: take it as one */
                d->h[arg].reviver = i;
                if (arg == d->me) dl_push(d, DL_O_REVIVE_PLAY, d->me, i);
            }
            d->h[arg].revive_last = now;
        }
        break;
    case DL_E_CANCEL:
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN && d->h[arg].reviver == i) {
            d->h[arg].reviver = -1;
            if (arg == d->me) dl_push(d, DL_O_REVIVE_STOP, d->me, i);
        }
        break;
    case DL_E_DONE:   /* i raised arg (arg == i: "I am up", the engine finished the revive on his side) */
        if (arg >= 0 && arg < DL_MAX && d->h[arg].state == DL_DOWN) {
            dl_human *t = &d->h[arg];
            dl_human_reset(t, DL_ALIVE, now);
            d->n_revive++;
            if (arg == d->me) dl_push(d, DL_O_GET_UP, d->me, i);
            else dl_push(d, DL_O_PUPPET_UP, arg, 0);
        }
        break;
    default:
        break;
    }
    dl_check_over(d);
}

/* the unreliable STATE of human i: down / dead flags. Dead is believed at once (hp 0 is certain); a disagreement about DOWN / UP has to last DL_TRUST_MS (the reliable event may be on its way) */
static inline void dl_remote_sample(dl_t *d, int i, int down, int dead, uint32_t now)
{
    if (i < 0 || i >= DL_MAX || i == d->me || !d->cfg.enabled) return;
    dl_human *r = &d->h[i];
    if (r->state == DL_GONE) return;
    r->last_seen = now;
    int said = dead ? DL_DEAD : down ? DL_DOWN : DL_ALIVE;
    if (said == r->state) {
        r->mismatch_since = 0;
        return;
    }
    if (said == DL_DEAD) {
        dl_remote_event(d, i, DL_E_DEAD, 0, now);
        return;
    }
    if (r->mismatch_state != said || !r->mismatch_since) {
        r->mismatch_state = said, r->mismatch_since = now ? now : 1;
        return;
    }
    if (dl_diff(now, r->mismatch_since) >= (int32_t)DL_TRUST_MS) {
        r->mismatch_since = 0, d->n_stale_fix++;
        if (said == DL_DOWN) dl_remote_event(d, i, DL_E_DOWN, 0, now);
        else if (r->state == DL_DOWN) dl_remote_event(d, i, DL_E_DONE, i, now);   /* he is up: whatever stopped him lying, the mirror follows */
        else if (r->state == DL_DEAD) {   /* a dead mirror that comes back to life = a reload we missed: believe the sample */
            dl_human_reset(r, DL_ALIVE, now);
            dl_push(d, DL_O_PUPPET_UP, i, 0);
        }
    }
}

/* a player left / joined */
static inline void dl_set_present(dl_t *d, int i, int present, uint32_t now)
{
    if (i < 0 || i >= DL_MAX || i == d->me) return;
    if (!present) {
        d->h[i].state = DL_GONE;
        if (d->h[d->me].hold_target == i) d->h[d->me].hold_target = -1, d->h[d->me].hold_ms = 0;
        if (d->h[d->me].state == DL_DOWN) {
            d->h[d->me].reviver = d->h[d->me].reviver == i ? -1 : d->h[d->me].reviver;
            if (d->cfg.get_up_on_leave && dl_count_present(d) <= 1) {   /* alone and lying: stand up */
                dl_human_reset(&d->h[d->me], DL_ALIVE, now);
                dl_push(d, DL_O_GET_UP, d->me, -1);
            }
        }
    } else if (d->h[i].state == DL_GONE) {
        dl_human_reset(&d->h[i], DL_ALIVE, now);
    }
}

/* ---- time ---- call every pass (10..60 Hz) ---- */
static inline void dl_tick(dl_t *d, uint32_t now)
{
    int32_t dt = dl_diff(now, d->last_tick);
    if (dt < 0) dt = 0;
    if (dt > 1000) dt = 1000;   /* a stall (loading, the debugger): the bleed timer does not eat it */
    d->last_tick = now;
    if (!d->cfg.enabled) return;
    for (int i = 0; i < DL_MAX; i++) {
        dl_human *h = &d->h[i];
        if (h->state != DL_DOWN) continue;
        if (h->reviver >= 0) {   /* being raised: frozen; the watchdog ends a revive nobody keeps alive */
            uint32_t since = (uint32_t)dl_diff(now, h->revive_last);
            int mine = h->reviver == d->me;   /* my own hold is kept alive by dl_local_hold / the engine glue every pass */
            if (!mine && since > DL_WATCHDOG_MS) {
                h->reviver = -1;
                if (i == d->me) dl_push(d, DL_O_REVIVE_STOP, d->me, -1);
            }
            continue;
        }
        if (h->bleed_left == 0x7FFFFFFF) continue;   /* never */
        h->bleed_left -= dt;
        if (h->bleed_left <= 0) {
            if (i == d->me) {   /* my own bleed-out: I die, the engine's game over follows */
                h->state = DL_DEAD;
                d->n_bleed++;
                dl_push(d, DL_O_SEND_DEAD, d->me, 1);
                dl_push(d, DL_O_DIE, d->me, 0);
                dl_check_over(d);
            } else if (h->bleed_left <= -(int32_t)DL_REMOTE_GRACE_MS) {   /* a mirror: the DEAD event is late: believe the prediction */
                h->state = DL_DEAD;
                d->n_bleed++;
                dl_push(d, DL_O_PUPPET_UP, i, 0);
                dl_check_over(d);
            }
        }
    }
    dl_check_over(d);
}

static inline const char *dl_state_name(int s) { return s == DL_ALIVE ? "up" : s == DL_DOWN ? "DOWN" : s == DL_DEAD ? "DEAD" : "gone"; }

/* one line for the console / the log */
static inline int dl_status(const dl_t *d, char *out, int size)
{
    int o = snprintf(out, (size_t)size, "downed: %s, bleed %us revive %ums, over %d; downs %u revives %u bleed-outs %u game-overs %u (dropped %u, mirror fixes %u);",
                     d->cfg.enabled ? "ON" : "off", d->cfg.bleed_ms / 1000, d->cfg.revive_ms, d->over, d->n_down, d->n_revive, d->n_bleed, d->n_over, d->n_dropped, d->n_stale_fix);
    for (int i = 0; i < DL_MAX && o < size - 40; i++) {
        if (d->h[i].state == DL_GONE) continue;
        o += snprintf(out + o, (size_t)(size - o), " [%d%s %s", i, i == d->me ? " me" : "", dl_state_name(d->h[i].state));
        if (d->h[i].state == DL_DOWN) o += snprintf(out + o, (size_t)(size - o), " left %ds%s", d->h[i].bleed_left == 0x7FFFFFFF ? -1 : d->h[i].bleed_left / 1000, d->h[i].reviver >= 0 ? " REVIVED" : "");
        if (d->h[i].hold_target >= 0) o += snprintf(out + o, (size_t)(size - o), " holding %d %ums", d->h[i].hold_target, d->h[i].hold_ms);
        o += snprintf(out + o, (size_t)(size - o), "]");
    }
    return o;
}

/* session flags byte (cp_session.flags): bit 0 = rule on, bits 1..2 = bleed-out preset */
#define CP_SESS_DOWNED 1u
static inline uint32_t dl_preset_ms(unsigned preset) { static const uint32_t t[4] = {60000u, 30000u, 120000u, 0u}; return t[preset & 3]; }
static inline void dl_cfg_from_flags(dl_cfg *c, unsigned flags)
{
    c->enabled = (flags & CP_SESS_DOWNED) != 0;
    c->bleed_ms = dl_preset_ms((flags >> 1) & 3);
    c->revive_ms = DL_REVIVE_DEFAULT_MS;
    c->get_up_on_leave = 1;
}

#endif
