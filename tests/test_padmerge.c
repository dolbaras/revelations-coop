/* Offline test of r1_input_merge (native/coop_input.h): the record of the second pad is merged into the first pad's while a game over / pause screen runs.
 * Build + run: sh tests/build.sh (compiles and runs it after test_lobby1). */
#include <stdio.h>
#include <string.h>
#include "../native/coop_input.h"

static int fails, checks;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", msg); } else printf("ok   %s\n", msg); } while (0)

int main(void)
{
    r1_input_t a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    b.buttons = 0x1000;   /* A of the second pad */
    r1_input_merge(&a, &b);
    CHECK(a.buttons == 0x1000, "A of the second pad reaches pad 0 (game over: Continue?)");
    a.buttons = 0x0001; b.buttons = 0x0002;
    r1_input_merge(&a, &b);
    CHECK(a.buttons == 0x0003, "buttons are OR-ed");
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.lt = 10; b.lt = 200; a.rt = 90; b.rt = 5;
    r1_input_merge(&a, &b);
    CHECK(a.lt == 200 && a.rt == 90, "triggers: the larger of the two");
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.lx = 1000; a.ly = 0; b.lx = 0; b.ly = -30000;
    r1_input_merge(&a, &b);
    CHECK(a.lx == 0 && a.ly == -30000, "left stick: the bigger deflection wins as a pair (a menu moves by the second pad's stick)");
    a.lx = 32000; a.ly = 0; b.lx = 0; b.ly = 5000;
    r1_input_merge(&a, &b);
    CHECK(a.lx == 32000 && a.ly == 0, "left stick: a small drift of the other pad does not replace a real deflection");
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    b.rx = 20000; b.ry = 100;
    r1_input_merge(&a, &b);
    CHECK(a.rx == 20000 && a.ry == 100 && a.lx == 0, "right stick merged on its own");
    CHECK(sizeof(r1_input_t) == 12, "the record keeps the XINPUT_GAMEPAD layout (12 bytes)");
    printf("%d of %d checks passed\n", checks - fails, checks);
    return fails != 0;
}
