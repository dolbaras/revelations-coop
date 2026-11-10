/*
 * Co-op session layer, see coop_session.h and docs/protocol.md.
 *
 * Topology: a star. The host (pid 0) is the hub; clients (pid 1..3) talk to the host only, the host relays STATE and EVENT
 * between clients. Pid = index in the lobby table; on the host the transport peer of a client is looked up (peers change
 * when a player reconnects, pids do not).
 */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_session.h"
#include "coop_steam.h"

#define RING 32
#define STATE_INTERVAL_MS 45
#define HB_INTERVAL_MS 500
#define HELLO_DEADLINE_MS 5000
#define JOIN_DEADLINE_MS 10000
#define JOIN_DEADLINE_STEAM_MS 30000   /* lobby search + JoinLobby + Steam relay setup take longer than a UDP connect */
/* Stage 6i: 300 s. The first limit (120 s) was shorter than the slowest test PC's story load (the N100 needs ~150 s for a chapter demo plus the area,
 * and a player's old disk can be slower still): the host opened the barrier 30 s early and ran the opening scene alone. A hung game still ends the wait. */
#define BARRIER_TIMEOUT_MS 300000
#define WAIT_NOTE_FIRST_MS 20000    /* a machine that has finished loading tells its player after this long that it waits for the other one */
#define WAIT_NOTE_EVERY_MS 30000
/* Silence of a partner whose link is up (the transport keeps it: ENet on its own thread, Steam pings). The game runs its logic only while its window is active
 * (stage2a.md section 1: byte app+0x2551), so a player in Alt-Tab, a stalled PC (the owner's main PC: ~90 s, stage6.md 413 / 478) or a game that hangs sends
 * nothing: CS_N_SILENT tells the other player after 8 s and every 10 s, the session gives up on it after 10 minutes (a defined end of every wait, also of the
 * barrier of a client whose host never comes back). A real crash ends the link through the transport much sooner (ENet 10-30 s, Steam 3-30 s). */
#define SILENT_FIRST_MS 8000
#define SILENT_EVERY_MS 10000
#define SILENT_LIMIT_MS 600000
/* a known player who connects again while its old link still looks alive (the transport has not noticed the crash yet) takes its seat back when the old link
 * has been silent this long; a live game sends a heartbeat every 500 ms */
#define GHOST_SILENT_MS 3000
#define DEFAULT_INTERP_MS 100
#define EXTRAP_MAX_MS 250
#define STALE_MS 1000
#define JUMP_UNITS 500.0f

typedef struct { cp_state s; uint32_t rx; int jump; } rsample;
typedef struct {
    rsample r[RING];
    int count, head;   /* head = index of the newest sample */
    int have_seq;
    uint16_t last_seq;
    uint32_t off_ref;  /* first (local - remote) clock difference, modulo 2^32: the machines' uptimes may differ by weeks */
    double off_rel;    /* minimum of (difference - off_ref) over time, milliseconds */
    int have_off;
    uint32_t last_rx;
} rstate;

typedef struct {
    int used, connected, peer, hello_done;
    char name[CP_NAME_MAX + 1];
    uint8_t character, ready, flags, loaded;
    uint32_t hb_peer_t, hb_peer_rx;
    int have_hb;
    float srtt;
    int have_rtt;
    uint32_t n_rx, n_dup, n_old, n_stale, n_gap;
    rstate rs;
    uint32_t heard;        /* session clock of the last message of any kind from this player (the host watches its clients, a client its host) */
    uint32_t silent_next;  /* when the next CS_N_SILENT is due */
    int silent_on;         /* a silence note was given and the player has not been heard since */
} pl_t;

struct cs_t {
    CRITICAL_SECTION lock;
    cs_config cfg;
    char name[CP_NAME_MAX + 1], mod[CP_MOD_MAX + 1];
    cn_t *net;
    int role;          /* CN_ROLE_* of this session */
    int joining;       /* client: waiting for WELCOME */
    uint32_t join_start, join_deadline;
    int phase;
    int pid;
    cp_session session;
    uint32_t lobby_rev;
    pl_t pl[CP_MAX_PLAYERS];
    uint32_t pend_ms[CN_MAX_PEERS];   /* host: transport-connected peers that have not said hello yet (connect time + 1) */
    uint8_t epoch, me_load_epoch;
    uint32_t load_started;
    uint32_t wait_ls, wait_next;   /* stage 6i: the load_started that the last 'still waiting' note belongs to, and the time of the next note */
    uint32_t last_hb, last_state;
    uint16_t state_seq, event_seq, world_seq;
    cp_world world_q[64];   /* received WORLD messages waiting for the game thread (ring) */
    unsigned world_w, world_r;
    uint32_t world_last_seq[CP_MAX_PLAYERS]; int world_have_seq[CP_MAX_PLAYERS];
    uint32_t tx_world, rx_world, rx_world_drop;
    cp_pad pad_q[64];       /* received PAD messages waiting for the game thread (ring, the newest 64) */
    unsigned pad_w, pad_r;
    uint32_t tx_pad, rx_pad, rx_pad_drop;
    cs_stats st;
    int interp_delay;
    int threaded;
    uint32_t rx_now;   /* arrival time (network thread) of the message being handled */
    int host_left;     /* client: the host said LEAVE (its disconnect follows) */
};

static uint32_t now_of(cs_t *s) { return s->cfg.clock ? s->cfg.clock(s->cfg.clock_user) : cn_now_ms(); }
uint32_t cs_now(cs_t *s) { return now_of(s); }

static void note_ex(cs_t *s, int what, int pid, int a, int b, const char *text, const cp_event *ev, const char *peer_mod)
{
    if (!s->cfg.on_note) return;
    cs_note n;
    n.what = what, n.pid = pid, n.a = a, n.b = b, n.text = text, n.ev = ev, n.peer_mod = peer_mod;
    s->cfg.on_note(s->cfg.user, &n);
}
static void note(cs_t *s, int what, int pid, int a, int b, const char *text, const cp_event *ev) { note_ex(s, what, pid, a, b, text, ev, NULL); }

static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    if (!src || !*src) src = "player";
    for (; src[i] && i < CP_NAME_MAX; i++) dst[i] = (unsigned char)src[i] < 32 ? '?' : src[i];
    dst[i] = 0;
}

/* ---------------------------------------------------------------- helpers */
static int send_msg(cs_t *s, int peer, int channel, int reliable, const cp_msg *m)
{
    uint8_t buf[CP_MAX_PACKET];
    size_t n = cp_encode(m, buf, sizeof buf);
    if (!n) return 0;
    return cn_send(s->net, peer, channel, reliable, buf, n);
}

/* to everybody the sender is connected to: the host sends to every client except `except_pid`, a client only to the host */
static void broadcast(cs_t *s, int channel, int reliable, const cp_msg *m, int except_pid)
{
    if (s->role == CN_ROLE_CLIENT) {
        send_msg(s, 0, channel, reliable, m);
        return;
    }
    for (int i = 1; i < CP_MAX_PLAYERS; i++)
        if (s->pl[i].used && s->pl[i].connected && s->pl[i].hello_done && i != except_pid) send_msg(s, s->pl[i].peer, channel, reliable, m);
}

static int pid_of_peer(cs_t *s, int peer)
{
    if (s->role == CN_ROLE_CLIENT) return peer == 0 ? 0 : -1;
    for (int i = 1; i < CP_MAX_PLAYERS; i++)
        if (s->pl[i].used && s->pl[i].connected && s->pl[i].peer == peer) return i;
    return -1;
}

static void build_lobby(cs_t *s, cp_lobby *l)
{
    memset(l, 0, sizeof *l);
    l->rev = s->lobby_rev;
    for (int i = 0; i < CP_MAX_PLAYERS; i++) {
        if (!s->pl[i].used) continue;
        cp_player *p = &l->p[l->count++];
        p->pid = (uint8_t)i, p->character = s->pl[i].character, p->ready = s->pl[i].ready;
        p->flags = (uint8_t)((s->pl[i].flags & ~CP_PF_CONNECTED & ~CP_PF_LOADED) | (s->pl[i].connected ? CP_PF_CONNECTED : 0) | (s->pl[i].loaded ? CP_PF_LOADED : 0) | (i == 0 ? CP_PF_HOST : 0));
        memcpy(p->name, s->pl[i].name, sizeof p->name);
    }
}

static void apply_lobby(cs_t *s, const cp_lobby *l)
{
    int seen[CP_MAX_PLAYERS] = {0};
    for (int k = 0; k < l->count; k++) {
        const cp_player *p = &l->p[k];
        if (p->pid >= CP_MAX_PLAYERS) continue;
        pl_t *q = &s->pl[p->pid];
        seen[p->pid] = 1;
        if (!q->used) memset(q, 0, sizeof *q);
        q->used = 1;
        q->connected = (p->flags & CP_PF_CONNECTED) != 0;
        q->character = p->character, q->ready = p->ready, q->flags = p->flags & ~CP_PF_CONNECTED & ~CP_PF_LOADED, q->loaded = (p->flags & CP_PF_LOADED) != 0;
        memcpy(q->name, p->name, sizeof q->name);
        q->peer = p->pid == 0 ? 0 : -1;
    }
    for (int i = 0; i < CP_MAX_PLAYERS; i++)
        if (!seen[i] && s->pl[i].used) memset(&s->pl[i], 0, sizeof s->pl[i]);
    s->lobby_rev = l->rev;
}

static void send_lobby(cs_t *s, int except_pid)
{
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_LOBBY_STATE;
    build_lobby(s, &m.u.lobby);
    broadcast(s, CP_CH_CONTROL, 1, &m, except_pid);
}

static void lobby_changed(cs_t *s)
{
    s->lobby_rev++;
    send_lobby(s, -1);
    note(s, CS_N_LOBBY, -1, 0, 0, NULL, NULL);
}

static int slot_taken_by_other(cs_t *s, int pid, int character)
{
    for (int i = 0; i < CP_MAX_PLAYERS; i++)
        if (i != pid && s->pl[i].used && s->pl[i].connected && s->pl[i].character <= 3 && CP_CHAR_SLOT(s->pl[i].character) == CP_CHAR_SLOT(character)) return 1;
    return 0;
}

/* a character of the session's pair (part 0: Claire/Moira, part 1: Barry/Natalia) for a slot */
static int pair_char(cs_t *s, int slot) { return (s->session.part ? 2 : 0) + (slot & 1); }

static int free_character(cs_t *s, int pid, int want)
{
    if (want >= 0 && want <= 3 && !slot_taken_by_other(s, pid, want)) return want;
    int pair = (want >= 0 && want <= 3) ? (want & ~1) : (s->session.part ? 2 : 0);
    for (int slot = 0; slot < 2; slot++)
        if (!slot_taken_by_other(s, pid, pair + slot)) return pair + slot;
    return CP_CHAR_NONE;
}

static uint32_t random32(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    uint32_t x = (uint32_t)c.QuadPart ^ ((uint32_t)GetTickCount() * 2654435761u) ^ ((uint32_t)GetCurrentProcessId() << 16);
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return x ? x : 1;
}

static void reset_session(cs_t *s)
{
    memset(s->pl, 0, sizeof s->pl);
    memset(s->pend_ms, 0, sizeof s->pend_ms);
    memset(&s->st, 0, sizeof s->st);
    memset(&s->session, 0, sizeof s->session);
    s->pid = -1, s->phase = CP_PHASE_NONE, s->epoch = 0, s->me_load_epoch = 0, s->joining = 0, s->role = CN_ROLE_NONE;
    s->lobby_rev = 0, s->state_seq = 0, s->event_seq = 0, s->last_hb = 0, s->last_state = 0;
    s->host_left = 0;
}

/* a message of any kind came from pid at time t: it is alive (and the player is told when it was silent before) */
static void heard(cs_t *s, int pid, uint32_t t)
{
    if (pid < 0 || pid >= CP_MAX_PLAYERS || pid == s->pid || !s->pl[pid].used) return;
    pl_t *p = &s->pl[pid];
    if (p->silent_on) {
        uint32_t quiet = t - p->heard;
        p->silent_on = 0;
        cn_log("session: player %d (%s) is heard again after %u s of silence", pid, p->name, quiet / 1000u);
        note(s, CS_N_SILENT, pid, 0, (int)(quiet / 1000u), p->name, NULL);
    }
    p->heard = t;
}

/* ---------------------------------------------------------------- state ring and interpolation */
static void rs_clear(rstate *r) { memset(r, 0, sizeof *r); }

static void clear_remote_states(cs_t *s)
{
    for (int i = 0; i < CP_MAX_PLAYERS; i++) rs_clear(&s->pl[i].rs), s->world_have_seq[i] = 0;
    s->world_r = s->world_w;   /* world records of the old level instance are of no use */
    s->pad_r = s->pad_w;       /* neither are pad records */
}

/* returns 1 when the sample was accepted */
static int rs_push(cs_t *s, pl_t *p, const cp_state *st, uint32_t now)
{
    rstate *r = &p->rs;
    if (st->epoch != s->epoch) { p->n_stale++, s->st.rx_stale_epoch++; return 0; }   /* another level instance: not part of this sequence */
    if (r->have_seq) {
        if (st->seq == r->last_seq) { p->n_dup++, s->st.rx_dup++; return 0; }
        if (!cp_seq16_newer(st->seq, r->last_seq)) { p->n_old++, s->st.rx_old++; return 0; }
        uint16_t gap = (uint16_t)(st->seq - r->last_seq - 1);
        p->n_gap += gap, s->st.rx_gap += gap;
    }
    r->have_seq = 1, r->last_seq = st->seq;
    if (!r->have_off) r->off_ref = now - st->t_ms, r->off_rel = 0, r->have_off = 1;
    else {
        double d = (double)(int32_t)((now - st->t_ms) - r->off_ref);
        r->off_rel += (double)(int32_t)(now - r->last_rx) * 0.0001;   /* allow the minimum to creep up (clock drift) */
        if (d < r->off_rel) r->off_rel = d;
    }
    r->last_rx = now;
    rsample *n = &r->r[(r->head + (r->count ? 1 : 0)) % RING];
    int jump = 0;
    if (r->count) {
        const rsample *a = &r->r[r->head];
        float dx = st->pos[0] - a->s.pos[0], dy = st->pos[1] - a->s.pos[1], dz = st->pos[2] - a->s.pos[2];
        jump = (st->flags & CP_SF_TELEPORT) || sqrtf(dx * dx + dy * dy + dz * dz) > JUMP_UNITS;
    }
    n->s = *st, n->rx = now, n->jump = jump;
    r->head = (int)(n - r->r);
    if (r->count < RING) r->count++;
    p->n_rx++, s->st.rx_state++;
    return 1;
}

static float lerp(float a, float b, float u) { return a + (b - a) * u; }
static float lerp_angle(float a, float b, float u, float period)   /* shortest way round a circle of `period` */
{
    float d = fmodf(b - a, period);
    if (d > period * 0.5f) d -= period;
    if (d < -period * 0.5f) d += period;
    return a + d * u;
}

static const rsample *rs_at(const rstate *r, int back)   /* 0 = newest */
{
    return &r->r[(r->head - back + RING * 2) % RING];
}

int cs_remote_sample(cs_t *s, int pid, uint32_t now_ms, cp_state *out)
{
    EnterCriticalSection(&s->lock);
    int res = CS_SAMPLE_NONE;
    if (pid >= 0 && pid < CP_MAX_PLAYERS && s->pl[pid].used) {
        const rstate *r = &s->pl[pid].rs;
        if (r->count > 0) {
            int32_t rt = (int32_t)(now_ms - r->off_ref - (uint32_t)(int32_t)lround(r->off_rel) - (uint32_t)s->interp_delay);   /* remote clock time being shown */
            const rsample *newest = rs_at(r, 0), *oldest = rs_at(r, r->count - 1);
            int32_t tn = (int32_t)newest->s.t_ms;
            if ((int32_t)(rt - (int32_t)oldest->s.t_ms) <= 0) {
                *out = oldest->s;
                out->t_ms = (uint32_t)rt;
                res = CS_SAMPLE_INTERP;
            } else if ((int32_t)(rt - tn) < 0) {
                int k = 0;   /* newest sample with t <= rt is rs_at(k+1) while rs_at(k).t > rt */
                while (k + 1 < r->count && (int32_t)(rs_at(r, k + 1)->s.t_ms - (uint32_t)rt) > 0) k++;
                const rsample *b = rs_at(r, k), *a = rs_at(r, k + 1 < r->count ? k + 1 : k);
                *out = a->s;
                int32_t span = (int32_t)(b->s.t_ms - a->s.t_ms);
                if (!b->jump && span > 0 && span <= 1000 && a != b) {
                    float u = (float)(rt - (int32_t)a->s.t_ms) / (float)span;
                    if (u < 0) u = 0;
                    if (u > 1) u = 1;
                    for (int i = 0; i < 3; i++) out->pos[i] = lerp(a->s.pos[i], b->s.pos[i], u), out->aimv[i] = lerp(a->s.aimv[i], b->s.aimv[i], u);
                    out->yaw = lerp_angle(a->s.yaw, b->s.yaw, u, 6.2831853f);
                    out->aim = lerp_angle(a->s.aim, b->s.aim, u, 360.0f);
                }
                out->t_ms = (uint32_t)rt;
                res = CS_SAMPLE_INTERP;
            } else {
                int32_t ahead = rt - tn;
                *out = newest->s;
                res = ahead > STALE_MS ? CS_SAMPLE_STALE : CS_SAMPLE_EXTRAP;
                out->t_ms = (uint32_t)rt;
                if (ahead <= STALE_MS && r->count >= 2 && !newest->jump) {
                    const rsample *prev = rs_at(r, 1);
                    int32_t span = (int32_t)(newest->s.t_ms - prev->s.t_ms);
                    if (span >= 10 && span <= 300) {
                        float dt = (float)(ahead > EXTRAP_MAX_MS ? EXTRAP_MAX_MS : ahead);
                        for (int i = 0; i < 3; i++) out->pos[i] = newest->s.pos[i] + (newest->s.pos[i] - prev->s.pos[i]) / (float)span * dt;
                    }
                }
            }
        }
    }
    LeaveCriticalSection(&s->lock);
    return res;
}

/* ---------------------------------------------------------------- lifecycle */
cs_t *cs_new(const cs_config *cfg)
{
    cs_t *s = (cs_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    InitializeCriticalSection(&s->lock);
    s->cfg = *cfg;
    copy_name(s->name, cfg->name);
    snprintf(s->mod, sizeof s->mod, "%s", cfg->mod_version ? cfg->mod_version : "");
    s->cfg.name = s->name, s->cfg.mod_version = s->mod;
    s->interp_delay = cfg->interp_delay_ms > 0 ? cfg->interp_delay_ms : DEFAULT_INTERP_MS;
    s->net = cn_new();
    if (s->net && cfg->clock) cn_set_clock(s->net, cfg->clock, cfg->clock_user);
    reset_session(s);
    return s;
}

void cs_free(cs_t *s)
{
    if (!s) return;
    cs_leave(s, 0);
    cn_free(s->net);
    DeleteCriticalSection(&s->lock);
    free(s);
}

static void init_local_player(cs_t *s, int pid, int host)
{
    pl_t *p = &s->pl[pid];
    memset(p, 0, sizeof *p);
    p->used = 1, p->connected = 1, p->hello_done = 1, p->peer = -1;
    p->flags = (uint8_t)((host ? CP_PF_HOST : 0) | (s->cfg.is_bot ? CP_PF_BOT : 0) | CP_PF_CONNECTED);
    memcpy(p->name, s->name, sizeof p->name);
    p->character = CP_CHAR_NONE;
}

int cs_host(cs_t *s, const char *transport, int port, int threaded, char *err, size_t esz)
{
    EnterCriticalSection(&s->lock);
    cs_leave(s, 0);
    reset_session(s);
    s->threaded = threaded;
    int ok = cn_host(s->net, transport, port, CP_MAX_PLAYERS - 1, threaded, err, esz);
    if (ok) {
        s->role = CN_ROLE_HOST, s->pid = 0, s->phase = CP_PHASE_LOBBY;
        init_local_player(s, 0, 1);
        s->session.id = random32();
        s->session.game_type = 0, s->session.episode = 1, s->session.part = 0, s->session.difficulty = 1, s->session.max_players = 2;
        s->session.seed = random32();
        s->pl[0].character = (uint8_t)(s->cfg.want_char >= 0 && s->cfg.want_char <= 3 ? s->cfg.want_char : pair_char(s, 0));
        s->lobby_rev = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

int cs_join(cs_t *s, const char *transport, const char *target, int threaded, char *err, size_t esz)
{
    EnterCriticalSection(&s->lock);
    cs_leave(s, 0);
    reset_session(s);
    s->threaded = threaded;
    int ok = cn_join(s->net, transport, target, threaded, err, esz);
    if (ok) {
        s->role = CN_ROLE_CLIENT, s->joining = 1, s->join_start = now_of(s), s->join_deadline = transport && !strcmp(transport, "steam") ? JOIN_DEADLINE_STEAM_MS : JOIN_DEADLINE_MS;
        s->phase = CP_PHASE_NONE;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

void cs_leave(cs_t *s, int reason)
{
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && s->net) {
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_LEAVE, m.u.leave.reason = (uint8_t)reason;
        broadcast(s, CP_CH_CONTROL, 1, &m, -1);
        cn_stop(s->net);
        cn_log("session: left (reason %d)", reason);
    }
    reset_session(s);
    LeaveCriticalSection(&s->lock);
}

/* ---------------------------------------------------------------- host side */
static void check_barrier(cs_t *s);
/* why: 0 left or the transport dropped it, 1 given up after SILENT_LIMIT_MS, 2 its old link is replaced by a new connection of the same player, 3 removed (cs_kick) */
static void player_gone(cs_t *s, int pid, int lost, int why)
{
    pl_t *p = &s->pl[pid];
    if (!p->used || !p->connected) return;
    char name[CP_NAME_MAX + 1];
    memcpy(name, p->name, sizeof name);
    p->connected = 0, p->loaded = 0, p->silent_on = 0;
    cn_log("session: player %d (%s) %s", pid, name, why == 1 ? "given up after the silence limit" : why == 2 ? "replaced by its new connection" : why == 3 ? "removed by the host" : lost ? "lost" : "left");
    if (s->phase == CP_PHASE_LOBBY) {   /* before the start nobody keeps a seat */
        memset(p, 0, sizeof *p);
    } else {
        rs_clear(&p->rs);
    }
    note(s, CS_N_PEER_LEAVE, pid, lost, why, name, NULL);
    if (s->role == CN_ROLE_HOST) {
        s->lobby_rev++;
        send_lobby(s, -1);
        note(s, CS_N_LOBBY, -1, 0, 0, NULL, NULL);
        check_barrier(s);   /* a partner who leaves while a level loads must not hold the barrier: the host runs on (found by the first pair run from the menus, 2026-11-03) */
    }
}

static int barrier_complete(cs_t *s)
{
    int any = 0;
    for (int i = 0; i < CP_MAX_PLAYERS; i++)
        if (s->pl[i].used && s->pl[i].connected) {
            any = 1;
            if (!s->pl[i].loaded) return 0;
        }
    return any;
}

static void open_barrier(cs_t *s, const char *why)
{
    s->phase = CP_PHASE_PLAYING;
    clear_remote_states(s);
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_GO, m.u.load.epoch = s->epoch;
    broadcast(s, CP_CH_CONTROL, 1, &m, -1);
    cn_log("session: barrier open, epoch %u (%s)", s->epoch, why);
    note(s, CS_N_GO, -1, s->epoch, 0, why, NULL);
}

static void check_barrier(cs_t *s)
{
    if (s->role == CN_ROLE_HOST && s->phase == CP_PHASE_LOADING && barrier_complete(s)) open_barrier(s, "all loaded");
}

/* REFUSE (code, text, and the host's mod version: the 0.2.0 tail); text NULL = the standard text of the code */
static void refuse_text(cs_t *s, int peer, int code, const char *text)
{
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_REFUSE, m.u.refuse.code = (uint8_t)code;
    snprintf(m.u.refuse.text, sizeof m.u.refuse.text, "%s", text ? text : cp_refuse_text((unsigned)code));
    snprintf(m.u.refuse.mod, sizeof m.u.refuse.mod, "%s", s->mod);
    send_msg(s, peer, CP_CH_CONTROL, 1, &m);
    cn_disconnect_peer(s->net, peer, 1);
    cn_log("session: refused peer %d: %s", peer, m.u.refuse.text);
}
static void refuse(cs_t *s, int peer, int code) { refuse_text(s, peer, code, NULL); }

/* the same co-op package on both PCs: the release version of the mod (R2_VERSION) must be equal; bots (the lab's headless partner) and an empty version (a raw
 * test sender) are not checked. The text is English: an older client (0.1.x) shows it as it is; a newer one shows its own text in its language (by the code). */
static int mod_mismatch(const char *a, const char *b) { return a[0] && b[0] && strcmp(a, b) != 0; }
static void mod_mismatch_text(char *out, size_t size, const char *host_mod, const char *client_mod)
{
    if (host_mod[0]) snprintf(out, size, "co-op mod %s on the host, %s here: both need the same version (the Co-op shortcut updates it)", host_mod, client_mod);
    else snprintf(out, size, "the host has an older co-op mod, %s here: both need the same version (the Co-op shortcut updates it)", client_mod);
}

static void host_hello(cs_t *s, int peer, const cp_hello *h)
{
    s->pend_ms[peer] = 0;
    unsigned lo = h->min_version > CP_PROTO_MIN ? h->min_version : CP_PROTO_MIN, hi = h->version < CP_PROTO_VERSION ? h->version : CP_PROTO_VERSION;
    if (lo > hi) { refuse(s, peer, CP_REF_VERSION); return; }
    static const uint8_t zero[8];
    if (memcmp(h->exe_tag, zero, 8) && memcmp(s->cfg.exe_tag, zero, 8) && memcmp(h->exe_tag, s->cfg.exe_tag, 8)) { refuse(s, peer, CP_REF_BUILD); return; }
    if (!(h->flags & CP_PF_BOT) && !s->cfg.is_bot && mod_mismatch(h->mod, s->mod)) {
        char t[CP_TEXT_MAX + 1];
        mod_mismatch_text(t, sizeof t, s->mod, h->mod);
        refuse_text(s, peer, CP_REF_MODVER, t);
        return;
    }
    if (s->phase != CP_PHASE_LOBBY && s->cfg.no_rejoin) {   /* product rule (Rev1 0.1.0): a running game takes nobody, not even its own player again */
        cn_log("session: %s wants to join a running game: refused (no rejoin after the start)", h->name);
        refuse(s, peer, CP_REF_STARTED);
        return;
    }
    {   /* re-entry while the transport has not yet noticed that the old link is dead (a crash, then a quick restart): the old link gives the seat back */
        uint32_t now = now_of(s);
        for (int i = 1; i < CP_MAX_PLAYERS; i++) {
            pl_t *g = &s->pl[i];
            if (g->used && g->connected && g->hello_done && g->peer != peer && !strncmp(g->name, h->name, CP_NAME_MAX) && (int32_t)(now - g->heard) >= GHOST_SILENT_MS) {
                int old = g->peer;
                cn_log("session: player %d (%s) connects again while its old link still looks alive (silent %u ms): the old link is dropped", i, g->name,
                       (unsigned)(now - g->heard));
                player_gone(s, i, 1, 2);
                cn_disconnect_peer(s->net, old, 0);
                break;
            }
        }
    }
    int pid = -1;
    if (s->phase != CP_PHASE_LOBBY) {   /* after the start only a known player (same name, not connected) may come back */
        for (int i = 1; i < CP_MAX_PLAYERS; i++)
            if (s->pl[i].used && !s->pl[i].connected && !strncmp(s->pl[i].name, h->name, CP_NAME_MAX)) pid = i;
        if (pid < 0) { refuse(s, peer, CP_REF_STARTED); return; }
    } else {
        int count = 0;
        for (int i = 0; i < CP_MAX_PLAYERS; i++) count += s->pl[i].used;
        if (count >= (s->session.max_players ? s->session.max_players : 2)) { refuse(s, peer, CP_REF_FULL); return; }
        for (int i = 1; i < CP_MAX_PLAYERS; i++)
            if (!s->pl[i].used) { pid = i; break; }
        if (pid < 0) { refuse(s, peer, CP_REF_FULL); return; }
    }
    pl_t *p = &s->pl[pid];
    int rejoin = p->used;
    uint8_t keep_char = p->character;
    if (!rejoin) memset(p, 0, sizeof *p);
    p->used = 1, p->connected = 1, p->hello_done = 1, p->peer = peer;
    if (!rejoin) copy_name(p->name, h->name);
    p->flags = (uint8_t)((h->flags & CP_PF_BOT) | CP_PF_CONNECTED);
    p->character = rejoin ? keep_char : (uint8_t)free_character(s, pid, h->want_char);
    p->ready = rejoin ? 1 : 0, p->loaded = 0;
    p->heard = now_of(s), p->silent_on = 0;
    rs_clear(&p->rs);
    s->lobby_rev++;
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_WELCOME;
    m.u.welcome.version = (uint16_t)hi, m.u.welcome.pid = (uint8_t)pid, m.u.welcome.host_ms = now_of(s);
    m.u.welcome.flags = (uint8_t)(s->cfg.is_bot ? CP_PF_BOT : 0);
    m.u.welcome.phase = (uint8_t)s->phase, m.u.welcome.epoch = s->epoch;
    m.u.welcome.session = s->session;
    build_lobby(s, &m.u.welcome.lobby);
    snprintf(m.u.welcome.mod, sizeof m.u.welcome.mod, "%s", s->mod);
    send_msg(s, peer, CP_CH_CONTROL, 1, &m);
    send_lobby(s, pid);
    cn_log("session: player %d (%s) %s as %s", pid, p->name, rejoin ? "came back" : "joined", cp_char_name(p->character));
    note(s, CS_N_PEER_JOIN, pid, rejoin, 0, p->name, NULL);
    note(s, CS_N_LOBBY, -1, 0, 0, NULL, NULL);
    if (rejoin && s->phase == CP_PHASE_PLAYING) {
        /* a returning player must load into the current level before the host runs on: restart the barrier for the new epoch */
        s->epoch++;
        s->phase = CP_PHASE_LOADING, s->load_started = now_of(s);
        for (int i = 0; i < CP_MAX_PLAYERS; i++) s->pl[i].loaded = 0;
        cp_msg b;
        memset(&b, 0, sizeof b);
        b.type = CP_LOAD_BEGIN, b.u.load.pid = (uint8_t)pid, b.u.load.epoch = s->epoch, b.u.load.area_id = 0;
        broadcast(s, CP_CH_CONTROL, 1, &b, -1);
        note(s, CS_N_LOAD_BEGIN, pid, s->epoch, 0, NULL, NULL);
    }
}

/* ---------------------------------------------------------------- message handling */
static void handle_state(cs_t *s, int from_pid, const cp_msg *m, const uint8_t *raw, size_t len)
{
    const cp_state *st = &m->u.state;
    int src = s->role == CN_ROLE_HOST ? from_pid : st->pid;
    if (s->role == CN_ROLE_HOST && st->pid != from_pid) return;   /* a client may only speak for itself */
    if (src < 0 || src >= CP_MAX_PLAYERS || src == s->pid || !s->pl[src].used) return;
    if (s->phase == CP_PHASE_LOBBY || s->phase == CP_PHASE_NONE) return;
    int ok = rs_push(s, &s->pl[src], st, s->rx_now);
    if (ok && s->role == CN_ROLE_HOST)   /* relay to the other clients, unchanged */
        for (int i = 1; i < CP_MAX_PLAYERS; i++)
            if (i != src && s->pl[i].used && s->pl[i].connected && s->pl[i].hello_done) cn_send(s->net, s->pl[i].peer, CP_CH_STATE, 0, raw, len);
}

/* WORLD: unreliable, sequenced; kept for the game thread in a ring (the newest 64), stale epochs and old sequence numbers are dropped, the host relays to the other clients */
static void handle_world(cs_t *s, int from_pid, const cp_msg *m, const uint8_t *raw, size_t len)
{
    const cp_world *wd = &m->u.world;
    int src = s->role == CN_ROLE_HOST ? from_pid : wd->pid;
    if (s->role == CN_ROLE_HOST && wd->pid != from_pid) return;
    if (src < 0 || src >= CP_MAX_PLAYERS || src == s->pid || !s->pl[src].used) return;
    if (s->phase != CP_PHASE_PLAYING || wd->epoch != s->epoch) {
        s->rx_world_drop++;
        return;
    }
    if (s->world_have_seq[src] && !cp_seq16_newer(wd->seq, (uint16_t)s->world_last_seq[src])) {
        s->rx_world_drop++;
        return;
    }
    s->world_have_seq[src] = 1, s->world_last_seq[src] = wd->seq;
    s->world_q[s->world_w++ % 64] = *wd;
    if (s->world_w - s->world_r > 64) s->world_r = s->world_w - 64;
    s->rx_world++;
    if (s->role == CN_ROLE_HOST)
        for (int i = 1; i < CP_MAX_PLAYERS; i++)
            if (i != src && s->pl[i].used && s->pl[i].connected && s->pl[i].hello_done) cn_send(s->net, s->pl[i].peer, CP_CH_STATE, 0, raw, len);
}

/* PAD: unreliable, sequenced; kept for the game thread in a ring (the newest 64). Records are numbered per sender and a packet may repeat the newest record (keepalive), so the
 * sequence filter is the receiver's (puppet1.c); here only the phase, the epoch and the sender are checked. The host relays to the other clients. */
static void handle_pad(cs_t *s, int from_pid, const cp_msg *m, const uint8_t *raw, size_t len)
{
    const cp_pad *pd = &m->u.pad;
    int src = s->role == CN_ROLE_HOST ? from_pid : pd->pid;
    if (s->role == CN_ROLE_HOST && pd->pid != from_pid) return;
    if (src < 0 || src >= CP_MAX_PLAYERS || src == s->pid || !s->pl[src].used) return;
    if (s->phase != CP_PHASE_PLAYING || pd->epoch != s->epoch) {
        s->rx_pad_drop++;
        return;
    }
    s->pad_q[s->pad_w++ % 64] = *pd;
    if (s->pad_w - s->pad_r > 64) s->pad_r = s->pad_w - 64;
    s->rx_pad++;
    if (s->role == CN_ROLE_HOST)
        for (int i = 1; i < CP_MAX_PLAYERS; i++)
            if (i != src && s->pl[i].used && s->pl[i].connected && s->pl[i].hello_done) cn_send(s->net, s->pl[i].peer, CP_CH_STATE, 0, raw, len);
}

static void handle_heartbeat(cs_t *s, int from_pid, const cp_heartbeat *h)
{
    if (from_pid < 0 || from_pid >= CP_MAX_PLAYERS || !s->pl[from_pid].used) return;
    pl_t *p = &s->pl[from_pid];
    uint32_t now = s->rx_now;
    s->st.hb_rx++;
    p->hb_peer_t = h->t_ms, p->hb_peer_rx = now, p->have_hb = 1;
    if (h->echo_t_ms) {
        int32_t rtt = (int32_t)(now - h->echo_t_ms - h->echo_dwell_ms);
        if (rtt >= 0 && rtt < 20000) {
            p->srtt = p->have_rtt ? p->srtt * 0.8f + (float)rtt * 0.2f : (float)rtt;
            p->have_rtt = 1;
        }
    }
}

static void handle_load_begin(cs_t *s, int from_pid, const cp_msg *m)
{
    if (s->phase == CP_PHASE_LOBBY || s->phase == CP_PHASE_NONE) return;
    uint8_t e = m->u.load.epoch;
    int who = s->role == CN_ROLE_HOST ? from_pid : m->u.load.pid;
    if (who < 0 || who >= CP_MAX_PLAYERS) return;
    if (cp_seq8_newer(e, s->epoch)) {   /* a new level instance */
        s->epoch = e;
        s->phase = CP_PHASE_LOADING, s->load_started = now_of(s);
        for (int i = 0; i < CP_MAX_PLAYERS; i++) s->pl[i].loaded = 0;
        clear_remote_states(s);
        if (s->role == CN_ROLE_HOST) {
            cp_msg r = *m;
            r.u.load.pid = (uint8_t)who;
            broadcast(s, CP_CH_CONTROL, 1, &r, who);
        }
        note(s, CS_N_LOAD_BEGIN, who, e, (int)m->u.load.area_id, NULL, NULL);
    } else if (e == s->epoch && s->phase == CP_PHASE_LOADING) {
        s->pl[who].loaded = 0;
        note(s, CS_N_LOAD_BEGIN, who, e, (int)m->u.load.area_id, NULL, NULL);
    }
}

static void common_message(cs_t *s, int from_pid, const cp_msg *m, const uint8_t *raw, size_t len)
{
    switch (m->type) {
    case CP_HEARTBEAT:
        handle_heartbeat(s, s->role == CN_ROLE_HOST ? from_pid : m->u.hb.pid, &m->u.hb);
        break;
    case CP_STATE:
        handle_state(s, from_pid, m, raw, len);
        break;
    case CP_WORLD:
        handle_world(s, from_pid, m, raw, len);
        break;
    case CP_PAD:
        handle_pad(s, from_pid, m, raw, len);
        break;
    case CP_LOAD_BEGIN:
        handle_load_begin(s, from_pid, m);
        break;
    case CP_EVENT: {
        if (s->role == CN_ROLE_HOST && m->u.event.pid != from_pid) break;
        s->st.rx_event++;
        if (s->role == CN_ROLE_HOST) broadcast(s, CP_CH_CONTROL, 1, m, from_pid);
        note(s, CS_N_EVENT, m->u.event.pid, 0, 0, NULL, &m->u.event);
        break;
    }
    case CP_LEAVE:
        if (s->role == CN_ROLE_HOST) player_gone(s, from_pid, 0, 0);
        else s->host_left = 1;   /* client: the transport disconnect of the host follows; the player is told that the host left */
        break;
    default:
        break;
    }
}

static void host_message(cs_t *s, int peer, const cp_msg *m, const uint8_t *raw, size_t len)
{
    if (m->type == CP_HELLO) {
        if (pid_of_peer(s, peer) >= 0) return;   /* second hello: ignore */
        host_hello(s, peer, &m->u.hello);
        return;
    }
    int pid = pid_of_peer(s, peer);
    if (pid < 0) {   /* not welcomed yet: only the hello is allowed */
        cn_log("session: dropped %s from peer %d before hello", cp_type_name(m->type), peer);
        return;
    }
    switch (m->type) {
    case CP_LOBBY_PICK: {
        int ch = m->u.pick.character;
        cp_msg n;
        memset(&n, 0, sizeof n);
        n.type = CP_NOTICE;
        if (s->phase != CP_PHASE_LOBBY || ch > 3) {
            n.u.refuse.code = CP_NOTE_REJECTED;
            snprintf(n.u.refuse.text, sizeof n.u.refuse.text, "character change not possible now");
            send_msg(s, peer, CP_CH_CONTROL, 1, &n);
        } else if (slot_taken_by_other(s, pid, ch)) {
            n.u.refuse.code = CP_NOTE_SLOT_TAKEN;
            snprintf(n.u.refuse.text, sizeof n.u.refuse.text, "%s is taken", cp_char_name((unsigned)ch));
            send_msg(s, peer, CP_CH_CONTROL, 1, &n);
        } else {
            s->pl[pid].character = (uint8_t)ch, s->pl[pid].ready = 0;
            lobby_changed(s);
        }
        break;
    }
    case CP_LOBBY_READY:
        if (s->phase == CP_PHASE_LOBBY) {
            s->pl[pid].ready = m->u.ready.ready ? 1 : 0;
            lobby_changed(s);
        }
        break;
    case CP_LOAD_DONE:
        if (s->phase == CP_PHASE_LOADING && m->u.load.epoch == s->epoch) {
            s->pl[pid].loaded = 1;
            check_barrier(s);
        }
        break;
    default:
        common_message(s, pid, m, raw, len);
    }
}

static void client_message(cs_t *s, const cp_msg *m, const uint8_t *raw, size_t len)
{
    switch (m->type) {
    case CP_WELCOME:
        if (!s->joining) return;
        s->joining = 0;
        if (!s->cfg.is_bot && !(m->u.welcome.flags & CP_PF_BOT) && s->mod[0] && strcmp(m->u.welcome.mod, s->mod)) {
            /* the host runs another co-op package (an older host sends no version: it never checks ours): leave at once, before anything starts */
            char t[CP_TEXT_MAX + 1], hm[CP_MOD_MAX + 1];
            snprintf(hm, sizeof hm, "%s", m->u.welcome.mod);
            mod_mismatch_text(t, sizeof t, hm, s->mod);
            cn_log("session: %s: leaving the host", t);
            cp_msg lv;
            memset(&lv, 0, sizeof lv);
            lv.type = CP_LEAVE;
            send_msg(s, 0, CP_CH_CONTROL, 1, &lv);
            cn_stop(s->net);
            reset_session(s);
            note_ex(s, CS_N_REFUSED, -1, CP_REF_MODVER, 0, t, NULL, hm);
            return;
        }
        s->pid = m->u.welcome.pid;
        s->session = m->u.welcome.session;
        apply_lobby(s, &m->u.welcome.lobby);
        s->pl[0].peer = 0;
        s->phase = m->u.welcome.phase == CP_PHASE_NONE ? CP_PHASE_LOBBY : m->u.welcome.phase;
        s->epoch = m->u.welcome.epoch;
        if (s->phase == CP_PHASE_PLAYING) s->phase = CP_PHASE_LOADING;   /* joining a running game: load first, the host answers with LOAD_BEGIN */
        if (s->pid >= 0 && s->pid < CP_MAX_PLAYERS && s->pl[s->pid].used) s->pl[s->pid].peer = -1;
        s->pl[0].heard = s->rx_now, s->pl[0].silent_on = 0;
        cn_log("session: welcomed as player %d (%s), phase %d", s->pid, cp_char_name(s->pl[s->pid].character), s->phase);
        note(s, CS_N_CONNECTED, s->pid, s->phase, 0, NULL, NULL);
        note(s, CS_N_LOBBY, -1, 0, 0, NULL, NULL);
        break;
    case CP_REFUSE:
        if (!s->joining) {
            if (m->u.refuse.code != CP_REF_REMOVED || s->role != CN_ROLE_CLIENT) return;
            /* the host removed us after the WELCOME (the lobby's Remove partner): the session ends with the reason */
            cn_log("session: the host removed this player: %s", m->u.refuse.text);
            cn_stop(s->net);
            reset_session(s);
            note(s, CS_N_LOST, -1, 3, 0, "removed by the host", NULL);
            return;
        }
        s->joining = 0;
        cn_log("session: refused by the host (its co-op mod: %s): %s", m->u.refuse.mod[0] ? m->u.refuse.mod : "not named", m->u.refuse.text);
        note_ex(s, CS_N_REFUSED, -1, m->u.refuse.code, 0, m->u.refuse.text, NULL, m->u.refuse.mod);
        cn_stop(s->net);
        reset_session(s);
        break;
    case CP_NOTICE:
        note(s, CS_N_NOTICE, -1, m->u.refuse.code, 0, m->u.refuse.text, NULL);
        break;
    case CP_SESSION:
        s->session = m->u.session;
        note(s, CS_N_SESSION, -1, 0, 0, NULL, NULL);
        break;
    case CP_LOBBY_STATE:
        if (s->joining || (int32_t)(m->u.lobby.rev - s->lobby_rev) <= 0) return;   /* old / duplicate table */
        apply_lobby(s, &m->u.lobby);
        if (s->pid >= 0 && s->pid < CP_MAX_PLAYERS) s->pl[s->pid].peer = -1;
        note(s, CS_N_LOBBY, -1, 0, 0, NULL, NULL);
        break;
    case CP_START:
        if (s->joining) return;
        s->session = m->u.start.session;
        apply_lobby(s, &m->u.start.lobby);
        if (s->pid >= 0 && s->pid < CP_MAX_PLAYERS) s->pl[s->pid].peer = -1;
        s->phase = CP_PHASE_LOADING, s->epoch = 1, s->me_load_epoch = 0, s->load_started = now_of(s);
        for (int i = 0; i < CP_MAX_PLAYERS; i++) s->pl[i].loaded = 0;
        clear_remote_states(s);
        note(s, CS_N_START, -1, 0, 0, NULL, NULL);
        break;
    case CP_GO:
        if (s->phase == CP_PHASE_LOADING && !cp_seq8_newer(s->epoch, m->u.load.epoch)) {
            s->epoch = m->u.load.epoch;
            s->phase = CP_PHASE_PLAYING;
            clear_remote_states(s);
            note(s, CS_N_GO, -1, s->epoch, 0, NULL, NULL);
        }
        break;
    default:
        common_message(s, 0, m, raw, len);
    }
}

/* ---------------------------------------------------------------- poll */
static void send_hello(cs_t *s)
{
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_HELLO;
    cp_hello *h = &m.u.hello;
    h->version = CP_PROTO_VERSION, h->min_version = CP_PROTO_MIN;
    memcpy(h->exe_tag, s->cfg.exe_tag, 8);
    h->want_char = (uint8_t)(s->cfg.want_char >= 0 && s->cfg.want_char <= 3 ? s->cfg.want_char : CP_CHAR_NONE);
    h->flags = s->cfg.is_bot ? CP_PF_BOT : 0;
    memcpy(h->name, s->name, sizeof h->name);
    snprintf(h->mod, sizeof h->mod, "%s", s->mod);
    send_msg(s, 0, CP_CH_CONTROL, 1, &m);
}

/* SILENT_FIRST_MS / SILENT_LIMIT_MS (top of the file): the host watches its clients, a client its host */
static void silence_watch(cs_t *s, uint32_t now)
{
    for (int i = 0; i < CP_MAX_PLAYERS && s->role != CN_ROLE_NONE; i++) {
        pl_t *p = &s->pl[i];
        if (i == s->pid || !p->used || !p->connected) continue;
        if (s->role == CN_ROLE_HOST ? !p->hello_done : i != 0) continue;
        uint32_t quiet = now - p->heard;
        if ((int32_t)quiet < 0) continue;
        if (quiet >= SILENT_LIMIT_MS) {
            if (s->role == CN_ROLE_HOST) {
                int peer = p->peer;
                cn_log("session: player %d (%s) has sent nothing for %u s: given up, its seat is kept (it may join again)", i, p->name, quiet / 1000u);
                player_gone(s, i, 1, 1);
                cn_disconnect_peer(s->net, peer, 0);
            } else {
                char why[96];
                snprintf(why, sizeof why, "no answer from the host for %u s", quiet / 1000u);
                cn_log("session: %s: the session is closed", why);
                cn_stop(s->net);
                reset_session(s);
                note(s, CS_N_LOST, -1, 2, 0, why, NULL);
            }
            continue;
        }
        if (quiet >= SILENT_FIRST_MS && (!p->silent_on || (int32_t)(now - p->silent_next) >= 0)) {
            p->silent_on = 1, p->silent_next = now + SILENT_EVERY_MS;
            cn_log("session: nothing from player %d (%s) for %u s (its game paused or in the background, a long load or a stall); the session gives up at %u s", i, p->name,
                   quiet / 1000u, SILENT_LIMIT_MS / 1000u);
            note(s, CS_N_SILENT, i, (int)(quiet / 1000u), SILENT_LIMIT_MS / 1000, p->name, NULL);
        }
    }
}

void cs_poll(cs_t *s)
{
    EnterCriticalSection(&s->lock);
    if (s->role == CN_ROLE_NONE || !s->net) { LeaveCriticalSection(&s->lock); return; }
    if (!s->threaded) cn_pump(s->net);
    cn_event ev;
    while (cn_recv(s->net, &ev)) {
        if (ev.type == CN_EV_CONNECT) {
            if (s->role == CN_ROLE_CLIENT) send_hello(s);
            else s->pend_ms[ev.peer] = now_of(s) | 1u;
        } else if (ev.type == CN_EV_DISCONNECT) {
            if (s->role == CN_ROLE_CLIENT) {
                int was = s->joining, left = s->host_left;
                char why[80];
                snprintf(why, sizeof why, "%s", was ? "no answer from the host" : left ? "the host left the session" : "connection to the host lost");
                int active = s->joining || s->phase != CP_PHASE_NONE;
                cn_log("session: %s", why);
                if (active) {
                    cn_stop(s->net);
                    reset_session(s);
                    note(s, was ? CS_N_REFUSED : CS_N_LOST, -1, !was && left ? 1 : 0, 0, why, NULL);
                }
            } else {
                s->pend_ms[ev.peer] = 0;
                int pid = pid_of_peer(s, ev.peer);
                if (pid >= 0) player_gone(s, pid, 1, 0);
            }
        } else if (ev.type == CN_EV_RECEIVE) {
            cp_msg m;
            s->rx_now = ev.t_ms ? ev.t_ms : now_of(s);
            heard(s, s->role == CN_ROLE_HOST ? pid_of_peer(s, ev.peer) : (ev.peer == 0 && !s->joining ? 0 : -1), s->rx_now);
            if (!cp_decode(ev.data, ev.len, &m)) {
                s->st.rx_bad++;
                cn_log("session: malformed packet (%u bytes, type %02x) from peer %d", (unsigned)ev.len, ev.len ? ev.data[0] : 0, ev.peer);
            } else if (s->role == CN_ROLE_HOST) host_message(s, ev.peer, &m, ev.data, ev.len);
            else client_message(s, &m, ev.data, ev.len);
        }
        cn_event_free(&ev);
        if (s->role == CN_ROLE_NONE) break;
    }
    if (s->role != CN_ROLE_NONE) {
        uint32_t now = now_of(s);
        if (s->role == CN_ROLE_HOST) {
            for (int p = 1; p < CN_MAX_PEERS; p++)
                if (s->pend_ms[p] && (int32_t)(now - (s->pend_ms[p] & ~1u)) > HELLO_DEADLINE_MS) {
                    cn_log("session: peer %d sent no hello in %d ms: dropped", p, HELLO_DEADLINE_MS);
                    s->pend_ms[p] = 0;
                    cn_disconnect_peer(s->net, p, 0);
                }
            if (s->phase == CP_PHASE_LOADING && (int32_t)(now - s->load_started) > BARRIER_TIMEOUT_MS) {
                for (int i = 0; i < CP_MAX_PLAYERS; i++)
                    if (s->pl[i].used && s->pl[i].connected && !s->pl[i].loaded) cn_log("session: barrier timeout, player %d never reported loaded", i);
                open_barrier(s, "barrier timeout");
            }
            silence_watch(s, now);
        } else if (!s->joining) {
            silence_watch(s, now);   /* may close the session (the host silent for the limit) */
        } else {
            char why[200] = "";
            int gave_up = cn_failed(s->net, why, sizeof why);   /* the backend knows why (Steam: no such lobby, full, not ours, ...) */
            if (gave_up || (int32_t)(now - s->join_start) > (int32_t)s->join_deadline) {
                cn_log("session: %s", gave_up ? why : "no welcome in time");
                cn_stop(s->net);
                reset_session(s);
                note(s, CS_N_REFUSED, -1, 0, 0, gave_up ? why : "no answer from the host", NULL);
            }
        }
    }
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase == CP_PHASE_LOADING && s->pid >= 0 && s->pid < CP_MAX_PLAYERS && s->pl[s->pid].loaded) {
        /* stage 6i: this machine is playable and the barrier is still closed: tell the player (log + CS_N_WAIT) who the game waits for, first after 20 s, then every 30 s */
        uint32_t now = now_of(s);
        if (s->wait_ls != s->load_started || !s->wait_next) s->wait_ls = s->load_started, s->wait_next = s->load_started + WAIT_NOTE_FIRST_MS;
        if ((int32_t)(now - s->wait_next) >= 0) {
            int who = -1;
            for (int i = 0; i < CP_MAX_PLAYERS && who < 0; i++)
                if (i != s->pid && s->pl[i].used && s->pl[i].connected && (s->role == CN_ROLE_CLIENT || !s->pl[i].loaded)) who = i;
            uint32_t waited = (now - s->load_started) / 1000u;
            int limit = s->role == CN_ROLE_HOST ? BARRIER_TIMEOUT_MS / 1000 : 0;
            if (limit)
                cn_log("session: still waiting for %s (player %d) to finish loading: %u s so far, the barrier opens by itself at %d s", who >= 0 ? s->pl[who].name : "the other player", who, waited, limit);
            else
                cn_log("session: still waiting for %s (player %d) to finish loading: %u s so far", who >= 0 ? s->pl[who].name : "the other player", who, waited);
            note(s, CS_N_WAIT, who, (int)waited, limit, who >= 0 ? s->pl[who].name : "", NULL);
            s->wait_next = now + WAIT_NOTE_EVERY_MS;
        }
    }
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase != CP_PHASE_NONE) {
        uint32_t now = now_of(s);
        if ((int32_t)(now - s->last_hb) >= HB_INTERVAL_MS) {
            s->last_hb = now;
            cp_msg m;
            memset(&m, 0, sizeof m);
            m.type = CP_HEARTBEAT;
            m.u.hb.pid = (uint8_t)s->pid, m.u.hb.t_ms = now, m.u.hb.phase = (uint8_t)s->phase, m.u.hb.epoch = s->epoch, m.u.hb.state_seq = s->state_seq;
            if (s->role == CN_ROLE_CLIENT) {
                const pl_t *h = &s->pl[0];
                m.u.hb.echo_t_ms = h->have_hb ? h->hb_peer_t : 0, m.u.hb.echo_dwell_ms = h->have_hb ? now - h->hb_peer_rx : 0;
                send_msg(s, 0, CP_CH_STATE, 0, &m);
            } else {
                for (int i = 1; i < CP_MAX_PLAYERS; i++) {
                    pl_t *p = &s->pl[i];
                    if (!p->used || !p->connected || !p->hello_done) continue;
                    m.u.hb.echo_t_ms = p->have_hb ? p->hb_peer_t : 0, m.u.hb.echo_dwell_ms = p->have_hb ? now - p->hb_peer_rx : 0;
                    send_msg(s, p->peer, CP_CH_STATE, 0, &m);
                }
            }
        }
    }
    LeaveCriticalSection(&s->lock);
}

/* ---------------------------------------------------------------- local actions */
int cs_pick(cs_t *s, int character)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase == CP_PHASE_LOBBY && character >= 0 && character <= 3) {
        if (s->role == CN_ROLE_HOST) {
            if (!slot_taken_by_other(s, 0, character)) {
                s->pl[0].character = (uint8_t)character, s->pl[0].ready = 0;
                lobby_changed(s);
                ok = 1;
            }
        } else {
            cp_msg m;
            memset(&m, 0, sizeof m);
            m.type = CP_LOBBY_PICK, m.u.pick.character = (uint8_t)character;
            ok = send_msg(s, 0, CP_CH_CONTROL, 1, &m);
        }
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

/* host, lobby: the host plays `slot` (0 lead, 1 partner) and every other connected player the other slot (a swap of the two in one lobby revision, which a pick of
 * the host alone cannot do while the other player holds the slot). Ready flags are cleared when something changed. 1 = the roles are as asked. */
int cs_assign_roles(cs_t *s, int host_slot)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role == CN_ROLE_HOST && !s->joining && s->phase == CP_PHASE_LOBBY && (host_slot == 0 || host_slot == 1)) {
        int base = s->pl[0].character <= 3 ? (s->pl[0].character & ~1) : (s->session.part ? 2 : 0);
        int changed = 0;
        for (int i = 0; i < CP_MAX_PLAYERS; i++) {
            if (!s->pl[i].used || !s->pl[i].connected) continue;
            int want = base + (i == 0 ? host_slot : 1 - host_slot);
            if (s->pl[i].character != want) s->pl[i].character = (uint8_t)want, changed = 1;
        }
        if (changed) {
            for (int i = 0; i < CP_MAX_PLAYERS; i++) s->pl[i].ready = 0;
            lobby_changed(s);
        }
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

int cs_force_roles(cs_t *s, int host_slot)
{
    int r = -1;
    EnterCriticalSection(&s->lock);
    int first = s->phase == CP_PHASE_LOADING && s->epoch == 1;
    for (int i = 0; i < CP_MAX_PLAYERS && first; i++)
        if (s->pl[i].used && s->pl[i].loaded) first = 0;
    if (s->role == CN_ROLE_HOST && !s->joining && first && (host_slot == 0 || host_slot == 1)) {
        int base = s->pl[0].character <= 3 ? (s->pl[0].character & ~1) : (s->session.part ? 2 : 0);
        int changed = 0;
        for (int i = 0; i < CP_MAX_PLAYERS; i++) {
            if (!s->pl[i].used || !s->pl[i].connected) continue;
            int want = base + (i == 0 ? host_slot : 1 - host_slot);
            if (s->pl[i].character != want) s->pl[i].character = (uint8_t)want, changed = 1;
        }
        if (changed) {   /* the ready flags stay: the session has started */
            cn_log("session: the roles changed after the START, before the first level: the host plays slot %d (the co-op save's roles)", host_slot);
            lobby_changed(s);
        }
        r = changed;
    }
    LeaveCriticalSection(&s->lock);
    return r;
}

int cs_ready(cs_t *s, int ready)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase == CP_PHASE_LOBBY) {
        if (s->role == CN_ROLE_HOST) {
            s->pl[0].ready = ready ? 1 : 0;
            lobby_changed(s);
            ok = 1;
        } else {
            cp_msg m;
            memset(&m, 0, sizeof m);
            m.type = CP_LOBBY_READY, m.u.ready.ready = (uint8_t)(ready ? 1 : 0);
            ok = send_msg(s, 0, CP_CH_CONTROL, 1, &m);
        }
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

int cs_set_session(cs_t *s, const cp_session *ss)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role == CN_ROLE_HOST && s->phase == CP_PHASE_LOBBY) {
        uint32_t id = s->session.id;
        s->session = *ss;
        s->session.id = id;
        if (!s->session.max_players) s->session.max_players = 2;
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_SESSION, m.u.session = s->session;
        broadcast(s, CP_CH_CONTROL, 1, &m, -1);
        note(s, CS_N_SESSION, -1, 0, 0, NULL, NULL);
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

int cs_start(cs_t *s, int force, char *err, size_t esz)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    err[0] = 0;
    if (s->role != CN_ROLE_HOST || s->phase != CP_PHASE_LOBBY) snprintf(err, esz, "only the host can start, from the lobby");
    else {
        int n = 0, ready = 0;
        for (int i = 0; i < CP_MAX_PLAYERS; i++)
            if (s->pl[i].used && s->pl[i].connected) n++, ready += s->pl[i].ready || i == 0;
        if (n < 2 && !force) snprintf(err, esz, "no other player yet");
        else if (ready < n && !force) snprintf(err, esz, "not everybody is ready");
        else {
            for (int i = 0; i < CP_MAX_PLAYERS; i++)
                if (s->pl[i].used && s->pl[i].connected && s->pl[i].character > 3) {
                    s->pl[i].character = (uint8_t)free_character(s, i, CP_CHAR_NONE);
                }
            s->phase = CP_PHASE_LOADING, s->epoch = 1, s->me_load_epoch = 0, s->load_started = now_of(s);
            for (int i = 0; i < CP_MAX_PLAYERS; i++) s->pl[i].loaded = 0;
            s->lobby_rev++;
            clear_remote_states(s);
            cp_msg m;
            memset(&m, 0, sizeof m);
            m.type = CP_START, m.u.start.session = s->session;
            build_lobby(s, &m.u.start.lobby);
            broadcast(s, CP_CH_CONTROL, 1, &m, -1);
            cn_log("session: START, %d player(s)", n);
            note(s, CS_N_START, -1, 0, 0, NULL, NULL);
            ok = 1;
        }
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

void cs_load_begin(cs_t *s, uint32_t area_id)
{
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && (s->phase == CP_PHASE_LOADING || s->phase == CP_PHASE_PLAYING)) {
        int fresh = !(s->phase == CP_PHASE_LOADING && s->epoch != s->me_load_epoch);   /* an epoch others announced is joined, not advanced */
        if (fresh) {
            s->epoch++;
            s->phase = CP_PHASE_LOADING, s->load_started = now_of(s);
            for (int i = 0; i < CP_MAX_PLAYERS; i++) s->pl[i].loaded = 0;
            clear_remote_states(s);
        }
        s->me_load_epoch = s->epoch;
        if (s->pid >= 0) s->pl[s->pid].loaded = 0;
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_LOAD_BEGIN, m.u.load.pid = (uint8_t)s->pid, m.u.load.epoch = s->epoch, m.u.load.area_id = area_id;
        broadcast(s, CP_CH_CONTROL, 1, &m, -1);
        cn_log("session: local load begins, epoch %u area %u", s->epoch, area_id);
        note(s, CS_N_LOAD_BEGIN, s->pid, s->epoch, (int)area_id, NULL, NULL);
    }
    LeaveCriticalSection(&s->lock);
}

void cs_load_done(cs_t *s)
{
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase == CP_PHASE_LOADING && s->pid >= 0) {
        s->pl[s->pid].loaded = 1;
        if (s->role == CN_ROLE_CLIENT) {
            cp_msg m;
            memset(&m, 0, sizeof m);
            m.type = CP_LOAD_DONE, m.u.load.pid = (uint8_t)s->pid, m.u.load.epoch = s->epoch;
            send_msg(s, 0, CP_CH_CONTROL, 1, &m);
        } else
            check_barrier(s);
    }
    LeaveCriticalSection(&s->lock);
}

int cs_submit_state(cs_t *s, const cp_state *in)
{
    int sent = 0;
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase == CP_PHASE_PLAYING && s->pid >= 0) {
        uint32_t now = now_of(s);
        if ((int32_t)(now - s->last_state) >= STATE_INTERVAL_MS || (in->flags & CP_SF_TELEPORT)) {
            s->last_state = now;
            cp_msg m;
            memset(&m, 0, sizeof m);
            m.type = CP_STATE;
            m.u.state = *in;
            m.u.state.pid = (uint8_t)s->pid, m.u.state.seq = ++s->state_seq, m.u.state.t_ms = now, m.u.state.epoch = s->epoch;
            broadcast(s, CP_CH_STATE, 0, &m, -1);
            s->st.tx_state++;
            sent = 1;
        }
    }
    LeaveCriticalSection(&s->lock);
    return sent;
}

int cs_send_event(cs_t *s, unsigned kind, const void *body, unsigned len)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && s->pid >= 0 && len <= CP_EVENT_MAX) {
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_EVENT;
        cp_event *e = &m.u.event;
        e->pid = (uint8_t)s->pid, e->kind = (uint16_t)kind, e->eseq = ++s->event_seq, e->t_ms = now_of(s), e->epoch = s->epoch, e->len = (uint8_t)len;
        if (len) memcpy(e->body, body, len);
        broadcast(s, CP_CH_CONTROL, 1, &m, -1);
        s->st.tx_event++;
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

/* WORLD batches (stage 6): unreliable, sequenced, at most CP_WORLD_MAX bytes of body; only while PLAYING */
int cs_send_world(cs_t *s, unsigned kind, const void *body, unsigned len)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase == CP_PHASE_PLAYING && s->pid >= 0 && len <= CP_WORLD_MAX) {
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_WORLD;
        cp_world *wd = &m.u.world;
        wd->pid = (uint8_t)s->pid, wd->kind = (uint8_t)kind, wd->seq = ++s->world_seq, wd->t_ms = now_of(s), wd->epoch = s->epoch, wd->len = (uint16_t)len;
        if (len) memcpy(wd->body, body, len);
        broadcast(s, CP_CH_STATE, 0, &m, -1);
        s->tx_world++;
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

/* PAD (Revelations 1 puppet): the sender's pad records, unreliable on the state channel, only while PLAYING. pid, epoch and the flags mask are set here. */
int cs_send_pad(cs_t *s, const cp_pad *in)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role != CN_ROLE_NONE && !s->joining && s->phase == CP_PHASE_PLAYING && s->pid >= 0 && in->count >= 1 && in->count <= CP_PAD_MAX_REC) {
        cp_msg m;
        memset(&m, 0, sizeof m);
        m.type = CP_PAD;
        m.u.pad = *in;
        m.u.pad.pid = (uint8_t)s->pid, m.u.pad.epoch = s->epoch, m.u.pad.flags &= CP_PAD_FLAGS_MASK;
        broadcast(s, CP_CH_STATE, 0, &m, -1);
        s->tx_pad++;
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

int cs_recv_pad(cs_t *s, cp_pad *out)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->pad_r != s->pad_w) {
        *out = s->pad_q[s->pad_r++ % 64];
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

void cs_pad_stats(cs_t *s, uint32_t *tx, uint32_t *rx, uint32_t *drop)
{
    EnterCriticalSection(&s->lock);
    if (tx) *tx = s->tx_pad;
    if (rx) *rx = s->rx_pad;
    if (drop) *drop = s->rx_pad_drop;
    LeaveCriticalSection(&s->lock);
}

int cs_recv_world(cs_t *s, cp_world *out)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->world_r != s->world_w) {
        *out = s->world_q[s->world_r++ % 64];
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

void cs_world_stats(cs_t *s, uint32_t *tx, uint32_t *rx, uint32_t *drop)
{
    EnterCriticalSection(&s->lock);
    if (tx) *tx = s->tx_world;
    if (rx) *rx = s->rx_world;
    if (drop) *drop = s->rx_world_drop;
    LeaveCriticalSection(&s->lock);
}

/* ---------------------------------------------------------------- information */
int cs_active(cs_t *s) { return s->role != CN_ROLE_NONE; }
int cs_is_host(cs_t *s) { return s->role == CN_ROLE_HOST; }
int cs_pid(cs_t *s) { return s->pid; }
int cs_phase(cs_t *s) { return s->phase; }
unsigned cs_epoch(cs_t *s) { return s->epoch; }
cn_t *cs_transport(cs_t *s) { return s->net; }
void cs_get_stats(cs_t *s, cs_stats *out)
{
    EnterCriticalSection(&s->lock);
    *out = s->st;
    LeaveCriticalSection(&s->lock);
}
void cs_session_get(cs_t *s, cp_session *out)
{
    EnterCriticalSection(&s->lock);
    *out = s->session;
    LeaveCriticalSection(&s->lock);
}
int cs_local_character(cs_t *s)
{
    int c = CP_CHAR_NONE;
    EnterCriticalSection(&s->lock);
    if (s->pid >= 0 && s->pid < CP_MAX_PLAYERS && s->pl[s->pid].used) c = s->pl[s->pid].character;
    LeaveCriticalSection(&s->lock);
    return c;
}
int cs_player_count(cs_t *s)
{
    int n = 0;
    EnterCriticalSection(&s->lock);
    for (int i = 0; i < CP_MAX_PLAYERS; i++) n += s->pl[i].used && s->pl[i].connected;
    LeaveCriticalSection(&s->lock);
    return n;
}
int cs_kick(cs_t *s, int pid)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role == CN_ROLE_HOST && pid > 0 && pid < CP_MAX_PLAYERS && s->pl[pid].used && s->pl[pid].connected) {
        int peer = s->pl[pid].peer;
        cn_log("session: the host removes player %d (%s)", pid, s->pl[pid].name);
        if (peer >= 0) refuse(s, peer, CP_REF_REMOVED);
        player_gone(s, pid, 0, 3);
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

int cs_set_name(cs_t *s, const char *name)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (s->role == CN_ROLE_NONE) {
        copy_name(s->name, name);
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

void cs_set_no_rejoin(cs_t *s, int on)
{
    EnterCriticalSection(&s->lock);
    s->cfg.no_rejoin = on != 0;
    LeaveCriticalSection(&s->lock);
}

int cs_remote_pid(cs_t *s)
{
    int r = -1;
    EnterCriticalSection(&s->lock);
    for (int i = 0; i < CP_MAX_PLAYERS; i++)
        if (i != s->pid && s->pl[i].used && s->pl[i].connected) { r = i; break; }
    LeaveCriticalSection(&s->lock);
    return r;
}
int cs_player(cs_t *s, int pid, cs_player_info *out)
{
    int ok = 0;
    EnterCriticalSection(&s->lock);
    if (pid >= 0 && pid < CP_MAX_PLAYERS && s->pl[pid].used) {
        const pl_t *p = &s->pl[pid];
        memset(out, 0, sizeof *out);
        out->pid = pid, out->used = 1, out->connected = p->connected, out->peer = p->peer;
        memcpy(out->name, p->name, sizeof out->name);
        out->character = p->character, out->ready = p->ready, out->flags = p->flags, out->loaded = p->loaded;
        out->rtt_ms = p->have_rtt ? (int)(p->srtt + 0.5f) : -1;
        out->rx = p->n_rx, out->dup = p->n_dup, out->old = p->n_old, out->stale = p->n_stale, out->gap = p->n_gap;
        ok = 1;
    }
    LeaveCriticalSection(&s->lock);
    return ok;
}

void cs_status(cs_t *s, char *out, size_t size)
{
    static const char *const phases[] = {"none", "lobby", "loading", "playing"};
    EnterCriticalSection(&s->lock);
    if (s->role == CN_ROLE_NONE) {
        snprintf(out, size, "net: no session");
        LeaveCriticalSection(&s->lock);
        return;
    }
    int o = snprintf(out, size, "net: %s via %s, %s, pid %d, epoch %u, session %08x ep%u part%u diff%u max%u", s->role == CN_ROLE_HOST ? "HOST" : "CLIENT", cn_backend_name(s->net),
                     s->joining ? "joining" : phases[s->phase & 3], s->pid, (unsigned)s->epoch, s->session.id, s->session.episode, s->session.part, s->session.difficulty, s->session.max_players);
    for (int i = 0; i < CP_MAX_PLAYERS && o < (int)size - 160; i++) {
        const pl_t *p = &s->pl[i];
        if (!p->used) continue;
        o += snprintf(out + o, size - o, "\n  p%d %-7s \"%s\"%s%s%s%s", i, cp_char_name(p->character), p->name, i == 0 ? " HOST" : "", i == s->pid ? " (me)" : "",
                      p->connected ? "" : " DISCONNECTED", p->ready ? " ready" : "");
        if (p->loaded) o += snprintf(out + o, size - o, " loaded");
        if (i != s->pid && p->connected) {
            o += snprintf(out + o, size - o, " rtt=");
            if (p->have_rtt) o += snprintf(out + o, size - o, "%dms", (int)(p->srtt + 0.5f));
            else o += snprintf(out + o, size - o, "?");
            o += snprintf(out + o, size - o, " rx=%u dup=%u old=%u gap=%u stale=%u", p->n_rx, p->n_dup, p->n_old, p->n_gap, p->n_stale);
        }
    }
    cn_stats ns;
    cn_get_stats(s->net, &ns);
    snprintf(out + o, size - o, "\n  tx state=%u event=%u; rx event=%u hb=%u bad=%u; transport in %u+%u pk %u+%u B, out %u+%u pk %u+%u B", s->st.tx_state, s->st.tx_event, s->st.rx_event, s->st.hb_rx,
             s->st.rx_bad, ns.pk_in[0], ns.pk_in[1], ns.by_in[0], ns.by_in[1], ns.pk_out[0], ns.pk_out[1], ns.by_out[0], ns.by_out[1]);
    LeaveCriticalSection(&s->lock);
}

/* ---------------------------------------------------------------- console */
static void skip_word(const char **p)
{
    while (**p && **p != ' ') (*p)++;
    while (**p == ' ') (*p)++;
}

int cs_command(cs_t *s, const char *line, char *out, size_t size)
{
    char cmd[24] = "";
    const char *arg = line;
    while (*arg == ' ') arg++;
    sscanf(arg, "%23s", cmd);
    skip_word(&arg);
    char err[200];
    if (!strcmp(cmd, "host")) {
        int port = 28410;
        char tr[16] = "enet";
        sscanf(arg, "%d %15s", &port, tr);
        if (cs_host(s, tr, port, 1, err, sizeof err)) snprintf(out, size, "hosting on %s port %d, you are %s; waiting for a player (net status)", tr, port, cp_char_name((unsigned)cs_local_character(s)));
        else snprintf(out, size, "host failed: %s", err);
    } else if (!strcmp(cmd, "join")) {
        char tr[16] = "enet", target[160] = "";
        sscanf(arg, "%159s %15s", target, tr);
        if (!target[0]) snprintf(out, size, "usage: net join <ip[:port]> [transport]");
        else if (cs_join(s, tr, target, 1, err, sizeof err)) snprintf(out, size, "joining %s ...", target);
        else snprintf(out, size, "join failed: %s", err);
    } else if (!strcmp(cmd, "status") || !cmd[0]) {
        cs_status(s, out, size);
    } else if (!strcmp(cmd, "char") || !strcmp(cmd, "pick")) {
        char nm[24] = "";
        sscanf(arg, "%23s", nm);
        int c = cp_char_parse(nm);
        if (c == CP_CHAR_NONE) snprintf(out, size, "usage: net char <claire|moira|barry|natalia>");
        else if (!cs_active(s)) {
            s->cfg.want_char = c;
            snprintf(out, size, "will ask for %s when a session starts", cp_char_name((unsigned)c));
        } else if (cs_pick(s, c)) snprintf(out, size, "asked for %s (net status shows the answer)", cp_char_name((unsigned)c));
        else snprintf(out, size, "cannot pick %s now (lobby only; slot may be taken)", cp_char_name((unsigned)c));
    } else if (!strcmp(cmd, "ready")) {
        int v = 1;
        sscanf(arg, "%d", &v);
        snprintf(out, size, cs_ready(s, v) ? "ready=%d sent" : "cannot change ready now (ready=%d)", v);
    } else if (!strcmp(cmd, "start")) {
        int force = !strncmp(arg, "force", 5);
        if (cs_start(s, force, err, sizeof err)) snprintf(out, size, "START sent");
        else snprintf(out, size, "not started: %s", err);
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "leave")) {
        cs_leave(s, 0);
        snprintf(out, size, "session closed");
    } else if (!strcmp(cmd, "session")) {
        cp_session ss;
        cs_session_get(s, &ss);
        unsigned ep = ss.episode, part = ss.part, diff = ss.difficulty;
        if (sscanf(arg, "%u %u %u", &ep, &part, &diff) >= 1) {
            ss.episode = (uint8_t)ep, ss.part = (uint8_t)part, ss.difficulty = (uint8_t)diff;
            snprintf(out, size, cs_set_session(s, &ss) ? "session: episode %u part %u difficulty %u" : "cannot change the session now (host, lobby)", ep, part, diff);
        } else snprintf(out, size, "session: episode %u part %u difficulty %u (net session <ep> <part> <diff>)", ep, part, diff);
    } else if (!strcmp(cmd, "event") || !strcmp(cmd, "say")) {
        unsigned kind = !strcmp(cmd, "say") ? CP_EV_TEXT : CP_EV_USER;
        if (!strcmp(cmd, "event")) { sscanf(arg, "%u", &kind); skip_word(&arg); }
        snprintf(out, size, cs_send_event(s, kind, arg, (unsigned)strlen(arg) > CP_EVENT_MAX ? CP_EVENT_MAX : (unsigned)strlen(arg)) ? "event %u sent" : "no session (event %u not sent)", kind);
    } else if (!strncmp(cmd, "steam", 5)) {
        return cst_command(s, cmd, arg, out, size);   /* steam, steamhost, steamjoin, steamlobbies, steaminvite, steamallow, steampipe (coop_steam.c) */
    } else
        return 0;
    return 1;
}
