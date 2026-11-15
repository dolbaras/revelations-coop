/*
 * objsync1.h - sync022 (notes\net-sync-max.md row W1): a lever the partner pulls moves on this PC too. Pure (no Windows, no engine): the wire, the sender's
 * change detection and the receiver's decision; objsync1_game.inc runs them, tests/test_objsync1.c drives them on a model of the lever's state machine.
 *
 * FACTS (Ghidra work\ghidra-sync022, <lab>\re1q\s22\o1.txt / o2.txt / o12.txt):
 *   - uLeverBase (uOMCommonLeverWall 0x80061900, uOMCommonLeverFloor 0x80061901) installs the state table 0x11303DC (uLeverBase_vf34 0x6450B0 -> FUN_0049CEE0):
 *     0 OffWait, 1 OffMove, 2 OnWait, 3 OnMove, 4 EndWait. Its manager is unit+0xE80: +0x14 the current state object, +0x3C the current index.
 *   - the wait states hand the operator to the move: OffWait vf9 0x643350 -> FUN_0049CD20(1, own slot or -1); OnWait vf9 0x643930 -> (3, own slot or -1); the
 *     move's enter 0x6439F0 keeps it at state+0x10. -1 = "nobody": the stock code itself uses it for a flag-driven move (the wait state's vf8 sets +0x10 = 1
 *     when the lever's flag changed), for the auto-return of types 0 / 3 (the move's end, 0x643B40 / 0x643550 case 4) and from EndWait (vf9 0x643EA0).
 *   - with -1 NO player is touched: 0x6439F0 `cmp eax,-1; je` around the bank binding of the operator, 0x644D60 positions the operator only `if (param != -1)`,
 *     0x644FB0 moves him only `if (+0x1104 != -1)`, the move's update (0x643B40 / 0x643550) asks / waits for his PlayerLever*State only `if (+0x10 != -1)`,
 *     0x643AC0 takes `operator == 0` as done. With an operator slot every one of these runs on that slot's unit: so a replay with the PARTNER's slot would run
 *     the interaction on the puppet - never done here.
 *   - the move's end (case 4) sets the lever's own scenario flag FUN_00645540(+0xF18, value) and goes to the next state by the lever type (+0x110C):
 *     OffMove -> OnWait (2), or EndWait (type 2), or OnMove(-1) (type 0: auto-return); OnMove -> OffWait (0), or EndWait (type 5), or OffMove(-1) (type 3).
 *   - the deferred request by index FUN_0049C790(mgr; index, arg, NULL) (+0x48 = 1, +0x4C index, +0x50 arg) is applied by the manager's own tick
 *     (FUN_0049CDE0 -> FUN_0049CD80(state, arg, args) -> the enter) on the object's own thread: the same entry FUN_0049CD20 uses, one frame later.
 * THE RULE: the PC whose own human starts a move (an index change to 1 or 3 with state+0x10 == the own slot) announces {key4, class, from, to}; the other PC,
 * when its copy is still in `from`, requests `to` with operator -1: its copy plays its own pull and its own end (pose, type rule, its flag). A move with any
 * other operator (-1: our replay, the flag, the auto-return) is never announced: no echo. A copy elsewhere is left alone (its own human / its scripts got there).
 */
#ifndef OBJSYNC1_H
#define OBJSYNC1_H

#include <stdint.h>
#include <string.h>

#define OS_CLS_LEVER_WALL 0x80061900u
#define OS_CLS_LEVER_FLOOR 0x80061901u
enum { OS_OFFWAIT = 0, OS_OFFMOVE = 1, OS_ONWAIT = 2, OS_ONMOVE = 3, OS_ENDWAIT = 4, OS_NSTATES = 5 };
#define OS_LEN 25           /* ver u8, kind u8, seq u8, from u8, to u8, class u32, key4 u32 x4 */
#define OS_VER 1
#define OS_KIND_LEVER 1
#define OS_MAX 96
#define OS_WAIT_MS 4000u    /* an announce whose copy is still running an earlier replayed move waits this long */

static inline int os_lever_class(uint32_t cls) { return cls == OS_CLS_LEVER_WALL || cls == OS_CLS_LEVER_FLOOR; }
static inline int os_is_move(int idx) { return idx == OS_OFFMOVE || idx == OS_ONMOVE; }
static inline int os_wait_of(int move) { return move == OS_OFFMOVE ? OS_OFFWAIT : move == OS_ONMOVE ? OS_ONWAIT : -1; }

typedef struct { uint8_t kind, seq, from, to; uint32_t cls, k4[4]; } os_msg;

static inline unsigned os_enc(uint8_t *b, const os_msg *m)
{
    b[0] = OS_VER, b[1] = m->kind, b[2] = m->seq, b[3] = m->from, b[4] = m->to;
    memcpy(b + 5, &m->cls, 4), memcpy(b + 9, m->k4, 16);
    return OS_LEN;
}
static inline int os_dec(const uint8_t *b, unsigned len, os_msg *m)
{
    if (!b || len != OS_LEN || b[0] != OS_VER || b[1] != OS_KIND_LEVER) return 0;
    m->kind = b[1], m->seq = b[2], m->from = b[3], m->to = b[4];
    memcpy(&m->cls, b + 5, 4), memcpy(m->k4, b + 9, 16);
    if (!os_lever_class(m->cls) || !os_is_move(m->to) || m->from != (uint8_t)os_wait_of(m->to)) return 0;
    return 1;
}

/* per lever key: the last index the sender saw (and its operator), the receiver's last applied sequence and a waiting announce */
typedef struct {
    uint32_t k4[4];
    uint8_t used, have, idx, rx_have, rx_seq, wait;
    int8_t op;
    os_msg pend;
    uint32_t pend_t;
} os_ent;
typedef struct {
    os_ent e[OS_MAX];
    uint8_t tx_seq;
    unsigned sent, seen_moves, not_own, rx, bad, old, replayed, already, other, waited, wait_expired, full;
} os_t;

static inline void os_reset(os_t *t) { memset(t, 0, sizeof *t); }
static inline os_ent *os_find(os_t *t, const uint32_t k4[4], int create)
{
    os_ent *fr = 0;
    for (unsigned i = 0; i < OS_MAX; i++) {
        if (t->e[i].used && !memcmp(t->e[i].k4, k4, 16)) return &t->e[i];
        if (!t->e[i].used && !fr) fr = &t->e[i];
    }
    if (!create) return 0;
    if (!fr) { t->full++; return 0; }
    memset(fr, 0, sizeof *fr), fr->used = 1, memcpy(fr->k4, k4, 16);
    return fr;
}

/* sender: the scan saw lever k4 in state idx (0..4) with the state's operator op (state+0x10; meaningful for the moves). 1 = announce *m now */
static inline int os_tx_step(os_t *t, const uint32_t k4[4], uint32_t cls, int idx, int op, int own, os_msg *m)
{
    if (!os_lever_class(cls) || idx < 0 || idx >= OS_NSTATES) return 0;
    os_ent *e = os_find(t, k4, 1);
    if (!e) return 0;
    int prev = e->have ? e->idx : -1;
    int changed = !e->have || prev != idx || (os_is_move(idx) && e->op != (int8_t)op);
    e->have = 1, e->idx = (uint8_t)idx, e->op = (int8_t)op;
    if (!changed || prev < 0 || !os_is_move(idx)) return 0;   /* the first sight of a lever is never a pull */
    t->seen_moves++;
    if (op != own || op < 0) {   /* our replay (-1), the flag, the auto-return, or the other human on a split screen: not this PC's human */
        t->not_own++;
        return 0;
    }
    m->kind = OS_KIND_LEVER, m->seq = ++t->tx_seq, m->from = (uint8_t)os_wait_of(idx), m->to = (uint8_t)idx, m->cls = cls;
    memcpy(m->k4, k4, 16);
    t->sent++;
    return 1;
}

/* receiver decisions */
enum { OS_SKIP_OLD = 0, OS_REPLAY = 1, OS_ALREADY = 2, OS_OTHER = 3, OS_WAIT = 4 };
/* an announce arrived (or a waiting one is tried again): cur = the copy's index (-1 = no copy / unknown state), cur_op = its operator when cur is a move */
static inline int os_rx_decide(os_t *t, const os_msg *m, int cur, int cur_op, int retry)
{
    os_ent *e = os_find(t, m->k4, 1);
    if (!e) return OS_OTHER;
    if (!retry) {
        t->rx++;
        if (e->rx_have && (int8_t)(uint8_t)(m->seq - e->rx_seq) <= 0) {
            t->old++;
            return OS_SKIP_OLD;
        }
        e->rx_have = 1, e->rx_seq = m->seq;
    }
    if (cur == (int)m->from) {
        t->replayed++;
        return OS_REPLAY;
    }
    if (cur == (int)m->to) {   /* the copy makes this move already (this PC's own human, its flag, an auto-return) */
        t->already++;
        return OS_ALREADY;
    }
    if (os_is_move(cur) && cur_op == -1) {   /* the other move of an earlier replay / auto-return is still running: try again when it ends */
        return OS_WAIT;
    }
    if (cur == OS_ENDWAIT || cur == (m->to == OS_OFFMOVE ? OS_ONWAIT : OS_OFFWAIT)) {   /* the copy is past it already */
        t->already++;
        return OS_ALREADY;
    }
    t->other++;
    return OS_OTHER;
}
/* keep a waiting announce (one per key; a newer one replaces it) */
static inline void os_wait_keep(os_t *t, const os_msg *m, uint32_t now)
{
    os_ent *e = os_find(t, m->k4, 1);
    if (!e) return;
    if (!e->wait) t->waited++;
    e->wait = 1, e->pend = *m, e->pend_t = now ? now : 1;
}
/* the next waiting announce to try again (1 = *m filled and taken off the wait; expired ones are dropped and counted) */
static inline int os_wait_next(os_t *t, uint32_t now, os_msg *m, unsigned *from_i)
{
    for (unsigned i = from_i ? *from_i : 0; i < OS_MAX; i++) {
        os_ent *e = &t->e[i];
        if (!e->used || !e->wait) continue;
        e->wait = 0;
        if ((uint32_t)(now - e->pend_t) > OS_WAIT_MS) {
            t->wait_expired++;
            continue;
        }
        *m = e->pend;
        if (from_i) *from_i = i + 1;
        return 1;
    }
    return 0;
}

#endif
