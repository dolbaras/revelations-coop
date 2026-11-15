/*
 * interact1.h - fix022 (the lead 2026-11-13, a player report: "in split screen the second player cannot interact with doors and cannot pass through them"):
 * the second human of the split screen operates the world's interactables (doors, items, switches, ladders ...). Pure (no Windows, no engine): capture.c feeds it, tests/test_interact1.c
 * drives it on a model of the engine's rule.
 *
 * ROOT CAUSE (notes\net-items-doors-re.md sections 2 / 3 / 7 + the 2026-11-13 reading, notes\fix022.md):
 *  - Everything an object does for "the player" asks the OWN slot: FUN_00882D60 (~190 callers; offline = FUN_0087E1B0 = sCoopManager+0xF8 or 0, answered by capture.c). FUN_0067AF10 =
 *    FUN_00882670(FUN_00882D60()) = "the own unit" is read by 85 object functions (the door's closed state 0x62A990 decides the opener from it, 0x62B4E0 passes the opener's slot on,
 *    the item's uItem_vf39 0x5D9C50 picks its candidate from it ...), the prompt is a cActionCommand owned by that slot and polled by sActionCommand (vtable 0xFEC094 slot 6 = 0x412600),
 *    which reads the raw pad of the CURRENT pad slot (sGamePad+0x870, 0 outside a character's own phases), and the pickup commit asks FUN_00881DF0(slot) = "slot == 0" offline.
 *  - In the split screen the own slot is the FIRST human's for ever (the camera managers, the HUD set and every "cam = slot == own ? mgr0 : mgr1" test depend on it), so the second human
 *    (slot field 1 / pad 1) is invisible to every door, item and switch: no prompt (the command is made for the own unit only), his button is never read (pad slot 0), his pickup is refused.
 *
 * THE RULE (this file): an interactable object is OPERATED BY the human that stands nearest to it. The object's own phases (the three phase dispatchers of capture.c and the state-machine
 * tick) run in the context of the OTHER human (the one who is not the own slot: his pad slot, his view index, the own-slot answer = his slot) while he is the nearest one within reach;
 * otherwise nothing changes (the first human / the stock path). The central action-command update (the one place that reads the buttons) runs in the context of the human who presses
 * a button of his own pad while he operates something and the first human presses nothing. A pickup commit is allowed for both humans while the split is on (the guard's mask).
 */
#ifndef INTERACT1_H
#define INTERACT1_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#define IA_REACH 450.0f        /* the other human operates an object this close (xz) and nearer than the first human */
#define IA_KEEP 800.0f         /* ... and keeps it while he is this close and not farther than IA_STICK behind the first human (the door's opening takes seconds) */
#define IA_STICK 200.0f        /* (the old stickiness: replaced by IA_HYST for an idle object, kept as the busy object's rule) */
#define IA_HYST 30.0f          /* an idle object changes hands when the other human is more than this much farther than the first one (sd14: the door he used stayed his for 100 u behind) */
#define IA_DY 300.0f           /* another floor does not count */
#define IA_MAX 48
#define IA_BTN_MASK 0xF300u    /* A B X Y LB RB: what an interaction can ask for (the triggers are aim / fire) */

typedef struct {
    uint32_t unit, mgr;        /* the object, its state manager (unit+0xE80; the machine tick's uCallThink has it at +0x30); 0 = none */
    float pos[3];
    uint8_t other;             /* 1 = operated by the other human */
    uint8_t item;              /* 1 = a world item (its action command is embedded at +0xFB0), 0 = an object (a pointer at +0xF3C) */
} ia_ent;

typedef struct {
    ia_ent e[IA_MAX];
    unsigned n;                /* entries used (only those the OTHER human operates are kept) */
} ia_tab;

/* the nearest-human rule for one object: 1 = the other human operates it. d_own / d_other = xz distances (< 0 = that human is not there), dy_* = height differences */
static inline int ia_pick(float d_own, float dy_own, float d_other, float dy_other, int prev_other, int busy)
{
    if (d_other < 0.0f || fabsf(dy_other) > IA_DY) return 0;
    int own_ok = d_own >= 0.0f && fabsf(dy_own) <= IA_DY;
    /* an interaction of his in progress (the door's state machine is not idle) stays his until it ends or he is out of IA_KEEP, whoever is nearer */
    if (prev_other && busy) return d_other <= IA_KEEP;
    /* an idle object: the nearest human, with a small hysteresis for the one who had it */
    if (prev_other) return d_other <= IA_REACH + IA_HYST && (!own_ok || d_other <= d_own + IA_HYST);
    return d_other <= IA_REACH && (!own_ok || d_other < d_own);
}

static inline int ia_find(const ia_tab *t, uint32_t unit)
{
    for (unsigned i = 0; i < t->n; i++)
        if (t->e[i].unit == unit) return (int)i;
    return -1;
}
static inline int ia_find_mgr(const ia_tab *t, uint32_t mgr)
{
    if (!mgr) return -1;
    for (unsigned i = 0; i < t->n; i++)
        if (t->e[i].mgr == mgr) return (int)i;
    return -1;
}

/* a pass over the objects: fills `out` with the objects the other human operates (previous = the table of the last pass, for the stickiness). Returns the count. */
typedef struct { uint32_t unit, mgr; float x, y, z; uint8_t item; uint8_t busy; } ia_obj;   /* busy: its state machine is not idle (a door that is opening) */
static inline unsigned ia_build(ia_tab *out, const ia_tab *prev, const ia_obj *objs, unsigned n, const float own[3], int own_ok, const float other[3], int other_ok)
{
    out->n = 0;
    if (!other_ok) return 0;
    for (unsigned i = 0; i < n && out->n < IA_MAX; i++) {
        float dxo = other[0] - objs[i].x, dzo = other[2] - objs[i].z, dyo = other[1] - objs[i].y;
        float d_other = sqrtf(dxo * dxo + dzo * dzo);
        float d_own = -1.0f, dy_own = 0.0f;
        if (own_ok) {
            float dx = own[0] - objs[i].x, dz = own[2] - objs[i].z;
            d_own = sqrtf(dx * dx + dz * dz), dy_own = own[1] - objs[i].y;
        }
        int was = prev && ia_find(prev, objs[i].unit) >= 0;
        if (!ia_pick(d_own, dy_own, d_other, dyo, was, objs[i].busy)) continue;
        ia_ent *e = &out->e[out->n++];
        e->unit = objs[i].unit, e->mgr = objs[i].mgr, e->pos[0] = objs[i].x, e->pos[1] = objs[i].y, e->pos[2] = objs[i].z, e->other = 1, e->item = objs[i].item;
    }
    return out->n;
}

/* the view bits (unit+0xC bits 16..25) of the prompt icon an object / item shows: the icon is made when the command first starts, with the mask of the view being created (viewport 0 for the
 * first human). Operated by the other human = his viewport only, else back to viewport 0. Pure: returns the new flag word. */
static inline uint32_t ia_icon_flags(uint32_t flags, int operated_by_other, unsigned other_view)
{
    uint32_t bit = operated_by_other ? 1u << (16 + (other_view < 10 ? other_view : 0)) : 1u << 16;
    return (flags & ~0x03FF0000u) | bit;
}

/* The other human's OWN state machine asks the own slot too while an object drives him (sd6 2026-11-13: the door's KnobHandOnPlayerState waited for his PlayerHandKnobState, which did nothing
 * for him): his phases run with his own-slot answer while (a) his current state derives from EventStateBase, or (b) a state request is pending whose argument is the serial of one of the
 * objects he operates. is_other = he is not the own slot; his other states keep the first human's answer (the camera rule). */
static inline int ia_event_scope(int live, int is_other, unsigned table_n, int cur_is_event, int req_pending, unsigned req_serial, const unsigned *op_serials, unsigned n_serials)
{
    if (!live || !is_other || !table_n) return 0;
    if (cur_is_event) return 1;
    if (req_pending)
        for (unsigned i = 0; i < n_serials; i++)
            if (op_serials[i] == req_serial) return 1;
    return 0;
}

/* the central action-command update (the single reader of the buttons): whose context does it run in this frame? 1 = the other human's, 0 = the own human's (stock).
 * other_btn / own_btn = the face / shoulder buttons held on their pads; operates = how many objects the other human operates now. */
static inline int ia_central_ctx(unsigned other_btn, unsigned own_btn, unsigned operates)
{
    return operates > 0 && (other_btn & IA_BTN_MASK) != 0 && (own_btn & IA_BTN_MASK) == 0;
}

/* OWNERSHIP (fix022, sd13 live trace 2026-11-14). A world object's "kind" (+0xE30) is its network ownership: 2 = mastered here, 3 = a copy driven by the owner slot (+0xE7C). The state manager's
 * state change (0x49CB70) sets kind 2 when owner slot == own slot (FUN_00882D60) and kind 3 when they differ (a player class 0x8001xxxx keeps its kind). The other human's object code runs in a
 * thread scope whose own-slot answer is HIS slot while the door's owner slot is the machine's (0): the door became a "remote copy" in his KnobHandOn, and the player's PlayerHandKnobState
 * (0x62BFA0) ends at once for a door of kind 0 / 3 (FUN_006CA000) while the door waits for it for ever.
 * What an owner-slot read answers: the real value, or (rule on) for a world object in a scoped thread the scope's slot ("owned by the human whose context runs"). */
static inline unsigned ia_owner_seen(unsigned real_owner, int world_object, int scope_slot, int rule_on)
{
    return rule_on && world_object && scope_slot >= 0 ? (unsigned)scope_slot : real_owner;
}
/* what a store into the owner slot records: the human of the scope taking an object is recorded as the machine's own slot (the first human's code reads it back unscoped) */
static inline unsigned ia_owner_store(unsigned slot, int world_object, int scope_slot, unsigned real_own, int rule_on)
{
    return rule_on && world_object && scope_slot >= 0 && slot == (unsigned)scope_slot ? real_own : slot;
}
/* the state change: the kind the object has afterwards (own = the own-slot answer of the thread that runs the state change) */
static inline int ia_kind_after_state_change(unsigned owner_seen, unsigned own_answer, int player_class, int kind_before)
{
    if (player_class) return kind_before;
    return owner_seen == own_answer ? 2 : 3;
}
/* PlayerHandKnobState's per-frame check: does the player's state go on? (door kind 0 / 3 = a remote copy: it ends at once; else it lasts while the door's state index is 1 or 2) */
static inline int ia_knob_state_continues(int door_kind, int door_state_idx)
{
    if (door_kind == 0 || door_kind == 3) return 0;
    return door_state_idx == 1 || door_state_idx == 2;
}

/* THE STALL WATCHDOG (sd14 live 2026-11-14): the door's KnobHandOn phase 5 waits for the door's motion resources to be loaded (FUN_006300C0: the resource state of the motion on layers 0 / 1 is 6);
 * they are streamed in around the first human, in the first minute after a section starts a far door does not get them, and the second human stands in his hand-on-the-knob state for ever.
 * An object in KnobHandOn (state id 55AD9BF7) whose runner phase has not changed for IA_STALL_MS is asked to go back (runner request +0x18 := 2, what the state's own update does when the
 * opener leaves the knob states): the door closes, the second human is free and can try again. */
#define IA_STALL_MS 6000u
#define IA_ID_KNOBHANDON 0x55AD9BF7u
#define IA_ID_CLOSED 0x5F9729E9u   /* the idle state of the doors / boxes (an object without a state machine reads 0) */
typedef struct { int armed; int phase; uint32_t since; } ia_stall_t;
static inline int ia_stall_step(ia_stall_t *s, int in_knob, int phase, uint32_t now, uint32_t limit_ms)
{
    if (!in_knob) { s->armed = 0; return 0; }
    if (!s->armed || s->phase != phase) { s->armed = 1; s->phase = phase; s->since = now; return 0; }
    if ((uint32_t)(now - s->since) >= limit_ms) { s->since = now; return 1; }
    return 0;
}

#endif
