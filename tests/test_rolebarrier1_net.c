/*
 * The role / barrier / restart logic (native/rolebarrier1.c) over the REAL link code: two lobby_link.c links on the in-process "sim" transport (the coop session, roles, START, the
 * load barrier LOAD_BEGIN / GO, reliable CP_EVENTs, the event callback lk_add_event_cb). The glue of the game (rolebarrier1_game.c) does the same with the same calls; only the memory
 * of the game is missing here. Real time, a few seconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_net.h"
#include "../native/lobby_link.h"
#include "../native/rolebarrier1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t len; uint8_t body[RB_MSG_MAX]; } evq_t;
typedef struct {
    lk_t *k;
    rb_t b;
    evq_t q[16];
    int qh, qt;
    unsigned foreign_events;
} pc_t;
static pc_t P[2];

static void on_event(void *u, const cp_event *e)
{
    pc_t *p = (pc_t *)u;
    if (e->kind != CP_EV_RB_KIND) {
        p->foreign_events++;
        return;
    }
    p->q[p->qh].len = e->len, memcpy(p->q[p->qh].body, e->body, e->len);
    p->qh = (p->qh + 1) % 16;
}
static uint32_t io_now(void *u) { (void)u; return GetTickCount(); }
static int io_session(void *u, rb_sinfo *o)
{
    pc_t *p = (pc_t *)u;
    lk_info in;
    lk_get_info(p->k, &in);
    if (!lk_active(p->k) || !in.started) return 0;
    o->started = 1, o->phase = in.phase, o->epoch = (unsigned)in.epoch, o->gos = in.gos;
    return 1;
}
static int io_send(void *u, const void *body, unsigned len) { pc_t *p = (pc_t *)u; cs_t *s = lk_session(p->k); return s && cs_send_event(s, CP_EV_RB_KIND, body, len); }
static void io_begin(void *u, uint32_t key) { lk_load_begin(((pc_t *)u)->k, key); }
static void io_done(void *u) { lk_load_done(((pc_t *)u)->k); }
static void io_log(void *u, const char *l) { (void)u; (void)l; }

static void pump_one(void)
{
    for (int i = 0; i < 2; i++) {
        lk_poll(P[i].k);
        while (P[i].qt != P[i].qh) {
            rb_on_event(&P[i].b, P[i].q[P[i].qt].body, P[i].q[P[i].qt].len);
            P[i].qt = (P[i].qt + 1) % 16;
        }
    }
    Sleep(2);
}
static void pump(int ms) { for (int t = 0; t < ms; t += 2) pump_one(); }
static int phase_of(int i) { lk_info in; lk_get_info(P[i].k, &in); return in.phase; }
static int gos_of(int i) { lk_info in; lk_get_info(P[i].k, &in); return in.gos; }

int main(void)
{
    cn_sim_real_time();
    char own[64], err[160];
    memset(P, 0, sizeof P);
    lk_config manual;
    memset(&manual, 0, sizeof manual);
    P[0].k = lk_new_cfg(NULL, "sim", 0, &manual);
    P[1].k = lk_new_cfg(NULL, "sim", 0, &manual);
    CHECK(P[0].k && P[1].k);
    rb_cfg cfg;
    rb_defaults(&cfg);
    cfg.settle_ms = 20, cfg.unplay_ms = 30, cfg.stale_epoch_ms = 400, cfg.latch_ms = 5000;   /* Sleep(2) takes ~15 ms on Windows: the test pumps slower than a game */
    for (int i = 0; i < 2; i++) {
        rb_io io = {io_now, io_session, io_send, io_begin, io_done, io_log, &P[i]};
        rb_init(&P[i].b, &io, &cfg);
        P[i].b.enabled = 1;
        CHECK(lk_add_event_cb(P[i].k, on_event, &P[i]) == 1);
        CHECK(lk_add_event_cb(P[i].k, on_event, &P[i]) == 1);   /* twice: still one registration */
    }
    P[0].b.host = 1;

    /* ---- connect, roles, start */
    CHECK(lk_start(P[0].k, L1N_HOST, "", LK_DEFAULT_PORT, own, sizeof own, err, sizeof err) == 1);
    CHECK(lk_start(P[1].k, L1N_JOIN, "sim:28410", LK_DEFAULT_PORT, own, sizeof own, err, sizeof err) == 1);
    pump(200);
    lk_set_ready(P[0].k, 1);
    lk_set_ready(P[1].k, 1);
    pump(200);
    pump(300);
    lk_info a, b;
    lk_get_info(P[0].k, &a), lk_get_info(P[1].k, &b);
    CHECK(a.started && b.started);
    CHECK(a.role == 0 && b.role == 1);
    {   /* the role plan of both PCs */
        rb_role_t r0, r1;
        rb_role_act x;
        rb_role_init(&r0), rb_role_init(&r1);
        CHECK(rb_role_step(&r0, a.started, a.role, &x) == RBR_APPLY && x.own_req == -1 && x.role == 0);
        CHECK(rb_role_step(&r1, b.started, b.role, &x) == RBR_APPLY && x.own_req == 1 && x.role == 1);
        CHECK(a.host == 1 && b.host == 0);
    }
    CHECK(phase_of(0) == 2 && phase_of(1) == 2);   /* LOADING, epoch 1 */

    /* ---- the first load: the host's level is up at once, the partner's later; GO only when both reported */
    {
        int g0 = gos_of(0);
        for (int t = 0; t < 300; t += 2) {
            rb_load_step(&P[0].b, 1);
            rb_load_step(&P[1].b, 0);
            pump_one();
        }
        CHECK(phase_of(0) == 2 && gos_of(0) == g0);
        for (int t = 0; t < 400 && phase_of(0) != 3; t += 2) {
            rb_load_step(&P[0].b, 1);
            rb_load_step(&P[1].b, 1);
            pump_one();
        }
        pump(60);
        CHECK(phase_of(0) == 3 && phase_of(1) == 3 && gos_of(0) == g0 + 1 && gos_of(1) == gos_of(0));
    }

    /* ---- events: a rendezvous at the end of a section over the real session */
    {
        int held = 0, freed = 0;
        for (int t = 0; t < 200; t += 2) {
            held += rb_gate(&P[0].b, RB_SECTION, 0) == RB_HOLD;   /* the host reached it, the partner did not */
            pump_one();
        }
        CHECK(held == 100 && P[0].b.arrivals_tx == 1);
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);     /* the partner arrives: the host's arrival came through the real line */
        for (int t = 0; t < 400 && !freed; t += 2) {
            pump_one();
            freed = rb_gate(&P[0].b, RB_SECTION, 0) == RB_PROCEED;
        }
        CHECK(freed && P[0].b.released_peer == 1 && P[1].b.released_peer == 1 && P[1].b.arrivals_rx == 1 && P[0].b.arrivals_rx == 1);
        /* a cutscene end, the other way round */
        rb_pcs_begin(&P[0].b, rb_hash("11_100_0"), 1), rb_pcs_begin(&P[1].b, rb_hash("11_100_0"), 1);
        CHECK(rb_pcs_end(&P[1].b, 1) == 0);
        pump(40);
        CHECK(rb_pcs_end(&P[0].b, 1) == 1);
        pump(40);
        CHECK(rb_pcs_end(&P[1].b, 1) == 1);
        CHECK(P[0].foreign_events == 0 && P[1].foreign_events == 0);
        /* an event of another kind (the world sync of Episode 1) is handed to the callback and left alone by this module */
        CHECK(cs_send_event(lk_session(P[0].k), CP_EV_W1, "x", 1) == 1);
        pump(40);
        CHECK(P[1].foreign_events == 1);
    }

    /* ---- Retry / Continue: the host's triple reaches the client over the session and rewrites its own */
    {
        rb_triple h = {0x91010200u, 1, 2}, own_t = {0x91010100u, 2, 1};
        CHECK(rb_restart_host(&P[0].b, &h) == 1);
        pump(60);
        CHECK(P[1].b.rs_have);
        CHECK(rb_restart_client(&P[1].b, &own_t) == 1 && own_t.area == 0x91010200u && own_t.a == 1 && own_t.b == 2);
        CHECK(!P[0].b.rs_have);   /* the host does not take its own triple back */
        rb_triple t2 = {0x91010100u, 0, 0};
        CHECK(rb_restart_host(&P[0].b, &t2) == 0);   /* both zero: not sent */
    }

    /* ---- lobby-must: the host's story start (FUN_00720540 observer, title New Game) reaches the client over the real session; the client at the title follows
     * it once, also when it is older than the in-level freshness limit (a late guest); a section-change caller sends nothing */
    {
        rb_triple rec = {0x91010100u, 7, 9}, out;
        unsigned sent = P[0].b.rs_sent;
        CHECK(rb_start_seen(&P[0].b, 0x42D47Cu) == 0);   /* cEpisodeEndState: the section change */
        CHECK(rb_start_seen(&P[0].b, 0x83E89Bu) == 1 && rb_start_triple(&P[0].b, &rec) == RB_ST_SENT && P[0].b.rs_sent == sent + 1);
        pump(60);
        CHECK(P[1].b.rs_have && P[1].b.rs_area == 0x91010100u && P[1].b.rs_a == 7 && P[1].b.rs_b == 9);
        CHECK(rb_start_seen(&P[1].b, 0x83E89Bu) == 0);   /* the client's own start writes are never sent */
        Sleep(1600);   /* the real clock of the link (io_now): older than follow_ms */
        CHECK(rb_restart_follow_at(&P[1].b, &out, 1) == 1 && out.area == 0x91010100u && out.a == 7 && out.b == 9);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 1) == 0);
        rb_triple t = out;   /* the engine call of the follow passes the client's restart hook with the same triple */
        CHECK(rb_restart_client(&P[1].b, &t) == 1 && t.a == 7 && !P[1].b.rs_have);
    }

    /* ---- ep1-flow E4 / E5 over the real session: the host's section change (no rendezvous: the client did not reach the end) sends the next section, the client
     * follows it once with the start triple; the end of 1-3 on the host tells the client (EPDONE), which then follows nothing */
    {
        rb_triple s12 = {0x91010200u, 512, 0}, out;
        P[0].b.pt[RB_SECTION].st = 0, P[1].b.pt[RB_SECTION].st = 0;
        P[1].b.cfg.sec_follow_ms = 2500;   /* pump(60) takes about 0.5 s of real time */
        CHECK(rb_section_change(&P[0].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_SENT);
        pump(60);
        CHECK(P[1].b.sec_pending && P[1].b.rs_area == 0x91010200u && P[1].b.rs_a == 512 && P[1].b.rs_b == 0 && P[1].b.sec_from == 0x91010100u);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0);   /* younger than sec_follow_ms */
        Sleep(2600);
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == RB_FOL_SECEND && P[1].b.sec_stock_count == 1);   /* (rev1 live 10) its level (1-1) is the section the host left: its own stock end */
        CHECK(rb_section_change(&P[1].b, 0x91010100u, 0x91010200u, &s12) == RB_SC_NOTED && !P[1].b.sec_pending && P[1].b.sec_consumed == 1);   /* that change consumes the triple */
        CHECK(rb_restart_follow_at(&P[1].b, &out, 0) == 0 && P[1].b.sec_followed == 0);
        CHECK(rb_section_change(&P[0].b, 0x91010300u, 0x91020100u, NULL) == RB_SC_EPEND && P[0].b.ep_local);
        pump(60);
        int loc = -1;
        CHECK(P[1].b.ep_peer && rb_ep_known(&P[1].b, &loc) == 1 && loc == 0 && P[1].b.ep_got == 1);
        rb_triple h = {0x91010300u, 257, 1};
        CHECK(rb_restart_host(&P[0].b, &h) == 1);
        pump(60);
        CHECK(!P[1].b.rs_have);   /* after the end of Episode 1 nothing is followed */
        CHECK(P[0].foreign_events == 0);
        P[0].b.ep_local = P[0].b.ep_peer = P[1].b.ep_local = P[1].b.ep_peer = 0;   /* (the load barrier blocks below do not depend on it) */
    }

    /* ---- the next level: both lose the level, begin, load, GO; epoch 2 */
    {
        int g0 = gos_of(0);
        int ep0 = ({ lk_info x; lk_get_info(P[0].k, &x); x.epoch; });
        for (int t = 0; t < 300 && phase_of(0) == 3; t += 2) {
            rb_load_step(&P[0].b, 1);
            rb_load_step(&P[1].b, 1);
            pump_one();
        }
        for (int t = 0; t < 200; t += 2) {
            rb_load_step(&P[0].b, 0);
            rb_load_step(&P[1].b, 0);
            pump_one();
        }
        CHECK(phase_of(0) == 2 && phase_of(1) == 2);
        CHECK(({ lk_info x; lk_get_info(P[0].k, &x); x.epoch; }) == ep0 + 1);
        for (int t = 0; t < 600 && phase_of(0) != 3; t += 2) {
            rb_load_step(&P[0].b, 1);
            rb_load_step(&P[1].b, t > 100);
            pump_one();
        }
        pump(60);
        CHECK(phase_of(0) == 3 && phase_of(1) == 3 && gos_of(0) == g0 + 1);
        CHECK(P[0].b.loads_stale == 0 && P[1].b.loads_stale == 0);
    }

    /* ---- the session ends: the plan releases the role, the rendezvous stops holding */
    {
        rb_role_t r;
        rb_role_act x;
        rb_role_init(&r);
        rb_role_step(&r, 1, 1, &x);
        lk_stop(P[0].k);
        pump(100);
        lk_get_info(P[1].k, &b);
        rb_sinfo si;
        int ok = io_session(&P[1], &si);
        CHECK(!ok);
        CHECK(rb_role_step(&r, ok, b.role, &x) == RBR_RELEASE);
        P[1].b.pt[RB_SECTION].st = 0;
        CHECK(rb_gate(&P[1].b, RB_SECTION, 0) == RB_PROCEED);
    }

    lk_free(P[0].k), lk_free(P[1].k);
    printf("test_rolebarrier1_net: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
