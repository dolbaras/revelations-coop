/*
 * rguard1.h - fix021 candidate (the lead's question after f21s5 07:03): an enemy COPY (kind 3: the client's replica of a host enemy, the host's reverse replica
 * of an enemy the client runs) is never killed by THIS PC's fall-out. Pure (no Windows, no engine): local1_game.inc feeds it every pass, tests/test_rguard1.c
 * drives it on a model of the engine's fall-out timer.
 *
 * FACTS (f21s5 l17pair-20261113-070321 + guest log 20261113-070331; allep4u; notes/allep-live.md):
 *  - FUN_006F16B0: while a move object has no ground (+0x2B5 = 1, +8 = 1) its timer +0x2B8 grows by the frame time, otherwise it is set to 0; at 5.0 s
 *    (0x109AB78) FUN_006F0660 kills the unit (state 0x32, hp 0, no damage line, its arrangement record used up). Only a human of another slot is spared:
 *    an enemy copy dies like an enemy.
 *  - A copy runs that update only while it is awake on this PC: the stock zone pass wakes the own human's zone, zkeep1.h rule 7 (client) and watch1.h rule 4
 *    (watching PC) keep host-simulated copies awake within 3000 u in OTHER zones too. Each PC streams only the stage parts under its own human (stage1.h), so
 *    an awake copy can stand over floor this PC never loaded: it falls, op_update's snap (300 u) puts it back at the owner's height over the same missing
 *    floor, the timer never resets (no ground) and 5 s later this PC kills it: f21s5 07:11:36.244 'kill record ... victim 24dfaef0 ... by human slot -1',
 *    07:11:36.317 '2/3 GONE ... record used up 1', while the host's 2/3 kept hp 1000 (07:11:58): the guest's PC never shows that enemy again.
 * THE RULE: every pass, on both PCs, the fall-out timer of every kind-3 enemy copy is held at 0, so its death is decided only by the PC that runs it (the DEATH
 * record, local1.h / hitx1.h). Nothing else changes: a copy over missing floor still falls and snaps (seen, not killed); the PC's own enemies (kind 2) and
 * the humans keep the stock fall-out (a pit still kills the enemy on the PC that runs it, and that death reaches the copy). Switch: `world set rguard 0`.
 */
#ifndef RGUARD1_H
#define RGUARD1_H

#define RG_KIND_COPY 3u   /* unit +0xE30: 3 = a copy driven by the other PC (the replica / the reverse replica) */

/* one unit in one pass: 1 = write 0 to its move object's fall-out timer now (on = the switch, kind = unit +0xE30, timer = move object +0x2B8) */
static inline int rg_hold(int on, unsigned kind, float timer)
{
    return on && kind == RG_KIND_COPY && timer > 0.0f;
}

#endif
