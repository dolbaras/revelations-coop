/*
 * test_scan1.c - offline test of the Genesis scanner for the second player (native/scan1.h, the glue scan1.c; notes/scan1.md).
 *   G: who gets a Genesis (a source on either slot, the allowed slots, one try per bag object, never over another word, not while switched off), and a
 *      model of a whole session: loads (new bags), refusals, the story giving it to one character; in the end every allowed human has one;
 *   X: the split exchange of the second human: every condition of the start, the cooldown, the end (quiet passes, activity, the guard, a lost unit), and a
 *      frame model of two humans who pick up and drop the Genesis at random: never two users of the one scanner, every exchange ends;
 *   R: the rate is shared only in 1-2; the second split human's own rate and his one-herb latch around his exchanges (story mode, a new GUI after a load),
 *      checked against a model of the stock reward (100 -> a herb, reset unless story mode);
 *   V: the REVEAL wire (round trip, every refusal), the receiver's decision, the dedup per level instance, and two PCs on a delayed reliable line with
 *      loads: every hidden item revealed on one PC is visible on both once both are in the same level instance, nothing is applied twice;
 *   build facts: the stock bytes of every call of scan1.c in the executable image (when it is at hand), the vtable slot, the ret sizes of the calls;
 *   glue: wiring in probe.c / build.sh / split.c / split.h / story1_game.inc, the event kind is free.
 *   sh tests/build.sh   ->  tests/build/test_scan1.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/scan1.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned rng = 12345u;
static unsigned rnd(void) { return rng = rng * 1103515245u + 12345u, (rng >> 16) & 0x7FFFu; }

/* ---------------------------------------------------------------- G */
static sc_human hum(int present, int in_play, int has, int empty, uint32_t bag)
{
    sc_human h;
    h.present = present, h.in_play = in_play, h.has = has, h.empty = empty, h.bag = bag;
    return h;
}
static void t_give(void)
{
    sc_give_t g;
    sc_give_init(&g);
    int both[2] = {1, 1}, none[2] = {0, 0}, only1[2] = {0, 1};
    sc_human h[2];
    /* nobody has one: nothing */
    h[0] = hum(1, 1, 0, 1, 0x100), h[1] = hum(1, 1, 0, 1, 0x200);
    CHECK(sc_give_targets(&g, h, both) == 0);
    /* Jill got it from the story: Parker gets one */
    h[0] = hum(1, 1, 1, 0, 0x100);
    CHECK(sc_give_targets(&g, h, both) == 2u);
    /* the other way round (Ep2: Chris has it, Jessica not) */
    h[0] = hum(1, 1, 0, 1, 0x100), h[1] = hum(1, 1, 1, 0, 0x200);
    CHECK(sc_give_targets(&g, h, both) == 1u);
    /* not allowed on this PC (no co-op) */
    CHECK(sc_give_targets(&g, h, none) == 0);
    /* the source may be a puppet (network: the FSM gave it to this PC's copy of the partner) and the target only the allowed one */
    h[0] = hum(1, 1, 1, 0, 0x100), h[1] = hum(1, 1, 0, 1, 0x200);
    CHECK(sc_give_targets(&g, h, only1) == 2u);
    /* switched off by the story (solo part), absent, no bag: never */
    h[1] = hum(1, 0, 0, 1, 0x200);
    CHECK(sc_give_targets(&g, h, both) == 0);
    h[1] = hum(0, 1, 0, 1, 0x200);
    CHECK(sc_give_targets(&g, h, both) == 0);
    h[1] = hum(1, 1, 0, 1, 0);
    CHECK(sc_give_targets(&g, h, both) == 0);
    /* a foreign word at +0x3C: never overwritten (counted) */
    unsigned sk = g.skipped;
    h[1] = hum(1, 1, 0, 0, 0x200);
    CHECK(sc_give_targets(&g, h, both) == 0 && g.skipped == sk + 1);
    /* one try per bag object: a refusal is not repeated, a new bag (a load) is tried again */
    h[1] = hum(1, 1, 0, 1, 0x200);
    CHECK(sc_give_targets(&g, h, both) == 2u);
    sc_give_result(&g, 1, 0x200, 0);
    CHECK(g.refused == 1 && sc_give_targets(&g, h, both) == 0);
    h[1].bag = 0x300;
    CHECK(sc_give_targets(&g, h, both) == 2u);
    sc_give_result(&g, 1, 0x300, 1);
    CHECK(g.gives == 1);
    h[1].has = 1, h[1].empty = 0;
    CHECK(sc_give_targets(&g, h, both) == 0);
    sc_give_result(&g, 5, 0x1, 1);   /* a bad slot is ignored */
    CHECK(g.gives == 1);
    /* both empty, one source missing on both: nothing; both have: nothing */
    h[0] = hum(1, 1, 1, 0, 0x100), h[1] = hum(1, 1, 1, 0, 0x300);
    CHECK(sc_give_targets(&g, h, both) == 0);

    /* a session model: 2000 passes, loads every ~100 passes (new bags, the story re-gives the first character in some levels), a refusal now and then;
     * after every pass in which a source exists and the target is in play with a fresh bag, at most one try per bag; at the end of a level both have one
     * unless the try was refused */
    sc_give_init(&g);
    uint32_t bag[2] = {0x1000, 0x2000};
    int has[2] = {0, 0}, tries_on_bag[2] = {0, 0}, story_level = 0;
    unsigned levels = 0, both_ok = 0, refused_levels = 0;
    int refused_this[2] = {0, 0};
    for (int pass = 0; pass < 2000; pass++) {
        if (pass % 100 == 0) {   /* a load: new bags; the level's story gives the first character a Genesis (or the second, or nobody) */
            if (pass) {
                levels++;
                if (has[0] && has[1]) both_ok++;
                else if (refused_this[0] || refused_this[1]) refused_levels++;
            }
            bag[0] += 0x10, bag[1] += 0x10;
            story_level = (int)(rnd() % 3);
            has[0] = story_level == 1, has[1] = story_level == 2;
            tries_on_bag[0] = tries_on_bag[1] = 0, refused_this[0] = refused_this[1] = 0;
        }
        h[0] = hum(1, 1, has[0], !has[0], bag[0]), h[1] = hum(1, pass % 100 > 5, has[1], !has[1], bag[1]);   /* the second one comes into play a little later */
        unsigned t = sc_give_targets(&g, h, both);
        for (int i = 0; i < 2; i++)
            if (t & (1u << i)) {
                tries_on_bag[i]++;
                int ok = rnd() % 10 != 0;
                if (ok) has[i] = 1;
                else refused_this[i] = 1;
                sc_give_result(&g, i, bag[i], ok);
            }
        CHECK(tries_on_bag[0] <= 1 && tries_on_bag[1] <= 1);
        CHECK(!(t & 1u) || !h[0].has);
    }
    CHECK(levels == 19 && both_ok + refused_levels + 0 >= 1);
    printf("G: %u levels, %u with both armed, %u with a refused try, gives %u refused %u\n", levels, both_ok, refused_levels, g.gives, g.refused);
}

/* ---------------------------------------------------------------- R */
static void t_rate_shared(void)
{
    CHECK(sc_rate_shared(0x91010200u) == 1);
    CHECK(sc_rate_shared(0x91010100u) == 0 && sc_rate_shared(0x91010300u) == 0 && sc_rate_shared(0x91020100u) == 0 && sc_rate_shared(0) == 0);
    CHECK(sc_clamp_rate(-5) == 0 && sc_clamp_rate(150) == 100 && sc_clamp_rate(42) == 42);
}

/* ---------------------------------------------------------------- X */
static sc_enter_in ok_in(void)
{
    sc_enter_in in;
    memset(&in, 0, sizeof in);
    in.split_ok = 1, in.p2_present = 1, in.p2_has = 1, in.p2_allowed = 1, in.p2_hold = 1, in.now = 50000;
    return in;
}
static void t_enter_leave(void)
{
    sc_enter_in in = ok_in();
    CHECK(sc_want_enter(&in) == 1);
    sc_enter_in b;
    b = in, b.split_ok = 0;
    CHECK(!sc_want_enter(&b));
    b = in, b.p2_present = 0;
    CHECK(!sc_want_enter(&b));
    b = in, b.p2_has = 0;
    CHECK(!sc_want_enter(&b));
    b = in, b.p2_allowed = 0;
    CHECK(!sc_want_enter(&b));
    b = in, b.p2_hold = 0;
    CHECK(!sc_want_enter(&b));
    b = in, b.p1_busy = 1;   /* one scanner: the first human is using it */
    CHECK(!sc_want_enter(&b));
    b = in, b.ever_ended = 1, b.last_end = 49500;   /* cooldown */
    CHECK(!sc_want_enter(&b));
    b.last_end = 49000;
    CHECK(sc_want_enter(&b));
    b.ever_ended = 1, b.now = 10, b.last_end = 0xFFFFFF00u;   /* the tick wraps: 0x10A ms later = still the cooldown */
    CHECK(!sc_want_enter(&b));
    b.now = 0x400;
    CHECK(sc_want_enter(&b));

    sc_x_t x;
    memset(&x, 0, sizeof x);
    sc_leave_in l;
    memset(&l, 0, sizeof l);
    l.p2_present = 1;
    CHECK(sc_x_want_leave(&x, &l, 0) == 0);   /* not on */
    sc_x_begin(&x, 1000);
    l.p2_genesis = 1;
    for (int i = 0; i < 50; i++) CHECK(sc_x_want_leave(&x, &l, 1000u + (uint32_t)i * 16u) == 0);
    l.p2_genesis = 0, l.gui_open = 1;
    CHECK(sc_x_want_leave(&x, &l, 2000) == 0);
    l.gui_open = 0, l.p2_view = 1;
    CHECK(sc_x_want_leave(&x, &l, 2016) == 0);
    l.p2_view = 0;
    int left = 0, at = -1;
    for (int i = 0; i < 20 && !left; i++) {
        left = sc_x_want_leave(&x, &l, 2100u + (uint32_t)i * 16u);
        if (left) at = i;
    }
    CHECK(left && at == SC_X_QUIET - 1);
    /* activity in between resets the count */
    sc_x_begin(&x, 5000);
    for (int i = 0; i < SC_X_QUIET - 1; i++) CHECK(!sc_x_want_leave(&x, &l, 5000));
    l.p2_genesis = 1;
    CHECK(!sc_x_want_leave(&x, &l, 5000));
    l.p2_genesis = 0;
    for (int i = 0; i < SC_X_QUIET - 1; i++) CHECK(!sc_x_want_leave(&x, &l, 5000));
    CHECK(sc_x_want_leave(&x, &l, 5000));
    /* the guard and a lost unit end it at once, even while he still holds it */
    sc_x_begin(&x, 100);
    l.p2_genesis = 1;
    CHECK(!sc_x_want_leave(&x, &l, 100 + SC_X_GUARD_MS));
    CHECK(sc_x_want_leave(&x, &l, 101 + SC_X_GUARD_MS));
    sc_x_begin(&x, 100);
    l.p2_present = 0;
    CHECK(sc_x_want_leave(&x, &l, 200));
    /* a story camera (cutscene / event) ends it at once, even while he holds the Genesis (sc1s3: the story acted during an exchange) */
    sc_x_begin(&x, 100);
    l.p2_present = 1, l.p2_genesis = 1, l.gui_open = 1, l.story_camera = 1;
    CHECK(sc_x_want_leave(&x, &l, 116));
    l.story_camera = 0;
}

/* the reward of the stock close (FUN_007A5770): 100 and room -> a herb; the rate reset unless story mode; story mode: one herb (latch) */
typedef struct { int rate, latch, story; uint32_t gui; } game_t;
static int stock_close(game_t *g)
{
    if (g->rate != 100 || (g->story && g->latch)) return 0;
    if (g->story) g->latch = 1;
    else g->rate = 0;
    return 1;
}
static void t_gauge(void)
{
    sc_gauge_t ga;
    memset(&ga, 0, sizeof ga);
    int nr, nl;
    /* outside 1-2: the second human's own rate is swapped in; what he scans stays his; the first human's comes back */
    sc_gauge_begin(&ga, 0, 0xA0, 40, 0, &nr, &nl);
    CHECK(nr == 0 && nl == 0 && ga.on && ga.p1_rate == 40);
    sc_gauge_end(&ga, 0xA0, 25, 0, &nr, &nl);
    CHECK(nr == 40 && ga.p2_rate == 25 && !ga.on);
    sc_gauge_begin(&ga, 0, 0xA0, 55, 0, &nr, &nl);
    CHECK(nr == 25);
    sc_gauge_end(&ga, 0xA0, 100, 0, &nr, &nl);
    CHECK(nr == 55 && ga.p2_rate == 100);
    /* end without begin: unchanged */
    sc_gauge_end(&ga, 0xA0, 77, 1, &nr, &nl);
    CHECK(nr == 77 && nl == 1);
    /* 1-2: the team's rate is left alone, the latch is per player */
    memset(&ga, 0, sizeof ga);
    sc_gauge_begin(&ga, 1, 0xB0, 100, 1, &nr, &nl);   /* the first human already took his herb */
    CHECK(nr == 100 && nl == 0);
    sc_gauge_end(&ga, 0xB0, 100, 1, &nr, &nl);        /* the second took his */
    CHECK(nr == 100 && nl == 1 && ga.p2_latch == 1);
    sc_gauge_begin(&ga, 1, 0xB0, 100, 1, &nr, &nl);
    CHECK(nl == 1);                                    /* his latch is kept: no second herb */
    sc_gauge_end(&ga, 0xB0, 100, 1, &nr, &nl);
    /* a new GUI (a load): his latch starts at 0 */
    sc_gauge_begin(&ga, 1, 0xC0, 30, 0, &nr, &nl);
    CHECK(nl == 0);
    sc_gauge_end(&ga, 0xD0, 30, 1, &nr, &nl);         /* a load in between: both start again */
    CHECK(nl == 0 && ga.p2_latch == 0);

    /* a model of a split session outside 1-2: the two humans scan in turns (the second only inside his exchange), each scan adds 10..40, a close at 100
     * gives a herb to the scanner and resets HIS rate; the sum of herbs = what each one's own scans earned */
    memset(&ga, 0, sizeof ga);
    game_t g = {0, 0, 0, 0xE0};
    int own[2] = {0, 0}, herbs[2] = {0, 0}, expect_herbs[2] = {0, 0};
    for (int turn = 0; turn < 400; turn++) {
        int who = (int)(rnd() % 2);
        if (who == 1) {
            sc_gauge_begin(&ga, 0, g.gui, g.rate, g.latch, &nr, &nl);
            g.rate = nr, g.latch = nl;
        }
        CHECK(g.rate == own[who]);   /* the game shows the scanner's own rate */
        int scans = 1 + (int)(rnd() % 3);
        for (int s = 0; s < scans; s++) {
            int q = 10 + (int)(rnd() % 31);
            g.rate = sc_clamp_rate(g.rate + q);
            own[who] = sc_clamp_rate(own[who] + q);
        }
        if (stock_close(&g)) herbs[who]++;
        if (own[who] == 100) own[who] = 0, expect_herbs[who]++;
        if (who == 1) {
            sc_gauge_end(&ga, g.gui, g.rate, g.latch, &nr, &nl);
            g.rate = nr, g.latch = nl;
        }
        CHECK(g.rate == own[0] && ga.p2_rate == own[1]);
    }
    CHECK(herbs[0] == expect_herbs[0] && herbs[1] == expect_herbs[1] && herbs[0] > 10 && herbs[1] > 10);
    printf("R: split model: herbs %d / %d, rates now %d / %d\n", herbs[0], herbs[1], own[0], own[1]);

    /* 1-2 (story mode): one team rate, one herb each */
    memset(&ga, 0, sizeof ga);
    game_t s = {0, 0, 1, 0xF0};
    int h0 = 0, h1 = 0;
    for (int turn = 0; turn < 60; turn++) {
        int who = turn % 2;
        if (who) {
            sc_gauge_begin(&ga, 1, s.gui, s.rate, s.latch, &nr, &nl);
            s.rate = nr, s.latch = nl;
        }
        s.rate = sc_clamp_rate(s.rate + 18);
        if (stock_close(&s)) *(who ? &h1 : &h0) += 1;
        if (who) {
            sc_gauge_end(&ga, s.gui, s.rate, s.latch, &nr, &nl);
            s.rate = nr, s.latch = nl;
        }
    }
    CHECK(s.rate == 100 && h0 == 1 && h1 == 1);
}

/* the frame model of the exchange: two humans pick up and put down the Genesis at random; the first human uses it only while he is the own human (no
 * exchange), the second only through the exchange; the scanner GUI belongs to the own human. Never two users, every exchange ends, he gets his turns. */
static void t_x_model(void)
{
    sc_x_t x;
    memset(&x, 0, sizeof x);
    int p1_hold = 0, p2_hold = 0, p2_in_genesis = 0, gui_open = 0, gui_owner = -1, ended = 0;
    uint32_t now = 1000, last_end = 0;
    unsigned enters = 0, leaves = 0, p2_scans = 0, p1_scans = 0, both = 0, p2_waits = 0;
    for (int f = 0; f < 200000; f++, now += 16) {
        if (rnd() % 120 == 0) p1_hold = !p1_hold;
        if (rnd() % 100 == 0) p2_hold = !p2_hold;
        int p1_own = !x.on;
        /* the stock pipeline: the own human's hold opens the GUI */
        int p1_using = p1_own && p1_hold;
        int p2_using = x.on && p2_hold;
        p2_in_genesis = p2_hold;   /* his upper state follows his hold (outside the exchange: no overlay) */
        if (p1_using && p2_using) both++;
        if (gui_open && gui_owner == 0 && !p1_using) gui_open = 0;
        if (gui_open && gui_owner == 1 && !p2_using) gui_open = 0;
        if (!gui_open && (p1_using || p2_using)) gui_open = 1, gui_owner = p1_using ? 0 : 1, *(p1_using ? &p1_scans : &p2_scans) += 1;
        if (!x.on) {
            sc_enter_in in;
            memset(&in, 0, sizeof in);
            in.split_ok = 1, in.p2_present = 1, in.p2_has = 1, in.p2_allowed = 1, in.p2_hold = p2_hold, in.p1_busy = p1_hold || gui_open, in.now = now, in.last_end = last_end,
            in.ever_ended = ended;
            if (p2_hold && in.p1_busy) p2_waits++;
            if (sc_want_enter(&in)) sc_x_begin(&x, now), enters++;
        } else {
            sc_leave_in l;
            memset(&l, 0, sizeof l);
            l.p2_present = 1, l.p2_genesis = p2_in_genesis, l.gui_open = gui_open;
            if (sc_x_want_leave(&x, &l, now)) x.on = 0, last_end = now, ended = 1, leaves++;
        }
        CHECK(!(gui_open && gui_owner == 0 && x.on && p1_hold && 0));
    }
    CHECK(both == 0);
    CHECK(enters >= leaves && enters - leaves <= 1 && enters > 100 && p2_scans > 100 && p1_scans > 100);
    printf("X: %u exchanges (%u ended), scans P1 %u P2 %u, P2 waited %u frames for the scanner\n", enters, leaves, p1_scans, p2_scans, p2_waits);
}

/* ---------------------------------------------------------------- V */
static void t_wire(void)
{
    sc_msg m, d;
    memset(&m, 0, sizeof m);
    m.type = SC_M_REVEAL, m.epoch = 7, m.sect = 0x91010300u, m.cls = 0x80041500u, m.from_slot = 1;
    m.k[0] = 0x11223344u, m.k[1] = 0x55667788u, m.k[2] = 3, m.k[3] = 0xFFFFFFFFu;
    uint8_t b[64];
    unsigned n = sc_enc(b, sizeof b, &m);
    CHECK(n == SC_MSG_LEN && sc_dec(b, n, &d));
    CHECK(d.type == m.type && d.epoch == 7 && d.sect == m.sect && d.cls == m.cls && d.from_slot == 1 && !memcmp(d.k, m.k, 16));
    CHECK(sc_enc(b, SC_MSG_LEN - 1, &m) == 0);
    CHECK(!sc_dec(b, n - 1, &d));
    uint8_t c[SC_MSG_LEN];
    memcpy(c, b, n), c[0] = 3;
    CHECK(!sc_dec(c, n, &d));
    memcpy(c, b, n), c[2] = 2;
    CHECK(!sc_dec(c, n, &d));
    memcpy(c, b, n), c[28] = 0;
    CHECK(!sc_dec(c, n, &d));
    memcpy(c, b, n), c[29] = 0;
    CHECK(!sc_dec(c, n, &d));
    CHECK(SC_MSG_LEN <= 64);   /* fits a cp_event body (CP_EVENT_MAX) */
    CHECK(sc_item_class(0x80041203u) && sc_item_class(0x80041500u) && !sc_item_class(0x80031000u) && !sc_item_class(0x80051B30u));

    sc_seen_t s;
    memset(&s, 0, sizeof s);
    uint32_t k1[4] = {1, 2, 3, 4}, k2[4] = {1, 2, 3, 5};
    sc_seen_epoch(&s, 3);
    CHECK(sc_seen_add(&s, k1) == 1 && sc_seen_add(&s, k1) == 0 && sc_seen_add(&s, k2) == 1 && s.n == 2);
    sc_seen_epoch(&s, 3);
    CHECK(s.n == 2);
    sc_seen_epoch(&s, 4);   /* a new level instance: forgotten */
    CHECK(s.n == 0 && !sc_seen_has(&s, k1));
    for (unsigned i = 0; i < SC_SEEN_MAX + 10; i++) {
        uint32_t k[4] = {i, 0, 0, 0};
        sc_seen_add(&s, k);
    }
    CHECK(s.n == SC_SEEN_MAX);

    /* the receiver */
    memset(&s, 0, sizeof s);
    sc_seen_epoch(&s, 9);
    m.epoch = 9, m.sect = 0x91010300u;
    memcpy(m.k, k1, 16);
    CHECK(sc_rx_decide(&s, &m, 0, 9, 0x91010300u, 1, 0) == SC_RX_WAIT);
    CHECK(sc_rx_decide(&s, &m, 1, 9, 0x91010300u, 1, 0) == SC_RX_APPLY);
    CHECK(sc_rx_decide(&s, &m, 1, 10, 0x91010300u, 1, 0) == SC_RX_DROP_EPOCH);
    CHECK(sc_rx_decide(&s, &m, 1, 9, 0x91010200u, 1, 0) == SC_RX_DROP_SECT);
    CHECK(sc_rx_decide(&s, &m, 1, 9, 0x91010300u, 0, 0) == SC_RX_DROP_MISSING);
    CHECK(sc_rx_decide(&s, &m, 1, 9, 0x91010300u, 1, 1) == SC_RX_DROP_VISIBLE);
    sc_seen_add(&s, k1);
    CHECK(sc_rx_decide(&s, &m, 1, 9, 0x91010300u, 1, 0) == SC_RX_DROP_DUP);
}

/* two PCs on a reliable, in-order line with a delay; each PC has its copy of the level's hidden items; loads (both PCs, at slightly different times) make a
 * new level instance with fresh hidden items; scans happen on either PC. The glue's rule (scan1.c net_pass) is modelled: a local reveal is sent once per key
 * and instance, a received one waits while this PC loads or is behind, else applied when the item is there and hidden. */
#define NIT 8
typedef struct {
    int loading;
    uint8_t epoch;
    uint32_t sect;
    int visible[NIT], taken[NIT];
    sc_seen_t seen;
    unsigned applied, sent, dropped, dup;
} pc_t;
typedef struct { uint8_t b[SC_MSG_LEN]; uint32_t at, t0; } wire_t;
#define WQ 256
typedef struct { wire_t q[WQ]; int h, t; } line_t;
static void line_put(line_t *l, const uint8_t *b, uint32_t at)
{
    int n = (l->h + 1) % WQ;
    if (n == l->t) return;
    memcpy(l->q[l->h].b, b, SC_MSG_LEN), l->q[l->h].at = at, l->q[l->h].t0 = at;
    l->h = n;
}
static void local_scan(pc_t *p, line_t *out, int item, uint32_t now)
{
    if (p->loading || p->taken[item] || p->visible[item]) return;
    p->visible[item] = 1;   /* the stock reveal here */
    sc_seen_epoch(&p->seen, p->epoch);
    uint32_t k[4] = {0x500u + (uint32_t)item, 0x77u, (uint32_t)item, 1};
    if (!sc_seen_add(&p->seen, k)) {
        p->dup++;
        return;
    }
    sc_msg m;
    memset(&m, 0, sizeof m);
    m.type = SC_M_REVEAL, m.epoch = p->epoch, m.sect = p->sect, m.cls = 0x80041203u;
    memcpy(m.k, k, 16);
    uint8_t b[SC_MSG_LEN];
    sc_enc(b, sizeof b, &m);
    line_put(out, b, now + 30 + rnd() % 200);
    p->sent++;
}
static void deliver(pc_t *p, line_t *in, uint32_t now)
{
    int count = (in->h - in->t + WQ) % WQ;
    for (int i = 0; i < count; i++) {
        wire_t w = in->q[in->t];
        if (w.at > now) break;   /* in order: the head is not due yet */
        in->t = (in->t + 1) % WQ;
        sc_msg m;
        if (!sc_dec(w.b, SC_MSG_LEN, &m)) continue;
        sc_seen_epoch(&p->seen, p->epoch);
        int item = (int)m.k[2];
        int newer = (int8_t)(uint8_t)(m.epoch - p->epoch) > 0;
        int found = !p->loading && m.epoch == p->epoch && item >= 0 && item < NIT && !p->taken[item];
        int d = sc_rx_decide(&p->seen, &m, !p->loading && !newer, p->epoch, p->sect, found, found && p->visible[item]);
        if (d == SC_RX_WAIT) {
            if (now - w.t0 < 60000u) {
                int n = (in->h + 1) % WQ;
                in->q[in->h] = w, in->q[in->h].at = now + 16, in->h = n;   /* back to the tail */
            }
            continue;
        }
        sc_seen_add(&p->seen, m.k);
        if (d == SC_RX_APPLY) p->visible[item] = 1, p->applied++;
        else p->dropped++;
    }
}
static void new_level(pc_t *p, uint8_t epoch, uint32_t sect)
{
    p->epoch = epoch, p->sect = sect, p->loading = 0;
    memset(p->visible, 0, sizeof p->visible), memset(p->taken, 0, sizeof p->taken);
}
static void t_net(void)
{
    pc_t A, B;
    memset(&A, 0, sizeof A), memset(&B, 0, sizeof B);
    line_t ab, ba;
    memset(&ab, 0, sizeof ab), memset(&ba, 0, sizeof ba);
    new_level(&A, 1, 0x91010300u), new_level(&B, 1, 0x91010300u);
    uint32_t now = 0;
    unsigned levels = 0, consistent = 0;
    uint8_t ep = 1;
    for (int f = 0; f < 120000; f++, now += 16) {
        /* a load every ~20 s: A first, B up to 3 s later (the guest follows) */
        if (f % 1250 == 0 && f) {
            /* before the load: everything both PCs revealed is visible on both (the line had time: no scan in the last 2 s) */
            int same = 1;
            for (int i = 0; i < NIT; i++) same &= A.visible[i] == B.visible[i];
            levels++, consistent += same;
            ep++;
            A.loading = 1, B.loading = 1;
        }
        if (A.loading && f % 1250 == 40) new_level(&A, ep, (ep & 1) ? 0x91010300u : 0x91020100u);
        if (B.loading && f % 1250 == 40 + (int)(ep % 4) * 50) new_level(&B, ep, (ep & 1) ? 0x91010300u : 0x91020100u);
        int phase = f % 1250;
        if (phase > 300 && phase < 1100) {   /* scans in the middle of a level */
            if (rnd() % 90 == 0) local_scan(&A, &ab, (int)(rnd() % NIT), now);
            if (rnd() % 90 == 0) local_scan(&B, &ba, (int)(rnd() % NIT), now);
        }
        deliver(&A, &ba, now), deliver(&B, &ab, now);
    }
    CHECK(levels > 50 && consistent == levels);
    CHECK(A.applied + B.applied > 50 && A.applied <= B.sent && B.applied <= A.sent);
    printf("V: %u levels, all consistent %d; sent %u / %u, applied %u / %u, dropped %u / %u, local dup %u / %u\n", levels, consistent == levels, A.sent, B.sent, A.applied, B.applied,
           A.dropped, B.dropped, A.dup, B.dup);
    /* a reveal from the partner who is a level ahead waits until this PC is there, then applies to the NEW level's item */
    pc_t P;
    memset(&P, 0, sizeof P);
    line_t l;
    memset(&l, 0, sizeof l);
    new_level(&P, 5, 0x91010300u);
    sc_msg m;
    memset(&m, 0, sizeof m);
    m.type = SC_M_REVEAL, m.epoch = 6, m.sect = 0x91010300u, m.cls = 0x80041203u, m.k[0] = 0x502, m.k[1] = 0x77, m.k[2] = 2, m.k[3] = 1;
    uint8_t b[SC_MSG_LEN];
    sc_enc(b, sizeof b, &m);
    line_put(&l, b, 0);
    deliver(&P, &l, 100);
    CHECK(!P.visible[2] && P.applied == 0 && l.h != l.t);   /* waits */
    P.loading = 1;
    deliver(&P, &l, 200);
    CHECK(P.applied == 0);
    new_level(&P, 6, 0x91010300u);
    deliver(&P, &l, 300);
    CHECK(P.visible[2] && P.applied == 1);
    /* the same key again (the partner scanned it twice / both scanned): dropped */
    line_put(&l, b, 300);
    deliver(&P, &l, 400);
    CHECK(P.applied == 1 && P.dropped == 1);
    /* an old instance's reveal is dropped */
    m.epoch = 5;
    sc_enc(b, sizeof b, &m);
    line_put(&l, b, 400);
    deliver(&P, &l, 500);
    CHECK(P.applied == 1 && P.dropped == 2);
}

/* HAVE (sc1n1 live: in 1-2 the guest's PC had no Genesis on any human, the host's PC on both): each PC tells the other once per level instance; the
 * receiver's rule G counts it as a source for that instance. A model of two PCs: per level the story gives the Genesis on ONE PC only (the host's, or in the
 * R7 role the guest's), loads at slightly different times; after the line delivered HAVE every human on both PCs has one. */
static void t_have(void)
{
    sc_have_t h;
    memset(&h, 0, sizeof h);
    CHECK(sc_have_should_send(&h, 3, 1, 1) == 1 && sc_have_should_send(&h, 3, 0, 1) == 0 && sc_have_should_send(&h, 3, 1, 0) == 0);
    sc_have_sent(&h, 3);
    CHECK(sc_have_should_send(&h, 3, 1, 1) == 0 && sc_have_should_send(&h, 4, 1, 1) == 1);
    CHECK(sc_have_source(&h, 3) == 0);
    sc_have_remote(&h, 3);
    CHECK(sc_have_source(&h, 3) == 1 && sc_have_source(&h, 4) == 0);
    sc_have_reset(&h);
    CHECK(sc_have_source(&h, 3) == 0 && sc_have_should_send(&h, 3, 1, 1) == 1);
    /* the wire carries it */
    sc_msg m, d;
    memset(&m, 0, sizeof m);
    m.type = SC_M_HAVE, m.epoch = 9, m.sect = 0x91010200u, m.cls = SC_CLASS_SCANNER;
    uint8_t b[SC_MSG_LEN];
    CHECK(sc_enc(b, sizeof b, &m) == SC_MSG_LEN && sc_dec(b, SC_MSG_LEN, &d) && d.type == SC_M_HAVE && d.epoch == 9);
    b[0] = 3;
    CHECK(!sc_dec(b, SC_MSG_LEN, &d));
    /* rule G with a remote source: no local source, the own human and the puppet get one */
    sc_give_t g;
    sc_give_init(&g);
    sc_human hh[2] = {hum(1, 1, 0, 1, 0x10), hum(1, 1, 0, 1, 0x20)};
    int both[2] = {1, 1};
    CHECK(sc_give_targets_src(&g, hh, both, 0) == 0 && sc_give_targets_src(&g, hh, both, 1) == 3u);

    /* the two-PC model */
    struct { sc_have_t hv; sc_give_t g; int has[2]; uint32_t bag[2]; uint8_t epoch; int loading; } P[2];
    memset(P, 0, sizeof P);
    for (int k = 0; k < 2; k++) sc_give_init(&P[k].g);
    struct { int to; uint8_t epoch; uint32_t at; } q[64];
    int nq = 0;
    unsigned levels = 0, ok_levels = 0;
    uint8_t ep = 1;
    int story_pc = 0, guest_delay = 0;
    for (int f = 0; f < 60000; f++) {
        uint32_t now = (uint32_t)f * 16u;
        if (f % 1000 == 0) {   /* a load: the host first, the guest up to 40 frames later; the story gives the Genesis on one PC (both of its humans) */
            if (f) {
                levels++;
                ok_levels += P[0].has[0] && P[0].has[1] && P[1].has[0] && P[1].has[1];
            }
            ep++;
            story_pc = (int)(rnd() % 2), guest_delay = (int)(rnd() % 40);
            for (int k = 0; k < 2; k++) P[k].loading = 1;
        }
        for (int k = 0; k < 2; k++)
            if (P[k].loading && f % 1000 == 5 + k * guest_delay) {
                P[k].loading = 0, P[k].epoch = ep;
                P[k].bag[0] += 0x100, P[k].bag[1] += 0x100;
                P[k].has[0] = P[k].has[1] = k == story_pc;
            }
        /* the line */
        for (int i = 0; i < nq; i++)
            if (q[i].at <= now) {
                int to = q[i].to;
                int8_t age = (int8_t)(uint8_t)(q[i].epoch - P[to].epoch);
                if (age == 0 && !P[to].loading) sc_have_remote(&P[to].hv, q[i].epoch);
                else if (age > 0 || P[to].loading) { q[i].at = now + 16; continue; }   /* ahead: kept */
                q[i] = q[--nq], i--;
            }
        for (int k = 0; k < 2; k++) {
            if (P[k].loading || f % 30) continue;   /* the glue's 500 ms poll */
            sc_human hk[2];
            for (int i = 0; i < 2; i++) hk[i] = hum(1, 1, P[k].has[i], !P[k].has[i], P[k].bag[i]);
            int local = P[k].has[0] || P[k].has[1];
            if (sc_have_should_send(&P[k].hv, P[k].epoch, local, 1) && nq < 64) {
                q[nq].to = 1 - k, q[nq].epoch = P[k].epoch, q[nq].at = now + 30 + rnd() % 300, nq++;
                sc_have_sent(&P[k].hv, P[k].epoch);
            }
            unsigned t = sc_give_targets_src(&P[k].g, hk, both, sc_have_source(&P[k].hv, P[k].epoch));
            for (int i = 0; i < 2; i++)
                if (t & (1u << i)) P[k].has[i] = 1, sc_give_result(&P[k].g, i, P[k].bag[i], 1);
        }
    }
    CHECK(levels == 59 && ok_levels == levels);
    printf("HAVE: %u levels (the story gave the Genesis on one PC each), every human on both PCs armed in %u\n", levels, ok_levels);
}

/* ---------------------------------------------------------------- the image and the glue's sources */
static unsigned char *img;
static long img_n;
static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) {
        fclose(f), free(s);
        return NULL;
    }
    fclose(f);
    s[n] = 0;
    if (len) *len = n;
    return s;
}
static uint32_t i32(uint32_t va)
{
    long o = (long)va - 0x400000L;
    if (!img || o < 0 || o + 4 > img_n) return 0xFFFFFFFFu;
    return (uint32_t)img[o] | (uint32_t)img[o + 1] << 8 | (uint32_t)img[o + 2] << 16 | (uint32_t)img[o + 3] << 24;
}
static void t_image(const char *glue)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    char *d = slurp(path, &img_n);
    if (!d) {
        printf("(no executable image at %s: the build facts of scan1 are not compared)\n", path);
        return;
    }
    img = (unsigned char *)d;
    /* the vtables and the slots the RE relies on */
    CHECK(i32(0x106AD20u + 0x14u) == 0x6DE990u);   /* cGenesisInvisibleItemParam slot 5 = the hidden item's scan result */
    CHECK(i32(0x106AD08u + 0x14u) == 0x6DE8A0u && i32(0x106AD38u + 0x14u) == 0x6DFD20u);   /* enemy / hand print params */
    CHECK(i32(0x1064F54u) == 0x699370u && i32(0x1064F54u + 0x14u) == 0x699E40u);   /* Upper::GenesisAimState: dtor, enter */
    CHECK(i32(0x1020CB8u) == 0x5DEDE0u);   /* uWpScanner */
    CHECK(i32(0x1021B80u + 34u * 4u) == 0x5E9AA0u);   /* uGenesisSight_vf34: the own-slot gate */
    CHECK(i32(0x1080A00u + 9u * 4u) == 0x7ABDC0u);    /* uGUI_GadgetScanner_vf9 */
    CHECK(i32(0x10186B0u) != 0xFFFFFFFFu);
    /* the facts behind the rules, by bytes */
    CHECK(img[0x7A37ADu - 0x400000u] == 0x66 && i32(0x7A37ADu + 3u) == 0x320u && img[0x7A37ADu + 7u - 0x400000u] == 1);   /* mov word [esi+0x320], 1: story mode */
    CHECK(i32(0x7A37A4u + 3u) == 0x92010200u);   /* ... when the scene is 1-2 */
    CHECK(img[0x6DE997u - 0x400000u] == 0x8B && img[0x6DE998u - 0x400000u] == 0x49 && img[0x6DE999u - 0x400000u] == 0x08);   /* ecx = [param+8] = the item */
    CHECK(i32(0x5DAAB6u) == 0xF19u);   /* item+0xF19 = on */
    /* the place-status guard (sc1s3): both call sites of FUN_006BEF50 are `call 0x49CD20` after `push 5`, ChangeState's array is mgr+0x30 (ptr) / +0x34 (count) */
    static const uint32_t sites[2] = {0x6BF0DBu, 0x6BF16Cu};
    for (int i = 0; i < 2; i++) {
        long o = (long)sites[i] - 0x400000L;
        CHECK(img[o] == 0xE8 && sites[i] + 5u + i32(sites[i] + 1u) == 0x49CD20u && img[o - 2] == 0x6A && img[o - 1] == 0x05);
    }
    CHECK(img[0x49CD2Fu - 0x400000u] == 0x83 && img[0x49CD31u - 0x400000u] == 0x30 && img[0x49CD33u - 0x400000u] == 0x8D && img[0x49CD35u - 0x400000u] == 0x30);   /* cmp [esi+0x30],0; lea ecx,[esi+0x30] */
    CHECK(img[0x49BF19u - 0x400000u] == 0x8B && img[0x49BF1Au - 0x400000u] == 0x46 && img[0x49BF1Bu - 0x400000u] == 0x04);   /* the count at array+4 */
    CHECK(img[0x49CD2Cu - 0x400000u] == 0xC2 && img[0x49CD2Du - 0x400000u] == 0x0C);   /* ret 0xC */
    /* every SIG entry of scan1.c: its 12 bytes at its address */
    int nsig = 0;
    const char *p = glue ? strstr(glue, "} SIG[] = {") : NULL;
    const char *end = p ? strstr(p, "};") : NULL;
    while (p && end && (p = strstr(p, "\n    {SCA_")) && p < end) {
        char name[40];
        unsigned b[12];
        if (sscanf(p, "\n    {%39[A-Z0-9_], {0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x}", name, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &b[8],
                   &b[9], &b[10], &b[11]) == 13) {
            char key[64];
            snprintf(key, sizeof key, "#define %s ", name);
            const char *def = strstr(glue, key);
            unsigned va = 0;
            CHECK(def && sscanf(def + strlen(key), " 0x%x", &va) == 1);
            int same = 1;
            for (int k = 0; k < 12; k++) same &= va >= 0x400000u && va - 0x400000u + 12u <= (unsigned)img_n && img[va - 0x400000u + (unsigned)k] == b[k];
            if (!same) printf("FAIL: SIG %s at %08x differs from the image\n", name, va);
            CHECK(same);
            nsig++;
        }
        p++;
    }
    CHECK(nsig == 4);
    /* the stack words every called stock function pops: the glue's thiscall typedefs push exactly that many (traps.md: ret N / force_align) */
    static const struct { uint32_t fn, ret_at; unsigned bytes; const char *typedef_text; } RETS[] = {
        {0x772500u, 0x7725BEu, 12, "typedef unsigned (TC *fsm4_fn)(void *self, void *param, void *x, void *unit);"},
        {0x5DAAB0u, 0x5DAB54u, 4, "typedef void (TC *reveal_fn)(void *item, int on);"},
        {0x6DE990u, 0x6DE9A1u, 0, "typedef void (TC *param5_fn)(void *param);"},
        {0x720320u, 0x720350u, 0, "typedef uint32_t (TC *sect_fn)(void *episode);"},
    };
    for (unsigned i = 0; i < sizeof RETS / sizeof RETS[0]; i++) {
        long o = (long)RETS[i].ret_at - 0x400000L;
        int ok = o >= 0 && o + 3 <= img_n && (RETS[i].bytes ? img[o] == 0xC2 && img[o + 1] == RETS[i].bytes && img[o + 2] == 0 : img[o] == 0xC3);
        if (!ok) printf("FAIL: the ret of %08x at %08x is not %u bytes (%02x %02x %02x)\n", (unsigned)RETS[i].fn, (unsigned)RETS[i].ret_at, RETS[i].bytes, img[o], img[o + 1], img[o + 2]);
        CHECK(ok);
        CHECK(glue && strstr(glue, RETS[i].typedef_text) != NULL);
    }
    printf("image: %d stock signatures of scan1.c match, vtable slots and the story-mode / reveal facts checked\n", nsig);
    free(d), img = NULL;
}
static int count_of(const char *s, const char *w)
{
    int n = 0;
    for (const char *p = s; p && (p = strstr(p, w)); p += strlen(w)) n++;
    return n;
}
static void t_glue(void)
{
    char *glue = slurp("../native/scan1.c", NULL), *probe = slurp("../native/probe.c", NULL), *build = slurp("../native/build.sh", NULL), *tbuild = slurp("build.sh", NULL);
    char *split = slurp("../native/split.c", NULL), *splith = slurp("../native/split.h", NULL), *story = slurp("../native/story1_game.inc", NULL);
    CHECK(glue && probe && build && tbuild && split && splith && story);
    if (glue && probe && build && tbuild && split && splith && story) {
        t_image(glue);
        CHECK(strstr(probe, "scan1_init();") && strstr(probe, "\"scan\", \"give\", NULL") && strstr(probe, "scan_cmd(arg, out, size);") && strstr(probe, "#include \"scan1_game.h\""));
        CHECK(strstr(build, " give1.c scan1.c ") != NULL && strstr(tbuild, "test_scan1") != NULL);
        /* every entry the game (or lt.c) calls realigns the stack (g1n1: a 4-byte aligned stack + SSE = #GP) */
        CHECK(strstr(glue, "static void TC ALIGNED reveal_hook(void *param)") && strstr(glue, "static void ALIGNED scan_frame(void)") && strstr(glue, "int ALIGNED scan1_split_hook(int op)") &&
              strstr(glue, "int ALIGNED scan1_rate_shared_now(void)") && strstr(glue, "void ALIGNED scan_cmd(const char *arg, char *out, int size)"));
        /* the reveal hook calls the stock result first, sends nothing outside a session, and the receiver applies through the stock code without the hook */
        const char *hk = strstr(glue, "static void TC ALIGNED reveal_hook(void *param)");
        CHECK(hk && strstr(hk, "((param5_fn)(uintptr_t)SCA_PARAM_VF5)(param);") && strstr(hk, "if (!g_on || !net1_session_active()) return;") >
              strstr(hk, "((param5_fn)(uintptr_t)SCA_PARAM_VF5)(param);"));
        CHECK(strstr(glue, "((param5_fn)(uintptr_t)SCA_PARAM_VF5)((void *)(uintptr_t)p);   /* the stock result itself (no hook: no echo) */") != NULL);
        /* the vtable slot is only written when it holds the stock function (or ours) */
        CHECK(strstr(glue, "if (cur != SCA_PARAM_VF5) {") && strstr(glue, "wr_code32(SCA_PARAM_SLOT5, mine)"));
        /* G writes nothing but the stock AddWeapon (no bag word) */
        CHECK(count_of(glue, "wr32(b + ") == 0 && count_of(glue, "SCA_ADDWEAPON)(NULL, param, NULL, (void *)(uintptr_t)unit)") == 1);
        /* split.c: kind 3 is entered / left only through the hook; BEGIN after the exchange is on, END after it is off (also from a teardown) */
        CHECK(strstr(split, "else if (xg_hook && xg_hook(SPLIT_XG_WANT_ENTER))\r\n                menu_enter(3);") || strstr(split, "else if (xg_hook && xg_hook(SPLIT_XG_WANT_ENTER))\n                menu_enter(3);"));
        CHECK(strstr(split, "if (!xg_hook || xg_hook(SPLIT_XG_WANT_LEAVE))") && strstr(split, "xg_hook(SPLIT_XG_BEGIN);") && strstr(split, "xg_hook(SPLIT_XG_END);"));
        const char *ml = strstr(split, "static void menu_leave(const char *why)");
        CHECK(ml && strstr(ml, "mx.on = 0;") < strstr(ml, "xg_hook(SPLIT_XG_END);"));
        const char *me = strstr(split, "static void menu_enter(int kind)");
        CHECK(me && strstr(me, "mx.on = 1, mx.kind = kind") < strstr(me, "xg_hook(SPLIT_XG_BEGIN);"));
        CHECK(strstr(splith, "void split_set_genesis_hook(int (*hook)(int op));") && strstr(splith, "SPLIT_XG_WANT_ENTER = 0, SPLIT_XG_BEGIN = 1, SPLIT_XG_WANT_LEAVE = 2, SPLIT_XG_END = 3"));
        CHECK(strstr(glue, "split_set_genesis_hook(scan1_split_hook);") != NULL);
        CHECK(strstr(glue, "static unsigned TC ALIGNED chg_guard(void *mgr, unsigned id, unsigned a, unsigned b)") && strstr(glue, "typedef unsigned (TC *chg_fn)(void *mgr, unsigned id, unsigned a, unsigned b);"));
        CHECK(strstr(glue, "if (now != SCA_CHGSTATE && now != (uint32_t)(uintptr_t)chg_guard) return 0;") != NULL);   /* a foreign call there is left alone */
        /* story1_game.inc: E2 sends, applies and digests the rate only where scan1 says it is shared; the poll always runs (the baseline follows) */
        CHECK(count_of(story, "scan1_rate_shared_now()") == 3);
        CHECK(strstr(story, "if (cur >= 0 && sf1_rate_poll(&SF, cur, &kind, &v) && scan1_rate_shared_now()) story_send_rate(kind, v);") != NULL);
        CHECK(strstr(story, "if (!scan1_rate_shared_now()) return;   /* scan1: outside 1-2 each player has his own rate */") != NULL);
        CHECK(strstr(story, "if (rv >= 0 && scan1_rate_shared_now()) story_send_rate(SF1_RATE_ABS, rv);") != NULL);
    }
    free(glue), free(probe), free(build), free(tbuild), free(split), free(splith), free(story);
}
static void t_kind_free(void)
{
    static const char *files[] = {"../native/rolebarrier1.h", "../native/downed1_net.h", "../native/coop_proto.h", "../native/world1.h", "../native/coopsave1.h", "../native/give1.h",
                                  "../native/hunk1.h", NULL};
    for (int i = 0; files[i]; i++) {
        char *s = slurp(files[i], NULL);
        if (!s) continue;
        CHECK(!strstr(s, "KIND 0x145") && !strstr(s, "USER + 0x45"));
        free(s);
    }
    CHECK(SC_EV_KIND == 0x100u + 0x45u);
}

int main(void)
{
    t_give();
    t_rate_shared();
    t_enter_leave();
    t_gauge();
    t_x_model();
    t_wire();
    t_net();
    t_have();
    t_glue();
    t_kind_free();
    printf("test_scan1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
