/*
 * Steam backend of the co-op transport (cn_ops "steam"), the lobby/command layer and the hooks the proxy calls.
 * Platform independent: every Steam call goes through cst_plat (coop_steam.h); coop_steam_real.c holds the real one
 * (the game's own Steam interfaces, 32-bit process only), tests/steam_mock.c a mock universe.
 *
 * Flow
 *   host   CreateLobby (type friends-only unless told otherwise, limit = players), lobby data with OUR tag (r1c = R1COOP1, never a Raid key),
 *          rich presence `connect` = "+connect_lobby <id>" (the friends list shows "Join game"), then every lobby member that passes the policy
 *          (friend, or on the allow list) gets a link handshake on the message pipe; peers are numbered 1..n.
 *   client JoinLobby (from a lobby id, or from a friend's SteamID: the friend's game info lobby, else a lobby search for r1c_host = id),
 *          check the tag, then the same handshake with the lobby owner = peer 0.
 *   wire   every pipe message starts with a type byte: H hello / W welcome (7 bytes: version, lobby id) open the link, D data (channel byte +
 *          the session's message), P ping / Q pong (RTT and keep-alive), B bye. Reliable and unreliable map to the pipe's send types.
 *   down   B from the peer, the member gone from the lobby for 3 s, the pipe session failed for 5 s, nothing heard for 30 s.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_proto.h"
#include "coop_steam.h"

#define APP_ID 222480u   /* Resident Evil Revelations (Rev1). lm6 live: the port had kept Rev2's 287290, so a friend in Rev1 was never found */
#define MSG_MAX 65536
#define HELLO_EVERY_MS 500
#define GREET_GIVE_UP_MS 20000
#define MEMBER_REFRESH_MS 250
#define MEMBER_GONE_MS 3000
#define PING_EVERY_MS 1000
#define SILENT_MS 30000
#define PIPE_FAIL_MS 5000
#define CLOSE_LINGER_MS 150
#define CLIENT_TOTAL_MS 25000
#define SEARCH_MS 9000
#define PROTO 1

/* ---------------------------------------------------------------------------------------- global lock */
static CRITICAL_SECTION g_lock;
static volatile LONG g_lock_state;
static void lock_ensure(void)
{
    if (g_lock_state == 2) return;
    if (InterlockedCompareExchange(&g_lock_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        g_lock_state = 2;
    } else
        while (g_lock_state != 2) Sleep(0);
}
static void LOCK(void) { lock_ensure(), EnterCriticalSection(&g_lock); }
static void UNLOCK(void) { LeaveCriticalSection(&g_lock); }
static int TRYLOCK(void) { lock_ensure(); return TryEnterCriticalSection(&g_lock); }

/* ---------------------------------------------------------------------------------------- small helpers */
static char g_last_err[200];
static int g_trace;                 /* net steamtrace / REV2_STEAM_TRACE=1: log pipe messages (all but the first 20 data messages each way) */
static int g_trace_n, g_trace_rx;
const char *cst_last_error(void) { return g_last_err; }
static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_err, sizeof g_last_err, fmt, ap);
    va_end(ap);
    cn_log("steam: %s", g_last_err);
}

#define ULL(x) ((unsigned long long)(x))
static int id_type(uint64_t id) { return (int)((id >> 52) & 0xF); }          /* 1 individual, 8 chat (lobby) */
static int is_user_id(uint64_t id) { return id_type(id) == 1; }
static int is_lobby_id(uint64_t id) { return id_type(id) == 8; }

static int parse_u64(const char *s, uint64_t *out)
{
    while (*s == ' ') s++;
    if (!*s) return 0;
    char *end = NULL;
    unsigned long long v = _strtoui64(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
    if (end == s || (*end && *end != ' ')) return 0;
    *out = v;
    return v != 0;
}

/* "lobby:<n>" | "steam:<n>" | "+connect_lobby <n>" | "<n>" : a SteamID64 whose type bits say lobby or user */
static int parse_target(const char *t, uint64_t *id)
{
    while (*t == ' ') t++;
    const char *k = strstr(t, "+connect_lobby");
    if (k) return parse_u64(k + 14, id) && is_lobby_id(*id);
    if (!strncmp(t, "lobby:", 6)) t += 6;
    else if (!strncmp(t, "steam:", 6)) t += 6;
    if (!parse_u64(t, id)) return 0;
    return is_lobby_id(*id) || is_user_id(*id);
}

static void ascii_name(const char *in, char *out, size_t size)   /* lobby data / logs: printable ASCII only */
{
    size_t o = 0;
    for (; in && *in && o + 1 < size; in++) {
        unsigned char c = (unsigned char)*in;
        out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------------------------------- options (set by the commands, read by host()) */
static int g_opt_type = CST_LOBBY_FRIENDS, g_opt_max = 4;
void cst_set_host_options(int lobby_type, int max_players)
{
    g_opt_type = lobby_type < 0 || lobby_type > 2 ? CST_LOBBY_FRIENDS : lobby_type;
    g_opt_max = max_players < 2 ? 2 : max_players > CP_MAX_PLAYERS ? CP_MAX_PLAYERS : max_players;
}
static int g_threaded = 1;
void cst_set_threaded(int on) { g_threaded = on != 0; }
static int g_policy;
static uint64_t g_allow[8];
static int g_n_allow;
void cst_set_policy(int policy) { g_policy = policy < 0 || policy > 2 ? 0 : policy; }
void cst_allow(uint64_t id)
{
    if (!id || g_n_allow >= 8) return;
    for (int i = 0; i < g_n_allow; i++)
        if (g_allow[i] == id) return;
    g_allow[g_n_allow++] = id;
}
static int in_allow(uint64_t id)
{
    for (int i = 0; i < g_n_allow; i++)
        if (g_allow[i] == id) return 1;
    return 0;
}

/* ---------------------------------------------------------------------------------------- platform selection, hubs */
static const cst_plat *g_plat;
static void *g_pctx;
static int (*g_run_line)(const char *line, char *out, int size);

void cst_set_platform(const cst_plat *p, void *ctx) { g_plat = p, g_pctx = ctx; }
typedef struct cst_hub {
    const cst_plat *p;
    void *c;
    int state;                     /* 0 new, 1 ready, 2 failed */
    DWORD t_try;
    char err[160];
    uint64_t me;
    char my_name[64];
    volatile uint64_t my_lobby;    /* the lobby this process hosts or has joined (the swallow filter reads it from the game thread) */
    /* results of asynchronous calls */
    int create_done, create_res;
    uint64_t create_lobby;
    int enter_done, enter_res;
    uint64_t enter_lobby;
    int list_done, list_n;
    /* join requests, session requests, failures */
    uint64_t pend_lobby, pend_friend;
    uint64_t req[8];
    DWORD req_t[8];
    int n_req;
    uint64_t failed[8];
    DWORD failed_t[8];
    int dirty;
} cst_hub;

static cst_hub g_hubs[8];
static int g_n_hubs;

static cst_hub *hub_get(const cst_plat *p, void *c, int create)
{
    for (int i = 0; i < g_n_hubs; i++)
        if (g_hubs[i].p == p && g_hubs[i].c == c) return &g_hubs[i];
    if (!create || g_n_hubs >= 8) return NULL;
    cst_hub *h = &g_hubs[g_n_hubs++];
    memset(h, 0, sizeof *h);
    h->p = p, h->c = c;
    return h;
}
static cst_hub *default_hub(int create)
{
    const cst_plat *p = g_plat ? g_plat : cst_real_platform();
    if (!p) return NULL;
    return hub_get(p, g_plat ? g_pctx : NULL, create);
}

static int g_cb_wanted;   /* the callback objects were asked for (once per process, at the first ready hub of the game tick) */

void cst_reset(void)
{
    LOCK();
    memset(g_hubs, 0, sizeof g_hubs);
    g_n_hubs = 0;
    g_plat = NULL, g_pctx = NULL;
    g_policy = 0, g_n_allow = 0, g_opt_type = CST_LOBBY_FRIENDS, g_opt_max = 4, g_threaded = 1;
    g_last_err[0] = 0;
    g_cb_wanted = 0;
    UNLOCK();
}
void cst_set_runner(int (*run)(const char *line, char *out, int size)) { g_run_line = run; }

/* 1 = ready; err gets the reason otherwise */
static int hub_ready(cst_hub *h, char *err, size_t esz)
{
    if (h->state == 1) return 1;
    if (h->state == 2 && GetTickCount() - h->t_try < 3000) {
        snprintf(err, esz, "%s", h->err);
        return 0;
    }
    char e[160] = "";
    h->t_try = GetTickCount();
    if (h->p->init(h->c, e, sizeof e)) {
        h->state = 1;
        h->me = h->p->my_id(h->c);
        ascii_name(h->p->my_name(h->c), h->my_name, sizeof h->my_name);
        cn_log("steam: ready (%s), you are %llu \"%s\", pipe %s", h->p->name, ULL(h->me), h->my_name, h->p->pipe_name(h->c));
        return 1;
    }
    h->state = 2;
    snprintf(h->err, sizeof h->err, "%s", e[0] ? e : "Steam is not available");
    snprintf(err, esz, "%s", h->err);
    return 0;
}

static int parse_connect_lobby(const char *s, uint64_t *id)
{
    const char *k = strstr(s, "+connect_lobby");
    return k && parse_u64(k + 14, id) && is_lobby_id(*id);
}

static void hub_pump(cst_hub *h)
{
    cst_event ev;
    for (int guard = 0; guard < 128 && h->p->poll(h->c, &ev); guard++) {
        switch (ev.type) {
        case CST_EV_LOBBY_CREATED:
            h->create_done = 1, h->create_res = ev.result, h->create_lobby = ev.id;
            break;
        case CST_EV_LOBBY_ENTER:
            h->enter_done = 1, h->enter_res = ev.result, h->enter_lobby = ev.id;
            break;
        case CST_EV_LOBBY_LIST:
            h->list_done = 1, h->list_n = ev.n;
            break;
        case CST_EV_LOBBY_CHANGE:
            h->dirty = 1;
            break;
        case CST_EV_JOIN_LOBBY:
            if (ev.id && ev.id != h->my_lobby) h->pend_lobby = ev.id, h->pend_friend = ev.id2;
            cn_log("steam: join request (invite / Join game) for lobby %llu from %llu", ULL(ev.id), ULL(ev.id2));
            break;
        case CST_EV_JOIN_RICH: {
            uint64_t l = 0;
            ev.text[sizeof ev.text - 1] = 0;
            cn_log("steam: Join game request from %llu, connect string \"%.80s\"", ULL(ev.id2), ev.text);
            if (parse_connect_lobby(ev.text, &l) && l != h->my_lobby) h->pend_lobby = l, h->pend_friend = ev.id2;
            break;
        }
        case CST_EV_SESSION_REQUEST: {
            int have = 0;
            for (int i = 0; i < h->n_req; i++) have |= h->req[i] == ev.id2;
            if (!have && h->n_req < 8) h->req_t[h->n_req] = GetTickCount(), h->req[h->n_req++] = ev.id2;
            break;
        }
        case CST_EV_SESSION_FAILED:
            for (int i = 0; i < 8; i++)
                if (!h->failed[i] || h->failed[i] == ev.id2) {
                    h->failed[i] = ev.id2, h->failed_t[i] = GetTickCount();
                    break;
                }
            break;
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------------------------------- backend */
enum { PS_FREE = 0, PS_GREET, PS_LINKED, PS_CLOSING, PS_BYE };
enum { S_HOST_UP = 1, S_C_RESOLVE, S_C_SEARCH, S_C_JOIN, S_C_LOBBY, S_C_LINKED, S_DEAD };

typedef struct {
    uint64_t id;
    int state;
    int quiet;                 /* CLOSING: no DISCONNECT event (cn already reported it) */
    uint32_t t_h, t_rx, t_ping, t_seen, t_close, first_greet, t_bad;
    int rtt;
} cst_peer;

typedef struct cst_st {
    cst_hub *h;
    const cst_plat *p;
    void *c;
    int host, max_peers, stage, lobby_type;
    uint64_t lobby, owner, target;
    uint32_t t0, t_stage, t_members, t_check, t_ping_round, t_owner_bad, t_friend;
    cst_peer peer[CN_MAX_PEERS];
    cn_event q[64];
    int nq;
    uint8_t *rx;
    uint64_t ignored[8];
    int n_ignored;
    uint32_t sent_fail;
    char why[200];              /* client: why the join failed for good (reported through cn_ops.failed) */
    int in_lobby;               /* client: JoinLobby succeeded (LeaveLobby is due on close) */
} cst_st;

static void qpush(cst_st *s, int type, int peer, int channel, uint8_t *data, size_t len)
{
    if (s->nq >= 64) { free(data); return; }
    cn_event *e = &s->q[s->nq++];
    memset(e, 0, sizeof *e);
    e->type = type, e->peer = peer, e->channel = channel, e->data = data, e->len = len;
}

static const char *pname(cst_st *s, uint64_t id)
{
    static char b[4][64];
    static int k;
    char *o = b[k++ & 3];
    ascii_name(s->p->name_of(s->c, id), o, 64);
    return o;
}

static int peer_index(cst_st *s, uint64_t id)
{
    if (!id) return -1;
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (s->peer[i].state != PS_FREE && s->peer[i].id == id) return i;
    return -1;
}

static int pipe_put(cst_st *s, uint64_t to, int reliable, const void *a, size_t an, const void *b, size_t bn)
{
    uint8_t stack_buf[256];
    uint8_t *buf = an + bn <= sizeof stack_buf ? stack_buf : (uint8_t *)malloc(an + bn);
    if (!buf) return 0;
    memcpy(buf, a, an);
    if (bn) memcpy(buf + an, b, bn);
    int ok = s->p->pipe_send(s->c, to, buf, an + bn, reliable);
    if (g_trace && (buf[0] != 'D' || g_trace_n++ < 20)) cn_log("steam: pipe send '%c' %u bytes to %llu %s -> %d", buf[0] >= 32 ? buf[0] : '?', (unsigned)(an + bn), ULL(to), reliable ? "reliable" : "unreliable", ok);
    if (buf != stack_buf) free(buf);
    if (!ok && s->sent_fail++ < 5) cn_log("steam: send to %llu refused by the pipe", ULL(to));
    return ok;
}

static void send_hello(cst_st *s, uint64_t to, char kind)
{
    uint8_t m[12];
    m[0] = (uint8_t)kind, m[1] = PROTO;
    memcpy(m + 2, &s->lobby, 8);
    m[10] = (uint8_t)(s->host ? 1 : 0), m[11] = 0;
    pipe_put(s, to, 1, m, sizeof m, NULL, 0);
}

static void send_bye(cst_st *s, uint64_t to)
{
    uint8_t m = 'B';
    pipe_put(s, to, 1, &m, 1, NULL, 0);
}

static void link_peer(cst_st *s, int idx, uint32_t now)
{
    cst_peer *pe = &s->peer[idx];
    pe->state = PS_LINKED, pe->t_rx = now, pe->t_ping = now - PING_EVERY_MS, pe->rtt = -1, pe->t_bad = 0;
    cn_log("steam: linked with %llu \"%s\" (peer %d, pipe %s)", ULL(pe->id), pname(s, pe->id), idx, s->p->pipe_name(s->c));
    qpush(s, CN_EV_CONNECT, idx, 0, NULL, 0);
    if (!s->host) s->stage = S_C_LINKED;
}

/* idx leaves: emit DISCONNECT when it was linked, close the pipe session, forget it */
static void drop_peer(cst_st *s, int idx, const char *why, int to_bye)
{
    cst_peer *pe = &s->peer[idx];
    if (pe->state == PS_FREE) return;
    cn_log("steam: peer %d (%llu) gone: %s", idx, ULL(pe->id), why);
    if (pe->state == PS_LINKED) qpush(s, CN_EV_DISCONNECT, idx, 0, NULL, 0);
    s->p->pipe_close(s->c, pe->id);
    if (to_bye) {
        pe->state = PS_BYE;   /* still a lobby member: park it, only its own hello revives it */
        pe->t_seen = s->p->now(s->c);
    } else
        memset(pe, 0, sizeof *pe);
}

static int policy_allows(cst_st *s, uint64_t id)
{
    if (g_policy == 1) return 1;
    if (in_allow(id)) return 1;
    if (g_policy == 2) return 0;
    return s->p->is_friend(s->c, id);
}

static void host_presence(cst_st *s)
{
    if (!s->lobby) return;
    int n = s->p->lobby_count(s->c, s->lobby);
    char v[64];
    snprintf(v, sizeof v, "+connect_lobby %llu", ULL(s->lobby));
    if (!s->p->presence(s->c, "connect", v)) cn_log("steam: SetRichPresence(connect) refused: friends will not see \"Join game\"");
    snprintf(v, sizeof v, "Resident Evil Revelations co-op %d/%d", n, s->max_peers + 1);
    s->p->presence(s->c, "status", v);
    snprintf(v, sizeof v, "%llu", ULL(s->lobby));
    s->p->presence(s->c, "steam_player_group", v);
    snprintf(v, sizeof v, "%d", n);
    s->p->presence(s->c, "steam_player_group_size", v);
}

static void host_lobby_data(cst_st *s)
{
    char v[64];
    int bad = 0;
    bad += !s->p->lobby_set(s->c, s->lobby, CST_TAG_KEY, CST_TAG_VAL);
    snprintf(v, sizeof v, "%llu", ULL(s->h->me));
    bad += !s->p->lobby_set(s->c, s->lobby, CST_KEY_HOST, v);
    bad += !s->p->lobby_set(s->c, s->lobby, CST_KEY_NAME, s->h->my_name);
    snprintf(v, sizeof v, "%d", s->max_peers + 1);
    bad += !s->p->lobby_set(s->c, s->lobby, CST_KEY_MAX, v);
    bad += !s->p->lobby_set(s->c, s->lobby, CST_KEY_PHASE, "lobby");
    bad += !s->p->lobby_set(s->c, s->lobby, CST_KEY_BUILD, "rev2-coop");
    if (bad) cn_log("steam: SetLobbyData refused %d of 6 keys (only the lobby owner may set them)", bad);
}

static int st_host(void **stp, int port, int max_peers, char *err, size_t esz)
{
    (void)port;
    cst_hub *h = default_hub(1);
    if (!h) { snprintf(err, esz, "the Steam transport needs the game process (32-bit) or a test platform"); return 0; }
    LOCK();
    if (!hub_ready(h, err, esz)) { UNLOCK(); return 0; }
    cst_st *s = (cst_st *)calloc(1, sizeof *s);
    s->rx = (uint8_t *)malloc(MSG_MAX);
    s->h = h, s->p = h->p, s->c = h->c, s->host = 1;
    s->max_peers = max_peers < 1 ? 1 : max_peers > CN_MAX_PEERS - 1 ? CN_MAX_PEERS - 1 : max_peers;
    if (g_opt_max - 1 < s->max_peers) s->max_peers = g_opt_max - 1;
    s->lobby_type = g_opt_type;
    s->t0 = s->p->now(s->c);
    for (int i = 0; i < CN_MAX_PEERS; i++) s->peer[i].rtt = -1;
    h->create_done = 0;
    g_last_err[0] = 0;
    if (!s->p->lobby_create(s->c, s->lobby_type == CST_LOBBY_PUBLIC ? 2 : s->lobby_type == CST_LOBBY_PRIVATE ? 0 : 1, s->max_peers + 1)) {
        snprintf(err, esz, "Steam refused CreateLobby");
        free(s->rx), free(s);
        UNLOCK();
        return 0;
    }
    DWORD t = GetTickCount();
    for (;;) {   /* the result arrives in a few hundred ms; wait for it here so the console can say whether hosting works */
        hub_pump(h);
        if (h->create_done || GetTickCount() - t > 8000) break;
        UNLOCK();
        Sleep(5);
        LOCK();
    }
    if (!h->create_done || h->create_res != 1) {
        snprintf(err, esz, h->create_done ? "Steam lobby not created (result %d)" : "Steam lobby not created (no answer in 8 s)", h->create_res);
        set_err("%s", err);
        free(s->rx), free(s);
        UNLOCK();
        return 0;
    }
    s->lobby = h->create_lobby, s->owner = h->me;
    h->my_lobby = s->lobby;
    host_lobby_data(s);
    host_presence(s);
    s->stage = S_HOST_UP;
    cn_log("steam: hosting lobby %llu (%s, up to %d players); friends see \"Join game\"", ULL(s->lobby),
           s->lobby_type == CST_LOBBY_PUBLIC ? "public" : s->lobby_type == CST_LOBBY_PRIVATE ? "private" : "friends only", s->max_peers + 1);
    *stp = s;
    UNLOCK();
    return 1;
}

static int st_join(void **stp, const char *target, char *err, size_t esz)
{
    uint64_t id = 0;
    if (!target || !parse_target(target, &id)) {
        snprintf(err, esz, "bad Steam target '%s' (use a lobby id, lobby:<id>, steam:<steamid64> or a friend's SteamID64)", target ? target : "");
        return 0;
    }
    cst_hub *h = default_hub(1);
    if (!h) { snprintf(err, esz, "the Steam transport needs the game process (32-bit) or a test platform"); return 0; }
    LOCK();
    if (!hub_ready(h, err, esz)) { UNLOCK(); return 0; }
    if (id == h->me) { snprintf(err, esz, "that is your own SteamID"); UNLOCK(); return 0; }
    cst_st *s = (cst_st *)calloc(1, sizeof *s);
    s->rx = (uint8_t *)malloc(MSG_MAX);
    s->h = h, s->p = h->p, s->c = h->c, s->host = 0;
    s->t0 = s->t_stage = s->p->now(s->c);
    for (int i = 0; i < CN_MAX_PEERS; i++) s->peer[i].rtt = -1;
    g_last_err[0] = 0;
    if (is_lobby_id(id)) {
        s->lobby = id;
        s->stage = S_C_JOIN;
        h->enter_done = 0;
        if (!s->p->lobby_join(s->c, id)) { snprintf(err, esz, "Steam refused JoinLobby"); free(s->rx), free(s); UNLOCK(); return 0; }
        cn_log("steam: joining lobby %llu", ULL(id));
    } else {
        s->target = id;
        s->stage = S_C_RESOLVE;
        h->list_done = 0;
        cn_log("steam: looking for the lobby of %llu \"%s\"", ULL(id), s->p->name_of(s->c, id));
    }
    *stp = s;
    UNLOCK();
    return 1;
}

static void st_send(void *sp, int peer, int channel, int reliable, const void *data, size_t len)
{
    cst_st *s = (cst_st *)sp;
    if (peer < 0 || peer >= CN_MAX_PEERS) return;
    LOCK();
    if (s->peer[peer].state == PS_LINKED) {
        uint8_t hdr[2] = {'D', (uint8_t)channel};
        pipe_put(s, s->peer[peer].id, reliable, hdr, 2, data, len);
    }
    UNLOCK();
}

static void client_fail(cst_st *s, const char *why)
{
    if (s->stage == S_DEAD) return;
    set_err("join failed: %s", why);
    snprintf(s->why, sizeof s->why, "Steam: %s", why);
    if (s->peer[0].state == PS_LINKED) qpush(s, CN_EV_DISCONNECT, 0, 0, NULL, 0);
    if (s->peer[0].state != PS_FREE) s->p->pipe_close(s->c, s->peer[0].id);
    memset(&s->peer[0], 0, sizeof s->peer[0]);
    s->peer[0].rtt = -1;
    if (s->lobby && s->in_lobby) s->p->lobby_leave(s->c, s->lobby);
    s->in_lobby = 0;
    if (s->h->my_lobby == s->lobby) s->h->my_lobby = 0;
    s->stage = S_DEAD;
}

/* ---- client: from the target to a lobby ---- */
static int tag_ok(const cst_plat *p, void *c, uint64_t lobby)
{
    char v[32] = "";
    p->lobby_get(c, lobby, CST_TAG_KEY, v, sizeof v);
    return !strcmp(v, CST_TAG_VAL);
}
static int lobby_tag_ok(cst_st *s, uint64_t lobby) { return tag_ok(s->p, s->c, lobby); }

static void client_resolve(cst_st *s, uint32_t now)
{
    cst_hub *h = s->h;
    /* a friend who is in the game with a lobby */
    if ((int32_t)(now - s->t_friend) >= 400 || !s->t_friend) {
        s->t_friend = now ? now : 1;
        uint32_t app = 0;
        uint64_t lobby = 0;
        if (s->p->friend_game(s->c, s->target, &app, &lobby) && lobby && (app & 0xFFFFFF) == APP_ID) {
            cn_log("steam: friend %llu is in lobby %llu (game info)", ULL(s->target), ULL(lobby));
            s->lobby = lobby, s->stage = S_C_JOIN, h->enter_done = 0;
            if (!s->p->lobby_join(s->c, lobby)) client_fail(s, "Steam refused JoinLobby");
            return;
        }
    }
    if (s->stage == S_C_RESOLVE && (int32_t)(now - s->t_stage) >= 1200) {   /* not found by presence: ask the public list for r1c_host */
        char v[32];
        snprintf(v, sizeof v, "%llu", ULL(s->target));
        h->list_done = 0;
        s->p->lobby_search(s->c, CST_TAG_KEY, CST_TAG_VAL, CST_KEY_HOST, v, 20);
        s->stage = S_C_SEARCH, s->t_stage = now;
    }
}

static void client_search(cst_st *s, uint32_t now)
{
    cst_hub *h = s->h;
    uint32_t app = 0;
    uint64_t lobby = 0;
    if ((int32_t)(now - s->t_friend) >= 400) {   /* presence may arrive late */
        s->t_friend = now;
        if (s->p->friend_game(s->c, s->target, &app, &lobby) && lobby && (app & 0xFFFFFF) == APP_ID) {
            s->lobby = lobby, s->stage = S_C_JOIN, h->enter_done = 0;
            if (!s->p->lobby_join(s->c, lobby)) client_fail(s, "Steam refused JoinLobby");
            return;
        }
    }
    if (h->list_done) {
        for (int i = 0; i < h->list_n && i < 20; i++) {
            uint64_t l = s->p->lobby_at(s->c, i);
            char v[40] = "";
            s->p->lobby_get(s->c, l, CST_KEY_HOST, v, sizeof v);
            if (l && lobby_tag_ok(s, l) && strtoull(v, NULL, 10) == s->target) {
                cn_log("steam: found public lobby %llu of %llu", ULL(l), ULL(s->target));
                s->lobby = l, s->stage = S_C_JOIN, h->enter_done = 0;
                if (!s->p->lobby_join(s->c, l)) client_fail(s, "Steam refused JoinLobby");
                return;
            }
        }
        h->list_done = 0;   /* nothing yet: ask again */
        if ((int32_t)(now - s->t_stage) < SEARCH_MS) {
            char v[32];
            snprintf(v, sizeof v, "%llu", ULL(s->target));
            s->p->lobby_search(s->c, CST_TAG_KEY, CST_TAG_VAL, CST_KEY_HOST, v, 20);
        }
    }
    if ((int32_t)(now - s->t_stage) > SEARCH_MS) client_fail(s, "no co-op lobby of that player was found (is the host running a Revelations co-op session? friends only needs a Steam friend)");
}

static void client_joining(cst_st *s, uint32_t now)
{
    cst_hub *h = s->h;
    if (!h->enter_done) return;
    if (h->enter_lobby != s->lobby) { h->enter_done = 0; return; }   /* the answer of an older call */
    if (h->enter_res != 1) {
        char m[120];
        snprintf(m, sizeof m, "Steam would not let us into the lobby (response %d%s)", h->enter_res,
                 h->enter_res == 2 ? ": it does not exist any more" : h->enter_res == 4 ? ": full" : h->enter_res == 3 ? ": not allowed (is it friends-only and you are not friends?)" : "");
        h->enter_done = 0;
        s->stage = S_DEAD;   /* nothing joined: no LeaveLobby */
        set_err("join failed: %s", m);
        snprintf(s->why, sizeof s->why, "Steam: %s", m);
        return;
    }
    h->enter_done = 0;
    s->in_lobby = 1;
    if (!lobby_tag_ok(s, s->lobby)) {
        s->p->lobby_leave(s->c, s->lobby);
        s->in_lobby = 0;
        s->stage = S_DEAD;
        set_err("join failed: that lobby is not a Resident Evil Revelations co-op lobby (no %s tag)", CST_TAG_KEY);
        snprintf(s->why, sizeof s->why, "Steam: that lobby is not a Resident Evil Revelations co-op lobby");
        return;
    }
    s->owner = s->p->lobby_owner(s->c, s->lobby);
    if (!s->owner || s->owner == h->me) {
        s->p->lobby_leave(s->c, s->lobby);
        s->in_lobby = 0;
        s->stage = S_DEAD;
        set_err("join failed: the lobby has no other owner");
        snprintf(s->why, sizeof s->why, "Steam: the lobby has no other owner");
        return;
    }
    h->my_lobby = s->lobby;
    cst_peer *pe = &s->peer[0];
    memset(pe, 0, sizeof *pe);
    pe->id = s->owner, pe->state = PS_GREET, pe->first_greet = now, pe->t_h = now - HELLO_EVERY_MS, pe->rtt = -1, pe->t_seen = now;
    s->stage = S_C_LOBBY;
    cn_log("steam: in lobby %llu, host %llu \"%s\"", ULL(s->lobby), ULL(s->owner), pname(s, s->owner));
}

/* ---- host: lobby members -> peers ---- */
static void host_members(cst_st *s, uint32_t now)
{
    int n = s->p->lobby_count(s->c, s->lobby);
    for (int i = 0; i < n; i++) {
        uint64_t id = s->p->lobby_member(s->c, s->lobby, i);
        if (!id || id == s->h->me) continue;
        int idx = peer_index(s, id);
        if (idx < 0) {
            if (!policy_allows(s, id)) {
                int seen = 0;
                for (int k = 0; k < s->n_ignored; k++) seen |= s->ignored[k] == id;
                if (!seen && s->n_ignored < 8) {
                    s->ignored[s->n_ignored++] = id;
                    cn_log("steam: lobby member %llu \"%s\" ignored (not a friend; net steamallow %llu to let them in)", ULL(id), pname(s, id), ULL(id));
                }
                continue;
            }
            for (int k = 1; k < CN_MAX_PEERS && k <= s->max_peers; k++)
                if (s->peer[k].state == PS_FREE) { idx = k; break; }
            if (idx < 0) continue;   /* more members than seats: the lobby limit prevents it, Steam may still show a late one */
            cst_peer *pe = &s->peer[idx];
            memset(pe, 0, sizeof *pe);
            pe->id = id, pe->state = PS_GREET, pe->first_greet = now, pe->t_h = now - HELLO_EVERY_MS, pe->rtt = -1;
            cn_log("steam: \"%s\" (%llu) joined the lobby, greeting (peer %d)", pname(s, id), ULL(id), idx);
            host_presence(s);
        }
        s->peer[idx].t_seen = now;
    }
    for (int k = 1; k < CN_MAX_PEERS; k++) {
        cst_peer *pe = &s->peer[k];
        if (pe->state == PS_FREE || pe->state == PS_CLOSING) continue;
        if ((int32_t)(now - pe->t_seen) > MEMBER_GONE_MS) {
            drop_peer(s, k, "left the lobby", 0);
            host_presence(s);
        }
    }
}

static void deal_message(cst_st *s, uint64_t from, const uint8_t *m, size_t n, uint32_t now)
{
    if (n < 1) return;
    if (g_trace && (m[0] != 'D' || g_trace_rx++ < 20)) cn_log("steam: pipe received '%c' %u bytes from %llu%s", m[0] >= 32 ? m[0] : '?', (unsigned)n, ULL(from), peer_index(s, from) < 0 ? " (not a known peer)" : "");
    int idx = peer_index(s, from);
    if (idx < 0) {
        /* a member we have not listed yet (the pipe is faster than the refresh): only a hello may introduce it */
        if (!s->host || m[0] != 'H') return;
        host_members(s, now);
        idx = peer_index(s, from);
        if (idx < 0) return;
    }
    cst_peer *pe = &s->peer[idx];
    if (pe->state == PS_CLOSING) return;
    switch (m[0]) {
    case 'H':
    case 'W': {
        uint64_t lobby = 0;
        if (n < 11 || m[1] != PROTO) return;
        memcpy(&lobby, m + 2, 8);
        if (lobby != s->lobby) return;   /* a hello of another / an older lobby */
        if (pe->state == PS_BYE && m[0] != 'H') return;   /* a parked peer is revived by its own hello only */
        pe->t_rx = now;
        if (m[0] == 'H') send_hello(s, from, 'W');
        if (pe->state == PS_GREET || pe->state == PS_BYE) link_peer(s, idx, now);
        break;
    }
    case 'D':
        if (pe->state != PS_LINKED || n < 3) return;
        pe->t_rx = now;
        if (s->nq < 60) {
            uint8_t *d = (uint8_t *)malloc(n - 2);
            if (d) {
                memcpy(d, m + 2, n - 2);
                qpush(s, CN_EV_RECEIVE, idx, m[1], d, n - 2);
            }
        }
        break;
    case 'P':
        if (pe->state != PS_LINKED || n < 5) return;
        pe->t_rx = now;
        {
            uint8_t q[5] = {'Q', m[1], m[2], m[3], m[4]};
            pipe_put(s, from, 0, q, 5, NULL, 0);
        }
        break;
    case 'Q':
        if (pe->state != PS_LINKED || n < 5) return;
        {
            uint32_t t;
            memcpy(&t, m + 1, 4);
            pe->t_rx = now;
            uint32_t rtt = now - t;
            if (rtt < 20000) pe->rtt = (int)rtt;
        }
        break;
    case 'B':
        cn_log("steam: peer %d said goodbye", idx);
        if (s->host) drop_peer(s, idx, "said bye", 1);
        else {
            drop_peer(s, idx, "the host said bye", 0);
            s->stage = S_DEAD;
        }
        break;
    default:
        break;
    }
}

static void timers(cst_st *s, uint32_t now)
{
    for (int i = 0; i < CN_MAX_PEERS; i++) {
        cst_peer *pe = &s->peer[i];
        switch (pe->state) {
        case PS_GREET:
            if ((int32_t)(now - pe->t_h) >= HELLO_EVERY_MS) {
                pe->t_h = now;
                send_hello(s, pe->id, 'H');
            }
            if (s->host && (int32_t)(now - pe->first_greet) > GREET_GIVE_UP_MS) {
                cn_log("steam: peer %d (%llu) never answered the hello: parked (is it running the co-op mod?)", i, ULL(pe->id));
                pe->state = PS_BYE;
                s->p->pipe_close(s->c, pe->id);
            }
            break;
        case PS_LINKED:
            if ((int32_t)(now - pe->t_ping) >= PING_EVERY_MS) {
                uint8_t m[5] = {'P'};
                pe->t_ping = now;
                memcpy(m + 1, &now, 4);
                pipe_put(s, pe->id, 0, m, 5, NULL, 0);
                int ping = -1;
                int st = s->p->pipe_state(s->c, pe->id, &ping);
                if (pe->rtt < 0 && ping >= 0) pe->rtt = ping;
                if (st == CST_PS_FAILED) {
                    if (!pe->t_bad) pe->t_bad = now | 1u;
                    else if ((int32_t)(now - (pe->t_bad & ~1u)) > PIPE_FAIL_MS) drop_peer(s, i, "the Steam session failed", 0);
                } else
                    pe->t_bad = 0;
            }
            if (pe->state == PS_LINKED && (int32_t)(now - pe->t_rx) > SILENT_MS) {
                drop_peer(s, i, "silent for 30 s", 0);
                if (!s->host) s->stage = S_DEAD;
            }
            break;
        case PS_CLOSING:
            if ((int32_t)(now - pe->t_close) >= 0) {
                if (!pe->quiet) qpush(s, CN_EV_DISCONNECT, i, 0, NULL, 0);
                s->p->pipe_close(s->c, pe->id);
                memset(pe, 0, sizeof *pe);
                pe->rtt = -1;
            }
            break;
        default:
            break;
        }
    }
}

static int member_of_lobby(cst_st *s, uint64_t id)
{
    int n = s->p->lobby_count(s->c, s->lobby);
    for (int i = 0; i < n; i++)
        if (s->p->lobby_member(s->c, s->lobby, i) == id) return 1;
    return 0;
}

/* session requests on the pipe (Steam callback 1251 / 1202): accept only people we expect */
static void accept_requests(cst_st *s)
{
    cst_hub *h = s->h;
    DWORD tick = GetTickCount();
    for (int i = 0; i < h->n_req; i++) {
        uint64_t id = h->req[i];
        int accept = 0, keep = 0;
        if (s->host) {
            if (peer_index(s, id) >= 0 || (member_of_lobby(s, id) && policy_allows(s, id))) accept = 1;
            else keep = 1;   /* maybe the lobby list is not up to date yet */
        } else if (s->peer[0].id == id && s->peer[0].state != PS_FREE)
            accept = 1;
        else if (s->stage < S_C_LOBBY)
            keep = 1;
        if (accept) s->p->pipe_accept(s->c, id);
        if (keep && tick - h->req_t[i] > 15000) keep = 0;
        if (!keep) {
            h->req[i] = h->req[h->n_req - 1], h->req_t[i] = h->req_t[h->n_req - 1];
            h->n_req--;
            i--;
        }
    }
}

static int st_service(void *sp, cn_event *out, int max)
{
    cst_st *s = (cst_st *)sp;
    LOCK();
    uint32_t now = s->p->now(s->c);
    cst_hub *h = s->h;
    hub_pump(h);
    if (s->stage != S_DEAD) {
        if (s->host) {
            if ((int32_t)(now - s->t_members) >= MEMBER_REFRESH_MS || h->dirty) {
                s->t_members = now, h->dirty = 0;
                host_members(s, now);
            }
        } else {
            switch (s->stage) {
            case S_C_RESOLVE: client_resolve(s, now); break;
            case S_C_SEARCH: client_search(s, now); break;
            case S_C_JOIN: client_joining(s, now); break;
            case S_C_LOBBY:
            case S_C_LINKED:
                if ((int32_t)(now - s->t_check) >= 500 || h->dirty) {
                    s->t_check = now, h->dirty = 0;
                    uint64_t o = s->p->lobby_owner(s->c, s->lobby);
                    int n = s->p->lobby_count(s->c, s->lobby);
                    if (o == s->owner && n > 0) s->t_owner_bad = 0;
                    else if (!s->t_owner_bad) s->t_owner_bad = now | 1u;
                    else if ((int32_t)(now - (s->t_owner_bad & ~1u)) > MEMBER_GONE_MS) {
                        if (s->stage == S_C_LINKED) {
                            drop_peer(s, 0, "the host left the lobby", 0);
                            s->stage = S_DEAD;
                        } else
                            client_fail(s, "the host left the lobby before the link opened");
                    }
                }
                if (s->stage == S_C_LOBBY && (int32_t)(now - s->t0) > CLIENT_TOTAL_MS) client_fail(s, "the host did not answer on the Steam pipe (firewall / relay problem?)");
                break;
            }
            if (s->stage != S_C_LINKED && s->stage != S_C_LOBBY && s->stage != S_DEAD && (int32_t)(now - s->t0) > CLIENT_TOTAL_MS) client_fail(s, "timed out");
        }
        accept_requests(s);
        for (int k = 0; k < 128 && s->nq < 56; k++) {
            uint64_t from = 0;
            size_t len = 0;
            if (!s->p->pipe_recv(s->c, &from, s->rx, MSG_MAX, &len)) break;
            if (s->stage == S_DEAD) break;
            deal_message(s, from, s->rx, len, now);
        }
        timers(s, now);
    }
    int n = 0;
    while (n < max && s->nq > 0) {
        out[n++] = s->q[0];
        memmove(&s->q[0], &s->q[1], (size_t)(s->nq - 1) * sizeof s->q[0]);
        s->nq--;
    }
    UNLOCK();
    return n;
}

static void st_disconnect(void *sp, int peer, int graceful)
{
    cst_st *s = (cst_st *)sp;
    if (peer < 0 || peer >= CN_MAX_PEERS) return;
    LOCK();
    cst_peer *pe = &s->peer[peer];
    if (pe->state == PS_LINKED || pe->state == PS_GREET) {
        if (pe->state == PS_LINKED) send_bye(s, pe->id);
        pe->state = PS_CLOSING;
        pe->quiet = !graceful;
        pe->t_close = s->p->now(s->c) + CLOSE_LINGER_MS;
    }
    UNLOCK();
}

static int st_rtt(void *sp, int peer)
{
    cst_st *s = (cst_st *)sp;
    if (peer < 0 || peer >= CN_MAX_PEERS) return -1;
    LOCK();
    int r = s->peer[peer].state == PS_LINKED ? s->peer[peer].rtt : -1;
    UNLOCK();
    return r;
}

static void st_close(void *sp)
{
    cst_st *s = (cst_st *)sp;
    if (!s) return;
    LOCK();
    int said = 0;
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (s->peer[i].state == PS_LINKED) send_bye(s, s->peer[i].id), said = 1;
    if (said) {
        UNLOCK();
        Sleep(40);   /* let the goodbyes leave before the sessions are torn down */
        LOCK();
    }
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (s->peer[i].state != PS_FREE) s->p->pipe_close(s->c, s->peer[i].id);
    if (s->lobby && (s->host || s->in_lobby)) s->p->lobby_leave(s->c, s->lobby);
    if (s->host) s->p->presence_clear(s->c);
    if (s->h->my_lobby == s->lobby) s->h->my_lobby = 0;
    for (int i = 0; i < s->nq; i++) free(s->q[i].data);
    cn_log("steam: transport closed (lobby %llu)", ULL(s->lobby));
    free(s->rx);
    free(s);
    UNLOCK();
}

static int st_failed(void *sp, char *why, size_t size)
{
    cst_st *s = (cst_st *)sp;
    int r;
    LOCK();
    r = !s->host && s->stage == S_DEAD && s->why[0] && s->peer[0].state == PS_FREE;
    if (r) snprintf(why, size, "%s", s->why);
    UNLOCK();
    return r;
}

const cn_ops cn_ops_steam = {"steam", st_host, st_join, st_send, st_service, st_disconnect, st_rtt, st_close, st_failed};

/* ---------------------------------------------------------------------------------------- hooks (proxy) */
static int g_listen = 1;
static DWORD g_tick_t0;
static int g_tick_started;

void cst_note_context(void *ctx) { cst_real_note_context(ctx); }
void cst_note_callback_layout(int kind) { cst_real_note_callback_layout(kind); }

uint64_t cst_take_pending_lobby(void)
{
    uint64_t l = 0;
    LOCK();
    cst_hub *h = default_hub(0);
    if (h) l = h->pend_lobby, h->pend_lobby = 0;
    UNLOCK();
    return l;
}

/* +connect_lobby <id> on the command line (Steam starts the game that way from "Join game" / an invite when it is not running) */
int cst_callbacks_wanted(void) { return g_cb_wanted; }

static void parse_cmdline(cst_hub *h)
{
    const char *cl = GetCommandLineA();
    uint64_t l = 0;
    if (cl && parse_connect_lobby(cl, &l)) {
        h->pend_lobby = l;
        cn_log("steam: started by \"Join game\" with +connect_lobby %llu", ULL(l));
    }
}

void cst_game_tick(void)
{
    char v[8];
    if (!g_tick_started) {
        g_tick_started = 1;
        g_tick_t0 = GetTickCount();
        if (GetEnvironmentVariableA("REV2_STEAM", v, sizeof v) && v[0] == '0') g_listen = 0;
        if (GetEnvironmentVariableA("REV2_STEAM_TRACE", v, sizeof v) && v[0] == '1') g_trace = 1;
    }
    if (!g_listen) return;
    cst_real_tick();   /* registers the game-thread callbacks once something wants them */
    if (!TRYLOCK()) return;
    cst_hub *h = default_hub(1);
    if (h) {
        char e[160];
        if (hub_ready(h, e, sizeof e)) {   /* a failed attempt is retried only every 3 s */
            /* rel010 (b11a live): once, when the hub is ready the first time, whoever made it ready. Before, only a hub that was still untried here (state 0)
             * asked for the callback objects: in Revelations 1 the first tick comes before the game's Steam context exists (state 2, "not created yet"),
             * the retry 3 s later succeeded with first = 0, and Join game / invites (callback 333) never reached the mod. */
            if (!g_cb_wanted) {
                g_cb_wanted = 1;
                cst_real_want_callbacks();
                parse_cmdline(h);
            }
            hub_pump(h);
        }
    }
    uint64_t lobby = 0;
    int auto_join = 1;
    if (h && h->state == 1 && h->pend_lobby) {
        lobby = h->pend_lobby;
        h->pend_lobby = 0;
        if (h->my_lobby) {
            cn_log("steam: join request for lobby %llu ignored: this process is already in lobby %llu", ULL(lobby), ULL(h->my_lobby));
            lobby = 0;
        }
    }
    UNLOCK();
    if (lobby) {
        if (GetEnvironmentVariableA("REV2_STEAM_AUTOJOIN", v, sizeof v) && v[0] == '0') auto_join = 0;
        if (auto_join && g_run_line) {
            char line[80], out[400] = "";
            snprintf(line, sizeof line, "join lobby:%llu steam", ULL(lobby));
            g_run_line(line, out, sizeof out);
            cn_log("steam: auto-join of lobby %llu: %s", ULL(lobby), out);
        } else {
            LOCK();
            if (h) h->pend_lobby = lobby;   /* kept for `net steamjoin pending` */
            UNLOCK();
        }
    }
}

int cst_swallow_game_callback(int id, const void *pv)
{
    if ((id != 333 && id != 505 && id != 506) || !pv || !g_listen) return 0;
    char v[8];
    if (GetEnvironmentVariableA("REV2_STEAM_SWALLOW", v, sizeof v) && v[0] == '0') return 0;
    cst_hub *h = default_hub(0);   /* read without the lock: this runs inside the game's RunCallbacks and must never wait for a Steam call of another thread */
    if (!h || h->state != 1) return 0;
    uint64_t lobby;
    memcpy(&lobby, pv, 8);
    if (lobby && lobby == h->my_lobby) return 1;
    if (id != 333 || !lobby) return 0;
    /* an invite / Join game request for a lobby we know nothing about: look at its data (a Steam call), but never wait long for the lock */
    int got = 0;
    for (int i = 0; i < 25 && !(got = TRYLOCK()); i++) Sleep(2);
    if (!got) return 0;
    int r = 0;
    char t[32] = "", raid[32] = "";
    h->p->lobby_get(h->c, lobby, CST_TAG_KEY, t, sizeof t);
    if (!strcmp(t, CST_TAG_VAL)) r = 1;
    else {
        h->p->lobby_get(h->c, lobby, "SlotPublicOpen", raid, sizeof raid);
        if (!raid[0]) h->p->lobby_get(h->c, lobby, "OwnerId", raid, sizeof raid);
        if (!raid[0]) h->p->lobby_get(h->c, lobby, "SearchKeyNum", raid, sizeof raid);
        r = raid[0] ? 0 : 1;   /* no Raid key visible: nobody but this mod can have made an invite to it (the Raid servers are gone) */
    }
    UNLOCK();
    return r;
}

/* ---------------------------------------------------------------------------------------- commands */
static const char *lobby_type_name(int t) { return t == CST_LOBBY_PUBLIC ? "public" : t == CST_LOBBY_PRIVATE ? "private" : "friends only"; }

static void status_text(char *out, size_t size)
{
    LOCK();
    const cst_plat *p = g_plat ? g_plat : cst_real_platform();
    cst_hub *h = p ? default_hub(1) : NULL;
    char e[160] = "";
    int o = 0;
    if (!p) o = snprintf(out, size, "steam: not available in this process (needs the 32-bit game process or a test platform)");
    else if (!h || !hub_ready(h, e, sizeof e)) o = snprintf(out, size, "steam: not ready: %s", e[0] ? e : "no hub");
    else {
        o = snprintf(out, size, "steam: ready (%s), you are %llu \"%s\", pipe %s, listening %s, accepts %s%s", h->p->name, ULL(h->me), h->my_name, h->p->pipe_name(h->c),
                     g_listen ? "yes" : "no", g_policy == 1 ? "any lobby member" : g_policy == 2 ? "allow list only" : "friends", g_n_allow ? " + allow list" : "");
        if (h->p->diag) {
            char d[160] = "";
            h->p->diag(h->c, d, sizeof d);
            if (d[0]) o += snprintf(out + o, size - o, "\n%s", d);
        }
        if (h->my_lobby) o += snprintf(out + o, size - o, "\nlobby %llu (%d member(s))", ULL(h->my_lobby), h->p->lobby_count(h->c, h->my_lobby));
        else o += snprintf(out + o, size - o, "\nno lobby");
        if (h->pend_lobby) o += snprintf(out + o, size - o, "; pending join request for lobby %llu (net steamjoin pending)", ULL(h->pend_lobby));
    }
    if (g_last_err[0] && (size_t)o < size) snprintf(out + o, size - o, "\nlast error: %s", g_last_err);
    UNLOCK();
}

static int do_lobbies(char *out, size_t size)
{
    LOCK();
    cst_hub *h = default_hub(1);
    char e[160] = "";
    if (!h || !hub_ready(h, e, sizeof e)) {
        snprintf(out, size, "steam not ready: %s", e);
        UNLOCK();
        return 1;
    }
    h->list_done = 0;
    h->p->lobby_search(h->c, CST_TAG_KEY, CST_TAG_VAL, NULL, NULL, 20);
    UNLOCK();
    DWORD t = GetTickCount();
    for (;;) {
        LOCK();
        hub_pump(h);
        int done = h->list_done;
        UNLOCK();
        if (done || GetTickCount() - t > 4000) break;
        Sleep(20);
    }
    LOCK();
    size_t o = 0;
    o += (size_t)snprintf(out + o, size - o, "public lobbies with our tag (%s=%s): %d%s", CST_TAG_KEY, CST_TAG_VAL, h->list_done ? h->list_n : 0, h->list_done ? "" : " (no answer in 4 s)");
    for (int i = 0; h->list_done && i < h->list_n && i < 20 && o + 100 < size; i++) {
        uint64_t l = h->p->lobby_at(h->c, i);
        char host[40] = "", name[64] = "", mx[8] = "", ph[16] = "";
        h->p->lobby_get(h->c, l, CST_KEY_HOST, host, sizeof host);
        h->p->lobby_get(h->c, l, CST_KEY_NAME, name, sizeof name);
        h->p->lobby_get(h->c, l, CST_KEY_MAX, mx, sizeof mx);
        h->p->lobby_get(h->c, l, CST_KEY_PHASE, ph, sizeof ph);
        o += (size_t)snprintf(out + o, size - o, "\n  lobby %llu  host %s \"%s\"  %d/%s  %s", ULL(l), host, name, h->p->lobby_count(h->c, l), mx[0] ? mx : "?", ph);
    }
    int nf = h->p->friend_count(h->c), shown = 0;
    for (int i = 0; i < nf && o + 100 < size; i++) {
        uint64_t f = h->p->friend_at(h->c, i);
        uint32_t app = 0;
        uint64_t lobby = 0;
        if (!h->p->friend_game(h->c, f, &app, &lobby) || (app & 0xFFFFFF) != APP_ID) continue;
        char nm[64];
        ascii_name(h->p->name_of(h->c, f), nm, sizeof nm);
        o += (size_t)snprintf(out + o, size - o, "%s\n  friend %llu \"%s\" is in the game%s", shown++ ? "" : "\nfriends in Revelations:", ULL(f), nm, lobby ? "" : " (no lobby)");
        if (lobby) o += (size_t)snprintf(out + o, size - o, ", lobby %llu%s", ULL(lobby), tag_ok(h->p, h->c, lobby) ? " (ours)" : "");
    }
    if (!shown && o + 40 < size) snprintf(out + o, size - o, "\nno friend is in Revelations right now");
    UNLOCK();
    return 1;
}

/* rel010: the lobby's friend row: friends who are in Revelations with a lobby right now (the game info Steam gives for each friend), in the friends list's
 * order, at most max. A Raid lobby of the game looks the same from here: joining one fails with the usual "no co-op lobby" reason (the same as a typed ID). */
int cst_friends_in_game(uint64_t *ids, char (*names)[40], int max)
{
    int n = 0;
    LOCK();
    cst_hub *h = default_hub(1);
    char e[160] = "";
    if (h && hub_ready(h, e, sizeof e)) {
        int nf = h->p->friend_count(h->c);
        for (int i = 0; i < nf && n < max; i++) {
            uint64_t f = h->p->friend_at(h->c, i), lobby = 0;
            uint32_t app = 0;
            if (!f || !h->p->friend_game(h->c, f, &app, &lobby) || (app & 0xFFFFFF) != APP_ID || !lobby) continue;
            ids[n] = f;
            ascii_name(h->p->name_of(h->c, f), names[n], 40);
            n++;
        }
    }
    UNLOCK();
    return n;
}

/* a friend by SteamID64 or by (part of) the persona name */
static uint64_t find_friend(cst_hub *h, const char *arg)
{
    uint64_t id = 0;
    while (*arg == ' ') arg++;
    if (parse_u64(arg, &id) && is_user_id(id)) return id;
    if (!*arg) return 0;
    char want[64];
    ascii_name(arg, want, sizeof want);
    _strlwr(want);
    int n = h->p->friend_count(h->c);
    for (int i = 0; i < n; i++) {
        uint64_t f = h->p->friend_at(h->c, i);
        char nm[64];
        ascii_name(h->p->name_of(h->c, f), nm, sizeof nm);
        _strlwr(nm);
        if (strstr(nm, want)) return f;
    }
    return 0;
}

static int do_invite(const char *arg, char *out, size_t size)
{
    LOCK();
    cst_hub *h = default_hub(0);
    if (!h || h->state != 1 || !h->my_lobby) {
        snprintf(out, size, "no Steam lobby: host first (net steamhost)");
        UNLOCK();
        return 1;
    }
    while (*arg == ' ') arg++;
    if (!*arg) {
        int ok = h->p->invite_dialog(h->c, h->my_lobby);
        snprintf(out, size, ok ? "Steam's invite dialog opened for lobby %llu (or use: net steaminvite <steamid|name>)" : "the invite dialog could not be opened (use: net steaminvite <steamid|name>)", ULL(h->my_lobby));
    } else {
        uint64_t f = find_friend(h, arg);
        if (!f) snprintf(out, size, "no friend '%s' (use the SteamID64 or part of the name)", arg);
        else {
            int ok = h->p->lobby_invite(h->c, h->my_lobby, f);
            char nm[64];
            ascii_name(h->p->name_of(h->c, f), nm, sizeof nm);
            snprintf(out, size, ok ? "invite to %llu \"%s\" sent (lobby %llu); they get a Steam notification and can click Join" : "Steam refused the invite to %llu \"%s\" (lobby %llu)", ULL(f), nm, ULL(h->my_lobby));
        }
    }
    UNLOCK();
    return 1;
}

int cst_command(cs_t *s, const char *cmd, const char *arg, char *out, size_t size)
{
    char err[200];
    while (*arg == ' ') arg++;
    if (!strcmp(cmd, "steam") || !strcmp(cmd, "steamstatus")) {
        status_text(out, size);
    } else if (!strcmp(cmd, "steamhost")) {
        int type = CST_LOBBY_FRIENDS, max = 2;
        char w[24];
        const char *p = arg;
        while (sscanf(p, "%23s", w) == 1) {
            if (!strcmp(w, "public")) type = CST_LOBBY_PUBLIC;
            else if (!strcmp(w, "friends")) type = CST_LOBBY_FRIENDS;
            else if (!strcmp(w, "private")) type = CST_LOBBY_PRIVATE;
            else if (w[0] >= '2' && w[0] <= '4' && !w[1]) max = w[0] - '0';
            p = strstr(p, w) + strlen(w);
            while (*p == ' ') p++;
        }
        cst_set_host_options(type, max);
        if (cs_host(s, "steam", 0, g_threaded, err, sizeof err)) {
            LOCK();
            cst_hub *h = default_hub(0);
            snprintf(out, size, "hosting a Steam lobby (%s, up to %d players): lobby %llu; your SteamID %llu. Friends see \"Join game\" on your Steam profile; or they run: net steamjoin %llu",
                     lobby_type_name(type), max, ULL(h ? h->my_lobby : 0), ULL(h ? h->me : 0), ULL(h ? h->me : 0));
            UNLOCK();
        } else
            snprintf(out, size, "steamhost failed: %s", err);
    } else if (!strcmp(cmd, "steamjoin")) {
        char target[80];
        if (!arg[0]) {
            snprintf(out, size, "usage: net steamjoin <host steamid64 | lobby id | lobby:<id> | pending>");
            return 1;
        }
        if (!strncmp(arg, "pending", 7)) {
            uint64_t l = cst_take_pending_lobby();
            if (!l) {
                snprintf(out, size, "no pending join request (net steam shows it when there is one)");
                return 1;
            }
            snprintf(target, sizeof target, "lobby:%llu", ULL(l));
        } else
            snprintf(target, sizeof target, "%s", arg);
        if (cs_join(s, "steam", target, g_threaded, err, sizeof err)) snprintf(out, size, "joining %s through Steam ... (net status / net steam show how far it got)", target);
        else snprintf(out, size, "steamjoin failed: %s", err);
    } else if (!strcmp(cmd, "steamlobbies")) {
        do_lobbies(out, size);
    } else if (!strcmp(cmd, "steaminvite")) {
        do_invite(arg, out, size);
    } else if (!strcmp(cmd, "steamallow")) {
        uint64_t id;
        if (!strcmp(arg, "any")) cst_set_policy(1), snprintf(out, size, "the host now links with ANY member of its lobby");
        else if (!strcmp(arg, "friends")) cst_set_policy(0), snprintf(out, size, "the host links with friends (and the allow list) only");
        else if (!strcmp(arg, "list")) cst_set_policy(2), snprintf(out, size, "the host links with the allow list only");
        else if (parse_u64(arg, &id)) cst_allow(id), snprintf(out, size, "%llu added to the allow list", ULL(id));
        else snprintf(out, size, "usage: net steamallow any|friends|list|<steamid64>");
    } else if (!strcmp(cmd, "steamtrace")) {
        g_trace = arg[0] ? atoi(arg) : 1;
        g_trace_n = g_trace_rx = 0;
        snprintf(out, size, "steam trace %s", g_trace ? "on: every pipe message but the data ones is logged (the first 20 of those)" : "off");
    } else if (!strcmp(cmd, "steampipe")) {
        cst_real_set_pipe(arg, out, size);
    } else
        return 0;
    return 1;
}
