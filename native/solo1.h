/*
 * solo1.h - the story's solo parts in a two-human game (ep1-solo, E3): pure decisions, no game, no Windows. Used by the network glue
 * (solo1_game.inc inside world1_game.c), the puppet module (puppet_game.c), the downed rule (downed1.c), the in-game strip (menu.c) and the local
 * split (split.c). Evidence: notes\ep1-solo.md section 1.
 *
 * The engine (build 13298487):
 *   - FsmSetEnablePartner 0x752820 picks "the partner" as the first player unit whose slot is NOT the own slot (0x41BC10) and clears / sets the unit's
 *     update + draw bits (+0xC 0x400 | 0x800) and those of its weapon. 1-3 calls it with 0 after SF[4943] (and at a restart past it) and with 1 in 13_050_0.
 *   - The PC that plays the partner answers the own slot with 1 (capture), so the stock action switches the LEAD off there. The fix runs the action (and
 *     its siblings that resolve "partner" / "main" the same way) with the own slot answering 0 for that thread: the story's view, slot 1 = the partner.
 *   - Then the PC whose own human is switched off WATCHES the other human: camera manager 1 (which the engine already drives for the character that is not
 *     the own slot: its right stick, its aim) is enabled (+0xC |= 0xC00, as the split does), targeted at the other human (+0x74 / +0x78) and shown in
 *     viewport 0 instead of manager 0. A cutscene camera on viewport 0 is never replaced; when the own human comes back, viewport 0 gets manager 0 again
 *     and manager 1 its old flags / target.
 *   - Meanwhile the other PC's puppet of the switched-off human is held (the story owns it there too: no pad replay, no position correction) and the
 *     downed rule does not count a switched-off partner as present (a lethal hit is an ordinary death, nobody waits for a revive that cannot come).
 */
#ifndef SOLO1_H
#define SOLO1_H
#include <stdint.h>
#include <string.h>

#define S1_UNIT_ON 0xC00u   /* unit +0xC: 0x400 update | 0x800 draw */

/* the slot the own-slot query must answer inside a story partner action: 0 (the story's lead), or -1 = no scope needed (capture off: the engine answers 0
 * itself; this PC plays the lead: already 0) */
static inline int s1_story_scope(int capture_on, int own_effective) { return capture_on && own_effective != 0 ? 0 : -1; }

/* ---- one player unit: is it switched off by the story? ----
 * off = the update AND the draw bit are both clear (FsmSetEnablePartner clears both) AND (the unit was seen with both set before, or the story hook said "off"
 * for this unit), or the story hook said "off". A unit that was never seen on and the hook never named is not called off (a stray flag pattern of a unit
 * being built is no solo part). A new unit pointer starts over. ep1-probe e1p1 / e1p2 live: the grating's examine motion clears the draw bit alone (+0xC
 * 180f4452 / 200f4452: 0x400 set, 0x800 clear) for a minute; the old rule (one bit missing = off) started the WATCH on the host's own Jill during it. */
typedef struct {
    uint32_t unit;
    int seen_on, said_off, off;
    uint32_t since;      /* now of the last change of off */
    unsigned changes;
} s1_unit;

static inline void s1_unit_reset(s1_unit *u) { memset(u, 0, sizeof *u); }

/* hook_said: 1 = the story switched this unit off just now, 0 = switched it on, -1 = nothing. Returns 1 when off changed. */
static inline int s1_unit_step(s1_unit *u, uint32_t unit, uint32_t flags, int hook_said, uint32_t now)
{
    int prev = u->off;   /* a new unit (a reload) under a switched-off one is a change too: the slot is back in play until the story says otherwise */
    if (unit != u->unit) {
        unsigned ch = u->changes;
        s1_unit_reset(u);
        u->unit = unit, u->changes = ch, u->since = now;
    }
    int off = 0;
    if (unit) {
        if (hook_said == 1) u->said_off = 1;
        else if (hook_said == 0) u->said_off = 0;
        int on = (flags & S1_UNIT_ON) == S1_UNIT_ON, both_clear = (flags & S1_UNIT_ON) == 0;
        if (on) u->seen_on = 1;
        off = (both_clear && u->seen_on) || (!on && u->said_off);
    }
    u->off = off;
    if (off != prev) {
        u->since = now, u->changes++;
        return 1;
    }
    return 0;
}

/* ---- the watch (the PC whose own human is switched off) ---- */
typedef struct {
    int in_session;                 /* a network session runs with the roles bound (the world model is bound) */
    uint32_t own_unit, other_unit;  /* the human of this PC / the other human's unit here */
    int own_off, other_off;         /* s1_unit off */
    uint32_t mgr0, mgr1;            /* [sCamera+0xCE0] / [+0xCE4], 0 when not a live manager */
    uint32_t vp0;                   /* viewport 0's camera now */
    uint32_t mgr1_flags, mgr1_t74, mgr1_t78;   /* manager 1 now */
} s1_in;

enum { S1_W_IDLE = 0, S1_W_ON };
typedef struct {
    int state;
    int disabled;                   /* console `world solo watch 0`: no camera change (the story view and the holds stay) */
    uint32_t mgr0, mgr1, own_unit, watched;    /* the instance of the watch */
    uint32_t sv_flags, sv_t74, sv_t78;         /* manager 1 as it was before the watch */
    int back_pending;               /* the watch ended under a cutscene camera: the scheduler gives viewport 0 back manager 1 later -> manager 0 then */
    unsigned enters, exits, vp_sets, vp_backs, tgt_sets, cutscene_passes;
} s1_watch;

typedef struct {
    int set_vp0;    uint32_t vp0;
    int set_flags;  uint32_t flags;
    int set_tgt;    uint32_t t74, t78;
    int entered, left;              /* for the log */
    int gone;                       /* left because the level / managers went away (nothing written) */
} s1_out;

static inline int s1_watch_want(const s1_watch *w, const s1_in *in)
{
    return in->in_session && !w->disabled && in->own_unit && in->own_off && in->other_unit && !in->other_off && in->mgr0 && in->mgr1 && in->mgr0 != in->mgr1;
}

static inline void s1_watch_step(s1_watch *w, const s1_in *in, s1_out *o)
{
    memset(o, 0, sizeof *o);
    int want = s1_watch_want(w, in);
    if (w->state == S1_W_ON) {
        int same_mgrs = in->mgr0 == w->mgr0 && in->mgr1 == w->mgr1;
        if (!want || !same_mgrs || in->own_unit != w->own_unit || in->other_unit != w->watched) {
            w->state = S1_W_IDLE, w->exits++, o->left = 1;
            if (same_mgrs && in->mgr0 && in->mgr1) {
                if (in->vp0 == w->mgr1) o->set_vp0 = 1, o->vp0 = w->mgr0, w->vp_backs++;
                else if (in->vp0 != w->mgr0) w->back_pending = 1;   /* a cutscene camera now: it may hand manager 1 back when it ends */
                o->set_flags = 1, o->flags = w->sv_flags;
                o->set_tgt = 1, o->t74 = w->sv_t74, o->t78 = w->sv_t78;
            } else
                o->gone = 1, w->back_pending = 0;
            return;
        }
    } else if (want) {
        w->state = S1_W_ON, w->enters++, o->entered = 1, w->back_pending = 0;
        w->mgr0 = in->mgr0, w->mgr1 = in->mgr1, w->own_unit = in->own_unit, w->watched = in->other_unit;
        w->sv_flags = in->mgr1_flags, w->sv_t74 = in->mgr1_t74, w->sv_t78 = in->mgr1_t78;
    }
    if (w->state != S1_W_ON) {
        if (w->back_pending) {
            if (in->mgr0 != w->mgr0 || in->mgr1 != w->mgr1 || !in->mgr0) w->back_pending = 0;
            else if (in->vp0 == w->mgr1) o->set_vp0 = 1, o->vp0 = w->mgr0, w->back_pending = 0, w->vp_backs++;
            else if (in->vp0 == w->mgr0) w->back_pending = 0;
        }
        return;
    }
    /* ON: manager 1 updates and follows the other human; viewport 0 shows it while viewport 0 shows gameplay */
    if ((in->mgr1_flags & S1_UNIT_ON) != S1_UNIT_ON) o->set_flags = 1, o->flags = in->mgr1_flags | S1_UNIT_ON;
    if (in->mgr1_t74 != w->watched || in->mgr1_t78 != w->watched) o->set_tgt = 1, o->t74 = o->t78 = w->watched, w->tgt_sets++;
    if (in->vp0 == w->mgr0) o->set_vp0 = 1, o->vp0 = w->mgr1, w->vp_sets++;
    else if (in->vp0 != w->mgr1) w->cutscene_passes++;   /* a cutscene / event camera: left alone, it gives back what it found */
}

/* viewport 0 shows the gameplay view: manager 0, or manager 1 while this PC watches (the puppet module must not take the watch for a cutscene: it would hold
 * the other human's puppet, the very character this PC watches) */
static inline int s1_cam_gameplay(const s1_watch *w, uint32_t vp0, uint32_t mgr0)
{
    if (vp0 == mgr0) return 1;
    return w && w->state == S1_W_ON && vp0 == w->mgr1 && mgr0 == w->mgr0;
}

/* the downed rule: the partner counts as present while his PC is connected and the story has not switched him off here */
static inline int s1_partner_present(int present, int partner_off) { return present && !partner_off; }

/* the puppet module: the hold flags of THIS PC for the other human's puppet; a puppet the story switched off here is the story's (SILENT, as in a
 * cutscene: no pad replay, no position correction; one teleport to the owner's position when it comes back) */
static inline uint16_t s1_puppet_hold(uint16_t hold, int puppet_off, uint16_t cutscene_flag) { return puppet_off ? (uint16_t)(hold | cutscene_flag) : hold; }

#endif
