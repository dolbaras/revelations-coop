/*
 * Session-layer tests on the in-process "sim" transport with a virtual clock: handshake and version gate, refusals, lobby rules,
 * start, load barrier, stale-epoch drop, state streaming under loss / duplication / reordering with interpolation error,
 * three-player relay, leave / reconnect / host loss, malformed input. Deterministic (seeded), runs in a second.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_session.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t vclock = 1000;

typedef struct {
    cs_t *s;
    cn_t *raw;           /* a hand-driven transport endpoint instead of a session (protocol-level tests) */
    uint32_t offset;     /* this machine's clock = vclock + offset (different uptimes) */
    int count[16];       /* notifications by kind */
    cs_note last[16];
    char text[16][160];
    char pmod[16][24];   /* peer_mod of the last note of each kind */
    cp_event ev;         /* last event received */
    int ev_n;
    int ev_char;         /* rev1 live 16: this node's own character when the last event arrived */
} Node;

static uint32_t clk(void *u) { return vclock + ((Node *)u)->offset; }
static void on_note(void *u, const cs_note *n)
{
    Node *nd = (Node *)u;
    if (n->what >= 0 && n->what < 16) {
        nd->count[n->what]++;
        nd->last[n->what] = *n;
        snprintf(nd->text[n->what], sizeof nd->text[n->what], "%s", n->text ? n->text : "");
        nd->last[n->what].text = nd->text[n->what];
        snprintf(nd->pmod[n->what], sizeof nd->pmod[n->what], "%s", n->peer_mod ? n->peer_mod : "(none)");
        nd->last[n->what].peer_mod = nd->pmod[n->what];
    }
    if (n->what == CS_N_EVENT && n->ev) nd->ev = *n->ev, nd->ev_n++, nd->ev_char = nd->s ? cs_local_character(nd->s) : -1;
}

static void node_init(Node *nd, const char *name, int want, uint32_t offset)
{
    memset(nd, 0, sizeof *nd);
    nd->offset = offset;
    cs_config c;
    memset(&c, 0, sizeof c);
    c.name = name, c.want_char = want, c.mod_version = "test", c.is_bot = 1, c.clock = clk, c.clock_user = nd, c.on_note = on_note, c.user = nd;
    nd->s = cs_new(&c);
}

static Node *all_nodes[8];
static int n_all;
static void reg(Node *nd) { all_nodes[n_all++] = nd; }
static void unreg_all(void) { n_all = 0; }

static void step(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        vclock += 5;
        cn_sim_set_time(vclock);
        for (int i = 0; i < n_all; i++) {
            if (all_nodes[i]->s) cs_poll(all_nodes[i]->s);
            if (all_nodes[i]->raw) cn_pump(all_nodes[i]->raw);
        }
    }
}

static void faults(int loss, int dup, int reorder, int delay, int jitter, uint32_t seed)
{
    cn_sim_params p;
    memset(&p, 0, sizeof p);
    p.loss_pct = loss, p.dup_pct = dup, p.reorder_pct = reorder, p.delay_ms = delay, p.jitter_ms = jitter, p.reorder_extra_ms = 60, p.seed = seed;
    cn_sim_set(&p);
}

static cp_state path_state(int pid, uint32_t t_ms)
{
    /* a circle of radius 300 around (1000*pid, 0, 0), 200 units/s, a walking action */
    cp_state s;
    memset(&s, 0, sizeof s);
    double a = (double)t_ms / 1000.0 * (200.0 / 300.0);
    s.pos[0] = (float)(1000.0 * pid + 300.0 * cos(a)), s.pos[1] = 0, s.pos[2] = (float)(300.0 * sin(a));
    s.yaw = (float)fmod(a + 1.5707963, 6.2831853) - ((fmod(a + 1.5707963, 6.2831853) > 3.1415927) ? 6.2831853f : 0.0f);
    s.action = 3, s.hp = 800, s.hpmax = 800, s.weapon = 2;
    return s;
}

static int connect_pair(Node *h, Node *c, int port)
{
    char err[200], target[32];
    snprintf(target, sizeof target, "sim:%d", port);
    if (!cs_host(h->s, "sim", port, 0, err, sizeof err)) { printf("host: %s\n", err); return 0; }
    if (!cs_join(c->s, "sim", target, 0, err, sizeof err)) { printf("join: %s\n", err); return 0; }
    step(100);
    return c->count[CS_N_CONNECTED] == 1;
}

static void start_game(Node *h, Node *c)
{
    char err[100];
    cs_ready(c->s, 1);
    step(100);
    CHECK(cs_start(h->s, 0, err, sizeof err));
    step(100);
    cs_load_done(h->s);
    cs_load_done(c->s);
    step(100);
}

/* rev1 live 16 (the lead's decision: Continue keeps the roles of the co-op save): the host's title Continue of a co-op save made in the other order changes the
 * roles after the START, before the first level (cs_force_roles). The model before the fix (cs_assign_roles = the room's own role change) refuses after the
 * START: the partner keeps the swapped character, whose bag its co-op save does not hold (m15l7 LIVE: the guest's Parker unarmed). */
static void test_force_roles(void)
{
    printf("-- the roles put back after the start, before the first level (rev1 live 16)\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 0, 0, 3);
    CHECK(connect_pair(&h, &c, 7));
    char err[100];
    CHECK(cs_ready(c.s, 1));
    step(60);
    CHECK(cs_start(h.s, 0, err, sizeof err));
    step(100);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING && CP_CHAR_SLOT(cs_local_character(h.s)) == 0 && CP_CHAR_SLOT(cs_local_character(c.s)) == 1);
    CHECK(!cs_assign_roles(h.s, 1));   /* the model before the fix: no role change after the START */
    step(60);
    CHECK(CP_CHAR_SLOT(cs_local_character(h.s)) == 0 && CP_CHAR_SLOT(cs_local_character(c.s)) == 1);
    int lob0 = c.count[CS_N_LOBBY];
    CHECK(cs_force_roles(c.s, 0) == -1);   /* the host only */
    CHECK(cs_force_roles(h.s, 2) == -1);   /* a bad slot */
    CHECK(cs_force_roles(h.s, 1) == 1);
    static const uint8_t body[4] = {1, 2, 3, 4};
    CHECK(cs_send_event(h.s, CP_EV_USER, body, sizeof body));   /* what the host sends next (its start triple) */
    step(60);
    CHECK(CP_CHAR_SLOT(cs_local_character(h.s)) == 1 && CP_CHAR_SLOT(cs_local_character(c.s)) == 0);   /* both PCs */
    CHECK(c.count[CS_N_LOBBY] > lob0 && c.ev_n == 1 && c.ev_char >= 0 && CP_CHAR_SLOT(c.ev_char) == 0);   /* the new table came first (one reliable channel) */
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && pi.ready);   /* the ready flags stay: the session has started */
    CHECK(cs_force_roles(h.s, 1) == 0);           /* already so */
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING && c.count[CS_N_START] == 1 && cs_epoch(c.s) == 1);
    cs_load_done(c.s);   /* a level of this session was loaded: the roles stay from now on */
    step(60);
    CHECK(cs_force_roles(h.s, 0) == -1);
    cs_load_done(h.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_force_roles(h.s, 0) == -1);
    CHECK(CP_CHAR_SLOT(cs_local_character(h.s)) == 1 && CP_CHAR_SLOT(cs_local_character(c.s)) == 0);
    cs_free(h.s), cs_free(c.s);
}

/* ------------------------------------------------------------------------------------------------------------------ */
static void test_handshake_lobby_start(void)
{
    printf("-- handshake, lobby, start, barrier\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 0x7FFFF000u);   /* a client whose clock is 24 days ahead (wraps through 2^31 and 2^32) */
    reg(&h), reg(&c);
    faults(0, 0, 0, 0, 0, 1);
    CHECK(connect_pair(&h, &c, 1));
    CHECK(cs_pid(c.s) == 1 && cs_pid(h.s) == 0);
    CHECK(cs_phase(h.s) == CP_PHASE_LOBBY && cs_phase(c.s) == CP_PHASE_LOBBY);
    CHECK(cs_local_character(h.s) == CP_CHAR_CLAIRE && cs_local_character(c.s) == CP_CHAR_MOIRA);   /* default: host Claire, client Moira */
    CHECK(h.count[CS_N_PEER_JOIN] == 1 && cs_player_count(h.s) == 2 && cs_player_count(c.s) == 2);

    CHECK(cs_pick(c.s, CP_CHAR_CLAIRE));   /* asked; the host refuses with a notice */
    step(60);
    CHECK(c.count[CS_N_NOTICE] == 1 && c.last[CS_N_NOTICE].a == CP_NOTE_SLOT_TAKEN);
    CHECK(cs_local_character(c.s) == CP_CHAR_MOIRA);
    CHECK(cs_pick(c.s, CP_CHAR_BARRY));    /* Barry is the same slot as Claire */
    step(60);
    CHECK(c.count[CS_N_NOTICE] == 2 && cs_local_character(c.s) == CP_CHAR_MOIRA);
    CHECK(cs_pick(c.s, CP_CHAR_NATALIA));  /* the Barry/Natalia pair: Natalia is slot 1, free for the client */
    step(60);
    CHECK(cs_local_character(c.s) == CP_CHAR_NATALIA && cs_local_character(h.s) == CP_CHAR_CLAIRE);
    CHECK(!cs_pick(h.s, CP_CHAR_MOIRA));   /* the host cannot take the client's slot */
    CHECK(cs_pick(c.s, CP_CHAR_MOIRA));
    step(60);
    CHECK(cs_local_character(c.s) == CP_CHAR_MOIRA);

    char err[100];
    CHECK(!cs_start(h.s, 0, err, sizeof err));   /* the client is not ready */
    CHECK(strstr(err, "ready") != NULL);
    cp_session ss;
    cs_session_get(h.s, &ss);
    ss.episode = 3, ss.part = 1, ss.difficulty = 2;
    CHECK(cs_set_session(h.s, &ss));
    step(60);
    cs_session_get(c.s, &ss);
    CHECK(ss.episode == 3 && ss.part == 1 && ss.difficulty == 2 && ss.id == (cs_session_get(h.s, &ss), ss.id));
    CHECK(cs_ready(c.s, 1));
    step(60);
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && pi.ready && pi.connected);
    CHECK(cs_start(h.s, 0, err, sizeof err));
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING && cs_epoch(h.s) == 1 && cs_epoch(c.s) == 1);
    CHECK(h.count[CS_N_START] == 1 && c.count[CS_N_START] == 1);
    CHECK(!cs_pick(c.s, CP_CHAR_CLAIRE));   /* locked after the start */

    /* barrier: GO only when both loaded */
    cp_state st = path_state(0, 0);
    CHECK(!cs_submit_state(h.s, &st));       /* no state while loading */
    cs_load_done(h.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    cs_load_done(c.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    CHECK(h.count[CS_N_GO] == 1 && c.count[CS_N_GO] == 1);

    /* heartbeats give an RTT (sim delay 0 -> 0..10 ms) */
    step(2500);
    CHECK(cs_player(h.s, 1, &pi) && pi.rtt_ms >= 0 && pi.rtt_ms <= 20);
    CHECK(cs_player(c.s, 0, &pi) && pi.rtt_ms >= 0 && pi.rtt_ms <= 20);
    char text[1200];
    cs_status(h.s, text, sizeof text);
    CHECK(strstr(text, "HOST") && strstr(text, "partner") && strstr(text, "playing"));
    cs_free(h.s), cs_free(c.s);
}

static void test_refusals(void)
{
    printf("-- refusals: version gate, build gate, full, garbage, hello first\n");
    Node h;
    unreg_all();
    cs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.name = "Host", cfg.want_char = CP_CHAR_CLAIRE, cfg.mod_version = "t", cfg.clock = clk, cfg.clock_user = &h, cfg.on_note = on_note, cfg.user = &h;
    for (int i = 0; i < 8; i++) cfg.exe_tag[i] = (uint8_t)(0xA0 + i);
    memset(&h, 0, sizeof h);
    h.s = cs_new(&cfg);
    reg(&h);
    faults(0, 0, 0, 0, 0, 1);
    char err[200];
    CHECK(cs_host(h.s, "sim", 2, 0, err, sizeof err));

    struct { const char *what; uint16_t ver, minv; int tag; int expect; } cases[] = {
        {"too new", 5, 4, 1, CP_REF_VERSION},
        {"too old", 0, 0, 1, CP_REF_VERSION},
        {"version 1 (40 byte states)", 1, 1, 1, CP_REF_VERSION},
        {"version 2 (no CP_PAD, the puppet would stand)", 2, 2, 1, CP_REF_VERSION},
        {"wrong build", 3, 3, 2, CP_REF_BUILD},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Node r;
        memset(&r, 0, sizeof r);
        r.raw = cn_new();
        reg(&r);
        CHECK(cn_join(r.raw, "sim", "sim:2", 0, err, sizeof err));
        step(30);
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_HELLO;
        m.u.hello.version = cases[i].ver, m.u.hello.min_version = cases[i].minv, m.u.hello.want_char = CP_CHAR_NONE;
        snprintf(m.u.hello.name, sizeof m.u.hello.name, "Raw%u", i);
        for (int k = 0; k < 8; k++) m.u.hello.exe_tag[k] = cases[i].tag == 1 ? (uint8_t)(0xA0 + k) : cases[i].tag == 2 ? 0x11 : 0;
        uint8_t buf[CP_MAX_PACKET];
        size_t n = cp_encode(&m, buf, sizeof buf);
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, buf, n);
        step(60);
        cn_event ev;
        int got = 0;
        while (cn_recv(r.raw, &ev)) {
            if (ev.type == CN_EV_RECEIVE && cp_decode(ev.data, ev.len, &m) && m.type == CP_REFUSE) {
                CHECK(m.u.refuse.code == cases[i].expect);
                got = 1;
            }
            cn_event_free(&ev);
        }
        if (!got) printf("  no REFUSE for case '%s'\n", cases[i].what);
        CHECK(got);
        CHECK(cs_player_count(h.s) == 1);   /* nobody was added */
        cn_free(r.raw);
        r.raw = NULL;
        unreg_all();
        reg(&h);
        step(40);
    }

    /* a matching hello is accepted (zero tag = bot skips the build check) */
    {
        Node r;
        memset(&r, 0, sizeof r);
        r.raw = cn_new();
        reg(&r);
        cn_join(r.raw, "sim", "sim:2", 0, err, sizeof err);
        step(30);
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_HELLO, m.u.hello.version = 3, m.u.hello.min_version = 3, m.u.hello.want_char = CP_CHAR_NONE;   /* a newer peer that still speaks 3 would be accepted the same way */
        snprintf(m.u.hello.name, sizeof m.u.hello.name, "NoTag");
        uint8_t buf[CP_MAX_PACKET];
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, buf, cp_encode(&m, buf, sizeof buf));
        step(60);
        CHECK(cs_player_count(h.s) == 2);
        /* garbage after the handshake: counted, no crash */
        uint8_t junk[] = {0x77, 1, 2, 3, 4};
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, junk, sizeof junk);
        cn_send(r.raw, 0, CP_CH_STATE, 0, junk, 3);
        step(40);
        cs_stats st;
        cs_get_stats(h.s, &st);
        CHECK(st.rx_bad == 2);
        cn_free(r.raw);
        unreg_all();
        reg(&h);
        step(40);
        CHECK(cs_player_count(h.s) == 1);   /* the raw peer left: lobby seat freed */
    }

    /* data before the hello is dropped; a peer that never says hello is cut after the deadline */
    {
        Node r;
        memset(&r, 0, sizeof r);
        r.raw = cn_new();
        reg(&r);
        cn_join(r.raw, "sim", "sim:2", 0, err, sizeof err);
        step(30);
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_LOBBY_PICK, m.u.pick.character = CP_CHAR_MOIRA;
        uint8_t buf[CP_MAX_PACKET];
        cn_send(r.raw, 0, CP_CH_CONTROL, 1, buf, cp_encode(&m, buf, sizeof buf));
        step(100);
        CHECK(cs_player_count(h.s) == 1);
        step(5200);
        CHECK(!cn_connected(r.raw, 0));
        cn_free(r.raw);
        unreg_all();
        reg(&h);
    }
    cs_free(h.s);

    /* full: a third player in a two-player session */
    {
        Node a, b, c;
        unreg_all();
        node_init(&a, "A", CP_CHAR_CLAIRE, 0), node_init(&b, "B", CP_CHAR_NONE, 0), node_init(&c, "C", CP_CHAR_NONE, 0);
        reg(&a), reg(&b), reg(&c);
        CHECK(connect_pair(&a, &b, 3));
        CHECK(cs_join(c.s, "sim", "sim:3", 0, err, sizeof err));
        step(100);
        CHECK(c.count[CS_N_REFUSED] == 1 && c.last[CS_N_REFUSED].a == CP_REF_FULL);
        CHECK(!cs_active(c.s) && cs_player_count(a.s) == 2);
        cs_free(a.s), cs_free(b.s), cs_free(c.s);
    }
}

static void test_barrier_epochs(void)
{
    printf("-- second load: client-initiated epoch, host joins it, stale states dropped\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 12345);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 4));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    /* a few states flow */
    for (int i = 0; i < 40; i++) {
        cp_state s = path_state(0, cs_now(h.s));
        cs_submit_state(h.s, &s);
        s = path_state(1, cs_now(c.s));
        cs_submit_state(c.s, &s);
        step(50);
    }
    cs_stats st;
    cs_get_stats(h.s, &st);
    CHECK(st.rx_state >= 35);
    /* the client starts loading level instance 2 */
    cs_load_begin(c.s, 700);
    step(80);
    CHECK(cs_epoch(c.s) == 2 && cs_epoch(h.s) == 2);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING && cs_phase(h.s) == CP_PHASE_LOADING);
    CHECK(h.count[CS_N_LOAD_BEGIN] >= 1 && h.last[CS_N_LOAD_BEGIN].pid == 1 && h.last[CS_N_LOAD_BEGIN].b == 700);
    /* the host's game loads too (its glue answers the notification with its own begin/done) */
    cs_load_begin(h.s, 700);
    CHECK(cs_epoch(h.s) == 2);   /* joins the announced epoch instead of advancing */
    cs_load_done(c.s);
    step(60);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);
    cs_load_done(h.s);
    step(80);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && c.count[CS_N_GO] == 2 && h.count[CS_N_GO] == 2);
    /* a state of the old epoch that arrives late is dropped as stale */
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_STATE;
    m.u.state = path_state(1, cs_now(c.s));
    m.u.state.pid = 1, m.u.state.seq = 60000, m.u.state.t_ms = cs_now(c.s), m.u.state.epoch = 1;
    uint8_t raw[64];
    size_t n = cp_encode(&m, raw, sizeof raw);
    cn_send(cs_transport(c.s), 0, CP_CH_STATE, 0, raw, n);
    step(60);
    cs_get_stats(h.s, &st);
    CHECK(st.rx_stale_epoch >= 1);
    /* both begin a load at the same moment (host-first): the epochs agree, no double increment */
    cs_load_begin(h.s, 800);
    cs_load_begin(c.s, 800);
    step(100);
    CHECK(cs_epoch(h.s) == 3 && cs_epoch(c.s) == 3);
    cs_load_done(h.s), cs_load_done(c.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(c.s);
}

static void test_stream_under_faults(const char *label, int loss, int dup, int reorder, int delay, int jitter, double max_err, double max_mean)
{
    printf("-- 20 Hz stream, %s: loss %d%% dup %d%% reorder %d%% delay %d+%d ms\n", label, loss, dup, reorder, delay, jitter);
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 0x80000100u);   /* clocks 24 days apart */
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 7);
    CHECK(connect_pair(&h, &c, 5));
    start_game(&h, &c);
    faults(loss, dup, reorder, delay, jitter, 99);
    double err_sum = 0, err_max = 0;
    int err_n = 0, none = 0, stale = 0;
    int sent = 0;
    for (int t = 0; t < 30000; t += 5) {
        if (t % 50 == 0) {   /* 20 Hz */
            cp_state s = path_state(0, cs_now(h.s));
            sent += cs_submit_state(h.s, &s);
            cp_state s2 = path_state(1, cs_now(c.s));
            cs_submit_state(c.s, &s2);
        }
        step(5);
        if (t > 3000 && t % 15 == 0) {   /* the game samples about 60 times a second */
            cp_state out;
            int r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
            if (r == CS_SAMPLE_NONE) { none++; continue; }
            if (r == CS_SAMPLE_STALE) stale++;
            cp_state truth = path_state(0, out.t_ms);   /* out.t_ms = the sender clock time being shown */
            double dx = out.pos[0] - truth.pos[0], dz = out.pos[2] - truth.pos[2], e = sqrt(dx * dx + dz * dz);
            err_sum += e, err_n++;
            if (e > err_max) err_max = e;
        }
    }
    cs_stats st;
    cs_get_stats(c.s, &st);
    cs_player_info pi;
    cs_player(c.s, 0, &pi);
    printf("   client received %u of %d sent (gap %u, dup dropped %u, old dropped %u); position error mean %.2f max %.2f units over %d samples; stale %d none %d\n", st.rx_state, sent,
           st.rx_gap, st.rx_dup, st.rx_old, err_n ? err_sum / err_n : -1, err_max, err_n, stale, none);
    CHECK(err_n > 1000 && none == 0);
    CHECK(err_max <= max_err);
    CHECK(err_n && err_sum / err_n <= max_mean);
    CHECK(st.rx_state + st.rx_gap >= (uint32_t)sent - 3);   /* every sent sample is either accepted or counted as a gap */
    if (dup) CHECK(st.rx_dup > 0);
    if (reorder) CHECK(st.rx_old > 0 || st.rx_gap > 0);
    if (loss) CHECK(st.rx_gap > 0);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(c.s);
}

static void test_interpolation_cases(void)
{
    printf("-- interpolation: teleport, extrapolation cap, stale, yaw wrap\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Client", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 0, 0, 1);
    CHECK(connect_pair(&h, &c, 6));
    start_game(&h, &c);
    cp_state s;
    memset(&s, 0, sizeof s);
    s.hp = 100, s.hpmax = 100;
    for (int i = 0; i < 10; i++) {
        s.pos[0] = 100.0f + 10.0f * (float)i, s.yaw = 3.0f + 0.05f * (float)i;   /* yaw crosses +pi */
        if (s.yaw > 3.1415927f) s.yaw -= 6.2831853f;
        cs_submit_state(h.s, &s);
        step(50);
    }
    cp_state out;
    int r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
    CHECK(r == CS_SAMPLE_INTERP || r == CS_SAMPLE_EXTRAP);
    CHECK(out.pos[0] > 100 && out.pos[0] <= 190);
    CHECK(fabsf(out.yaw) > 2.5f || fabsf(out.yaw) < 0.05f * 10);   /* never the average across the wrap (~0) of two values near +-pi */
    /* teleport: the sample after the jump is not blended */
    s.pos[0] = 5000.0f, s.flags = CP_SF_TELEPORT;
    cs_submit_state(h.s, &s);
    s.flags = 0;
    step(50);
    s.pos[0] = 5010.0f;
    cs_submit_state(h.s, &s);
    for (int i = 0; i < 5; i++) {
        step(30);
        cs_remote_sample(c.s, 0, cs_now(c.s), &out);
        CHECK(out.pos[0] < 195 || out.pos[0] > 4990);   /* never somewhere between */
    }
    /* silence: extrapolation is capped, then STALE after 1 s */
    step(300);
    r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
    CHECK(r == CS_SAMPLE_EXTRAP || r == CS_SAMPLE_INTERP);
    CHECK(out.pos[0] < 5010 + 100);
    step(1500);
    r = cs_remote_sample(c.s, 0, cs_now(c.s), &out);
    CHECK(r == CS_SAMPLE_STALE);
    CHECK(cs_remote_sample(c.s, 1, cs_now(c.s), &out) == CS_SAMPLE_NONE);   /* own pid / unknown */
    CHECK(cs_remote_sample(c.s, 3, cs_now(c.s), &out) == CS_SAMPLE_NONE);
    cs_free(h.s), cs_free(c.s);
}

static void test_three_players_relay_events(void)
{
    printf("-- three players: relay of state and events through the host\n");
    Node h, a, b;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&a, "A", CP_CHAR_NONE, 0);
    node_init(&b, "B", CP_CHAR_NONE, 777);
    reg(&h), reg(&a), reg(&b);
    faults(0, 0, 0, 5, 0, 1);
    char err[100];
    CHECK(cs_host(h.s, "sim", 7, 0, err, sizeof err));
    cp_session ss;
    cs_session_get(h.s, &ss);
    ss.max_players = 3;
    CHECK(cs_set_session(h.s, &ss));
    CHECK(cs_join(a.s, "sim", "sim:7", 0, err, sizeof err));
    CHECK(cs_join(b.s, "sim", "sim:7", 0, err, sizeof err));
    step(200);
    CHECK(cs_player_count(h.s) == 3 && cs_player_count(a.s) == 3 && cs_player_count(b.s) == 3);
    CHECK(cs_pid(a.s) != cs_pid(b.s));
    cs_ready(a.s, 1), cs_ready(b.s, 1);
    step(100);
    CHECK(cs_start(h.s, 0, err, sizeof err));
    step(100);
    cs_load_done(h.s), cs_load_done(a.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);   /* B is still loading */
    cs_load_done(b.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(a.s) == CP_PHASE_PLAYING && cs_phase(b.s) == CP_PHASE_PLAYING);
    for (int i = 0; i < 30; i++) {
        cp_state s = path_state(cs_pid(a.s), cs_now(a.s));
        cs_submit_state(a.s, &s);
        step(50);
    }
    cp_state out;
    CHECK(cs_remote_sample(b.s, cs_pid(a.s), cs_now(b.s), &out) != CS_SAMPLE_NONE);   /* B sees A's state, relayed */
    CHECK(out.pid == cs_pid(a.s) && fabsf(out.pos[0] - (1000.0f * (float)cs_pid(a.s) + 300.0f)) < 400.0f);
    CHECK(cs_remote_sample(h.s, cs_pid(a.s), cs_now(h.s), &out) != CS_SAMPLE_NONE);
    CHECK(cs_remote_sample(a.s, cs_pid(a.s), cs_now(a.s), &out) == CS_SAMPLE_NONE);   /* no echo of oneself */
    const char body[] = {1, 2, 3, 4};
    CHECK(cs_send_event(a.s, CP_EV_PICKUP, body, sizeof body));
    step(100);
    CHECK(h.ev_n == 1 && h.ev.kind == CP_EV_PICKUP && h.ev.pid == cs_pid(a.s) && h.ev.len == 4 && !memcmp(h.ev.body, body, 4));
    CHECK(b.ev_n == 1 && b.ev.kind == CP_EV_PICKUP && b.ev.pid == cs_pid(a.s));   /* relayed */
    CHECK(a.ev_n == 0);
    /* WORLD batches (stage 6): unreliable and sequenced, relayed by the host, never echoed, newest 64 kept */
    {
        uint8_t wb[200];
        cp_world w;
        for (int i = 0; i < 200; i++) wb[i] = (uint8_t)(i * 7);
        CHECK(cs_send_world(a.s, 1, wb, sizeof wb));
        step(100);
        CHECK(cs_recv_world(h.s, &w) && w.pid == cs_pid(a.s) && w.kind == 1 && w.len == 200 && !memcmp(w.body, wb, 200));
        CHECK(cs_recv_world(b.s, &w) && w.pid == cs_pid(a.s) && w.len == 200);   /* relayed */
        CHECK(!cs_recv_world(a.s, &w) && !cs_recv_world(h.s, &w));
        CHECK(!cs_send_world(a.s, 1, wb, CP_WORLD_MAX + 1));
        for (int i = 0; i < 70; i++) cs_send_world(h.s, 2, wb, 10), step(5);
        int n = 0;
        while (cs_recv_world(a.s, &w)) n++;
        CHECK(n > 0 && n <= 64);
        uint32_t tx, rx, dr;
        cs_world_stats(h.s, &tx, &rx, &dr);
        CHECK(tx == 70 && rx == 1);
    }
    /* a client cannot speak for someone else: a forged state / event with another pid is ignored by the host */
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_EVENT, m.u.event.pid = 0, m.u.event.kind = CP_EV_DEATH;
    uint8_t raw[64];
    cn_send(cs_transport(a.s), 0, CP_CH_CONTROL, 1, raw, cp_encode(&m, raw, sizeof raw));
    step(60);
    CHECK(h.ev_n == 1 && b.ev_n == 1);
    /* a client leaves: the others are told */
    cs_leave(a.s, 1);
    step(100);
    CHECK(h.count[CS_N_PEER_LEAVE] >= 1 && b.count[CS_N_LOBBY] >= 2);
    CHECK(cs_player_count(h.s) == 2);
    cs_free(h.s), cs_free(a.s), cs_free(b.s);
}

/* the partner leaves while a level load is pending (the pause menu's Leave goes through the game's quit flow, which announces a load first) */
static void test_leave_during_load(void)
{
    printf("-- partner leaves while the barrier waits\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 21));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING);
    cs_load_begin(c.s, 100);   /* the client announces a load ... */
    step(60);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);
    cs_load_begin(h.s, 100), cs_load_done(h.s);   /* ... the host is loaded at once and waits for it */
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);
    cs_free(c.s);   /* ... and the client quits instead of finishing it */
    unreg_all();
    reg(&h);
    step(200);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING);   /* the barrier opened without the missing player */
    cs_free(h.s);
    unreg_all();
}

/* stage 6f (retry hang, quit during a barrier): every way a barrier can lose a player must leave both PCs in a defined state */
static void test_quit_during_barrier(void)
{
    printf("-- 6f: host quits / client quits / client never reports during a retry barrier\n");
    char err[100];
    Node h, c;

    /* (1) the host starts a retry load, loads, waits; the client quits without ever finishing its load: the host runs on */
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 31));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_load_begin(h.s, 100);   /* the host's retry */
    step(60);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    cs_load_done(h.s);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);   /* waits for the client */
    cs_free(c.s);
    unreg_all();
    reg(&h);
    step(200);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && cs_phase(h.s) == CP_PHASE_PLAYING);
    cs_free(h.s);
    unreg_all();

    /* (2) the client has loaded and waits at the barrier, the host quits: the client is told (LOST) and its session is gone, not stuck in LOADING */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 32));
    start_game(&h, &c);
    cs_load_begin(c.s, 100);
    step(60);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING);
    cs_load_done(c.s);
    step(60);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING);   /* the host has not loaded yet */
    cs_leave(h.s, 0);                           /* the host quits (pause menu Leave / window closed) */
    step(100);
    CHECK(c.count[CS_N_LOST] == 1 && !cs_active(c.s) && cs_phase(c.s) == CP_PHASE_NONE);
    cs_free(h.s), cs_free(c.s);
    unreg_all();

    /* (3) the host disappears without a word (crash): same result for the waiting client */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 33));
    start_game(&h, &c);
    cs_load_begin(h.s, 100);
    step(60);
    cs_load_done(c.s);
    cn_disconnect_peer(cs_transport(h.s), 1, 0);
    step(100);
    CHECK(c.count[CS_N_LOST] == 1 && !cs_active(c.s));
    cs_free(h.s), cs_free(c.s);
    unreg_all();

    /* (4) the client's game hangs and never reports "loaded" (the s6e2 retry hang): the host's barrier timeout opens the barrier, both PCs end in PLAYING */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 34));
    start_game(&h, &c);
    cs_load_begin(h.s, 100);
    cs_load_begin(c.s, 100);
    step(60);
    cs_load_done(h.s);
    step(60000);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);   /* 60 s: still waiting */
    step(70000);   /* 130 s: past the OLD 120 s limit (6i raised it to 300 s: the N100 needs 150 s for a story load) */
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    CHECK(h.count[CS_N_WAIT] >= 4 && h.last[CS_N_WAIT].pid == 1 && !strcmp(h.last[CS_N_WAIT].text, "Moira Player") && h.last[CS_N_WAIT].b == 300);   /* notes at 20, 50, 80, 110 s name the awaited player and the limit */
    step(180000);   /* 310 s: past the barrier timeout */
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    CHECK(h.count[CS_N_GO] >= 2 && c.count[CS_N_GO] >= 2);
    CHECK(c.count[CS_N_WAIT] == 0);   /* the client never finished loading: it has nothing to wait for */
    (void)err;
    cs_free(h.s), cs_free(c.s);
    unreg_all();
}

/* stage 6i: a slow story load (the N100 needs ~150 s) must not make the host open the barrier early, and the machine that finished first tells its player who it waits for */
static void test_slow_load_barrier(void)
{
    printf("-- 6i: slow load of 150 s, waiting notes on the machine that is ready first\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 35));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    int go_h = h.count[CS_N_GO];

    /* (a) the CLIENT is the slow one: the host is ready after 5 s and waits 145 s more, no timeout */
    cs_load_begin(h.s, 153);
    cs_load_begin(c.s, 153);
    step(5000);
    cs_load_done(h.s);
    step(10000);
    CHECK(h.count[CS_N_WAIT] == 0);   /* before the first note (20 s) */
    step(10000);
    CHECK(h.count[CS_N_WAIT] == 1 && h.last[CS_N_WAIT].a >= 20 && h.last[CS_N_WAIT].a <= 21);
    step(125000);   /* 150 s */
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(c.s) == CP_PHASE_LOADING);
    CHECK(h.count[CS_N_WAIT] == 5 && h.last[CS_N_WAIT].a >= 140);   /* 20 50 80 110 140 s */
    cs_load_done(c.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && h.count[CS_N_GO] == go_h + 1);   /* "all loaded", not the timeout */
    int w = h.count[CS_N_WAIT];
    step(70000);
    CHECK(h.count[CS_N_WAIT] == w);   /* playing: no more notes */

    /* (b) the HOST is the slow one: the client is ready after 5 s, waits for the host and says so (no limit known on a client: b = 0) */
    int cw = c.count[CS_N_WAIT];
    cs_load_begin(h.s, 154);
    step(100);
    cs_load_begin(c.s, 154);   /* the client's own game starts its load (it joined the epoch the host announced) */
    step(5000);
    cs_load_done(c.s);
    step(60000);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING && c.count[CS_N_WAIT] == cw + 2 && c.last[CS_N_WAIT].pid == 0 && !strcmp(c.last[CS_N_WAIT].text, "Host") && c.last[CS_N_WAIT].b == 0);
    cs_load_done(h.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(c.s);
    unreg_all();
}

static void test_leave_reconnect_hostloss(void)
{
    printf("-- client leaves and comes back mid-game, host leaves\n");
    Node h, c, c2;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 5, 0, 1);
    CHECK(connect_pair(&h, &c, 8));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING);
    cs_free(c.s);   /* quits abruptly: LEAVE + disconnect */
    unreg_all();
    reg(&h);
    step(200);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && cs_player_count(h.s) == 1);
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && !pi.connected && pi.character == CP_CHAR_MOIRA);   /* the seat is kept */
    /* a stranger cannot take the seat */
    node_init(&c2, "Somebody Else", CP_CHAR_NONE, 0);
    reg(&c2);
    char err[100];
    CHECK(cs_join(c2.s, "sim", "sim:8", 0, err, sizeof err));
    step(100);
    CHECK(c2.count[CS_N_REFUSED] == 1 && c2.last[CS_N_REFUSED].a == CP_REF_STARTED);
    cs_free(c2.s);
    unreg_all();
    reg(&h);
    /* the same name returns: same pid, same character, a new barrier */
    Node r;
    node_init(&r, "Moira Player", CP_CHAR_NONE, 0);
    reg(&r);
    CHECK(cs_join(r.s, "sim", "sim:8", 0, err, sizeof err));
    step(150);
    CHECK(r.count[CS_N_CONNECTED] == 1 && cs_pid(r.s) == 1 && cs_local_character(r.s) == CP_CHAR_MOIRA);
    CHECK(cs_phase(r.s) == CP_PHASE_LOADING && cs_phase(h.s) == CP_PHASE_LOADING && cs_epoch(h.s) == cs_epoch(r.s));
    cs_load_done(h.s), cs_load_done(r.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(r.s) == CP_PHASE_PLAYING);
    /* host goes away */
    cs_leave(h.s, 0);
    step(100);
    CHECK(r.count[CS_N_LOST] == 1 && !cs_active(r.s));
    cs_free(h.s), cs_free(r.s);

    /* the host disappears without a word: client sees the transport drop */
    Node h2, c3;
    unreg_all();
    node_init(&h2, "H", CP_CHAR_CLAIRE, 0), node_init(&c3, "C", CP_CHAR_NONE, 0);
    reg(&h2), reg(&c3);
    CHECK(connect_pair(&h2, &c3, 9));
    cn_disconnect_peer(cs_transport(h2.s), 1, 0);   /* the transport kills the link, no LEAVE message */
    step(100);
    CHECK(c3.count[CS_N_LOST] == 1);
    cs_free(h2.s), cs_free(c3.s);

    /* the join that nobody answers */
    Node c4;
    unreg_all();
    node_init(&c4, "C", CP_CHAR_NONE, 0);
    reg(&c4);
    CHECK(!cs_join(c4.s, "sim", "sim:77", 0, err, sizeof err));
    CHECK(strstr(err, "no host") != NULL);
    cs_free(c4.s);
}

static void test_transport_selection(void)
{
    printf("-- transport selection: unknown name, Steam without a platform, real ENet host and join on loopback\n");
    Node h;
    unreg_all();
    node_init(&h, "H", CP_CHAR_CLAIRE, 0);
    char err[200];
    CHECK(!cs_host(h.s, "nonsense", 1, 0, err, sizeof err) && strstr(err, "unknown transport"));
    CHECK(!cs_host(h.s, "steam", 1, 0, err, sizeof err) && strstr(err, "game process"));   /* the real Steam backend; no Steam in a test process (test_steam.c has the mock) */
    CHECK(!cs_join(h.s, "steam", "lobby:109775241006780450", 0, err, sizeof err) && strstr(err, "game process"));
    CHECK(!cs_join(h.s, "enet", "", 0, err, sizeof err) && strstr(err, "bad address"));
    CHECK(!cs_join(h.s, "enet", "no.such.host.invalid:28410", 0, err, sizeof err));
    CHECK(!cs_active(h.s));
    /* the command parser (what `net ...` runs) */
    char out[400];
    CHECK(cs_command(h.s, "status", out, sizeof out) && strstr(out, "no session"));
    CHECK(cs_command(h.s, "char barry", out, sizeof out) && strstr(out, "lead-b"));
    CHECK(cs_command(h.s, "host 28431 enet", out, sizeof out) && strstr(out, "hosting") && strstr(out, "lead-b"));
    CHECK(cs_is_host(h.s) && cs_local_character(h.s) == CP_CHAR_BARRY);
    CHECK(cs_command(h.s, "session 2 1 3", out, sizeof out) && strstr(out, "episode 2"));
    CHECK(!cs_command(h.s, "frobnicate", out, sizeof out));
    CHECK(cs_command(h.s, "stop", out, sizeof out) && !cs_active(h.s));
    cs_free(h.s);
}

/* ================================================================================================================== */
/* P5 resilience (notes\release-0.2.0-plan.md, blocker B9): silent / crashed / stalled partners, leave and re-entry in every phase, version gates */

static void node_init_v(Node *nd, const char *name, int want, uint32_t offset, const char *mod, int bot)
{
    memset(nd, 0, sizeof *nd);
    nd->offset = offset;
    cs_config c;
    memset(&c, 0, sizeof c);
    c.name = name, c.want_char = want, c.mod_version = mod, c.is_bot = bot, c.clock = clk, c.clock_user = nd, c.on_note = on_note, c.user = nd;
    nd->s = cs_new(&c);
}

/* a node whose game does not run (window in the background, a stalled PC, a hung game): it is not polled, its link stays up */
static void freeze(Node *nd)
{
    for (int i = 0; i < n_all; i++)
        if (all_nodes[i] == nd) {
            all_nodes[i] = all_nodes[--n_all];
            return;
        }
}
static void thaw(Node *nd) { reg(nd); }

/* both games run and send their state at 20 Hz */
static void stream(Node *a, Node *b, int ms)
{
    for (int t = 0; t < ms; t += 50) {
        cp_state s = path_state(cs_pid(a->s), cs_now(a->s));
        cs_submit_state(a->s, &s);
        s = path_state(cs_pid(b->s), cs_now(b->s));
        cs_submit_state(b->s, &s);
        step(50);
    }
}

static uint32_t rx_states(Node *nd)
{
    cs_stats st;
    cs_get_stats(nd->s, &st);
    return st.rx_state;
}

static void test_silent_partner(void)
{
    printf("-- P5: the partner's game sends nothing for 30 s (paused, Alt-Tab, a stall); the link stays up\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 41));
    start_game(&h, &c);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING);
    stream(&h, &c, 1000);
    /* (a) the client's game stops: the host is told after 8 s and every 10 s; the session, the seat and the phase stay */
    freeze(&c);
    step(7500);
    CHECK(h.count[CS_N_SILENT] == 0);
    step(1000);
    CHECK(h.count[CS_N_SILENT] == 1 && h.last[CS_N_SILENT].pid == 1 && h.last[CS_N_SILENT].a == 8 && h.last[CS_N_SILENT].b == 600 && !strcmp(h.text[CS_N_SILENT], "Moira Player"));
    step(21500);   /* 30 s */
    CHECK(h.count[CS_N_SILENT] == 3 && h.last[CS_N_SILENT].a == 28);
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && pi.connected && cs_phase(h.s) == CP_PHASE_PLAYING && h.count[CS_N_PEER_LEAVE] == 0);
    cp_state o;
    CHECK(cs_remote_sample(h.s, 1, cs_now(h.s), &o) == CS_SAMPLE_STALE);
    uint32_t rx0 = rx_states(&h);
    thaw(&c);
    stream(&h, &c, 1000);
    CHECK(h.count[CS_N_SILENT] == 4 && h.last[CS_N_SILENT].pid == 1 && h.last[CS_N_SILENT].a == 0 && h.last[CS_N_SILENT].b >= 30 && h.last[CS_N_SILENT].b <= 31);
    CHECK(c.count[CS_N_SILENT] == 0);   /* the resumed client finds the host's messages waiting: no false alarm about the host */
    CHECK(rx_states(&h) >= rx0 + 15 && cs_remote_sample(h.s, 1, cs_now(h.s), &o) != CS_SAMPLE_STALE);
    CHECK(c.count[CS_N_LOST] == 0 && cs_phase(c.s) == CP_PHASE_PLAYING && cs_phase(h.s) == CP_PHASE_PLAYING);
    /* (b) the host's game stops for 30 s: the client is told, nothing ends */
    freeze(&h);
    step(30000);
    CHECK(c.count[CS_N_SILENT] == 3 && c.last[CS_N_SILENT].pid == 0 && c.last[CS_N_SILENT].a == 28 && !strcmp(c.text[CS_N_SILENT], "Host"));
    CHECK(cs_active(c.s) && cs_phase(c.s) == CP_PHASE_PLAYING && c.count[CS_N_LOST] == 0);
    thaw(&h);
    stream(&h, &c, 1000);
    CHECK(c.count[CS_N_SILENT] == 4 && c.last[CS_N_SILENT].a == 0);
    CHECK(h.count[CS_N_SILENT] == 4);   /* no false alarm on the host after its own pause */
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && cs_player_count(h.s) == 2);
    cs_free(h.s), cs_free(c.s);
    unreg_all();
}

static void test_owner_stall_90s(void)
{
    printf("-- P5: the owner's PC stalls for 90 s (stage6.md 413 / 539): in the barrier and while playing; nothing ends, the barrier opens 'all loaded'\n");
    Node h, c;
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 42));
    start_game(&h, &c);
    /* (a) a load begins, the client is ready at once, the host freezes for 90 s before its own load is done */
    int go_h = h.count[CS_N_GO], go_c = c.count[CS_N_GO];
    cs_load_begin(h.s, 153);
    step(100);
    cs_load_begin(c.s, 153);
    cs_load_done(c.s);
    step(100);
    freeze(&h);
    step(90000);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING && cs_active(c.s) && c.count[CS_N_LOST] == 0);
    CHECK(c.count[CS_N_WAIT] == 3 && c.last[CS_N_WAIT].pid == 0);   /* 20, 50, 80 s: "waiting for Host to finish loading" */
    CHECK(c.count[CS_N_SILENT] == 9 && c.last[CS_N_SILENT].a == 88);   /* 8 .. 88 s */
    thaw(&h);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);   /* the host's own load is not done yet */
    cs_load_done(h.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && h.count[CS_N_GO] == go_h + 1 && c.count[CS_N_GO] == go_c + 1);
    CHECK(!strcmp(h.text[CS_N_GO], "all loaded"));
    CHECK(c.last[CS_N_SILENT].a == 0 && h.count[CS_N_SILENT] == 0 && h.count[CS_N_PEER_LEAVE] == 0);
    /* (b) the client freezes for 90 s while the host plays on */
    stream(&h, &c, 500);
    int sh = h.count[CS_N_SILENT];
    freeze(&c);
    for (int t = 0; t < 90000; t += 50) {
        cp_state s = path_state(0, cs_now(h.s));
        cs_submit_state(h.s, &s);
        step(50);
    }
    CHECK(h.count[CS_N_SILENT] == sh + 9 && cs_player_count(h.s) == 2 && h.count[CS_N_PEER_LEAVE] == 0);
    thaw(&c);
    stream(&h, &c, 1000);
    CHECK(h.count[CS_N_SILENT] == sh + 10 && h.last[CS_N_SILENT].a == 0 && h.last[CS_N_SILENT].b >= 90 && h.last[CS_N_SILENT].b <= 91);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && c.count[CS_N_LOST] == 0);
    cp_state o;
    CHECK(cs_remote_sample(c.s, 0, cs_now(c.s), &o) != CS_SAMPLE_NONE && cs_remote_sample(h.s, 1, cs_now(h.s), &o) != CS_SAMPLE_STALE);
    /* (c) the client freezes for 90 s in the middle of its own load while the host waits at the barrier: no timeout (300 s), 'all loaded' after it */
    go_h = h.count[CS_N_GO];
    int wh = h.count[CS_N_WAIT];
    cs_load_begin(h.s, 154);
    step(100);
    cs_load_begin(c.s, 154);
    cs_load_done(h.s);
    step(100);
    freeze(&c);
    step(90000);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && h.count[CS_N_WAIT] == wh + 3 && h.last[CS_N_WAIT].pid == 1 && h.last[CS_N_WAIT].b == 300);
    thaw(&c);
    step(100);
    cs_load_done(c.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(c.s) == CP_PHASE_PLAYING && h.count[CS_N_GO] == go_h + 1 && !strcmp(h.text[CS_N_GO], "all loaded"));
    CHECK(h.count[CS_N_PEER_LEAVE] == 0 && c.count[CS_N_LOST] == 0);
    cs_free(h.s), cs_free(c.s);
    unreg_all();
}

static void test_silence_limit(void)
{
    printf("-- P5: a partner that never answers again: the session gives up after 10 minutes, the barrier never waits for ever\n");
    char err[100];
    Node h, c, r;
    /* (a) the client's game hangs for good while playing (its link stays up): the host gives the seat up at 600 s and keeps it for a re-entry */
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 43));
    start_game(&h, &c);
    stream(&h, &c, 500);
    freeze(&c);
    step(595000);
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && pi.connected && h.count[CS_N_PEER_LEAVE] == 0 && h.count[CS_N_SILENT] == 59);
    step(10000);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && h.last[CS_N_PEER_LEAVE].pid == 1 && h.last[CS_N_PEER_LEAVE].a == 1 && h.last[CS_N_PEER_LEAVE].b == 1);
    CHECK(cs_player(h.s, 1, &pi) && !pi.connected && pi.character == CP_CHAR_MOIRA && cs_phase(h.s) == CP_PHASE_PLAYING && cs_player_count(h.s) == 1);
    int notes = h.count[CS_N_SILENT];
    step(30000);
    CHECK(h.count[CS_N_SILENT] == notes);   /* no more notes about a seat that is gone */
    /* the hung game comes back: its old link was dropped by the host, so it learns that its session is over */
    thaw(&c);
    step(200);
    CHECK(c.count[CS_N_LOST] == 1 && !cs_active(c.s));
    cs_free(c.s);
    unreg_all();
    reg(&h);
    /* ... and the same player joins again: the kept seat, the same character, a new barrier */
    node_init(&r, "Moira Player", CP_CHAR_NONE, 0);
    reg(&r);
    CHECK(cs_join(r.s, "sim", "sim:43", 0, err, sizeof err));
    step(200);
    CHECK(r.count[CS_N_CONNECTED] == 1 && cs_pid(r.s) == 1 && cs_local_character(r.s) == CP_CHAR_MOIRA && cs_phase(r.s) == CP_PHASE_LOADING && cs_epoch(r.s) == cs_epoch(h.s));
    cs_load_done(h.s), cs_load_done(r.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(r.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(r.s);
    unreg_all();

    /* (b) the client waits at the barrier for a host that never answers again: LOST at 600 s with the reason, not before; the wait has an end */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 44));
    start_game(&h, &c);
    cs_load_begin(h.s, 100);
    step(100);
    cs_load_begin(c.s, 100), cs_load_done(c.s);
    step(100);
    freeze(&h);
    step(590000);
    CHECK(cs_phase(c.s) == CP_PHASE_LOADING && cs_active(c.s) && c.count[CS_N_LOST] == 0);
    step(15000);
    CHECK(c.count[CS_N_LOST] == 1 && c.last[CS_N_LOST].a == 2 && strstr(c.text[CS_N_LOST], "no answer from the host for 600 s") != NULL);
    CHECK(!cs_active(c.s) && cs_phase(c.s) == CP_PHASE_NONE);
    cs_free(h.s), cs_free(c.s);
    unreg_all();
}

static void test_leave_cases(void)
{
    printf("-- P5: Leave while loading (client, host), Leave while joining\n");
    char err[100];
    Node h, c;
    /* (a) the client leaves (pause menu Leave) while the host waits at the barrier: the host is told, runs on, keeps the seat; the leaver has no session */
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 45));
    start_game(&h, &c);
    cs_load_begin(h.s, 101);
    step(100);
    cs_load_done(h.s);
    cs_load_begin(c.s, 101);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);
    cs_leave(c.s, 0);
    step(200);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && h.last[CS_N_PEER_LEAVE].a == 0 && h.last[CS_N_PEER_LEAVE].b == 0);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && !strcmp(h.text[CS_N_GO], "all loaded"));
    cs_player_info pi;
    CHECK(cs_player(h.s, 1, &pi) && !pi.connected);
    CHECK(!cs_active(c.s) && cs_phase(c.s) == CP_PHASE_NONE && c.count[CS_N_LOST] == 0);
    /* the leaver's game finishes its load afterwards: nothing is sent, nothing changes, nothing crashes */
    cs_load_done(c.s);
    cs_load_begin(c.s, 101);
    cp_state st = path_state(1, cs_now(c.s));
    CHECK(!cs_submit_state(c.s, &st) && !cs_send_event(c.s, CP_EV_USER, "x", 1) && !cs_send_world(c.s, 1, "x", 1));
    CHECK(!cs_pick(c.s, CP_CHAR_MOIRA) && !cs_ready(c.s, 1) && !cs_start(c.s, 1, err, sizeof err));
    CHECK(cs_phase(c.s) == CP_PHASE_NONE && !cs_active(c.s));
    step(100);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && cs_phase(h.s) == CP_PHASE_PLAYING);
    cs_free(h.s), cs_free(c.s);
    unreg_all();

    /* (b) the host leaves while the client is still loading: the client is told that the host left (not "connection lost") */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 46));
    start_game(&h, &c);
    cs_load_begin(h.s, 101);
    step(100);
    cs_leave(h.s, 0);
    step(200);
    CHECK(c.count[CS_N_LOST] == 1 && c.last[CS_N_LOST].a == 1 && !strcmp(c.text[CS_N_LOST], "the host left the session") && !cs_active(c.s));
    cs_free(h.s), cs_free(c.s);
    unreg_all();

    /* (c) the client leaves while its join is under way (the WELCOME still on the wire): no seat is kept, it can join again at once */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 50, 0, 1);
    CHECK(cs_host(h.s, "sim", 47, 0, err, sizeof err));
    CHECK(cs_join(c.s, "sim", "sim:47", 0, err, sizeof err));
    step(60);
    cs_leave(c.s, 0);
    step(400);
    CHECK(cs_player_count(h.s) == 1 && !cs_active(c.s) && c.count[CS_N_CONNECTED] == 0);
    CHECK(cs_join(c.s, "sim", "sim:47", 0, err, sizeof err));
    step(300);
    CHECK(c.count[CS_N_CONNECTED] == 1 && cs_pid(c.s) == 1 && cs_player_count(h.s) == 2);
    cs_free(h.s), cs_free(c.s);
    unreg_all();
    faults(0, 0, 0, 10, 0, 1);
}

static void test_rejoin_cases(void)
{
    printf("-- P5: re-entry: while the host loads, while the old link still looks alive (lobby, game), a live namesake, twice in a row\n");
    char err[100];
    Node h, c, r, r2;
    cs_player_info pi;
    /* (a) the client crashes while the host itself is still loading; it comes back before the host is done: same epoch, the barrier opens 'all loaded' */
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 48));
    start_game(&h, &c);
    cs_load_begin(h.s, 102);
    step(100);
    unsigned e = cs_epoch(h.s);
    freeze(&c);
    cn_disconnect_peer(cs_transport(h.s), 1, 0);   /* the transport noticed the crash */
    step(100);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && h.last[CS_N_PEER_LEAVE].a == 1 && cs_phase(h.s) == CP_PHASE_LOADING);   /* the host itself is not loaded: no GO */
    cs_free(c.s);
    node_init(&r, "Moira Player", CP_CHAR_NONE, 0);
    reg(&r);
    CHECK(cs_join(r.s, "sim", "sim:48", 0, err, sizeof err));
    step(200);
    CHECK(r.count[CS_N_CONNECTED] == 1 && cs_pid(r.s) == 1 && cs_phase(r.s) == CP_PHASE_LOADING && cs_epoch(r.s) == e && cs_epoch(h.s) == e);
    cs_load_done(h.s);
    step(100);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING);   /* waits for the returned player */
    cs_load_begin(r.s, 102), cs_load_done(r.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(r.s) == CP_PHASE_PLAYING && !strcmp(h.text[CS_N_GO], "all loaded") && cs_epoch(r.s) == e);
    cs_free(h.s), cs_free(r.s);
    unreg_all();

    /* (b) the client's game dies and the player starts it again before the transport noticed: the new connection takes the seat over */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 49));
    start_game(&h, &c);
    stream(&h, &c, 500);
    freeze(&c);   /* no LEAVE, no disconnect: the old link still looks alive */
    step(4000);
    node_init(&r, "Moira Player", CP_CHAR_NONE, 0);
    reg(&r);
    CHECK(cs_join(r.s, "sim", "sim:49", 0, err, sizeof err));
    step(200);
    CHECK(r.count[CS_N_CONNECTED] == 1 && cs_pid(r.s) == 1 && cs_local_character(r.s) == CP_CHAR_MOIRA && r.count[CS_N_REFUSED] == 0);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && h.last[CS_N_PEER_LEAVE].b == 2 && h.count[CS_N_PEER_JOIN] == 2 && h.last[CS_N_PEER_JOIN].a == 1);
    CHECK(cs_phase(h.s) == CP_PHASE_LOADING && cs_phase(r.s) == CP_PHASE_LOADING && cs_epoch(h.s) == cs_epoch(r.s));
    cs_load_done(h.s), cs_load_done(r.s);
    step(200);
    CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(r.s) == CP_PHASE_PLAYING && cs_player_count(h.s) == 2);
    /* the dead game's process wakes up after all: its session is over, the new one is not disturbed */
    thaw(&c);
    stream(&h, &r, 500);
    CHECK(c.count[CS_N_LOST] == 1 && !cs_active(c.s));
    CHECK(cs_phase(r.s) == CP_PHASE_PLAYING && cs_player(h.s, 1, &pi) && pi.connected && r.count[CS_N_LOST] == 0);
    cs_free(c.s);
    unreg_all();
    reg(&h), reg(&r);
    /* (c) a namesake joins while the seat's player is alive and heard: refused, the seat stays with the live player */
    node_init(&r2, "Moira Player", CP_CHAR_NONE, 0);
    reg(&r2);
    stream(&h, &r, 200);
    CHECK(cs_join(r2.s, "sim", "sim:49", 0, err, sizeof err));
    for (int t = 0; t < 400; t += 50) stream(&h, &r, 50);
    CHECK(r2.count[CS_N_REFUSED] == 1 && r2.last[CS_N_REFUSED].a == CP_REF_STARTED && cs_player(h.s, 1, &pi) && pi.connected && r.count[CS_N_LOST] == 0);
    cs_free(r2.s);
    unreg_all();
    reg(&h), reg(&r);
    /* (e) crash and re-entry twice more in a row: the seat, the character and the barrier stay right */
    for (int k = 0; k < 2; k++) {
        unsigned e0 = cs_epoch(h.s);
        cs_free(r.s);   /* quits abruptly */
        unreg_all();
        reg(&h);
        step(200);
        node_init(&r, "Moira Player", CP_CHAR_NONE, 0);
        reg(&r);
        CHECK(cs_join(r.s, "sim", "sim:49", 0, err, sizeof err));
        step(200);
        CHECK(cs_pid(r.s) == 1 && cs_local_character(r.s) == CP_CHAR_MOIRA && cs_phase(r.s) == CP_PHASE_LOADING && cs_epoch(h.s) == (uint8_t)(e0 + 1));
        cs_load_done(h.s), cs_load_done(r.s);
        step(200);
        CHECK(cs_phase(h.s) == CP_PHASE_PLAYING && cs_phase(r.s) == CP_PHASE_PLAYING);
    }
    cs_free(h.s), cs_free(r.s);
    unreg_all();

    /* (d) the same in the lobby: the crashed client's old link would hold the second seat ("session is full") until the transport notices */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Moira Player", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(connect_pair(&h, &c, 50));
    freeze(&c);
    step(4000);
    node_init(&r, "Moira Player", CP_CHAR_NONE, 0);
    reg(&r);
    CHECK(cs_join(r.s, "sim", "sim:50", 0, err, sizeof err));
    step(200);
    CHECK(r.count[CS_N_CONNECTED] == 1 && r.count[CS_N_REFUSED] == 0 && cs_pid(r.s) == 1 && cs_player_count(h.s) == 2 && cs_phase(r.s) == CP_PHASE_LOBBY);
    cs_free(c.s), cs_free(h.s), cs_free(r.s);
    unreg_all();
}

/* a raw host on the sim transport that answers the first HELLO the way an older (0.1.x) host would, or with a REFUSE */
static int raw_host_answer(Node *raw, int kind, const char *mod, int old_format, int bot_flag)
{
    cn_event ev;
    int answered = 0;
    while (cn_recv(raw->raw, &ev)) {
        cp_msg m;
        if (ev.type == CN_EV_RECEIVE && cp_decode(ev.data, ev.len, &m) && m.type == CP_HELLO && !answered) {
            cp_msg a;
            memset(&a, 0, sizeof a);
            if (kind == CP_WELCOME) {
                a.type = CP_WELCOME, a.u.welcome.version = 2, a.u.welcome.pid = 1, a.u.welcome.phase = CP_PHASE_LOBBY, a.u.welcome.flags = (uint8_t)(bot_flag ? CP_PF_BOT : 0);
                a.u.welcome.lobby.rev = 1, a.u.welcome.lobby.count = 2;
                a.u.welcome.lobby.p[0].pid = 0, a.u.welcome.lobby.p[0].character = CP_CHAR_CLAIRE, a.u.welcome.lobby.p[0].flags = CP_PF_HOST | CP_PF_CONNECTED;
                snprintf(a.u.welcome.lobby.p[0].name, sizeof a.u.welcome.lobby.p[0].name, "Old Host");
                a.u.welcome.lobby.p[1].pid = 1, a.u.welcome.lobby.p[1].character = CP_CHAR_MOIRA, a.u.welcome.lobby.p[1].flags = CP_PF_CONNECTED;
                snprintf(a.u.welcome.lobby.p[1].name, sizeof a.u.welcome.lobby.p[1].name, "%s", m.u.hello.name);
                a.u.welcome.session.episode = 1, a.u.welcome.session.max_players = 2;
                snprintf(a.u.welcome.mod, sizeof a.u.welcome.mod, "%s", mod);
            } else {
                a.type = CP_REFUSE, a.u.refuse.code = (uint8_t)kind;
                snprintf(a.u.refuse.text, sizeof a.u.refuse.text, "%s", cp_refuse_text((unsigned)kind));
                snprintf(a.u.refuse.mod, sizeof a.u.refuse.mod, "%s", mod);
            }
            uint8_t buf[CP_MAX_PACKET];
            size_t n = cp_encode(&a, buf, sizeof buf);
            if (old_format) n -= 1 + strlen(mod);   /* an older host sends no version tail */
            cn_send(raw->raw, ev.peer, CP_CH_CONTROL, 1, buf, n);
            answered = 1;
        } else if (ev.type == CN_EV_RECEIVE && cp_decode(ev.data, ev.len, &m) && m.type == CP_LEAVE)
            raw->ev_n++;   /* counts the LEAVE messages the raw host received */
        cn_event_free(&ev);
    }
    return answered;
}

static void test_version_gate(void)
{
    printf("-- P5: co-op package version gate (both directions, older hosts), protocol refusal with the host's version\n");
    char err[100];
    Node h, c;
    /* (a) host 0.2.0, client 0.1.1 (both real games): refused with CP_REF_MODVER, the host's version comes along, the host keeps one player */
    unreg_all();
    node_init_v(&h, "Host", CP_CHAR_CLAIRE, 0, "0.2.0", 0);
    node_init_v(&c, "Moira Player", CP_CHAR_NONE, 0, "0.1.1", 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(cs_host(h.s, "sim", 51, 0, err, sizeof err));
    CHECK(cs_join(c.s, "sim", "sim:51", 0, err, sizeof err));
    step(200);
    CHECK(c.count[CS_N_REFUSED] == 1 && c.last[CS_N_REFUSED].a == CP_REF_MODVER && !strcmp(c.pmod[CS_N_REFUSED], "0.2.0"));
    CHECK(strstr(c.text[CS_N_REFUSED], "0.2.0") && strstr(c.text[CS_N_REFUSED], "0.1.1") && strstr(c.text[CS_N_REFUSED], "same version"));
    CHECK(!cs_active(c.s) && cs_player_count(h.s) == 1 && c.count[CS_N_CONNECTED] == 0);
    cs_free(c.s);
    unreg_all();
    reg(&h);
    /* (b) the same version is accepted */
    node_init_v(&c, "Moira Player", CP_CHAR_NONE, 0, "0.2.0", 0);
    reg(&c);
    CHECK(cs_join(c.s, "sim", "sim:51", 0, err, sizeof err));
    step(200);
    CHECK(c.count[CS_N_CONNECTED] == 1 && cs_player_count(h.s) == 2);
    cs_free(c.s);
    unreg_all();
    reg(&h);
    step(100);
    /* (c) the lab's headless bot is not checked */
    node_init_v(&c, "Bot", CP_CHAR_NONE, 0, "bot", 1);
    reg(&c);
    CHECK(cs_join(c.s, "sim", "sim:51", 0, err, sizeof err));
    step(200);
    CHECK(c.count[CS_N_CONNECTED] == 1 && cs_player_count(h.s) == 2);
    cs_free(c.s), cs_free(h.s);
    unreg_all();

    /* (d) a 0.2.0 client joins an older host (0.1.x: no version in its WELCOME, it never checks ours): the client leaves at once and says why */
    Node raw;
    memset(&raw, 0, sizeof raw);
    raw.raw = cn_new();
    CHECK(cn_host(raw.raw, "sim", 52, 3, 0, err, sizeof err));
    /* each case: a new 0.2.0 client joins the raw host, the raw host answers its HELLO, the client is freed and the in-flight packets land
       (the sim gives the next client peer index 1 again at once: an old client's LEAVE still on the wire would be taken for the new one's) */
    struct { const char *what; int kind; const char *mod; int old_format, bot; } gc[] = {
        {"(d) older host: no version in the WELCOME", CP_WELCOME, "", 1, 0},
        {"(e) a host with another version in the tail", CP_WELCOME, "0.2.1", 0, 0},
        {"(f) a bot host with another version (the lab bot hosting): accepted", CP_WELCOME, "bot", 0, 1},
        {"(g) a protocol refusal carries the host's version", CP_REF_VERSION, "0.3.0", 0, 0},
        {"(g) an older host's REFUSE without the tail", CP_REF_VERSION, "", 1, 0},
    };
    for (unsigned k = 0; k < sizeof gc / sizeof gc[0]; k++) {
        unreg_all();
        reg(&raw);
        raw.ev_n = 0;
        node_init_v(&c, "Moira Player", CP_CHAR_NONE, 0, "0.2.0", 0);
        reg(&c);
        CHECK(cs_join(c.s, "sim", "sim:52", 0, err, sizeof err));
        step(40);
        int answered = raw_host_answer(&raw, gc[k].kind, gc[k].mod, gc[k].old_format, gc[k].bot);
        CHECK(answered);
        step(60);
        raw_host_answer(&raw, gc[k].kind, gc[k].mod, gc[k].old_format, gc[k].bot);   /* counts the LEAVE of a client that left */
        int ok = 0;
        if (k == 0) ok = c.count[CS_N_REFUSED] == 1 && c.last[CS_N_REFUSED].a == CP_REF_MODVER && !strcmp(c.pmod[CS_N_REFUSED], "") && strstr(c.text[CS_N_REFUSED], "older") &&
                         !cs_active(c.s) && c.count[CS_N_CONNECTED] == 0 && raw.ev_n == 1;   /* the old host got a LEAVE */
        if (k == 1) ok = c.count[CS_N_REFUSED] == 1 && c.last[CS_N_REFUSED].a == CP_REF_MODVER && !strcmp(c.pmod[CS_N_REFUSED], "0.2.1") && strstr(c.text[CS_N_REFUSED], "0.2.1") &&
                         !cs_active(c.s) && raw.ev_n == 1;
        if (k == 2) ok = c.count[CS_N_CONNECTED] == 1 && c.count[CS_N_REFUSED] == 0 && cs_active(c.s) && raw.ev_n == 0;
        if (k == 3) ok = c.count[CS_N_REFUSED] == 1 && c.last[CS_N_REFUSED].a == CP_REF_VERSION && !strcmp(c.pmod[CS_N_REFUSED], "0.3.0") && !cs_active(c.s);
        if (k == 4) ok = c.count[CS_N_REFUSED] == 1 && c.last[CS_N_REFUSED].a == CP_REF_VERSION && !strcmp(c.pmod[CS_N_REFUSED], "") &&
                         !strcmp(c.text[CS_N_REFUSED], "different co-op protocol version") && !cs_active(c.s);
        if (!ok) printf("  case %s: refused %d (code %d, host mod '%s', text '%s'), connected %d, LEAVE seen %d\n", gc[k].what, c.count[CS_N_REFUSED], c.last[CS_N_REFUSED].a,
                        c.pmod[CS_N_REFUSED], c.text[CS_N_REFUSED], c.count[CS_N_CONNECTED], raw.ev_n);
        CHECK(ok);
        cs_free(c.s);
        unreg_all();
        reg(&raw);
        step(100);
        raw_host_answer(&raw, CP_WELCOME, "", 0, 0);   /* drain */
    }
    cn_free(raw.raw);
    unreg_all();
}

/* ------------------------------------------------------------------------------------------------------------------ */
/* lobby-must (Rev1 0.1.0): the host removes the joined player (lobby Remove partner), the product rule "no rejoin after the start", the name set before a session */
static void test_kick_norejoin_name(void)
{
    printf("-- lobby-must: kick in the lobby, no rejoin after the start (product switch), the name\n");
    char err[100];
    Node h, c, r;
    cs_player_info pi;
    /* (a) kick in the lobby: the guest's session ends with LOST a=3, the host frees the seat (PEER_LEAVE b=3), a new join works at once */
    unreg_all();
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Guest", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    faults(0, 0, 0, 10, 0, 1);
    CHECK(connect_pair(&h, &c, 61));
    CHECK(cs_kick(c.s, 0) == 0 && cs_kick(h.s, 0) == 0 && cs_kick(h.s, 5) == 0);   /* only the host, only a connected other player */
    CHECK(cs_kick(h.s, 1) == 1);
    step(200);
    CHECK(c.count[CS_N_LOST] == 1 && c.last[CS_N_LOST].a == 3 && !cs_active(c.s) && cs_phase(c.s) == CP_PHASE_NONE);
    CHECK(h.count[CS_N_PEER_LEAVE] == 1 && h.last[CS_N_PEER_LEAVE].b == 3 && cs_remote_pid(h.s) < 0 && !cs_player(h.s, 1, &pi));
    CHECK(cs_join(c.s, "sim", "sim:61", 0, err, sizeof err));
    step(200);
    CHECK(c.count[CS_N_CONNECTED] == 2 && cs_pid(c.s) == 1);
    cs_free(h.s), cs_free(c.s);
    unreg_all();

    /* (b) the product rule: after the start nobody comes back, not even the same name (REFUSE STARTED); the lab default lets him back */
    for (int lab = 0; lab < 2; lab++) {
        node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
        node_init(&c, "Guest", CP_CHAR_NONE, 0);
        reg(&h), reg(&c);
        cs_set_no_rejoin(h.s, !lab);
        CHECK(connect_pair(&h, &c, 62 + lab));
        start_game(&h, &c);
        freeze(&c);
        cn_disconnect_peer(cs_transport(h.s), 1, 0);
        step(100);
        CHECK(h.count[CS_N_PEER_LEAVE] == 1);
        cs_free(c.s);
        node_init(&r, "Guest", CP_CHAR_NONE, 0);
        reg(&r);
        char target[16];
        snprintf(target, sizeof target, "sim:%d", 62 + lab);
        CHECK(cs_join(r.s, "sim", target, 0, err, sizeof err));
        step(300);
        if (!lab) CHECK(r.count[CS_N_REFUSED] == 1 && r.last[CS_N_REFUSED].a == CP_REF_STARTED && r.count[CS_N_CONNECTED] == 0 && cs_player(h.s, 1, &pi) && !pi.connected);
        else CHECK(r.count[CS_N_CONNECTED] == 1 && cs_pid(r.s) == 1);
        cs_free(h.s), cs_free(r.s);
        unreg_all();
    }
    /* (c) a ghost link after the start (crash + quick restart while the old link looks alive) is refused too: the old seat stays as it is */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Guest", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    cs_set_no_rejoin(h.s, 1);
    CHECK(connect_pair(&h, &c, 64));
    start_game(&h, &c);
    freeze(&c);
    step(4000);
    node_init(&r, "Guest", CP_CHAR_NONE, 0);
    reg(&r);
    CHECK(cs_join(r.s, "sim", "sim:64", 0, err, sizeof err));
    step(300);
    CHECK(r.count[CS_N_REFUSED] == 1 && r.last[CS_N_REFUSED].a == CP_REF_STARTED && h.count[CS_N_PEER_LEAVE] == 0);
    cs_free(h.s), cs_free(c.s), cs_free(r.s);
    unreg_all();

    /* (d) the name: changed only while no session runs, sent in HELLO */
    node_init(&h, "Host", CP_CHAR_CLAIRE, 0);
    node_init(&c, "Old", CP_CHAR_NONE, 0);
    reg(&h), reg(&c);
    CHECK(cs_set_name(c.s, "Player 2") == 1);
    CHECK(connect_pair(&h, &c, 65));
    CHECK(cs_set_name(c.s, "Other") == 0);   /* a running session keeps its name */
    CHECK(cs_player(h.s, 1, &pi) && !strcmp(pi.name, "Player 2"));
    cs_free(h.s), cs_free(c.s);
    unreg_all();
    faults(0, 0, 0, 0, 0, 1);
}

int main(void)
{
    cn_sim_set_time(vclock);
    test_handshake_lobby_start();
    test_force_roles();
    test_refusals();
    test_barrier_epochs();
    test_stream_under_faults("clean", 0, 0, 0, 20, 0, 1.0, 0.5);
    test_stream_under_faults("lan jitter", 0, 0, 0, 5, 10, 2.0, 0.7);
    test_stream_under_faults("loss 15%", 15, 0, 0, 40, 10, 6.0, 1.5);
    test_stream_under_faults("loss + dup + reorder", 10, 10, 10, 50, 20, 8.0, 2.0);
    test_stream_under_faults("bad wifi", 25, 5, 15, 80, 40, 20.0, 4.0);
    test_interpolation_cases();
    test_three_players_relay_events();
    test_leave_during_load();
    test_quit_during_barrier();
    test_slow_load_barrier();
    test_leave_reconnect_hostloss();
    test_transport_selection();
    test_silent_partner();
    test_owner_stall_90s();
    test_silence_limit();
    test_leave_cases();
    test_rejoin_cases();
    test_version_gate();
    test_kick_norejoin_name();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
