/*
 * wgate1.h - fix021 (0.2.1): the world's work of a level instance waits until THIS PC's own level is up. Pure (no Windows, no engine); world1_game.c runs it,
 * tests/test_wgate1.c drives it.
 *
 * FACTS (fw1 on de2598fc 2026-11-12 23:04 and its repro on 5848e1d2 23:53, the guest's game ended at the 1-2 start with the assert '(reg < Region::RegionNum)'
 * of uEnemyBase.cpp 0x580): the session's load barrier of the first level instance counts from the session START; the title (93 s / 150 s) and the 1-2
 * intro movie (~207 s) passed its 300 s timeout, so the host opened it ("barrier timeout, player 0 / 1 never reported loaded") while BOTH PCs were still in
 * the movie. Phase PLAYING then ran the world model on a PC whose level had not started: at the level start (the host 1.5 s ahead after the level-start
 * hold) the host's SPAWN of the 1-2 Oozes (block 2) reached the guest before the guest's own scene script had placed them, op_spawn placed them the
 * model's way, and 22 ms later the enemy 2/0 asserted. In the six good 1-2 starts (allep3c, sc1n1, sc1n2, fr6, dw1, fw1 b2db38bb) the guest's own
 * SpawnChara of 2/0 came 35-38 ms after the 12_005_0 cutscene start and the barrier opened (after LOAD_DONE) about 140 ms later; in both crashes there is
 * no own SpawnChara line at all and the unit appeared 23 ms after the cutscene start (repro).
 * THE RULE: in a level instance the world model (receiving the other PC's events and batches, sending, the passes) runs on a PC only when its own level is
 * up: this PC reported LOAD_DONE for that epoch (the usual order: nothing changes), or, when the barrier opened before that (its timeout), this PC's human
 * has been placed by the stock level start for WG_UP_MS (the start scene places its own enemies within ~0.4 s of the placement). Events not taken are not
 * acknowledged: the sender repeats them (W1_RTO_MS) until this PC is up.
 */
#ifndef WGATE1_H
#define WGATE1_H

#include <stdint.h>

#define WG_UP_MS 2000u

typedef struct {
    int valid, up;
    unsigned epoch;
    uint32_t placed_ms;
} wg_t;

enum { WG_HOW_NONE = 0, WG_HOW_LOADED = 1, WG_HOW_PLACED = 2 };

/* one frame of the level instance `epoch`: 1 = this PC's level is up (the world's work runs); *how = WG_HOW_* the moment it became up (else 0) */
static inline int wg_step(wg_t *g, unsigned epoch, int reported_loaded, int placed, uint32_t now, int *how)
{
    if (how) *how = WG_HOW_NONE;
    if (!g->valid || g->epoch != epoch) g->valid = 1, g->epoch = epoch, g->up = 0, g->placed_ms = 0;
    if (g->up) return 1;
    if (reported_loaded) {
        g->up = 1;
        if (how) *how = WG_HOW_LOADED;
        return 1;
    }
    if (!placed) {
        g->placed_ms = 0;
        return 0;
    }
    if (!g->placed_ms) g->placed_ms = now ? now : 1;
    if ((uint32_t)(now - g->placed_ms) < WG_UP_MS) return 0;
    g->up = 1;
    if (how) *how = WG_HOW_PLACED;
    return 1;
}

#endif
