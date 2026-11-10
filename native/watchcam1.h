/*
 * watchcam1.h - rev1 live round 14 (task 2): the WATCHING PC's camera shows what the watched human's own PC shows. Pure (no Windows, no engine): the wire,
 * the sender's rule, the smoothing of the received camera; watch1_game.inc runs it, tests/test_watchcam1.c drives it on a model of both cameras.
 *
 * FACTS (Ghidra l14 o9 / o10 / o11 / o14):
 *   - uCamera properties (uCamera_vf3 0xA2C9F0): mFarPlane +0x30, mNearPlane +0x34, mAspect +0x38, mFov +0x3C, mCameraPos +0x40, mCameraUp +0x50,
 *     mTargetPos +0x60 (vec3 + pad each);
 *   - uCameraManage's update (vtable slot 9, 0x4962B0 "uCamera_vf31") runs the current camera mode (cTPSCamera ...) and then copies its output into those
 *     fields (+0x3C <- +0x8C, +0x40 <- +0x90, +0x50 <- +0xA0, +0x60 <- +0xB0); the frame's draw reads them afterwards;
 *   - the TPS camera turns by the right stick of the CURRENT pad slot (FUN_0048C0E0 -> FUN_0088E7C0 reads sGamePad+0x870), and the manager runs as its own
 *     unit outside the characters' phases (slot 0 = the watching PC's own pad): so camera manager 1, which the watch (solo1.h) puts in viewport 0, follows
 *     the watched human's POSITION but never turns with him (round 13: walls in three of four pictures).
 * THE RULE: the PC whose human is in play while the other human is switched off by the story (the solo part) sends its gameplay camera (manager 0, only while
 * viewport 0 shows it) every WC_TX_MS (ext CP_W1_CAM); the watching PC writes the newest one, smoothed, into manager 1's mCameraPos / mTargetPos / mCameraUp /
 * mFov right after manager 1's own update (a private vtable copy whose slot 9 calls the stock one first), so the picture is the watched player's own view.
 * Stale (no camera for WC_STALE_MS) = the stock manager 1 camera again. A jump over WC_SNAP (a cut, a teleport) is taken at once.
 */
#ifndef WATCHCAM1_H
#define WATCHCAM1_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define WC_TX_MS 50u          /* 20 Hz */
#define WC_STALE_MS 700u
#define WC_SNAP 400.0f        /* units: a larger jump of the eye or the target is taken without smoothing */
#define WC_ALPHA 0.35f        /* per frame (60 fps: ~3 frames to close half the gap) */
#define WC_LEN 42             /* seq u16, eye f32 x3, target f32 x3, up f32 x3, fov f32 */

typedef struct { float eye[3], tgt[3], up[3], fov; } wc_cam;

static inline unsigned wc_enc(uint8_t *b, uint16_t seq, const wc_cam *c)
{
    b[0] = (uint8_t)(seq & 0xFF), b[1] = (uint8_t)(seq >> 8);
    memcpy(b + 2, c->eye, 12), memcpy(b + 14, c->tgt, 12), memcpy(b + 26, c->up, 12), memcpy(b + 38, &c->fov, 4);
    return WC_LEN;
}
static inline int wc_finite(const wc_cam *c)
{
    for (int i = 0; i < 3; i++)
        if (!isfinite(c->eye[i]) || !isfinite(c->tgt[i]) || !isfinite(c->up[i]) || fabsf(c->eye[i]) > 1.0e6f || fabsf(c->tgt[i]) > 1.0e6f) return 0;
    return isfinite(c->fov) && c->fov > 1.0f && c->fov < 170.0f;
}
static inline int wc_dec(const uint8_t *b, unsigned len, uint16_t *seq, wc_cam *c)
{
    if (!b || len != WC_LEN) return 0;
    *seq = (uint16_t)(b[0] | (b[1] << 8));
    memcpy(c->eye, b + 2, 12), memcpy(c->tgt, b + 14, 12), memcpy(c->up, b + 26, 12), memcpy(&c->fov, b + 38, 4);
    return wc_finite(c);
}

/* the sender: the own human in play, the other human switched off by the story, viewport 0 shows the gameplay camera (manager 0) */
static inline int wc_tx_due(int on, int own_off, int other_off, int vp0_is_mgr0, uint32_t last_ms, uint32_t now)
{
    return on && !own_off && other_off && vp0_is_mgr0 && (uint32_t)(now - last_ms) >= WC_TX_MS;
}

/* the receiver (logic thread): the newest by sequence (16-bit, wrap-aware) */
typedef struct {
    wc_cam rx;
    uint16_t seq;
    uint8_t have;
    uint32_t rx_ms;
    unsigned got, old, bad;
} wc_rx_t;
static inline int wc_rx(wc_rx_t *r, const uint8_t *b, unsigned len, uint32_t now)
{
    uint16_t s;
    wc_cam c;
    if (!wc_dec(b, len, &s, &c)) { r->bad++; return 0; }
    if (r->have && (int16_t)(s - r->seq) <= 0) { r->old++; return 0; }
    r->rx = c, r->seq = s, r->have = 1, r->rx_ms = now, r->got++;
    return 1;
}
static inline int wc_fresh(const wc_rx_t *r, uint32_t now) { return r->have && (uint32_t)(now - r->rx_ms) <= WC_STALE_MS; }

/* the watcher's per-frame write (engine thread): cur follows tgt; 1 = cur holds a camera to write */
static inline float wc_d3(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}
static inline void wc_smooth(wc_cam *cur, int *have_cur, const wc_cam *tgt, float alpha)
{
    if (!*have_cur || wc_d3(cur->eye, tgt->eye) > WC_SNAP || wc_d3(cur->tgt, tgt->tgt) > WC_SNAP) {
        *cur = *tgt, *have_cur = 1;
        return;
    }
    for (int i = 0; i < 3; i++) {
        cur->eye[i] += (tgt->eye[i] - cur->eye[i]) * alpha;
        cur->tgt[i] += (tgt->tgt[i] - cur->tgt[i]) * alpha;
        cur->up[i] += (tgt->up[i] - cur->up[i]) * alpha;
    }
    cur->fov += (tgt->fov - cur->fov) * alpha;
}

/* the viewing direction in degrees around the vertical (0 = +z): what the pictures show */
static inline float wc_yaw_deg(const wc_cam *c)
{
    return atan2f(c->tgt[0] - c->eye[0], c->tgt[2] - c->eye[2]) * 57.29578f;
}

/* ---- the watched human's LIGHT (round 13 m13c: the watched Jill a black silhouette on the watcher, the room lit)
 * FACTS (Ghidra l14 o26-o34): the zone managers (sZoneManager: sGameFilters, sGameLights, the 0x1E0 one; sGameSystemsHolder_Episode [0x116A7D4] +0x40 /
 * +0x44 / +0x48) pick their zone in their update (sZoneManager_vf6 0x8DD370 -> FUN_008DB970) from the move object of the unit at +0x24 (FUN_006EDF70 zone
 * block / unique) and load that zone's light / filter set; on the watching PC that unit is its own human, switched off by the story in another zone.
 * THE RULE: while this PC watches, each zone manager's target is the watched human; the value found before is given back when the watch ends (only if the
 * target is still the one written); a target the game changes meanwhile is kept as the one to give back (and the watched human written again). */
typedef struct { uint32_t mgr, saved, set; } wl_ent;
/* per pass: mgr = the manager now (0 = none), cur = its +0x24 now, watched = the watched human (0 = this PC does not watch). 1 = write *wv into +0x24. */
static inline int wl_step(wl_ent *e, int on, uint32_t mgr, uint32_t cur, uint32_t watched, uint32_t *wv)
{
    if (e->mgr && e->mgr != mgr) e->mgr = 0, e->set = 0;   /* another manager (a new level): nothing of the old one is ours */
    if (!on || !watched || !mgr) {
        int back = e->set && e->mgr == mgr && mgr && cur == e->set;   /* give back what the game had */
        if (back) *wv = e->saved;
        e->set = 0;
        return back;
    }
    if (cur == watched) return 0;
    if (!e->mgr) e->mgr = mgr, e->saved = cur;
    else if (cur != e->set) e->saved = cur;   /* the game changed it meanwhile: that is what goes back */
    e->set = watched;
    *wv = watched;
    return 1;
}

#endif
