/*
 * Offline test of native/cut1.h (sync022 cutscenes): two PCs on a line with a delay, each with a model of the game's movie player (FUN_00785B30: state 2 a delay,
 * 3 preparing until its movie object is ready, 4 one frame, 5 playing at the movie's own speed until its end or the stock stop FUN_00784210; the skip test of
 * state 5: a press sets +0x20C4 and calls the stop, no press makes the playing frame's call) and the glue's two detours as cut1_game.inc runs them:
 *   the update: the start barrier in state 3 (the update not run while held), after a playing frame the partner's vote that completes this player's;
 *   the stop: from the skip test inside the update with +0x20C4 0 -> 1 = this player's press = his vote; withheld (no stop, +0x20C4 back, the playing frame's call).
 * Checks: the start gap of two PCs that load at different speeds is at most the line's delay (+1 frame); a partner that never gets there releases after
 * CT_START_MS; no session = no hold; one player's press alone stops nothing (the movie ends on both by itself, together); the partner's press alone stops
 * nothing; both presses (either order) stop both PCs within the delay; a vote of an older run of the same movie (a retry) or of another movie never counts;
 * the wire. The model before the fix (0.2.1: no barrier, the stock skip on one PC) must fail: the PCs start apart and one PC leaves the movie alone
 * (MODEL BEFORE THE FIX FAILS). The image's facts the glue relies on (work\image\rerev-image.bin).
 *   sh tests/build.sh -> tests/build/test_cut1.exe
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../native/cut1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define MS_PER_TICK 33u
#define NPC 2
typedef struct {
    /* the movie player */
    int state, t_state, ready_at, len, pos, skipped, started_at, ended_at, stopped_by_skip;
    const char *movie;
    int press_at[4], npress;   /* ticks of this player's skip presses */
    /* the glue */
    ct_t ct;
    int last_state;
    uint32_t last_key;
    int session;
    int upd_on, pre_skipped;
    unsigned withheld, forced, second;
} Pc;
static Pc pc[NPC];
static int fixed = 1;          /* 0 = the model before the fix (no detours) */
static int delay = 3, tick;
static struct { uint8_t b[CT_LEN]; int at, to; } q[256];
static int nq;

static void send_to(int from, const ct_msg *m)
{
    if (nq >= 256) return;
    ct_enc(q[nq].b, m), q[nq].at = tick + delay, q[nq].to = 1 - from, nq++;
}
static uint32_t now_ms(void) { return (uint32_t)tick * MS_PER_TICK + 1000u; }

/* the game: a movie is asked (state 2) */
static void play(int i, const char *movie, int ready_after, int len)
{
    Pc *p = &pc[i];
    p->state = 2, p->t_state = tick, p->ready_at = tick + 2 + ready_after, p->len = len, p->pos = 0, p->skipped = 0, p->movie = movie;
    p->started_at = p->ended_at = -1, p->stopped_by_skip = 0, p->npress = 0;
}
static int pressed_now(Pc *p)
{
    for (int k = 0; k < p->npress; k++)
        if (p->press_at[k] == tick) return 1;
    return 0;
}

/* FUN_00784210 through the glue's stop hook (from_skip = called by the skip test) */
static void stock_stop(Pc *p) { p->state = 0, p->ended_at = tick; }
static void hook_stop(int i, int from_skip)
{
    Pc *p = &pc[i];
    if (fixed && p->upd_on && from_skip && p->skipped == 1 && p->pre_skipped == 0) {
        ct_msg m;
        int snd = 0, r = p->ct.have ? ct_skip_step(&p->ct, p->session, 1, &m, &snd) : 0;
        if (snd) send_to(i, &m);
        if (r == 1) {
            p->skipped = 0, p->withheld++;   /* the playing frame's own call instead: nothing else */
            return;
        }
        if (r == 2) p->second++;
    }
    if (from_skip) p->stopped_by_skip = 1;
    stock_stop(p);
}
/* FUN_00785B30, the stock part */
static void stock_update(int i)
{
    Pc *p = &pc[i];
    switch (p->state) {
    case 2: if (tick - p->t_state >= 2) p->state = 3, p->t_state = tick; break;
    case 3: if (tick >= p->ready_at) p->state = 4, p->started_at = tick; break;
    case 4: p->state = 5; break;
    case 5:
        p->pos++;   /* the movie plays at its own speed */
        if (p->pos >= p->len) { stock_stop(p); break; }                 /* it ended by itself: +0x20C4 left alone */
        if (pressed_now(p)) { p->skipped = 1; hook_stop(i, 1); break; }   /* the skip test */
        break;
    }
}
static uint32_t key_of(Pc *p) { return p->movie ? ct_key(p->movie) : 0; }
/* the glue's update hook (cut1_game.inc hook_mplay) */
static void hook_update(int i)
{
    Pc *p = &pc[i];
    if (!fixed) {
        stock_update(i);
        return;
    }
    int st = p->state;
    uint32_t key = (st >= 3 && st <= 5) ? key_of(p) : 0, now = now_ms();
    int ready = st == 3 && tick >= p->ready_at;
    if (st == 3 && (p->last_state != 3 || key != p->last_key) && key) ct_begin(&p->ct, key, now);
    p->last_state = st, p->last_key = key;
    if (st == 0 || st > 5) ct_end(&p->ct);
    if (ready && p->ct.have && !p->ct.released) {
        ct_msg m;
        int snd = 0, r = ct_ready_step(&p->ct, p->session, now, &m, &snd);
        if (snd) send_to(i, &m);
        if (r == CT_HOLD) return;
    }
    int vote = st == 5;
    if (vote) p->upd_on = 1, p->pre_skipped = p->skipped;
    stock_update(i);
    p->upd_on = 0;
    if (!vote || p->state != 5) return;
    if (p->ct.have && !p->ct.skip_done && p->ct.vote_sent && ct_peer_vote_has(&p->ct, p->ct.key)) {
        ct_msg m;
        int snd = 0;
        if (ct_skip_step(&p->ct, p->session, 0, &m, &snd) == 2) {
            p->forced++;
            p->skipped = 1, p->stopped_by_skip = 1;   /* the stock skip's own steps: +0x20C4 (the movie has not ended), the stock stop */
            stock_stop(p);
        }
    }
}
static void step(int n)
{
    for (int k = 0; k < n; k++) {
        tick++;
        for (int j = 0; j < nq;) {
            if (q[j].at <= tick) {
                ct_msg m;
                if (ct_dec(q[j].b, CT_LEN, &m)) ct_rx(&pc[q[j].to].ct, &m, now_ms());
                q[j] = q[--nq];
            } else j++;
        }
        for (int i = 0; i < NPC; i++) {
            if (pc[i].state == 0 && pc[i].ct.have && fixed) hook_update(i);   /* the stop's state: the glue sees it once (ct_end) */
            else if (pc[i].state) hook_update(i);
        }
    }
}
static void setup(int session)
{
    memset(pc, 0, sizeof pc), nq = 0, tick = 0;
    for (int i = 0; i < NPC; i++) ct_reset(&pc[i].ct), pc[i].last_state = -1, pc[i].session = session;
}
static int run_until_both_end(int max)
{
    for (int k = 0; k < max && (pc[0].state || pc[1].state); k++) step(1);
    return !pc[0].state && !pc[1].state;
}

static int scenarios(void)
{
    int f0 = fails;
    /* 1. the start barrier: B loads 60 frames slower; both start within the delay */
    setup(1);
    play(0, "92_020_0", 10, 400), play(1, "92_020_0", 70, 400);
    CHECK(run_until_both_end(2000));
    CHECK(pc[0].started_at >= 0 && pc[1].started_at >= 0 && abs(pc[0].started_at - pc[1].started_at) <= delay + 1);
    CHECK(abs(pc[0].ended_at - pc[1].ended_at) <= delay + 1 && !pc[0].stopped_by_skip && !pc[1].stopped_by_skip);
    CHECK(pc[0].ct.released_peer == 1 && pc[1].ct.released_peer == 1 && pc[0].ct.holds == 1);
    /* 2. the partner never gets there (another area, an older build): released after CT_START_MS */
    setup(1);
    play(0, "92_020_0", 10, 100);
    int t_ready = 12;
    CHECK(run_until_both_end(3000));
    int waited = pc[0].started_at - t_ready;
    CHECK(pc[0].ct.released_timeout == 1 && waited * (int)MS_PER_TICK >= (int)CT_START_MS && waited * (int)MS_PER_TICK <= (int)CT_START_MS + 3 * (int)MS_PER_TICK);
    /* 3. no session: no hold at all */
    setup(0);
    play(0, "92_020_0", 10, 100);
    CHECK(run_until_both_end(400));
    CHECK(pc[0].started_at == 12 && pc[0].ct.holds == 0 && pc[0].ct.released_nosession == 1);
    /* 4. one player's press alone stops nothing: the movie ends on both by itself, together */
    setup(1);
    play(0, "92_020_0", 10, 300), play(1, "92_020_0", 10, 300);
    pc[0].press_at[0] = 100, pc[0].press_at[1] = 150, pc[0].npress = 2;
    CHECK(run_until_both_end(2000));
    CHECK(!pc[0].stopped_by_skip && !pc[1].stopped_by_skip && pc[0].withheld == 2 && abs(pc[0].ended_at - pc[1].ended_at) <= delay + 1);
    CHECK(pc[0].ct.votes_local == 1 && pc[1].ct.votes_peer == 1);   /* one vote, the second press does not send again */
    /* 5. the partner's press alone stops nothing here either */
    setup(1);
    play(0, "92_020_0", 10, 300), play(1, "92_020_0", 10, 300);
    pc[1].press_at[0] = 100, pc[1].npress = 1;
    CHECK(run_until_both_end(2000));
    CHECK(!pc[0].stopped_by_skip && !pc[1].stopped_by_skip && pc[1].withheld == 1);
    /* 6. both press (A first, B 40 frames later): both stop within the delay of B's press; A through the completed vote, B through its own press */
    setup(1);
    play(0, "92_020_0", 10, 600), play(1, "92_020_0", 10, 600);
    pc[0].press_at[0] = 100, pc[0].npress = 1, pc[1].press_at[0] = 140, pc[1].npress = 1;
    CHECK(run_until_both_end(2000));
    CHECK(pc[0].stopped_by_skip && pc[1].stopped_by_skip && pc[1].ended_at == 140 && pc[0].ended_at >= 140 && pc[0].ended_at <= 140 + delay + 1);
    CHECK(pc[0].forced == 1 && pc[1].second == 1 && pc[0].withheld == 1 && pc[1].withheld == 0);
    /* 7. both press in the same frame: both stop within the delay */
    setup(1);
    play(0, "92_020_0", 10, 600), play(1, "92_020_0", 10, 600);
    pc[0].press_at[0] = 120, pc[0].npress = 1, pc[1].press_at[0] = 120, pc[1].npress = 1;
    CHECK(run_until_both_end(2000));
    CHECK(pc[0].stopped_by_skip && pc[1].stopped_by_skip && abs(pc[0].ended_at - pc[1].ended_at) <= delay + 1 && pc[0].ended_at <= 120 + delay + 1);
    /* 8. a vote of an older run of the same movie (B pressed at its end, the scene was retried 10 s later) never counts: A's press alone stops nothing */
    setup(1);
    play(0, "92_020_0", 10, 200), play(1, "92_020_0", 10, 200);
    pc[1].press_at[0] = 205, pc[1].npress = 1;
    CHECK(run_until_both_end(2000));
    CHECK(!pc[0].stopped_by_skip && !pc[1].stopped_by_skip && pc[0].ct.votes_peer == 1);
    step(300);   /* 10 s */
    play(0, "92_020_0", 10, 300), play(1, "92_020_0", 80, 300);   /* B's PC is slow this time */
    pc[0].press_at[0] = tick + 100, pc[0].npress = 1;
    CHECK(run_until_both_end(2000));
    CHECK(!pc[0].stopped_by_skip && !pc[1].stopped_by_skip && pc[0].withheld == 1);
    /* 9. a vote of another movie never counts */
    setup(1);
    play(0, "92_020_0", 10, 300), play(1, "11_010_0", 10, 300);
    pc[1].press_at[0] = 950, pc[1].npress = 1, pc[0].press_at[0] = 960, pc[0].npress = 1;   /* both playing by then */
    CHECK(run_until_both_end(3000));   /* (the start barrier released them by the time limit: different movies) */
    CHECK(!pc[0].stopped_by_skip && !pc[1].stopped_by_skip && pc[0].ct.released_timeout == 1 && pc[1].ct.released_timeout == 1 && pc[0].withheld == 1 && pc[1].withheld == 1);
    /* 10. a new level instance (ct_epoch) forgets the partner's words */
    setup(1);
    ct_msg v = {CT_SKIP, 1, ct_key("92_020_0")};
    ct_rx(&pc[0].ct, &v, now_ms());
    ct_epoch(&pc[0].ct);
    CHECK(pc[0].ct.nvote == 0 && pc[0].ct.votes_peer == 1);
    return fails - f0;
}

/* the build facts cut1_game.inc relies on */
static void image_facts(void)
{
    FILE *f = fopen("../../image/rerev-image.bin", "rb");
    if (!f) {
        printf("image not found: cut1 build facts not checked\n");
        return;
    }
    static uint8_t img[0xE80000];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    CHECK(n > 0xD40000);
#define AT(va) (img + ((va) - 0x400000u))
    static const uint8_t upd[16] = {0x53, 0x55, 0x8b, 0xe9, 0x8b, 0x85, 0xb4, 0x20, 0x00, 0x00, 0x57, 0x33, 0xff, 0xbb, 0x01, 0x00};
    static const uint8_t stop[16] = {0x53, 0x56, 0x8b, 0xf1, 0x57, 0x8d, 0x8e, 0x44, 0x20, 0x00, 0x00, 0xe8, 0xd0, 0xff, 0x0e, 0x00};
    CHECK(!memcmp(AT(0x785B30u), upd, 16) && !memcmp(AT(0x784210u), stop, 16));
    /* the update's only caller: 0x7867E3 call 0x785B30, then reads +0x20F8 (its return is not used) */
    int32_t rel;
    memcpy(&rel, AT(0x7867E4u), 4);
    CHECK(*AT(0x7867E3u) == 0xE8 && 0x7867E8u + (uint32_t)rel == 0x785B30u && AT(0x7867E8u)[0] == 0x8B && AT(0x7867E8u)[1] == 0x86);
    /* the movie's name is inline at +0x2064 (state 2: lea edx, [ebp+0x2064]; push edx; mov ecx, ebp; call 0x785580), the stream at +0x20A4 */
    memcpy(&rel, AT(0x785C05u), 4);
    CHECK(!memcmp(AT(0x785BF1u), "\x8b\x8d\xa4\x20\x00\x00", 6) && !memcmp(AT(0x785BFBu), "\x8d\x95\x64\x20\x00\x00\x52\x8b\xcd\xe8", 10) && 0x785C09u + (uint32_t)rel == 0x785580u);
    /* the four skip calls of the stop: mov [ebp+0x20C4], bl | mov ecx, ebp | call 0x784210 | cmp [ebp+0x2040], edi */
    static const uint32_t rets[4] = {0x786126u, 0x7861B4u, 0x7862D3u, 0x786379u};
    static const uint8_t mv[6] = {0x88, 0x9D, 0xC4, 0x20, 0x00, 0x00}, cmpe[6] = {0x39, 0xBD, 0x40, 0x20, 0x00, 0x00};
    for (int i = 0; i < 4; i++) {
        uint32_t r = rets[i];
        memcpy(&rel, AT(r - 4), 4);
        CHECK(!memcmp(AT(r - 13), mv, 6) && AT(r - 7)[0] == 0x8B && AT(r - 7)[1] == 0xCD && AT(r - 5)[0] == 0xE8 && r + (uint32_t)rel == 0x784210u && !memcmp(AT(r), cmpe, 6));
    }
    /* the no-skip call of a playing frame: 0x786344 call 0x783F70, which starts by zeroing ecx (it does not use the player) */
    memcpy(&rel, AT(0x786345u), 4);
    CHECK(*AT(0x786344u) == 0xE8 && 0x786349u + (uint32_t)rel == 0x783F70u && !memcmp(AT(0x783F70u), "\x33\xd2\x33\xc9", 4));
    /* the skip test reads sGamePad [0x11E0388] (not the main object [0x11E0534]): 0x786057 mov eax, [0x11E0388]; test byte [ecx+eax+0x1A0], 8 */
    CHECK(!memcmp(AT(0x786057u), "\xa1\x88\x03\x1e\x01", 5) && !memcmp(AT(0x786068u), "\xf6\x84\x01\xa0\x01\x00\x00\x08", 8));
    /* state 3 with a movie object: cmp byte [ebp+0x2058], 0 at 0x785C1D; the ready query FUN_008741B0 = [[ecx+4]+0x38] == 3; over FUN_008741F0 = 0 / 7 / 8 */
    CHECK(!memcmp(AT(0x785C1Du), "\x80\xbd\x58\x20\x00\x00\x00", 7));
    CHECK(!memcmp(AT(0x8741B0u), "\x8b\x49\x04\x85\xc9\x75\x03\x32\xc0\xc3\xe8", 11) && !memcmp(AT(0x8741BFu), "\x83\xf8\x03\x0f\x94\xc0\xc3", 7));
    memcpy(&rel, AT(0x8741BBu), 4);
    CHECK(0x8741BFu + (uint32_t)rel == 0x997F20u && !memcmp(AT(0x997F20u), "\x8b\x41\x38\xc3", 4));
    CHECK(!memcmp(AT(0x8741FCu), "\x83\xf8\x07\x74\x0c\x83\xf8\x08\x74\x07\x85\xc0\x74\x03", 14));
    /* the other kind: FUN_004108C0 alive = (+0xC & 7) 1 / 2; FUN_00891CA0 over = +0x34 && !+0x30 */
    CHECK(!memcmp(AT(0x4108C0u), "\x8b\x41\x0c\x83\xe0\x07\x83\xf8\x02\x74\x08\x83\xf8\x01", 14));
    CHECK(!memcmp(AT(0x891CA0u), "\x32\xc0\x38\x41\x34\x74\x08\x83\x79\x30\x00\x75\x02\xb0\x01\xc3", 16));
}

int main(void)
{
    image_facts();
    /* the wire */
    ct_msg m = {CT_SKIP, 3, 0xDEADBEEFu}, d;
    uint8_t b[CT_LEN];
    CHECK(ct_enc(b, &m) == CT_LEN && ct_dec(b, CT_LEN, &d) && d.kind == CT_SKIP && d.n == 3 && d.key == 0xDEADBEEFu);
    CHECK(!ct_dec(b, CT_LEN - 1, &d));
    b[0] = 9;
    CHECK(!ct_dec(b, CT_LEN, &d));
    ct_enc(b, &m), b[1] = 7;
    CHECK(!ct_dec(b, CT_LEN, &d));
    m.n = 0, ct_enc(b, &m);
    CHECK(!ct_dec(b, CT_LEN, &d));
    CHECK(ct_key("92_020_0") != ct_key("92_020_1") && ct_key("") == ct_key(NULL) && ct_key("x") != 0);

    int s = scenarios();
    CHECK(s == 0);
    /* the model before the fix (0.2.1): no barrier, the stock skip on one PC */
    fixed = 0;
    int f0 = fails;
    setup(1);
    play(0, "92_020_0", 10, 400), play(1, "92_020_0", 70, 400);
    run_until_both_end(2000);
    int gap = abs(pc[0].started_at - pc[1].started_at);
    setup(1);
    play(0, "92_020_0", 10, 300), play(1, "92_020_0", 10, 300);
    pc[0].press_at[0] = 100, pc[0].npress = 1;
    run_until_both_end(2000);
    int alone = pc[0].stopped_by_skip && !pc[1].stopped_by_skip;
    int bad = gap > delay + 1 || alone;
    printf("MODEL BEFORE THE FIX: start gap %d frames (slow loader), one press stops one PC only: %s -> %s\n", gap, alone ? "yes" : "no",
           bad ? "the PCs part company (MODEL BEFORE THE FIX FAILS)" : "together (?!)");
    CHECK(bad);
    fails = f0 + (bad ? 0 : 1);
    fixed = 1;
    printf("test_cut1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
