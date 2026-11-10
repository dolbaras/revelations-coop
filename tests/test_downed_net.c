/*
 * Offline replay of the Revelations 1 NETWORK downed rule (native/downed1_net.h on top of native/downed_logic.h): two PCs (host = slot 0, client = slot 1), one machine each,
 * events over a simulated line with latency (and loss where a check needs it). No game, no Windows.
 *   sh tests/build.sh   ->  tests/build/test_downed_net.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/downed1_net.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s: ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define T0 2000000u
#define LAT 40u          /* one-way latency of the simulated line, ms */
#define PASS 16u         /* one logic pass */

/* ---- two PCs and a line ---- */
typedef struct { uint32_t at; int to; unsigned len; uint8_t b[D1N_MSG_LEN]; } wire_t;
static wire_t W[512];
static int nw;
static int drop_to[2];   /* drop everything that goes to this PC (a cut link) */
static d1n_t PC[2];
/* what each PC's glue was asked to do, accumulated per run */
static int seen[2][D1N_A_GAME_OVER + 1][2];
static float own_hp[2];
static int own_dead[2], pup_dead[2], own_lying[2], pup_lying[2];

static void reset_seen(void) { memset(seen, 0, sizeof seen); }

static void apply(int pc, const d1n_action *a, uint32_t now)
{
    if (a->act < 0 || a->act > D1N_A_GAME_OVER) return;
    if (a->act == D1N_A_SEND) {
        if (nw < 512 && !drop_to[1 - pc]) {
            W[nw].at = now + LAT, W[nw].to = 1 - pc, W[nw].len = (unsigned)a->len;
            memcpy(W[nw].b, a->msg, (size_t)a->len);
            nw++;
        }
        seen[pc][a->act][0]++;
        return;
    }
    int w = a->who == pc ? 0 : 1;   /* 0 = own human, 1 = the puppet of the partner */
    seen[pc][a->act][w]++;
    switch (a->act) {
    case D1N_A_GO_DOWN: own_hp[pc] = 1.0f, own_lying[pc] = 1; break;
    case D1N_A_DIE: own_hp[pc] = 0.0f, own_dead[pc] = 1, own_lying[pc] = 0; break;
    case D1N_A_GET_UP: own_hp[pc] = 350.0f, own_lying[pc] = 0; break;
    case D1N_A_PUP_DOWN: pup_lying[pc] = 1; break;
    case D1N_A_PUP_UP: pup_lying[pc] = 0; break;
    case D1N_A_PUP_DIE: pup_lying[pc] = 0, pup_dead[pc] = 1; break;
    default: break;
    }
}

static void deliver(uint32_t now)
{
    int k = 0;
    for (int i = 0; i < nw; i++) {
        if (W[i].at <= now) {
            d1n_recv(&PC[W[i].to], W[i].b, W[i].len, now);
        } else {
            W[k++] = W[i];
        }
    }
    nw = k;
}

/* one pass of both PCs. holding[pc] / range[pc]: the own human's revive button and distance on that PC */
static int holding[2], range[2], flags_on;
static void pass(uint32_t now)
{
    deliver(now);
    if (flags_on)   /* the partner's pad / STATE flags (CP_SF_DOWN while his own machine says DOWN), as downed1.c feeds them every pass */
        for (int pc = 0; pc < 2; pc++)
            if (!drop_to[pc]) d1n_sample(&PC[pc], d1n_own_down(&PC[1 - pc]), now);
    for (int pc = 0; pc < 2; pc++) {
        if (own_hp[pc] <= 0.0f && dl_state(&PC[pc].d, pc) != DL_DEAD) d1n_dead(&PC[pc], now);   /* the glue: the engine killed the own human */
        d1n_hold(&PC[pc], holding[pc], range[pc], now);
        d1n_action a[32];
        int n = d1n_step(&PC[pc], now, a, 32);
        for (int i = 0; i < n; i++) apply(pc, &a[i], now);
    }
}
static uint32_t run(uint32_t t, uint32_t ms)
{
    for (uint32_t e = 0; e < ms; e += PASS) pass(t + e);
    return t + ms;
}
static void start(uint32_t t, unsigned flags, unsigned epoch)
{
    nw = 0, drop_to[0] = drop_to[1] = 0;
    for (int pc = 0; pc < 2; pc++) {
        d1n_init(&PC[pc], flags, pc, t);
        d1n_epoch(&PC[pc], epoch, t);
        d1n_present(&PC[pc], 1, t);
        own_hp[pc] = 1000.0f, own_dead[pc] = pup_dead[pc] = own_lying[pc] = pup_lying[pc] = 0;
        holding[pc] = range[pc] = 0;
    }
    reset_seen();
}
/* a lethal hit on PC pc's own human, as the Damage hook sees it */
static float lethal(int pc, uint32_t t)
{
    float d = d1n_absorb(&PC[pc], own_hp[pc], own_hp[pc] + 500.0f, t);
    own_hp[pc] -= d;
    if (own_hp[pc] < 0.0f) own_hp[pc] = 0.0f;
    return d;
}

int main(void)
{
    uint32_t t = T0;

    /* 1. the host's human goes down: hp 1 on the host, his puppet lies on the client */
    start(t, CP_SESS_DOWNED, 7);
    CHECK(d1n_absorb(&PC[0], 1000.0f, 200.0f, t) == 200.0f);   /* not lethal: unchanged */
    float d = lethal(0, t);
    CHECKF(d == 999.0f && own_hp[0] == 1.0f, "absorbed to hp 1 (dmg %.1f hp %.1f)", d, own_hp[0]);
    t = run(t, 200);
    CHECK(seen[0][D1N_A_GO_DOWN][0] == 1 && own_lying[0]);
    CHECK(seen[1][D1N_A_PUP_DOWN][1] == 1 && pup_lying[1]);
    CHECK(d1n_own_down(&PC[0]) && d1n_partner_down(&PC[1]));
    CHECK(!d1n_own_down(&PC[1]));
    CHECK(PC[0].tx >= 1 && PC[1].rx >= 1);
    /* a second lethal hit while down: absorbed completely (stays at 1) */
    CHECK(d1n_absorb(&PC[0], 1.0f, 300.0f, t) == 0.0f);

    /* 2. the client's human revives him: holds A in range for 3 s on the CLIENT; the host runs the get-up */
    holding[1] = 1, range[1] = 1;
    t = run(t, 1500);
    CHECK(seen[0][D1N_A_REVIVE_PLAY][0] == 1);   /* the host learnt that somebody holds the button on him */
    CHECK(dl_being_revived(&PC[0].d, 0));
    t = run(t, 2000);
    holding[1] = 0;
    t = run(t, 200);
    CHECK(seen[0][D1N_A_GET_UP][0] == 1 && !own_lying[0] && own_hp[0] > 1.0f);
    CHECK(seen[1][D1N_A_PUP_UP][1] == 1 && !pup_lying[1]);
    CHECK(!d1n_own_down(&PC[0]) && !d1n_partner_down(&PC[1]));
    CHECK(seen[0][D1N_A_DIE][0] == 0 && seen[1][D1N_A_DIE][0] == 0);
    CHECK(!PC[0].d.over && !PC[1].d.over);

    /* 3. the other way round: the client goes down, the HOST revives him */
    reset_seen();
    own_hp[1] = 1000.0f;
    lethal(1, t);
    t = run(t, 200);
    CHECK(seen[1][D1N_A_GO_DOWN][0] == 1 && seen[0][D1N_A_PUP_DOWN][1] == 1);
    holding[0] = 1, range[0] = 1;
    t = run(t, 3400);
    holding[0] = 0;
    t = run(t, 200);
    CHECK(seen[1][D1N_A_GET_UP][0] == 1 && seen[0][D1N_A_PUP_UP][1] == 1);
    CHECK(!d1n_own_down(&PC[1]));

    /* 4. out of range: holding does nothing */
    reset_seen();
    lethal(1, t);
    t = run(t, 200);
    holding[0] = 1, range[0] = 0;
    t = run(t, 4000);
    CHECK(seen[1][D1N_A_REVIVE_PLAY][0] == 0 && d1n_own_down(&PC[1]));
    /* 5. a hold released early: CANCEL, back to lying, the bleed timer runs again */
    range[0] = 1;
    t = run(t, 1000);
    CHECK(seen[1][D1N_A_REVIVE_PLAY][0] == 1);
    holding[0] = 0;
    t = run(t, 300);
    CHECK(seen[1][D1N_A_REVIVE_STOP][0] == 1 && d1n_own_down(&PC[1]) && !dl_being_revived(&PC[1].d, 1));

    /* 6. bleed-out (60 s): the downed PC kills its human and says so; the other PC: his puppet dies, the own human dies too (game over on both) */
    reset_seen();
    t = run(t, 61000);
    CHECK(seen[1][D1N_A_DIE][0] == 1 && own_dead[1]);
    CHECK(seen[0][D1N_A_PUP_DIE][1] == 1 && pup_dead[0]);
    CHECK(seen[0][D1N_A_DIE][0] == 1 && own_dead[0]);
    CHECK(seen[0][D1N_A_GAME_OVER][0] >= 1 && seen[1][D1N_A_GAME_OVER][1] + seen[1][D1N_A_GAME_OVER][0] >= 1);
    /* the host's own death reached the client: his puppet dies there as well (death state on both PCs) */
    t = run(t, 300);
    CHECK(seen[1][D1N_A_PUP_DIE][1] == 1 && pup_dead[1]);
    CHECK(dl_state(&PC[0].d, 0) == DL_DEAD && dl_state(&PC[0].d, 1) == DL_DEAD && dl_state(&PC[1].d, 0) == DL_DEAD && dl_state(&PC[1].d, 1) == DL_DEAD);

    /* 7. the Continue: a new level instance (epoch) on both: everybody up; a late event of the old epoch is refused */
    for (int pc = 0; pc < 2; pc++) d1n_epoch(&PC[pc], 8, t), own_hp[pc] = 1000.0f, own_dead[pc] = pup_dead[pc] = 0;
    CHECK(dl_state(&PC[0].d, 0) == DL_ALIVE && dl_state(&PC[0].d, 1) == DL_ALIVE && !PC[0].d.over);
    {
        uint8_t old[D1N_MSG_LEN] = {D1N_VERSION, 7, DL_E_DOWN, 1, 0xFF, 0};
        uint32_t st0 = PC[0].rx_stale;
        CHECK(d1n_recv(&PC[0], old, D1N_MSG_LEN, t) == 0 && PC[0].rx_stale == st0 + 1);
        CHECK(!d1n_partner_down(&PC[0]));
        uint8_t foreign[D1N_MSG_LEN] = {D1N_VERSION, 8, DL_E_DOWN, 0, 0xFF, 0};   /* "from" = my own slot */
        CHECK(d1n_recv(&PC[0], foreign, D1N_MSG_LEN, t) == 0 && PC[0].rx_foreign == 1);
        uint8_t bad[D1N_MSG_LEN] = {9, 8, DL_E_DOWN, 1, 0xFF, 0};
        CHECK(d1n_recv(&PC[0], bad, D1N_MSG_LEN, t) == 0 && PC[0].rx_bad == 1);
        CHECK(d1n_recv(&PC[0], old, 3, t) == 0 && PC[0].rx_bad == 2);
    }

    /* 8. a revive whose link dies half way: the reviver lets go, but his CANCEL is lost; the downed side's watchdog stops it (2.5 s), he keeps lying */
    start(t, CP_SESS_DOWNED, 9);
    lethal(0, t);
    t = run(t, 200);
    holding[1] = 1, range[1] = 1;
    t = run(t, 800);
    CHECK(dl_being_revived(&PC[0].d, 0));
    drop_to[0] = 1;   /* nothing reaches the host any more */
    holding[1] = 0;
    t = run(t, 1000);
    CHECK(dl_being_revived(&PC[0].d, 0));   /* still believed (the watchdog is 2.5 s) */
    t = run(t, 2000);
    CHECK(seen[0][D1N_A_REVIVE_STOP][0] >= 1 && !dl_being_revived(&PC[0].d, 0) && d1n_own_down(&PC[0]));
    drop_to[0] = 0;
    t = run(t, 200);
    CHECK(d1n_partner_down(&PC[1]));   /* both PCs agree: the host still lies */

    /* 9. the DEAD event is lost: the client believes the host's predicted bleed-out after the grace and couples the game over */
    reset_seen();
    drop_to[1] = 1;   /* the host's DEAD never arrives */
    t = run(t, 61000);
    CHECK(seen[0][D1N_A_DIE][0] == 1);
    t = run(t, 2000);
    CHECKF(seen[1][D1N_A_PUP_DIE][1] == 1 && seen[1][D1N_A_DIE][0] == 1, "client: pupdie %d die %d over %d mirror %d left %d host %d", seen[1][D1N_A_PUP_DIE][1], seen[1][D1N_A_DIE][0], PC[1].d.over, dl_state(&PC[1].d, 0), PC[1].d.h[0].bleed_left, dl_state(&PC[0].d, 0));
    drop_to[1] = 0;

    /* 9b. a lost DOWN (and a lost DONE) is repaired by the unreliable STATE flags after DL_TRUST_MS (the glue feeds the partner's CP_SF_DOWN, never DEAD) */
    start(t, CP_SESS_DOWNED, 20);
    drop_to[1] = 1;
    lethal(0, t);
    t = run(t, 300);
    CHECK(!d1n_partner_down(&PC[1]));
    drop_to[1] = 0;
    dl_remote_sample(&PC[1].d, 0, 1, 0, t);
    t = run(t, 300);
    CHECK(!d1n_partner_down(&PC[1]));   /* not yet: 600 ms of disagreement first */
    dl_remote_sample(&PC[1].d, 0, 1, 0, t);
    t = run(t, 400);
    dl_remote_sample(&PC[1].d, 0, 1, 0, t);
    t = run(t, 50);
    CHECK(d1n_partner_down(&PC[1]) && seen[1][D1N_A_PUP_DOWN][1] == 1);

    /* 10. both down within one round trip: the second one is not absorbed (nobody up to raise him) -> both dead on both PCs */
    start(t, CP_SESS_DOWNED, 10);
    lethal(0, t);
    t = run(t, 100);
    float d2 = lethal(1, t);   /* the client knows the host is down: nobody to raise him -> an ordinary death */
    CHECKF(d2 >= 1000.0f, "not absorbed: %.1f", d2);
    t = run(t, 400);
    CHECK(own_hp[1] == 0.0f && seen[0][D1N_A_DIE][0] == 1 && seen[0][D1N_A_PUP_DIE][1] == 1);
    CHECK(PC[0].d.over && PC[1].d.over);

    /* 11. the partner leaves while I lie: I stand up (get_up_on_leave) */
    start(t, CP_SESS_DOWNED, 11);
    lethal(1, t);
    t = run(t, 200);
    d1n_present(&PC[1], 0, t);
    t = run(t, 100);
    CHECK(seen[1][D1N_A_GET_UP][0] == 1 && !d1n_own_down(&PC[1]));

    /* 12. the rule switched off: nothing is absorbed, nothing is sent */
    start(t, 0, 12);
    CHECK(d1n_absorb(&PC[0], 100.0f, 500.0f, t) == 500.0f);
    t = run(t, 500);
    CHECK(PC[0].tx == 0 && PC[1].tx == 0);

    /* 12b. wp6 live: a coupled game over with the flags fed every pass: the dead partner's flags say "not down", the mirror must stay DEAD (no puppet UP) */
    start(t, CP_SESS_DOWNED, 30);
    flags_on = 1;
    lethal(0, t);
    t = run(t, 200);
    CHECK(d1n_partner_down(&PC[1]) && seen[1][D1N_A_PUP_DOWN][1] == 1);
    t = run(t, 61000);
    CHECK(seen[0][D1N_A_DIE][0] == 1 && seen[1][D1N_A_DIE][0] == 1 && seen[1][D1N_A_PUP_DIE][1] == 1 && seen[0][D1N_A_PUP_DIE][1] == 1);
    t = run(t, 3000);
    CHECKF(seen[0][D1N_A_PUP_UP][1] == 0 && seen[1][D1N_A_PUP_UP][1] == 0, "a dead mirror came back: host %d client %d", seen[0][D1N_A_PUP_UP][1], seen[1][D1N_A_PUP_UP][1]);
    CHECK(dl_state(&PC[0].d, 1) == DL_DEAD && dl_state(&PC[1].d, 0) == DL_DEAD);
    /* ... and the repair of a lost DOWN still works with the flags on */
    start(t, CP_SESS_DOWNED, 31);
    flags_on = 1;
    drop_to[1] = 1;
    lethal(0, t);
    t = run(t, 200);
    drop_to[1] = 0;
    CHECK(!d1n_partner_down(&PC[1]));
    t = run(t, 800);
    CHECK(d1n_partner_down(&PC[1]) && seen[1][D1N_A_PUP_DOWN][1] == 1);
    flags_on = 0;

    /* 13. bleed preset 30 s */
    start(t, CP_SESS_DOWNED | (1u << 1), 13);
    lethal(0, t);
    t = run(t, 29000);
    CHECK(seen[0][D1N_A_DIE][0] == 0);
    t = run(t, 2000);
    CHECK(seen[0][D1N_A_DIE][0] == 1 && seen[1][D1N_A_PUP_DIE][1] == 1);

    /* 14. random replay: no crash, the queue bound holds, the states stay in range */
    {
        int bad = 0;
        srand(7);
        start(t, CP_SESS_DOWNED, 14);
        for (int i = 0; i < 40000; i++) {
            t += PASS;
            int r = rand() % 1000;
            if (r < 3) lethal(rand() % 2, t);
            if (r == 5) for (int pc = 0; pc < 2; pc++) d1n_epoch(&PC[pc], (unsigned)(15 + i), t), own_hp[pc] = 1000.0f;
            if (r == 6) drop_to[rand() % 2] ^= 1;
            holding[0] = rand() % 3 == 0, range[0] = rand() % 2;
            holding[1] = rand() % 3 == 0, range[1] = rand() % 2;
            pass(t);
            for (int pc = 0; pc < 2; pc++)
                for (int h = 0; h < 2; h++)
                    if (dl_state(&PC[pc].d, h) < DL_ALIVE || dl_state(&PC[pc].d, h) > DL_GONE) bad++;
            if (nw > 400) bad++, nw = 0;
        }
        CHECKF(bad == 0, "%d bad replay steps", bad);
    }

    /* ---- round 4: the rescuer's hold (wp7 live: the host's hold ended 0.73 s after BEGIN, the client bled out) ---- */

    /* 15. the reach: horizontal + floor; a running hold keeps going up to D1N_REACH_KEEP; legacy = the old 3-D test */
    start(t, CP_SESS_DOWNED, 40);
    CHECK(d1n_in_reach(&PC[0], 169.0f, 0.0f, 0.0f) && !d1n_in_reach(&PC[0], 120.0f, 0.0f, 125.0f));
    CHECK(d1n_in_reach(&PC[0], 100.0f, 140.0f, 0.0f) && !d1n_in_reach(&PC[0], 100.0f, 160.0f, 0.0f));   /* a lying body's root lower than the rescuer: same floor */
    PC[0].legacy = 1;
    CHECK(!d1n_in_reach(&PC[0], 100.0f, 140.0f, 0.0f) && d1n_in_reach(&PC[0], 100.0f, 0.0f, 130.0f));
    PC[0].legacy = 0;
    lethal(1, t);
    t = run(t, 200);
    holding[0] = 1, range[0] = 1;
    t = run(t, 300);
    CHECK(d1n_hold_running(&PC[0]));
    CHECK(d1n_in_reach(&PC[0], 220.0f, 0.0f, 0.0f) && !d1n_in_reach(&PC[0], 240.0f, 0.0f, 0.0f));   /* keep range while the hold runs */
    holding[0] = 0;
    t = run(t, 400);
    CHECK(!d1n_hold_running(&PC[0]) && !d1n_in_reach(&PC[0], 220.0f, 0.0f, 0.0f));

    /* 16. who owns the button: only a standing own human next to the lying partner; never in the legacy mode */
    CHECK(d1n_owns_button(&PC[0], 1) && !d1n_owns_button(&PC[0], 0));
    CHECK(!d1n_owns_button(&PC[1], 1));   /* the client lies himself */
    PC[0].legacy = 1;
    CHECK(!d1n_owns_button(&PC[0], 1));
    PC[0].legacy = 0;
    /* the button: the game's word or the own pad in play; legacy: the game's word only */
    CHECK(d1n_button(&PC[0], 1, 0, 0) && d1n_button(&PC[0], 0, 1, 1) && !d1n_button(&PC[0], 0, 1, 0) && !d1n_button(&PC[0], 0, 0, 1));
    PC[0].legacy = 1;
    CHECK(!d1n_button(&PC[0], 0, 1, 1) && d1n_button(&PC[0], 1, 0, 0));
    PC[0].legacy = 0;
    t = run(t, 3000);   /* the client still lies (bleed 60 s) */

    /* 17. one bad pass every 10 passes (160 ms) during the host's hold: bridged, no CANCEL, the client gets up after ~3 s of holding */
    start(t, CP_SESS_DOWNED, 41);
    lethal(1, t);
    t = run(t, 200);
    reset_seen();
    {
        uint32_t t0 = t;
        int done_at = -1;
        for (int i = 0; i < 400 && done_at < 0; i++) {
            holding[0] = i % 10 != 9, range[0] = 1;
            pass(t);
            t += PASS;
            if (seen[1][D1N_A_GET_UP][0]) done_at = (int)(t - t0);
        }
        holding[0] = 0;
        CHECKF(done_at > 0 && seen[1][D1N_A_REVIVE_STOP][0] == 0, "done at %d ms, revive stops %d", done_at, seen[1][D1N_A_REVIVE_STOP][0]);
        CHECKF(done_at >= 3000 && done_at < 3700, "the gaps add no progress: done at %d ms", done_at);
        CHECK(PC[0].hold_bridged >= 20 && PC[0].hold_cancels == 0);
        CHECK(seen[0][D1N_A_PUP_UP][1] == 1 && !d1n_own_down(&PC[1]));
    }

    /* 18. the same with the range flickering (a lying body's root, the rescuer's small moves): bridged too */
    start(t, CP_SESS_DOWNED, 42);
    lethal(1, t);
    t = run(t, 200);
    reset_seen();
    for (int i = 0; i < 260; i++) {
        holding[0] = 1, range[0] = i % 7 != 3;
        pass(t);
        t += PASS;
    }
    holding[0] = 0;
    t = run(t, 200);
    CHECK(seen[1][D1N_A_GET_UP][0] == 1 && seen[1][D1N_A_REVIVE_STOP][0] == 0 && PC[0].hold_cancels == 0);

    /* 19. a real release (longer than the grace) still ends the hold: CANCEL, the client lies on, his bleed timer runs again */
    start(t, CP_SESS_DOWNED, 43);
    lethal(1, t);
    t = run(t, 200);
    reset_seen();
    holding[0] = 1, range[0] = 1;
    t = run(t, 1000);
    holding[0] = 0;
    t = run(t, D1N_HOLD_GRACE_MS + 100);
    CHECK(PC[0].hold_cancels == 1 && seen[1][D1N_A_REVIVE_STOP][0] == 1 && d1n_own_down(&PC[1]) && !dl_being_revived(&PC[1].d, 1));
    /* gaps that keep coming back within the grace but never reach 3 s of holding do not revive by themselves */
    holding[0] = 1;
    t = run(t, 1500);
    CHECK(seen[1][D1N_A_GET_UP][0] == 0);
    holding[0] = 0;
    t = run(t, 400);

    /* 20. the wp7 replay: A held 3.8 s, the game's word lost after 0.73 s (the stock action took the press), the own pad still holds A.
     *     legacy (02a375b): CANCEL at ~0.73 s, the client bleeds out; new: the own pad counts (the revive owns A), the client gets up */
    for (int legacy = 1; legacy >= 0; legacy--) {
        start(t, CP_SESS_DOWNED | (1u << 1), 44 + (unsigned)legacy);   /* bleed 30 s */
        PC[0].legacy = legacy;
        lethal(1, t);
        t = run(t, 25000);   /* the hold starts late in the bleed-out (wp7: at the 58th second of 60) */
        reset_seen();
        uint32_t t0 = t;
        for (uint32_t e = 0; e < 8000; e += PASS) {
            int game_a = e < 3800 && e < 730, raw_a = e < 3800;
            holding[0] = d1n_button(&PC[0], game_a, raw_a, 1), range[0] = 1;
            pass(t0 + e);
        }
        t = t0 + 8000;
        if (legacy)
            CHECKF(PC[0].hold_cancels == 1 && seen[1][D1N_A_DIE][0] == 1 && seen[0][D1N_A_PUP_DIE][1] == 1, "legacy: cancels %u die %d", PC[0].hold_cancels, seen[1][D1N_A_DIE][0]);
        else
            CHECKF(PC[0].hold_cancels == 0 && seen[1][D1N_A_GET_UP][0] == 1 && seen[1][D1N_A_DIE][0] == 0 && seen[0][D1N_A_PUP_UP][1] == 1, "new: cancels %u getup %d die %d", PC[0].hold_cancels,
                   seen[1][D1N_A_GET_UP][0], seen[1][D1N_A_DIE][0]);
    }

    printf("test_downed_net: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
