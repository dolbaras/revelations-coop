/* test_rguard1.c - the enemy copies' fall-out guard (native/rguard1.h) on a model of the engine's fall-out timer (FUN_006F16B0 -> FUN_006F0660), the zone
 * pass (asleep = no update) and op_update's snap: zig cc -o test_rguard1.exe test_rguard1.c */
#include <stdio.h>
#include "../native/rguard1.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define DT (1.0f / 60.0f)
#define FALLOUT_S 5.0f

typedef struct {
    unsigned kind;          /* 2 = this PC runs it, 3 = a copy */
    int awake, ground, dead;
    float timer, y, floor_y, owner_y;
    int dead_frame, snaps;
} unit_t;

/* one engine frame of the unit (FUN_006F16B0 + gravity + the copy's snap); only an awake unit is updated (the zone pass's E1C 0x1000 = no update) */
static void engine_frame(unit_t *u, int frame, int has_floor)
{
    if (u->dead || !u->awake) return;
    u->ground = has_floor && u->y <= u->floor_y + 0.5f;
    if (!u->ground) u->y -= 600.0f * DT;   /* falling */
    if (has_floor && u->y < u->floor_y) u->y = u->floor_y, u->ground = 1;
    if (u->ground) u->timer = 0.0f;
    else {
        u->timer += DT;
        if (u->timer >= FALLOUT_S) { u->dead = 1, u->dead_frame = frame; return; }   /* FUN_006F0660: state 0x32, hp 0, record used up */
    }
    if (u->kind == 3 && u->owner_y - u->y > 300.0f) u->y = u->owner_y, u->snaps++;   /* op_update: farther than 300 u from the owner's sample */
}

/* the logic pass of local1_game.inc (every `every` frames) */
static void logic_pass(unit_t *u, int on)
{
    if (rg_hold(on, u->kind, u->timer)) u->timer = 0.0f;
}

static unit_t mk(unsigned kind, float y)
{
    unit_t u = {0};
    u.kind = kind, u.awake = 1, u.y = y, u.floor_y = y, u.owner_y = y, u.dead_frame = -1;
    return u;
}

/* run `frames` frames over missing floor (has_floor 0) or a floor; the logic pass every `every` frames; returns the frame of death or -1 */
static int run(unit_t *u, int frames, int has_floor, int on, int every)
{
    for (int f = 0; f < frames; f++) {
        engine_frame(u, f, has_floor);
        if (every > 0 && f % every == 0) logic_pass(u, on);
    }
    return u->dead ? u->dead_frame : -1;
}

int main(void)
{
    /* the predicate */
    CHECK(rg_hold(1, 3, 0.5f));
    CHECK(!rg_hold(1, 3, 0.0f));
    CHECK(!rg_hold(0, 3, 4.9f));
    CHECK(!rg_hold(1, 2, 4.9f));   /* the PC's own enemy: stock */
    CHECK(!rg_hold(1, 1, 4.9f));   /* a human: not this rule (fallres1.h / hunk1.h) */

    /* MODEL BEFORE THE FIX FAILS: a copy awake over floor this PC never loaded (the replica woken in 259/3, zkeep rule 7 / watch rule 4 far away) falls, snaps
     * back to the owner's height, and the engine kills it after 5 s of frames without ground */
    unit_t c = mk(3, 900.0f);
    int d = run(&c, 60 * 20, 0, 0, 1);
    CHECK(d >= 0 && d >= (int)(4.9f * 60) && d <= (int)(5.1f * 60));
    CHECK(c.snaps > 0);
    printf("model before the fix: the copy dies at frame %d (%.2f s) after %d snaps%s\n", d, d / 60.0f, c.snaps, d >= 0 ? " (MODEL BEFORE THE FIX FAILS: as f21s5 07:11:36)" : "");

    /* the guard, the logic pass every frame / every 4 / every 15 frames (250 ms): the copy never dies, keeps falling and snapping (seen, not killed) */
    int ev[3] = {1, 4, 15};
    for (int i = 0; i < 3; i++) {
        unit_t g = mk(3, 900.0f);
        CHECK(run(&g, 60 * 120, 0, 1, ev[i]) == -1);
        CHECK(g.snaps > 0 && !g.dead);
    }

    /* the S5 pattern: four awake spells of ~1.5 s with asleep time between (no update while asleep): the stock timer sums them (dead in the 4th), the guard not */
    for (int on = 0; on <= 1; on++) {
        unit_t s = mk(3, 900.0f);
        int dead_at = -1, frame = 0;
        for (int spell = 0; spell < 4 && dead_at < 0; spell++) {
            s.awake = 1;
            for (int f = 0; f < 90 && !s.dead; f++, frame++) {
                engine_frame(&s, frame, 0);
                logic_pass(&s, on);
            }
            if (s.dead) dead_at = spell;
            s.awake = 0;
            for (int f = 0; f < 60 * 20; f++, frame++) engine_frame(&s, frame, 0);   /* asleep: frozen, the timer kept */
        }
        if (!on) CHECK(dead_at == 3);
        else CHECK(dead_at == -1 && !s.dead);
    }

    /* the PC's own enemy (kind 2) keeps the stock fall-out with the guard on (a pit kills it there, and that death reaches the copy) */
    unit_t o = mk(2, 900.0f);
    d = run(&o, 60 * 20, 0, 1, 1);
    CHECK(d >= (int)(4.9f * 60) && d <= (int)(5.1f * 60));

    /* a copy on loaded floor: the timer stays 0 with or without the guard, nothing changes */
    for (int on = 0; on <= 1; on++) {
        unit_t f = mk(3, 900.0f);
        CHECK(run(&f, 60 * 30, 1, on, 1) == -1);
        CHECK(f.timer == 0.0f && f.snaps == 0 && f.y == 900.0f);
    }

    /* the guard off (`world set rguard 0`) = the model before the fix */
    unit_t z = mk(3, 900.0f);
    CHECK(run(&z, 60 * 20, 0, 0, 1) >= 0);

    printf("test_rguard1: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
