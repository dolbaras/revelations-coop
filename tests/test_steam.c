/*
 * Tests of the Steam backend (native/coop_steam.c) on a mock Steam universe (steam_mock.c): lobby creation with our private tag, joining by lobby id /
 * by the host's SteamID (friend presence, public lobby search), policy (friends only, allow list), full / private / foreign lobbies, session requests
 * that have to be accepted, disconnect detection in every form, the whole session layer over the backend under loss / duplication / reordering,
 * "Join game" / invite requests with auto-join and the callback filter, and the console commands. Virtual time, deterministic, runs in seconds.
 * The real Steam calls (vtables, struct layouts) are NOT covered here: that is the job of the hardware test.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_session.h"
#include "../native/coop_steam.h"
#include "steam_mock.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define ULL(x) ((unsigned long long)(x))

static uint32_t vclock = 1000;
typedef struct {
    cs_t *s;
    mock_user *u;
    int count[16], dead;
    char text[16][120];
    int last_a[16];
} Node;
static Node *nodes[8];
static int n_nodes;

static uint32_t clk(void *u) { (void)u; return vclock; }
static void on_note(void *u, const cs_note *n)
{
    Node *nd = (Node *)u;
    if (n->what >= 0 && n->what < 16) {
        nd->count[n->what]++;
        nd->last_a[n->what] = n->a;
        snprintf(nd->text[n->what], sizeof nd->text[n->what], "%s", n->text ? n->text : "");
    }
}

static void world(void)
{
    for (int i = 0; i < n_nodes; i++) {
        if (nodes[i]->s) cs_free(nodes[i]->s);
        free(nodes[i]);
    }
    n_nodes = 0;
    cst_reset();
    cst_set_threaded(0);
    mock_reset();
    mock_set_time(vclock);
}
static Node *mk(const char *name)
{
    Node *nd = (Node *)calloc(1, sizeof *nd);
    nd->u = mock_add_user(name);
    cs_config c;
    memset(&c, 0, sizeof c);
    c.name = name, c.want_char = CP_CHAR_NONE, c.mod_version = "test", c.is_bot = 1, c.clock = clk, c.on_note = on_note, c.user = nd;
    nd->s = cs_new(&c);
    nodes[n_nodes++] = nd;
    return nd;
}
static void plat(Node *n) { cst_set_platform(mock_plat(), n->u); }
static void step(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        vclock += 5;
        mock_set_time(vclock);
        for (int i = 0; i < n_nodes; i++)
            if (!nodes[i]->dead && nodes[i]->s) cs_poll(nodes[i]->s);
    }
}
static int host(Node *h, int type, int max, char *err, size_t esz)
{
    plat(h);
    cst_set_host_options(type, max);
    return cs_host(h->s, "steam", 0, 0, err, esz);
}
static int join(Node *c, const char *target, char *err, size_t esz)
{
    plat(c);
    return cs_join(c->s, "steam", target, 0, err, esz);
}
static int join_lobby(Node *c, uint64_t lobby)
{
    char t[40], err[200];
    snprintf(t, sizeof t, "lobby:%llu", ULL(lobby));
    return join(c, t, err, sizeof err);
}
static int command(Node *n, const char *line, char *out, size_t size)
{
    plat(n);
    return cs_command(n->s, line, out, size);
}

static cp_state path_state(int pid, uint32_t t_ms)
{
    cp_state s;
    memset(&s, 0, sizeof s);
    double a = (double)t_ms / 1000.0 * (200.0 / 300.0);
    s.pos[0] = (float)(1000.0 * pid + 300.0 * cos(a)), s.pos[1] = 0, s.pos[2] = (float)(300.0 * sin(a));
    s.yaw = (float)fmod(a + 1.5707963, 6.2831853) - ((fmod(a + 1.5707963, 6.2831853) > 3.1415927) ? 6.2831853f : 0.0f);
    s.action = 3, s.hp = 800, s.hpmax = 800, s.weapon = 2;
    return s;
}

static void start_game(Node *h, Node *c)
{
    char err[100];
    cs_ready(c->s, 1);
    step(200);
    CHECK(cs_start(h->s, 0, err, sizeof err));
    step(300);
    cs_load_done(h->s);
    cs_load_done(c->s);
    step(300);
}

/* host + client (friends) linked by lobby id */
static int connected_pair(Node *h, Node *c, int type)
{
    char err[200];
    if (!host(h, type, 2, err, sizeof err)) { printf("host: %s\n", err); return 0; }
    if (!join_lobby(c, mock_owned_lobby(h->u))) return 0;
    step(2500);
    return c->count[CS_N_CONNECTED] == 1;
}

/* ------------------------------------------------------------------------------------------------------------------ */
static void test_unavailable(void)
{
    printf("-- no Steam: no platform in this process, Steam offline\n");
    world();
    Node *h = mk("H");
    char err[200], out[600];
    cst_set_platform(NULL, NULL);
    CHECK(!cs_host(h->s, "steam", 0, 0, err, sizeof err) && strstr(err, "game process"));
    CHECK(!cs_join(h->s, "steam", "lobby:109775241006780450", 0, err, sizeof err) && strstr(err, "game process"));
    CHECK(cs_command(h->s, "steam", out, sizeof out) && strstr(out, "not available"));
    CHECK(!cs_join(h->s, "steam", "banana", 0, err, sizeof err) && strstr(err, "bad Steam target"));
    mock_set_init_fail(h->u, "Steam is offline (not logged on)");
    plat(h);
    CHECK(!cs_host(h->s, "steam", 0, 0, err, sizeof err) && strstr(err, "offline"));
    CHECK(!cs_active(h->s));
    CHECK(cs_command(h->s, "steam", out, sizeof out) && strstr(out, "not ready") && strstr(out, "offline"));
}

static void test_host_and_join(void)
{
    printf("-- host: lobby data with our tag only, presence; client joins by lobby id; session over the backend\n");
    world();
    mock_set_faults(0, 0, 0, 20, 0, 3);
    Node *h = mk("Alisa77"), *c = mk("Boris6");
    mock_befriend(h->u, c->u);
    char err[200];
    CHECK(host(h, CST_LOBBY_FRIENDS, 2, err, sizeof err));
    uint64_t L = mock_owned_lobby(h->u);
    CHECK(L != 0);
    CHECK(mock_lobby_type(L) == 1 && mock_lobby_limit(L) == 2 && mock_lobby_count(L) == 1);
    CHECK(!strcmp(mock_lobby_get(L, CST_TAG_KEY), CST_TAG_VAL));
    char idtxt[32];
    snprintf(idtxt, sizeof idtxt, "%llu", ULL(mock_id(h->u)));
    CHECK(!strcmp(mock_lobby_get(L, CST_KEY_HOST), idtxt));
    CHECK(!strcmp(mock_lobby_get(L, CST_KEY_NAME), "Alisa77") && !strcmp(mock_lobby_get(L, CST_KEY_MAX), "2"));
    /* never one of the game's own (Raid) keys: its searches and filters cannot see this lobby, and ours cannot be mistaken for theirs */
    CHECK(!mock_lobby_has_key_like(L, "Slot") && !mock_lobby_has_key_like(L, "SearchKey") && !mock_lobby_has_key_like(L, "Owner") && !mock_lobby_has_key_like(L, "Binary") &&
          !mock_lobby_has_key_like(L, "IsBehind") && !mock_lobby_has_key_like(L, "Name"));
    char want[64];
    snprintf(want, sizeof want, "+connect_lobby %llu", ULL(L));
    CHECK(!strcmp(mock_presence(h->u, "connect"), want));
    snprintf(want, sizeof want, "%llu", ULL(L));
    CHECK(!strcmp(mock_presence(h->u, "steam_player_group"), want) && !strcmp(mock_presence(h->u, "steam_player_group_size"), "1"));
    CHECK(strstr(mock_presence(h->u, "status"), "1/2") != NULL);

    CHECK(join_lobby(c, L));
    step(2500);
    CHECK(c->count[CS_N_CONNECTED] == 1 && cs_pid(c->s) == 1 && h->count[CS_N_PEER_JOIN] == 1);
    CHECK(mock_lobby_count(L) == 2 && !strcmp(mock_presence(h->u, "steam_player_group_size"), "2"));
    cs_player_info pi;
    CHECK(cs_player(h->s, 1, &pi) && pi.connected && pi.rtt_ms >= 30 && pi.rtt_ms <= 80);   /* 2 x 20 ms + stepping */
    CHECK(cs_local_character(h->s) == CP_CHAR_CLAIRE && cs_local_character(c->s) == CP_CHAR_MOIRA);
    start_game(h, c);
    CHECK(cs_phase(h->s) == CP_PHASE_PLAYING && cs_phase(c->s) == CP_PHASE_PLAYING);
    cp_state st = path_state(0, cs_now(h->s));
    for (int i = 0; i < 20; i++) {
        cs_submit_state(h->s, &st);
        step(50);
    }
    cs_stats ss;
    cs_get_stats(c->s, &ss);
    CHECK(ss.rx_state > 10);
    CHECK(cs_send_event(h->s, CP_EV_USER, "hello", 5));
    step(200);
    cs_get_stats(c->s, &ss);
    CHECK(ss.rx_event == 1);
}

static void test_join_by_host_id(void)
{
    printf("-- join by the host's SteamID: a friend in the game with a lobby; a public lobby found by search\n");
    world();
    Node *h = mk("Alisa77"), *c = mk("Boris6"), *p = mk("Stranger");
    mock_befriend(h->u, c->u);
    char err[200], t[40];
    CHECK(host(h, CST_LOBBY_FRIENDS, 2, err, sizeof err));
    mock_set_in_game(h->u, 1);
    snprintf(t, sizeof t, "%llu", ULL(mock_id(h->u)));
    CHECK(join(c, t, err, sizeof err));
    step(3000);
    CHECK(c->count[CS_N_CONNECTED] == 1);
    cs_leave(c->s, 0);
    step(500);

    /* a stranger cannot find a friends-only lobby, and Steam would refuse him anyway */
    CHECK(join(p, t, err, sizeof err));
    step(12000);
    CHECK(p->count[CS_N_CONNECTED] == 0 && strstr(cst_last_error(), "no co-op lobby of that player") && p->count[CS_N_REFUSED] == 1);
    cs_leave(p->s, 0);

    /* public lobby: found by the search for r2c_host */
    cs_leave(h->s, 0);
    step(300);
    mock_set_in_game(h->u, 0);
    CHECK(host(h, CST_LOBBY_PUBLIC, 2, err, sizeof err));
    cst_allow(mock_id(p->u));
    CHECK(join(p, t, err, sizeof err));
    step(4000);
    CHECK(p->count[CS_N_CONNECTED] == 1);
}

static void test_policy(void)
{
    printf("-- policy: a stranger in a public lobby is ignored until allowed; any / list modes\n");
    world();
    Node *h = mk("Host"), *s = mk("Stranger");
    char err[200];
    CHECK(host(h, CST_LOBBY_PUBLIC, 2, err, sizeof err));
    uint64_t L = mock_owned_lobby(h->u);
    CHECK(join_lobby(s, L));
    step(4000);
    CHECK(mock_lobby_count(L) == 2 && s->count[CS_N_CONNECTED] == 0 && h->count[CS_N_PEER_JOIN] == 0);   /* in the lobby, but nobody links with him */
    cst_allow(mock_id(s->u));
    step(3000);
    CHECK(s->count[CS_N_CONNECTED] == 1 && h->count[CS_N_PEER_JOIN] == 1);

    world();
    Node *h2 = mk("Host"), *s2 = mk("Stranger");
    CHECK(host(h2, CST_LOBBY_PUBLIC, 2, err, sizeof err));
    CHECK(join_lobby(s2, mock_owned_lobby(h2->u)));
    step(2000);
    CHECK(s2->count[CS_N_CONNECTED] == 0);
    cst_set_policy(1);   /* any */
    step(3000);
    CHECK(s2->count[CS_N_CONNECTED] == 1);

    world();
    Node *h3 = mk("Host"), *f3 = mk("Friend");
    mock_befriend(h3->u, f3->u);
    cst_set_policy(2);   /* allow list only: friends do not count */
    CHECK(host(h3, CST_LOBBY_FRIENDS, 2, err, sizeof err));
    CHECK(join_lobby(f3, mock_owned_lobby(h3->u)));
    step(3000);
    CHECK(f3->count[CS_N_CONNECTED] == 0);
    cst_allow(mock_id(f3->u));
    step(3000);
    CHECK(f3->count[CS_N_CONNECTED] == 1);
}

static void test_foreign_and_full(void)
{
    printf("-- lobbies that are not ours: other games', Raid's, full, private, friends-only\n");
    world();
    Node *h = mk("Host"), *c = mk("Client"), *c2 = mk("Client2"), *x = mk("Other");
    mock_befriend(h->u, c->u), mock_befriend(h->u, c2->u);
    char err[200], out[900];
    uint64_t raid = mock_foreign_lobby(x->u, 2, 1), other = mock_foreign_lobby(x->u, 2, 0);
    CHECK(host(h, CST_LOBBY_PUBLIC, 2, err, sizeof err));
    uint64_t L = mock_owned_lobby(h->u);
    CHECK(command(c, "steamlobbies", out, sizeof out));
    CHECK(strstr(out, "lobbies with our tag") && strstr(out, ": 1") && strstr(out, "Alisa77") == NULL);
    char want[40];
    snprintf(want, sizeof want, "lobby %llu", ULL(L));
    CHECK(strstr(out, want) != NULL);
    snprintf(want, sizeof want, "lobby %llu", ULL(raid));
    CHECK(strstr(out, want) == NULL);
    snprintf(want, sizeof want, "lobby %llu", ULL(other));
    CHECK(strstr(out, want) == NULL);

    /* joining a lobby that has no tag is refused and left again */
    CHECK(join_lobby(c, raid));
    step(2000);
    CHECK(c->count[CS_N_CONNECTED] == 0 && strstr(cst_last_error(), "not a Resident Evil Revelations co-op lobby"));
    CHECK(c->count[CS_N_REFUSED] == 1 && strstr(c->text[CS_N_REFUSED], "not a Resident Evil Revelations co-op lobby"));   /* told at once, with the reason */
    CHECK(mock_lobby_count(raid) == 1);   /* it left */
    cs_leave(c->s, 0);

    /* full: host + client in a 2-player lobby, the second friend is told */
    CHECK(join_lobby(c, L));
    step(2500);
    CHECK(c->count[CS_N_CONNECTED] == 1);
    CHECK(join_lobby(c2, L));
    step(2000);
    CHECK(c2->count[CS_N_CONNECTED] == 0 && strstr(cst_last_error(), "response 4"));
    CHECK(c2->count[CS_N_REFUSED] == 1 && strstr(c2->text[CS_N_REFUSED], "full"));

    /* friends-only: a stranger is not allowed (response 3) */
    world();
    Node *h4 = mk("Host"), *s4 = mk("Stranger");
    CHECK(host(h4, CST_LOBBY_FRIENDS, 2, err, sizeof err));
    CHECK(join_lobby(s4, mock_owned_lobby(h4->u)));
    step(1500);
    CHECK(s4->count[CS_N_CONNECTED] == 0 && strstr(cst_last_error(), "response 3"));
    /* private: only with an invitation */
    world();
    Node *h5 = mk("Host"), *c5 = mk("Friend");
    mock_befriend(h5->u, c5->u);
    CHECK(host(h5, CST_LOBBY_PRIVATE, 2, err, sizeof err));
    uint64_t L5 = mock_owned_lobby(h5->u);
    CHECK(join_lobby(c5, L5));
    step(1500);
    CHECK(c5->count[CS_N_CONNECTED] == 0 && strstr(cst_last_error(), "response 3"));
    cs_leave(c5->s, 0);
    CHECK(command(h5, "steaminvite Friend", out, sizeof out) && strstr(out, "sent"));
    CHECK(join_lobby(c5, L5));
    step(2500);
    CHECK(c5->count[CS_N_CONNECTED] == 1);
}

static void test_session_requests(void)
{
    printf("-- pipe sessions must be accepted: without implicit acceptance, and a stranger's hello is ignored\n");
    world();
    mock_set_implicit_accept(0);
    Node *h = mk("Host"), *c = mk("Client"), *x = mk("Intruder");
    mock_befriend(h->u, c->u), mock_befriend(h->u, x->u);
    CHECK(connected_pair(h, c, CST_LOBBY_FRIENDS));
    CHECK(mock_sessions_requested(h->u) + mock_sessions_requested(c->u) >= 1);   /* whoever spoke second had to accept */
    /* a friend who is not in the lobby sends a well-formed hello: no peer, no session */
    uint64_t L = mock_owned_lobby(h->u);
    uint8_t hello[12] = {'H', 1};
    memcpy(hello + 2, &L, 8);
    hello[10] = 0;
    mock_plat()->pipe_send(x->u, mock_id(h->u), hello, sizeof hello, 1);
    step(1500);
    CHECK(h->count[CS_N_PEER_JOIN] == 1);
    cs_player_info pi;
    CHECK(!cs_player(h->s, 2, &pi) || !pi.used);
}

static void test_disconnects(void)
{
    printf("-- disconnects: bye, host leaves, killed process (silence), left the lobby, broken link, re-join\n");
    char err[200];
    /* client says goodbye */
    world();
    mock_set_faults(0, 0, 0, 10, 0, 1);
    Node *h = mk("Host"), *c = mk("Client");
    mock_befriend(h->u, c->u);
    CHECK(connected_pair(h, c, CST_LOBBY_FRIENDS));
    uint64_t L = mock_owned_lobby(h->u);
    cs_leave(c->s, 0);
    step(800);
    CHECK(h->count[CS_N_PEER_LEAVE] == 1 && mock_lobby_count(L) == 1);
    /* re-join the same lobby: same seat */
    CHECK(join_lobby(c, L));
    step(2500);
    CHECK(c->count[CS_N_CONNECTED] == 2 && h->count[CS_N_PEER_JOIN] == 2 && cs_pid(c->s) == 1);

    /* the host leaves */
    cs_leave(h->s, 0);
    step(800);
    CHECK(c->count[CS_N_LOST] == 1);

    /* the client process dies: the host notices after 30 s of silence */
    world();
    Node *h2 = mk("Host"), *c2 = mk("Client");
    mock_befriend(h2->u, c2->u);
    CHECK(connected_pair(h2, c2, CST_LOBBY_FRIENDS));
    mock_kill(c2->u);
    c2->dead = 1;
    step(25000);
    CHECK(h2->count[CS_N_PEER_LEAVE] == 0 || h2->count[CS_N_PEER_LEAVE] == 1);   /* the broken-pipe path may be faster than the silence rule */
    step(10000);
    CHECK(h2->count[CS_N_PEER_LEAVE] == 1 && h2->last_a[CS_N_PEER_LEAVE] == 1);   /* a = 1: lost without goodbye */

    /* Steam drops the client out of the lobby (no bye) */
    world();
    Node *h3 = mk("Host"), *c3 = mk("Client");
    mock_befriend(h3->u, c3->u);
    CHECK(connected_pair(h3, c3, CST_LOBBY_FRIENDS));
    mock_leave_all(c3->u);
    step(1500);
    CHECK(h3->count[CS_N_PEER_LEAVE] == 0);   /* a short absence is tolerated */
    step(3000);
    CHECK(h3->count[CS_N_PEER_LEAVE] == 1);
    CHECK(c3->count[CS_N_LOST] == 1);          /* and the client sees the lobby empty / its owner gone */

    /* the link breaks (relay down): the pipe state says failed for 5 s */
    world();
    Node *h4 = mk("Host"), *c4 = mk("Client");
    mock_befriend(h4->u, c4->u);
    CHECK(connected_pair(h4, c4, CST_LOBBY_FRIENDS));
    mock_break_link(h4->u, c4->u);
    step(4000);
    CHECK(h4->count[CS_N_PEER_LEAVE] == 0);
    step(4000);
    CHECK(h4->count[CS_N_PEER_LEAVE] == 1 && c4->count[CS_N_LOST] == 1);

    /* silence: the pipe says up but nothing arrives for 30 s */
    world();
    Node *h5 = mk("Host"), *c5 = mk("Client");
    mock_befriend(h5->u, c5->u);
    CHECK(connected_pair(h5, c5, CST_LOBBY_FRIENDS));
    mock_blackhole(h5->u, c5->u);
    step(25000);
    CHECK(h5->count[CS_N_PEER_LEAVE] == 0 && c5->count[CS_N_LOST] == 0);
    step(8000);
    CHECK(h5->count[CS_N_PEER_LEAVE] == 1 && h5->last_a[CS_N_PEER_LEAVE] == 1 && c5->count[CS_N_LOST] == 1);
    (void)err;
}

static void test_stream_under_faults(const char *label, int loss, int dup, int reorder, int delay, int jitter, double max_err, double max_mean)
{
    printf("-- 20 Hz stream over the Steam backend, %s: loss %d%% dup %d%% reorder %d%% delay %d+%d ms\n", label, loss, dup, reorder, delay, jitter);
    world();
    mock_set_faults(0, 0, 0, 10, 0, 7);
    Node *h = mk("Host"), *c = mk("Client");
    mock_befriend(h->u, c->u);
    CHECK(connected_pair(h, c, CST_LOBBY_FRIENDS));
    start_game(h, c);
    mock_set_faults(loss, dup, reorder, delay, jitter, 99);
    double err_sum = 0, err_max = 0;
    int err_n = 0, none = 0, stale = 0, sent = 0;
    for (int t = 0; t < 30000; t += 5) {
        if (t % 50 == 0) {
            cp_state s = path_state(0, cs_now(h->s));
            sent += cs_submit_state(h->s, &s);
            cp_state s2 = path_state(1, cs_now(c->s));
            cs_submit_state(c->s, &s2);
        }
        step(5);
        if (t > 3000 && t % 15 == 0) {
            cp_state out;
            int r = cs_remote_sample(c->s, 0, cs_now(c->s), &out);
            if (r == CS_SAMPLE_NONE) { none++; continue; }
            if (r == CS_SAMPLE_STALE) stale++;
            cp_state truth = path_state(0, out.t_ms);
            double dx = out.pos[0] - truth.pos[0], dz = out.pos[2] - truth.pos[2], e = sqrt(dx * dx + dz * dz);
            err_sum += e, err_n++;
            if (e > err_max) err_max = e;
        }
    }
    cs_stats st;
    cs_get_stats(c->s, &st);
    printf("   client received %u of %d sent (gap %u, dup %u, old %u); error mean %.2f max %.2f over %d samples; stale %d none %d\n", st.rx_state, sent, st.rx_gap, st.rx_dup, st.rx_old,
           err_n ? err_sum / err_n : -1, err_max, err_n, stale, none);
    CHECK(err_n > 1000 && none == 0);
    CHECK(err_max <= max_err);
    CHECK(err_n && err_sum / err_n <= max_mean);
    CHECK(st.rx_state + st.rx_gap >= (uint32_t)sent - 3);
    if (dup) CHECK(st.rx_dup > 0);
    if (loss) CHECK(st.rx_gap > 0);
    CHECK(cs_phase(h->s) == CP_PHASE_PLAYING && cs_phase(c->s) == CP_PHASE_PLAYING);
}

static char runner_line[200];
static int runner_calls;
static int runner(const char *line, char *out, int size)
{
    snprintf(runner_line, sizeof runner_line, "%s", line);
    runner_calls++;
    snprintf(out, (size_t)size, "ok");
    return 1;
}

/* rel010 (b11a live): the game tick asks for the callback objects (Join game, invites) at the first READY hub, also when its first try failed because the
 * game had not created its Steam context yet (the retry comes 3 s later). Before the fix only an untried hub did it, and Join game never reached the mod. */
static void test_callbacks_after_a_late_context(void)
{
    printf("-- the callback objects are asked for when the game's Steam context comes late\n");
    world();
    Node *c = mk("Late");
    plat(c);
    mock_set_init_fail(c->u, "the game has not created its Steam context yet");
    cst_game_tick();
    CHECK(!cst_callbacks_wanted());
    mock_set_init_fail(c->u, NULL);
    Sleep(3100);   /* hub_ready retries a failed hub after 3 s of real time */
    cst_game_tick();
    CHECK(cst_callbacks_wanted());
}

static void test_join_requests_and_filter(void)
{
    printf("-- \"Join game\" / invite requests: pending lobby, auto-join, ignored while in a lobby; the game-callback filter\n");
    world();
    cst_set_runner(runner);
    Node *h = mk("Host"), *c = mk("Client");
    mock_befriend(h->u, c->u);
    char err[200], out[600], conn[80];
    CHECK(host(h, CST_LOBBY_FRIENDS, 2, err, sizeof err));
    uint64_t L = mock_owned_lobby(h->u);
    snprintf(conn, sizeof conn, "+connect_lobby %llu", ULL(L));

    plat(c);
    cst_game_tick();   /* idle: initialises the hub of this process' user, nothing else */
    CHECK(runner_calls == 0);
    mock_click_join_rich(c->u, h->u, conn);
    cst_game_tick();
    CHECK(runner_calls == 1 && strstr(runner_line, "join lobby:") && strstr(runner_line, " steam"));
    char want[40];
    snprintf(want, sizeof want, "lobby:%llu ", ULL(L));
    CHECK(strstr(runner_line, want) != NULL);
    CHECK(cst_take_pending_lobby() == 0);

    mock_click_join_lobby(c->u, L, h->u);   /* the invite way */
    cst_game_tick();
    CHECK(runner_calls == 2);

    /* a request for a lobby with no connect string of ours does nothing */
    mock_click_join_rich(c->u, h->u, "+connect 1.2.3.4:27015");
    cst_game_tick();
    CHECK(runner_calls == 2);

    /* once in a lobby, requests are ignored */
    CHECK(join_lobby(c, L));
    step(2500);
    CHECK(c->count[CS_N_CONNECTED] == 1);
    mock_click_join_lobby(c->u, L + 1, h->u);
    cst_game_tick();
    CHECK(runner_calls == 2);

    /* the callback filter */
    plat(h);
    cst_game_tick();
    uint64_t pv[3] = {L, 0, 0};
    CHECK(cst_swallow_game_callback(333, pv) == 1 && cst_swallow_game_callback(505, pv) == 1 && cst_swallow_game_callback(506, pv) == 1);
    CHECK(cst_swallow_game_callback(1202, pv) == 0 && cst_swallow_game_callback(304, pv) == 0);
    Node *x = mk("Other");
    uint64_t raid = mock_foreign_lobby(x->u, 2, 1), other = mock_foreign_lobby(x->u, 2, 0);
    uint64_t rv[3] = {raid, 0, 0}, ov[3] = {other, 0, 0};
    CHECK(cst_swallow_game_callback(505, rv) == 0 && cst_swallow_game_callback(506, rv) == 0);   /* data of a lobby that is not ours */
    CHECK(cst_swallow_game_callback(333, ov) == 1);   /* unknown lobby, no Raid key visible: the invite can only be ours */
    /* a Raid lobby that the user can see (searched): the game gets the request */
    plat(h);
    mock_plat()->lobby_search(h->u, NULL, NULL, NULL, NULL, 10);
    cst_event ev;
    while (mock_plat()->poll(h->u, &ev)) {}
    CHECK(cst_swallow_game_callback(333, rv) == 0);
    SetEnvironmentVariableA("REV2_STEAM_SWALLOW", "0");
    CHECK(cst_swallow_game_callback(333, pv) == 0);
    SetEnvironmentVariableA("REV2_STEAM_SWALLOW", NULL);
    (void)out;
    cst_set_runner(NULL);
}

static void test_commands(void)
{
    printf("-- console commands: steam, steamhost, steamjoin, steamlobbies, steaminvite, steamallow\n");
    world();
    Node *h = mk("Alisa77"), *c = mk("Boris6"), *z = mk("Zed");
    mock_befriend(h->u, c->u), mock_befriend(h->u, z->u);
    char out[900], t[120];
    CHECK(command(h, "steam", out, sizeof out) && strstr(out, "ready (mock)") && strstr(out, "no lobby"));
    CHECK(command(h, "steamhost public 3", out, sizeof out) && strstr(out, "hosting a Steam lobby (public, up to 3 players)"));
    uint64_t L = mock_owned_lobby(h->u);
    CHECK(mock_lobby_type(L) == 2 && mock_lobby_limit(L) == 3);
    CHECK(command(h, "steam", out, sizeof out) && strstr(out, "(1 member(s))"));
    CHECK(command(h, "steaminvite", out, sizeof out) && strstr(out, "dialog opened"));
    CHECK(command(h, "steaminvite nobody", out, sizeof out) && strstr(out, "no friend"));
    CHECK(command(h, "steaminvite boris", out, sizeof out) && strstr(out, "sent"));
    snprintf(t, sizeof t, "steamjoin %llu", ULL(mock_id(h->u)));
    CHECK(command(c, t, out, sizeof out) && strstr(out, "joining"));
    step(3000);
    CHECK(c->count[CS_N_CONNECTED] == 1);
    CHECK(command(c, "steam", out, sizeof out) && strstr(out, "member(s)"));
    CHECK(command(z, "steamjoin", out, sizeof out) && strstr(out, "usage"));
    CHECK(command(z, "steamjoin pending", out, sizeof out) && strstr(out, "no pending"));
    CHECK(command(z, "steamjoin 12", out, sizeof out) && strstr(out, "bad Steam target"));
    snprintf(t, sizeof t, "steamjoin %llu", ULL(mock_id(z->u)));
    CHECK(command(z, t, out, sizeof out) && strstr(out, "own SteamID"));
    CHECK(command(h, "steamallow any", out, sizeof out) && strstr(out, "ANY"));
    CHECK(command(h, "steamallow friends", out, sizeof out) && strstr(out, "friends"));
    CHECK(command(h, "steamallow banana", out, sizeof out) && strstr(out, "usage"));
    snprintf(t, sizeof t, "steamallow %llu", ULL(mock_id(z->u)));
    CHECK(command(h, t, out, sizeof out) && strstr(out, "added"));
    CHECK(command(h, "steampipe", out, sizeof out) && strstr(out, "no real Steam platform"));
    CHECK(command(h, "stop", out, sizeof out) && !cs_active(h->s));
    step(300);
    CHECK(!mock_lobby_exists(L) || mock_lobby_count(L) <= 1);
    CHECK(command(h, "steam", out, sizeof out) && strstr(out, "no lobby"));
    CHECK(strlen(mock_presence(h->u, "connect")) == 0);   /* presence cleared: no stale "Join game" */
}

static void test_transport_registered(void)
{
    printf("-- transport selection: the name 'steam' is the real backend now\n");
    world();
    Node *h = mk("H");
    char err[200];
    cst_set_platform(NULL, NULL);
    CHECK(!cs_host(h->s, "steam", 0, 0, err, sizeof err) && !strstr(err, "not implemented") && !strstr(err, "unknown transport"));
    CHECK(!strcmp(cn_ops_steam.name, "steam"));
}

static void print_log(const char *line) { printf("      | %s\n", line); }

int main(void)
{
    if (getenv("STEAM_TEST_LOG")) cn_set_log(print_log);
    /* lm6 live: coop_steam.c compares the friends' game with ITS app id; the mock's friends play Rev1 (222480, the steam_appid.txt of the lab copies):
     * with Rev2's 287290 left in coop_steam.c the join-by-friend tests below fail */
    CHECK(MOCK_APP == 222480u);
    mock_reset();
    test_unavailable();
    test_host_and_join();
    test_join_by_host_id();
    test_policy();
    test_foreign_and_full();
    test_session_requests();
    test_disconnects();
    test_stream_under_faults("clean", 0, 0, 0, 20, 0, 1.0, 0.5);
    test_stream_under_faults("loss 15%", 15, 0, 0, 40, 10, 6.0, 1.5);
    test_stream_under_faults("bad wifi", 25, 5, 15, 80, 40, 20.0, 4.0);
    test_join_requests_and_filter();
    test_callbacks_after_a_late_context();
    test_commands();
    test_transport_registered();
    world();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
