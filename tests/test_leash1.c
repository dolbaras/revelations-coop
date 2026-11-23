/* test_leash1.c - the partner gate's leash and scene gate (native/leash1.h): zig cc -o test_leash1.exe test_leash1.c */
#include <stdio.h>
#include "../native/leash1.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    ls_t s;
    float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    uint32_t t = 1000;
    ls_init(&s);
    CHECK(s.on && s.n1 == 2500.0f && s.n2 == 4000.0f);

    /* close together: nothing, ever */
    for (int i = 0; i < 100; i++, t += 50) { b[0] = 300; CHECK(!ls_step(&s, a, b, 1, t)); CHECK(s.scale[0] == 1000 && s.scale[1] == 1000); }
    CHECK(s.level == 0 && s.hints == 0);

    /* human 1 walks away at 400 u/s: hint from N1 (renewed every LS_HINT_MS), damping from N2 on for HIM only, human 0 stands untouched */
    int hint_n = 0, saw_damp1 = 0, saw_damp0 = 0;
    for (b[0] = 300; b[0] < 6500; b[0] += 20, t += 50) {
        if (ls_step(&s, a, b, 1, t)) hint_n++;
        if (b[0] < 2500) CHECK(s.level == 0 && s.scale[1] == 1000);
        if (b[0] > 2600 && b[0] < 3900) CHECK(s.level == 1 && s.scale[1] == 1000);   /* hint zone: no damping yet */
        if (s.scale[1] < 1000) saw_damp1 = 1;
        if (s.scale[0] < 1000) saw_damp0 = 1;
    }
    CHECK(hint_n >= 5 && saw_damp1 && !saw_damp0);
    CHECK(s.scale[1] >= LS_MIN_PERMILLE && s.scale[1] <= 1000);
    CHECK(s.level == 2);
    /* far beyond the ramp: the minimum, never 0 */
    for (int i = 0; i < 20; i++, t += 50) { b[0] += 20; ls_step(&s, a, b, 1, t); }
    CHECK(s.scale[1] == LS_MIN_PERMILLE);

    /* walking back towards the partner is never damped */
    int back_damped = 0;
    for (; b[0] > 3000; b[0] -= 20, t += 50) {
        ls_step(&s, a, b, 1, t);
        if (b[0] < 6000 && s.scale[1] < 1000 && t > 0) { /* allowed only during the hold after the last away sample */ }
    }
    /* once he turned, after the hold at most the first few samples may be damped */
    b[0] = 6000; ls_reset(&s);
    ls_step(&s, a, b, 1, t += 200);
    for (int i = 0; i < 30; i++, t += 50) { b[0] -= 20; ls_step(&s, a, b, 1, t); if (i > 10 && s.scale[1] < 1000) back_damped++; }
    CHECK(back_damped == 0);

    /* the lead walks away from a standing partner: damped the same way */
    ls_init(&s);
    t += 10000; a[0] = 0; b[0] = 4600;
    ls_step(&s, a, b, 1, t);
    int d0 = 0;
    for (int i = 0; i < 40; i++, t += 50) { a[0] -= 25; ls_step(&s, a, b, 1, t); if (s.scale[0] < 1000) d0 = 1; CHECK(s.scale[1] == 1000); }
    CHECK(d0);

    /* inactive (cutscene / menu / solo / downed): all off and the history is dropped */
    CHECK(!ls_step(&s, a, b, 0, t += 50) && s.level == 0 && s.scale[0] == 1000 && !s.have_prev);

    /* switch off: nothing */
    ls_init(&s); s.on = 0; b[0] = 9000;
    for (int i = 0; i < 40; i++, t += 50) { CHECK(!ls_step(&s, a, b, 1, t)); CHECK(s.scale[1] == 1000); }

    /* hysteresis: right at N1 it does not flicker */
    ls_init(&s); a[0] = 0; b[0] = 2500; ls_step(&s, a, b, 1, t += 50); CHECK(s.level == 1);
    b[0] = 2400; ls_step(&s, a, b, 1, t += 50); CHECK(s.level == 1);
    b[0] = 2100; ls_step(&s, a, b, 1, t += 50); CHECK(s.level == 0);

    /* the gate: far -> move once; near -> no move, request consumed; stale -> dropped */
    ls_init(&s); a[0] = 0; b[0] = 3000;
    CHECK(!ls_gate_step(&s, a, b, t));                       /* no request */
    ls_gate_request(&s, t += 10); CHECK(ls_gate_step(&s, a, b, t += 50) == 1 && s.gate_moves == 1);
    CHECK(!ls_gate_step(&s, a, b, t += 50));                 /* consumed */
    b[0] = 500; ls_gate_request(&s, t += 10); CHECK(ls_gate_step(&s, a, b, t += 50) == 0 && s.gate_moves == 1);
    b[0] = 3000; ls_gate_request(&s, t += 10); CHECK(ls_gate_step(&s, a, b, t += LS_GATE_TTL_MS + 10) == 0);
    s.on = 0; ls_gate_request(&s, t += 10); CHECK(!ls_gate_step(&s, a, b, t += 50));

    printf("test_leash1: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
