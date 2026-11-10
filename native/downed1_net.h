/* downed1_net.h: the NETWORK side of the Revelations 1 "downed / revive" rule (two PCs, one human each). Pure C: no engine, no Windows, no session code.
 *
 * Each PC runs ONE machine of the Rev2 model (downed_logic.h) with ME = the unit slot of its own human (0 lead / host, 1 partner / client); the partner's machine runs on the
 * partner's PC. What a machine sends (DOWN, DEAD, BEGIN, KEEPALIVE, CANCEL, DONE) travels as a reliable CP_EVENT of kind CP_EV_D1 (0x142 = CP_EV_USER + 0x42, free in this tree:
 * 0x140 world, 0x141 role barrier). The authority is the Rev2 one: every PC decides DOWN / bleed-out / DEAD for ITS OWN human; the rescuer's hold is measured on the rescuer's PC
 * (his input, his distance to the partner's puppet) and reaches the downed PC as BEGIN / KEEPALIVE / DONE; the downed PC runs the get-up of its own human.
 *
 * What the glue (native/downed1.c) must do comes back as d1n_action:
 *   own human:  GO_DOWN (hp 1, knock-down, frozen get-up, input ban), DIE (campaign death state 0x32), GET_UP (the frozen get-up plays on, hp 35 %), REVIVE_PLAY / STOP, HOLD_RESET
 *   puppet:     PUP_DOWN (the partner went down: the same knock-down + frozen get-up on his puppet here), PUP_UP (revived: the puppet gets up), PUP_DIE (the partner is dead: his
 *               puppet enters the death state 0x32 here too, so both PCs show both humans dead)
 *   wire:       SEND (msg/len = the event body for cs_send_event)
 * The game over is coupled by the model itself: a DEAD partner (event, or his predicted bleed-out after a grace) makes dl_check_over ask for DIE of the own human on this PC.
 *
 * Wire body (6 bytes): version 1, epoch (low byte of the session's level instance), event kind (DL_E_*), from (sender's slot), arg (target slot, 0xFF = none), 0 (reserved).
 * An event of another epoch is refused (a late message of the previous level instance must not put the new one down). Offline replay: tests/test_downed_net.c.
 */
#ifndef DOWNED1_NET_H
#define DOWNED1_NET_H

#include "downed_logic.h"

#define CP_EV_D1_KIND 0x142u
#define D1N_MSG_LEN 6
#define D1N_VERSION 1

/* The rescuer's hold (round 4, wp7 live: the host held A 3.8 s 105 units from the client's lying puppet; BEGIN, KEEPALIVE, CANCEL within 0.73 s, the client bled out).
 * Static facts of that spot (census 91090200 + the scenario's teleport): the host's human stood ~97 units from the 9-2 uWeaponBox (-45,-1250,2267) and the stock A of the game
 * (the action command of the nearest interactable) took the same press: the human walks to the box / its menu opens, the hold sees A or the range go. INFERRED until the
 * live A/B (`downed revive old|new`). The rule therefore:
 *   - owns the revive button while the partner lies within reach of a standing own human (the glue hides A from the game then, testmode_pad_mask): the stock prompt cannot
 *     take the press (RE5 / RE6: the rescue prompt has priority next to a dying partner);
 *   - measures the reach horizontally with a floor test (|dy|), and a running hold keeps going up to D1N_REACH_KEEP (the rescuer's small moves, the lying body's root);
 *   - bridges a gap shorter than D1N_HOLD_GRACE_MS (one pass without the button / the range ended the hold before); the gap is not counted as progress. */
#define D1N_REACH 170.0f          /* start of a hold (= D1_REACH of the local rule) */
#define D1N_REACH_KEEP 230.0f     /* a running hold stays in range up to this */
#define D1N_REACH_DY 150.0f       /* same floor */
#define D1N_HOLD_GRACE_MS 200u

enum d1n_act {
    D1N_A_NONE = 0, D1N_A_GO_DOWN, D1N_A_DIE, D1N_A_GET_UP, D1N_A_REVIVE_PLAY, D1N_A_REVIVE_STOP, D1N_A_HOLD_RESET,
    D1N_A_PUP_DOWN, D1N_A_PUP_UP, D1N_A_PUP_DIE, D1N_A_SEND, D1N_A_GAME_OVER /* informational: the rule decided the run is over (arg 1 = somebody dead) */
};
typedef struct { int act, who, arg, len; uint8_t msg[D1N_MSG_LEN]; } d1n_action;

typedef struct {
    dl_t d;
    int me, other, on;
    unsigned epoch;            /* the level instance the machine belongs to */
    int have_epoch;
    uint32_t tx, rx, rx_stale, rx_bad, rx_foreign;
    uint32_t gap_since;        /* the running hold lost the button / the range at this time (0 = no gap) */
    int legacy;                /* lab A/B (`downed revive old`): the 02a375b hold (3-D reach 170, no grace; the glue: game word only, no button priority) */
    uint32_t hold_gaps, hold_bridged, hold_cancels;
} d1n_t;

static inline const char *d1n_act_name(int a)
{
    static const char *n[] = {"none", "go-down", "die", "get-up", "revive-play", "revive-stop", "hold-reset", "puppet-down", "puppet-up", "puppet-die", "send", "game-over"};
    return a >= 0 && a <= D1N_A_GAME_OVER ? n[a] : "?";
}
static inline const char *d1n_ev_name(int k)
{
    static const char *n[] = {"?", "DOWN", "DEAD", "BEGIN", "KEEPALIVE", "CANCEL", "DONE"};
    return k >= DL_E_DOWN && k <= DL_E_DONE ? n[k] : "?";
}

/* flags: CP_SESS_DOWNED + bleed preset (downed_logic.h); me = the own human's slot (0 / 1) */
static inline void d1n_init(d1n_t *n, unsigned flags, int me, uint32_t now)
{
    dl_cfg c;
    dl_cfg_from_flags(&c, flags);
    memset(n, 0, sizeof *n);
    n->me = me ? 1 : 0, n->other = 1 - n->me;
    dl_init(&n->d, &c, n->me, 2, now);
    n->on = c.enabled;
}

/* the session's level instance: a new one starts every human up (a load, a checkpoint, the Continue after a game over) */
static inline int d1n_epoch(d1n_t *n, unsigned epoch, uint32_t now)
{
    if (n->have_epoch && n->epoch == epoch) return 0;
    n->epoch = epoch, n->have_epoch = 1;
    dl_reset(&n->d, now);
    return 1;
}

/* the partner is in the session with a unit on this PC (debounced by the glue) */
static inline void d1n_present(d1n_t *n, int present, uint32_t now) { dl_set_present(&n->d, n->other, present, now); }

/* a hit of hp-changing damage on the OWN human: the damage the engine should really apply (a lethal hit leaves hp 1 while the partner can raise him; while down: absorbed) */
static inline float d1n_absorb(d1n_t *n, float hp, float dmg, uint32_t now)
{
    if (!n->on || !(dmg > 0.0f)) return dmg;
    if (hp - dmg > 0.0f && dl_state(&n->d, n->me) != DL_DOWN) return dmg;
    if (!dl_local_lethal(&n->d, now)) return dmg;
    return hp > 1.0f ? hp - 1.0f : 0.0f;
}

static inline void d1n_dead(d1n_t *n, uint32_t now) { dl_local_dead(&n->d, now); }
/* the own human stands again although the machine says DOWN (a heal or the engine's own rescue): the machine follows */
static inline void d1n_up(d1n_t *n, uint32_t now) { dl_local_up(&n->d, now); }
static inline int d1n_hold_running(const d1n_t *n) { return n->d.h[n->me].hold_target >= 0; }

/* the reach of the own human to the partner's puppet on THIS PC (dx dy dz = puppet - own): horizontal, same floor; a running hold keeps going up to D1N_REACH_KEEP.
 * Legacy (`downed revive old`): the 02a375b test, 3-D distance <= 170. */
static inline int d1n_in_reach(const d1n_t *n, float dx, float dy, float dz)
{
    if (n->legacy) return dx * dx + dy * dy + dz * dz <= D1N_REACH * D1N_REACH;
    float r = d1n_hold_running(n) ? D1N_REACH_KEEP : D1N_REACH;
    return (dy < 0 ? -dy : dy) <= D1N_REACH_DY && dx * dx + dz * dz <= r * r;
}

/* the revive owns the button now: the partner lies (mirror DOWN), the own human is up and within reach (the glue hides the button from the game, so the stock action of a
 * nearby interactable cannot take the press). Never in the legacy mode. */
static inline int d1n_owns_button(const d1n_t *n, int in_reach)
{
    return n->on && !n->legacy && in_reach && dl_state(&n->d, n->me) == DL_ALIVE && dl_state(&n->d, n->other) == DL_DOWN;
}

/* the revive button of this pass: game_a = the game's held word of the own pad slot (keyboard included), raw_a = the own pad's A before the mask (the game does not
 * see it while the revive owns it), in_play = no pause / menu / cutscene / load on this PC. Legacy: the game's word only (02a375b). */
static inline int d1n_button(const d1n_t *n, int game_a, int raw_a, int in_play)
{
    if (n->legacy) return game_a != 0;
    return game_a || (raw_a && in_play);
}

/* the own human's hold on the partner's puppet: holding = the revive button, in_range = d1n_in_reach on THIS PC. A running hold survives a gap shorter than
 * D1N_HOLD_GRACE_MS (not in the legacy mode); the passes of the gap add no progress. */
static inline void d1n_hold(d1n_t *n, int holding, int in_range, uint32_t now)
{
    dl_human *m = &n->d.h[n->me];
    int ok = holding && in_range;
    if (!n->legacy && m->hold_target >= 0 && dl_state(&n->d, m->hold_target) == DL_DOWN && m->state == DL_ALIVE) {
        if (!ok) {
            if (!n->gap_since) n->gap_since = now ? now : 1, n->hold_gaps++;
            if (dl_diff(now, n->gap_since) < (int32_t)D1N_HOLD_GRACE_MS) {
                m->hold_t = now;   /* the gap is no progress */
                return;
            }
        } else if (n->gap_since) {
            n->gap_since = 0, n->hold_bridged++;
        }
    }
    int was = m->hold_target >= 0;
    dl_local_hold(&n->d, n->other, holding, in_range, now);
    if (was && m->hold_target < 0 && dl_state(&n->d, n->other) == DL_DOWN) n->hold_cancels++;
    if (m->hold_target < 0) n->gap_since = 0;
}

/* the partner's newest flags (unreliable): repairs a lost DOWN / DONE after DL_TRUST_MS. Never while the mirror says DEAD (wp6 live: 600 ms after a coupled game over the
 * flags of the dead partner said "not down" and the mirror came back to life, the puppet got an UP: a dead human only comes back with a new level instance, d1n_epoch) */
static inline void d1n_sample(d1n_t *n, int down, uint32_t now)
{
    if (dl_state(&n->d, n->other) == DL_DEAD) return;
    dl_remote_sample(&n->d, n->other, down, 0, now);
}

static inline int d1n_own_down(const d1n_t *n) { return n->on && dl_state(&n->d, n->me) == DL_DOWN; }
static inline int d1n_partner_down(const d1n_t *n) { return n->on && dl_state(&n->d, n->other) == DL_DOWN; }

static inline int d1n_encode(const d1n_t *n, int kind, int from, int arg, uint8_t out[D1N_MSG_LEN])
{
    out[0] = D1N_VERSION, out[1] = (uint8_t)(n->epoch & 0xFFu), out[2] = (uint8_t)kind, out[3] = (uint8_t)from, out[4] = (uint8_t)(arg < 0 ? 0xFF : arg), out[5] = 0;
    return D1N_MSG_LEN;
}

/* an event body from the wire. 1 = applied, 0 = refused (counted: bad / stale epoch / not from the partner) */
static inline int d1n_recv(d1n_t *n, const uint8_t *b, unsigned len, uint32_t now)
{
    if (!b || len < 5 || b[0] != D1N_VERSION || b[2] < DL_E_DOWN || b[2] > DL_E_DONE) { n->rx_bad++; return 0; }
    if (!n->have_epoch || b[1] != (uint8_t)(n->epoch & 0xFFu)) { n->rx_stale++; return 0; }
    int from = b[3], arg = b[4] == 0xFF ? -1 : b[4];
    if (from != n->other) { n->rx_foreign++; return 0; }
    n->rx++;
    dl_remote_event(&n->d, from, b[2], arg, now);
    return 1;
}

/* per pass: the timers (bleed-out, watchdog, the partner's predicted bleed-out), then what the glue must do (at most max actions, in order) */
static inline int d1n_step(d1n_t *n, uint32_t now, d1n_action *out, int max)
{
    int k = 0;
    dl_tick(&n->d, now);
    dl_out o;
    while (dl_take(&n->d, &o)) {
        d1n_action a;
        memset(&a, 0, sizeof a);
        a.who = o.who, a.arg = o.arg;
        switch (o.kind) {
        case DL_O_GO_DOWN: a.act = D1N_A_GO_DOWN; break;
        case DL_O_DIE: a.act = D1N_A_DIE; break;
        case DL_O_GET_UP: a.act = D1N_A_GET_UP; break;
        case DL_O_REVIVE_PLAY: a.act = D1N_A_REVIVE_PLAY; break;
        case DL_O_REVIVE_STOP: a.act = D1N_A_REVIVE_STOP; break;
        case DL_O_HOLD_RESET: a.act = D1N_A_HOLD_RESET; break;
        case DL_O_PUPPET_DOWN: a.act = D1N_A_PUP_DOWN; break;
        case DL_O_PUPPET_UP: a.act = dl_state(&n->d, o.who) == DL_DEAD ? D1N_A_PUP_DIE : D1N_A_PUP_UP; break;   /* the model "un-lies" a dead partner; here he dies on this PC too */
        case DL_O_GAME_OVER: a.act = D1N_A_GAME_OVER; break;
        case DL_O_SEND_DOWN: a.act = D1N_A_SEND, a.len = d1n_encode(n, DL_E_DOWN, n->me, -1, a.msg); break;
        case DL_O_SEND_DEAD: a.act = D1N_A_SEND, a.len = d1n_encode(n, DL_E_DEAD, n->me, -1, a.msg); break;
        case DL_O_SEND_BEGIN: a.act = D1N_A_SEND, a.len = d1n_encode(n, DL_E_BEGIN, n->me, o.arg, a.msg); break;
        case DL_O_SEND_KEEPALIVE: a.act = D1N_A_SEND, a.len = d1n_encode(n, DL_E_KEEPALIVE, n->me, o.arg, a.msg); break;
        case DL_O_SEND_CANCEL: a.act = D1N_A_SEND, a.len = d1n_encode(n, DL_E_CANCEL, n->me, o.arg, a.msg); break;
        case DL_O_SEND_DONE: a.act = D1N_A_SEND, a.len = d1n_encode(n, DL_E_DONE, n->me, o.arg, a.msg); break;
        default: continue;
        }
        if (a.act == D1N_A_SEND) n->tx++;
        if (k < max) out[k++] = a;
    }
    return k;
}

#endif
