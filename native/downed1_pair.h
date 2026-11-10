/* downed1_pair.h: the Revelations 1 side of the "downed / revive" rule for LOCAL SPLIT (two humans on one PC, no network).
 *
 * Two state machines of the Rev2 model (downed_logic.h), one per human (index = the unit slot field: 0 = the first character, 1 = the second).
 * Machine k has ME = k. What machine k sends (DOWN, DEAD, BEGIN, KEEPALIVE, CANCEL, DONE) is delivered at once to the other machine, so the two
 * never disagree for longer than one pass (zero latency, no wire, no watchdog needed: the same code paths as the network version run).
 * What machine k wants done to ITS unit comes back as d1_action (GO_DOWN, DIE, GET_UP, REVIVE_PLAY / STOP, HOLD_RESET); PUPPET_* and GAME_OVER
 * are dropped (both humans are real local characters: nothing to puppet, and game over follows from DIE).
 *
 * Pure C: no engine, no Windows. The glue (native/downed1.c) reads the hp / position / pad facts, calls these functions and applies the actions.
 * Offline replay: tests/test_downed_pair.c.  Design and live checks: docs/downed-revive.md.
 */
#ifndef DOWNED1_PAIR_H
#define DOWNED1_PAIR_H

#include "downed_logic.h"

#define D1_N 2
#define D1_REVIVE_PCT 0.35f    /* hp after a revive, fraction of the max hp (GUESS: the Rev2 value; no Rev1 revive exists to copy) */
#define D1_REACH 170.0f        /* revive distance in world units (GUESS, the same as Rev2 until measured) */

typedef struct {
    dl_t d[D1_N];
    int on;                    /* the rule is on (flags CP_SESS_DOWNED) */
} d1_pair_t;

enum d1_act { D1_A_NONE = 0, D1_A_GO_DOWN, D1_A_DIE, D1_A_GET_UP, D1_A_REVIVE_PLAY, D1_A_REVIVE_STOP, D1_A_HOLD_RESET };
typedef struct { int act, who, arg; } d1_action;   /* who = the human whose unit the glue touches */

/* flags: CP_SESS_DOWNED (bit 0) and the bleed-out preset (bits 1..2); 0 = the rule is off, nothing else changes */
static inline void d1_init(d1_pair_t *p, unsigned flags, uint32_t now)
{
    dl_cfg c;
    dl_cfg_from_flags(&c, flags);
    for (int k = 0; k < D1_N; k++)
        dl_init(&p->d[k], &c, k, D1_N, now);
    p->on = c.enabled;
}

/* human k is in play (his unit exists) or not (the level is gone, the role is unbound). Machine k itself ignores its own index. */
static inline void d1_set_present(d1_pair_t *p, int k, int present, uint32_t now)
{
    for (int m = 0; m < D1_N; m++)
        if (m != k)
            dl_set_present(&p->d[m], k, present, now);
}

/* the engine killed human k in a way the rule did not soften (hp 0 without an absorbed hit) */
static inline void d1_dead(d1_pair_t *p, int k, uint32_t now)
{
    if (k >= 0 && k < D1_N)
        dl_local_dead(&p->d[k], now);
}

/* a hit of hp-changing damage on human k. Returns the damage the engine should really apply:
 * the rule keeps a lethal hit at 1 hp while it can raise him, and absorbs everything while he is already down. */
static inline float d1_absorb(d1_pair_t *p, int k, float hp, float dmg, uint32_t now)
{
    if (!p->on || k < 0 || k >= D1_N || !(dmg > 0.0f))
        return dmg;
    if (hp - dmg > 0.0f && dl_state(&p->d[k], k) != DL_DOWN)
        return dmg;
    if (!dl_local_lethal(&p->d[k], now))
        return dmg;            /* nobody to raise him, or already dead: an ordinary hit */
    return hp > 1.0f ? hp - 1.0f : 0.0f;
}

/* the hold of human k: holding = the revive button is down, in_range = the other human is close enough. The target is the other human. */
static inline void d1_hold(d1_pair_t *p, int k, int holding, int in_range, uint32_t now)
{
    if (k >= 0 && k < D1_N)
        dl_local_hold(&p->d[k], 1 - k, holding, in_range, now);
}

static inline int d1_is_down(const d1_pair_t *p, int k) { return k >= 0 && k < D1_N && dl_state(&p->d[k], k) == DL_DOWN; }

/* machine k's send -> the other machine (in-process delivery) */
static inline void d1_deliver(d1_pair_t *p, int from, const dl_out *o, uint32_t now)
{
    int to = 1 - from;
    switch (o->kind) {
    case DL_O_SEND_DOWN:      dl_remote_event(&p->d[to], from, DL_E_DOWN, 0, now); break;
    case DL_O_SEND_DEAD:      dl_remote_event(&p->d[to], from, DL_E_DEAD, 0, now); break;
    case DL_O_SEND_BEGIN:     dl_remote_event(&p->d[to], from, DL_E_BEGIN, o->arg, now); break;
    case DL_O_SEND_KEEPALIVE: dl_remote_event(&p->d[to], from, DL_E_KEEPALIVE, o->arg, now); break;
    case DL_O_SEND_CANCEL:    dl_remote_event(&p->d[to], from, DL_E_CANCEL, o->arg, now); break;
    case DL_O_SEND_DONE:      dl_remote_event(&p->d[to], from, DL_E_DONE, o->arg, now); break;
    default: break;
    }
}

/* drains both machines: sends are delivered (repeated until quiet, bounded), the actions for the glue are returned (at most max) */
static inline int d1_pump(d1_pair_t *p, uint32_t now, d1_action *out, int max)
{
    int n = 0;
    for (int round = 0; round < 8; round++) {
        int moved = 0;
        for (int k = 0; k < D1_N; k++) {
            dl_out o;
            while (dl_take(&p->d[k], &o)) {
                moved = 1;
                int act = o.kind == DL_O_GO_DOWN ? D1_A_GO_DOWN : o.kind == DL_O_DIE ? D1_A_DIE : o.kind == DL_O_GET_UP ? D1_A_GET_UP
                        : o.kind == DL_O_REVIVE_PLAY ? D1_A_REVIVE_PLAY : o.kind == DL_O_REVIVE_STOP ? D1_A_REVIVE_STOP
                        : o.kind == DL_O_HOLD_RESET ? D1_A_HOLD_RESET : D1_A_NONE;
                if (act != D1_A_NONE) {
                    if (n < max) out[n].act = act, out[n].who = k, out[n].arg = o.arg, n++;
                } else {
                    d1_deliver(p, k, &o, now);
                }
            }
        }
        if (!moved)
            break;
    }
    return n;
}

/* per pass: the time rules (bleed-out, watchdog), then the deliveries and actions. Call after the holds of this pass. */
static inline int d1_step(d1_pair_t *p, uint32_t now, d1_action *out, int max)
{
    for (int k = 0; k < D1_N; k++)
        dl_tick(&p->d[k], now);
    return d1_pump(p, now, out, max);
}

#endif
