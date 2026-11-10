/*
 * zkeep1.h - rev1 live round 17: a replica the host simulates stays visible and hittable on the client across the client's OWN zone passes. Pure (no Windows,
 * no engine): watch1_game.inc (the list for the zone-pass hook) and local1_game.inc (the zone-leave hook) use it, tests/test_zkeep1.c drives it on a model of
 * the client's zone pass and its shots in both role orders.
 *
 * FACTS (notes/ep1-flow.md "RESUME HERE (round 17"):
 *  - m16a1 (R7, the guest = Jill), m16a3 (lead order, the guest = Parker), m15c4, m14g: the guest's human followed the host's Ooze 2/0 out of zone 256/7 into
 *    257/1 during the fight; the guest's log `world: enemy: zone leave (FUN_0044F510) 2/0 ... kind 3 hp 2100 E1C 40001088 state type 1 kill 1`, then
 *    `E1C 40000088 -> 40001088`; every shot went through it (`HITX sent 0` for 25 tries) until a reload. The host's Ooze stayed awake (its own human shared
 *    the zone: host E1C 40000088), so the host still simulated it and its records said so.
 *  - FUN_00460250 (every frame) -> FUN_004592E0(sArrange, own zone now, own zone before): for every unit with a zone that is NOT the own human's zone and whose
 *    move object says "was in a zone" (+0xD) while E1C 0x1000 is clear: the zombie classes 0x80031000/10/40/60 stay visible (E1C |= 0x10) while
 *    uZombieBase_vf101 (0x5B5DA0: the current state == DAT_01172ABC) says so; otherwise E1C |= 0x1000 (hidden / asleep: no drawing, no shot) and, when the unit
 *    is in the zone the own human just left, FUN_0044F510(unit, 1): the unit's reset state (uZombieBase vf 0xDC 0x5B5EF0: state 3 or 0x5B; for some state
 *    types also a move to the record's / FUN_00448D50's point); a corpse (state type 12) is killed there (vf 0x3C) and its record used up. The own zone's units
 *    get 0x1000 (and 0x10) cleared. A hidden unit is hidden again in every frame until the own human enters its zone.
 *  - So the stock rule "an enemy chasing the player stays awake across zones" holds for the host's AI unit but not for the client's replica (kind 3 runs no AI
 *    state of its own): the client's engine sleeps an enemy the host is still fighting with.
 * THE RULE (rule 7 of the replicas; both role orders):
 *  7. the CLIENT keeps every replica (kind 3) that is alive, not finished by the host's DEATH (local1.h lr_on_death), whose host records say the host simulates
 *     it (fresh, a real ai, the host's E1C not hidden: local1.h lr_host_simulates) and that is within ZK_NEAR of its own active human: right after the zone
 *     pass the hidden bit is cleared again (watch1.h rule 4's way) and the pass's reset of it (FUN_0044F510 kill 1 from inside the pass) is skipped (the host's
 *     AI drives it; a corpse's kill is never skipped). Nothing else changes: an enemy the host does not simulate is slept by the client's engine as before and
 *     local1.h rule 3 decides whether the client runs it; a watching PC keeps using rule 4.
 * Switch: `world set zkeep 0` = the model before the fix.
 */
#ifndef ZKEEP1_H
#define ZKEEP1_H

#define ZK_NEAR 3000.0f            /* the client keeps host-simulated replicas this close to its own human (the take distance of local1.h, LR_NEAR) */
#define ZK_STATE_CORPSE 12u        /* FUN_0049C430 state type of a corpse: FUN_0044F510 kills it and uses its record up (never skipped) */

/* rule 7: keep this replica visible on this PC right after its zone pass */
static inline int zk_keep(int on, int is_client, int watching, unsigned kind, int alive, int host_sim, int dead_reported, float dist_own)
{
    return on && is_client && !watching && kind == 3 && alive && host_sim && !dead_reported && dist_own <= ZK_NEAR;
}

/* rule 7: skip the zone pass's reset (FUN_0044F510 with kill 1) of a kept replica; only inside the zone pass, never for a corpse */
static inline int zk_skip_reset(int on, int listed_keep, int in_pass, int kill, unsigned state_type)
{
    return on && listed_keep && in_pass && (kill & 0xFF) == 1 && state_type != ZK_STATE_CORPSE;
}

#endif
