/* test_descent1.c: descent1.h (rel027u, B2): one human jumps down 220 units, the other stays on the platform -> put next to him after the grace time; no follow when he comes down himself,
 * for stairs / small ledges, in cutscenes, with the switch off, and only once per drop. */
#include <stdio.h>
#include <math.h>
#include "../native/descent1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct { ds_t s; float p[2][3]; uint32_t now; int follow, who; } sim_t;
static void sim_init(sim_t *m)
{
    memset(m, 0, sizeof *m);
    ds_init(&m->s);
    m->p[0][0] = 2216, m->p[0][1] = 380, m->p[0][2] = -1825;
    m->p[1][0] = 1900, m->p[1][1] = 380, m->p[1][2] = -1880;
    m->now = 1000, m->who = -1;
}
/* runs ms of 20 ms passes; the humans move by the given speeds (units per second) */
static void run(sim_t *m, uint32_t ms, float v0y, float v1y, int active)
{
    for (uint32_t t = 0; t < ms; t += 20) {
        m->now += 20;
        m->p[0][1] += v0y * 0.02f, m->p[1][1] += v1y * 0.02f;
        int d = -1;
        if (ds_step(&m->s, m->p[0], m->p[1], active, m->now, &d)) m->follow++, m->who = d;
    }
}

int main(void)
{
    sim_t m;
    /* 1. human 0 jumps down 220 over 1.5 s and lands; human 1 stays: one follow, who = 0, after the grace time */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, -147.0f, 0, 1);                      /* the jump: 220 units */
    CHECK(fabsf(m.p[0][1] - 160.0f) < 3.0f);
    run(&m, DS_LAND_MS + DS_GRACE_MS - 500, 0, 0, 1);
    CHECK(m.follow == 0);                              /* still inside the grace time */
    run(&m, 1500, 0, 0, 1);
    CHECK(m.follow == 1 && m.who == 0);
    run(&m, 30000, 0, 0, 1);                           /* once only (after the placement the pair is together in the game; here nobody moved) */
    CHECK(m.follow == 1);
    /* 2. the second human is the one who goes down */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, 0, -147.0f, 1);
    run(&m, DS_LAND_MS + DS_GRACE_MS + 1500, 0, 0, 1);
    CHECK(m.follow == 1 && m.who == 1);
    /* 3. he follows on his own inside the grace time: no placement */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, -147.0f, 0, 1);
    run(&m, 2500, 0, 0, 1);
    run(&m, 1500, 0, -147.0f, 1);
    run(&m, 20000, 0, 0, 1);
    CHECK(m.follow == 0);
    /* 4. a stairs / a ledge: slow (150 u over 20 s) or small (100 u): nothing */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 20000, -8.0f, 0, 1);
    run(&m, 30000, 0, 0, 1);
    CHECK(m.follow == 0);
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 1000, -100.0f, 0, 1);
    run(&m, 30000, 0, 0, 1);
    CHECK(m.follow == 0);
    /* 5. both go down together (the partner is below him by the time of the landing): nothing */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, -147.0f, -147.0f, 1);
    run(&m, 30000, 0, 0, 1);
    CHECK(m.follow == 0);
    /* 6. a cutscene (not active) during the grace time only FREEZES the watching: the follow comes after it, with the grace time counted in play time only */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, -147.0f, 0, 1);
    run(&m, 2000, 0, 0, 1);
    run(&m, 60000, 0, 0, 0);
    run(&m, 2000, 0, 0, 1);
    CHECK(m.follow == 0);                              /* 2.7 s landed + 2 s: inside the grace time */
    run(&m, DS_GRACE_MS, 0, 0, 1);
    CHECK(m.follow == 1 && m.who == 0);
    /* 6b. the jump itself starts the scene (b2e1): the drop is seen, the scene runs 30 s while he lands, the follow comes after it */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 600, 0, -300.0f, 1);                       /* 180 units fallen, still falling */
    run(&m, 1500, 0, 0, 0);
    m.p[1][1] -= 40.0f;                                /* lands during the scene */
    run(&m, 30000, 0, 0, 0);
    run(&m, 3000, 0, 0, 1);
    CHECK(m.follow == 0);
    run(&m, DS_GRACE_MS, 0, 0, 1);
    CHECK(m.follow == 1 && m.who == 1);
    /* 6c. rel027z: a human who is still above 30 s after the landing (the owner's tester could take the jump himself) is NOT moved yet; only a really stranded one is */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, -147.0f, 0, 1);
    run(&m, 30000, 0, 0, 1);
    CHECK(m.follow == 0 && DS_GRACE_MS >= 40000u);
    /* 7. switch off */
    sim_init(&m);
    m.s.on = 0;
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, -147.0f, 0, 1);
    run(&m, 30000, 0, 0, 1);
    CHECK(m.follow == 0);
    /* 8. a teleport (a load: height changes by more than 400 in one sample) is no descent */
    sim_init(&m);
    run(&m, 2000, 0, 0, 1);
    m.p[0][1] = -100;
    run(&m, 30000, 0, 0, 1);
    CHECK(m.follow == 0);
    /* 9. horizontally close below (the partner is only 250 u away, 3D): no placement */
    sim_init(&m);
    m.p[1][0] = 2216 + 150, m.p[1][2] = -1825;
    run(&m, 2000, 0, 0, 1);
    run(&m, 1500, -147.0f, 0, 1);
    run(&m, 30000, 0, 0, 1);
    CHECK(m.follow == 0);
    printf("descent1: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
