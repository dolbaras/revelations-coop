/*
 * sfobj1.h - sync022 (the lead's double filter, 2026-11-15): which stores of an object-group flag (sf1.c: read by the scripts of Episodes 2-12, set by NO script)
 * may cross to the other PC. Pure: the glue (story1_game.inc) hands in the return address of the SetScenarioFlag call (and, for the object flag setter, the
 * return address of ITS caller, read from the stack) and the class of the object that owns the flag; tests/test_sf1.c part O drives it.
 *
 * FACTS (Ghidra work\ghidra-sync022, <lab>\re1q\s22\o15.txt / o16.txt; the call sites from the image):
 *   - FUN_0067DB80 (thiscall(object; value, idx)) = SetScenarioFlag(object+0xF18 + idx, value): the world objects' own flag setter. Its calls of 0x721DE0 are at
 *     0x67DBDC and 0x67DC14 (return addresses 0x67DBE1 / 0x67DC19); on both paths the stack at the store is [ret][id][value][saved esi][ret2][value][idx].
 *     Its callers are object states: the crate's break (uBreakBox BreakingState 0x6250D0), shutters (0x6329F0 ...), the valve's wait state (0x6395D0), traps,
 *     the load-room doors (0x647430 / 0x6496E0 / 0x64A650), the ground lift (0x652760), the elevator switch's wait state (0x658400), the laser trap (0x662EA0),
 *     the locker's opening (0x66D430), the weapon box (0x67FDD0) - and FUN_0065A500, whose only caller is the Genesis scan's end cGenesisEnemyParam_vf17 0x6DFD20:
 *     a scan (a hand print, a scannable object) - per player, never crossed.
 *   - uOpenBox: an emptied box (its content item taken) sets its flag (+0xF18) in uOpenBox_vf42 0x671030 and its states vf52 0x66E340 / vf66 0x66E450
 *     (calls 0x671104 / 0x671133, 0x66E412 / 0x66E442, 0x66E510 / 0x66E541).
 *   - AddItem cBioItemPack_vf6 0x5CA480 stores an item's flag when it enters the bag (calls 0x5CA54C, 0x5CB24D, 0x5CB2FB, 0x5CB3D8, 0x5CBBCC, 0x5CC0B0, 0x5CC232):
 *     a pickup of THIS PC's human (the pickup mirror already removes the item on the other PC).
 *   - everything else that calls 0x721DE0 (the FSM, enemies and their deaths, the Genesis enemy scan 0x6DFD20, the lever FUN_00645540 (row W1 replays the lever
 *     itself), NPCs, menus, achievements, the Raid sync handler FUN_00674970) is NOT an object path here: refused.
 * RULE: pass = the return address is one of the object paths above AND (for FUN_0067DB80) the caller of the setter is not the scan's end AND the owning object
 * (found by its +0xF18 range on the logic thread) is not a hand print, a lever or an enemy. A refused store stays on its PC (as before sync022).
 */
#ifndef SFOBJ1_H
#define SFOBJ1_H

#include <stdint.h>

#define SFO_FN_SET 0x67DB80u
#define SFO_RET_SET_A 0x67DBE1u
#define SFO_RET_SET_B 0x67DC19u
#define SFO_SCAN_LO 0x65A500u    /* FUN_0065A500 (the Genesis scan's end -> FUN_0067DB80(1, 0)) */
#define SFO_SCAN_HI 0x65A522u
#define SFO_CLS_HANDPRINT 0x80061636u

enum { SFO_PASS = 1, SFO_NOT_OBJECT_PATH, SFO_SCAN, SFO_NOSTACK, SFO_HANDPRINT, SFO_LEVER, SFO_ENEMY, SFO_N };
static const char *const sfo_why[SFO_N] = {"?", "an object path", "not a world-object path", "the Genesis scan's end (per player)", "the setter's caller unreadable",
                                           "a hand print (per player)", "a lever (row W1 replays the lever itself)", "an enemy"};

/* the return addresses of the 0x721DE0 calls that are object paths (the call site + 5); the test checks each is `E8 rel32 -> 0x721DE0` in the image */
static const uint32_t sfo_box_rets[6] = {0x66E417u, 0x66E447u, 0x66E515u, 0x66E546u, 0x671109u, 0x671138u};
static const uint32_t sfo_item_rets[7] = {0x5CA551u, 0x5CB252u, 0x5CB300u, 0x5CB3DDu, 0x5CBBD1u, 0x5CC0B5u, 0x5CC237u};

static inline int sfo_in(const uint32_t *t, unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; i++)
        if (t[i] == v) return 1;
    return 0;
}
/* the path of a store: 1 = the object flag setter (needs ret2 and the owner), 2 = a box, 3 = an item pickup, 0 = none of them */
static inline int sfo_path(uint32_t ret)
{
    if (ret == SFO_RET_SET_A || ret == SFO_RET_SET_B) return 1;
    if (sfo_in(sfo_box_rets, 6, ret)) return 2;
    if (sfo_in(sfo_item_rets, 7, ret)) return 3;
    return 0;
}
/* the decision. have2 = ret2 was read and checked (the stack layout of FUN_0067DB80); have_owner = an object of the list owns the id (its class in owner_cls) */
static inline int sfo_filter(uint32_t ret, uint32_t ret2, int have2, uint32_t owner_cls, int have_owner)
{
    int p = sfo_path(ret);
    if (!p) return SFO_NOT_OBJECT_PATH;
    if (p == 1) {
        if (!have2) return SFO_NOSTACK;
        if (ret2 >= SFO_SCAN_LO && ret2 < SFO_SCAN_HI) return SFO_SCAN;
    }
    if (have_owner) {
        if (owner_cls == SFO_CLS_HANDPRINT) return SFO_HANDPRINT;
        if (owner_cls == 0x80061900u || owner_cls == 0x80061901u) return SFO_LEVER;
        if ((owner_cls & 0xF00F0000u) == 0x80030000u) return SFO_ENEMY;
    }
    return SFO_PASS;
}

#endif
