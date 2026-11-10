/*
 * Offline tests of the network core (native/net1.c over lobby_link.c, the coop session and the in-process "sim" transport): the console `net` command,
 * the real exe tag (a host refuses another build), the whole path lobby -> roles -> settings -> both ready -> automatic session start -> load barrier
 * (LOAD_BEGIN / GO per epoch, also with loss, duplication and reordering of the unreliable packets), a lobby that is closed and opened again while
 * the link lives, a link started from the console that a lobby picks up later, stop and restart. The shared module net1 is the single link of the game
 * and here the host (or the joiner); the other player is a plain lk_t. Real time (the sim delivers at once), a few seconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_net.h"
#include "../native/lobby1.h"
#include "../native/net1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define REAL_SHA "1573b79eb921b1e7f571e6deb71c39961c7f1940fad6d19283fc3aea7fca48aa"

static char logbuf[16000];
static void tlog(const char *line)
{
    size_t n = strlen(logbuf);
    if (n + strlen(line) + 2 < sizeof logbuf) snprintf(logbuf + n, sizeof logbuf - n, "%s\n", line);
}

static l1_t *open_lobby(int mode, const char *join_addr)
{
    l1_t *l = l1_new();
    l1_settings s;
    memset(&s, 0, sizeof s);
    s.armed = 1, s.net_mode = mode;
    snprintf(s.join_addr, sizeof s.join_addr, "%s", join_addr ? join_addr : "");
    l1_env env;
    memset(&env, 0, sizeof env);
    env.lang = 0, env.net_rows = 1;
    l1_open(l, &s, &env);
    return l;
}

static int find_row(l1_t *l, int label_id)
{
    l1_view v;
    l1_view_get(l, &v);
    for (int i = 0; i < v.nrows; i++)
        if (!strcmp(v.rows[i].label, l1_text(label_id, 0))) return i;
    return -1;
}

static void click(l1_t *l, int label_id)
{
    int row = find_row(l, label_id);
    l1_event e;
    memset(&e, 0, sizeof e);
    e.kind = L1E_CLICK, e.row = row;
    if (row >= 0) l1_event_in(l, &e);
}

/* the value shown in the "You play" row (lobby-must: the network modes name the role row "You play"; a joined player sees it as information) */
static int p1_row_says(l1_t *l, int role_text)
{
    l1_view v;
    l1_view_get(l, &v);
    int r = find_row(l, L1T_YOU_PLAY);
    return r >= 0 && strstr(v.rows[r].value, l1_text(role_text, 0)) != NULL;
}

/* one step of the game: the shared link (net1_frame) and the other player's link */
static lk_t *g_other;
static void step(void)
{
    net1_frame();
    if (g_other) lk_poll(g_other);
    Sleep(2);
}
static void pump(int ms)
{
    for (int t = 0; t < ms; t += 2) step();
}
#define UNTIL(ms, cond) do { int t_ = 0; while (!(cond) && t_ < (ms)) { step(); t_ += 2; } } while (0)

static void send_states(lk_t *a, lk_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        cp_state st;
        memset(&st, 0, sizeof st);
        st.pos[0] = (float)i, st.hp = 800, st.hpmax = 800, st.action = 3;
        if (a) cs_submit_state(lk_session(a), &st);
        if (b) cs_submit_state(lk_session(b), &st);
        pump(50);
    }
}

static int info_started(lk_t *k)
{
    lk_info in;
    lk_get_info(k, &in);
    return in.started;
}
static int info_phase(lk_t *k)
{
    lk_info in;
    lk_get_info(k, &in);
    return in.phase;
}
static int info_gos(lk_t *k)
{
    lk_info in;
    lk_get_info(k, &in);
    return in.gos;
}

int main(void)
{
    cn_sim_real_time();
    char out[3000];
    net1_cfg nc;
    memset(&nc, 0, sizeof nc);
    nc.transport = "sim", nc.threaded = 0, nc.exe_sha = REAL_SHA, nc.name = "Host PC", nc.log = tlog;
    net1_configure(&nc);

    /* ---- 1. the exe tag: the first 8 bytes of the SHA-256 of build 13298487 */
    {
        uint8_t t[8];
        static const uint8_t want[8] = {0x15, 0x73, 0xB7, 0x9E, 0xB9, 0x21, 0xB1, 0xE7};
        CHECK(net1_parse_exe_tag(REAL_SHA, t) == 1 && memcmp(t, want, 8) == 0);
        CHECK(net1_parse_exe_tag("1573B79EB921B1E7F571E6DEB71C39961C7F1940FAD6D19283FC3AEA7FCA48AA", t) == 1 && memcmp(t, want, 8) == 0);
        CHECK(net1_parse_exe_tag("", t) == 0 && t[0] == 0 && t[7] == 0);
        CHECK(net1_parse_exe_tag("1573b79eb921b1e", t) == 0);   /* too short */
        CHECK(net1_parse_exe_tag("1573b79eb921b1eg", t) == 0);   /* not hex */
        CHECK(net1_parse_exe_tag(NULL, t) == 0);
    }

    /* ---- 2. the console without a link */
    {
        net1_cmd("status", out, sizeof out);
        CHECK(strstr(out, "off") != NULL);
        net1_cmd("ready", out, sizeof out);
        CHECK(strstr(out, "not running") != NULL);
        net1_cmd("join nonsense/address", out, sizeof out);
        CHECK(strstr(out, "must look like") != NULL && strstr(out, "28410") != NULL);
        net1_cmd("join", out, sizeof out);
        CHECK(strstr(out, "must look like") != NULL);
        net1_cmd("host 0", out, sizeof out);
        CHECK(strstr(out, "port must be") != NULL);
        net1_cmd("host 70000", out, sizeof out);
        CHECK(strstr(out, "port must be") != NULL);
        net1_cmd("bogus", out, sizeof out);
        CHECK(strstr(out, "not running") != NULL);
    }

    /* ---- 3. a host started from the console, a joiner with the same build: the whole path from the lobby to a started session */
    lk_config cc;
    memset(&cc, 0, sizeof cc);
    cc.name = "Guest PC";
    net1_parse_exe_tag(REAL_SHA, cc.exe_tag);
    {
        net1_cmd("host", out, sizeof out);
        CHECK(strstr(out, "hosting on port 28410") != NULL);
        net1_cmd("status", out, sizeof out);
        CHECK(strstr(out, "link: host, waiting for a player") != NULL);
        CHECK(strstr(logbuf, "net: host started") != NULL);
        CHECK(net1_link_peek() != NULL && lk_state(net1_link_peek()) == L1NS_WAIT);

        l1_t *cl = open_lobby(L1N_JOIN, "sim:28410");
        lk_t *ck = lk_new_cfg(cl, "sim", 0, &cc);
        g_other = ck;
        char err[160];
        CHECK(lk_start(ck, L1N_JOIN, "sim:28410", 0, NULL, 0, err, sizeof err) == 1);
        UNTIL(3000, lk_state(ck) == L1NS_CONNECTED && lk_state(net1_link_peek()) == L1NS_CONNECTED);
        CHECK(lk_state(ck) == L1NS_CONNECTED && lk_state(net1_link_peek()) == L1NS_CONNECTED);

        /* the lobby opens AFTER the link was started from the console: it picks the state, the mode and the roles up */
        l1_t *hl = open_lobby(L1N_OFF, "");
        net1_attach_lobby(hl);
        CHECK(l1_net_state(hl) == L1NS_CONNECTED);
        l1_settings hs;
        l1_settings_get(hl, &hs);
        CHECK(hs.net_mode == L1N_HOST);

        /* roles: the host plays the lead (default), the joiner the partner; the host's pick from the console swaps both */
        pump(60);
        {
            lk_info hi, ci;
            lk_get_info(net1_link_peek(), &hi);
            lk_get_info(ck, &ci);
            CHECK(hi.host == 1 && hi.role == 0 && hi.remote_role == 1);
            CHECK(ci.host == 0 && ci.role == 1 && ci.remote_role == 0);
            CHECK(!strcmp(ci.peer_name, "Host PC") && !strcmp(hi.peer_name, "Guest PC"));
            CHECK(p1_row_says(cl, L1T_PARTNER));   /* the joiner's lobby shows what the host gave him and does not offer a change */
            CHECK(find_row(cl, L1T_YOU_PLAY) >= 0);
            {
                l1_view v;
                l1_view_get(cl, &v);
                CHECK(v.rows[find_row(cl, L1T_YOU_PLAY)].action == 0);   /* information only: the host decides */
            }
        }
        net1_cmd("role partner", out, sizeof out);
        CHECK(strstr(out, "plays partner") != NULL);
        pump(60);
        {
            lk_info hi, ci;
            lk_get_info(net1_link_peek(), &hi);
            lk_get_info(ck, &ci);
            CHECK(hi.role == 1 && hi.remote_role == 0 && ci.role == 0 && ci.remote_role == 1);
            CHECK(p1_row_says(cl, L1T_LEAD));
        }
        /* the role set in the lobby model of the host wins when a lobby is attached (the model is the source) */
        {
            l1_net_set_role(hl, L1_LEAD);
            pump(60);
            lk_info hi, ci;
            lk_get_info(net1_link_peek(), &hi);
            lk_get_info(ck, &ci);
            CHECK(hi.role == 0 && ci.role == 1);
        }

        /* the host's settings travel in the session record */
        net1_cmd("set episode 1 difficulty 2 flags 3 area 91010100 seed abcd1234", out, sizeof out);
        CHECK(strstr(out, "settings sent") != NULL);
        net1_cmd("set bogus 1", out, sizeof out);
        CHECK(strstr(out, "unknown setting") != NULL);
        pump(60);
        {
            lk_info ci;
            lk_get_info(ck, &ci);
            CHECK(ci.set.episode == 1 && ci.set.difficulty == 2 && ci.set.flags == 3 && ci.set.area_id == 0x91010100u && ci.set.seed == 0xabcd1234u);
        }

        /* closing the lobby does not touch the link; a new lobby shows the same state */
        net1_attach_lobby(NULL);
        pump(100);
        CHECK(lk_state(net1_link_peek()) == L1NS_CONNECTED && lk_state(ck) == L1NS_CONNECTED);
        l1_free(hl);
        hl = open_lobby(L1N_OFF, "");
        net1_attach_lobby(hl);
        CHECK(l1_net_state(hl) == L1NS_CONNECTED);
        l1_settings_get(hl, &hs);
        CHECK(hs.net_mode == L1N_HOST);
        CHECK(find_row(hl, L1T_NET_STOP) >= 0 && find_row(hl, L1T_NET_READY) >= 0);

        /* nobody is ready: no automatic start, a manual start says why */
        pump(150);
        CHECK(!info_started(net1_link_peek()) && !info_started(ck));
        net1_cmd("start", out, sizeof out);
        CHECK(strstr(out, "ready") != NULL && strstr(out, "session started") == NULL);

        /* the joiner is ready (his lobby row), the host not yet: still no start */
        click(cl, L1T_NET_READY);
        pump(150);
        CHECK(l1_net_ready(cl) == 1);
        CHECK(!info_started(net1_link_peek()));
        /* the host changes the roles while the joiner waits: both ready flags are withdrawn (the joiner confirms again) */
        net1_cmd("role partner", out, sizeof out);
        l1_net_set_role(hl, L1_PARTNER);
        pump(150);
        CHECK(l1_net_ready(cl) == 0);
        CHECK(!info_started(net1_link_peek()));
        click(cl, L1T_NET_READY);
        pump(100);
        CHECK(l1_net_ready(cl) == 1);
        /* the host is ready too (console flag): the host starts the session by itself */
        net1_cmd("ready", out, sizeof out);
        UNTIL(3000, info_started(net1_link_peek()) && info_started(ck));
        CHECK(info_started(net1_link_peek()) && info_started(ck));
        CHECK(strstr(logbuf, "net: session STARTED, this PC plays partner, other lead") != NULL);
        {
            lk_info hi, ci;
            lk_get_info(net1_link_peek(), &hi);
            lk_get_info(ck, &ci);
            CHECK(hi.role == 1 && ci.role == 0);   /* final */
            CHECK(hi.set.episode == 1 && ci.set.episode == 1 && hi.set.difficulty == 2 && ci.set.difficulty == 2);
            CHECK(hi.set.flags == 3 && ci.set.flags == 3 && hi.set.area_id == 0x91010100u && ci.set.area_id == 0x91010100u);
            CHECK(hi.set.seed == 0xabcd1234u && ci.set.seed == 0xabcd1234u);
            CHECK(hi.phase == CP_PHASE_LOADING && ci.phase == CP_PHASE_LOADING && hi.epoch == 1 && ci.epoch == 1);
        }
        /* after the start the roles cannot be changed any more */
        net1_cmd("role lead", out, sizeof out);
        pump(60);
        {
            lk_info hi;
            lk_get_info(net1_link_peek(), &hi);
            CHECK(hi.role == 1);
        }
        net1_cmd("set episode 2", out, sizeof out);
        CHECK(strstr(out, "only the host") != NULL);

        /* ---- 4. the load barrier with a bad line: loss, duplication and reordering of the unreliable packets (states) */
        {
            cn_sim_params sp;
            memset(&sp, 0, sizeof sp);
            sp.loss_pct = 25, sp.dup_pct = 10, sp.reorder_pct = 10, sp.delay_ms = 4, sp.jitter_ms = 6, sp.reorder_extra_ms = 20, sp.seed = 5;
            cn_sim_set(&sp);
            lk_t *hk = net1_link_peek();
            /* the first load (the section start): both begin, the joiner is slower */
            lk_load_begin(hk, 0x91010100u);
            lk_load_begin(ck, 0x91010100u);
            send_states(hk, ck, 3);
            CHECK(info_phase(hk) == CP_PHASE_LOADING && info_phase(ck) == CP_PHASE_LOADING);
            CHECK(info_gos(hk) == 0 && info_gos(ck) == 0);
            lk_load_done(hk);
            pump(150);
            CHECK(info_phase(hk) == CP_PHASE_LOADING);   /* the barrier holds the host until the joiner is done too */
            lk_load_done(ck);
            UNTIL(3000, info_phase(hk) == CP_PHASE_PLAYING && info_phase(ck) == CP_PHASE_PLAYING);
            CHECK(info_phase(hk) == CP_PHASE_PLAYING && info_phase(ck) == CP_PHASE_PLAYING);
            {
                lk_info hi, ci;
                lk_get_info(hk, &hi);
                lk_get_info(ck, &ci);
                CHECK(hi.gos == 1 && ci.gos == 1 && hi.last_go_epoch == 1 && ci.last_go_epoch == 1);
                CHECK(hi.load_begins >= 1 && ci.load_begins >= 1);   /* each side saw the other's LOAD_BEGIN (the own one too: any player's) */
            }
            /* playing: states flow both ways through the bad line */
            send_states(hk, ck, 30);
            {
                cp_state ss;
                int hr = cs_remote_pid(lk_session(hk)), cr = cs_remote_pid(lk_session(ck));
                CHECK(hr >= 0 && cr >= 0);
                CHECK(cs_remote_sample(lk_session(hk), hr, cs_now(lk_session(hk)), &ss) != CS_SAMPLE_NONE);
                CHECK(cs_remote_sample(lk_session(ck), cr, cs_now(lk_session(ck)), &ss) != CS_SAMPLE_NONE);
            }
            /* a checkpoint load started by the joiner: the host follows (the glue calls lk_load_begin when it sees LOAD_BEGIN), epoch 2 */
            lk_load_begin(ck, 0x91010200u);
            UNTIL(2000, info_phase(hk) == CP_PHASE_LOADING);
            CHECK(info_phase(hk) == CP_PHASE_LOADING);
            lk_load_begin(hk, 0x91010200u);
            lk_load_done(ck);
            pump(100);
            CHECK(info_phase(hk) == CP_PHASE_LOADING);
            lk_load_done(hk);
            UNTIL(3000, info_phase(hk) == CP_PHASE_PLAYING && info_phase(ck) == CP_PHASE_PLAYING);
            {
                lk_info hi, ci;
                lk_get_info(hk, &hi);
                lk_get_info(ck, &ci);
                CHECK(info_phase(hk) == CP_PHASE_PLAYING && info_phase(ck) == CP_PHASE_PLAYING);
                CHECK(hi.gos == 2 && ci.gos == 2 && hi.last_go_epoch == 2 && ci.last_go_epoch == 2 && hi.epoch == 2 && ci.epoch == 2);
            }
            /* an area change on the host while the joiner's state is lost: epoch 3 */
            lk_load_begin(hk, 0x91010300u);
            UNTIL(2000, info_phase(ck) == CP_PHASE_LOADING);
            lk_load_begin(ck, 0x91010300u);
            lk_load_done(hk);
            lk_load_done(ck);
            UNTIL(3000, info_phase(hk) == CP_PHASE_PLAYING && info_phase(ck) == CP_PHASE_PLAYING);
            {
                lk_info hi, ci;
                lk_get_info(hk, &hi);
                lk_get_info(ck, &ci);
                CHECK(hi.gos == 3 && ci.gos == 3 && hi.epoch == 3 && ci.epoch == 3);
            }
            cn_sim_set(&(cn_sim_params){0});
        }
        CHECK(strstr(logbuf, "net: phase") != NULL);   /* the frame logs the barrier */

        /* the console's own barrier commands */
        net1_cmd("load 5", out, sizeof out);
        CHECK(strstr(out, "load begins") != NULL);
        UNTIL(2000, info_phase(ck) == CP_PHASE_LOADING);
        lk_load_begin(ck, 5);
        lk_load_done(ck);
        net1_cmd("done", out, sizeof out);
        UNTIL(3000, info_phase(ck) == CP_PHASE_PLAYING && info_phase(net1_link_peek()) == CP_PHASE_PLAYING);
        CHECK(info_phase(ck) == CP_PHASE_PLAYING && info_gos(ck) == 4);

        /* status shows the roles, the settings and the barrier counters */
        net1_cmd("status", out, sizeof out);
        CHECK(strstr(out, "STARTED") != NULL && strstr(out, "you partner, other lead") != NULL && strstr(out, "episode 1 difficulty 2") != NULL);

        /* ---- 5. stop: the joiner learns that the host is gone; a restart is a fresh lobby (not started, epoch 0) */
        net1_cmd("stop", out, sizeof out);
        CHECK(strstr(out, "stopped") != NULL);
        CHECK(lk_state(net1_link_peek()) == L1NS_IDLE && l1_net_state(hl) == L1NS_IDLE);
        UNTIL(3000, lk_state(ck) == L1NS_ERROR);
        CHECK(lk_state(ck) == L1NS_ERROR && lk_last_reason(ck)[0] != 0);
        lk_stop(ck);
        net1_cmd("host", out, sizeof out);
        CHECK(strstr(out, "hosting") != NULL);
        {
            lk_info hi;
            lk_get_info(net1_link_peek(), &hi);
            CHECK(!hi.started && hi.role == 0 && hi.remote_role == -1);
        }
        CHECK(lk_start(ck, L1N_JOIN, "sim:28410", 0, NULL, 0, err, sizeof err) == 1);
        UNTIL(3000, lk_state(ck) == L1NS_CONNECTED && lk_state(net1_link_peek()) == L1NS_CONNECTED);
        CHECK(lk_state(ck) == L1NS_CONNECTED && lk_state(net1_link_peek()) == L1NS_CONNECTED);
        {
            lk_info hi, ci;
            lk_get_info(net1_link_peek(), &hi);
            lk_get_info(ck, &ci);
            CHECK(!hi.started && !ci.started && hi.epoch == 0 && ci.epoch == 0);
        }
        /* a forced start without the ready flags (developer) */
        net1_cmd("start force", out, sizeof out);
        CHECK(strstr(out, "session started") != NULL);
        UNTIL(2000, info_started(ck));
        CHECK(info_started(ck));

        net1_cmd("stop", out, sizeof out);
        lk_stop(ck);
        g_other = NULL;
        lk_free(ck);
        l1_free(cl);
        net1_attach_lobby(NULL);
        l1_free(hl);
    }

    /* ---- 6. another build is refused (the exe tag), a build that is not known (zeros) is not checked */
    {
        lk_config other;
        memset(&other, 0, sizeof other);
        other.name = "Other build";
        net1_parse_exe_tag("aabbccddeeff0011", other.exe_tag);
        lk_t *ok = lk_new_cfg(NULL, "sim", 0, &other);
        char err[160];
        g_other = ok;
        CHECK(lk_start(ok, L1N_HOST, "", 28411, NULL, 0, err, sizeof err) == 1);
        net1_cmd("join sim:28411", out, sizeof out);
        CHECK(strstr(out, "joining") != NULL);
        UNTIL(3000, lk_state(net1_link_peek()) == L1NS_ERROR);
        CHECK(lk_state(net1_link_peek()) == L1NS_ERROR);
        CHECK(lk_last_reason(net1_link_peek())[0] != 0);
        net1_cmd("status", out, sizeof out);
        CHECK(strstr(out, "error") != NULL);
        CHECK(lk_state(ok) == L1NS_WAIT || lk_state(ok) == L1NS_CONNECTING);   /* the host never got a player */
        net1_cmd("stop", out, sizeof out);
        lk_stop(ok);
        lk_free(ok);
        g_other = NULL;

        /* the same host without a tag (a bot or a test) accepts the real build */
        lk_config zero;
        memset(&zero, 0, sizeof zero);
        zero.name = "Bot";
        lk_t *bot = lk_new_cfg(NULL, "sim", 0, &zero);
        g_other = bot;
        CHECK(lk_start(bot, L1N_HOST, "", 28412, NULL, 0, err, sizeof err) == 1);
        net1_cmd("join sim:28412", out, sizeof out);
        UNTIL(3000, lk_state(net1_link_peek()) == L1NS_CONNECTED && lk_state(bot) == L1NS_CONNECTED);
        CHECK(lk_state(net1_link_peek()) == L1NS_CONNECTED && lk_state(bot) == L1NS_CONNECTED);
        /* as the joiner the shared link gets the host's role and settings; both ready: the HOST (the bot) starts */
        net1_cmd("ready", out, sizeof out);
        lk_set_ready(bot, 1);
        UNTIL(3000, info_started(bot) && info_started(net1_link_peek()));
        CHECK(info_started(bot) && info_started(net1_link_peek()));
        {
            lk_info ji;
            lk_get_info(net1_link_peek(), &ji);
            CHECK(ji.host == 0 && ji.role == 1);
        }
        net1_cmd("role lead", out, sizeof out);
        CHECK(strstr(out, "only the host") != NULL);
        net1_cmd("stop", out, sizeof out);
        lk_stop(bot);
        lk_free(bot);
        g_other = NULL;
    }

    net1_shutdown();
    printf("test_netcore: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
