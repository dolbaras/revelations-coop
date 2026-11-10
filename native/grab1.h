/*
 * grab1.h - E11 (ep1-solo): the own human held by an enemy (an Ooze bite, a Wall Blister, a Globster eat) in a two-human network game. Pure, no game.
 * Evidence: notes\ep1-solo.md section 5 (static, build 13298487).
 *
 * The engine: a grab is a direct ChangeState of the victim's state manager into a "PlayerSide" state (parent GrappledStateBase 0x117A6C8) by the enemy's own
 * grab update (zombies 0x5A88C0); the bite enter 0x5A9020 sets victim+0xE34 = 1, the bite exit 0x5A99E0 clears it. Every grab state of the game writes
 * +0xE34 of its victim (FACT: the writers `mov [reg+0xE34], 1` in the enemy state code). The break-free QTE reads the raw pad of the current slot (0x412600),
 * so it belongs to the PC whose human is held; on the other PC that human is a puppet driven by his pad records: the waggle would walk the puppet away
 * from the grabbing replica (and the position correction would pull it back).
 * The rule: while the own human's +0xE34 is set, this PC's pad / STATE flags carry CP_SF_HELD; the partner's receiver holds the puppet neutral (no pad replay,
 * no position correction) until it clears; one teleport afterwards. A held word that never clears is dropped after cap_ms (a stuck field must not freeze the
 * partner's puppet for good).
 */
#ifndef GRAB1_H
#define GRAB1_H
#include <stdint.h>

#define GR1_OFF_HELD 0xE34u        /* player unit: held by an enemy (0 = free) */
#define GR1_CAP_MS 20000u          /* longest grab the rule believes (a full Ooze bite chain lasts a few seconds) */

typedef struct {
    uint32_t unit;                 /* the own human the state is for (a new unit = a new level: starts over) */
    int held;                      /* the flag is sent now */
    uint32_t since;
    int capped;                    /* the field stayed set longer than the cap: ignored until it clears */
    unsigned grabs, caps;
} gr1_t;

/* one call per pass with the own human's +0xE34; returns 1 while CP_SF_HELD is to be sent */
static inline int gr1_step(gr1_t *g, uint32_t unit, uint32_t e34, uint32_t now, uint32_t cap_ms)
{
    if (unit != g->unit) g->unit = unit, g->held = 0, g->capped = 0;
    int set = unit && e34 != 0;
    if (!set) {
        g->held = 0, g->capped = 0;
        return 0;
    }
    if (g->capped) return 0;
    if (!g->held) g->held = 1, g->since = now, g->grabs++;
    if ((uint32_t)(now - g->since) > cap_ms) {
        g->held = 0, g->capped = 1, g->caps++;
        return 0;
    }
    return 1;
}

#endif
