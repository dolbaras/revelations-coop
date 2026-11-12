/*
 * fallres1.h - allep step 4: the FALL-OUT RESCUE of the own human. Pure (no Windows, no engine): local1_game.inc feeds it every pass, tests/test_fallres1.c drives it.
 *
 * FACTS (notes/allep-live.md "allep4u", lab run 2026-11-11, the hp write breakpoint named the chain):
 *  - FUN_006F16B0 (the move object's fall-out timer): while the move object has NO GROUND (flag +0x2B5 = 1, +8 = 1) the timer +0x2B8 grows with the frame time; at 5.0 s
 *    (constant 0x109AB78) FUN_006F0660 kills the unit (state 0x32 = DeadWait, Die() writes hp 0, no damage line). FUN_006F0660 spares every human whose slot is not the
 *    engine's own slot (FUN_00882D60): only the OWN human of a PC is killed.
 *  - The stage streams by a cast under the own human (stage1.h): a human placed where the story put the AI partner (2-2: Parker at (-22.6, 500, -2666), another stage than Jill's)
 *    has no floor, hovers and is killed exactly 5 s later. The same happens after a teleport into a block whose collision is not loaded (1-3 hops) and after the restart starts.
 * THE RULE: an own human that has had no ground for FRS_AFTER_MS without losing height (a placement problem; a FALLING human is not rescued: a pit still kills) is put next to the
 * partner (the partner's puppet / unit, which stands on a loaded floor) once, with a cooldown, and its fall-out timer is cleared. The glue does the teleport with the move
 * object's teleport byte (as op_update) and zeroes +0x2B8.
 */
#ifndef FALLRES1_H
#define FALLRES1_H

#include <stdint.h>
#include <math.h>
#include <string.h>

#define FRS_AFTER_MS 1500u       /* no ground for this long before the rescue (the engine kills at 5000 ms) */
#define FRS_FALL_TOL 30.0f       /* the height may change by this much in that time: more = the unit is falling, the engine's death stays */
#define FRS_COOLDOWN_MS 10000u   /* after a rescue: no second one for this long */

typedef struct {
    int armed;               /* 1 = a no-ground spell is being timed */
    uint32_t since_ms, last_rescue_ms;
    float y0;
    unsigned rescues, falling_left;
} frs_t;

static inline void frs_reset(frs_t *f) { memset(f, 0, sizeof *f); }

/* one pass. noground = the own human's move object says no ground (+0x2B5 == 1 and +8 == 1); y = its height; partner_ok = the partner's unit is there to go to; now = ms (nonzero).
 * Returns 1 = rescue NOW (teleport next to the partner, clear +0x2B8). */
static inline int frs_step(frs_t *f, int noground, float y, int partner_ok, uint32_t now)
{
    if (!noground) {
        f->armed = 0;
        return 0;
    }
    if (!f->armed) {
        f->armed = 1, f->since_ms = now, f->y0 = y;
        return 0;
    }
    if (fabsf(y - f->y0) > FRS_FALL_TOL) {   /* it is falling / moving vertically: not a placement problem: restart the spell, no rescue (a pit kills as before) */
        f->falling_left++;
        f->since_ms = now, f->y0 = y;
        return 0;
    }
    if ((uint32_t)(now - f->since_ms) < FRS_AFTER_MS || !partner_ok) return 0;
    if (f->last_rescue_ms && (uint32_t)(now - f->last_rescue_ms) < FRS_COOLDOWN_MS) return 0;
    f->last_rescue_ms = now, f->rescues++, f->armed = 0;
    return 1;
}

/* where the human goes: beside the partner (a fixed offset away so that the two capsules do not overlap), same height */
static inline void frs_target(const float partner[3], float out[3])
{
    out[0] = partner[0] + 70.0f, out[1] = partner[1], out[2] = partner[2] + 70.0f;
}

#endif
