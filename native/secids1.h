/*
 * secids1.h - the campaign's section ids (allep1, room option "Experimental: all episodes"): which 0x91EESS00 area is a paired section, the solo part, no section, or the end.
 * Header only, shared by rolebarrier1.c (the barrier's gates) and lobby1.c (the host's save warning). Source: notes\episode-catalog.md (every id entered live in the game).
 */
#ifndef SECIDS1_H
#define SECIDS1_H
#include <stdint.h>

/* 28 sections start with two playable characters (PAIR); 16-2 (0x91100200, Parker alone) is SOLO. Episode 16 (16-1, 16-2) is the hidden TGS demo episode: the campaign never reaches it
 * (msg\campaign_<lang>.gmd lists episode_01..12 and a hidden episode_TGS; no section ends into 0x9110xxxx), only the lab console `warp` starts it, so the campaign has no one-character section; 0-1 / 0-2 / 8-4 (no area class: warp falls back to the
 * Episode 1 start) and F1 (the game exits) are NOTSEC; END = the campaign is over (0x91FF0000 "no next section") or the id is no 0x91 campaign id. The area the game really goes to after 12-3 is 0x910E0000 (allep4s): no section, no start table entry = NOTSEC; the end of co-op comes from rb_end_kind_from(12-3). */
enum { RB_SK_PAIR = 0, RB_SK_SOLO = 1, RB_SK_NOTSEC = 2, RB_SK_END = 3 };

/* fix021 (0.2.1): the characters of a paired section (notes\episode-catalog.md; slot 0 = the lead, slot 1 = the partner) as lobby1.c's persons (0 Jill, 1 Chris,
 * 2 Parker, 3 Keith, 4 Jessica, 5 Quint, 6 Raymond, 7 Hunk: 2-2's partner in the product, hunk1.h). 1 = known. The host's room names the co-op save's pair with it. */
static inline int sec_pair(uint32_t area, int *lead, int *partner)
{
    static const struct { uint32_t a; signed char l, p; } T[] = {
        {0x91010100u, 0, 2}, {0x91010200u, 0, 2}, {0x91010300u, 0, 2}, {0x91020100u, 1, 4}, {0x91020200u, 0, 7}, {0x91030100u, 2, 4}, {0x91030200u, 0, 2},
        {0x91040100u, 0, 2}, {0x91050100u, 0, 2}, {0x91050200u, 3, 5}, {0x91050300u, 3, 5}, {0x91060100u, 1, 4}, {0x91060200u, 0, 2}, {0x91070100u, 0, 2},
        {0x91070200u, 0, 2}, {0x91080100u, 0, 2}, {0x91080200u, 1, 4}, {0x91080300u, 0, 1}, {0x91090100u, 3, 5}, {0x91090200u, 0, 1}, {0x910A0100u, 2, 4},
        {0x910A0200u, 0, 1}, {0x910B0100u, 0, 1}, {0x910B0200u, 2, 4}, {0x910C0100u, 0, 1}, {0x910C0200u, 0, 1}, {0x910C0300u, 0, 1}, {0x91100100u, 0, 2}};
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (T[i].a == area) {
            if (lead) *lead = T[i].l;
            if (partner) *partner = T[i].p;
            return 1;
        }
    return 0;
}

static inline int sec_kind(uint32_t area)
{
    static const uint32_t pairs[28] = {0x91010100u, 0x91010200u, 0x91010300u, 0x91020100u, 0x91020200u, 0x91030100u, 0x91030200u, 0x91040100u, 0x91050100u, 0x91050200u,
                                       0x91050300u, 0x91060100u, 0x91060200u, 0x91070100u, 0x91070200u, 0x91080100u, 0x91080200u, 0x91080300u, 0x91090100u, 0x91090200u,
                                       0x910A0100u, 0x910A0200u, 0x910B0100u, 0x910B0200u, 0x910C0100u, 0x910C0200u, 0x910C0300u, 0x91100100u};
    if ((area >> 24) != 0x91u || area == 0x91FF0000u) return RB_SK_END;
    for (unsigned i = 0; i < 28; i++)
        if (pairs[i] == area) return RB_SK_PAIR;
    if (area == 0x91100200u) return RB_SK_SOLO;
    return RB_SK_NOTSEC;
}

/* allep1 step 3: sections whose SECOND human cannot play (live allep1b-d, 2n): 1 = the solo 16-2 (no unit for slot 1), 2 = the unit exists but the story drains the human Parker to death within seconds
 * while the AI Parker lives (2-2 and 16-1: the same stage; the vanilla control without a session keeps both alive 2 minutes). The partner's PC SPECTATES the lead there; control returns at the next paired section. */
static inline int sec_spectate(uint32_t area)
{
    if (area == 0x91100200u) return 1;
    if (area == 0x91020200u || area == 0x91100100u) return 2;
    return 0;
}
#endif
