/*
 * cut1.h - sync022 cutscenes (notes\net-sync-max.md section 5, the owner 2026-11-15 "Катсцены тоже надо будет синхронизировать, видел отставали"): the pre-rendered
 * movies of Rev1 start together on both PCs and are skipped only when BOTH players pressed skip (RE6 style). Pure (no Windows, no engine): the wire, the start
 * barrier and the skip vote; the glue (cut1_game.inc) runs them inside its detour of the movie player's update, tests/test_cut1.c drives them on two PCs.
 *
 * FACTS (Ghidra work\ghidra-sync022, <lab>\re1q\s22\o8.txt; the image):
 *   - the movie player's update FUN_00785B30 (fastcall, ecx = the player, called once per frame from 0x7867E3) runs a state machine at player+0x2040:
 *     2 = a delay, then FUN_00785580(name = player+0x2064 inline, stream = [player+0x20A4], ...) asks for the movie; 3 = preparing: when the movie is ready (FUN_008741B0 on
 *     player+0x2044) it starts the playback (FUN_008741D0) and goes to 4; 4 = the first frames (FUN_00874270 > 0) -> 5; 5 = playing; 6.. = the end.
 *   - state 3 has two kinds: +0x2058 set = a movie object at +0x2044 (ready = FUN_008741B0: [[+0x2048]+0x38] == 3; over = FUN_008741F0: that status 0 / 7 / 8),
 *     else the +0x20B0 kind goes to 5 at once (0x78600C).
 *   - the skip (state 5, 0x78602A..0x786342, four copies of one test: the two kinds x the game system [0x1169120] present or not): when the movie did not end by
 *     itself, a press is the Start edge of the current pad (sGamePad [0x11E0388]: [+0x870] * 0x2CC + 0x1A0 bit 3), the key FUN_0085A270(keyboard
 *     [0x11E0558]; 3, 0x12), the keyboard bits (+0x160 & 0x8002000, +0x164 & 1) or the mouse button ([0x11E057C]+0x40 & 1), not while FUN_008494D0 (a dialog of
 *     the system) holds it; then `mov [ebp+0x20C4], bl` (1, skipped; left alone when the movie ended by itself), `mov ecx, ebp; call FUN_00784210` (the stock
 *     stop; return addresses 0x786126 / 0x7861B4 / 0x7862D3 / 0x786379), +0x20AC = (state == 0). No press: FUN_00783F70 (0x786344, the paused game under the
 *     movie) and the same epilogue. On that PC only: nothing tells the partner (the cutscene lag of section 5: the PCs part company there).
 *   - no seek / step of a playing movie was found (only stop): drift is not corrected, the start barrier removes the start gap.
 * THE RULES (in a started network session):
 *   (a) START: when this PC's movie is ready (state 3, FUN_008741B0 true) it tells the partner READY{key, n} (key = hash of the movie name, n = its n-th start
 *       of that name in the level instance) and holds the playback start (the update is not run, the movie stays prepared on its first frame) until the
 *       partner's READY of the same key arrived (no older than CT_FRESH_MS before this PC's own start: n is only logged, a movie that played on one PC only must not
 *       shift every later match), or CT_START_MS passed, or the session is gone. The strip: "Waiting for %s to finish loading...".
 *   (b) SKIP: a skip press of this player while the movie plays (the game's own test decided it: the glue reads no input) is his VOTE (SKIP{key, n}); the stop
 *       that press asked for is not run (+0x20C4 back to 0, the no-skip call FUN_00783F70 instead: that frame is a normal playing frame). When both voted for the
 *       same movie, each PC skips through the stock path: the second press runs the stock stop itself; the PC that voted first runs the stock skip's own steps
 *       after its next update (+0x20C4 unless the movie ended, FUN_00784210, +0x20AC). A vote of another movie never counts.
 *   (d) END: the movies end together when they started together (both play at the movie's own speed); nothing else is held.
 */
#ifndef CUT1_H
#define CUT1_H

#include <stdint.h>
#include <string.h>

#define CT_LEN 8                 /* ver u8, kind u8, n u16, key u32 */
#define CT_VER 1
#define CT_READY 1
#define CT_SKIP 2
#define CT_START_MS 30000u       /* a partner that never reports the same movie ready (another area, an older build, a stall) does not keep this PC waiting longer */
#define CT_NPEER 16
#define CT_NOCC 16
#define CT_FRESH_MS 60000u       /* a partner's READY counts when it arrived at most this long before this PC's own movie began (or later) */
#define CT_VOTE_EARLY_MS 3000u   /* a partner's SKIP counts when it arrived at most this long before this PC's own movie began (the starts are within the latency) */

enum { CT_GO = 0, CT_HOLD = 1 };

typedef struct { uint8_t kind; uint16_t n; uint32_t key; } ct_msg;
static inline unsigned ct_enc(uint8_t *b, const ct_msg *m)
{
    b[0] = CT_VER, b[1] = m->kind, b[2] = (uint8_t)m->n, b[3] = (uint8_t)(m->n >> 8);
    memcpy(b + 4, &m->key, 4);
    return CT_LEN;
}
static inline int ct_dec(const uint8_t *b, unsigned len, ct_msg *m)
{
    if (!b || len != CT_LEN || b[0] != CT_VER || (b[1] != CT_READY && b[1] != CT_SKIP)) return 0;
    m->kind = b[1], m->n = (uint16_t)(b[2] | (b[3] << 8));
    memcpy(&m->key, b + 4, 4);
    return m->n != 0;
}
/* the key of a movie: FNV-1a of its name (the same name on both PCs: the movie id the script plays, e.g. "92_020_0") */
static inline uint32_t ct_key(const char *name)
{
    uint32_t h = 2166136261u;
    for (; name && *name; name++) h = (h ^ (uint8_t)*name) * 16777619u;
    return h ? h : 1;
}

typedef struct {
    /* this PC's movie */
    int have;                    /* a movie of this level instance is between its start and its end */
    uint32_t key;
    uint16_t n;
    int ready_sent, released, vote_sent, skip_done;
    uint32_t t_ready;
    uint32_t t_begin;
    int held_now;
    /* the partner's words (rings, with the time they arrived here) */
    struct { uint32_t key; uint16_t n; uint32_t t; } ready[CT_NPEER], vote[CT_NPEER];
    unsigned nready, nvote;
    struct { uint32_t key; uint16_t count; } occ[CT_NOCC];
    unsigned nocc;
    unsigned starts, held_frames, holds, released_peer, released_timeout, released_nosession, votes_local, votes_peer, skips, withheld, stale_words;
} ct_t;
static inline void ct_reset(ct_t *t) { memset(t, 0, sizeof *t); }

static inline int ct_peer_ready_has(const ct_t *t, uint32_t key)
{
    for (unsigned i = 0; i < CT_NPEER && i < t->nready; i++)
        if (t->ready[i].key == key && (int32_t)(t->ready[i].t - (t->t_begin - CT_FRESH_MS)) >= 0) return 1;
    return 0;
}
static inline int ct_peer_vote_has(const ct_t *t, uint32_t key)
{
    for (unsigned i = 0; i < CT_NPEER && i < t->nvote; i++)
        if (t->vote[i].key == key && (int32_t)(t->vote[i].t - (t->t_begin - CT_VOTE_EARLY_MS)) >= 0) return 1;
    return 0;
}

/* the player entered state 3 (preparing) for a movie: a new occurrence of its key (1 = this is a new start) */
static inline int ct_begin(ct_t *t, uint32_t key, uint32_t now)
{
    unsigned i;
    for (i = 0; i < t->nocc && i < CT_NOCC && t->occ[i].key != key; i++) {}
    if (i == t->nocc || i >= CT_NOCC) {
        if (t->nocc < CT_NOCC) t->occ[t->nocc].key = key, t->occ[t->nocc].count = 0, i = t->nocc++;
        else i = (t->starts % CT_NOCC), t->occ[i].key = key, t->occ[i].count = 0;
    }
    t->occ[i].count++;
    t->have = 1, t->key = key, t->n = t->occ[i].count;
    t->ready_sent = t->released = t->vote_sent = t->skip_done = t->held_now = 0, t->t_ready = 0, t->t_begin = now;
    t->starts++;
    return 1;
}
/* the movie has ended / the player left it (state 0 / another movie): this PC's movie is over */
static inline void ct_end(ct_t *t) { t->have = 0; }

/* (a) state 3 with the movie ready. session = a started network session with the partner present. *send = a READY to send now. CT_HOLD = do not run the update */
static inline int ct_ready_step(ct_t *t, int session, uint32_t now, ct_msg *send, int *do_send)
{
    *do_send = 0;
    if (!t->have || t->released) return CT_GO;
    if (!session) {
        t->released = 1, t->released_nosession++;
        return CT_GO;
    }
    if (!t->ready_sent) {
        t->ready_sent = 1, t->t_ready = now ? now : 1;
        send->kind = CT_READY, send->key = t->key, send->n = t->n, *do_send = 1;
    }
    if (ct_peer_ready_has(t, t->key)) {
        t->released = 1, t->released_peer++;
        return CT_GO;
    }
    if ((uint32_t)(now - t->t_ready) >= CT_START_MS) {
        t->released = 1, t->released_timeout++;
        return CT_GO;
    }
    if (!t->held_now) t->held_now = 1, t->holds++;
    t->held_frames++;
    return CT_HOLD;
}

/* (b) state 5: pressed = this player's skip input this frame. Returns 0 = run the update unchanged, 1 = withhold this frame (the vote is out, the partner's not yet),
 * 2 = both voted: skip now through the stock path (the glue gives the stock test a Start edge for this one update). *send = a SKIP vote to send */
static inline int ct_skip_step(ct_t *t, int session, int pressed, ct_msg *send, int *do_send)
{
    *do_send = 0;
    if (!t->have || t->skip_done) return 0;
    if (!session) return 0;   /* no partner: the stock skip as before */
    if (pressed && !t->vote_sent) {
        t->vote_sent = 1, t->votes_local++;
        send->kind = CT_SKIP, send->key = t->key, send->n = t->n, *do_send = 1;
    }
    if (t->vote_sent && ct_peer_vote_has(t, t->key)) {
        t->skip_done = 1, t->skips++;
        return 2;
    }
    if (pressed) {
        t->withheld++;
        return 1;
    }
    return 0;
}
/* the partner voted to skip a movie that is playing here, and this player has not: the strip asks him (1 = show "%s wants to skip the cutscene") */
static inline int ct_partner_waits(const ct_t *t) { return t->have && !t->skip_done && !t->vote_sent && ct_peer_vote_has(t, t->key); }
static inline int ct_self_waits(const ct_t *t) { return t->have && !t->skip_done && t->vote_sent && !ct_peer_vote_has(t, t->key); }

/* a word of the partner */
static inline void ct_rx(ct_t *t, const ct_msg *m, uint32_t now)
{
    if (m->kind == CT_READY) {
        unsigned i = t->nready % CT_NPEER;
        t->ready[i].key = m->key, t->ready[i].n = m->n, t->ready[i].t = now, t->nready++;
    } else if (m->kind == CT_SKIP) {
        unsigned i = t->nvote % CT_NPEER;
        t->vote[i].key = m->key, t->vote[i].n = m->n, t->vote[i].t = now, t->nvote++, t->votes_peer++;
    }
}
/* a new level instance: the occurrence counts and the partner's words of the old one go (both PCs reset together at the session's epoch) */
static inline void ct_epoch(ct_t *t)
{
    unsigned keep[11] = {t->starts, t->held_frames, t->holds, t->released_peer, t->released_timeout, t->released_nosession, t->votes_local, t->votes_peer, t->skips, t->withheld, t->stale_words};
    memset(t, 0, sizeof *t);
    t->starts = keep[0], t->held_frames = keep[1], t->holds = keep[2], t->released_peer = keep[3], t->released_timeout = keep[4], t->released_nosession = keep[5];
    t->votes_local = keep[6], t->votes_peer = keep[7], t->skips = keep[8], t->withheld = keep[9], t->stale_words = keep[10];
}

#endif
