/*
 * pickpend1.h - pickups whose world item leaves the list only AFTER the bag's AddItem returned (pure: no Windows, no game; tests/test_pickpend1.c).
 *
 * wp9 live (2026-11-07, host N100, rev1-int f164392): the host's human took the 9-2 herb 8/12 - `18:32:51.308 world: item 24150410 class 80040100 key 9/2/8/12:
 * added to the own bag 23f73a00 but still in the world list (E1C 40004000): not sent` - and a few minutes later the host's `world items` no longer listed it while
 * the client still did. So the world item is taken out of the list (or marked E1C bit 31) later than the AddItem call, and the old rule "sent only when gone at
 * once" never sent a real pickup. Now such an add is watched: every pass of the world frame asks whether the item unit is still in the item list; when it is gone
 * the PICKUP goes out (once); a level change drops the watch silently; an item still listed after max_ms is dropped (logged: an add that took nothing away).
 * The glue (world1_game.c) uses max_ms = 60 s: how long the engine keeps the taken item listed is not known yet; the sent line logs the age.
 */
#ifndef PICKPEND1_H
#define PICKPEND1_H

#include <stdint.h>
#include <string.h>

#define PP_N 8

typedef struct {
    int used;
    uint32_t unit, cls, t0;
    uint32_t k[4];
} pp_ent;

typedef struct {
    pp_ent e[PP_N];
    unsigned added, sent, expired, dropped_level, full, dup;
} pp_t;

static inline void pp_init(pp_t *p) { memset(p, 0, sizeof *p); }

static inline int pp_count(const pp_t *p)
{
    int n = 0;
    for (int i = 0; i < PP_N; i++) n += p->e[i].used != 0;
    return n;
}

/* watch an item the own bag got while it was still listed. 1 = watched, 0 = already watched (same unit) or the table is full */
static inline int pp_add(pp_t *p, uint32_t unit, uint32_t cls, const uint32_t k[4], uint32_t now)
{
    int f = -1;
    if (!unit) return 0;
    for (int i = 0; i < PP_N; i++) {
        if (p->e[i].used && p->e[i].unit == unit) {
            p->dup++;
            return 0;
        }
        if (!p->e[i].used && f < 0) f = i;
    }
    if (f < 0) {
        p->full++;
        return 0;
    }
    p->e[f].used = 1, p->e[f].unit = unit, p->e[f].cls = cls, p->e[f].t0 = now;
    memcpy(p->e[f].k, k, sizeof p->e[f].k);
    p->added++;
    return 1;
}

/* listed(user, unit): 1 = the item unit is still in the world item list (and not marked for deletion), 0 = gone, -1 = no level (sArrange gone: a load). */
typedef int (*pp_listed_fn)(void *user, uint32_t unit);
typedef void (*pp_ent_fn)(void *user, const pp_ent *e, uint32_t age_ms);

/* one pass: gone -> sent(e) once and forgotten; no level -> forgotten silently; still listed after max_ms -> expired(e) and forgotten. Returns the number sent. */
static inline int pp_pass(pp_t *p, uint32_t now, uint32_t max_ms, pp_listed_fn listed, pp_ent_fn sent, pp_ent_fn expired, void *user)
{
    int n = 0;
    for (int i = 0; i < PP_N; i++) {
        pp_ent *e = &p->e[i];
        if (!e->used) continue;
        uint32_t age = now - e->t0;   /* unsigned: the tick may wrap */
        int st = listed(user, e->unit);
        if (st < 0) {
            e->used = 0, p->dropped_level++;
            continue;
        }
        if (st == 0) {
            if (sent) sent(user, e, age);
            e->used = 0, p->sent++, n++;
            continue;
        }
        if (age > max_ms) {
            if (expired) expired(user, e, age);
            e->used = 0, p->expired++;
        }
    }
    return n;
}

/* a new level instance / model bind: nothing of the old level is sent */
static inline void pp_forget(pp_t *p)
{
    for (int i = 0; i < PP_N; i++) p->e[i].used = 0;
}

#endif
