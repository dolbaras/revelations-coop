/*
 * aimgate1.h - the "aim allowed" byte of the pad module for a second human (pure C; used by capture.c, tests/test_aimgate1.c).
 *
 * Found 2026-11-11 (trigger slot trg1, notes\allep-resume.md "TRIGGER SLOT"): sGamePad+0x883 is ONE global byte. FUN_006B3100 (the player's frame) writes it only for the
 * OWN human (kind 1 and unit == FUN_0040EEF0()): FUN_00883C70(FUN_006AD740(unit, 0)) = "the own human may aim now" (a gun in the hand: not unarmed, not an item, not the
 * Genesis scanner, not in a state that forbids weapons). Its only reader FUN_00888590 ("the aim button of the CURRENT pad slot, if aiming is allowed", +0x882 must be set
 * too) feeds the aim flag unit+0x1151 (uPlayer_updatePad) and the upper-body states. So a second human (split screen: the character on pad slot 1; network: the partner's
 * puppet) could aim only while the OWN human could. Live (N100, 9-2, the lab profile's Jill unarmed): LT of the second human reached Pad[1] (held 0x400, LT byte 0xFF) by the
 * virtual pad, the synthetic pad and the record path alike, +0x883 stayed 0, he never left Upper::WaitState and never fired - with the published 0.1.0 proxy as well.
 * The fix: inside the swapped sections of a captured human that is NOT the own one the byte holds HIS value (FUN_006AD740 on him, asked in his own phases on the logic
 * thread; his weapon and state-machine sections, which may run on workers, use that cached value) and gets the game's value back when the section ends.
 */
#ifndef AIMGATE1_H
#define AIMGATE1_H

#define AG1_EN_OFF 0x882u        /* sGamePad + this: pad input enabled (byte); FUN_00888590 needs it and the flag below */
#define AG1_FLAG_OFF 0x883u      /* sGamePad + this: aim allowed (byte); the only writer is FUN_00883C70, called from FUN_006B3100 for the own human */
#define AG1_CANAIM_FN 0x6AD740u  /* may this human aim now: thiscall(unit, char 0) -> al, ret 4 (prologue 83 EC 0C 53 56 8B F1 8B 86) */
#define AG1_BAG_OFF 0xFE8u       /* unit + this: cBioItemPack; FUN_006AD740 asserts (the game ends) when it is NULL */
#define AG1_UNKNOWN (-1)         /* the cached value of a capture before its first own phase: the byte is left alone */

/* does a swapped section of a capture set the byte? enabled = the fix is on, taken = the capture took the unit over (a human, not lost), unit_slot = its slot field
 * (unit+0xE2C), own = the own slot now (the game writes the byte for that human itself) */
static inline int ag1_applies(int enabled, int taken, unsigned unit_slot, int own)
{
    return enabled && taken && unit_slot <= 1u && (int)unit_slot != own;
}

/* the value for this section. char_section = the section of the character itself (logic thread): the game's answer `can` is taken (no bag = may not aim) and cached;
 * other sections (his weapon, his state machine) return the cache. AG1_UNKNOWN = leave the byte alone. */
static inline int ag1_value(int char_section, int have_bag, int can, int *cache)
{
    if (char_section)
        *cache = have_bag && can ? 1 : 0;
    return *cache;
}

#endif
