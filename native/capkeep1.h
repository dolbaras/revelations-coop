/* capkeep1.h: the decisions of the capture keep (capture.c), pure C (tests/test_world1_game.c).
 *
 * A captured human (the partner's puppet in a network game, the second human of the split screen) has control type, kind and upper-body mode 1 (cap_take). The game can take
 * him back with uPlayer::setKind (vtable slot 0xF0 = 0x6A7B40) - wp5 live: the host's puppet of the client read kind 2 ctl 2 five minutes after the capture, the same unit -
 * or by writing a field directly. keep = 1 (default): a setKind(v != 1) on him becomes setKind(1), a changed field is written back; keep = 0 (lab A/B): the game wins and the
 * capture counts as LOST (his blows are the AI's, world1 credits nobody for them). */
#ifndef CAPKEEP1_H
#define CAPKEEP1_H

enum { CK_PASS = 0, CK_KEEP = 1, CK_LOST = 2 };

/* setKind(v) on a unit: captured = the unit is a taken-over capture. *pass_v = the value the stock setKind gets. */
static inline int ck_setkind(int captured, int keep, int v, int *pass_v)
{
    if (!captured || v == 1) {
        *pass_v = v;
        return CK_PASS;
    }
    if (keep) {
        *pass_v = 1;
        return CK_KEEP;
    }
    *pass_v = v;
    return CK_LOST;
}

/* the per-pass check of a captured human: control type ct, kind kd, upper mode tm, its uCallThink th (the one of the capture: th_cap). CK_PASS = all fine,
 * CK_KEEP = write them back to 1 (and take th as the new uCallThink), CK_LOST = leave them (keep 0) */
static inline int ck_fields(unsigned ct, unsigned kd, unsigned tm, unsigned th, unsigned th_cap, int keep)
{
    if (ct == 1 && kd == 1 && tm == 1 && th == th_cap) return CK_PASS;
    return keep ? CK_KEEP : CK_LOST;
}

#endif
