/*
 * Offline test of native/grab1.h (ep1-solo E11) with the real puppet sender / receiver (puppet1.c): the host's human is bitten by an Ooze and waggles the
 * stick to break free (the QTE reads the host's raw pad); the client's puppet of him must stand (no waggle replay, no position correction) while the grab
 * lasts and run again afterwards. The model before the fix (the sender never sets CP_SF_HELD) must fail: the waggle reaches the client's puppet.
 *   sh tests/build.sh   ->  tests/build/test_grab1.exe
 */
#include <stdio.h>
#include <string.h>

#include "../native/coop_proto.h"
#include "../native/puppet1.h"
#include "../native/grab1.h"

static int fails, checks, quiet, *cur = &fails;
#define CHECK(c) do { checks++; if (!(c)) { (*cur)++; if (!quiet) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { unsigned pushes, moving; } sinkc;
static int sink_push(void *u, const cp_pad_rec *r, uint32_t seq, uint32_t ts)
{
    (void)seq, (void)ts;
    sinkc *s = u;
    s->pushes++;
    if (r->lx || r->ly) s->moving++;
    return 1;
}
static unsigned tps;
static int u_get(void *u, float pos[3], float *yaw) { (void)u; pos[0] = 100, pos[1] = 0, pos[2] = 100; *yaw = 0; return 1; }
static int u_tp(void *u, const float pos[3], float yaw) { (void)u, (void)pos, (void)yaw; tps++; return 1; }

static void rules(void)
{
    gr1_t g;
    memset(&g, 0, sizeof g);
    CHECK(gr1_step(&g, 0, 1, 0, GR1_CAP_MS) == 0);                 /* no unit */
    CHECK(gr1_step(&g, 0x100, 0, 10, GR1_CAP_MS) == 0);            /* free */
    CHECK(gr1_step(&g, 0x100, 1, 20, GR1_CAP_MS) == 1 && g.grabs == 1);
    CHECK(gr1_step(&g, 0x100, 1, 3000, GR1_CAP_MS) == 1);
    CHECK(gr1_step(&g, 0x100, 0, 3100, GR1_CAP_MS) == 0 && !g.held);
    CHECK(gr1_step(&g, 0x100, 1, 4000, GR1_CAP_MS) == 1 && g.grabs == 2);
    /* a field that never clears: dropped after the cap, not believed again until it clears */
    CHECK(gr1_step(&g, 0x100, 1, 4000 + GR1_CAP_MS + 1, GR1_CAP_MS) == 0 && g.caps == 1 && g.capped);
    CHECK(gr1_step(&g, 0x100, 1, 4000 + GR1_CAP_MS + 500, GR1_CAP_MS) == 0);
    CHECK(gr1_step(&g, 0x100, 0, 4000 + GR1_CAP_MS + 600, GR1_CAP_MS) == 0 && !g.capped);
    CHECK(gr1_step(&g, 0x100, 1, 4000 + GR1_CAP_MS + 700, GR1_CAP_MS) == 1);
    /* a new unit (a new level) starts over */
    CHECK(gr1_step(&g, 0x200, 0, 30000, GR1_CAP_MS) == 0 && !g.held && g.unit == 0x200);
    CHECK((P1_HOLD_MASK & CP_SF_HELD) != 0);
}

/* the host's human: walks 2 s, is bitten for 4 s (waggles the stick, +0xE34 = 1), walks again 2 s. Returns the client's counters for the grab window. */
static void run(int fix, unsigned *moving_in_grab, unsigned *tp_in_grab, unsigned *moving_after)
{
    p1_tx tx;
    p1_tx_init(&tx, NULL);
    sinkc sk = {0, 0};
    p1_rx rx;
    p1_rx_init(&rx, NULL, (p1_sink){sink_push, &sk});
    gr1_t g;
    memset(&g, 0, sizeof g);
    p1_unit_ops ops = {NULL, u_get, u_tp};
    p1_corr_cfg cc;
    p1_corr_defaults(&cc);
    p1_corr_result res;
    unsigned mv0 = 0, tp0 = 0, mv1 = 0;
    tps = 0;
    for (uint32_t t = 1000; t < 9000; t += 16) {
        int grab = t >= 3000 && t < 7000;
        cp_pad_rec cur;
        memset(&cur, 0, sizeof cur);
        if (grab) cur.lx = ((t / 64) & 1) ? 30000 : -30000;   /* the break-free waggle */
        else cur.ly = 20000;                                   /* walking */
        uint16_t flags = 0;
        if (fix && gr1_step(&g, 0x500, grab ? 1u : 0u, t, GR1_CAP_MS)) flags |= CP_SF_HELD;
        cp_pad pk;
        if (p1_tx_step(&tx, t, &cur, (uint16_t)(flags & CP_PAD_FLAGS_MASK), &pk)) {
            pk.pid = 0, pk.epoch = 1;
            p1_rx_packet(&rx, t + 30, &pk, 1);
        }
        /* the host's STATE every 50 ms: its human stands at (400, 0, 100) during the bite (far from the client's puppet: a correction would teleport) */
        if (t % 48 == 0) {
            float p[3] = {400, 0, 100};
            p1_rx_state(&rx, t + 30, 1, p, 0, flags, t, 1);
        }
        unsigned before_mv = sk.moving, before_tp = tps;
        p1_rx_tick(&rx, t + 30, 1, 0);
        p1_correct(&rx, t + 30, &cc, &ops, &res);
        if (t >= 3100 && t < 7000) mv0 += sk.moving - before_mv, tp0 += tps - before_tp;
        if (t >= 7300) mv1 += sk.moving - before_mv;
    }
    *moving_in_grab = mv0, *tp_in_grab = tp0, *moving_after = mv1;
}

int main(void)
{
    rules();
    unsigned mv, tp, after;
    run(1, &mv, &tp, &after);
    printf("  fix: moving records pushed during the bite %u, teleports %u; moving records after it %u\n", mv, tp, after);
    CHECK(mv == 0 && tp == 0);   /* the client's puppet stands during the host's bite */
    CHECK(after > 20);           /* and walks again afterwards */
    /* the model before the fix: the sender never says HELD */
    int fake = 0;
    cur = &fake, quiet = 1;
    unsigned bmv, btp, bafter;
    run(0, &bmv, &btp, &bafter);
    CHECK(bmv == 0 && btp == 0);
    cur = &fails, quiet = 0;
    printf("  the model before the fix: moving records pushed during the bite %u, teleports %u (fails %d check)\n", bmv, btp, fake);
    CHECK(fake == 1 && bmv > 50);
    printf("test_grab1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
