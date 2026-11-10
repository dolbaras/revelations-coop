/*
 * packcopy1.h - LAB ONLY: the console `packcopy <from slot> <to slot>` and its undo (pure: no Windows, no game; tests/test_packcopy1.c).
 *
 * `packcopy` writes the words +4..+0x140 of one human's bag (unit+0xFE8, cBioItemPack) into another human's bag and gives every weapon object found there the receiver as
 * owner (+0x30). The same weapon objects then sit in TWO bags. Lobby agent (lobby-must c77a623, 2026-11-07): on a reload (Continue / retry after a game over) both bags free
 * them, the manager list [0x11695C8] keeps a dead node and the next uWpBullet registration (FUN_006C96E0) crashes (exe+2c9700, wp6). The product never writes bags or owners.
 * So the copy keeps what it overwrote: `packcopy undo` writes the receiver's original words back and the weapon owners back (newest copy first) and must run before any
 * death / Continue / retry step of a lab run. Since rev1-int (lobby-must merged) probe.c registers pc1_before_reload with rolebarrier_on_restart, so the undo also runs
 * by itself on the game thread right before FUN_00720870 reloads (host Continue / game-over "Yes", the guest's own, the follow) while the restart hook is in force.
 * The first copy into a bag keeps the ORIGINAL words; a second copy into the same bag does not overwrite that save (the undo returns to the state before the first copy).
 */
#ifndef PACKCOPY1_H
#define PACKCOPY1_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PC1_FIRST 4u
#define PC1_END 0x144u
#define PC1_NW ((PC1_END - PC1_FIRST) / 4u)   /* 80 words */
#define PC1_OWNER_OFF 0x30u
#define PC1_MAX 4                           /* bags with a pending undo */

typedef struct {
    void *u;
    int (*rd)(void *u, uint32_t addr, uint32_t *v);
    int (*wr)(void *u, uint32_t addr, uint32_t v);
    int (*is_weapon)(void *u, uint32_t obj);   /* uWp*, not uWpUnarmed / uWpGndHand / uWpGndPulse */
    int (*bag_of)(void *u, uint32_t unit, uint32_t *bag);   /* the unit's bag now (unit+0xFE8); 0 = the unit is gone */
} pc1_mem;

typedef struct {
    int used;
    uint32_t bag, unit;                     /* the receiving bag and its unit */
    uint32_t words[PC1_NW];                 /* its words before the first copy */
    int nown;
    uint32_t own_obj[PC1_NW], own_old[PC1_NW];   /* weapon objects whose owner was changed, their owner before */
} pc1_rec;

typedef struct {
    pc1_rec r[PC1_MAX];
    int order[PC1_MAX], n;                  /* copy order (index into r), oldest first */
} pc1_t;

static inline void pc1_init(pc1_t *s) { memset(s, 0, sizeof *s); }
static inline int pc1_pending(const pc1_t *s) { return s->n; }

static inline pc1_rec *pc1_find(pc1_t *s, uint32_t bag)
{
    for (int i = 0; i < PC1_MAX; i++)
        if (s->r[i].used && s->r[i].bag == bag) return &s->r[i];
    return NULL;
}

/* the copy. Returns 1 (words / owners written counted), 0 = refused (bad bags, no free undo record, the source unreadable) - then nothing is written */
static inline int pc1_copy(pc1_t *s, const pc1_mem *m, uint32_t from_bag, uint32_t to_bag, uint32_t to_unit, int *words, int *owners)
{
    uint32_t src[PC1_NW], dst[PC1_NW];
    *words = 0, *owners = 0;
    if (!from_bag || !to_bag || from_bag == to_bag) return 0;
    for (unsigned i = 0; i < PC1_NW; i++)
        if (!m->rd(m->u, from_bag + PC1_FIRST + 4u * i, &src[i]) || !m->rd(m->u, to_bag + PC1_FIRST + 4u * i, &dst[i])) return 0;
    pc1_rec *r = pc1_find(s, to_bag);
    if (!r) {
        int k;
        for (k = 0; k < PC1_MAX && s->r[k].used; k++) {}
        if (k == PC1_MAX) return 0;
        r = &s->r[k];
        memset(r, 0, sizeof *r);
        r->used = 1, r->bag = to_bag, r->unit = to_unit;
        memcpy(r->words, dst, sizeof dst);
        s->order[s->n++] = k;
    }
    for (unsigned i = 0; i < PC1_NW; i++) {
        uint32_t v = src[i];
        if (dst[i] != v && m->wr(m->u, to_bag + PC1_FIRST + 4u * i, v)) (*words)++;
        if (v > 0x10000u && m->is_weapon(m->u, v)) {
            uint32_t old = 0;
            if (!m->rd(m->u, v + PC1_OWNER_OFF, &old)) continue;
            int seen = 0;
            for (int j = 0; j < r->nown; j++) seen |= r->own_obj[j] == v;
            if (!seen && r->nown < (int)PC1_NW) r->own_obj[r->nown] = v, r->own_old[r->nown] = old, r->nown++;
            if (m->wr(m->u, v + PC1_OWNER_OFF, to_unit)) (*owners)++;
        }
    }
    return 1;
}

/* undo every pending copy, newest first: the weapon owners, then the receiver's words. Returns the number of bags put back; *stale = records dropped because the
 * receiving unit is gone or has another bag (a load made new units: its old memory is never written) */
static inline int pc1_undo(pc1_t *s, const pc1_mem *m, int *words, int *owners, int *stale)
{
    int bags = 0;
    *words = 0, *owners = 0, *stale = 0;
    while (s->n > 0) {
        pc1_rec *r = &s->r[s->order[--s->n]];
        uint32_t now_bag = 0;
        if (!m->bag_of(m->u, r->unit, &now_bag) || now_bag != r->bag) {
            r->used = 0;
            (*stale)++;
            continue;
        }
        for (int j = r->nown - 1; j >= 0; j--)
            if (m->wr(m->u, r->own_obj[j] + PC1_OWNER_OFF, r->own_old[j])) (*owners)++;
        for (unsigned i = 0; i < PC1_NW; i++) {
            uint32_t cur = 0;
            if (m->rd(m->u, r->bag + PC1_FIRST + 4u * i, &cur) && cur != r->words[i] && m->wr(m->u, r->bag + PC1_FIRST + 4u * i, r->words[i])) (*words)++;
        }
        r->used = 0;
        bags++;
    }
    return bags;
}

/* a level load made new units / bags: the saved words belong to freed bags; forget them (never write into freed memory) */
static inline void pc1_forget(pc1_t *s) { pc1_init(s); }

/* the restart callback's body (probe.c, rolebarrier_on_restart): right before the engine reloads, every pending copy is undone, so the teardown never meets a weapon
 * object in two bags (wp6 exe+2c9700). Returns the bags put back and fills out with the log line; -1 = nothing pending (nothing written, out untouched). */
static inline int pc1_before_reload(pc1_t *s, const pc1_mem *m, char *out, int size)
{
    if (!pc1_pending(s)) return -1;
    int words = 0, owners = 0, stale = 0, bags = pc1_undo(s, m, &words, &owners, &stale);
    if (out && size > 0)
        snprintf(out, (size_t)size, "before the reload: packcopy undo by itself: %d bag(s) put back (%d words, %d weapon owners), %d record(s) dropped (unit gone / other bag)", bags,
                 words, owners, stale);
    return bags;
}

#endif
