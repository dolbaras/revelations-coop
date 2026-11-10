/*
 * plug_walk.c - LAB plug-in (rev1 live round 8): walks a human of THIS PC along a route with the left stick of XInput user 0, in process, at the frame rate,
 * instead of teleports (a teleport sweeps the player's capsule through every event zone and kill volume on the straight line: ef6 / e1s3).
 * Not part of the product DLL. Build: sh native/build_plug.sh r1plug_walk8 plug_walk.c ; load: console `plug load <abs path>`; commands: `P walk ...`.
 *
 *   P walk to <unit hex> <x> <z> [<x> <z> ...] [r=<arrive radius>] [t=<seconds>] [a=0|1]   start (up to 64 points; y is ignored: stairs are walked)
 *   P walk status            one line: state, point k/n, unit position, distance, stuck count, mirror bit, yaw bias, A presses, elapsed
 *   P walk stop              neutral stick (user 0 stays synthetic and neutral: never "pad 0 off")
 *   P guard on [unit hex] | off | status   (rev1 live 10, l10a: Jill died of a bite 4 s before the harness's polled waggle) the bite guard: while the unit's
 *                            held word +0xE34 (the grab / bite states set it) is non-zero, the left stick waggles +-30000 every 90 ms in process (the ep1-probe P2
 *                            recipe freed Jill in ~3.3 s), the walk's clocks stand still; a walk started with `P walk to` takes its unit for the guard.
 *
 * Stick: the stick is camera relative. Camera forward F = horizontal direction from the eye of camera manager 0 ([[0x11E00B8]+0xCE0]+0x40) to the unit
 * (the player code uses manager 0 for the own human: stage5.md); right R = (F.z, -F.x). Stick = (s * d.R, d.F) for the wanted world direction d, rotated by
 * an adaptive yaw bias that the measured motion corrects every 0.5 s; s (mirror bit) flips when the measured motion fits the other sign better.
 * Stuck: the distance to the point did not fall by 15 units in 1.5 s -> one A tap (doors) when a=1, after 4 stuck periods at one point the walk ends
 * (state STUCK). Arrival: horizontal distance < r (default 60). Time limit t (default 120 s): state TIMEOUT. Unit gone (a load): state GONE.
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "r1.h"

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} xpad_t;

static const r1_api_t *A;
enum { W_IDLE = 0, W_WALK, W_DONE, W_STUCK, W_TIMEOUT, W_GONE, W_STOPPED };
static const char *const k_state[] = {"idle", "walking", "DONE", "STUCK", "TIMEOUT", "GONE", "stopped"};

static struct {
    int state;
    unsigned unit;
    float px[64], pz[64];
    int n, k;
    float arrive;
    DWORD t0, tmax_ms;
    int press_a;
    /* control */
    int mirror;              /* 0: s = +1, 1: s = -1 */
    float bias;              /* radians added to the wanted direction */
    float wx, wz;            /* the commanded world direction of the last window */
    float sx, sz;            /* unit position at the start of the measurement window */
    DWORD wt;                /* window start */
    float best_d;            /* best distance to point k in the stuck window */
    DWORD st;                /* stuck window start */
    int stuck, stuck_total, a_taps;
    DWORD a_until;           /* A held until */
    float last_x, last_y, last_z, last_d;
    int err_plus, err_minus; /* votes of the two mirror hypotheses */
    float fx, fz, last_err;  /* the camera forward used last, the last measured direction error (radians) */
    int cam_ok;              /* the camera eye was readable in the last frame */
    float wd0;               /* the wanted direction (radians) at the start of the measurement window */
    int k0;                  /* the point index at the start of the window */
    DWORD kt;                /* when the current point became the target */
} W;

static struct { int on, active, sign, bites; unsigned unit; DWORD since, flip, total_ms; } G;   /* the bite guard */

static int rd(unsigned a, void *o, unsigned n) { return a && A->safe_read((const void *)(uintptr_t)a, o, n); }
static unsigned rd32(unsigned a) { unsigned v = 0; return rd(a, &v, 4) ? v : 0; }
static int rdf3(unsigned a, float *x, float *y, float *z)
{
    float v[3];
    if (!rd(a, v, 12)) return 0;
    *x = v[0], *y = v[1], *z = v[2];
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2];
}

static void pad_out(int lx, int ly, WORD buttons)
{
    xpad_t g;
    memset(&g, 0, sizeof g);
    g.wButtons = buttons;
    g.sThumbLX = (SHORT)lx, g.sThumbLY = (SHORT)ly;
    A->pad_set(0, 1, &g);
}

static float wrap(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static void finish(int st)
{
    W.state = st;
    pad_out(0, 0, 0);
    A->log("plug_walk: %s at point %d/%d, unit (%.0f,%.0f,%.0f), distance %.0f, stuck periods %d, A taps %d, mirror %d, bias %.2f, %lu ms", k_state[st], W.k, W.n,
           W.last_x, W.last_y, W.last_z, W.last_d, W.stuck_total, W.a_taps, W.mirror, W.bias, (unsigned long)(GetTickCount() - W.t0));
}

static int guard_frame(DWORD now)   /* 1 = the guard owns the stick this frame */
{
    char cn[64] = "";
    if (!G.on || !G.unit) return 0;
    if (!A->object_name(G.unit, cn, sizeof cn) || strncmp(cn, "uPl", 3)) {
        if (G.active) pad_out(0, 0, 0);
        A->log("plug_walk: guard: unit %08x is gone: the guard waits for a new unit (P guard on <unit>)", G.unit);
        G.unit = 0, G.active = 0;
        return 0;
    }
    unsigned held = rd32(G.unit + 0xE34);
    if (held) {
        if (!G.active) {
            G.active = 1, G.since = now, G.flip = now, G.sign = 1, G.bites++;
            A->log("plug_walk: guard: unit %08x is HELD (+0xE34 = %u): waggling (bite %d)", G.unit, held, G.bites);
        }
        if (now - G.flip >= 90) G.sign = -G.sign, G.flip = now;
        pad_out(G.sign * 30000, 0, 0);
        return 1;
    }
    if (G.active) {
        G.active = 0, G.total_ms += now - G.since;
        A->log("plug_walk: guard: unit %08x is free again after %lu ms", G.unit, (unsigned long)(now - G.since));
        pad_out(0, 0, 0);
        if (W.state == W_WALK) {   /* the walk's clocks did not run while held */
            DWORD d = now - G.since;
            W.t0 += d, W.st = now, W.wt = now, W.kt = now, W.best_d = 1e9f;
        }
    }
    return 0;
}

static void on_frame_(void)
{
    DWORD now = GetTickCount();
    if (guard_frame(now)) return;
    if (W.state != W_WALK) return;
    float x, y, z;
    char cn[64] = "";
    if (!A->object_name(W.unit, cn, sizeof cn) || strncmp(cn, "uPl", 3) || !rdf3(W.unit + 0x40, &x, &y, &z)) { finish(W_GONE); return; }
    W.last_x = x, W.last_y = y, W.last_z = z;
    if (now - W.t0 > W.tmax_ms) { finish(W_TIMEOUT); return; }
    float dx = W.px[W.k] - x, dz = W.pz[W.k] - z, d = sqrtf(dx * dx + dz * dz);
    W.last_d = d;
    if (d < W.arrive) {
        A->log("plug_walk: point %d/%d reached at (%.0f,%.0f,%.0f) after %lu ms", W.k + 1, W.n, x, y, z, (unsigned long)(now - W.t0));
        W.k++, W.stuck = 0, W.best_d = 1e9f, W.st = now, W.kt = now;
        if (W.k >= W.n) { finish(W_DONE); return; }
        dx = W.px[W.k] - x, dz = W.pz[W.k] - z, d = sqrtf(dx * dx + dz * dz);
    }
    /* stuck window */
    if (d < W.best_d - 15.0f) W.best_d = d, W.st = now;
    else if (now - W.st > 1500) {
        W.stuck++, W.stuck_total++, W.st = now, W.best_d = d;
        A->log("plug_walk: no progress to point %d/%d for 1.5 s (distance %.0f, period %d at this point)", W.k + 1, W.n, d, W.stuck);
        if (W.press_a && W.stuck <= 3) { W.a_until = now + 200; W.a_taps++; }
        if (W.stuck >= 4) { finish(W_STUCK); return; }
    }
    /* camera forward */
    unsigned root = rd32(0x11E00B8u), mgr = root ? rd32(root + 0xCE0) : 0;
    float ex, ey, ez, fx = 0, fz = 1;
    W.cam_ok = 0;
    if (mgr && rdf3(mgr + 0x40, &ex, &ey, &ez)) {
        float cx = x - ex, cz = z - ez, cl = sqrtf(cx * cx + cz * cz);
        if (cl > 1.0f && cl < 3000.0f) fx = cx / cl, fz = cz / cl, W.cam_ok = 1;
    }
    if (!W.cam_ok && (W.fx != 0 || W.fz != 0)) fx = W.fx, fz = W.fz;   /* keep the last good camera direction */
    W.fx = fx, W.fz = fz;
    /* measurement window: correct the bias / the mirror bit from the real motion */
    if (now - W.wt >= 500) {
        float mx = x - W.sx, mz = z - W.sz, ml = sqrtf(mx * mx + mz * mz);
        float wd_now = atan2f(dx, dz);
        /* only a window in which the wanted direction stayed (no new point, no turn > 20 degrees) and that started 1 s after the point changed measures
         * the stick mapping: a corner or the turn on the spot is no evidence for the mirror bit */
        int steady = W.k == W.k0 && fabsf(wrap(wd_now - W.wd0)) < 0.35f && W.wt - W.kt >= 1000;
        if (steady && ml > 20.0f) {
            float e = wrap(atan2f(mx, mz) - W.wd0);
            if (fabsf(e) > 1.8f) W.err_plus++;
            else W.err_minus++;
            if (W.err_plus >= 3 && W.err_plus > 2 * W.err_minus) {
                W.mirror ^= 1, W.err_plus = W.err_minus = 0, W.bias = 0;
                A->log("plug_walk: the motion fits the other stick sign: mirror now %d", W.mirror);
            } else if (fabsf(e) < 1.6f) {
                W.bias = wrap(W.bias - 0.5f * e);
                if (W.bias > 1.5f) W.bias = 1.5f;
                if (W.bias < -1.5f) W.bias = -1.5f;
            }
            W.last_err = e;
        }
        W.sx = x, W.sz = z, W.wt = now, W.wd0 = wd_now, W.k0 = W.k;
    }
    /* wanted direction, rotated by the bias */
    float a = atan2f(dx, dz) + W.bias, ux = sinf(a), uz = cosf(a);
    W.wx = sinf(atan2f(dx, dz)), W.wz = cosf(atan2f(dx, dz));
    float rx = fz, rz = -fx;
    float sxv = (ux * rx + uz * rz) * (W.mirror ? -1.0f : 1.0f), syv = ux * fx + uz * fz;
    float m = sqrtf(sxv * sxv + syv * syv);
    if (m > 1e-3f) sxv /= m, syv /= m;
    pad_out((int)(sxv * 32000.0f), (int)(syv * 32000.0f), now < W.a_until ? 0x1000 : 0);
}

static int on_cmd(const char *name, const char *arg, char *out, int size)
{
    if (!strcmp(name, "guard")) {
        char sub[16] = "";
        unsigned u = 0;
        sscanf(arg, "%15s %x", sub, &u);
        if (!strcmp(sub, "on")) { G.on = 1; if (u) G.unit = u; }
        else if (!strcmp(sub, "off")) { if (G.active) pad_out(0, 0, 0); G.on = 0, G.active = 0; }
        snprintf(out, size, "guard: %s, unit %08x, held now %u, %s, bites %d (held %lu ms in all)", G.on ? "ON" : "off", G.unit, G.unit ? rd32(G.unit + 0xE34) : 0,
                 G.active ? "WAGGLING" : "idle", G.bites, (unsigned long)G.total_ms);
        return 1;
    }
    if (strcmp(name, "walk")) return 0;
    char sub[16] = "";
    int used = 0;
    sscanf(arg, "%15s %n", sub, &used);
    if (!strcmp(sub, "to")) {
        const char *p = arg + used;
        unsigned u = 0;
        int k = 0;
        if (sscanf(p, "%x %n", &u, &used) < 1 || !u) { snprintf(out, size, "walk to <unit hex> <x> <z> [...] [r=60] [t=120] [a=1]"); return 1; }
        p += used;
        memset(&W, 0, sizeof W);
        W.arrive = 60.0f, W.tmax_ms = 120000, W.press_a = 1;
        W.mirror = 1;   /* l8d live: every walk of this game ended with mirror 1 (the first seconds with 0 were lost and often ended STUCK); m=0 overrides */
        while (*p) {
            char tok[32] = "";
            if (sscanf(p, "%31s %n", tok, &used) < 1) break;
            p += used;
            if (!strncmp(tok, "r=", 2)) W.arrive = (float)atof(tok + 2);
            else if (!strncmp(tok, "t=", 2)) W.tmax_ms = (DWORD)(atof(tok + 2) * 1000.0);
            else if (!strncmp(tok, "a=", 2)) W.press_a = atoi(tok + 2);
            else if (!strncmp(tok, "m=", 2)) W.mirror = atoi(tok + 2) != 0;
            else if (k < 128) {
                float v = (float)atof(tok);
                if (k % 2 == 0) W.px[k / 2] = v;
                else W.pz[k / 2] = v;
                k++;
            }
        }
        W.n = k / 2;
        if (W.n < 1) { snprintf(out, size, "walk: no point given"); return 1; }
        W.unit = u, W.t0 = W.wt = W.st = W.kt = GetTickCount(), W.best_d = 1e9f, W.state = W_WALK;
        if (G.on && G.unit != u) G.unit = u, G.active = 0;   /* the guard follows the walked unit */
        rdf3(u + 0x40, &W.sx, &W.last_y, &W.sz);
        A->log("plug_walk: walk unit %08x through %d points (first %.0f,%.0f, last %.0f,%.0f), arrive %.0f, limit %lu s, A taps %d", u, W.n, W.px[0], W.pz[0], W.px[W.n - 1],
               W.pz[W.n - 1], W.arrive, (unsigned long)(W.tmax_ms / 1000), W.press_a);
        snprintf(out, size, "walk: started, %d points, unit %08x", W.n, u);
        return 1;
    }
    if (!strcmp(sub, "stop")) {
        if (W.state == W_WALK) finish(W_STOPPED);
        else pad_out(0, 0, 0);
        snprintf(out, size, "walk: stopped (stick neutral)");
        return 1;
    }
    snprintf(out, size, "walk: %s point %d/%d unit %08x pos (%.0f,%.0f,%.0f) distance %.0f stuck %d (total %d) A taps %d mirror %d bias %.2f cam (%.2f,%.2f)%s err %.2f elapsed %lu ms",
             k_state[W.state], W.k < W.n ? W.k + 1 : W.n, W.n, W.unit, W.last_x, W.last_y, W.last_z, W.last_d, W.stuck, W.stuck_total, W.a_taps, W.mirror, W.bias,
             W.fx, W.fz, W.cam_ok ? "" : " (kept)", W.last_err, (unsigned long)(W.state == W_IDLE ? 0 : GetTickCount() - W.t0));
    return 1;
}

static void on_exit_(void)
{
    if (W.state == W_WALK) finish(W_STOPPED);
    A->log("plug_walk: unloaded");
}

__declspec(dllexport) int plug_init(const r1_api_t *api, r1_plug_t *out)
{
    A = api;
    A->log("plug_walk: loaded (api v%u), lab walker of the own human (user 0 stick)", A->version);
    out->on_cmd = on_cmd;
    out->on_frame = on_frame_;
    out->on_exit = on_exit_;
    return 0;
}
