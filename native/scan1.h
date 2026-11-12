/*
 * scan1.h - the Genesis scanner for the second player (Revelations 1): the pure part (no Windows, no engine). Design, static RE and live results:
 * notes/scan1.md. The engine glue is scan1.c; tests/test_scan1.c drives this file.
 *
 * FACTS it is built on (build 13298487, notes/scan1.md section 1):
 *   - Episode 1 gives the Genesis (uWpScanner 0x80051B30, bag word +0x3C = index 0xE) only through story FSM actions, in 1-2 to Jill alone (mChara=0), in 1-3
 *     to both; later episodes give it to one character of a pair. The action is FUN_00772500 (PcsMainCharacterAddWeapon).
 *   - the engine has ONE scanner pipeline, bound to the own slot: the scan view (FUN_006AEF60 acts only for camera manager 0), the sight
 *     (uGenesisSight_vf34 returns for a non-own owner), every target's ray (FUN_006DF380: camera manager 0, FUN_0040EEF0) and one uGUI_GadgetScanner.
 *   - the analysis rate [[0x116A84C]+0x74] is the 1-2 story counter (FSM thresholds; the GUI's story mode +0x320 = 1 in that scene: the rate stays at 100
 *     after its one herb, latch +0x321).
 *   - a hidden item's scan result: cGenesisInvisibleItemParam vf5 0x6DE990: param+0x20 = 2, FUN_005DAAB0([param+8], 1) (item+0xF19 = 1 visible).
 * RULES:
 *   G  every human unit of this PC (own, the second local human, a puppet) whose bag word +0x3C is empty gets a Genesis through the stock action as soon as
 *      any player unit holds one (one try per bag object).
 *   X  split screen: the second human uses the Genesis in the WHOLE view, as the own human (split.c's menu exchange, kind 3); one scanner: first come first served.
 *   R  the rate is shared (network mirror E2) only in 1-2; elsewhere each player has his own (split: the second human's value is swapped in for his exchange).
 *   V  network: a hidden item revealed by a scan on one PC is revealed on the other PC too (SC REVEAL, a reliable session event of kind SC_EV_KIND).
 */
#ifndef SCAN1_H
#define SCAN1_H

#include <stdint.h>
#include <string.h>

#define SC_CLASS_SCANNER 0x80051B30u
#define SC_BAG_GENESIS 0x3Cu          /* bag word of index 0xE */
#define SC_GENESIS_INDEX 0xEu
#define SC_SECTION_12 0x91010200u     /* FUN_00720320's id of 1-2 */
#define SC_EV_KIND 0x145u             /* CP_EV_USER + 0x45 (0x140 world, 0x141 role barrier, 0x142 downed, 0x143 pre-load digest, 0x144 give are taken) */
#define SC_GIVE_POLL_MS 500u
#define SC_X_QUIET 8                  /* passes without any Genesis use before the exchange ends */
#define SC_X_GUARD_MS 600000u         /* a safety net only: a player may walk around with the scanner for minutes (sc1s4: 2 min ended a scan in use) */
#define SC_X_COOLDOWN_MS 1000u        /* after an exchange ended, the next one may start */

/* ---------------------------------------------------------------- G: who gets a Genesis */
typedef struct {
    int present;      /* the unit exists */
    int in_play;      /* its update / draw bits are on (+0xC & 0xC00 == 0xC00): not switched off by the story */
    int has;          /* bag word +0x3C holds a uWpScanner */
    int empty;        /* bag word +0x3C is 0 (the stock action puts it there) */
    uint32_t bag;     /* the bag object (a new one after every load) */
} sc_human;

typedef struct {
    uint32_t tried_bag[2];   /* the bag a try was made on (one try per bag object) */
    unsigned gives, refused, skipped;
} sc_give_t;

static inline void sc_give_init(sc_give_t *g) { memset(g, 0, sizeof *g); }

/* the slots to give a Genesis to now (bit 0 = slot 0, bit 1 = slot 1). allowed[i] = slot i may be written on this PC (split: both, network: the own human and the
 * puppet, solo: none). A slot is given to once per bag object; source = any present unit that has one. */
static inline unsigned sc_give_targets_src(sc_give_t *g, const sc_human h[2], const int allowed[2], int remote_source)
{
    int source = remote_source || (h[0].present && h[0].has) || (h[1].present && h[1].has);
    unsigned out = 0;
    if (!source) return 0;
    for (int i = 0; i < 2; i++) {
        if (!allowed[i] || !h[i].present || !h[i].in_play || h[i].has || !h[i].bag) continue;
        if (!h[i].empty) { g->skipped++; continue; }   /* something else at +0x3C: never overwritten */
        if (g->tried_bag[i] == h[i].bag) continue;
        out |= 1u << i;
    }
    return out;
}
static inline unsigned sc_give_targets(sc_give_t *g, const sc_human h[2], const int allowed[2]) { return sc_give_targets_src(g, h, allowed, 0); }
/* the result of a try: done = the bag holds a Genesis afterwards */
static inline void sc_give_result(sc_give_t *g, int slot, uint32_t bag, int done)
{
    if (slot < 0 || slot > 1) return;
    g->tried_bag[slot] = bag;
    if (done) g->gives++;
    else g->refused++;
}

/* ---------------------------------------------------------------- R: is the rate one team value here */
static inline int sc_rate_shared(uint32_t section) { return section == SC_SECTION_12; }

/* ---------------------------------------------------------------- X: the split screen exchange of the second human */
typedef struct {
    int split_ok;       /* split screen active, gameplay (no menu, cutscene, solo part), no exchange running, both characters bound to the two pad slots */
    int p2_present;     /* the second human's unit exists and is in play */
    int p2_has;         /* his bag holds a Genesis */
    int p2_allowed;     /* the game's own "Genesis allowed" bit of his pad slot (PadData[slot]+8 bit 4, written in his phase by uPlayer_vf42) */
    int p2_hold;        /* his physical pad holds the Genesis button of his control scheme (LB scheme 0, RB scheme 1) */
    int p1_busy;        /* the first human uses the Genesis (his upper state, or the scanner GUI is open): one scanner, first come first served */
    uint32_t now, last_end;
    int ever_ended;     /* last_end is valid */
} sc_enter_in;

static inline int sc_want_enter(const sc_enter_in *in)
{
    if (!in->split_ok || !in->p2_present || !in->p2_has || !in->p2_allowed || !in->p2_hold || in->p1_busy) return 0;
    if (in->ever_ended && in->now - in->last_end < SC_X_COOLDOWN_MS) return 0;
    return 1;
}

typedef struct {
    int on;
    int quiet;
    uint32_t t_begin;
} sc_x_t;

typedef struct {
    int p2_present;     /* his unit still exists */
    int p2_genesis;     /* his upper state is the Genesis one, or the scanner is in his hand */
    int p2_view;        /* his scan view flag (+0xF44 bit 0x20000) */
    int gui_open;       /* the scanner GUI runs */
    int story_camera;   /* a cutscene / event camera holds viewport 0: the story takes over, the normal layout comes back at once */
} sc_leave_in;

static inline void sc_x_begin(sc_x_t *x, uint32_t now)
{
    x->on = 1, x->quiet = 0, x->t_begin = now;
}
/* 1 = the exchange should end now */
static inline int sc_x_want_leave(sc_x_t *x, const sc_leave_in *in, uint32_t now)
{
    if (!x->on) return 0;
    if (!in->p2_present || in->story_camera) return 1;
    if (now - x->t_begin > SC_X_GUARD_MS) return 1;
    if (in->p2_genesis || in->p2_view || in->gui_open) {
        x->quiet = 0;
        return 0;
    }
    return ++x->quiet >= SC_X_QUIET;
}

/* ---------------------------------------------------------------- R in split: the second human's rate and his herb latch */
typedef struct {
    int p2_rate;          /* his own analysis rate (outside 1-2) */
    int p1_rate;          /* the first human's, kept while the second one's is in the game */
    uint32_t gui;         /* the GUI instance the latches belong to (a new GUI after every load: the latches start at 0) */
    int p2_latch, p1_latch;
    int on, story;        /* swapped now; story = 1-2 (the rate is the team's: only the latch is swapped) */
} sc_gauge_t;

static inline int sc_clamp_rate(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

/* the exchange begins: what to write into the game (rate, latch); cur_* = the game's values now */
static inline void sc_gauge_begin(sc_gauge_t *g, int story, uint32_t gui, int cur_rate, int cur_latch, int *new_rate, int *new_latch)
{
    if (gui != g->gui) g->gui = gui, g->p2_latch = 0;
    g->story = story, g->on = 1;
    g->p1_rate = sc_clamp_rate(cur_rate), g->p1_latch = cur_latch ? 1 : 0;
    *new_rate = story ? cur_rate : sc_clamp_rate(g->p2_rate);
    *new_latch = g->p2_latch;
}
/* the exchange ends: keep the second human's values, give the first human his back */
static inline void sc_gauge_end(sc_gauge_t *g, uint32_t gui, int cur_rate, int cur_latch, int *new_rate, int *new_latch)
{
    if (!g->on) {
        *new_rate = cur_rate, *new_latch = cur_latch;
        return;
    }
    g->on = 0;
    if (gui == g->gui) g->p2_latch = cur_latch ? 1 : 0;
    else g->gui = gui, g->p2_latch = 0, g->p1_latch = 0;   /* a load came in between: a new GUI, both latches start again */
    if (g->story) {
        *new_rate = cur_rate;   /* the team's counter: whatever the second human scanned stays */
    } else {
        g->p2_rate = sc_clamp_rate(cur_rate);
        *new_rate = g->p1_rate;
    }
    *new_latch = g->p1_latch;
}

/* ---------------------------------------------------------------- V: the reveal of a hidden item (network) */
#define SC_M_REVEAL 1
#define SC_M_HAVE 2           /* a human on the sender's PC holds a Genesis in this level instance (rule G's source for the other PC: sc1n1 live, the guest's
                               * PC never got one in 1-2, its story did not give its copy of Jill any) */
#define SC_MSG_LEN 30
typedef struct {
    uint8_t type, epoch;
    uint32_t sect, cls;
    uint32_t k[4];          /* world1's item key: the arrange entry's +0x10, +0x14, +0x38, +0x3C */
    uint8_t from_slot;
} sc_msg;

static inline void sc_put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v, b[1] = (uint8_t)(v >> 8), b[2] = (uint8_t)(v >> 16), b[3] = (uint8_t)(v >> 24); }
static inline uint32_t sc_get32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

static inline unsigned sc_enc(uint8_t *b, unsigned cap, const sc_msg *m)
{
    if (cap < SC_MSG_LEN) return 0;
    b[0] = m->type, b[1] = m->epoch, b[2] = m->from_slot, b[3] = 0;
    sc_put32(b + 4, m->sect), sc_put32(b + 8, m->cls);
    for (int i = 0; i < 4; i++) sc_put32(b + 12 + 4 * i, m->k[i]);
    b[28] = 0x5C, b[29] = 0x31;   /* a tag: "scan1" */
    return SC_MSG_LEN;
}
static inline int sc_dec(const uint8_t *b, unsigned n, sc_msg *m)
{
    if (n != SC_MSG_LEN || (b[0] != SC_M_REVEAL && b[0] != SC_M_HAVE) || b[2] > 1 || b[28] != 0x5C || b[29] != 0x31) return 0;
    memset(m, 0, sizeof *m);
    m->type = b[0], m->epoch = b[1], m->from_slot = b[2];
    m->sect = sc_get32(b + 4), m->cls = sc_get32(b + 8);
    for (int i = 0; i < 4; i++) m->k[i] = sc_get32(b + 12 + 4 * i);
    return 1;
}
/* HAVE: each PC tells the other once per level instance that one of its humans holds a Genesis; the receiver's rule G counts that as a source while it is
 * in the same instance (its own humans then get one even when nobody on its PC has one: the story ran its add on the other PC only) */
typedef struct {
    int sent_valid, remote_valid;
    uint8_t sent_epoch, remote_epoch;
    unsigned sent, received;
} sc_have_t;
/* 1 = send HAVE now (a local source exists and this instance was not told yet) */
static inline int sc_have_should_send(const sc_have_t *h, uint8_t epoch, int local_source, int level_ok)
{
    return level_ok && local_source && !(h->sent_valid && h->sent_epoch == epoch);
}
static inline void sc_have_sent(sc_have_t *h, uint8_t epoch) { h->sent_valid = 1, h->sent_epoch = epoch, h->sent++; }
static inline void sc_have_remote(sc_have_t *h, uint8_t epoch) { h->remote_valid = 1, h->remote_epoch = epoch, h->received++; }
/* the partner's HAVE counts for this PC's level instance only */
static inline int sc_have_source(const sc_have_t *h, uint8_t my_epoch) { return h->remote_valid && h->remote_epoch == my_epoch; }
/* the session ended or restarted: forget both sides */
static inline void sc_have_reset(sc_have_t *h) { h->sent_valid = h->remote_valid = 0; }

/* a hidden item class: 0x8004xxxx (uItRandomInvisible 0x80041203, uItKeyBarn 0x80041500 ...) */
static inline int sc_item_class(uint32_t cls) { return (cls & 0xF00F0000u) == 0x80040000u; }

/* the keys revealed in this level instance (sent or applied): a duplicate is neither sent nor applied twice */
#define SC_SEEN_MAX 64
typedef struct {
    uint8_t epoch;
    int valid;
    unsigned n;
    uint32_t k[SC_SEEN_MAX][4];
} sc_seen_t;

static inline void sc_seen_epoch(sc_seen_t *s, uint8_t epoch)
{
    if (!s->valid || s->epoch != epoch) s->valid = 1, s->epoch = epoch, s->n = 0;
}
static inline int sc_seen_has(const sc_seen_t *s, const uint32_t k[4])
{
    for (unsigned i = 0; i < s->n; i++)
        if (!memcmp(s->k[i], k, 16)) return 1;
    return 0;
}
/* 1 = new (recorded), 0 = seen before */
static inline int sc_seen_add(sc_seen_t *s, const uint32_t k[4])
{
    if (sc_seen_has(s, k)) return 0;
    if (s->n < SC_SEEN_MAX) memcpy(s->k[s->n++], k, 16);
    return 1;
}

/* what the receiver does with a REVEAL */
enum { SC_RX_APPLY = 0, SC_RX_WAIT, SC_RX_DROP_EPOCH, SC_RX_DROP_SECT, SC_RX_DROP_DUP, SC_RX_DROP_MISSING, SC_RX_DROP_VISIBLE };
/* level_ok = this PC's level is in play (not loading); found = the key is in this PC's item list; visible = its +0xF19 is already 1 */
static inline int sc_rx_decide(const sc_seen_t *s, const sc_msg *m, int level_ok, uint8_t my_epoch, uint32_t my_sect, int found, int visible)
{
    if (!level_ok) return SC_RX_WAIT;
    if (m->epoch != my_epoch) return SC_RX_DROP_EPOCH;
    if (m->sect != my_sect) return SC_RX_DROP_SECT;
    if (s->valid && s->epoch == my_epoch && sc_seen_has(s, m->k)) return SC_RX_DROP_DUP;
    if (!found) return SC_RX_DROP_MISSING;
    if (visible) return SC_RX_DROP_VISIBLE;
    return SC_RX_APPLY;
}

#endif
