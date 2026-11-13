/* test_wgate1.c - fix021 (0.2.1): the world's work of a level instance waits until this PC's own level is up (native/wgate1.h).
 * The fw1 23:53 repro on 5848e1d2, the guest's clock: the barrier opened by its timeout at t = 0 (phase PLAYING, nothing reported loaded), the guest's human
 * placed at 180404 ms (its level start), its 12_005_0 cutscene at +350 ms, its own scene script would place the Oozes (block 2) at +385 ms (the six good
 * 1-2 starts: 35-38 ms after the cutscene start); the host's SPAWN of block 2 arrived at about +345 ms (the host 1.5 s ahead after the level-start hold,
 * its own spawn 28 ms after its cutscene). The model before the fix applied that SPAWN at its arrival: op_spawn placed 2/0 before the scene did, the
 * assert came 22 ms after the unit appeared. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/wgate1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* the time the guest applies an event that arrives at `arrive` (the sender repeats it every 200 ms until it is taken); -1 = never in the window */
static long applied_at(int fixed, int reported_loaded, long placed_at, long arrive, long end)
{
    wg_t g;
    memset(&g, 0, sizeof g);
    for (long t = 0; t <= end; t += 5) {   /* the logic frames */
        int up = fixed ? wg_step(&g, 1, reported_loaded, t >= placed_at, (uint32_t)t, NULL) : 1;
        if (up && t >= arrive && ((t - arrive) % 200 == 0 || !fixed)) return t;
    }
    return -1;
}

int main(void)
{
    const long placed = 180405, own_scene_spawn = placed + 385, host_spawn_arrives = placed + 345;
    long before = applied_at(0, 0, placed, host_spawn_arrives, placed + 10000), after = applied_at(1, 0, placed, host_spawn_arrives, placed + 10000);
    if (before < own_scene_spawn)
        printf("  MODEL BEFORE THE FIX FAILS: fw1 23:53 repro: the host's SPAWN of 2/0 applied %ld ms after the placement, the guest's own scene places it at %ld ms\n", before - placed,
               own_scene_spawn - placed);
    CHECK(before >= 0 && before < own_scene_spawn, "the model before the fix: the host's SPAWN placed the Ooze before the guest's own scene (the crash order)");
    CHECK(after >= placed + (long)WG_UP_MS && after > own_scene_spawn, "the fix: the host's SPAWN only after this PC's level is up (placed + 2 s), after its own scene placed it");

    wg_t g;
    memset(&g, 0, sizeof g);
    int how = -1;
    CHECK(wg_step(&g, 3, 1, 1, 1000, &how) && how == WG_HOW_LOADED, "the usual order: this PC reported LOAD_DONE before the barrier opened: up at once (no change)");
    CHECK(wg_step(&g, 3, 1, 1, 1005, &how) && how == WG_HOW_NONE, "  and the moment is told once");
    CHECK(!wg_step(&g, 4, 0, 0, 2000, &how), "a new level instance (epoch 4) opened by the timeout: not up while the human is unplaced (the intro movie)");
    CHECK(!wg_step(&g, 4, 0, 0, 200000, &how), "  minutes of the intro: still not up");
    CHECK(!wg_step(&g, 4, 0, 1, 200100, &how) && !wg_step(&g, 4, 0, 1, 202099, &how), "  placed: not up for WG_UP_MS");
    CHECK(wg_step(&g, 4, 0, 1, 202100, &how) && how == WG_HOW_PLACED, "  placed for 2 s: up (the level start's own spawns are done)");
    CHECK(wg_step(&g, 4, 0, 0, 202200, &how), "  once up it stays up in that level instance (a cutscene that hides the human does not stop the world)");
    CHECK(!wg_step(&g, 5, 0, 1, 300000, &how), "the next level instance starts over");
    memset(&g, 0, sizeof g);
    CHECK(!wg_step(&g, 6, 0, 1, 1000, &how) && !wg_step(&g, 6, 0, 0, 2500, &how) && !wg_step(&g, 6, 0, 1, 2600, &how) && !wg_step(&g, 6, 0, 1, 4599, &how)
              && wg_step(&g, 6, 0, 1, 4600, &how), "a placement that drops back to 0,0,0 (a reload) counts again from the new placement");
    memset(&g, 0, sizeof g);
    CHECK(wg_step(&g, 7, 0, 1, 0xFFFFFFF0u, &how) == 0 && wg_step(&g, 7, 0, 1, 2000u, &how) == 1, "the clock wrap");

    {   /* the glue: the world frame and the note tap use the gate */
        FILE *f = fopen("../native/world1_game.c", "rb");
        static char buf[1 << 21];
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[n] = 0;
        CHECK(strstr(buf, "if (bound && g_world_up) w1n_note(&N, n);") && strstr(buf, "if (up) w1n_frame(&N);") && strstr(buf, "zone_pass(playing && up);")
                  && strstr(buf, "if (playing && up) {") && strstr(buf, "rolebarrier_loaded_epoch(cs_epoch(bound_cs)), own_placed()"),
              "the glue: events, batches, zones and the passes wait for wg_step");
        f = fopen("../native/coop_session.c", "rb");
        n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        if (f) fclose(f);
        buf[n] = 0;
        CHECK(strstr(buf, "s->first_loaded && (int32_t)(now - s->first_loaded) > BARRIER_TIMEOUT_MS") && !strstr(buf, "(int32_t)(now - s->load_started) > BARRIER_TIMEOUT_MS"),
              "the session: the barrier timeout counts from the first LOAD_DONE (never while nobody has loaded)");
    }
    printf("test_wgate1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
