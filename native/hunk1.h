/*
 * hunk1.h - Hunk as the second human where the story has none for him. PRODUCT (0.2.0): 2-2 only (HK_PRODUCT; REV1_LAB_HUNK=0 switches it off); the LAB modes 1 / 2 / 3
 * (REV1_LAB_HUNK=1|2|3) are the earlier experiments (16-2, every paired section, 2-2 + 16-1).
 *
 * NOTE (2026-11-11): episode 16 is the hidden TGS demo episode, the campaign never reaches 16-2; the useful case is mode 3 (2-2: the story locks Parker away, Hunk instead).
 * Why (the first idea): 16-2 is the one section played by a single character (Parker alone): the engine writes the class id 0x80000000 into slot 1 of the player setup object
 * ([[0x116A340] + 0x28]) and builds no second human. Writing the class id of Hunk (0x800110B0, resource pl10b0, present in the installed game) into that slot AFTER the engine's
 * section-start write makes the engine's own creation path (FUN_0042B720 preload, FUN_0042A730 create) build a Hunk human (allep4z: kind 1, ctl 1, hp 1000). The id must be
 * written only for the solo section and taken away at the next section start, otherwise the next paired section would build a Hunk instead of its partner.
 *
 * The decisions are pure (this header); the writes are world1_game.c (world_player_class), the call sites are the three section starts of rolebarrier1_game.c (host restart,
 * client restart, section change). Header only, no game access.
 */
#ifndef HUNK1_H
#define HUNK1_H
#include <stdint.h>
#include "secids1.h"

#define HK_CLASS_HUNK 0x800110B0u   /* uPl< Hunk >, resource pl10b0 */
#define HK_BAG_AS 0x80011020u        /* the item box's bag Hunk stands in for (Parker's: nibble 2 of the class id = the bag id)  */
#define HK_SLOT 1u                  /* the second human of the section */
#define HK_START_GUN 0x80051000u    /* uWpHdgM92F: the handgun every campaign character can carry (not a DLC gun: its archive is in every install); a Hunk human has no profile bag */

enum { HK_KEEP = 0, HK_SET = 1, HK_CLEAR = 2 };

typedef struct { int on; uint32_t area; unsigned sets, clears; } hk_t;

static inline void hk_reset(hk_t *h) { h->on = 0, h->area = 0, h->sets = h->clears = 0; }

/* the area is the solo part and the option is on (1); the LAB mode 2 (REV1_LAB_HUNK=2) puts Hunk in slot 1 of EVERY paired section too, so that his walk / shots / kills can be tested where there is
 * floor, weapons and enemies (Continue of a co-op save), in the guest's real network configuration */
/* HK_PRODUCT (2026-11-12, the lead after lab slot S7: 2-2 with Hunk went to its end and into 3-1 on both PCs, no crash): the default without REV1_LAB_HUNK = Hunk instead of the
 * story-locked Parker of 2-2 ONLY: not 16-1 / 16-2 (episode 16 is the hidden TGS demo: mode 1 stays off), no paired section. */
#define HK_PRODUCT 4
static inline int hk_ep16(uint32_t area) { return ((area >> 16) & 0xFFu) == 0x10u; }

static inline int hk_wanted(int enabled, uint32_t area)
{
    int k = sec_kind(area);
    if (enabled == 2) return k == RB_SK_SOLO || k == RB_SK_PAIR;
    if (enabled == 3) return k == RB_SK_SOLO || sec_spectate(area) == 2;   /* 3 = the solo part AND the story-locked sections (2-2, 16-1): Hunk instead of the locked Parker (lab, S5) */
    if (enabled == HK_PRODUCT) return sec_spectate(area) == 2 && !hk_ep16(area);   /* the product: 2-2 only */
    return enabled == 1 && k == RB_SK_SOLO;
}

/* a section starts (any of the three hooks, possibly the same section again after a restart): what to do with the persisted slot-1 class.
 * SET = (re)write HK_CLASS_HUNK into slot 1 and keep it written; CLEAR = stop writing (the next paired section builds its own partner); KEEP = nothing was on. */
static inline int hk_step(hk_t *h, int enabled, uint32_t area)
{
    if (hk_wanted(enabled, area)) {
        h->on = 1, h->area = area, h->sets++;
        return HK_SET;
    }
    if (h->on) {
        h->on = 0, h->area = area, h->clears++;
        return HK_CLEAR;
    }
    return HK_KEEP;
}

/* the spectator rule with Hunk: the solo part is no longer watched (the second human plays Hunk); 2-2 / 16-1 stay as they are */
static inline int hk_spectate(int enabled, uint32_t area)
{
    int s = sec_spectate(area);
    if (enabled == HK_PRODUCT) return s == 2 && !hk_ep16(area) ? 0 : s;   /* the product: 2-2 is played by two (Hunk), 16-1 / 16-2 keep the spectator rule */
    return (s == 1 && enabled) || (s == 2 && enabled == 3) ? 0 : s;
}

/* the network roles of the section: the host keeps slot 0 (Parker), the guest plays slot 1 (Hunk) = the same unit slots as in a paired section, so no role mapping changes.
 * Returns the unit slot a PC plays in this area: host 0, guest 1. */
static inline unsigned hk_own_slot(int host) { return host ? 0u : 1u; }

#endif
