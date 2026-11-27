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

/* rel027 (owner 2026-11-25): the story's HOLD of a captured human. While the story's PcsMain/SubCharacterThinkOff runs on a human (th_t0 in capture.c) the scripted commands of the
 * demo park his lower state machine in a wait state (EventWaitState [0] by the event motion / CharacterSetMotion; the standing waits ExWait [2], CustomWaitReady [3], CustomWait [4]),
 * which ignores his pad: the second human stood still until the story let go (13_00 start: Parker at the sink for 75 s). In the plain play view (no movie, no scene camera, no
 * pause) the human is asked for MoveState ([5], the state every event ends in: EventStateBase_vf9 -> ChangeState(MoveState, 0)) after a grace time; the story's own triggers
 * (zones, flags) go on unchanged and its next command may park him again (retried at most every CK_HOLD_RETRY_MS). */
#define CK_HOLD_GRACE_MS 400u
#define CK_HOLD_RETRY_MS 1000u
static inline int ck_hold_state(int idx) { return idx == 0 || idx == 2 || idx == 3 || idx == 4; }
static inline int ck_hold_release(int on, int story_wait, int play_view, int idx, unsigned held_ms, unsigned since_req_ms)
{
    if (!on || !story_wait || !play_view || !ck_hold_state(idx)) return 0;
    return held_ms >= CK_HOLD_GRACE_MS && since_req_ms >= CK_HOLD_RETRY_MS;
}

/* rel027p: the story's LOCK of the second human (FUN_006B3B80: E1C |= 4) is lifted in the plain play view. unlock_on = switch, e1c = unit+0xE1C, slot = unit slot field, own = the own slot, kind = unit+0xE30,
 * play = the split view is drawing (no movie / scene camera / pause), since_ms = time since the last unlock call of this unit. Never the own human, never the dead (E1C bit 0), only a human (kind 1). */
static inline int ck_unlock_wanted(int unlock_on, unsigned e1c, unsigned slot, int own, unsigned kind, int play, unsigned since_ms)
{
    if (!unlock_on || !play || !(e1c & 4u) || (e1c & 1u) || slot > 1 || (int)slot == own || kind != 1) return 0;
    return since_ms >= 500u;
}

#endif
