/*
 * descent1.h - rel027u (B2, tester: start of chapter 2, the B.O.W. inspection): nobody is left on the platform. Pure (no Windows, no engine): split.c feeds it every logic pass,
 * tests/test_descent1.c drives it.
 *
 * FACTS (notes/rel027t.md B2, lab N100): at the start of 1-2 both humans stand on a platform at y = 380; the only way down is ONE prompt, a scripted jump of 220 units to the beach
 * (y = 88). When one human takes it the other one stays on the platform for ever (tester: 60 s and more) - or until the story's scene gate brings him along much later.
 * THE RULE (split screen, both humans in play): one human DROPS (his height falls by DS_DROP or more within DS_WINDOW_MS) and lands (his height is steady for DS_LAND_MS); if the other
 * human is then still DS_ABOVE or more above him and DS_FAR or more away, he gets DS_GRACE_MS to follow on his own (he can take the same prompt / jump himself); if he has not, he is put
 * next to the human who went down (the gate's placement). One follow per drop, DS_COOLDOWN_MS between two. A cutscene / menu / downed state (not active) FREEZES the watching (the clock of the
 * timers stands still, nothing is forgotten): the jump itself starts a scene (the O'Brien meeting, lab b2e1), the follow comes when the game is playable again.
 */
#ifndef DESCENT1_H
#define DESCENT1_H

#include <stdint.h>
#include <math.h>
#include <string.h>

#define DS_DROP 180.0f         /* a fall of this much within DS_WINDOW_MS is a descent (the beach jump is 220; a staircase takes far longer, a ledge of 1 m is below) */
#define DS_WINDOW_MS 4000u
#define DS_STEADY 4.0f         /* the height moved by less than this per sample = landed */
#define DS_LAND_MS 700u        /* steady for this long = he has landed */
#define DS_ABOVE 120.0f        /* the other human is at least this much higher than the one who went down */
#define DS_FAR 300.0f          /* ... and at least this far away (3D) */
#define DS_GRACE_MS 45000u     /* the other human may follow on his own for this long after the landing (rel027z: was 5000, the owner's tester was moved while he could still take the jump himself; the clock only runs while the game is playable) */
#define DS_COOLDOWN_MS 20000u
#define DS_SAMPLE_MS 100u
#define DS_RING 48             /* samples kept per human = 4.8 s */

typedef struct {
    int on;
    /* history */
    float ring[2][DS_RING];
    int head, filled;
    uint32_t t_sample, clock, last_real;
    /* state */
    int who;                   /* the human who went down (0 / 1), -1 = none */
    uint32_t t_drop, t_steady, t_land, t_last;
    float land_y;
    unsigned drops, follows, cancels;
} ds_t;

static inline void ds_init(ds_t *s)
{
    memset(s, 0, sizeof *s);
    s->on = 1, s->who = -1;
}

static inline void ds_reset(ds_t *s)
{
    int on = s->on;
    unsigned d = s->drops, f = s->follows, c = s->cancels;
    uint32_t t = s->t_last, ck = s->clock, lr = s->last_real;
    memset(s, 0, sizeof *s);
    s->on = on, s->who = -1, s->drops = d, s->follows = f, s->cancels = c, s->t_last = t, s->clock = ck, s->last_real = lr;
}

static inline float ds_dist(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return sqrtf(x * x + y * y + z * z);
}

/* One pass. p0 / p1 = the humans' positions, active = two humans in play and no cutscene / menu / downed state, now = ms (nonzero).
 * Returns 0 = nothing, 1 + h = put human (1 - h)... see *who_down: the return is 1 when the OTHER human (not *down) is to be put next to the human *down (0 / 1). */
static inline int ds_step(ds_t *s, const float p0[3], const float p1[3], int active, uint32_t now, int *down)
{
    if (!s->on) {
        ds_reset(s);
        return 0;
    }
    if (!active) {   /* frozen: the next active pass adds no time for the gap */
        s->last_real = 0;
        return 0;
    }
    {
        uint32_t dt = s->last_real ? now - s->last_real : 0;
        if (dt > 500u) dt = 500u;
        s->last_real = now;
        s->clock += dt;
        if (!s->clock) s->clock = 1;
        now = s->clock + 1000u;
    }
    const float *p[2] = {p0, p1};
    if ((uint32_t)(now - s->t_sample) < DS_SAMPLE_MS && s->filled)
        return 0;
    /* a jump in position larger than any walk (a teleport, a load, the gate itself) restarts the watching */
    if (s->filled) {
        int last = (s->head + DS_RING - 1) % DS_RING;
        for (int h = 0; h < 2; h++)
            if (fabsf(p[h][1] - s->ring[h][last]) > 400.0f) {
                ds_reset(s);
                break;
            }
    }
    s->t_sample = now;
    int prev = (s->head + DS_RING - 1) % DS_RING;
    int had = s->filled;
    for (int h = 0; h < 2; h++)
        s->ring[h][s->head] = p[h][1];
    s->head = (s->head + 1) % DS_RING;
    if (s->filled < DS_RING) s->filled++;
    if (s->who < 0) {
        if (s->t_last && (uint32_t)(now - s->t_last) < DS_COOLDOWN_MS) return 0;
        /* a drop: the height now against the highest point of the last DS_WINDOW_MS */
        int n = s->filled < (int)(DS_WINDOW_MS / DS_SAMPLE_MS) ? s->filled : (int)(DS_WINDOW_MS / DS_SAMPLE_MS);
        for (int h = 0; h < 2; h++) {
            float hi = p[h][1];
            for (int k = 1; k <= n; k++) {
                float v = s->ring[h][(s->head + DS_RING - 1 - k) % DS_RING];
                if (v > hi) hi = v;
            }
            if (hi - p[h][1] >= DS_DROP && had) {
                s->who = h, s->t_drop = now, s->t_steady = 0, s->t_land = 0, s->drops++;
                break;
            }
        }
        (void)prev;
        return 0;
    }
    int h = s->who;
    /* landed: the height is steady */
    int last2 = (s->head + DS_RING - 2) % DS_RING;
    if (fabsf(p[h][1] - s->ring[h][last2]) <= DS_STEADY) {
        if (!s->t_steady) s->t_steady = now;
    } else
        s->t_steady = 0, s->t_land = 0;
    if (!s->t_land) {
        if (s->t_steady && (uint32_t)(now - s->t_steady) >= DS_LAND_MS) s->t_land = now, s->land_y = p[h][1];
        else if ((uint32_t)(now - s->t_drop) > 12000u) s->who = -1;   /* it never landed (a long fall / a ladder): not a descent */
        return 0;
    }
    int o = 1 - h;
    if (p[o][1] - p[h][1] < DS_ABOVE || ds_dist(p[0], p[1]) < DS_FAR) {   /* he came down (or never was far above): done */
        if (s->who >= 0) s->cancels++;
        s->who = -1;
        return 0;
    }
    if ((uint32_t)(now - s->t_land) < DS_GRACE_MS) return 0;
    s->follows++, s->t_last = now, s->who = -1;
    if (down) *down = h;
    return 1;
}

#endif
