/*
 * leash1.h - PARTNER GATE v1 (fix026g, knowledge\partner-gate-design.md): the soft leash between the two humans and the scene gate. Pure (no Windows, no engine): split.c feeds it
 * every logic pass, tests/test_leash1.c drives it.
 *
 * Units are the engine's (1 unit = 1 cm: the capsule offset of frs_target is 70).
 *  - level 1 (>= LS_N1, default 25 m): the hint "Wait for your partner" is shown (the strip of menu.c, refreshed every LS_HINT_MS);
 *  - level 2 (>= LS_N2, default 40 m): the stick of a human who is walking AWAY from the partner is scaled down (to LS_MIN_PERMILLE at LS_N2 + LS_RAMP), a human walking towards him or
 *    standing is never touched; nobody is teleported or killed by the leash (the fall rescue of fallres1.h remains the net under the floor).
 *  - gate: before a scene / movie / section change a far second human (>= LS_GATE_R) is put next to the first one (the glue reuses the fall rescue's placement) and the hint says so.
 * Off by switch (ls.on), in cutscenes / menus / solo parts / downed states (the glue passes active = 0).
 */
#ifndef LEASH1_H
#define LEASH1_H

#include <stdint.h>
#include <math.h>
#include <string.h>

#define LS_N1_DEFAULT 2500.0f
#define LS_N2_DEFAULT 4000.0f
#define LS_RAMP 1500.0f           /* the damping reaches its minimum this far beyond N2 */
#define LS_MIN_PERMILLE 250       /* light damping (design addendum 2: no hard leash in the series): a quarter of the speed at most, never a full stop */
#define LS_SAMPLE_MS 100u         /* movement is judged between two samples */
#define LS_AWAY_EPS 1.5f          /* units: the distance to the partner grew by more than this per sample = walking away */
#define LS_AWAY_HOLD_MS 300u      /* a human that walked away stays damped this long (no flicker when the sticks jitter) */
#define LS_HINT_MS 500u           /* the hint is renewed this often while the level holds */
#define LS_HYST 300.0f            /* the levels drop this much below their threshold */
#define LS_GATE_R_DEFAULT 800.0f  /* a scene / section start with the second human further away than this puts him next to the first one */
#define LS_GATE_TTL_MS 2000u      /* a gate request lives this long (the units may be recreated by the load meanwhile) */

typedef struct {
    int on;                    /* the switch (console `split leash`, ini) */
    float n1, n2, gate_r;
    /* state */
    int level;                 /* 0 / 1 / 2 */
    uint32_t t_sample, t_hint, away_until[2];
    float p_prev[2][3];
    int have_prev;
    int scale[2];              /* permille for human 0 / 1 now (1000 = untouched) */
    float dist;
    /* gate */
    uint32_t gate_at;          /* request time, 0 = none */
    /* statistics */
    unsigned hints, damped_ms, gates, gate_moves;
} ls_t;

static inline void ls_init(ls_t *s)
{
    memset(s, 0, sizeof *s);
    s->on = 1, s->n1 = LS_N1_DEFAULT, s->n2 = LS_N2_DEFAULT, s->gate_r = LS_GATE_R_DEFAULT;
    s->scale[0] = s->scale[1] = 1000;
}

static inline float ls_dist(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

/* forget the movement history (a load, a teleport: the next sample is a fresh start) */
static inline void ls_reset(ls_t *s)
{
    s->level = 0, s->have_prev = 0, s->scale[0] = s->scale[1] = 1000, s->away_until[0] = s->away_until[1] = 0;
}

/* The damping factor (permille) for a distance: 1000 below N2, falling linearly to LS_MIN_PERMILLE at N2 + LS_RAMP. */
static inline int ls_factor(const ls_t *s, float d)
{
    if (d <= s->n2) return 1000;
    float k = (d - s->n2) / LS_RAMP;
    if (k >= 1.0f) return LS_MIN_PERMILLE;
    return (int)(1000.0f - k * (1000 - LS_MIN_PERMILLE));
}

/* One pass. p0 / p1 = the two humans' positions, active = both are two humans in play (not a cutscene, a menu, a solo part, a downed state), now = ms.
 * Returns the hint wanted NOW (1 = the glue shows / renews the strip). s->scale[h] is the permille to apply to human h's left stick. */
static inline int ls_step(ls_t *s, const float p0[3], const float p1[3], int active, uint32_t now)
{
    s->scale[0] = s->scale[1] = 1000;
    if (!s->on || !active) {
        ls_reset(s);
        return 0;
    }
    float d = ls_dist(p0, p1);
    s->dist = d;
    /* levels with hysteresis */
    if (s->level == 0 && d >= s->n1) s->level = 1;
    if (s->level >= 1 && d >= s->n2) s->level = 2;
    if (s->level == 2 && d < s->n2 - LS_HYST) s->level = 1;
    if (s->level >= 1 && d < s->n1 - LS_HYST) s->level = 0;
    if (!s->have_prev) {
        memcpy(s->p_prev[0], p0, 12), memcpy(s->p_prev[1], p1, 12);
        s->t_sample = now, s->have_prev = 1;
    } else if ((uint32_t)(now - s->t_sample) >= LS_SAMPLE_MS) {
        /* human h walked away when his distance to the partner's OLD position grew */
        const float *cur[2] = {p0, p1};
        for (int h = 0; h < 2; h++) {
            float before = ls_dist(s->p_prev[h], s->p_prev[1 - h]), after = ls_dist(cur[h], s->p_prev[1 - h]);
            if (after > before + LS_AWAY_EPS) s->away_until[h] = now + LS_AWAY_HOLD_MS;
        }
        memcpy(s->p_prev[0], p0, 12), memcpy(s->p_prev[1], p1, 12);
        s->t_sample = now;
    }
    if (s->level == 2) {
        int f = ls_factor(s, d);
        for (int h = 0; h < 2; h++)
            if ((int32_t)(s->away_until[h] - now) > 0) s->scale[h] = f;
        if (s->scale[0] < 1000 || s->scale[1] < 1000) s->damped_ms++;
    }
    if (s->level >= 1 && (!s->t_hint || (uint32_t)(now - s->t_hint) >= LS_HINT_MS)) {
        s->t_hint = now | 1u, s->hints++;
        return 1;
    }
    return 0;
}

/* the gate: a scene / movie / section change is about to start */
static inline void ls_gate_request(ls_t *s, uint32_t now)
{
    if (s->on) s->gate_at = now | 1u, s->gates++;
}

/* Called every pass while a request is pending: 1 = put the second human next to the first one NOW (far enough and the request is fresh), the request is consumed either way once decided. */
static inline int ls_gate_step(ls_t *s, const float p0[3], const float p1[3], uint32_t now)
{
    if (!s->gate_at) return 0;
    if ((uint32_t)(now - s->gate_at) > LS_GATE_TTL_MS) {
        s->gate_at = 0;
        return 0;
    }
    s->gate_at = 0;
    if (ls_dist(p0, p1) < s->gate_r) return 0;
    s->gate_moves++;
    return 1;
}

#endif
