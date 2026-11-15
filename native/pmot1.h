/*
 * pmot1.h - sync022 rows P5-P10 / P12 (notes\net-sync-max.md): the partner's interaction motions on his puppet here (picking something up, a door by hand, a lever,
 * a valve, pushing a shelf, the weapon box). Pure (no Windows, no engine): the wire, the sender's detection and the receiver's state machine; pmot1_game.inc runs
 * them, tests/test_pmot1.c drives them on a model of the puppet (its lower state machine, its motion layers, the bank resources and their reference counts).
 *
 * FACTS (Ghidra work\ghidra-sync022, <lab>\re1q\s22\o1-o7.txt, o12.txt):
 *   - the puppet never enters an interaction state here: every object takes "the own unit" (FUN_0067AF10) as its operator. The partner's PC has his human in the
 *     interaction state (an EventStateBase child: PlayerHandKnob / OpeningDoor / Lever* / DoorGimmick* / PressOM* / WeaponBoxOpen, or PickUpState*), which plays
 *     its motions through the unit's own setter (vt+0x68 -> vt+0x6C = 0x6B8CB0, (layer, id, blend, frame, speed, flags)); ids 0x0Exx come from bank 0xE, which the
 *     object bound to its operator in its enter: FUN_00A2D7E0(player; object+0xD28 + i*4, 0xE) (+ FUN_006B06F0(player; object+0xF48 + i*8, 0xE) for the lever and
 *     the press, 0x6439F0 / 0x6665C0), i = FUN_006A7B70(player) (0 / 1 by the character class). FUN_00A2D7E0 releases the old list (FUN_009B9670) and
 *     takes a reference on the new one (FUN_009B9660); with 0 it only releases (what PcsMainCharacterResetEventMotion does for bank 0xF, 0x771850).
 *   - the stock way to play a motion on a human from outside (the FSM's CharacterSetMotion 0x770B10, used by the scenes) is: hold him (FUN_006B3B80: the lower
 *     state index 0 = EventWaitState, player table 0x1137498), then vt+0x6C. EventWaitState's enter / exit are symmetric (0x689EA0 / 0x689C70); its update
 *     (EventStateBase_vf9 0x689DB0) sets no motion and ends the state only when it is finished (-> index 5 = MoveState, the normal lower state).
 *   - the deferred request by index FUN_0049C790(mgr; index, 0, NULL) is applied by the unit's own tick (FUN_0049CDE0): the hold and the release are requests,
 *     not calls into the unit's states from this thread.
 * THE RULE: the partner's PC reports its own human's interaction (BEGIN, the motion changes, KEEP every 300 ms, END). This PC, only while the puppet is free (its
 * lower state is MoveState) and nothing holds it (pause, cutscene, load, death, downed), asks for EventWaitState, binds bank 0xE from ITS copy of the same object
 * when the motion needs it, and sets the reported motions; END, no word for 1200 ms or 20 s in all -> MoveState and bank 0xE released again. The puppet never
 * runs an interaction state; no object is touched.
 */
#ifndef PMOT1_H
#define PMOT1_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define PM_LEN 52
#define PM_VER 1
#define PM_KEEP_MS 300u
#define PM_SILENT_MS 1200u
#define PM_MAX_MS 20000u
#define PM_FREEZE_MS 600u
#define PM_RELEASE_MS 600u
#define PM_IDX_EVENTWAIT 0
#define PM_IDX_EXWAIT 2   /* ExWaitState (the player table 0x1137498 [2]): the standing wait; live s22p1: the host's puppet stood in a non-Move state for a whole pickup */
#define PM_IDX_CWREADY 3  /* CustomWaitReadyState [3]: live s22p3: both PCs' own humans stood idle in it */
#define PM_IDX_CWAIT 4    /* CustomWaitState [4]: its idle follow-up */
#define PM_IDX_MOVE 5
/* the lower states the mirror may freeze from: the walk (MoveState) and the standing waits (2 / 3 / 4, each proven by its DTI name at run time); a hit, a grab, a
 * scene, a turn ... never */
static inline int pm_idx_free(int pidx) { return pidx == PM_IDX_MOVE || pidx == PM_IDX_EXWAIT || pidx == PM_IDX_CWREADY || pidx == PM_IDX_CWAIT; }
#define PM_BANK_EVENT 0x0E

enum { PM_BEGIN = 1, PM_MOT = 2, PM_KEEP = 3, PM_END = 4 };
/* the mirrored interaction states (their DTI objects: work\dti-rev1.txt); the glue resolves their ids at run time */
enum {
    PM_T_PICKUP = 0, PM_T_PICKUP_UW, PM_T_PICKUP_WS, PM_T_KNOB, PM_T_KNOBLOOP, PM_T_DOOROPEN, PM_T_LEVERON, PM_T_LEVEROFF, PM_T_FLOORLEVEROFF, PM_T_DG_START, PM_T_DG_MOVE,
    PM_T_DG_WAIT, PM_T_DG_END, PM_T_PRESS_START, PM_T_PRESS_LOOP, PM_T_PRESS_END, PM_T_WEAPONBOX,
    PM_T_N_INTERACT,
    /* traversal (`world set pmtrav 1`, default off: not proven live) */
    PM_T_LADDER = PM_T_N_INTERACT, PM_T_WATERLADDER, PM_T_CLIMBLOW, PM_T_CLIMBHIGH, PM_T_CLIMBOVER, PM_T_JUMPDOWN, PM_T_JUMPDOWNLOW, PM_T_JUMPWATER, PM_T_JUMPACROSS, PM_T_WATERCLIMB,
    PM_T_N
};
typedef struct { uint32_t dti; const char *name; } pm_state_def;
static const pm_state_def pm_states[PM_T_N] = {
    {0x0117A404u, "PickUpState"}, {0x0117A424u, "PickUpStateUnderWater"}, {0x0117A4E4u, "PickUpStateWaterSurface"}, {0x01175820u, "PlayerHandKnobState"},
    {0x01175800u, "PlayerHandKnobLoopState"}, {0x01175880u, "PlayerOpeningDoorState"}, {0x01176AB4u, "PlayerLeverOnMoveState"}, {0x01176B74u, "PlayerLeverOffMoveState"},
    {0x01176BC0u, "PlayerFloorLeverOffMoveState"}, {0x01176480u, "PlayerDoorGimmickStartState"}, {0x01176440u, "PlayerDoorGimmickMoveState"},
    {0x011764E0u, "PlayerDoorGimmickWaitState"}, {0x01176460u, "PlayerDoorGimmickEndState"}, {0x0117924Cu, "PlayerPressOMStartState"}, {0x0117926Cu, "PlayerPressOMLoopState"},
    {0x011792ACu, "PlayerPressOMEndState"}, {0x0117A1ECu, "PlayerWeaponBoxOpenState"},
    {0x0117A364u, "ClimbLadderState"}, {0x0117A564u, "WaterClimbLadderState"}, {0x0117A464u, "ClimbWallLow"}, {0x0117A3C4u, "ClimbWallHigh"}, {0x0117A384u, "ClimbOver"},
    {0x0117A444u, "JumpDownState"}, {0x0117A544u, "JumpDownLowState"}, {0x0117A3A4u, "JumpDownToWaterState"}, {0x0117A584u, "JumpAcrossState"}, {0x0117A3E4u, "WaterClimbState"},
};

/* one motion layer as the engine keeps it (unit + 0x2D0 + layer * 0x140: +4 id, +0x40 blend, +0x4C frame, +0x5C speed) */
typedef struct { uint16_t id; float blend, frame, speed; } pm_layer;
typedef struct {
    uint8_t kind, ev, seq, tag;
    pm_layer l0, l1;
    uint8_t has_src, src_i, src_params;   /* bank 0xE: the object whose list (obj+0xD28 + src_i*4) the human holds; params: also obj+0xF48 + src_i*8 */
    uint32_t k4[4];
} pm_msg;

static inline void pm_putf(uint8_t *b, float v) { memcpy(b, &v, 4); }
static inline float pm_getf(const uint8_t *b) { float v; memcpy(&v, b, 4); return v; }
static inline unsigned pm_enc(uint8_t *b, const pm_msg *m)
{
    memset(b, 0, PM_LEN);
    b[0] = PM_VER, b[1] = m->kind, b[2] = m->ev, b[3] = m->seq, b[4] = m->tag;
    b[5] = (uint8_t)m->l0.id, b[6] = (uint8_t)(m->l0.id >> 8);
    pm_putf(b + 7, m->l0.blend), pm_putf(b + 11, m->l0.frame), pm_putf(b + 15, m->l0.speed);
    b[19] = (uint8_t)m->l1.id, b[20] = (uint8_t)(m->l1.id >> 8);
    pm_putf(b + 21, m->l1.blend), pm_putf(b + 25, m->l1.frame), pm_putf(b + 29, m->l1.speed);
    b[33] = m->has_src, b[34] = m->src_i, b[35] = m->src_params;
    memcpy(b + 36, m->k4, 16);
    return PM_LEN;
}
static inline int pm_layer_ok(const pm_layer *l) { return isfinite(l->blend) && isfinite(l->frame) && isfinite(l->speed) && fabsf(l->frame) < 1.0e6f && fabsf(l->speed) < 100.0f && l->blend >= 0.0f && l->blend < 1000.0f; }
static inline int pm_dec(const uint8_t *b, unsigned len, pm_msg *m)
{
    if (!b || len != PM_LEN || b[0] != PM_VER || b[1] < PM_BEGIN || b[1] > PM_END || b[4] >= PM_T_N) return 0;
    memset(m, 0, sizeof *m);
    m->kind = b[1], m->ev = b[2], m->seq = b[3], m->tag = b[4];
    m->l0.id = (uint16_t)(b[5] | (b[6] << 8)), m->l0.blend = pm_getf(b + 7), m->l0.frame = pm_getf(b + 11), m->l0.speed = pm_getf(b + 15);
    m->l1.id = (uint16_t)(b[19] | (b[20] << 8)), m->l1.blend = pm_getf(b + 21), m->l1.frame = pm_getf(b + 25), m->l1.speed = pm_getf(b + 29);
    m->has_src = b[33] ? 1 : 0, m->src_i = b[34], m->src_params = b[35] ? 1 : 0;
    memcpy(m->k4, b + 36, 16);
    if (!pm_layer_ok(&m->l0) || !pm_layer_ok(&m->l1) || m->src_i > 1) return 0;
    return 1;
}
static inline int pm_needs_bank(uint16_t id) { return id != 0xFFFF && (id >> 8) == PM_BANK_EVENT; }

/* ---------------------------------------------------------------- the sender (the PC whose own human interacts) */
typedef struct {
    int on;                 /* in an event now */
    uint8_t ev, seq, tag;
    uint16_t l0, l1;
    uint8_t has_src;
    uint32_t k4[4];
    uint32_t last_ms;
    unsigned begins, mots, keeps, ends;
} pm_tx;
static inline void pm_tx_reset(pm_tx *t) { memset(t, 0, sizeof *t); }
/* one pass: tag = the human's mirrored state (-1 none); l0 / l1 = its layers; src = the bank-0xE object (may be NULL). 1 = send *m now */
static inline int pm_tx_step(pm_tx *t, int tag, const pm_layer *l0, const pm_layer *l1, const pm_msg *src, uint32_t now, pm_msg *m)
{
    int kind = 0;
    if (tag < 0) {
        if (!t->on) return 0;
        t->on = 0, kind = PM_END, t->ends++;
    } else if (!t->on) {
        t->on = 1, t->ev++, t->tag = (uint8_t)tag, kind = PM_BEGIN, t->begins++;
    } else if ((uint8_t)tag != t->tag || l0->id != t->l0 || l1->id != t->l1 || (src && src->has_src) != t->has_src) {
        t->tag = (uint8_t)tag, kind = PM_MOT, t->mots++;
    } else if ((uint32_t)(now - t->last_ms) >= PM_KEEP_MS) {
        kind = PM_KEEP, t->keeps++;
    } else {
        return 0;
    }
    memset(m, 0, sizeof *m);
    m->kind = (uint8_t)kind, m->ev = t->ev, m->seq = ++t->seq, m->tag = t->tag;
    if (kind != PM_END) {
        m->l0 = *l0, m->l1 = *l1;
        t->l0 = l0->id, t->l1 = l1->id;
        if (src && src->has_src) m->has_src = 1, m->src_i = src->src_i, m->src_params = src->src_params, memcpy(m->k4, src->k4, 16);
        t->has_src = m->has_src;
    }
    t->last_ms = now;
    return 1;
}

/* ---------------------------------------------------------------- the receiver (the PC that shows the puppet) */
typedef struct pm_rx pm_rx;
static inline void pm_rx_drop_state(pm_rx *r);
enum { PM_R_IDLE = 0, PM_R_FREEZE, PM_R_HOLD, PM_R_RELEASE };
/* actions for the glue, in this order within one pass */
enum { PM_A_REQ_FREEZE = 1, PM_A_BIND = 2, PM_A_UNBIND = 4, PM_A_MOTION = 8, PM_A_REQ_RELEASE = 16, PM_A_DROP = 32, PM_A_UNFREEZE = 64 };
#define PM_STALE_MS 1500u   /* a stock freeze of the puppet with no hold here and no event of the partner: released after this long (live s22p3) */
#define PM_STALE_EVERY_MS 3000u
struct pm_rx {
    int st;
    uint8_t ev, have_ev, last_seq, tag;
    pm_msg cur;             /* the newest word of the event */
    int cur_new;            /* a motion not yet set */
    int bound;              /* bank 0xE bound by us (released at the end) */
    int owed;               /* a hold took the puppet while OUR EventWaitState was on: once the hold is over and he still waits there, we release him */
    uint32_t t_state, t_word, t_begin, t_stale, t_stale_rel;
    int release_tries;
    unsigned events, freezes, freeze_timeouts, motions, binds, unbinds, releases, silent, too_long, refused_busy, held_drops, foreign_exit, old, no_src, adopted, unfreezes;
};
static inline void pm_rx_reset(pm_rx *r) { memset(r, 0, sizeof *r); }
/* a word of the partner's PC. hold = something holds the puppet here (pause, cutscene, load, dead, downed) */
static inline void pm_rx_word(pm_rx *r, const pm_msg *m, uint32_t now)
{
    if (r->have_ev && m->ev == r->ev && (int8_t)(uint8_t)(m->seq - r->last_seq) <= 0) {
        r->old++;
        return;
    }
    int fresh = 0;
    if (!r->have_ev || m->ev != r->ev) {   /* a new event of the partner */
        if (m->kind == PM_END) {           /* the end of an event this PC never saw begin: nothing to do */
            r->have_ev = 1, r->ev = m->ev, r->last_seq = m->seq, r->cur.kind = PM_END;
            return;
        }
        r->have_ev = 1, r->ev = m->ev, r->events++;
        r->t_begin = now, fresh = 1;
    }
    r->last_seq = m->seq, r->t_word = now;
    if (m->kind == PM_END) {
        r->cur.kind = PM_END;
        return;
    }
    if (fresh || m->kind == PM_BEGIN || m->l0.id != r->cur.l0.id || m->l1.id != r->cur.l1.id) r->cur_new = 1;   /* a KEEP / a tag change with the same motions sets nothing again */
    r->cur = *m;
}
/* one pass. pidx = the puppet's lower state index (-1 unknown / no puppet); hold = a cutscene / load / death / downed holds the puppet now (they own his state
 * machine: the mirror lets go WITHOUT any call, pm_rx_drop; a pause is no hold here: the timers run and a release request waits in the manager); returns the
 * actions (PM_A_*) for the glue */
/* frozen = the puppet carries the stock freeze (FUN_006B3B80: unit+0xF44 bit 3); its stock release FUN_006AE390 comes with every release (PM_A_UNFREEZE), and
 * a stock freeze with no hold here and no event of the partner is released after PM_STALE_MS (the partner is free on his PC: live s22p3, the door) */
static inline int pm_rx_step_f(pm_rx *r, int pidx, int hold, int frozen, uint32_t now);
static inline int pm_rx_step(pm_rx *r, int pidx, int hold, uint32_t now) { return pm_rx_step_f(r, pidx, hold, 0, now); }
static inline int pm_rx_step_core(pm_rx *r, int pidx, int hold, uint32_t now);
static inline int pm_rx_step_f(pm_rx *r, int pidx, int hold, int frozen, uint32_t now)
{
    int active = r->have_ev && r->cur.kind != PM_END && (uint32_t)(now - r->t_word) < PM_SILENT_MS && (uint32_t)(now - r->t_begin) < PM_MAX_MS;
    int a = pm_rx_step_core(r, pidx, hold, now);
    if ((a & PM_A_REQ_RELEASE) && frozen) a |= PM_A_UNFREEZE, r->unfreezes++;
    if (r->st == PM_R_IDLE && !r->owed && !active && !hold && frozen && pidx == PM_IDX_EVENTWAIT) {
        if (!r->t_stale) r->t_stale = now ? now : 1;
        else if ((uint32_t)(now - r->t_stale) >= PM_STALE_MS && (!r->t_stale_rel || (uint32_t)(now - r->t_stale_rel) >= PM_STALE_EVERY_MS))
            a |= PM_A_UNFREEZE, r->unfreezes++, r->t_stale_rel = now ? now : 1;
    } else r->t_stale = 0;
    return a;
}
static inline int pm_rx_step_core(pm_rx *r, int pidx, int hold, uint32_t now)
{
    int a = 0;
    int active = r->have_ev && r->cur.kind != PM_END && (uint32_t)(now - r->t_word) < PM_SILENT_MS && (uint32_t)(now - r->t_begin) < PM_MAX_MS;
    switch (r->st) {
    case PM_R_IDLE:
        if (r->owed && !hold && pidx >= 0) {
            if (pidx != PM_IDX_EVENTWAIT) {
                r->owed = 0;   /* the hold's owner gave him back to his own states */
            } else if (!active) {
                r->owed = 0;
                a |= PM_A_REQ_RELEASE, r->st = PM_R_RELEASE, r->t_state = now, r->release_tries = 1, r->releases++;
                break;
            }
        }
        if (!active || hold || pidx < 0) break;
        if (r->owed && pidx == PM_IDX_EVENTWAIT) {   /* still ours and the partner's event goes on: take it up again */
            r->owed = 0;
            r->st = PM_R_FREEZE, r->t_state = now;
            break;
        }
        if (pidx == PM_IDX_EVENTWAIT) {   /* live s22p2: he already waits in an EventWaitState of this PC's own stock code (not ours; no hold here and the partner's PC
                                           * says he acts): the mirror takes that hold as its own: no request, the motion, the release at the end */
            r->adopted++;
            r->st = PM_R_FREEZE, r->t_state = now;
            break;
        }
        if (!pm_idx_free(pidx)) {   /* the puppet is busy (a hit, a grab, a scene, an earlier hold of the engine): not now */
            r->refused_busy++;
            break;
        }
        a |= PM_A_REQ_FREEZE;
        r->st = PM_R_FREEZE, r->t_state = now, r->freezes++;
        break;
    case PM_R_FREEZE:
        if (hold || pidx < 0) {   /* the owner of the hold decides the puppet's state from now on */
            r->held_drops++;
            pm_rx_drop_state(r);
            return PM_A_DROP;
        }
        if (pidx == PM_IDX_EVENTWAIT) {
            r->st = PM_R_HOLD, r->t_state = now;
            if (!active) {   /* the event ended while the hold was being made */
                a |= PM_A_REQ_RELEASE, r->st = PM_R_RELEASE, r->release_tries = 1;
                break;
            }
            if (pm_needs_bank(r->cur.l0.id) || pm_needs_bank(r->cur.l1.id)) {
                if (r->cur.has_src) a |= PM_A_BIND, r->bound = 1, r->binds++;
                else r->no_src++;
            }
            a |= PM_A_MOTION, r->cur_new = 0, r->motions++;
            break;
        }
        if ((uint32_t)(now - r->t_state) >= PM_FREEZE_MS) {   /* the engine did not take the hold */
            r->freeze_timeouts++;
            a |= PM_A_REQ_RELEASE, r->st = PM_R_RELEASE, r->t_state = now, r->release_tries = 1;
        }
        break;
    case PM_R_HOLD:
        if (pidx != PM_IDX_EVENTWAIT) {   /* something else took the puppet (a hit, death, the story): his state machine is its own again; only our bank goes */
            r->foreign_exit++;
            if (r->bound) a |= PM_A_UNBIND, r->bound = 0, r->unbinds++;
            a |= PM_A_DROP;
            r->st = PM_R_IDLE;
            break;
        }
        if (hold) {   /* a cutscene / load / death / downed takes the puppet: no call; bank 0xE stays bound as an operator's would (the next bind or the unit's end releases it) */
            r->held_drops++;
            pm_rx_drop_state(r);
            return PM_A_DROP;
        }
        if (!active) {
            if (r->cur.kind != PM_END && (uint32_t)(now - r->t_word) >= PM_SILENT_MS) r->silent++;
            else if (r->cur.kind != PM_END && (uint32_t)(now - r->t_begin) >= PM_MAX_MS) r->too_long++;
            if (r->bound) a |= PM_A_UNBIND, r->bound = 0, r->unbinds++;
            a |= PM_A_REQ_RELEASE, r->st = PM_R_RELEASE, r->t_state = now, r->release_tries = 1, r->releases++;
            break;
        }
        if (r->cur_new) {
            if ((pm_needs_bank(r->cur.l0.id) || pm_needs_bank(r->cur.l1.id)) && r->cur.has_src) a |= PM_A_BIND, r->bound = 1, r->binds++;   /* another object's list */
            a |= PM_A_MOTION, r->cur_new = 0, r->motions++;
        }
        break;
    case PM_R_RELEASE:
        if (pidx != PM_IDX_EVENTWAIT || hold) {
            r->st = PM_R_IDLE;
            break;
        }
        if ((uint32_t)(now - r->t_state) >= PM_RELEASE_MS) {
            if (r->release_tries >= 3) {   /* leave it to the engine (EventWaitState ends by itself only when finished): logged by the glue */
                r->st = PM_R_IDLE, a |= PM_A_DROP;
                break;
            }
            r->release_tries++, r->t_state = now;
            a |= PM_A_REQ_RELEASE;
        }
        break;
    }
    return a;
}
/* the state of the mirror goes, the event's identity stays (a KEEP of the same event does not start it again) and so do the counters */
static inline void pm_rx_drop_state(pm_rx *r)
{
    r->owed = r->st == PM_R_FREEZE || r->st == PM_R_HOLD;
    r->st = PM_R_IDLE, r->bound = 0, r->cur_new = 0;
}
/* a stage change / restart / new level instance / the puppet unit changed: everything of the mirror is dropped WITHOUT any call on the old unit; 1 = there was something */
static inline int pm_rx_drop(pm_rx *r)
{
    int had = r->st != PM_R_IDLE || r->bound;
    unsigned keep[16] = {r->events, r->freezes, r->freeze_timeouts, r->motions, r->binds, r->unbinds, r->releases, r->silent, r->too_long, r->refused_busy, r->held_drops, r->foreign_exit, r->old, r->no_src, r->adopted, r->unfreezes};
    memset(r, 0, sizeof *r);
    r->events = keep[0], r->freezes = keep[1], r->freeze_timeouts = keep[2], r->motions = keep[3], r->binds = keep[4], r->unbinds = keep[5], r->releases = keep[6];
    r->silent = keep[7], r->too_long = keep[8], r->refused_busy = keep[9], r->held_drops = keep[10], r->foreign_exit = keep[11], r->old = keep[12], r->no_src = keep[13], r->adopted = keep[14], r->unfreezes = keep[15];
    return had;
}

#endif
