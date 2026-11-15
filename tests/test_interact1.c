/* test_interact1.c - the split screen's interaction rule (native/interact1.h) on a model of the engine's own-slot rule: zig cc -o test_interact1.exe test_interact1.c
 *
 * The model engine (notes\fix022.md): an object's phase asks "the own unit" (own slot) and, when that unit stands within range, makes a prompt command OWNED by that slot; the central
 * action-command update reads the pad of the CURRENT pad slot and accepts a command only when its owner is the own slot of the context it runs in. The stock context is slot 0 / pad 0
 * for everything but the characters' own phases (capture.c wraps those). "Old rule" = the 0.2.1 model: no object or central context of the other human. */
#include <stdio.h>
#include <string.h>
#include "../native/interact1.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { float pos[3]; unsigned btn; } human_t;
typedef struct {
    float pos[3];
    int prompt_owner;     /* -1 = no prompt, else the slot the command belongs to */
    int open_by;          /* -1 = closed, else the slot that opened it */
    unsigned long prompt_frames;
    int busy;             /* its state machine is not idle (an opening in progress) */
} door_t;

#define DOOR_RANGE 200.0f

static float xz(const float *a, const float *b) { float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }

/* one frame of the model. new_rule: the other human's contexts exist. own = 0 (the split's own slot never changes). */
static void frame(door_t *d, const human_t h[2], int new_rule, ia_tab *tab, ia_tab *prev, int *other_operates_door)
{
    const int own = 0;
    /* the table pass (capture_frame, once per frame) */
    if (new_rule) {
        ia_obj o = {1u, 100u, d->pos[0], d->pos[1], d->pos[2], 0, (uint8_t)d->busy};
        ia_tab t;
        ia_build(&t, prev, &o, 1, h[0].pos, 1, h[1].pos, 1);
        *tab = t;
        *prev = t;
    } else tab->n = 0;
    *other_operates_door = ia_find(tab, 1u) >= 0;
    /* the door's phase: the context = the other human's when the table says so, else the own slot's */
    int ctx_slot = *other_operates_door ? 1 : own;
    if (d->open_by < 0) {
        if (xz(h[ctx_slot].pos, d->pos) <= DOOR_RANGE) d->prompt_owner = ctx_slot, d->prompt_frames++;
        else d->prompt_owner = -1;
    }
    /* the central update: its context = the other human's only when ia_central_ctx says */
    int central_other = new_rule && ia_central_ctx(h[1].btn, h[0].btn, tab->n);
    int own_c = central_other ? 1 : own, pad_c = central_other ? 1 : 0;
    if (d->open_by < 0 && d->prompt_owner == own_c && (h[pad_c].btn & 0x1000u)) d->open_by = own_c;
}

static void run(door_t *d, human_t h[2], int new_rule, int frames, ia_tab *tab, ia_tab *prev)
{
    int op;
    for (int i = 0; i < frames; i++) frame(d, h, new_rule, tab, prev, &op);
}

static door_t mk(void)
{
    door_t d = {{0, 0, 0}, -1, -1, 0, 0};
    return d;
}

/* the two-stage door (sd6 2026-11-13: closed -> KnobHandOn [the door asked the opener's own state machine for PlayerHandKnobState and waits for it] -> open): the opener's event state completes
 * only when the own-slot answer in HIS phases is his own slot. event_rule = the event scope (ia_event_scope), else his phases see the first human's answer (0). */
static int knob_door(int event_rule)
{
    ia_tab tab, prev;
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    human_t h[2] = {{{3000, 0, 0}, 0}, {{100, 0, 0}, 0x1000}};
    float dpos[3] = {0, 0, 0};
    int stage = 0, opener = -1;
    for (int f = 0; f < 120; f++) {
        ia_obj o = {1u, 100u, dpos[0], dpos[1], dpos[2], 0};
        ia_build(&tab, &prev, &o, 1, h[0].pos, 1, h[1].pos, 1);
        prev = tab;
        int other_operates = ia_find(&tab, 1u) >= 0;
        int ctx_slot = other_operates ? 1 : 0;
        int central_other = ia_central_ctx(h[1].btn, h[0].btn, tab.n);
        if (stage == 0 && xz(h[ctx_slot].pos, dpos) <= DOOR_RANGE && (h[central_other ? 1 : 0].btn & 0x1000u) && (central_other ? 1 : 0) == ctx_slot) stage = 1, opener = ctx_slot;
        if (stage == 1) {   /* the opener's own event state: completes when his phase's own-slot answer is his slot */
            unsigned ser[1] = {0x25u};
            int answer = (opener == 1 && ia_event_scope(event_rule, 1, tab.n, 1, 0, 0, ser, 1)) ? 1 : 0;
            if (answer == opener) stage = 2;
        }
    }
    return stage;
}

int main(void)
{
    ia_tab tab, prev;
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);

    /* the pure rule */
    CHECK(ia_pick(-1, 0, 100, 0, 0, 0));              /* the first human is gone: the other one operates */
    CHECK(!ia_pick(100, 0, -1, 0, 0, 0));             /* the other human is not there */
    CHECK(ia_pick(300, 0, 120, 0, 0, 0));             /* nearer */
    CHECK(!ia_pick(120, 0, 300, 0, 0, 0));            /* farther */
    CHECK(!ia_pick(300, 0, 500, 0, 0, 0));            /* out of reach */
    CHECK(ia_pick(500, 0, 700, 0, 1, 1));             /* a BUSY object (his interaction in progress) stays his inside IA_KEEP whoever is nearer */
    CHECK(ia_pick(85, 0, 190, 0, 1, 1));              /* ... even with the first human at 85 u (sd14) */
    CHECK(!ia_pick(300, 0, 700 + 150, 0, 1, 1));      /* beyond IA_KEEP */
    CHECK(!ia_pick(85, 0, 190, 0, 1, 0));             /* an IDLE object he had: the first human 105 u nearer takes it (sd14: A's A did nothing at the door B had just used) */
    CHECK(ia_pick(100, 0, 120, 0, 1, 0));             /* ... but a small hysteresis keeps it for the one who had it */
    CHECK(!ia_pick(100, 0, 140, 0, 1, 0));
    CHECK(ia_pick(-1, 0, 480, 0, 1, 0) && !ia_pick(-1, 0, 490, 0, 1, 0));   /* alone: IA_REACH + IA_HYST */
    CHECK(!ia_pick(300, 0, 100, 900, 0, 0));          /* another floor */

    /* the prompt icon's view bits (unit+0xC bits 16..25): the other human's viewport only, or back to the first human's */
    CHECK(ia_icon_flags(0x000F4FF9u | 0x10000u, 1, 2) == ((0x000F4FF9u & ~0x03FF0000u) | 0x40000u));   /* viewport 2 = bit 18 */
    CHECK(ia_icon_flags(0x00040000u | 0x2000u, 0, 2) == (0x2000u | 0x10000u));                        /* back to viewport 0 = bit 16, the other bits kept */
    CHECK(ia_icon_flags(0x2000u, 1, 99) == (0x2000u | 0x10000u));                                      /* an absurd view index falls back to viewport 0 */

    /* the two-stage door: the object contexts alone STALL in the knob state (the live sd6 result), with the event scope the second human opens it */
    CHECK(knob_door(0) == 1);
    CHECK(knob_door(1) == 2);
    printf("model with the object contexts only: the second human's door stays in stage %d (KnobHandOn), with the event scope in stage %d (open)\n", knob_door(0), knob_door(1));

    /* the event scope (the other human's own state machine) */
    {
        unsigned ser[2] = {0x25u, 0x31u};
        CHECK(ia_event_scope(1, 1, 2, 1, 0, 0, ser, 2));            /* he is in an object's event state */
        CHECK(ia_event_scope(1, 1, 2, 0, 1, 0x25u, ser, 2));        /* a request for one of his objects is pending */
        CHECK(!ia_event_scope(1, 1, 2, 0, 1, 0x99u, ser, 2));       /* a request for something else: his normal states keep the first human's answer */
        CHECK(!ia_event_scope(1, 1, 2, 0, 0, 0x25u, ser, 2));       /* nothing pending, not in an event state */
        CHECK(!ia_event_scope(1, 0, 2, 1, 1, 0x25u, ser, 2));       /* the own human: never */
        CHECK(!ia_event_scope(0, 1, 2, 1, 1, 0x25u, ser, 2));       /* the table is not live */
        CHECK(!ia_event_scope(1, 1, 0, 1, 1, 0x25u, ser, 0));       /* he operates nothing */
    }

    /* the central context */
    CHECK(ia_central_ctx(0x1000, 0, 1));
    CHECK(!ia_central_ctx(0x1000, 0x1000, 1));        /* both press: the first human's */
    CHECK(!ia_central_ctx(0x1000, 0, 0));             /* he operates nothing */
    CHECK(!ia_central_ctx(0x0010, 0, 1));             /* START is the menu exchange's */
    CHECK(!ia_central_ctx(0, 0, 1));

    /* the model engine, the OLD rule (0.2.1): the second human never opens a door; the first human does */
    {
        human_t h[2] = {{{3000, 0, 0}, 0}, {{100, 0, 0}, 0}};   /* the first human far away, the second at the door */
        door_t d = mk();
        run(&d, h, 0, 20, &tab, &prev);
        CHECK(d.prompt_owner == -1 && d.prompt_frames == 0);    /* no prompt for him */
        h[1].btn = 0x1000;
        run(&d, h, 0, 20, &tab, &prev);
        CHECK(d.open_by == -1);                                 /* MODEL BEFORE THE FIX FAILS: he presses A at the door and nothing happens */
        printf("model before the fix: the second human at the door, A pressed 20 frames: open_by %d (prompt frames %lu)\n", d.open_by, d.prompt_frames);
    }
    {
        human_t h[2] = {{{100, 0, 0}, 0}, {{3000, 0, 0}, 0}};   /* the first human at the door: the stock path works */
        door_t d = mk();
        run(&d, h, 0, 5, &tab, &prev);
        h[0].btn = 0x1000;
        run(&d, h, 0, 5, &tab, &prev);
        CHECK(d.open_by == 0);
    }

    /* the NEW rule */
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    {
        human_t h[2] = {{{3000, 0, 0}, 0}, {{100, 0, 0}, 0}};
        door_t d = mk();
        run(&d, h, 1, 5, &tab, &prev);
        CHECK(d.prompt_owner == 1 && d.prompt_frames == 5);     /* the prompt is made for him from the first frame */
        run(&d, h, 1, 5, &tab, &prev);
        CHECK(d.open_by == -1);                                 /* no press yet */
        h[1].btn = 0x1000;
        run(&d, h, 1, 1, &tab, &prev);
        CHECK(d.open_by == 1);                                  /* his A opens it, in the frame it is pressed */
    }
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    {   /* the first human at the door, the second far: unchanged */
        human_t h[2] = {{{100, 0, 0}, 0}, {{3000, 0, 0}, 0}};
        door_t d = mk();
        run(&d, h, 1, 5, &tab, &prev);
        CHECK(d.prompt_owner == 0 && tab.n == 0);
        h[0].btn = 0x1000;
        run(&d, h, 1, 1, &tab, &prev);
        CHECK(d.open_by == 0);
    }
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    {   /* both at the door, the second one nearer: he operates it; the first human's A opens nothing (the nearest rule) */
        human_t h[2] = {{{150, 0, 0}, 0}, {{60, 0, 0}, 0}};
        door_t d = mk();
        h[0].btn = 0x1000;
        run(&d, h, 1, 10, &tab, &prev);
        CHECK(d.prompt_owner == 1 && d.open_by == -1);
        h[0].btn = 0;
        h[1].btn = 0x1000;
        run(&d, h, 1, 1, &tab, &prev);
        CHECK(d.open_by == 1);
    }
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    {   /* both at the door, the first one nearer: he opens it, as before */
        human_t h[2] = {{{60, 0, 0}, 0x1000}, {{150, 0, 0}, 0x1000}};
        door_t d = mk();
        run(&d, h, 1, 3, &tab, &prev);
        CHECK(d.open_by == 0);
    }
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    {   /* the second human walks away from a door he operates (busy = the opening in progress): keeps it to IA_KEEP, then it is the stock path again */
        human_t h[2] = {{{3000, 0, 0}, 0}, {{100, 0, 0}, 0}};
        door_t d = mk();
        run(&d, h, 1, 3, &tab, &prev);
        d.busy = 1;
        CHECK(tab.n == 1);
        h[1].pos[0] = 700;
        run(&d, h, 1, 1, &tab, &prev);
        CHECK(tab.n == 1);
        h[1].pos[0] = 900;
        run(&d, h, 1, 1, &tab, &prev);
        CHECK(tab.n == 0);
    }
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    {   /* sd14: the second human has just passed a door (idle again), the first one stands at it 105 u nearer and presses A: it is his (stock path) */
        human_t h[2] = {{{85, 0, 0}, 0}, {{-190, 0, 0}, 0}};
        door_t d = mk();
        h[1].pos[0] = 60;
        h[0].pos[0] = 150;
        run(&d, h, 1, 3, &tab, &prev);
        CHECK(tab.n == 1);                       /* he had it (nearer) */
        h[0].pos[0] = 85;
        h[1].pos[0] = 190;
        run(&d, h, 1, 1, &tab, &prev);
        CHECK(tab.n == 0);                       /* the idle door is the first human's now */
        h[0].btn = 0x1000;
        run(&d, h, 1, 1, &tab, &prev);
        CHECK(d.open_by == 0);
    }
    memset(&tab, 0, sizeof tab), memset(&prev, 0, sizeof prev);
    {   /* the table: manager lookup, capacity */
        ia_obj objs[IA_MAX + 10];
        for (unsigned i = 0; i < IA_MAX + 10; i++) objs[i].unit = 100 + i, objs[i].mgr = 5000 + i, objs[i].x = (float)i, objs[i].y = 0, objs[i].z = 0;
        float own[3] = {5000, 0, 0}, oth[3] = {0, 0, 0};
        ia_tab t;
        unsigned n = ia_build(&t, NULL, objs, IA_MAX + 10, own, 1, oth, 1);
        CHECK(n == IA_MAX);
        CHECK(ia_find(&t, 100) == 0 && ia_find(&t, 99999) < 0);
        CHECK(ia_find_mgr(&t, 5003) == 3 && ia_find_mgr(&t, 0) < 0 && ia_find_mgr(&t, 77) < 0);
        CHECK(ia_build(&t, NULL, objs, 4, own, 1, oth, 0) == 0);   /* the other human is not there: nothing */
    }

    {   /* OWNERSHIP (sd13 live trace): the second human's door in his thread scope (own-slot answer 1, owner slot 0) */
        int world = 1;
        unsigned seen_old = ia_owner_seen(0, world, 1, 0);   /* the 0.2.1 rule: the owner slot is read as it is */
        int kind_old = ia_kind_after_state_change(seen_old, 1, 0, 2);
        CHECK(kind_old == 3);                                /* the door becomes a remote copy ... */
        CHECK(!ia_knob_state_continues(kind_old, 1));        /* ... and his PlayerHandKnobState ends at once: the stall */
        unsigned seen = ia_owner_seen(0, world, 1, 1);       /* the fix */
        int kind = ia_kind_after_state_change(seen, 1, 0, 2);
        CHECK(seen == 1 && kind == 2);
        CHECK(ia_knob_state_continues(kind, 1) && ia_knob_state_continues(kind, 2) && !ia_knob_state_continues(kind, 0) && !ia_knob_state_continues(kind, 4));
        CHECK(ia_owner_seen(0, world, -1, 1) == 0 && ia_kind_after_state_change(0, 0, 0, 2) == 2);   /* the first human (no scope): unchanged */
        CHECK(ia_kind_after_state_change(0, 1, 1, 1) == 1);  /* a player class keeps its kind */
        CHECK(ia_owner_seen(0, 0, 1, 1) == 0);               /* not a world object: untouched */
        CHECK(ia_owner_store(1, world, 1, 0, 1) == 0);       /* the store: the scope's slot is recorded as the real own slot */
        CHECK(ia_owner_store(2, world, 1, 0, 1) == 2);
        CHECK(ia_owner_store(1, world, -1, 0, 1) == 1);
        CHECK(ia_owner_store(1, 0, 1, 0, 1) == 1);
        CHECK(ia_owner_store(1, world, 1, 0, 0) == 1);       /* rule off: stock */
        CHECK(ia_kind_after_state_change(ia_owner_seen(0, world, -1, 1), 0, 0, 3) == 2);   /* the first human's later state change of the same door: mastered here again */
        printf("MODEL BEFORE THE OWNERSHIP FIX: door kind %d, the knob state %s\n", kind_old, ia_knob_state_continues(kind_old, 1) ? "goes on" : "ends at once (the stall)");
    }

    {   /* the stall watchdog: KnobHandOn with an unchanged phase for IA_STALL_MS is cancelled once, then re-armed; a phase change or leaving the state resets */
        ia_stall_t s;
        memset(&s, 0, sizeof s);
        CHECK(!ia_stall_step(&s, 0, 1, 1000, IA_STALL_MS));
        CHECK(!ia_stall_step(&s, 1, 1, 1000, IA_STALL_MS) && !ia_stall_step(&s, 1, 1, 6900, IA_STALL_MS));
        CHECK(ia_stall_step(&s, 1, 1, 7000, IA_STALL_MS));                      /* 6000 ms in phase 1 */
        CHECK(!ia_stall_step(&s, 1, 1, 7100, IA_STALL_MS));                     /* re-armed */
        CHECK(!ia_stall_step(&s, 1, 5, 12000, IA_STALL_MS) && !ia_stall_step(&s, 1, 5, 17000, IA_STALL_MS));   /* phase 5 from 12000 ... */
        CHECK(ia_stall_step(&s, 1, 5, 18000, IA_STALL_MS));
        CHECK(!ia_stall_step(&s, 0, 5, 18100, IA_STALL_MS) && !ia_stall_step(&s, 1, 5, 18200, IA_STALL_MS) && !ia_stall_step(&s, 1, 5, 20000, IA_STALL_MS));   /* left the state: reset */
        CHECK(!ia_stall_step(&s, 1, 1, 30000, IA_STALL_MS) && !ia_stall_step(&s, 1, 4, 33000, IA_STALL_MS) && !ia_stall_step(&s, 1, 5, 36000, IA_STALL_MS) && !ia_stall_step(&s, 1, 6, 39000, IA_STALL_MS));   /* a normal opening moves on */
    }

    printf("test_interact1: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
