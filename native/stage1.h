/*
 * stage1.h - rev1 live round 12, task 5: the watching PC streams the stage parts of the human it watches. Pure (no Windows, no engine): local1_game.inc feeds
 * it, tests/test_stage1.c drives it.
 *
 * FACTS (notes/ep1-flow.md "RESUME HERE (round 12"):
 *  - the stage system's per-frame pass FUN_004177D0 asks FUN_004176B0 for the stage parts wanted now: it casts down under the OWN player (FUN_0040EEF0: the
 *    unit whose +0xE2C equals FUN_0087E1B0) and copies the 4 part codes of the floor it hits ((stage & 0xFFF) << 4 | part) to system+0x40..+0x4C (returns 1);
 *    no floor = returns 0 and the old list stays; then FUN_004177D0 loads the listed parts and unloads the others (FUN_00415A20 / FUN_00415A60).
 *  - l12a / l12b: the watching PC's own human stays in stage 256 while the watched one walks in 257 (cc_must00[3] is already in 257): 257's parts are never
 *    listed there, so the watched human stands in a black void; its sScrollVisibility finds no floor under the camera (block 0, zone -1). Scoping the own
 *    slot to the watched human (r28 `watchstage`) does not help: the cast under the watched human finds no floor either (the parts are not loaded).
 * THE RULE: each PC sends the part codes of ITS OWN human's floor while that human is active (CP_W1_STAGE, on change; the ext channel is reliable); the PC
 * that watches (its human switched off by the story) answers FUN_004176B0 with the watched human's codes instead of casting, so it streams what the active
 * PC streams. The codes belong to one level instance (reset at a new one). All-zero codes are never sent or used.
 */
#ifndef STAGE1_H
#define STAGE1_H

#include <stdint.h>
#include <string.h>

#define ST_N 4
#define ST_MSG_LEN 16

typedef struct {
    uint32_t sent[ST_N];
    int sent_any;
    uint32_t partner[ST_N];
    int partner_valid;
    unsigned tx, rx, rx_bad, used;
} st_t;

static inline void st_reset(st_t *s) { memset(s, 0, sizeof *s); }

static inline int st_codes_ok(const uint32_t c[ST_N])
{
    for (int i = 0; i < ST_N; i++)
        if (c[i] && (c[i] >> 16) == 0) return 1;   /* a real code is (stage << 4 | part) < 0x10000 */
    return 0;
}

/* this PC's own floor gave codes c (FUN_004176B0 returned 1 by itself). watching = this PC's human is switched off (its codes mean nothing to the other PC).
 * 1 = send c now (the first valid codes in this level instance, or different from the last sent) */
static inline int st_own(st_t *s, const uint32_t c[ST_N], int watching)
{
    if (watching || !st_codes_ok(c)) return 0;
    if (s->sent_any && !memcmp(s->sent, c, sizeof s->sent)) return 0;
    memcpy(s->sent, c, sizeof s->sent);
    s->sent_any = 1, s->tx++;
    return 1;
}

static inline unsigned st_enc(uint8_t *b, const uint32_t c[ST_N])
{
    memcpy(b, c, ST_MSG_LEN);
    return ST_MSG_LEN;
}

/* the other PC's codes arrived: 1 = taken */
static inline int st_rx(st_t *s, const uint8_t *b, unsigned len)
{
    uint32_t c[ST_N];
    if (len < ST_MSG_LEN) {
        s->rx_bad++;
        return 0;
    }
    memcpy(c, b, sizeof c);
    if (!st_codes_ok(c)) {
        s->rx_bad++;
        return 0;
    }
    memcpy(s->partner, c, sizeof c);
    s->partner_valid = 1, s->rx++;
    return 1;
}

/* may this pass answer FUN_004176B0 with the partner's codes? (on: the switch; watching: this PC watches the other human) */
static inline int st_use_partner(const st_t *s, int on, int watching)
{
    return on && watching && s->partner_valid && st_codes_ok(s->partner);
}

#endif
