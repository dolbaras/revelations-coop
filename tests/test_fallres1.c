/* test_fallres1.c - the fall-out rescue decision (native/fallres1.h): zig cc -o test_fallres1.exe test_fallres1.c */
#include <stdio.h>
#include "../native/fallres1.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    frs_t f;
    /* ground all the time: never */
    frs_reset(&f);
    for (uint32_t t = 1000; t < 20000; t += 250) CHECK(!frs_step(&f, 0, 500.0f, 1, t));
    CHECK(f.rescues == 0);

    /* 2-2: no ground from the first pass (hovering at y 500): the rescue comes after FRS_AFTER_MS, well before the engine's 5000 ms, exactly once */
    frs_reset(&f);
    uint32_t first = 0;
    int n = 0;
    for (uint32_t t = 1000; t <= 6000; t += 250) {
        int r = frs_step(&f, 1, 500.0f, 1, t);
        if (r) { n++; if (!first) first = t; }
    }
    CHECK(n == 1 && first >= 1000 + FRS_AFTER_MS && first < 1000 + 5000);
    /* after the rescue the human stands on ground: nothing more */
    for (uint32_t t = 7000; t < 30000; t += 250) CHECK(!frs_step(&f, 0, 500.0f, 1, t));
    CHECK(f.rescues == 1);

    /* a falling human (height drops every pass) is never rescued: the pit still kills */
    frs_reset(&f);
    for (uint32_t t = 1000, k = 0; t < 8000; t += 250, k++) CHECK(!frs_step(&f, 1, 500.0f - 40.0f * (float)k, 1, t));
    CHECK(f.rescues == 0 && f.falling_left > 0);

    /* a slow drift inside the tolerance still counts as hovering */
    frs_reset(&f);
    n = 0;
    for (uint32_t t = 1000, k = 0; t < 5000; t += 250, k++) n += frs_step(&f, 1, 500.0f - 1.0f * (float)k, 1, t);
    CHECK(n == 1);

    /* no partner unit: no rescue (nothing to go to) */
    frs_reset(&f);
    for (uint32_t t = 1000; t < 9000; t += 250) CHECK(!frs_step(&f, 1, 500.0f, 0, t));

    /* ground flickers on / off: the spell restarts, no rescue */
    frs_reset(&f);
    for (uint32_t t = 1000, k = 0; t < 12000; t += 250, k++) CHECK(!frs_step(&f, (k % 4) != 3, 500.0f, 1, t));
    CHECK(f.rescues == 0);

    /* cooldown: a second ground-less spell right after a rescue waits FRS_COOLDOWN_MS */
    frs_reset(&f);
    uint32_t t = 1000;
    int got = 0;
    for (; t < 4000; t += 250) got += frs_step(&f, 1, 500.0f, 1, t);
    CHECK(got == 1);
    uint32_t t_resc = t;
    got = 0;
    for (; t < t_resc + FRS_COOLDOWN_MS - 3000; t += 250) got += frs_step(&f, 1, 500.0f, 1, t);   /* inside the cooldown (a new spell would be due earlier) */
    CHECK(got == 0);
    for (; t < t_resc + FRS_COOLDOWN_MS + 4000; t += 250) got += frs_step(&f, 1, 500.0f, 1, t);
    CHECK(got == 1);

    float p[3] = {100.0f, 500.0f, -200.0f}, o[3];
    frs_target(p, o);
    CHECK(o[1] == 500.0f && fabsf(o[0] - 170.0f) < 0.01f && fabsf(o[2] + 130.0f) < 0.01f);

    printf("test_fallres1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
