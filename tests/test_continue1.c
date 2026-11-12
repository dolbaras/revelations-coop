/*
 * Offline tests of continue1 (native/rolebarrier1.c section 6, notes\continue1.md): the Continue after a shared game over in a network session.
 *   (A) the host's checkpoint record travels with RESTART; the guest's record takes it before its reload, so the area start FUN_0041c8d0 decides the opening movie
 *       as on the host (e1p6: the guest's record said mID 0, the host's 1: the guest played 11_010_0 for 112 s, the host reloaded in 2 s);
 *   (B) the level start of a restart is a rendezvous (the aCut area's load step FUN_0041cfc0 held), and the own humans are shielded until the session's barrier opens
 *       (e1p6: the host's level was live 112 s alone and the Ooze killed Jill again: the second game over);
 *   (C) the guest's follow goes through its own stock Continue, with the direct call as a fallback; a late stock restart call after the direct one is dropped;
 *   (D) Continue chosen on the guest only: the guest asks the host (RBM_CONTREQ), its restart waits for the host's triple (fallback: its own checkpoint);
 *   plus the hook bytes, the image facts and the glue wiring. Each fix has its "model before the fix" (the cfg switch off) that fails the same situation.
 *   sh tests/build.sh   ->  tests/build/test_continue1.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/coop_proto.h"
#include "../native/rolebarrier1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static char logbuf[400000];
static void blogf(const char *line)
{
    size_t n = strlen(logbuf);
    if (n + strlen(line) + 2 < sizeof logbuf) snprintf(logbuf + n, sizeof logbuf - n, "%s\n", line);
}

/* ---------------------------------------------------------------- the simulated session and line (the rules of coop_session.c, as test_rolebarrier1.c) */
#define MAXQ 64
typedef struct { uint32_t due; uint8_t body[RB_MSG_MAX]; unsigned len; int to; } pkt;
static uint32_t T;
static pkt q[MAXQ];
static int nq;
static uint32_t line_delay = 20;
typedef struct {
    int started, phase;
    unsigned epoch;
    int me_load_epoch[2], loaded[2], gos[2];
    uint32_t load_area[2];
    unsigned load_area_epoch[2];
} simsess;
static simsess S;
typedef struct { int id; rb_t b; int sends_ok; } pc;
static pc P[2];

static void deliver_due(void)
{
    for (int i = 0; i < nq;) {
        if ((int32_t)(T - q[i].due) >= 0) {
            rb_on_event(&P[q[i].to].b, q[i].body, q[i].len);
            memmove(&q[i], &q[i + 1], (size_t)(nq - i - 1) * sizeof q[0]);
            nq--;
        } else
            i++;
    }
}
static uint32_t io_now(void *u) { (void)u; return T; }
static int io_session(void *u, rb_sinfo *o)
{
    pc *p = (pc *)u;
    if (!S.started) return 0;
    o->started = 1, o->phase = S.phase, o->epoch = S.epoch, o->gos = S.gos[p->id];
    o->other_area = S.load_area[1 - p->id], o->other_area_epoch = S.load_area_epoch[1 - p->id];
    return 1;
}
static int io_send(void *u, const void *body, unsigned len)
{
    pc *p = (pc *)u;
    if (!p->sends_ok || !S.started || nq + 1 >= MAXQ) return 0;
    pkt *k = &q[nq++];
    k->due = T + line_delay;
    memcpy(k->body, body, len), k->len = len, k->to = 1 - p->id;
    return 1;
}
static void sim_check_open(void)
{
    if (S.phase == 2 && S.loaded[0] && S.loaded[1]) S.phase = 3, S.gos[0]++, S.gos[1]++;
}
static void io_begin(void *u, uint32_t key)
{
    pc *p = (pc *)u;
    int fresh = !(S.phase == 2 && S.epoch != (unsigned)S.me_load_epoch[p->id]);
    if (fresh) S.epoch++, S.phase = 2, S.loaded[0] = S.loaded[1] = 0;
    S.me_load_epoch[p->id] = (int)S.epoch;
    S.loaded[p->id] = 0;
    S.load_area[p->id] = key, S.load_area_epoch[p->id] = S.epoch;
}
static void io_done(void *u)
{
    pc *p = (pc *)u;
    if (S.phase == 2) S.loaded[p->id] = 1, sim_check_open();
}
static void io_log(void *u, const char *line) { (void)u; blogf(line); }

static void sim_start(void)
{
    memset(&S, 0, sizeof S);
    S.started = 1, S.phase = 3, S.epoch = 1;   /* both playing level instance 1 */
    nq = 0, T = 100000, line_delay = 20;
    logbuf[0] = 0;
    for (int i = 0; i < 2; i++) {
        rb_io io = {io_now, io_session, io_send, io_begin, io_done, io_log, &P[i]};
        memset(&P[i], 0, sizeof P[i]);
        P[i].id = i, P[i].sends_ok = 1;
        rb_init(&P[i].b, &io, NULL);
        P[i].b.enabled = 1;
        P[i].b.host = i == 0;
    }
}
static void tick(int ms)
{
    for (int i = 0; i < ms; i++) T++, deliver_due();
}
static void both_cfg(int cp_sync, int lstart_hold, int shield, int follow_stock, int cont_req)
{
    for (int i = 0; i < 2; i++) {
        P[i].b.cfg.cp_sync = cp_sync, P[i].b.cfg.lstart_hold = lstart_hold, P[i].b.cfg.shield = shield;
        P[i].b.cfg.follow_stock = follow_stock, P[i].b.cfg.cont_req = cont_req;
    }
}

/* the live words of the sCheckPoint record (sCheckPoint + 0x60 .. + 0x84), e1p6 / e1p9 B2 dumps (log.txt `CP record`) */
static const uint32_t REC_HOST[9] = {0x0106a024u, 0x010e1d24u, 0u, 0x010e1d24u, 3u, 0x91010100u, 1u, 256u, 4u};    /* mask 3, mID 1, 256 / 4 */
static const uint32_t REC_GUEST[9] = {0x0106a024u, 0x010e1d24u, 0u, 0x010e1d24u, 1u, 0x91010100u, 0u, 256u, 4u};   /* mask 1, mID 0 (not written) */
static const uint32_t REC_CLEARED[9] = {0x0106a024u, 0x010e1d24u, 0u, 0x010e1d24u, 0u, 0x91FF0000u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};   /* e1p2: cData_vf7 */
static const rb_triple CP1 = {0x91010100u, 256, 4};

/* ---------------------------------------------------------------- (A) + (C): the e1p6 Continue, the guest follows. Each PC's record before the reload, the movie decision of
 * its area start (rb_start_movie_plays with the restart bit FUN_00720870 sets) and the reload time (2 s at a checkpoint, 112 s with 11_010_0). via_stock: the guest's
 * follow went through its stock Continue (the restart call from FUN_006d0cb0, call_stock 1), else the direct call. Returns the guest's reload ms; *movie = the guest's
 * movie decision; *same = the guest's record equals the host's after its reload began. */
static int a_follow(int cp_sync, int force_stock, const uint32_t *guest_rec, int *movie, int *same)
{
    sim_start();
    both_cfg(cp_sync, 1, 1, 1, 1);
    P[1].b.cp_saved = (rb_cprec){1, (int32_t)guest_rec[6], guest_rec[4], guest_rec[5], (int32_t)guest_rec[7], (int32_t)guest_rec[8]};   /* what its save holds */
    uint32_t hrec[9], grec[9];
    memcpy(hrec, REC_HOST, sizeof hrec), memcpy(grec, guest_rec, sizeof grec);
    /* the host's stock Continue: its checkpoint read restored hrec; the glue reads it in the restart hook */
    P[0].b.tx_cp = (rb_cp){1, (int32_t)hrec[6], hrec[4]};
    P[0].b.tx_diff = 1;
    rb_triple t = CP1;
    CHECK(rb_restart_host(&P[0].b, &t) == 1);
    tick(30);
    CHECK(P[1].b.rs_have && P[1].b.rs_diff == 1);
    CHECK(P[1].b.rs_cp.have == (cp_sync != 0) && (!cp_sync || (P[1].b.rs_cp.id == 1 && P[1].b.rs_cp.mask == 3)));
    rb_triple fol;
    int r = 0;
    for (int i = 0; i < 2000 && !r; i++) tick(1), r = rb_restart_follow_at(&P[1].b, &fol, 0);
    CHECK(r == RB_FOL_CALL && fol.area == CP1.area && fol.a == 256 && fol.b == 4);
    /* c1a live: the stock Continue is chosen only when the guest's SAVE holds the host's checkpoint (cp_saved = its record as the engine last wrote it) */
    int save_ok = guest_rec[6] == REC_HOST[6] && guest_rec[5] == REC_HOST[5] && guest_rec[7] == REC_HOST[7] && guest_rec[8] == REC_HOST[8];
    CHECK(P[1].b.fol_try_stock == (cp_sync && save_ok));
    int via_stock = force_stock || P[1].b.fol_try_stock;
    rb_follow_went(&P[1].b, via_stock);
    if (via_stock) tick(400);   /* aStartContinue, the guest's checkpoint read, then FUN_006d0cb0 */
    rb_triple own = {grec[5], (int32_t)grec[7], (int32_t)grec[8]};
    if (own.area == 0x91FF0000u) own = (rb_triple){0x91010100u, 256, 1};   /* a cleared record: 6d0cb0 takes the start table's */
    P[1].b.call_stock = via_stock;
    int rc = rb_restart_client(&P[1].b, &own);
    P[1].b.call_stock = 0;
    CHECK(rc == 1 && own.area == CP1.area && own.a == 256 && own.b == 4);
    if (P[1].b.applied_cp.have) rb_cp_rec_take(grec, &own, &P[1].b.applied_cp);   /* the glue's cp_take */
    if (via_stock) memcpy(grec, guest_rec, sizeof grec);   /* c1a live: the stock Continue applies the guest's OWN save during the load, after the hook */
    *movie = rb_start_movie_plays(0, 1, (int32_t)grec[6]);
    *same = !memcmp(grec + 2, hrec + 2, 7 * sizeof(uint32_t));
    CHECK(rb_start_movie_plays(0, 1, (int32_t)hrec[6]) == 0);   /* the host reloads in place */
    return *movie ? 112000 : 2000;
}

/* ---------------------------------------------------------------- (B): the level start and the shield, on the session timeline. The host restarts at 0 and is ready at
 * 2000 ms; the guest follows after follow_ms (+ the line) and is ready guest_ready ms after the RESTART. Each PC: unplayable from its restart until its level started
 * (the gate passed) + 300 ms (placed); LOAD_BEGIN / LOAD_DONE / GO by rb_load_step. Measured: alone = how long one PC's level ran while the other's had not started;
 * hits = the ms in which a started level of a PC took the Ooze's blows unshielded before the barrier opened (the e1p6 death); gap = the two starts apart. */
typedef struct { int alone, hits, gap, opened, host_start, guest_start, guest_timeout; } lsres;
static lsres b_run(int lstart_hold, int shield, int guest_ready, int guest_comes)
{
    lsres o;
    memset(&o, 0, sizeof o);
    sim_start();
    both_cfg(1, lstart_hold, shield, 0, 1);
    S.phase = 2;   /* level instance 1: both load it and play (the first load of the session is behind them) */
    for (int i = 0; i < 1000; i++) tick(1), rb_load_step(&P[0].b, 1), rb_load_step(&P[1].b, 1);
    CHECK(S.phase == 3 && S.epoch == 1);
    rb_triple t = CP1;
    P[0].b.tx_cp = (rb_cp){1, 1, 3};
    rb_restart_host(&P[0].b, &t);
    int restarted[2] = {1, 0}, ready_at[2] = {2000, -1}, started[2] = {-1, -1};
    uint32_t t0 = T;
    o.host_start = o.guest_start = -1, o.opened = -1;
    for (int ms = 0; ms < 200000; ms++) {
        tick(1);
        if (!restarted[1] && guest_comes) {
            rb_triple fol;
            if (rb_restart_follow_at(&P[1].b, &fol, 0)) {
                rb_follow_went(&P[1].b, 0);
                rb_triple own = {0x91010100u, 256, 4};
                rb_restart_client(&P[1].b, &own);
                restarted[1] = 1, ready_at[1] = (int)(T - t0) + guest_ready - 1500;
            }
        }
        for (int k = 0; k < 2; k++) {
            int playable = !restarted[k];
            if (restarted[k] && ready_at[k] >= 0 && ms >= ready_at[k]) {
                if (started[k] < 0 && rb_lstart_gate(&P[k].b, 1) == RB_PROCEED) started[k] = ms;
                playable = started[k] >= 0 && ms >= started[k] + 300;
            }
            rb_load_step(&P[k].b, playable);
        }
        if (S.phase == 3 && S.epoch >= 2 && o.opened < 0) o.opened = ms;
        for (int k = 0; k < 2; k++) {   /* the Ooze stands next to the humans at 256 / 4: a started, placed level whose barrier is not open gets blows unless shielded */
            int sh = rb_shield_on(&P[k].b);
            if (started[k] >= 0 && ms >= started[k] + 300 && o.opened < 0 && !sh) o.hits++;
        }
        if (o.opened >= 0 && ms > o.opened + 2000) break;
    }
    o.host_start = started[0], o.guest_start = started[1];
    if (started[0] >= 0 && started[1] >= 0) o.gap = abs(started[0] - started[1]);
    o.alone = started[0] >= 0 ? (started[1] >= 0 ? started[1] - started[0] : 200000 - started[0]) : 0;
    o.guest_timeout = (int)P[0].b.ls_timeout;
    return o;
}

/* ---------------------------------------------------------------- source / image helpers */
static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long m = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)m + 1);
    size_t got = fread(b, 1, (size_t)m, f), o = 0;
    fclose(f);
    for (size_t i = 0; i < got; i++)
        if (b[i] != '\r') b[o++] = b[i];   /* the checkout is CRLF */
    b[o] = 0;
    return b;
}
static int count_of(const char *s, const char *what)
{
    int n = 0;
    for (const char *p = s; p && (p = strstr(p, what)); p++) n++;
    return n;
}

/* ---------------------------------------------------------------- task 4: an engine model of the record and of FUN_006d1a70 (0x6D1A70, notes/continue1.md section 1) */
typedef struct { rb_cprec r; int busy; unsigned passes, refused; } cpeng;
#define CPENG_INIT(e) do { memset((e), 0, sizeof (e)); for (int q_ = 0; q_ < 2; q_++) (e)[q_].r = (rb_cprec){1, -1, 0, 0x91FF0000u, -1, -1}; } while (0)   /* the record's ctor: cleared */
static void eng_pass(cpeng *e, int id, int a, int b, int mflag, uint32_t cur_area)
{
    if (id == e->r.id) { e->refused++; return; }                                   /* the same mID already current */
    if (!(mflag & 1) && id >= 0 && id < 32 && (e->r.mask & (1u << id))) { e->refused++; return; }   /* passed before */
    if (e->busy) { e->refused++; return; }                                         /* the save manager runs a job */
    e->r.have = 1, e->r.id = id, e->r.a = a, e->r.b = b, e->r.area = cur_area, e->r.mask |= 1u << id, e->passes++;
}
/* one frame of both PCs: each reads its record, the mirror step, the glue's call of the pass (the content of the partner's record) */
static void cpm_frame(cpeng *e, const int lvl[2])
{
    for (int k = 0; k < 2; k++) {
        rb_cprec out;
        if (rb_cpm_step(&P[k].b, &e[k].r, lvl[k], &out)) {
            eng_pass(&e[k], out.id, out.a, out.b, 0, 0x91010100u);
            rb_cpm_applied(&P[k].b, 1, &e[k].r);
        }
    }
}
static void cpm_run(cpeng *e, const int lvl[2], int ms)
{
    for (int i = 0; i < ms; i += 16) tick(16), cpm_frame(e, lvl);
}

int main(void)
{
    /* ============================================ (A) the message and the record */
    {
        uint8_t m[32];
        rb_cp cp = {1, 1, 3};
        CHECK(rb_msg_restart_cp(m, sizeof m, 2, 0x91010100u, 256, 4, 1, &cp) == RB_RESTART_CP_LEN && m[1] == RBM_RESTART && m[16] == 1);
        CHECK(rb_msg_restart_cp(m, sizeof m, 2, 0x91010100u, 256, 4, -1, &cp) == RB_RESTART_CP_LEN && m[16] == 0xFF);
        CHECK(rb_msg_restart_cp(m, 20, 2, 0x91010100u, 256, 4, 1, &cp) == 0);
        cp.have = 0;
        CHECK(rb_msg_restart_cp(m, sizeof m, 2, 0x91010100u, 256, 4, 1, &cp) == 17 && rb_msg_restart_cp(m, sizeof m, 2, 0x91010100u, 256, 4, -1, &cp) == 16);
        CHECK(rb_msg_contreq(m, sizeof m, 9) == RB_CONTREQ_MSG_LEN && m[1] == RBM_CONTREQ && rb_msg_contreq(m, 3, 9) == 0);
        /* the guest decodes the record; an unknown difficulty stays -1; an old 17-byte RESTART carries no record */
        sim_start();
        P[0].b.tx_cp = (rb_cp){1, 2, 7}, P[0].b.tx_diff = -1;
        rb_triple t = {0x91010100u, 256, 5};
        CHECK(rb_restart_host(&P[0].b, &t) == 1 && P[0].b.cp_sent == 1);
        tick(30);
        CHECK(P[1].b.rs_have && P[1].b.rs_diff == -1 && P[1].b.rs_cp.have && P[1].b.rs_cp.id == 2 && P[1].b.rs_cp.mask == 7 && P[1].b.cp_got == 1);
        CHECK(strstr(logbuf, "its checkpoint record: mID 2, passed mask 0007") != NULL);
        rb_msg_restart_d(m, sizeof m, 5, 0x91010100u, 256, 4, 2);
        CHECK(rb_on_event(&P[1].b, m, 17) == 1 && P[1].b.rs_diff == 2 && !P[1].b.rs_cp.have);
        /* the record words: the e1p6 split and the take */
        uint32_t g[9], h[9], c[9];
        memcpy(g, REC_GUEST, sizeof g), memcpy(h, REC_HOST, sizeof h), memcpy(c, REC_CLEARED, sizeof c);
        CHECK(rb_start_movie_plays(0, 1, (int32_t)g[6]) == 1 && rb_start_movie_plays(0, 1, (int32_t)h[6]) == 0);   /* e1p6: 11_010_0 on the guest only */
        CHECK(rb_start_movie_plays(0, 0, 1) == 1 && rb_start_movie_plays(1, 0, 0) == 0 && rb_start_movie_plays(0, 1, -1) == 1);   /* no restart bit / movie done / section start */
        rb_cp hc = {1, (int32_t)h[6], h[4]};
        CHECK(rb_cp_rec_take(g, &CP1, &hc) == 1 && !memcmp(g, h, sizeof g) && rb_start_movie_plays(0, 1, (int32_t)g[6]) == 0);
        CHECK(rb_cp_rec_take(g, &CP1, &hc) == 0);   /* already the host's */
        CHECK(rb_cp_rec_take(c, &CP1, &hc) == 1 && !memcmp(c, h, sizeof c));   /* e1p2's cleared record */
        c[2] = 0x13u;
        CHECK(rb_cp_rec_take(c, &CP1, &hc) == 1 && c[2] == 0x11u);   /* bit 1 of +8 cleared as FUN_006d1a70 does */
        hc.have = 0;
        CHECK(rb_cp_rec_take(c, &CP1, &hc) == 0);
    }

    /* ============================================ (A) + (C) the e1p6 Continue, both follow ways, three guest records */
    {
        int movie, same;
        const uint32_t *recs[3] = {REC_GUEST, REC_CLEARED, REC_HOST};
        for (int k = 0; k < 3; k++) {
            int ms = a_follow(1, 0, recs[k], &movie, &same);
            CHECK(ms == 2000 && movie == 0 && same == 1);
            CHECK(P[1].b.cp_applied == 1 && P[1].b.ls_armed && P[1].b.sh_armed);
            CHECK(P[1].b.stock_refused == (unsigned)(k < 2));   /* the guest's save does not hold mID 1: the direct call */
        }
        /* c1a live (18:40): the stock follow whatever the guest's save holds = the model before this fix: the guest's own save (mID 0) comes back during the load:
         * 11_010_0 (112 s) although the hook had written the host's record */
        int bad_stock = 0;
        for (int k = 0; k < 3; k++) bad_stock += a_follow(1, 1, recs[k], &movie, &same) != 2000;
        CHECK(bad_stock == 2);
        /* the model before the fix (cp_sync 0): the guest's own record decides: the 112 s movie with mID 0 / a cleared record (e1p6), fast only by luck (e1p9: B4 had put
         * the client's Parker into the checkpoint sphere, its record said mID 1) */
        int bad = 0;
        for (int k = 0; k < 3; k++) {
            int ms = a_follow(0, 0, recs[k], &movie, &same);
            bad += ms != 2000 || !same;
        }
        CHECK(bad == 2);
        printf("  (A) the guest's reload after the host's Continue: 2000 ms for all 3 guest records (the stock way only on a matching save); the stock way always (c1a live): %d of 3 play 11_010_0; without the record: %d of 3 (112 s)\n", bad_stock, bad);
    }

    /* ============================================ (B) the level start and the shield */
    {
        lsres a = b_run(1, 1, 3000, 1), z = b_run(0, 0, 3000, 1), e = b_run(0, 0, 113500, 1), ea = b_run(1, 1, 113500, 1);
        CHECK(a.host_start >= 0 && a.guest_start >= 0 && a.gap <= 25 && a.alone <= 25 && a.hits == 0 && a.opened > 0);
        CHECK(ea.gap <= 25 && ea.hits == 0 && ea.host_start >= 113000);   /* even with a 112 s guest load: the host waits at its level start */
        CHECK(P[0].b.ls_peer == 1 && P[1].b.ls_peer == 1 && P[0].b.ls_timeout == 0);
        CHECK(z.alone >= 900 && z.hits > 0);        /* the model before the fix: the host plays alone and is hit before the barrier opens */
        CHECK(e.alone >= 110000 && e.hits > 100000); /* e1p6: 112 s alone (the Ooze killed Jill) */
        printf("  (B) host alone before the guest's level started: fixed %d ms (gap %d, hits %d; with a 112 s guest load %d ms, hits %d); before the fix %d ms (hits %d), "
               "e1p6-like %d ms (hits %d)\n", a.alone, a.gap, a.hits, ea.alone, ea.hits, z.alone, z.hits, e.alone, e.hits);
        /* the guest never comes: the host starts after lstart_ms (120 s), the shield ends when its barrier opens or by its limit */
        lsres n = b_run(1, 1, 3000, 0);
        CHECK(n.host_start >= 121900 && n.host_start <= 122100 && n.guest_timeout == 1);
        CHECK(strstr(logbuf, "level start #1 RELEASED by the TIMEOUT") != NULL);
        /* no restart = no hold (a section change, a first load) */
        sim_start();
        CHECK(rb_lstart_gate(&P[0].b, 1) == RB_PROCEED && rb_lstart_gate(&P[1].b, 1) == RB_PROCEED && rb_lstart_gate(&P[0].b, 0) == RB_PROCEED && !rb_shield_on(&P[0].b));
        /* a stale arm (the restart call did not reload) is not held */
        rb_triple t = CP1;
        rb_restart_host(&P[0].b, &t);
        tick(700000);
        CHECK(rb_lstart_gate(&P[0].b, 1) == RB_PROCEED && P[0].b.ls_stale == 1 && !P[0].b.ls_armed);
        CHECK(!rb_shield_on(&P[0].b) && P[0].b.sh_capped == 1);
        /* the session ends: nothing is held */
        sim_start();
        rb_restart_host(&P[0].b, &t);
        S.started = 0;
        CHECK(rb_lstart_gate(&P[0].b, 1) == RB_PROCEED && !rb_shield_on(&P[0].b));
        /* the guest's shield armed when the host's LOADING epoch had begun opens at that epoch's GO */
        sim_start();
        S.phase = 2, S.epoch = 2;
        P[1].b.rs_have = 1, P[1].b.rs_seq = 4, P[1].b.rs_area = CP1.area, P[1].b.rs_a = 256, P[1].b.rs_b = 4, P[1].b.rs_t = T;
        rb_triple own = CP1;
        CHECK(rb_restart_client(&P[1].b, &own) == 1 && rb_shield_on(&P[1].b) == 1);
        S.phase = 3;
        CHECK(rb_shield_on(&P[1].b) == 0 && P[1].b.sh_opened == 1);
    }

    /* ============================================ (C) the stock follow's fallback and the dropped late call */
    {
        sim_start();
        rb_triple t = CP1, fol;
        P[0].b.tx_cp = (rb_cp){1, 1, 3};
        P[1].b.cp_saved = (rb_cprec){1, 1, 3, CP1.area, 256, 4};   /* the guest's save holds the host's checkpoint (the task 4 mirror passed it there) */
        rb_restart_host(&P[0].b, &t);
        int r = 0;
        for (int i = 0; i < 2000 && !r; i++) tick(1), r = rb_restart_follow_at(&P[1].b, &fol, 0);
        CHECK(r == 1 && P[1].b.fol_try_stock == 1);
        rb_follow_went(&P[1].b, 1);
        CHECK(P[1].b.fol_try_stock == 0 && P[1].b.fol_stock_asked == 1);
        int again = 0, at = -1;
        for (int i = 0; i < 9000; i++) {
            tick(1);
            if (rb_restart_follow_at(&P[1].b, &fol, 0)) again++, at = i;
        }
        CHECK(again == 1 && at >= 7900 && at <= 8100 && P[1].b.fol_stock_fallback == 1 && P[1].b.fol_try_stock == 0);
        rb_triple own = {0x91010100u, 256, 1};
        CHECK(rb_restart_client(&P[1].b, &own) == 1 && own.b == 4);   /* the direct call (not the stock caller) */
        tick(500);
        own = (rb_triple){0x91010100u, 256, 1};
        P[1].b.call_stock = 1;
        CHECK(rb_restart_client(&P[1].b, &own) == -1 && P[1].b.rs_dups == 1);   /* the late stock call: dropped, no second reload */
        P[1].b.call_stock = 0;
        tick(11000);
        P[1].b.call_stock = 1;
        CHECK(rb_restart_client(&P[1].b, &own) == 0 && own.b == 1);   /* much later: a new Continue of its own */
        P[1].b.call_stock = 0;
        /* the stock Continue that used the triple: no fallback */
        sim_start();
        rb_restart_host(&P[0].b, &t);
        r = 0;
        for (int i = 0; i < 2000 && !r; i++) tick(1), r = rb_restart_follow_at(&P[1].b, &fol, 0);
        rb_follow_went(&P[1].b, 1);
        tick(300);
        own = (rb_triple){0x91010100u, 256, 1};
        P[1].b.call_stock = 1;
        CHECK(rb_restart_client(&P[1].b, &own) == 1 && own.b == 4);
        P[1].b.call_stock = 0;
        again = 0;
        for (int i = 0; i < 12000; i++) tick(1), again += rb_restart_follow_at(&P[1].b, &fol, 0);
        CHECK(again == 0 && P[1].b.fol_stock_fallback == 0);
        /* the guest's own Continue (its menu) after the host's triple came: the follow waits for its restart, which takes the triple; no double reload */
        sim_start();
        rb_restart_host(&P[0].b, &t);
        tick(300);
        CHECK(P[1].b.rs_have && rb_cont_request(&P[1].b, 0) == RB_PROCEED && P[1].b.cr_sent == 0);   /* the triple is there: no CONTREQ */
        r = 0;
        for (int i = 0; i < 3000 && !r; i++) tick(1), r = rb_restart_follow_at(&P[1].b, &fol, 0);
        CHECK(r == 0);
        own = (rb_triple){0x91010100u, 256, 1};
        P[1].b.call_stock = 1;
        CHECK(rb_restart_client(&P[1].b, &own) == 1 && own.b == 4);
        P[1].b.call_stock = 0;
        for (int i = 0; i < 9000; i++) tick(1), r += rb_restart_follow_at(&P[1].b, &fol, 0);
        CHECK(r == 0);
        /* ... and when its stock flow never reaches the restart: the follow after stock_wait_ms */
        sim_start();
        rb_restart_host(&P[0].b, &t);
        tick(300);
        rb_cont_request(&P[1].b, 0);
        int at2 = -1;
        r = 0;
        for (int i = 0; i < 9000 && !r; i++) tick(1), r = rb_restart_follow_at(&P[1].b, &fol, 0), at2 = i;
        CHECK(r == 1 && at2 >= 7900 && at2 <= 8100);
        /* at the title the follow stays the direct call (the session start's proven path) */
        sim_start();
        rb_restart_host(&P[0].b, &t);
        r = 0;
        for (int i = 0; i < 2000 && !r; i++) tick(1), r = rb_restart_follow_at(&P[1].b, &fol, 1);
        CHECK(r == 1 && P[1].b.fol_try_stock == 0);
        /* the model before the fix (follow_stock 0): always the direct call */
        sim_start();
        both_cfg(1, 1, 1, 0, 1);
        rb_restart_host(&P[0].b, &t);
        r = 0;
        for (int i = 0; i < 2000 && !r; i++) tick(1), r = rb_restart_follow_at(&P[1].b, &fol, 0);
        CHECK(r == 1 && P[1].b.fol_try_stock == 0);
    }

    /* ============================================ (D) Continue chosen on the guest only */
    {
        sim_start();
        rb_triple own = {0x91010100u, 256, 4}, out;
        int host = -1;
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED && P[1].b.cr_sent == 1);   /* its game-over Yes: CONTREQ */
        tick(30);
        CHECK(P[0].b.cr_got == 1 && P[0].b.cr_pending == 1 && rb_cont_request_step(&P[0].b) == 1);
        /* the guest's checkpoint read ends before the host's triple: its restart waits */
        tick(170);
        P[1].b.call_stock = 1;
        rb_triple g = own;
        CHECK(rb_restart_client(&P[1].b, &g) == -1 && P[1].b.held == 1 && P[1].b.held_count == 1);
        P[1].b.call_stock = 0;
        CHECK(rb_held_step(&P[1].b, &out, &host) == 0);
        /* the host answers: its own stock Continue (the mod's call), its restart sends RESTART */
        CHECK(rb_cont_request(&P[0].b, 1) == RB_PROCEED);
        rb_cont_answered(&P[0].b, 1, "test");
        CHECK(P[0].b.cr_pending == 0 && P[0].b.cr_done == 1 && rb_cont_request_step(&P[0].b) == 0);
        tick(300);
        P[0].b.tx_cp = (rb_cp){1, 1, 3};
        rb_triple t = CP1;
        CHECK(rb_restart_host(&P[0].b, &t) == 1);
        int follows = 0;
        rb_triple fol;
        for (int i = 0; i < 40; i++) tick(1), follows += rb_restart_follow_at(&P[1].b, &fol, 0);
        CHECK(follows == 0);   /* held: the normal follow does not also fire */
        CHECK(rb_held_step(&P[1].b, &out, &host) == 1 && host == 1 && out.area == CP1.area && out.b == 4 && !P[1].b.held);
        g = out;
        CHECK(rb_restart_client(&P[1].b, &g) == 1 && P[1].b.applied_cp.have && P[1].b.ls_armed && P[1].b.ls_key == P[0].b.ls_key);
        for (int i = 0; i < 3000; i++) tick(1), follows += rb_restart_follow_at(&P[1].b, &fol, 0);
        CHECK(follows == 0);   /* the triple is used: no second reload */
        /* c1a live: the guest's save does not hold the host's checkpoint (known from its RESTART / CPPASS): its own stock Continue is NOT run (it would load its
         * older save), CONTREQ, held; the host's triple loads by the direct call */
        sim_start();
        P[1].b.host_cp = (rb_cprec){1, 1, 3, CP1.area, 256, 4};
        P[1].b.cp_saved = (rb_cprec){1, 0, 1, CP1.area, 256, 4};
        CHECK(rb_cont_request(&P[1].b, 0) == RB_HOLD && P[1].b.cr_sent == 1 && P[1].b.held == 1 && P[1].b.own_cont_redirected == 1);
        tick(30);
        CHECK(P[0].b.cr_pending == 1);
        P[0].b.tx_cp = (rb_cp){1, 1, 3};
        rb_cont_answered(&P[0].b, 1, "test");
        t = CP1;
        rb_restart_host(&P[0].b, &t);
        tick(30);
        CHECK(rb_held_step(&P[1].b, &out, &host) == 1 && host == 1 && out.b == 4);
        g = out;
        CHECK(rb_restart_client(&P[1].b, &g) == 1 && g.b == 4 && P[1].b.applied_cp.have && P[1].b.applied_cp.id == 1);
        /* ... with a matching save the stock way stays */
        sim_start();
        P[1].b.host_cp = (rb_cprec){1, 1, 3, CP1.area, 256, 4};
        P[1].b.cp_saved = P[1].b.host_cp;
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED && P[1].b.cr_sent == 1 && !P[1].b.held && P[1].b.own_cont_redirected == 0);
        /* the host never answers: the guest's own checkpoint after cont_wait_ms */
        sim_start();
        P[0].sends_ok = 1;
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED);
        tick(200);
        g = own;
        P[1].b.call_stock = 1;
        CHECK(rb_restart_client(&P[1].b, &g) == -1);
        P[1].b.call_stock = 0;
        int got = 0, at = -1;
        for (int i = 0; i < 16000 && !got; i++) tick(1), got = rb_held_step(&P[1].b, &out, &host), at = i;
        CHECK(got == 1 && host == 0 && out.area == own.area && out.b == 4 && at >= 14900 && at <= 15100);
        g = out;
        CHECK(rb_restart_client(&P[1].b, &g) == 0 && P[1].b.held_own_used == 1 && !P[1].b.ls_armed);   /* the glue's call (not the stock caller): its own checkpoint */
        /* the host was already continuing (its own Yes): the request is answered by that */
        sim_start();
        CHECK(rb_cont_request(&P[0].b, 0) == RB_PROCEED);
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED);
        tick(30);
        CHECK(rb_cont_request_step(&P[0].b) == 0 && P[0].b.cr_done == 1);
        /* the host restarted just before the request (the RESTART was still on its way to the guest) */
        sim_start();
        t = CP1;
        rb_restart_host(&P[0].b, &t);
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED && P[1].b.cr_sent == 1);   /* the guest did not have the triple yet */
        tick(30);
        CHECK(rb_cont_request_step(&P[0].b) == 0 && P[0].b.cr_done == 1);
        g = own;
        P[1].b.call_stock = 1;
        CHECK(rb_restart_client(&P[1].b, &g) == 1);   /* its restart takes the arrived triple */
        P[1].b.call_stock = 0;
        /* a second stock Continue request while one is on its way: dropped (aStartContinue would assert), solo: never */
        sim_start();
        CHECK(rb_cont_request(&P[1].b, 1) == RB_PROCEED && rb_cont_request(&P[1].b, 0) == RB_HOLD && P[1].b.cont_dropped == 1);
        tick(10100);
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED);
        P[1].b.cont_t = 0;
        S.started = 0;
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED && rb_cont_request(&P[1].b, 0) == RB_PROCEED && P[1].b.cont_t == 0);
        /* a level loaded (LOAD_DONE): the next request is a new one */
        sim_start();
        CHECK(rb_cont_request(&P[1].b, 1) == RB_PROCEED);
        S.phase = 2, S.epoch = 2;
        for (int i = 0; i < 700; i++) tick(1), rb_load_step(&P[1].b, 1);
        CHECK(P[1].b.cont_t == 0 && rb_cont_request(&P[1].b, 0) == RB_PROCEED);
        /* the model before the fix (cont_req 0): the guest's stock restart takes its own checkpoint at once, no CONTREQ: the host stays on its game over (the PCs split) */
        sim_start();
        both_cfg(1, 1, 1, 1, 0);
        CHECK(rb_cont_request(&P[1].b, 0) == RB_PROCEED && P[1].b.cr_sent == 0);
        tick(30);
        g = own;
        P[1].b.call_stock = 1;
        int split = rb_restart_client(&P[1].b, &g) == 0 && P[0].b.cr_got == 0 && P[0].b.rs_sent == 0;
        P[1].b.call_stock = 0;
        CHECK(split);
        printf("  (D) Continue on the guest only: CONTREQ -> the host's Continue -> the guest loads the host's checkpoint; before the fix the guest reloads alone (split %d)\n", split);
    }

    /* ============================================ task 2: the host's SPAWN after a Continue places the Ooze on the guest
     * SpawnChara 0x4688F0: the current stage = FUN_0078fb50(FUN_007203c0()), but -1 when from_host and the running area is an aCut (0x92) one (0x468DF3); at the end the
     * unit stays only when the block's stage (+0x18) equals it, else vf15 + the deleting destructor and entry+4 = 0 ("placement registered without a unit"). After the
     * Continue the guest's own script placed nothing (the host's 2/0 and 0/0 came back by its checkpoint read), so only op_spawn can place them. */
    {
        int stage_now = 256, acut = 1, kept_fix = 0, kept_old = 0;
        const int block_stage[3] = {256, 256, 257};   /* 2/0 the Ooze, 0/0 the zombie, a placement of another stage */
        for (int fh = 0; fh < 2; fh++)
            for (int k = 0; k < 3; k++) {
                int stage = fh && acut ? -1 : stage_now;
                int kept = block_stage[k] == stage;
                if (fh) kept_old += kept;
                else kept_fix += kept;
            }
        CHECK(kept_fix == 2 && kept_old == 0);   /* fixed: 2/0 and 0/0 are replicas on the guest; another stage stays without a unit; before: none (e1p6 / e1p9 `B5 enemies: H 2 | C 0`) */
        char *w = slurp("../native/world1_game.c");
        CHECK(w && strstr(w, "static volatile int f_spawnfh;") && strstr(w, "r->spawn_flags, f_spawnfh ? 1 : 0, 0);") && strstr(w, "{\"spawnfh\", &f_spawnfh, 1}"));
        free(w);
        printf("  task 2: after the Continue the guest gets %d of 2 enemies as replicas (before the fix %d)\n", kept_fix, kept_old);
    }

    /* ============================================ task 4 (Q8): the checkpoint pass mirror */
    {
        const int lv[2] = {1, 1};
        int both1 = 0, old1 = 0;
        for (int fix = 1; fix >= 0; fix--) {
            sim_start();
            for (int k = 0; k < 2; k++) P[k].b.cfg.cp_mirror = fix;
            cpeng e[2];
            CPENG_INIT(e);
            for (int k = 0; k < 2; k++) eng_pass(&e[k], 0, 256, 4, 0, 0x91010100u);   /* the level start (FUN_006d1e90): mID 0 with the start triple */
            cpm_run(e, lv, 500);
            CHECK(P[0].b.cpm_sent == 0 && P[1].b.cpm_sent == 0);   /* the section start's mID 0 is not told */
            eng_pass(&e[0], 1, 256, 4, 0, 0x91010100u);   /* 4390 on the host: its Jill in the CheckPoint sphere; the guest's zone does not fire (e1p5 / e1p6 / e1p9) */
            cpm_run(e, lv, 1000);
            CHECK(!fix || e[1].r.id == 0);   /* taken 1.2 s after the CPPASS (the flags set with it come first) */
            cpm_run(e, lv, 500);
            if (fix) both1 = e[0].r.id == 1 && e[1].r.id == 1 && e[1].r.mask == 3 && e[1].r.a == 256 && e[1].r.b == 4;
            else old1 = e[1].r.id == 1;
            if (fix) {
                CHECK(both1 && P[0].b.cpm_sent == 1 && P[1].b.cpm_got == 1 && P[1].b.cpm_done == 1 && P[1].b.cpm_sent == 0);   /* no echo back */
                CHECK(strstr(logbuf, "the partner's checkpoint mID 1 passed here too") != NULL);
                /* c1c live: the mirrored pass is the engine's write (record + checkpoint save): the guest's saved checkpoint is the host's, so its follow / own
                 * Continue may take the stock way (the mark only keeps it from being told back) */
                CHECK(P[1].b.cp_saved.have && P[1].b.cp_saved.id == 1 && P[1].b.cp_saved.a == 256 && P[1].b.cp_saved.b == 4);
                /* mID 2 (256 / 5) passed first on the GUEST (its own human in the second sphere): the host takes it */
                eng_pass(&e[1], 2, 256, 5, 0, 0x91010100u);
                cpm_run(e, lv, 1500);
                CHECK(e[0].r.id == 2 && e[0].r.b == 5 && e[0].r.mask == 7 && P[1].b.cpm_sent == 1 && P[0].b.cpm_done == 1);
                /* a late stale pass of mID 1 to a PC at mID 2: passed before, nothing called */
                unsigned calls = P[0].b.cpm_calls;
                rb_cprec st = {1, 1, 3, 0x91010100u, 256, 4};
                uint8_t m[24];
                CHECK(rb_msg_cppass(m, sizeof m, 9, &st) == RB_CPPASS_MSG_LEN && rb_on_event(&P[0].b, m, RB_CPPASS_MSG_LEN) == 1);
                cpm_run(e, lv, 300);
                CHECK(e[0].r.id == 2 && P[0].b.cpm_calls == calls && !P[0].b.cpm_in.have);
            }
        }
        CHECK(both1 == 1 && old1 == 0);
        printf("  task 4: CP mID 1 on both after the host's pass: %d (before the fix the guest's record stays mID 0: %d)\n", both1, !old1);
        /* e1p2: the guest's record was CLEARED by the mirrored 4390; the host's pass restores it */
        sim_start();
        cpeng e[2];
        CPENG_INIT(e);
        for (int k = 0; k < 2; k++) eng_pass(&e[k], 0, 256, 4, 0, 0x91010100u);
        cpm_run(e, lv, 300);
        e[1].r = (rb_cprec){1, -1, 0, 0x91FF0000u, -1, -1};   /* sCheckPoint::cData_vf7 */
        eng_pass(&e[0], 1, 256, 4, 0, 0x91010100u);
        cpm_run(e, lv, 1500);
        CHECK(e[1].r.id == 1 && e[1].r.area == 0x91010100u && e[1].r.a == 256 && e[1].r.b == 4);
        /* the guest's save manager busy: refused, tried again every 500 ms, taken when it is free */
        sim_start();
        CPENG_INIT(e);
        for (int k = 0; k < 2; k++) eng_pass(&e[k], 0, 256, 4, 0, 0x91010100u);
        cpm_run(e, lv, 300);
        e[1].busy = 1;
        eng_pass(&e[0], 1, 256, 4, 0, 0x91010100u);
        cpm_run(e, lv, 2600);
        CHECK(e[1].r.id == 0 && e[1].refused >= 2 && e[1].refused <= 4 && P[1].b.cpm_in.have);
        e[1].busy = 0;
        cpm_run(e, lv, 600);
        CHECK(e[1].r.id == 1 && !P[1].b.cpm_in.have);
        /* the guest still loading (no level): kept, taken once its level is playable; never after 15 s */
        sim_start();
        CPENG_INIT(e);
        for (int k = 0; k < 2; k++) eng_pass(&e[k], 0, 256, 4, 0, 0x91010100u);
        cpm_run(e, lv, 300);
        int lw[2] = {1, 0};
        eng_pass(&e[0], 1, 256, 4, 0, 0x91010100u);
        cpm_run(e, lw, 3000);
        CHECK(e[1].r.id == 0 && P[1].b.cpm_in.have);
        lw[1] = 1;
        cpm_run(e, lw, 300);
        CHECK(e[1].r.id == 1);
        eng_pass(&e[0], 2, 256, 5, 0, 0x91010100u);
        lw[1] = 0;
        cpm_run(e, lw, 16000);
        CHECK(e[1].r.id == 1 && !P[1].b.cpm_in.have && P[1].b.cpm_dropped == 1);
        /* another section on the guest: not taken; the mod's own record write (the restart's cp_take) is not told */
        sim_start();
        CPENG_INIT(e);
        eng_pass(&e[0], 0, 256, 4, 0, 0x91010100u), eng_pass(&e[1], 0, 512, 0, 0, 0x91010200u);
        cpm_run(e, lv, 300);
        eng_pass(&e[0], 1, 256, 4, 0, 0x91010100u);
        cpm_run(e, lv, 1500);
        CHECK(e[1].r.id == 0 && e[1].r.area == 0x91010200u && P[1].b.cpm_other == 1);
        rb_cprec mk = {1, 1, 3, 0x91010100u, 256, 4};
        rb_cpm_mark(&P[1].b, &mk);
        e[1].r = mk;
        unsigned sent = P[1].b.cpm_sent;
        cpm_run(e, lv, 300);
        CHECK(P[1].b.cpm_sent == sent);
    }

    /* ============================================ hooks, image facts, glue wiring */
    {
        static const uint8_t ls[7] = {0x51, 0x56, 0x57, 0x8B, 0xF9, 0x33, 0xF6}, ct[7] = {0xA1, 0x10, 0x91, 0x16, 0x01, 0x85, 0xC0};
        CHECK(RBK_N == 11 && rbk_defs[RBK_LSTART].va == 0x41CFC0u && !memcmp(rbk_defs[RBK_LSTART].sig, ls, 7) && rbk_defs[RBK_LSTART].steal == 7 &&
              rbk_defs[RBK_LSTART].kind == RBK_POST && rbk_defs[RBK_LSTART].hold_ret == -1 && !strcmp(rbk_defs[RBK_LSTART].name, "lstart"));
        CHECK(rbk_defs[RBK_CONT].va == 0x41EFE0u && !memcmp(rbk_defs[RBK_CONT].sig, ct, 7) && rbk_defs[RBK_CONT].steal == 7 && rbk_defs[RBK_CONT].kind == RBK_PRE &&
              rbk_defs[RBK_CONT].hold_ret == 0 && !strcmp(rbk_defs[RBK_CONT].name, "cont"));
        uint8_t out[128];
        int n = rbk_stub_build(out, sizeof out, &rbk_defs[RBK_CONT], 0x70000000u, 0x12345678u, 0);
        CHECK(n > 0 && out[n - 1] == 0xC3 && out[n - 2] == 0xC0 && out[n - 3] == 0x31);   /* hold: xor eax,eax / ret */
        n = rbk_stub_build(out, sizeof out, &rbk_defs[RBK_LSTART], 0x70000000u, 0x12345678u, 0x70000068u);
        CHECK(n > 0 && out[0] == 0x89 && out[1] == 0x0D);   /* POST: mov [cell],ecx */
        FILE *f = fopen("../../image/rerev-image.bin", "rb");
        if (f) {
            uint8_t b[16];
#define AT(va, len) (fseek(f, (long)((va) - 0x400000u), SEEK_SET) == 0 && fread(b, 1, (len), f) == (len))
            CHECK(AT(0x41CFC0u, 7) && !memcmp(b, ls, 7));
            CHECK(AT(0x41EFE0u, 7) && !memcmp(b, ct, 7));
            static const uint8_t cmp18[6] = {0x83, 0x78, 0x18, 0x00, 0x0F, 0x8F};   /* FUN_0041c8d0: cmp dword [eax+0x18],0 / jg (skip the movie) after FUN_006d0b90 */
            CHECK(AT(0x41C9A8u, 6) && !memcmp(b, cmp18, 6));
            CHECK(AT(0x41C9A3u, 1) && b[0] == 0xE8);
            int32_t rel;
            CHECK(AT(0x6D0E5Fu, 5) && b[0] == 0xE8 && (memcpy(&rel, b + 1, 4), 0x6D0E64u + (uint32_t)rel == 0x720870u));   /* the stock Continue's restart call, return 0x6D0E64 */
            CHECK(AT(0x422772u, 5) && b[0] == 0xE8 && (memcpy(&rel, b + 1, 4), 0x422777u + (uint32_t)rel == 0x41EFE0u));
            CHECK(AT(0x4229E0u, 5) && b[0] == 0xE8 && (memcpy(&rel, b + 1, 4), 0x4229E5u + (uint32_t)rel == 0x41EFE0u));
            static const uint8_t dti[5] = {0x68, 0xE4, 0xB3, 0x16, 0x01};   /* push DTI_aStartContinue 0x116B3E4 */
            CHECK(AT(0x41F028u, 5) && !memcmp(b, dti, 5));
            /* task 4: the engine's own checkpoint pass that cp_pass repeats: FUN_006d1e90 builds a cZoneContentsCheckPoint at [esp+0x10] (ctor 0x8D5400), mID 0 at
             * +0x10 ([esp+0x20]), A at +0x1C ([esp+0x2c]), B at +0x1E ([esp+0x2e]), push it, ecx = sCheckPoint, call 0x6D1A70 (ret 4 at 0x6D1E7F), dtor 0x8D4F40 */
            static const uint8_t pass_sig[12] = {0x51, 0xA1, 0x08, 0x91, 0x16, 0x01, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xE9}, ctor_sig[5] = {0x56, 0x57, 0x8B, 0xF1, 0xE8},
                                 dtor_sig[6] = {0xC7, 0x01, 0xCC, 0xC1, 0x09, 0x01}, ret4[3] = {0xC2, 0x04, 0x00};
            CHECK(AT(0x6D1A70u, 12) && !memcmp(b, pass_sig, 12) && AT(0x8D5400u, 5) && !memcmp(b, ctor_sig, 5) && AT(0x8D4F40u, 6) && !memcmp(b, dtor_sig, 6));
            CHECK(AT(0x6D1E7Fu, 3) && !memcmp(b, ret4, 3));
            static const uint8_t mid0[5] = {0x66, 0x89, 0x54, 0x24, 0x20}, sta[5] = {0x66, 0x89, 0x44, 0x24, 0x2C}, stb[5] = {0x66, 0x89, 0x4C, 0x24, 0x2E};
            CHECK(AT(0x6D1F1Eu, 5) && !memcmp(b, mid0, 5) && AT(0x6D1F27u, 5) && !memcmp(b, sta, 5) && AT(0x6D1F34u, 5) && !memcmp(b, stb, 5));
            CHECK(AT(0x6D1F3Cu, 5) && b[0] == 0xE8 && (memcpy(&rel, b + 1, 4), 0x6D1F41u + (uint32_t)rel == 0x6D1A70u));
#undef AT
            fclose(f);
            printf("ok   image: the hook bytes, FUN_0041c8d0's record test, the stock Continue's callers\n");
        } else
            printf("skip image checks (../../image/rerev-image.bin not found)\n");
        char *g = slurp("../native/rolebarrier1_game.c"), *w = slurp("../native/world1_game.c");
        CHECK(g && w);
        if (g && w) {
            CHECK(strstr(g, "int hold = rb_lstart_gate(&B, 1) == RB_HOLD;") && strstr(g, "return hold ? (result & ~0xFFu) : result;"));
            CHECK(strstr(g, "cp_read(&B.tx_cp);") && strstr(g, "if (cp_take_it.have) cp_take(&cp_t, &cp_take_it);"));
            CHECK(strstr(g, "B.call_stock = f->ret_addr == 0x6D0E64u;") && strstr(g, "rb_cont_request(&B, ours) == RB_HOLD"));
            CHECK(strstr(g, "rb_held_step(&B, &held_t, &held_host)") && strstr(g, "rb_cont_request_step(&B)") && strstr(g, "g_shield = (want && active) ? rb_shield_on(&B) : 0;"));
            CHECK(strstr(g, "case RBK_LSTART: return (uint32_t)(uintptr_t)cb_lstart;") && strstr(g, "case RBK_CONT: return (uint32_t)(uintptr_t)cb_cont;"));
            CHECK(strstr(g, "if (try_stock && !blk && ask_stock_continue())"));
            CHECK(strstr(g, "cp_call = rb_cpm_step(&B, &cpo, lvl, &cpa);") && strstr(g, "int called = cp_pass(&cpa, why, sizeof why);") && strstr(g, "rb_cpm_applied(&B, called, &after);"));
            CHECK(strstr(g, "rb_cpm_mark(&B, &m);"));
            CHECK(count_of(w, "rolebarrier_shielded()") == 2 && strstr(w, "rolebarrier_shield_drop(0);") && strstr(w, "rolebarrier_shield_drop(1);"));
        }
        free(g), free(w);
    }

    printf("test_continue1: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
