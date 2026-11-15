/*
 * Offline test of native/objsync1.h (sync022 row W1): a lever the partner pulls moves on this PC too, through a replay with operator -1.
 * The lever is a model of uLeverBase's state machine as decompiled (notes\net-sync-max.md, objsync1.h FACTS):
 *   - 0 OffWait (enter: flag 0) / 2 OnWait (enter: flag 1): the own human's press -> the move with the own slot; the lever's flag changed by someone else
 *     -> the move with -1 (the stock flag-driven path, the Episode 1 story mirror);
 *   - 1 OffMove / 3 OnMove: an operator slot != -1 asks that slot's human for PlayerLever*State and positions him (counted: never allowed on the replaying PC);
 *     the move takes D ticks; its end by the type (+0x110C): OffMove -> type 2 EndWait (flag 1), type 0 OnMove(-1) (flag 1, auto-return), else OnWait;
 *     OnMove -> type 5 EndWait (flag 0), type 3 OffMove(-1) (flag 0), else OffWait;
 *   - the deferred request by index is applied at the copy's next tick (FUN_0049CDE0).
 * Two PCs (own slots 0 and 1), the glue of objsync1_game.inc in a few lines, a line with a delay. The model before the fix (no mirror) must fail.
 *   sh tests/build.sh -> tests/build/test_objsync1.exe
 */
#include <stdio.h>
#include <string.h>

#include "../native/objsync1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum { D = 6 };
typedef struct {
    int idx, op, type, flag, t, req, req_idx, req_arg;
    int asked_player[2];          /* PlayerLever*State asked on the human of slot k (counts) */
} Lever;
typedef struct {
    int own;
    Lever L;
    os_t os;
    int press;                    /* this PC's human presses the lever now */
    int ext_flag;                 /* -1, or a flag value written from outside (the story mirror) */
    unsigned sent, replays;
} PC;

static void enter(Lever *l, int idx, int arg)
{
    l->idx = idx, l->op = arg, l->t = 0;
    if (idx == OS_OFFWAIT) l->flag = 0;
    if (idx == OS_ONWAIT) l->flag = 1;
    if (os_is_move(idx) && arg >= 0 && arg < 2) l->asked_player[arg]++;   /* 0x643B40 / 0x643550 case 1 */
}
static void lever_tick(PC *p)
{
    Lever *l = &p->L;
    if (p->ext_flag >= 0) l->flag = p->ext_flag, p->ext_flag = -1;
    if (l->req) {   /* the deferred request: the copy's own tick */
        l->req = 0;
        enter(l, l->req_idx, l->req_arg);
        return;
    }
    switch (l->idx) {
    case OS_OFFWAIT:
        if (p->press) enter(l, OS_OFFMOVE, p->own);
        else if (l->flag) enter(l, OS_OFFMOVE, -1);
        break;
    case OS_ONWAIT:
        if (p->press) enter(l, OS_ONMOVE, p->own);
        else if (!l->flag) enter(l, OS_ONMOVE, -1);
        break;
    case OS_OFFMOVE:
        if (++l->t >= D) {
            if (l->type == 2) l->flag = 1, enter(l, OS_ENDWAIT, 0);
            else if (l->type == 0) l->flag = 1, enter(l, OS_ONMOVE, -1);
            else enter(l, OS_ONWAIT, 0);
        }
        break;
    case OS_ONMOVE:
        if (++l->t >= D) {
            if (l->type == 5) l->flag = 0, enter(l, OS_ENDWAIT, 0);
            else if (l->type == 3) l->flag = 0, enter(l, OS_OFFMOVE, -1);
            else enter(l, OS_OFFWAIT, 0);
        }
        break;
    default:
        break;
    }
    p->press = 0;
}

/* the line: messages delivered after a delay */
#define QN 64
typedef struct { uint8_t b[OS_LEN]; int at, to; } Q;
static Q q[QN];
static int nq;
static const uint32_t K[4] = {9, 2, 4, 7};
static int mirror_on = 1;

static void glue_scan(PC *p, int now, int to_pc)
{
    os_msg m;
    int op = os_is_move(p->L.idx) ? p->L.op : -1;
    if (os_tx_step(&p->os, K, OS_CLS_LEVER_WALL, p->L.idx, op, p->own, &m) && mirror_on && nq < QN) {
        os_enc(q[nq].b, &m), q[nq].at = now + 3, q[nq].to = to_pc, nq++;
        p->sent++;
    }
}
static void glue_rx(PC *p, const uint8_t *b, int retry_now)
{
    os_msg m;
    if (!os_dec(b, OS_LEN, &m)) return;
    int op = os_is_move(p->L.idx) ? p->L.op : -1;
    int d = os_rx_decide(&p->os, &m, p->L.idx, op, 0);
    if (d == OS_REPLAY) p->L.req = 1, p->L.req_idx = m.to, p->L.req_arg = -1, p->replays++;
    else if (d == OS_WAIT) os_wait_keep(&p->os, &m, (uint32_t)retry_now);
}
static void glue_retry(PC *p, int now)
{
    os_msg m;
    unsigned it = 0;
    while (os_wait_next(&p->os, (uint32_t)now, &m, &it)) {
        int op = os_is_move(p->L.idx) ? p->L.op : -1;
        int d = os_rx_decide(&p->os, &m, p->L.idx, op, 1);
        if (d == OS_REPLAY) p->L.req = 1, p->L.req_idx = m.to, p->L.req_arg = -1, p->replays++;
        else if (d == OS_WAIT) os_wait_keep(&p->os, &m, (uint32_t)now);
    }
}

static PC A, B;
static int now;
static void setup(int type, int start_idx)
{
    memset(&A, 0, sizeof A), memset(&B, 0, sizeof B);
    A.own = 0, B.own = 1, A.ext_flag = B.ext_flag = -1;
    os_reset(&A.os), os_reset(&B.os);
    A.L.type = B.L.type = type;
    enter(&A.L, start_idx, 0), enter(&B.L, start_idx, 0);
    A.L.asked_player[0] = B.L.asked_player[0] = 0;
    nq = 0, now = 0;
}
static void step(int n)
{
    for (int i = 0; i < n; i++) {
        now++;
        for (int k = 0; k < nq;) {
            if (q[k].at <= now) {
                glue_rx(q[k].to ? &B : &A, q[k].b, now);
                q[k] = q[--nq];
            } else k++;
        }
        glue_retry(&A, now), glue_retry(&B, now);
        lever_tick(&A), lever_tick(&B);
        glue_scan(&A, now, 1), glue_scan(&B, now, 0);
    }
}
static int same(void) { return A.L.idx == B.L.idx && A.L.flag == B.L.flag; }

static int scenarios(void)
{
    int f0 = fails;
    /* 1. type 1 (on / off): the host's human pulls, then the guest's human pulls back */
    setup(1, OS_OFFWAIT);
    step(5);
    A.press = 1;
    step(30);
    CHECK(A.L.idx == OS_ONWAIT && A.L.flag == 1);
    CHECK(B.L.idx == OS_ONWAIT && B.L.flag == 1);             /* the copy moved and set its own flag (its OnWait enter) */
    CHECK(A.L.asked_player[0] == 1 && A.L.asked_player[1] == 0);
    CHECK(B.L.asked_player[0] == 0 && B.L.asked_player[1] == 0);   /* the replay asked NO human on the guest's PC (operator -1) */
    CHECK(A.sent == 1 && B.sent == 0);                        /* no echo of the replay */
    B.press = 1;
    step(30);
    CHECK(same() && B.L.idx == OS_OFFWAIT && B.L.flag == 0);
    CHECK(B.L.asked_player[1] == 1 && A.L.asked_player[1] == 0 && A.sent == 1 && B.sent == 1);
    /* 2. type 0 (auto-return): both end back in OffWait, the auto-return is never announced */
    setup(0, OS_OFFWAIT);
    step(3);
    A.press = 1;
    step(60);
    CHECK(same() && A.L.idx == OS_OFFWAIT && A.sent == 1 && B.sent == 0 && B.L.asked_player[0] + B.L.asked_player[1] == 0);
    /* 3. type 2 (one shot): EndWait with the flag on both */
    setup(2, OS_OFFWAIT);
    step(3);
    B.press = 1;
    step(40);
    CHECK(same() && A.L.idx == OS_ENDWAIT && A.L.flag == 1 && A.L.asked_player[0] + A.L.asked_player[1] == 0);
    /* 4. both humans pull at the same moment: each plays its own pull, the announces find "already there" */
    setup(1, OS_OFFWAIT);
    step(2);
    A.press = 1, B.press = 1;
    step(40);
    CHECK(same() && A.L.idx == OS_ONWAIT && A.os.already + B.os.already == 2 && A.replays + B.replays == 0);
    /* 5. the flag arrives first (the Episode 1 story mirror): the copy moves by itself, the announce finds it moving */
    setup(1, OS_OFFWAIT);
    step(2);
    A.press = 1;
    B.ext_flag = 1;
    step(40);
    CHECK(same() && B.L.idx == OS_ONWAIT && B.replays == 0 && B.os.already == 1);
    /* 6. a second pull while the guest's replay still runs: the announce waits and is replayed when the copy is back in a wait state */
    setup(1, OS_OFFWAIT);
    step(2);
    A.press = 1;
    step(D + 2);                                               /* the host's move ended, the guest's replay (3 ticks later) still runs */
    CHECK(A.L.idx == OS_ONWAIT && os_is_move(B.L.idx));
    A.press = 1;
    step(2);
    step(40);
    CHECK(same() && A.L.idx == OS_OFFWAIT && B.replays == 2);
    /* 7. a repeated announce (same sequence) is dropped */
    {
        os_t r;
        os_reset(&r);
        os_msg m = {OS_KIND_LEVER, 5, OS_OFFWAIT, OS_OFFMOVE, OS_CLS_LEVER_WALL, {1, 2, 3, 4}};
        CHECK(os_rx_decide(&r, &m, OS_OFFWAIT, -1, 0) == OS_REPLAY);
        CHECK(os_rx_decide(&r, &m, OS_OFFWAIT, -1, 0) == OS_SKIP_OLD);
        m.seq = 6;
        CHECK(os_rx_decide(&r, &m, OS_ONMOVE, 1, 0) == OS_OTHER);       /* this PC's human pulls the other way: left alone */
        m.seq = 7;
        CHECK(os_rx_decide(&r, &m, OS_ENDWAIT, -1, 0) == OS_ALREADY);
    }
    return fails - f0;
}

/* the build facts the glue relies on, read from the memory image of the Steam build when it is there (work\image\rerev-image.bin, VA = 0x400000 + offset) */
static void image_facts(void)
{
    FILE *f = fopen("../../image/rerev-image.bin", "rb");
    if (!f) {
        printf("image ../../image/rerev-image.bin not found: build facts not checked\n");
        return;
    }
    static uint8_t img[0x1200000];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
#define AT(va) (img + ((va) - 0x400000u))
    static const uint8_t req[] = {0x81, 0xEC, 0x78, 0x03, 0x00, 0x00, 0x56, 0x8B, 0xF1, 0x80, 0x7E, 0x2D, 0x01, 0x75, 0x0C};
    CHECK(n > 0xD40000 && !memcmp(AT(0x49C790u), req, sizeof req));                 /* FUN_0049C790, the request by index the glue calls */
    static const uint32_t tab[5] = {0x01176AD4u, 0x01176B14u, 0x01176B54u, 0x01176BA0u, 0x01176AF4u};
    CHECK(!memcmp(AT(0x11303DCu), tab, sizeof tab));                                  /* the lever's state table: OffWait, OffMove, OnWait, OnMove, EndWait */
    static const uint8_t keep[] = {0x89, 0x6F, 0x10};                                 /* the move's enter: mov [edi+0x10], ebp (the operator) */
    static const uint8_t guard_enter[] = {0x83, 0xF8, 0xFF, 0x74, 0x39};              /* ... cmp eax,-1; je: no bank binding of an operator -1 */
    CHECK(!memcmp(AT(0x643A00u), keep, 3) && !memcmp(AT(0x643A6Du), guard_enter, 5));
    static const uint8_t guard_upd[] = {0x83, 0x7F, 0x10, 0xFF, 0x74, 0x31};          /* OnMove update case 1: cmp [edi+0x10],-1; je (no PlayerLeverOnMoveState) */
    CHECK(!memcmp(AT(0x643B73u), guard_upd, sizeof guard_upd));
    uint32_t vw, vf;
    memcpy(&vw, AT(0x103C140u + 0x10), 4), memcpy(&vf, AT(0x103BE10u + 0x10), 4);
    static const uint8_t dw[] = {0xB8, 0x14, 0x6C, 0x17, 0x01, 0xC3}, dfl[] = {0xB8, 0xE4, 0x6B, 0x17, 0x01, 0xC3};
    CHECK(vw == 0x644720u && vf == 0x6443A0u && !memcmp(AT(vw), dw, 6) && !memcmp(AT(vf), dfl, 6));   /* the class vtables (getDTI -> uOMCommonLeverWall / Floor) */
    printf("image ../../image/rerev-image.bin: the request call, the lever state table, the operator -1 guards and the two class vtables checked\n");
#undef AT
}

int main(void)
{
    image_facts();
    /* the wire */
    os_msg m = {OS_KIND_LEVER, 3, OS_ONWAIT, OS_ONMOVE, OS_CLS_LEVER_FLOOR, {0x10, 0x20, 7, 9}}, d;
    uint8_t b[OS_LEN];
    CHECK(os_enc(b, &m) == OS_LEN && os_dec(b, OS_LEN, &d) && d.seq == 3 && d.from == OS_ONWAIT && d.to == OS_ONMOVE && d.cls == OS_CLS_LEVER_FLOOR && d.k4[3] == 9);
    CHECK(!os_dec(b, OS_LEN - 1, &d));
    b[4] = OS_OFFWAIT;
    CHECK(!os_dec(b, OS_LEN, &d));                             /* only moves are announced */
    os_enc(b, &m), b[3] = OS_OFFWAIT;
    CHECK(!os_dec(b, OS_LEN, &d));                             /* the move must start from its own wait state */
    os_enc(b, &m), memcpy(b + 5, "\x00\x12\x06\x80", 4);
    CHECK(!os_dec(b, OS_LEN, &d));                             /* another class (0x80061200 uLockerDoor) */
    /* the sender: the first sight is no pull, a move by -1 or by another slot is no announce */
    os_t t;
    os_reset(&t);
    CHECK(!os_tx_step(&t, K, OS_CLS_LEVER_WALL, OS_OFFMOVE, 0, 0, &d));   /* first sight while moving: not announced */
    CHECK(!os_tx_step(&t, K, OS_CLS_LEVER_WALL, OS_ONWAIT, -1, 0, &d));
    CHECK(!os_tx_step(&t, K, OS_CLS_LEVER_WALL, OS_ONMOVE, -1, 0, &d));
    CHECK(!os_tx_step(&t, K, OS_CLS_LEVER_WALL, OS_OFFWAIT, -1, 0, &d));
    CHECK(!os_tx_step(&t, K, OS_CLS_LEVER_WALL, OS_OFFMOVE, 1, 0, &d));   /* the other human (split screen) */
    CHECK(!os_tx_step(&t, K, OS_CLS_LEVER_WALL, OS_ONWAIT, -1, 0, &d));
    CHECK(os_tx_step(&t, K, OS_CLS_LEVER_WALL, OS_ONMOVE, 0, 0, &d) && d.from == OS_ONWAIT && d.to == OS_ONMOVE);
    CHECK(!os_tx_step(&t, K, 0x80061200u, OS_OFFMOVE, 0, 0, &d));         /* a locker is not a lever */

    int s = scenarios();
    CHECK(s == 0);
    /* the model before the fix (0.2.1: no lever mirror): the guest's lever stays where it was */
    mirror_on = 0;
    int f0 = fails;
    setup(1, OS_OFFWAIT);
    step(3);
    A.press = 1;
    step(30);
    int before_ok = same();
    printf("MODEL BEFORE THE FIX: host lever %d flag %d, guest lever %d flag %d: %s\n", A.L.idx, A.L.flag, B.L.idx, B.L.flag, before_ok ? "the same (?!)" : "DIFFERENT (MODEL BEFORE THE FIX FAILS)");
    CHECK(!before_ok);
    fails = f0 + (before_ok ? 1 : 0);
    mirror_on = 1;
    printf("test_objsync1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
