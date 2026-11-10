/*
 * Headless co-op bot: speaks the real protocol over real UDP (ENet) with the same session code as the game, no game needed.
 *
 *   coop_bot host [--port 28411] [--char claire] [--name Host] [--players 2] ...
 *   coop_bot join <ip:port> [--char moira] [--name Guest] ...
 *
 * Script of a run: connect -> lobby (pick / ready, the host starts when everybody is ready) -> simulated level load (--load-ms)
 * -> PLAYING: walks a circle (radius 300, 200 units/s, centre 1000*pid) and sends its state at 20 Hz, samples the other
 * player at 60 Hz and measures the position error against the other bot's known path -> optional second level load
 * (--load-at), an event (--event-at), an early leave (--leave-at) -> prints one RESULT json line.
 * Options: --seconds N (playing time, default 12) --load-ms N (default 800) --load-at S --event-at S --leave-at S
 *          --autostart-delay MS --no-ready --verbose --exe-tag HEX16 --die-at S (exit without goodbye)
 *
 * --lk: the lobby / start / load scenario of the game's network core instead (lobby_link.c, the same code the proxy's `net` command and the lobby rows
 * drive; ENet): the bot is the other PC of a game that runs `net host` / `net join`. Script: connect -> host: role + settings -> ready (after --ready-delay MS)
 * -> the host starts when both are ready -> loads (--loads N: the first is the section start, later ones are checkpoint loads that the joining bot starts after
 * --load-gap S; a load that the other side begins is joined at once; each takes --load-ms) -> plays --seconds with states at 20 Hz -> one RESULT json line with
 * the roles, the settings it saw, the barrier counters (go count, last epoch) and the remote samples.
 *   host: --role lead|partner (what the bot's human plays) --episode N --difficulty N --flags N --area HEX --seed HEX
 *   both: --name S --exe-tag HEX16 --ready-delay MS --loads N --load-gap S --load-ms N --seconds N --area-id HEX (the area announced in LOAD_BEGIN) --no-ready
 * --pad (with --lk): the puppet traffic of the game. The bot sends its pad records (a scripted pad at 60 Hz through puppet1.c: dedup, three records per packet, keepalive) in CP_PAD packets and
 *   feeds the packets of the other side to a puppet1.c receiver (a counting sink instead of capture_push_remote); one extra line "PAD {...}" reports the sent records, received / lost / late /
 *   repeated records, order violations of the pushed records, neutral pushes, stalls / resumes, and the delay of the packets (one PC, one clock: receive time minus the sender's stamp).
 *   --pad-hold-at S --pad-hold-for S : the bot's flags say PAUSED for that long (the other side must hold its puppet neutral)   --pad-silent-at S --pad-silent-for S : no pad packets at all (a link cut)
 *   build: tests/build.sh builds it (build/coop_bot.exe); the sim-transport version of the same scenario, with loss, is tests/test_netcore.c
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "../native/coop_session.h"
#include "../native/lobby_link.h"
#include "../native/puppet1.h"

static int verbose;
static DWORD t_start;

static void logline(const char *line)
{
    if (verbose) fprintf(stderr, "[%6lu] %s\n", (unsigned long)(GetTickCount() - t_start), line);
}

typedef struct {
    int connected, refused, lost, started, go_count, load_begin_count, events, notices, lobby_changes;
    char refuse_text[120];
    int refuse_code;
} Notes;
static Notes notes;

static void on_note(void *u, const cs_note *n)
{
    (void)u;
    switch (n->what) {
    case CS_N_CONNECTED: notes.connected++; break;
    case CS_N_REFUSED: notes.refused++, notes.refuse_code = n->a, snprintf(notes.refuse_text, sizeof notes.refuse_text, "%s", n->text ? n->text : ""); break;
    case CS_N_LOST: notes.lost++; break;
    case CS_N_START: notes.started++; break;
    case CS_N_GO: notes.go_count++; break;
    case CS_N_LOAD_BEGIN: notes.load_begin_count++; break;
    case CS_N_EVENT: notes.events++; break;
    case CS_N_NOTICE: notes.notices++; break;
    case CS_N_LOBBY: notes.lobby_changes++; break;
    default: break;
    }
    if (verbose) {
        char b[200];
        snprintf(b, sizeof b, "note %d pid %d a %d b %d %s", n->what, n->pid, n->a, n->b, n->text ? n->text : "");
        logline(b);
    }
}

static cp_state path_state(int pid, uint32_t t_ms)
{
    cp_state s;
    memset(&s, 0, sizeof s);
    double a = (double)t_ms / 1000.0 * (200.0 / 300.0);
    s.pos[0] = (float)(1000.0 * pid + 300.0 * cos(a)), s.pos[1] = 0, s.pos[2] = (float)(300.0 * sin(a));
    double y = fmod(a + 1.5707963, 6.2831853);
    s.yaw = (float)(y > 3.1415927 ? y - 6.2831853 : y);
    s.action = 3, s.hp = 800, s.hpmax = 800, s.weapon = 2;
    s.flags = ((t_ms / 1000) & 1) ? CP_SF_AIMING : 0;
    s.aim = (float)(30.0 * sin(a)), s.aimv[0] = 0.0f, s.aimv[1] = 0.0f, s.aimv[2] = 1.0f;
    return s;
}

static const char *arg_str(int argc, char **argv, const char *name, const char *def)
{
    const char *r = def;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) r = argv[i + 1];   /* the last one wins */
    return r;
}
static double arg_num(int argc, char **argv, const char *name, double def)
{
    const char *v = arg_str(argc, argv, name, NULL);
    return v ? atof(v) : def;
}
static int arg_flag(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name)) return 1;
    return 0;
}

/* ---------------------------------------------------------------- --lk: the game's network core as the other PC */
static unsigned hexarg(int argc, char **argv, const char *name, unsigned def)
{
    const char *v = arg_str(argc, argv, name, NULL);
    return v ? (unsigned)strtoul(v, NULL, 16) : def;
}

/* ---- --pad ---- */
typedef struct {
    int n, bad_order, neutral, last_lx, repeat_same;
    unsigned pushed;
} PadSink;
static int pad_sink_push(void *u, const cp_pad_rec *r, uint32_t seq, uint32_t ts)
{
    PadSink *s = u;
    (void)seq, (void)ts;
    s->pushed++;
    if (!r->lx && !r->ly && !r->buttons && !r->lt && !r->rt && !r->rx && !r->ry) {
        s->neutral++;
        return 1;
    }
    if (s->n && r->lx < s->last_lx && s->last_lx - r->lx < 20000) s->bad_order++;   /* a wrap of the counter is a big drop */
    s->last_lx = r->lx, s->n++;
    return 1;
}

static void make_bot_pad(cp_pad_rec *r, unsigned frame, DWORD ms)
{
    memset(r, 0, sizeof *r);
    r->lx = (int16_t)(1000 + (int)((frame * 2) % 30000));   /* unique per frame, above the dedup tolerance only every ~96 frames: the bot runs eps 0, see below */
    r->ly = (int16_t)(((ms / 700) % 2) ? 20000 : -20000);
    r->buttons = (uint16_t)(((ms / 300) % 2) ? 0x1000 : 0);
    r->rt = (uint8_t)(((ms / 500) % 3) * 100);
}

static int run_lk(int argc, char **argv, int host)
{
    lk_config lc;
    memset(&lc, 0, sizeof lc);
    lc.name = arg_str(argc, argv, "--name", host ? "BotHost" : "BotGuest");
    lc.is_bot = 1;   /* the lab's partner: exempt from the mod-version check (a game with R1_VERSION takes it) */
    const char *tag = arg_str(argc, argv, "--exe-tag", "");
    for (int i = 0; i < 8 && strlen(tag) >= (size_t)(2 * i + 2); i++) {
        char b[3] = {tag[2 * i], tag[2 * i + 1], 0};
        lc.exe_tag[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    lk_t *k = lk_new_cfg(NULL, "enet", 1, &lc);
    if (!k) {
        fprintf(stderr, "cannot create the link\n");
        return 1;
    }
    char err[200] = "";
    double secs = arg_num(argc, argv, "--seconds", 8), load_gap = arg_num(argc, argv, "--load-gap", 3);
    int loads = (int)arg_num(argc, argv, "--loads", 1), load_ms = (int)arg_num(argc, argv, "--load-ms", 600), ready_delay = (int)arg_num(argc, argv, "--ready-delay", 300);
    uint32_t area = hexarg(argc, argv, "--area-id", 0x91010100u);
    if (host) {
        int port = (int)arg_num(argc, argv, "--port", 28411);
        const char *role = arg_str(argc, argv, "--role", "lead");
        if (!lk_start(k, L1N_HOST, "", port, NULL, 0, err, sizeof err)) {
            fprintf(stderr, "host failed: %s\n", err);
            return 1;
        }
        lk_set_role(k, !strcmp(role, "partner") ? L1_PARTNER : L1_LEAD);
        lk_settings st;
        memset(&st, 0, sizeof st);
        st.episode = (int)arg_num(argc, argv, "--episode", 1), st.difficulty = (int)arg_num(argc, argv, "--difficulty", 1), st.flags = (int)arg_num(argc, argv, "--flags", 0);
        st.area_id = hexarg(argc, argv, "--area", 0x91010100u), st.seed = hexarg(argc, argv, "--seed", 0);
        lk_set_settings(k, &st);
    } else if (argc < 3 || !lk_start(k, L1N_JOIN, argv[2], 0, NULL, 0, err, sizeof err)) {
        fprintf(stderr, "join failed: %s\n", argc < 3 ? "no address" : err);
        return 1;
    }
    int pad_on = arg_flag(argc, argv, "--pad");
    double hold_at = arg_num(argc, argv, "--pad-hold-at", -1), hold_for = arg_num(argc, argv, "--pad-hold-for", 2), silent_at = arg_num(argc, argv, "--pad-silent-at", -1),
           silent_for = arg_num(argc, argv, "--pad-silent-for", 2);
    PadSink psink;
    memset(&psink, 0, sizeof psink);
    p1_tx btx;
    p1_rx brx;
    {
        p1_tx_cfg tc;
        p1_tx_defaults(&tc);
        tc.stick_eps = 0;
        p1_tx_init(&btx, &tc);
        p1_sink sk = {pad_sink_push, &psink};
        p1_rx_init(&brx, NULL, sk);
    }
    unsigned pad_frame = 0, pad_rx_new = 0;
    DWORD last_pad = 0, play_at = 0;
    double lat_sum = 0, lat_max = 0;
    uint32_t last_seen_t = 0;
    int have_seen_t = 0;
    enum { S_CONNECT, S_LOBBY, S_LOADING, S_PLAY, S_DONE } stage = S_CONNECT;
    DWORD stage_t = GetTickCount(), play_t0 = 0, load_t = 0, last_state = 0, ready_t = 0;
    int exit_code = 0, ready_sent = 0, loads_done = 0, in_load = 0, own_begun = 0, samples = 0;
    unsigned seen_epoch = 0;
    while (stage != S_DONE) {
        lk_poll(k);
        DWORD now = GetTickCount();
        lk_info in;
        lk_get_info(k, &in);
        int st = lk_state(k);
        if (st == L1NS_ERROR) {
            printf("RESULT {\"error\":\"%s\",\"stage\":%d}\n", lk_last_reason(k), (int)stage);
            exit_code = stage == S_CONNECT ? 3 : 4;
            break;
        }
        if (stage == S_CONNECT) {
            if (st == L1NS_CONNECTED) stage = S_LOBBY, stage_t = ready_t = now;
            else if (now - stage_t > 60000) {
                printf("RESULT {\"error\":\"nobody connected\"}\n");
                exit_code = 5;
                break;
            }
        } else if (stage == S_LOBBY) {
            if (!ready_sent && !arg_flag(argc, argv, "--no-ready") && now - ready_t >= (DWORD)ready_delay) lk_set_ready(k, 1), ready_sent = 1;
            if (in.started) stage = S_LOADING, stage_t = now, in_load = 1, own_begun = 0;
            else if (now - stage_t > 60000) {
                printf("RESULT {\"error\":\"no start\"}\n");
                exit_code = 6;
                break;
            }
        } else if (stage == S_LOADING) {
            if (!own_begun) lk_load_begin(k, area), own_begun = 1, load_t = now;
            if (in_load && (int)(now - load_t) >= load_ms) lk_load_done(k), in_load = 0;
            if (!in_load && in.phase == CP_PHASE_PLAYING) {
                stage = S_PLAY, loads_done++;
                if (!play_t0) play_t0 = now;
                seen_epoch = (unsigned)in.epoch;
                stage_t = now;
                if (!play_at) play_at = now;
                p1_tx_restart(&btx);
            } else if (now - load_t > 30000) {
                printf("RESULT {\"error\":\"load barrier timeout\",\"phase\":%d,\"epoch\":%d}\n", in.phase, in.epoch);
                exit_code = 7;
                break;
            }
        } else if (stage == S_PLAY) {
            if (now - last_state >= 50) {
                last_state = now;
                cp_state ps = path_state(cs_pid(lk_session(k)), cs_now(lk_session(k)));
                cs_submit_state(lk_session(k), &ps);
            }
            int rp = cs_remote_pid(lk_session(k));
            cp_state out;
            if (rp >= 0 && cs_remote_sample(lk_session(k), rp, cs_now(lk_session(k)), &out) != CS_SAMPLE_NONE) samples++;
            if (pad_on) {
                DWORD tn = timeGetTime();   /* 1 ms clock (timeBeginPeriod in main): GetTickCount ticks every 15.6 ms */
                double tp = (now - play_at) / 1000.0;
                int silent = silent_at >= 0 && tp >= silent_at && tp < silent_at + silent_for;
                uint16_t pf = (hold_at >= 0 && tp >= hold_at && tp < hold_at + hold_for) ? CP_SF_PAUSED : 0;
                cp_pad pk;
                if (tn - last_pad >= 16) {
                    last_pad = tn;
                    cp_pad_rec cur;
                    make_bot_pad(&cur, ++pad_frame, tn);
                    if (!silent && p1_tx_step(&btx, tn, &cur, pf, &pk)) cs_send_pad(lk_session(k), &pk);
                }
                while (cs_recv_pad(lk_session(k), &pk)) {
                    if ((int)pk.pid != rp) continue;
                    if (!have_seen_t || (int32_t)(pk.t_ms - last_seen_t) > 0) {   /* a packet with a new record: delay = now - the record's stamp */
                        double lat = (double)(int32_t)(tn - pk.t_ms);
                        lat_sum += lat, pad_rx_new++;
                        if (lat > lat_max) lat_max = lat;
                        last_seen_t = pk.t_ms, have_seen_t = 1;
                    }
                    p1_rx_packet(&brx, tn, &pk, (unsigned)in.epoch);
                }
                p1_rx_tick(&brx, tn, (unsigned)in.epoch, 0);
            }
            if (in.phase == CP_PHASE_LOADING && (unsigned)in.epoch != seen_epoch) {   /* the other side loads: this PC loads too */
                stage = S_LOADING, in_load = 1, own_begun = 0, stage_t = now;
            } else if (!host && loads_done < loads && (now - stage_t) / 1000.0 >= load_gap) {   /* a checkpoint load started by the joiner */
                stage = S_LOADING, in_load = 1, own_begun = 0, stage_t = now;
            } else if ((now - play_t0) / 1000.0 >= secs && loads_done >= loads)
                stage = S_DONE;
        }
        Sleep(3);
    }
    if (exit_code == 0) {
        lk_info in;
        lk_get_info(k, &in);
        printf("RESULT {\"mode\":\"lk\",\"role\":\"%s\",\"remote_role\":\"%s\",\"host\":%d,\"started\":%d,\"episode\":%d,\"difficulty\":%d,\"flags\":%d,\"area\":\"%08x\",\"seed\":\"%08x\","
               "\"phase\":%d,\"epoch\":%d,\"go\":%d,\"last_go_epoch\":%d,\"load_begins\":%d,\"loads_done\":%d,\"samples\":%d,\"peer\":\"%s\"}\n",
               in.role == 0 ? "lead" : in.role == 1 ? "partner" : "?", in.remote_role == 0 ? "lead" : in.remote_role == 1 ? "partner" : "-", in.host, in.started, in.set.episode, in.set.difficulty, in.set.flags,
               (unsigned)in.set.area_id, (unsigned)in.set.seed, in.phase, in.epoch, in.gos, in.last_go_epoch, in.load_begins, loads_done, samples, in.peer_name);
    }
    if (exit_code == 0 && pad_on) {
        uint32_t ptx = 0, prx = 0, pdrop = 0;
        cs_pad_stats(lk_session(k), &ptx, &prx, &pdrop);
        printf("PAD {\"tx_packets\":%u,\"tx_records\":%u,\"keepalives\":%u,\"session_tx\":%u,\"session_rx\":%u,\"session_drop\":%u,\"rx_records\":%u,\"lost\":%u,\"late\":%u,\"repeats\":%u,\"wrong_epoch\":%u,"
               "\"pushed\":%u,\"order_bad\":%d,\"neutral\":%d,\"held_dropped\":%u,\"stalls\":%u,\"losts\":%u,\"resumes\":%u,\"lat_mean\":%.1f,\"lat_max\":%.0f,\"lat_n\":%u,\"link\":\"%s\",\"mode\":\"%s\"}\n",
               btx.packets, btx.records, btx.keepalives, ptx, prx, pdrop, brx.st.records, brx.st.lost, brx.st.late, brx.st.keepalive_or_dup, brx.st.wrong_epoch, psink.pushed, psink.bad_order, psink.neutral,
               brx.st.held_dropped, brx.st.stalls, brx.st.losts, brx.st.resumes, pad_rx_new ? lat_sum / pad_rx_new : -1.0, lat_max, pad_rx_new, p1_link_name(brx.link), p1_mode_name(brx.mode));
    }
    fflush(stdout);
    lk_free(k);
    return exit_code;
}

int main(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "host") && strcmp(argv[1], "join"))) {
        fprintf(stderr, "usage: coop_bot host [--port N] ... | coop_bot join <ip:port> ... (see the comment at the top of coop_bot.c)\n");
        return 2;
    }
    int host = !strcmp(argv[1], "host");
    verbose = arg_flag(argc, argv, "--verbose");
    t_start = GetTickCount();
    cn_set_log(logline);
    timeBeginPeriod(1);
    if (arg_flag(argc, argv, "--lk")) return run_lk(argc, argv, host);
    cs_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.name = arg_str(argc, argv, "--name", host ? "BotHost" : "BotGuest");
    cfg.want_char = cp_char_parse(arg_str(argc, argv, "--char", host ? "claire" : "none"));
    cfg.mod_version = "bot";
    cfg.is_bot = 1;
    cfg.on_note = on_note;
    const char *tag = arg_str(argc, argv, "--exe-tag", "");
    for (int i = 0; i < 8 && strlen(tag) >= (size_t)(2 * i + 2); i++) {
        char b[3] = {tag[2 * i], tag[2 * i + 1], 0};
        cfg.exe_tag[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    cs_t *s = cs_new(&cfg);
    char err[200];
    int players = (int)arg_num(argc, argv, "--players", 2);
    double play_secs = arg_num(argc, argv, "--seconds", 12);
    int load_ms = (int)arg_num(argc, argv, "--load-ms", 800);
    double load_at = arg_num(argc, argv, "--load-at", -1), event_at = arg_num(argc, argv, "--event-at", -1), leave_at = arg_num(argc, argv, "--leave-at", -1), die_at = arg_num(argc, argv, "--die-at", -1);
    int start_delay = (int)arg_num(argc, argv, "--autostart-delay", 300);
    int want_pick = !host && cfg.want_char != CP_CHAR_NONE;
    if (host) {
        int port = (int)arg_num(argc, argv, "--port", 28411);
        if (!cs_host(s, "enet", port, 1, err, sizeof err)) {
            fprintf(stderr, "host failed: %s\n", err);
            return 1;
        }
        cp_session ss;
        cs_session_get(s, &ss);
        ss.max_players = (uint8_t)players;
        ss.episode = (uint8_t)arg_num(argc, argv, "--episode", 1);
        cs_set_session(s, &ss);
    } else {
        if (argc < 3 || !cs_join(s, "enet", argv[2], 1, err, sizeof err)) {
            fprintf(stderr, "join failed: %s\n", argc < 3 ? "no address" : err);
            return 1;
        }
    }

    enum { W_CONNECT, W_LOBBY, W_LOADING, W_PLAYING, W_DONE } stage = W_CONNECT;
    DWORD stage_t = GetTickCount(), play_t0 = 0, last_state = 0, last_sample = 0, last_status = 0, ready_since = 0;
    int load_done_sent = 0, event_sent = 0, left_early = 0, mid_load_started = 0, in_mid_load = 0;
    DWORD mid_load_t = 0;
    double measure_from = 2.0;
    double err_sum = 0, err_max = 0;
    long err_n = 0, none_n = 0, stale_n = 0, extrap_n = 0;
    int ready_sent = 0, exit_code = 0;
    unsigned max_epoch = 0;

    while (stage != W_DONE) {
        cs_poll(s);
        DWORD now = GetTickCount();
        if (notes.refused) {
            printf("RESULT {\"refused\":%d,\"code\":%d,\"text\":\"%s\"}\n", notes.refused, notes.refuse_code, notes.refuse_text);
            exit_code = 3;
            break;
        }
        if (notes.lost && stage != W_CONNECT) {
            printf("RESULT {\"lost\":1,\"stage\":%d}\n", (int)stage);
            exit_code = 4;
            break;
        }
        if (verbose && now - last_status > 2000) {
            last_status = now;
            char st[1500];
            cs_status(s, st, sizeof st);
            logline(st);
        }
        if (cs_epoch(s) > max_epoch) max_epoch = cs_epoch(s);
        switch (stage) {
        case W_CONNECT:
            if (host ? cs_player_count(s) >= players : notes.connected) stage = W_LOBBY, stage_t = now;
            else if (now - stage_t > 60000) { printf("RESULT {\"error\":\"nobody connected\"}\n"); exit_code = 5; stage = W_DONE; }
            break;
        case W_LOBBY:
            if (!host && want_pick && cs_local_character(s) != cfg.want_char) {
                if (now - stage_t > 200) { cs_pick(s, cfg.want_char); stage_t = now; }
            } else if (!ready_sent && !arg_flag(argc, argv, "--no-ready")) {
                cs_ready(s, 1);
                ready_sent = 1;
                ready_since = now;
            }
            if (host && ready_sent && now - ready_since > (DWORD)start_delay) {
                if (cs_start(s, 0, err, sizeof err)) logline("start sent");
            }
            if (notes.started) stage = W_LOADING, stage_t = now;
            break;
        case W_LOADING:
            if (!load_done_sent && (int)(now - stage_t) >= load_ms) {
                cs_load_done(s);
                load_done_sent = 1;
            }
            if (in_mid_load && (int)(now - mid_load_t) >= load_ms) {
                cs_load_done(s);
                in_mid_load = 0;
            }
            if (cs_phase(s) == CP_PHASE_PLAYING) {
                stage = W_PLAYING;
                if (!play_t0) play_t0 = now;
                measure_from = (now - play_t0) / 1000.0 + 2.0;
            }
            break;
        case W_PLAYING: {
            double t = (now - play_t0) / 1000.0;
            if (now - last_state >= 50) {
                last_state = now;
                cp_state st = path_state(cs_pid(s), cs_now(s));
                cs_submit_state(s, &st);
            }
            if (now - last_sample >= 16) {
                last_sample = now;
                int rp = cs_remote_pid(s);
                cp_state out;
                if (rp >= 0) {
                    int r = cs_remote_sample(s, rp, cs_now(s), &out);
                    if (r == CS_SAMPLE_NONE) none_n++;
                    else {
                        if (r == CS_SAMPLE_STALE) stale_n++;
                        if (r == CS_SAMPLE_EXTRAP) extrap_n++;
                        if (t > measure_from && r != CS_SAMPLE_STALE) {
                            cp_state truth = path_state(rp, out.t_ms);
                            double dx = out.pos[0] - truth.pos[0], dz = out.pos[2] - truth.pos[2], e = sqrt(dx * dx + dz * dz);
                            err_sum += e, err_n++;
                            if (e > err_max) err_max = e;
                        }
                    }
                }
            }
            if (load_at >= 0 && !mid_load_started && t >= load_at) {   /* this bot's "game" starts loading the next level */
                mid_load_started = in_mid_load = 1;
                mid_load_t = now;
                cs_load_begin(s, 777);
                logline("mid-game load begins");
                stage = W_LOADING, stage_t = now;
                break;
            }
            if (cs_phase(s) == CP_PHASE_LOADING && !in_mid_load) {   /* somebody else started a load: this bot's game loads too */
                in_mid_load = 1;
                mid_load_t = now;
                cs_load_begin(s, 777);
                stage = W_LOADING, stage_t = now;
                break;
            }
            if (event_at >= 0 && !event_sent && t >= event_at) {
                const char body[] = {9, 8, 7, 6};
                cs_send_event(s, CP_EV_PICKUP, body, sizeof body);
                event_sent = 1;
            }
            if (leave_at >= 0 && t >= leave_at) {
                left_early = 1;
                stage = W_DONE;
            }
            if (die_at >= 0 && t >= die_at) ExitProcess(0);   /* no goodbye at all: the peers must notice by themselves */
            if (t >= play_secs) stage = W_DONE;
            break;
        }
        default:
            break;
        }
        Sleep(3);
    }

    if (exit_code == 0) {
        cs_stats st;
        cs_get_stats(s, &st);
        cs_player_info pi = {0};
        int rp = cs_remote_pid(s);
        if (rp >= 0) cs_player(s, rp, &pi);
        printf("RESULT {\"role\":\"%s\",\"pid\":%d,\"character\":\"%s\",\"remote_character\":\"%s\",\"phase\":%d,\"epoch\":%u,\"max_epoch\":%u,\"go\":%d,\"started\":%d,"
               "\"tx_state\":%u,\"rx_state\":%u,\"dup\":%u,\"old\":%u,\"gap\":%u,\"stale_epoch\":%u,\"err_mean\":%.3f,\"err_max\":%.3f,\"err_n\":%ld,\"none\":%ld,\"stale\":%ld,\"extrap\":%ld,"
               "\"events\":%d,\"rtt\":%d,\"left_early\":%d,\"bad\":%u,\"hb\":%u}\n",
               host ? "host" : "client", cs_pid(s), cp_char_name((unsigned)cs_local_character(s)), rp >= 0 ? cp_char_name(pi.character) : "none", cs_phase(s), cs_epoch(s), max_epoch,
               notes.go_count, notes.started, st.tx_state, st.rx_state, st.rx_dup, st.rx_old, st.rx_gap, st.rx_stale_epoch, err_n ? err_sum / (double)err_n : -1.0, err_max, err_n, none_n, stale_n,
               extrap_n, notes.events, pi.rtt_ms, left_early, st.rx_bad, st.hb_rx);
    }
    fflush(stdout);
    cs_free(s);
    return exit_code;
}
