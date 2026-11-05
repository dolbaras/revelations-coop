/*
 * Pad routing of the Revelations 1 mod: which physical XInput pad the game's user N means, hidden duplicates, who may read a real pad. Pure C, no Windows calls: the rules are
 * tested offline (tests/test_padmap.c); testmode.c feeds it with the real XInput states.
 *
 *  - The game polls XInput users 0..3 and gives pad slot k to the k-th connected user. So the mod presents the game with a COMPACT list: game user k = the k-th usable
 *    physical pad (R1-2: virtual pads / a pad that was unplugged leave a gap in the numbering, e.g. pads on users 0 and 2; the old code swapped user 0 with user 1 blindly).
 *  - "Exchanged pads" (lobby: player 1 uses gamepad 2; the second human's menu) swaps the first two game users. The SAME routing answers XInputGetState, XInputGetCapabilities,
 *    XInputSetState (rumble, R1-3) and the mod's own peek (R1-1: BACK / START of the second human); before, each of them did its own thing.
 *  - A pad that Windows shows twice (Steam Input / DS4Windows / a remapper next to the physical pad) must not become two players (R1-4 / J1): a pair of users that keep
 *    sending the SAME state while some stick axis is deflected is a mirror; the higher user of the pair is hidden from the game, the mod and the lobby count.
 *  - The mod's own readers (lobby, BACK / START of the second human) read a real pad only while the game window is in front (R1-5): a synthetic pad of the test console
 *    does not need the window.
 */
#ifndef R1_PADMAP_H
#define R1_PADMAP_H
#include <string.h>
#include "coop_input.h"

#define R1_PM_USERS 4

typedef struct {
    int n;                    /* usable pads */
    int phys[R1_PM_USERS];    /* phys[k] = physical XInput user of game user k (k < n) */
} r1_padlist_t;

/* usable pads = connected and not a hidden duplicate; ascending physical user */
static inline void r1_padlist_build(r1_padlist_t *l, unsigned conn, unsigned dup)
{
    l->n = 0;
    for (int u = 0; u < R1_PM_USERS; u++) {
        l->phys[u] = -1;
        if (((conn >> u) & 1u) && !((dup >> u) & 1u))
            l->phys[l->n++] = u;
    }
}

/* the exchange of the first two game users (synthetic pads of the console are numbered by the same slot) */
static inline int r1_pad_slot(int game_user, int swap)
{
    return swap && game_user >= 0 && game_user < 2 ? game_user ^ 1 : game_user;
}

/* the physical XInput user behind a game slot, -1 = nobody (not connected / a hidden duplicate / more users than pads) */
static inline int r1_pad_phys(const r1_padlist_t *l, int slot)
{
    return slot >= 0 && slot < l->n ? l->phys[slot] : -1;
}

/* game user -> physical user in one step (XInputGetState / SetState / GetCapabilities / peek) */
static inline int r1_pad_route(const r1_padlist_t *l, int game_user, int swap)
{
    if (game_user < 0 || game_user >= R1_PM_USERS)
        return -1;
    return r1_pad_phys(l, r1_pad_slot(game_user, swap));
}

/* may the mod read a REAL pad for its own purposes (lobby, second human's BACK / START)? a synthetic (console) pad always may */
static inline int r1_pad_peek_allowed(int synthetic_active, int pads_allowed_test, int window_in_front)
{
    return synthetic_active || pads_allowed_test || window_in_front;
}

/* ---- duplicate detection ---- */
#define R1_DUP_MIN_SAME 60        /* identical CHANGES of the state (a moving stick changes it at every sample: about half a second) before a pair counts as a mirror */
#define R1_DUP_MAX_RATIO 8        /* a mirror may differ in at most 1 of 8 informative samples (a transfer delay of a virtual pad) */
#define R1_DUP_HALVE 1024         /* counters are halved when they sum up to this (old evidence fades) */

typedef struct {
    unsigned short same[R1_PM_USERS][R1_PM_USERS], diff[R1_PM_USERS][R1_PM_USERS];
    unsigned char mirror[R1_PM_USERS][R1_PM_USERS];
    r1_input_t prev[R1_PM_USERS][R1_PM_USERS];   /* the last identical informative state of the pair: a constant state is no evidence (virtual test pads rest at the same odd value) */
} r1_dupdet_t;

static inline int r1_dup_neutral(const r1_input_t *a)
{
    return !a->buttons && !a->lt && !a->rt && !a->lx && !a->ly && !a->rx && !a->ry;
}

/* an identical state that carries analog evidence (and, see r1_dupdet_feed, that has CHANGED since the last such sample): a stick axis off zero but not pinned at the rim (two humans pushing both sticks to the stop would be identical too) */
static inline int r1_dup_informative(const r1_input_t *a)
{
    short v[4] = {a->lx, a->ly, a->rx, a->ry};
    for (int i = 0; i < 4; i++)
        if (v[i] != 0 && v[i] > -32000 && v[i] < 32000)
            return 1;
    return 0;
}

static inline void r1_dupdet_reset(r1_dupdet_t *d) { memset(d, 0, sizeof *d); }

/* One round of samples of every connected user (st[u] valid where conn has bit u). Returns the mask of users hidden as duplicates (the higher user of each mirrored pair). */
static inline unsigned r1_dupdet_feed(r1_dupdet_t *d, const r1_input_t st[R1_PM_USERS], unsigned conn)
{
    for (int a = 0; a < R1_PM_USERS; a++)
        for (int b = a + 1; b < R1_PM_USERS; b++) {
            if (!(((conn >> a) & 1u) && ((conn >> b) & 1u))) {   /* a pad that is gone forgets its history */
                d->same[a][b] = d->diff[a][b] = d->mirror[a][b] = 0;
                memset(&d->prev[a][b], 0, sizeof d->prev[a][b]);
                continue;
            }
            int eq = memcmp(&st[a], &st[b], sizeof(r1_input_t)) == 0;
            if (eq && r1_dup_informative(&st[a])) {
                if (memcmp(&st[a], &d->prev[a][b], sizeof(r1_input_t)) != 0)
                    d->same[a][b]++;
                d->prev[a][b] = st[a];
            } else if (!eq && !(r1_dup_neutral(&st[a]) && r1_dup_neutral(&st[b])))
                d->diff[a][b]++;
            if ((unsigned)d->same[a][b] + d->diff[a][b] >= R1_DUP_HALVE)
                d->same[a][b] >>= 1, d->diff[a][b] >>= 1;
            if (d->mirror[a][b]) {
                if ((unsigned)d->diff[a][b] * 4u > d->same[a][b])   /* no longer alike: two pads after all */
                    d->mirror[a][b] = 0;
            } else if (d->same[a][b] >= R1_DUP_MIN_SAME && (unsigned)d->diff[a][b] * R1_DUP_MAX_RATIO <= d->same[a][b])
                d->mirror[a][b] = 1;
        }
    unsigned hide = 0;
    for (int b = 1; b < R1_PM_USERS; b++)
        for (int a = 0; a < b; a++)
            if (d->mirror[a][b] && !((hide >> a) & 1u))
                hide |= 1u << b;
    return hide;
}

#endif
