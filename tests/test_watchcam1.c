/*
 * test_watchcam1.c - rev1 live round 14, task 2 (native/watchcam1.h): the WATCHING PC's camera shows the watched human's own view.
 * A model of the 1-3 solo part: the active PC's player walks and turns his camera around Jill (the right stick turns manager 0 there); the watching PC's
 * camera manager 1 follows Jill's position but its TPS camera turns only by the watching PC's own pad (slot 0, FUN_0088E7C0), which is idle: before the
 * fix its view direction never changes (round 13: walls in three of four pictures). After the fix the watcher's picture direction is the active PC's
 * within a few degrees after the line's delay; stale input gives the stock camera back; a cut / teleport is taken at once.
 *  A  wire and its checks;  B  the model before / after the fix;  C  staleness, order, snap;  D  the sender's rule;  E  the glue in the sources.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/watchcam1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static float angdiff(float a, float b)
{
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return fabsf(d);
}

static void t_wire(void)
{
    wc_cam c = {{1.5f, -597.0f, 13710.0f}, {-210.0f, -560.0f, 13900.0f}, {0.0f, 1.0f, 0.0f}, 45.0f}, d;
    uint8_t b[WC_LEN];
    uint16_t s;
    CHECK(wc_enc(b, 7, &c) == WC_LEN, "A length 42");
    CHECK(wc_dec(b, WC_LEN, &s, &d) && s == 7 && !memcmp(&c, &d, sizeof c), "A round trip");
    CHECK(!wc_dec(b, WC_LEN - 1, &s, &d), "A a short body is refused");
    c.fov = 0.0f, wc_enc(b, 8, &c);
    CHECK(!wc_dec(b, WC_LEN, &s, &d), "A a camera without a field of view is refused");
    c.fov = 45.0f, c.eye[1] = NAN, wc_enc(b, 9, &c);
    CHECK(!wc_dec(b, WC_LEN, &s, &d), "A NaN is refused");
}

/* the model: Jill walks along x and the active player turns 2 deg a frame for 3 s; delay 3 frames; 20 Hz samples; the watcher renders 60 fps */
static float run(int fixed, float *final_err, int *stock_frames)
{
    wc_rx_t R;
    memset(&R, 0, sizeof R);
    wc_cam cur;
    int have = 0;
    float worst = 0.0f;
    uint8_t q[64][WC_LEN];   /* the line: messages in flight */
    uint32_t q_at[64];
    unsigned qn = 0;
    uint16_t seq = 0;
    uint32_t last_tx = 0;
    *stock_frames = 0;
    for (int f = 0; f < 240; f++) {
        uint32_t now = 1000u + (uint32_t)f * 16u;
        float yaw = (float)f * 2.0f * 3.14159265f / 180.0f;
        float jx = (float)f * 2.0f, jz = 13700.0f;
        wc_cam act = {{jx - 250.0f * sinf(yaw), -450.0f, jz - 250.0f * cosf(yaw)}, {jx, -560.0f, jz}, {0, 1, 0}, 45.0f};
        if (wc_tx_due(1, 0, 1, 1, last_tx, now) && qn < 64) last_tx = now, wc_enc(q[qn], ++seq, &act), q_at[qn++] = now + 48u;
        for (unsigned i = 0; i < qn;) {   /* deliver */
            if (q_at[i] <= now) {
                wc_rx(&R, q[i], WC_LEN, now);
                memmove(q[i], q[i + 1], (qn - i - 1) * WC_LEN), memmove(&q_at[i], &q_at[i + 1], (qn - i - 1) * 4), qn--;
            } else i++;
        }
        /* the watcher's manager 1: the stock TPS camera stays behind Jill at yaw 0 (its stick = the idle own pad) */
        wc_cam shown = {{jx, -450.0f, jz - 250.0f}, {jx, -560.0f, jz}, {0, 1, 0}, 45.0f};
        if (fixed && wc_fresh(&R, now)) wc_smooth(&cur, &have, &R.rx, WC_ALPHA), shown = cur;
        else (*stock_frames)++, have = 0;
        if (f >= 30) {
            float e = angdiff(wc_yaw_deg(&shown), wc_yaw_deg(&act));
            if (e > worst) worst = e;
            *final_err = e;
        }
    }
    return worst;
}

static void t_model(void)
{
    float e0 = 0, e1 = 0;
    int s0 = 0, s1 = 0;
    float w0 = run(0, &e0, &s0), w1 = run(1, &e1, &s1);
    printf("B before the fix: the watcher's view off by up to %.0f deg (last %.0f); after: up to %.1f deg (last %.1f), stock frames %d\n", (double)w0, (double)e0, (double)w1, (double)e1, s1);
    CHECK(w0 > 90.0f, "B before the fix the watcher's camera never turns with the watched player's (the round 13 pictures: walls)");
    CHECK(w1 < 25.0f && e1 < 25.0f, "B the fix: the watcher looks where the watched player looks (within the delay + smoothing)");
    CHECK(s1 <= 4, "B the fix: the stock camera only until the first sample arrived");
}

static void t_rx(void)
{
    wc_rx_t R;
    memset(&R, 0, sizeof R);
    wc_cam a = {{0, 0, 0}, {0, 0, 100}, {0, 1, 0}, 45.0f}, b2 = a;
    uint8_t m[WC_LEN];
    wc_enc(m, 5, &a);
    CHECK(wc_rx(&R, m, WC_LEN, 100) && wc_fresh(&R, 100 + WC_STALE_MS) && !wc_fresh(&R, 101 + WC_STALE_MS), "C fresh for WC_STALE_MS, then the stock camera");
    b2.eye[0] = 50.0f, wc_enc(m, 4, &b2);
    CHECK(!wc_rx(&R, m, WC_LEN, 200) && R.rx.eye[0] == 0.0f && R.old == 1, "C an older sample is dropped");
    wc_enc(m, 0x8005, &b2);
    CHECK(!wc_rx(&R, m, WC_LEN, 200), "C a sequence half a range away counts as old");
    wc_enc(m, 6, &b2);
    CHECK(wc_rx(&R, m, WC_LEN, 300) && R.rx.eye[0] == 50.0f, "C the next one is taken");
    wc_cam cur = a, far = a;
    int have = 1;
    far.eye[0] = 5000.0f, far.tgt[0] = 5000.0f;
    wc_smooth(&cur, &have, &far, WC_ALPHA);
    CHECK(cur.eye[0] == 5000.0f, "C a jump over WC_SNAP (a cut, a teleport) is taken at once");
    wc_cam near = cur;
    near.eye[0] += 100.0f;
    wc_smooth(&cur, &have, &near, WC_ALPHA);
    CHECK(cur.eye[0] > 5000.0f && cur.eye[0] < 5100.0f, "C a small move is smoothed");
}

static void t_tx(void)
{
    CHECK(wc_tx_due(1, 0, 1, 1, 0, WC_TX_MS), "D the active PC sends while the other human is off");
    CHECK(!wc_tx_due(1, 0, 0, 1, 0, 1000), "D nothing while both humans play");
    CHECK(!wc_tx_due(1, 1, 1, 1, 0, 1000), "D nothing from the watching PC (its own human is off)");
    CHECK(!wc_tx_due(1, 0, 1, 0, 0, 1000), "D nothing while a cutscene camera is on viewport 0");
    CHECK(!wc_tx_due(0, 0, 1, 1, 0, 1000), "D switch off");
    CHECK(!wc_tx_due(1, 0, 1, 1, 1000, 1000 + WC_TX_MS - 1), "D at most every WC_TX_MS");
}

/* F: the light zone. Model: the zone manager lights the zone of its target unit; a character is lit only when it stands in the lit zone. The watcher's own
 * Parker (switched off) is in zone A, the watched Jill in zone B. Before the fix the target stays Parker: Jill black for the whole watch (m13c). */
static void t_light(void)
{
    const uint32_t PARKER = 0x24c25240u, JILL = 0x24c23300u, MGR = 0x2a000000u;
    for (int fixed = 0; fixed <= 1; fixed++) {
        wl_ent e;
        memset(&e, 0, sizeof e);
        uint32_t target = PARKER;
        int lit_frames = 0, frames = 0, game_reassign = 0;
        for (int pass = 0; pass < 40; pass++) {
            int watching = pass >= 4 && pass < 30;
            if (pass == 15) target = PARKER, game_reassign = 1;   /* the game sets its own target once during the watch (e.g. a reload of the zone set) */
            uint32_t v = 0;
            if (wl_step(&e, fixed, MGR, target, watching ? JILL : 0, &v)) target = v;
            if (watching) {
                frames++;
                lit_frames += target == JILL;
            }
            if (pass == 35) {
                CHECK(target == PARKER, fixed ? "F after the watch the zone manager follows the own human again (given back)" : "F before the fix the target never changed");
            }
        }
        if (!fixed) CHECK(lit_frames == 0, "F before the fix the watched Jill is never in the lit zone (the black silhouette of m13c)");
        else CHECK(lit_frames >= frames - 1 && game_reassign, "F the fix: the watched Jill's zone is lit for the whole watch (one pass after the game's own reassignment)");
    }
    wl_ent e;
    memset(&e, 0, sizeof e);
    uint32_t v = 0;
    CHECK(wl_step(&e, 1, 0x100, 0x200, 0x300, &v) && v == 0x300, "F the watched human written");
    CHECK(!wl_step(&e, 1, 0x100, 0x300, 0x300, &v), "F no write while it holds");
    CHECK(wl_step(&e, 1, 0x101, 0x500, 0x300, &v) && v == 0x300 && e.saved == 0x500, "F a new manager (a new level): its own target is the one to give back");
    CHECK(!wl_step(&e, 1, 0x101, 0x777, 0, &v) || v == 0x500, "F the end of the watch gives back the saved target");
    memset(&e, 0, sizeof e);
    wl_step(&e, 1, 0x100, 0x200, 0x300, &v);
    CHECK(!wl_step(&e, 1, 0x100, 0x999, 0, &v), "F the end of the watch leaves a target the game changed meanwhile");
    memset(&e, 0, sizeof e);
    wl_step(&e, 1, 0x100, 0, 0x300, &v);
    CHECK(wl_step(&e, 1, 0x100, 0x300, 0, &v) && v == 0, "F a null target before the watch is given back as null");
}

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (s) s[n] = 0;
    fclose(f);
    return s;
}
static void t_sources(void)
{
    char *g = slurp("../native/watch1_game.inc"), *s = slurp("../native/story1_game.inc"), *w = slurp("../native/world1_game.c"), *p = slurp("../native/coop_proto.h");
    CHECK(g && strstr(g, "wc_orig9(self);") && strstr(g, "f[0x40 / 4 + i] = wc_cur.eye[i]") && strstr(g, "f[0x60 / 4 + i] = wc_cur.tgt[i]"), "E slot 9 runs the stock update first, then the received camera");
    CHECK(g && strstr(g, "strcmp(cn, \"uCameraManage\")") && strstr(g, "c[9] = (uint32_t)(uintptr_t)wc_wrap9;"), "E only a uCameraManage gets the copy, slot 9 wrapped");
    CHECK(g && strstr(g, "solo_u[own].off, solo_u[1 - own].off, m0 && vp0 == m0"), "E the sender's rule: own in play, the other off, the gameplay camera on viewport 0");
    CHECK(g && strstr(g, "watchcam_pass();") && strstr(g, "watchcam_tx(GetTickCount());"), "E the pass installs, the frame sends");
    CHECK(w && strstr(w, "watchcam_revert();"), "E the hooks' revert gives the stock vtable back");
    CHECK(s && strstr(s, "sub == CP_W1_CAM") && strstr(s, "watchcam_rx(b, len)"), "E op_ext routes CP_W1_CAM");
    CHECK(p && strstr(p, "#define CP_W1_CAM 24"), "E CP_W1_CAM = 24 (inside CP_W1_EXT_FIRST..LAST)");
    CHECK(g && strstr(g, "wl_off[WL_N] = {0x40, 0x44, 0x48}") && strstr(g, "watchlight_pass(0);") && strstr(g, "watchlight_pass(1);"), "E the zone managers follow the watched human; given back on the revert");
    CHECK(g && strstr(g, "!strcmp(cn, \"sGameLights\")"), "E only a zone manager class is written");
    {   /* m14c3 (LIVE): solo_pass compared the two managers' vtables; the watchcam copy on manager 1 made the watch end and restart every pass (installs 331,
         * frames written 0): the copy must count as the stock vtable there */
        char *so = slurp("../native/solo1_game.inc");
        CHECK(so && strstr(so, "rd32(m0) != watchcam_stock_vt_of(m1)") && g && strstr(g, "return wc_vt && vt == (uint32_t)(uintptr_t)wc_vt ? wc_stock_vt : vt;"),
              "E the watch keeps going while manager 1 carries the watchcam vtable copy (m14c3 flapping)");
        free(so);
    }
    free(g), free(s), free(w), free(p);
}

int main(void)
{
    t_wire();
    t_model();
    t_rx();
    t_tx();
    t_light();
    t_sources();
    printf("test_watchcam1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
