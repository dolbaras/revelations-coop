/*
 * Offline replay of the Revelations 1 local-split pair (native/downed1_pair.h on top of native/downed_logic.h). No game, no Windows.
 *   sh tests/build.sh   ->  tests/build/test_downed_pair.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/downed1_pair.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define CHECKF(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s: ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static d1_pair_t P;
static d1_action A[16];
static int na;

static void step(uint32_t now) { na = d1_step(&P, now, A, 16); }
static int has(int act, int who)
{
    for (int i = 0; i < na; i++)
        if (A[i].act == act && A[i].who == who)
            return 1;
    return 0;
}
static int count(int act, int who)
{
    int c = 0;
    for (int i = 0; i < na; i++)
        c += A[i].act == act && A[i].who == who;
    return c;
}

/* a hit that the engine would apply: returns the amount after the rule */
static float hit(int k, float hp, float dmg, uint32_t now) { return d1_absorb(&P, k, hp, dmg, now); }

#define T0 1000000u

int main(void)
{
    /* 1. rule off: nothing changes, a lethal hit goes through */
    d1_init(&P, 0, T0);
    CHECK(!P.on);
    CHECK(hit(0, 5.0f, 9.0f, T0) == 9.0f);
    step(T0);
    CHECK(na == 0);

    /* 2. rule on: a lethal hit on 0 with 1 up is absorbed to 1 hp, 0 goes DOWN, 1 learns it at once */
    d1_init(&P, CP_SESS_DOWNED, T0);
    CHECK(P.on);
    CHECK(hit(0, 5.0f, 2.0f, T0) == 2.0f);          /* not lethal: unchanged */
    CHECK(hit(0, 5.0f, 9.0f, T0) == 4.0f);          /* lethal: keep 1 hp (5 - 4) */
    step(T0);
    CHECK(has(D1_A_GO_DOWN, 0));
    CHECK(d1_is_down(&P, 0));
    CHECK(dl_state(&P.d[1], 0) == DL_DOWN);         /* the other machine knows */
    CHECK(!d1_is_down(&P, 1));

    /* 3. hits on a DOWN human are absorbed, whatever the amount */
    CHECK(hit(0, 1.0f, 3.0f, T0 + 100) == 0.0f);
    CHECK(hit(0, 1.0f, 0.5f, T0 + 100) == 0.0f);
    CHECK(hit(0, 1.0f, 0.0f, T0 + 100) == 0.0f);    /* zero damage passes through unchanged */
    step(T0 + 100);
    CHECK(count(D1_A_GO_DOWN, 0) == 0);             /* not twice */

    /* 4. the partner is lethal now: nobody up to raise him -> the engine kills him, game over, the downed one dies too */
    CHECK(hit(1, 5.0f, 9.0f, T0 + 200) == 9.0f);
    d1_dead(&P, 1, T0 + 200);
    step(T0 + 200);
    CHECK(dl_state(&P.d[0], 1) == DL_DEAD);
    CHECK(has(D1_A_DIE, 0));                        /* the downed human is ended by the rule's game over */
    CHECK(has(D1_A_DIE, 0) && count(D1_A_DIE, 0) == 1);

    /* 5. revive: 0 is DOWN with 1 up; 1 holds the button near him for the revive time -> 0 gets up at 35 % */
    d1_init(&P, CP_SESS_DOWNED, T0);
    hit(0, 5.0f, 9.0f, T0); step(T0);
    CHECK(d1_is_down(&P, 0));
    int got_up = 0, revive_play = 0;
    for (uint32_t t = T0 + 100; t <= T0 + 4500; t += 100) {
        d1_hold(&P, 1, 1, 1, t);                    /* 1 holds A, in range */
        step(t);
        got_up += has(D1_A_GET_UP, 0);
        revive_play += has(D1_A_REVIVE_PLAY, 0);
        if (got_up) break;
    }
    CHECK(revive_play >= 1);
    CHECKF(got_up == 1, "got up %d", got_up);
    CHECK(!d1_is_down(&P, 0));
    CHECK(dl_state(&P.d[1], 0) == DL_ALIVE);

    /* 6. revive interrupted: holder lets go before the time -> REVIVE_STOP on the downed one, he is still down */
    d1_init(&P, CP_SESS_DOWNED, T0);
    hit(0, 5.0f, 9.0f, T0); step(T0);
    int stopped = 0;
    for (uint32_t t = T0 + 100; t <= T0 + 1000; t += 100) { d1_hold(&P, 1, 1, 1, t); step(t); }
    for (uint32_t t = T0 + 1100; t <= T0 + 1600; t += 100) { d1_hold(&P, 1, 0, 1, t); step(t); stopped += has(D1_A_REVIVE_STOP, 0); }
    CHECK(stopped >= 1);
    CHECK(d1_is_down(&P, 0));

    /* 7. out of range: no progress (the hold does not start) */
    d1_init(&P, CP_SESS_DOWNED, T0);
    hit(0, 5.0f, 9.0f, T0); step(T0);
    int up7 = 0;
    for (uint32_t t = T0 + 100; t <= T0 + 6000; t += 100) { d1_hold(&P, 1, 1, 0, t); step(t); up7 += has(D1_A_GET_UP, 0); }
    CHECK(up7 == 0);
    CHECK(d1_is_down(&P, 0));

    /* 8. bleed-out at the default 60 s (1 s steps: the time rule clamps a step to 1 s) */
    d1_init(&P, CP_SESS_DOWNED, T0);
    hit(0, 5.0f, 9.0f, T0); step(T0);
    int dies = 0;
    for (uint32_t t = T0 + 1000; t <= T0 + 59000; t += 1000) { step(t); dies += has(D1_A_DIE, 0); }
    CHECK(dies == 0);
    step(T0 + 61000);
    CHECK(has(D1_A_DIE, 0));

    /* 9. presets: flags bits 1..2 = 30 s (1), 120 s (2), never (3) */
    d1_init(&P, CP_SESS_DOWNED | (1u << 1), T0);
    hit(0, 5.0f, 9.0f, T0); step(T0);
    int early = 0;
    for (uint32_t t = T0 + 1000; t < T0 + 30000; t += 1000) { step(t); early += has(D1_A_DIE, 0); }
    CHECK(early == 0);
    step(T0 + 30000);
    CHECK(has(D1_A_DIE, 0));                        /* the timer runs out at exactly 30 s */
    d1_init(&P, CP_SESS_DOWNED | (3u << 1), T0);
    hit(0, 5.0f, 9.0f, T0); step(T0);
    for (uint32_t t = T0 + 1000; t <= T0 + 600000; t += 1000) { step(t); CHECKF(!has(D1_A_DIE, 0), "died at %u", (unsigned)(t - T0)); if (has(D1_A_DIE, 0)) break; }

    /* 10. single human (partner not in play): the rule has nobody to raise him -> an ordinary hit */
    d1_init(&P, CP_SESS_DOWNED, T0);
    d1_set_present(&P, 1, 0, T0);
    CHECK(hit(0, 5.0f, 9.0f, T0) == 9.0f);
    step(T0);
    CHECK(!d1_is_down(&P, 0));

    /* 11. partner leaves while I lie: I stand up (get_up_on_leave) */
    d1_init(&P, CP_SESS_DOWNED, T0);
    hit(0, 5.0f, 9.0f, T0); step(T0);
    d1_set_present(&P, 1, 0, T0 + 100);
    step(T0 + 100);
    CHECK(has(D1_A_GET_UP, 0));
    CHECK(!d1_is_down(&P, 0));

    /* 12. both lethal at the same moment: the first goes DOWN, the second has nobody up -> dies, the first dies with him */
    d1_init(&P, CP_SESS_DOWNED, T0);
    CHECK(hit(0, 5.0f, 9.0f, T0) == 4.0f);
    CHECK(hit(1, 5.0f, 9.0f, T0) == 4.0f);          /* the DOWN of 0 is not delivered yet: both see the other up, both are absorbed */
    step(T0);
    CHECK(dl_state(&P.d[0], 0) == DL_DEAD && dl_state(&P.d[1], 1) == DL_DEAD);   /* nobody up: game over, both are ended */
    CHECK(has(D1_A_DIE, 0) && has(D1_A_DIE, 1));

    /* 13. replay: random hits, holds and time for 20000 passes: bounded output, absorb never invents damage */
    srand(1234);
    d1_init(&P, CP_SESS_DOWNED, T0);
    uint32_t t = T0;
    int bad = 0;
    for (int i = 0; i < 20000; i++) {
        t += 10 + (uint32_t)(rand() % 40);
        int k = rand() % 2;
        if (rand() % 7 == 0) {
            float dmg = (float)(rand() % 900) / 10.0f;
            float hp = 1.0f + (float)(rand() % 500) / 10.0f;
            float got = hit(k, hp, dmg, t);
            if (got < 0.0f || (got > dmg && got > hp - 1.0f + 1e-3f)) bad++;   /* never more than the hit, or the hp-1 keep of a lethal one */
            if (hp - dmg <= 0.0f && got == dmg && P.d[k].h[k].state == DL_ALIVE) d1_dead(&P, k, t);   /* the engine kills him: an ordinary death */
        }
        d1_hold(&P, k, rand() % 3 != 0, rand() % 4 != 0, t);
        step(t);
        if (na > 16) bad++;
        if (rand() % 100 == 0) d1_init(&P, CP_SESS_DOWNED, t);
    }
    CHECKF(bad == 0, "%d bad replay steps", bad);

    printf("test_downed_pair: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
