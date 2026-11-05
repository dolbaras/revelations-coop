/* Offline test of the pad routing rules (native/padmap.h): R1-1 (the mod's own peek follows the exchange of the pads), R1-2 (numbering gaps), R1-3 (rumble follows the routing),
 * R1-4 (one physical pad seen twice is one player), R1-5 (a real pad is only read while the window is in front). Build + run: sh tests/build.sh. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../native/padmap.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } else printf("ok   %s\n", msg); } while (0)

/* a fake PC: which physical users are connected and what each one sends */
static r1_input_t fake[R1_PM_USERS];
static unsigned fake_conn;
static int rumble_to = -9;

static r1_padlist_t list_of(unsigned conn, unsigned dup)
{
    r1_padlist_t l;
    r1_padlist_build(&l, conn, dup);
    return l;
}

/* what XInputGetState(game_user) / the peek of the mod gives: both use r1_pad_route (this is the rule testmode.c uses for state, capabilities, rumble and peek) */
static int get_state(const r1_padlist_t *l, int game_user, int swap, r1_input_t *out)
{
    int ph = r1_pad_route(l, game_user, swap);
    if (ph < 0)
        return 0;
    *out = fake[ph];
    return 1;
}
static int set_rumble(const r1_padlist_t *l, int game_user, int swap) { rumble_to = r1_pad_route(l, game_user, swap); return rumble_to >= 0; }

static void sample(r1_input_t *a, short lx, short ly, unsigned short b)
{
    memset(a, 0, sizeof *a);
    a->lx = lx, a->ly = ly, a->buttons = b;
}

int main(void)
{
    memset(fake, 0, sizeof fake);
    r1_padlist_t l;

    /* ---- normal case: pads on users 0 and 1 ---- */
    l = list_of(3, 0);
    CHECK(l.n == 2 && l.phys[0] == 0 && l.phys[1] == 1, "pads on users 0 and 1: game user k = physical user k");
    CHECK(r1_pad_route(&l, 0, 0) == 0 && r1_pad_route(&l, 1, 0) == 1 && r1_pad_route(&l, 2, 0) == -1 && r1_pad_route(&l, 3, 0) == -1, "no exchange: identity, users 2 and 3 absent");
    CHECK(r1_pad_route(&l, 0, 1) == 1 && r1_pad_route(&l, 1, 1) == 0, "exchange: game users 0 and 1 swap");
    CHECK(r1_pad_route(&l, -1, 0) == -1 && r1_pad_route(&l, 4, 0) == -1 && r1_pad_route(&l, 9, 1) == -1, "out of range users are nobody");

    /* ---- R1-1: peek vs the pad exchange ("player 1 uses gamepad 2": the exchange is on all the time) ---- */
    sample(&fake[0], 0, 0, 0x0020);   /* physical pad 0 (first human) presses BACK */
    sample(&fake[1], 0, 0, 0);
    r1_input_t g;
    int ok = get_state(&l, 1, 1, &g);   /* the second human (game user 1) is on physical pad 0 */
    CHECK(ok && g.buttons == 0x0020, "R1-1: with the exchange on, the peek of game user 1 sees physical pad 0 (the old peek read physical pad 1 and took the wrong human's BACK)");
    ok = get_state(&l, 1, 0, &g);
    CHECK(ok && g.buttons == 0, "R1-1: without the exchange the peek of game user 1 sees physical pad 1");

    /* ---- R1-2: gaps in the numbering ---- */
    l = list_of(5, 0);   /* users 0 and 2 */
    CHECK(l.n == 2 && l.phys[0] == 0 && l.phys[1] == 2, "R1-2: pads on users 0 and 2 become game users 0 and 1");
    CHECK(r1_pad_route(&l, 1, 0) == 2 && r1_pad_route(&l, 2, 0) == -1, "R1-2: game user 1 is physical 2, game user 2 is nobody");
    CHECK(r1_pad_route(&l, 0, 1) == 2 && r1_pad_route(&l, 1, 1) == 0, "R1-2: the exchange swaps physical 0 and 2 (the old i ^ 1 asked physical user 1, which is absent)");
    l = list_of(6, 0);   /* users 1 and 2: pad 0 unplugged */
    CHECK(l.n == 2 && r1_pad_route(&l, 0, 0) == 1 && r1_pad_route(&l, 1, 0) == 2, "R1-2: pads on users 1 and 2 are game users 0 and 1");
    l = list_of(8, 0);   /* only user 3 */
    CHECK(l.n == 1 && r1_pad_route(&l, 0, 0) == 3 && r1_pad_route(&l, 1, 0) == -1, "R1-2: a single pad on user 3 is game user 0");
    CHECK(r1_pad_route(&l, 0, 1) == -1 && r1_pad_route(&l, 1, 1) == 3, "one pad and the exchange: the lone pad serves game user 1 (as before)");
    l = list_of(0, 0);
    CHECK(l.n == 0 && r1_pad_route(&l, 0, 0) == -1 && r1_pad_route(&l, 1, 1) == -1, "no pad: nobody");
    l = list_of(15, 0);
    CHECK(l.n == 4 && r1_pad_route(&l, 3, 0) == 3 && r1_pad_route(&l, 2, 1) == 2, "four pads: the exchange touches only the first two");

    /* ---- R1-3: rumble ---- */
    l = list_of(3, 0);
    set_rumble(&l, 0, 0);
    CHECK(rumble_to == 0, "R1-3: no exchange, rumble of game user 0 goes to physical pad 0");
    set_rumble(&l, 0, 1);
    CHECK(rumble_to == 1, "R1-3: with the exchange, rumble of game user 0 goes to physical pad 1 (the old SetState ignored the exchange)");
    set_rumble(&l, 1, 1);
    CHECK(rumble_to == 0, "R1-3: with the exchange, rumble of game user 1 goes to physical pad 0");
    l = list_of(5, 0);
    set_rumble(&l, 1, 0);
    CHECK(rumble_to == 2, "R1-3: pads on 0 and 2: rumble of game user 1 goes to physical 2");
    l = list_of(1, 0);
    CHECK(!set_rumble(&l, 1, 0) && rumble_to == -1, "R1-3: rumble for an absent game user reaches no pad");

    /* ---- R1-5: the focus gate of the mod's own pad reading ---- */
    CHECK(!r1_pad_peek_allowed(0, 0, 0), "R1-5: a real pad is not read while the window is not in front");
    CHECK(r1_pad_peek_allowed(0, 0, 1), "R1-5: the window in front: read");
    CHECK(r1_pad_peek_allowed(1, 0, 0), "R1-5: a synthetic (console) pad does not need the window");
    CHECK(r1_pad_peek_allowed(0, 1, 0), "R1-5: REV1_PADS=1 test copy (virtual pads in the background) reads");

    /* ---- R1-4: duplicates ---- */
    r1_dupdet_t d;
    r1_input_t st[R1_PM_USERS];
    unsigned hide = 0;
    memset(st, 0, sizeof st);

    r1_dupdet_reset(&d);
    for (int k = 0; k < 600; k++) hide = r1_dupdet_feed(&d, st, 3);
    CHECK(hide == 0, "two pads that both rest (neutral, identical) are not a duplicate");

    r1_dupdet_reset(&d);
    for (int k = 0; k < 600; k++) {
        sample(&st[0], (short)(-120 + (k % 7)), (short)(15000 + (k % 5) * 100), (k / 60) % 2 ? 0x1000 : 0);
        st[1] = st[0];   /* a virtual copy of pad 0 */
        hide = r1_dupdet_feed(&d, st, 3);
        if (k == R1_DUP_MIN_SAME - 2)
            CHECK(hide == 0, "R1-4: not yet a verdict before the evidence is complete");
    }
    CHECK(hide == 2, "R1-4: a pad seen twice (identical states while a stick is deflected): the higher user is hidden");
    r1_padlist_t lh = list_of(3, hide);
    CHECK(lh.n == 1 && lh.phys[0] == 0 && r1_pad_route(&lh, 1, 0) == -1, "R1-4: the game and the lobby see one pad, game user 1 is nobody");

    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 600; k++) {   /* a mirror that runs 2 samples behind at every change */
        sample(&st[0], (short)(10000 + (k % 40) * 100), 3000, 0);
        sample(&st[1], (short)(10000 + (k % 40) * 100), 3000, 0);
        if (k % 20 < 2) st[1].lx = (short)(st[1].lx - 100);
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 2, "R1-4: a virtual copy that lags behind in 10 percent of the samples is still a duplicate");

    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 1000; k++) {   /* two humans: one stick moves, the other pad rests */
        sample(&st[0], (short)(12000 + k % 100), 9000, 0);
        sample(&st[1], 0, 0, 0);
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 0, "R1-4: only one pad is used: two players");

    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 1000; k++) {   /* two humans push forward at the same time, with stick noise of their own */
        sample(&st[0], (short)(100 + k % 13), (short)(22000 + k % 37), 0x1000);
        sample(&st[1], (short)(-250 + k % 7), (short)(24000 + k % 29), 0x1000);
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 0, "R1-4: both humans walk the same way, press the same button: still two players (the sticks differ)");

    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 5000; k++) {   /* two virtual test pads at rest: the same odd constant stick value (seen live: ly -1869 on both), nothing ever changes */
        sample(&st[0], 0, -1869, 0);
        st[1] = st[0];
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 0, "R1-4: two pads that sit at the same constant non-zero stick value are no evidence (virtual pads of the lab; real pads differ)");

    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < R1_DUP_MIN_SAME - 1; k++) {   /* just short of the evidence */
        sample(&st[0], (short)(5000 + k), 200, 0);
        st[1] = st[0];
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 0, "R1-4: one change short of the evidence: no verdict");
    sample(&st[0], 9000, 200, 0);
    st[1] = st[0];
    hide = r1_dupdet_feed(&d, st, 3);
    CHECK(hide == 2, "R1-4: the change that completes the evidence: hidden");

    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 1000; k++) {   /* identical digital buttons only (D-pad menus): no analog evidence */
        memset(&st[0], 0, sizeof st[0]);
        st[0].buttons = 0x0001;
        st[1] = st[0];
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 0, "R1-4: identical buttons alone are no evidence");

    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 1000; k++) {   /* both sticks pinned at the rim, equal */
        sample(&st[0], 32767, 32767, 0);
        st[1] = st[0];
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 0, "R1-4: both sticks pinned at the rim are no evidence (two humans may do that)");

    /* flagged, then the second pad is really used by someone else: released again */
    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 400; k++) {
        sample(&st[0], (short)(8000 + k % 50), 4000, 0);
        st[1] = st[0];
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 2, "a flagged pair: hidden");
    for (int k = 0; k < 400 && hide; k++) {
        sample(&st[0], (short)(8000 + k % 50), 4000, 0);
        sample(&st[1], 0, 0, 0x2000);   /* the other pad is pressed by itself */
        hide = r1_dupdet_feed(&d, st, 3);
    }
    CHECK(hide == 0, "a flagged pair that stops being alike is released (two players again)");

    /* the pad is unplugged: the history goes */
    r1_dupdet_reset(&d);
    for (int k = 0; k < 300; k++) {
        sample(&st[0], (short)(8000 + k % 50), 4000, 0);
        st[1] = st[0];
        hide = r1_dupdet_feed(&d, st, 3);
    }
    hide = r1_dupdet_feed(&d, st, 1);
    CHECK(hide == 0, "the second pad is gone: nothing is hidden");
    sample(&st[0], 8000, 4000, 0);
    st[1] = st[0];
    hide = r1_dupdet_feed(&d, st, 3);
    CHECK(hide == 0, "the second pad is back: it starts without a verdict");

    /* three users: 0 and 2 are the same pad, 1 is another */
    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 600; k++) {
        sample(&st[0], (short)(9000 + k % 31), 7000, 0);
        sample(&st[1], (short)(-3000 + k % 17), 100, 0);
        st[2] = st[0];
        hide = r1_dupdet_feed(&d, st, 7);
    }
    CHECK(hide == 4, "users 0 and 2 are one pad: user 2 is hidden, user 1 stays");
    lh = list_of(7, hide);
    CHECK(lh.n == 2 && lh.phys[0] == 0 && lh.phys[1] == 1, "...and the game sees the pads of users 0 and 1");

    /* all three the same: one player */
    r1_dupdet_reset(&d);
    hide = 0;
    for (int k = 0; k < 600; k++) {
        sample(&st[0], (short)(9000 + k % 31), 7000, 0);
        st[1] = st[2] = st[0];
        hide = r1_dupdet_feed(&d, st, 7);
    }
    CHECK(hide == 6, "three copies of one pad: users 1 and 2 are hidden");

    (void)fake_conn;
    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
