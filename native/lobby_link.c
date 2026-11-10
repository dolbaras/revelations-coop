/*
 * lobby_link.c - the network link of the Revelations 1 co-op lobby (see lobby_link.h). Session notifications run inside cs_poll (lock
 * held, game thread); they only change the model and set flags. Session calls that could re-enter the session (leave, start) are done by
 * lk_poll after cs_poll returns.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>

#include "coop_proto.h"
#include "coop_session.h"
#include "lobby_link.h"

#define LK_MOD_VERSION "rev1-stage4"

struct lk_t {
    CRITICAL_SECTION lock;   /* every public call holds it: the console thread, the game thread and the lobby glue share one link (recursive: the session's notifications come back into the link) */
    l1_t *lobby;       /* optional */
    cs_t *s;
    const char *transport;
    int threaded;
    int manual_start;
    int mode;          /* L1N_OFF when stopped, else the mode started (HOST or JOIN) */
    int state;         /* the L1NS_* last reported to the model (the model clears the ready flags when it leaves CONNECTED) */
    int sent_ready;    /* this player's ready flag as last given to the session */
    int cmd_ready;     /* ready set by lk_set_ready (console) */
    int own_table_ready;   /* this player's ready flag as the lobby table showed it last (a 1 -> 0 that we did not ask for = the host reset it) */
    int role_wish;     /* host: what this PC's human should play (used when no lobby is attached) */
    int drop;          /* a notification ended the link: close the session after cs_poll returns */
    void (*tap)(void *user, const cs_note *n);   /* lk_set_note_tap */
    void *tap_user;
    int started;       /* the START notification came: roles / settings final */
    int load_begins, gos, last_go_epoch;
    uint32_t peer_load_area;      /* rev1 live 8: the area id the partner sent with its last LOAD_BEGIN (0 = none / not known) and that epoch */
    int peer_load_epoch;
    lk_event_cb ev_cb[LK_EVENT_CBS];   /* receivers of the other player's events (CS_N_EVENT) */
    void *ev_user[LK_EVENT_CBS];
    int n_ev;
    lk_settings set;   /* the settings at the start (final) */
    char why[120];     /* the session's reason for the last failure */
    char own_addr[48]; /* host: this PC's "ip:port" as found at the start */
    /* lobby-must: the reason of the last failure / partner event and the counters of the in-game strip (lk_info) */
    int reason;
    char ra1[64], ra2[24];
    char target[64];   /* join: the address typed (the "no answer" text names it) */
    char mod[24];      /* this PC's mod version (the version texts name it) */
    char last_peer[CP_NAME_MAX + 1];
    int peer_gone, peer_gone_kind, ended, ended_reason;
    int silent_s, wait_s;
    uint32_t silent_at, wait_at;
    char wait_name[CP_NAME_MAX + 1];
    char name_wish[CP_NAME_MAX + 1];   /* lk_set_name: taken by the session at the next start */
};

static void sync_model(lk_t *k);

/* the model's state, changed only when it changes (set_state(WAIT) twice would clear the ready flags twice) */
static void set_state(lk_t *k, int st)
{
    if (k->state == st) return;
    k->state = st;
    if (k->lobby) l1_net_set_state(k->lobby, st);
}

static int local_slot(lk_t *k)
{
    int c = cs_local_character(k->s);
    return c >= 0 && c <= 3 ? CP_CHAR_SLOT(c) : -1;
}

/* the other player's ready flag, from the session's lobby table (only meaningful while CONNECTED) */
static int peer_ready(lk_t *k)
{
    int pid = k->state == L1NS_CONNECTED ? cs_remote_pid(k->s) : -1;
    cs_player_info pi;
    return pid >= 0 && cs_player(k->s, pid, &pi) && pi.ready != 0;
}

/* the partner's ready flag and name for the model (the name is kept after he left, for the texts that name him) */
static void sync_peer(lk_t *k)
{
    int pid = cs_remote_pid(k->s);
    cs_player_info pi;
    memset(&pi, 0, sizeof pi);
    if (pid >= 0 && cs_player(k->s, pid, &pi) && pi.name[0]) snprintf(k->last_peer, sizeof k->last_peer, "%s", pi.name);
    else pi.name[0] = 0;
    if (k->lobby) {
        l1_net_set_peer_ready(k->lobby, peer_ready(k));
        l1_net_set_peer(k->lobby, pid >= 0 ? pi.name : "");
    }
}

static void set_reason(lk_t *k, int reason, const char *a1, const char *a2)
{
    k->reason = reason;
    snprintf(k->ra1, sizeof k->ra1, "%s", a1 ? a1 : "");
    snprintf(k->ra2, sizeof k->ra2, "%s", a2 ? a2 : "");
    if (k->lobby) l1_net_set_reason(k->lobby, reason, k->ra1, k->ra2);
}

/* the session's refusal code -> the reason the player reads (codes without a text of their own: the generic failure) */
static int refusal_reason(int code, const char *peer_mod)
{
    switch (code) {
    case CP_REF_VERSION: return L1RS_PROTO;
    case CP_REF_BUILD: return L1RS_BUILD;
    case CP_REF_FULL: return L1RS_FULL;
    case CP_REF_STARTED: return L1RS_STARTED;
    case CP_REF_MODVER: return peer_mod && peer_mod[0] ? L1RS_MODVER : L1RS_MODVER_OLD;
    case CP_REF_REMOVED: return L1RS_REMOVED;
    case 0: return L1RS_NOANSWER;   /* no answer in time (the join deadline) or the transport gave up */
    default: return L1RS_FAILED;
    }
}

static void read_settings(lk_t *k, lk_settings *o)
{
    cp_session ss;
    cs_session_get(k->s, &ss);
    o->episode = ss.episode, o->difficulty = ss.difficulty, o->flags = ss.flags, o->area_id = ss.area_id, o->seed = ss.seed;
}

static void on_note(void *user, const cs_note *n)
{
    lk_t *k = (lk_t *)user;
    if (k->tap) k->tap(k->tap_user, n);
    if (k->mode == L1N_OFF) return;
    switch (n->what) {
    case CS_N_CONNECTED:   /* client: the host accepted us */
        set_state(k, L1NS_CONNECTED);
        break;
    case CS_N_REFUSED: {   /* client: refused, or no answer in time */
        snprintf(k->why, sizeof k->why, "%s", n->text ? n->text : "refused");
        int r = refusal_reason(n->a, n->peer_mod);
        set_state(k, L1NS_ERROR);
        set_reason(k, r, r == L1RS_MODVER ? n->peer_mod : r == L1RS_NOANSWER ? k->target : "", k->mod);
        k->drop = 1;
        break;
    }
    case CS_N_LOST: {      /* client: the host is gone (a = 1 the host left, 2 silent for the limit, 3 removed us, 0 the link) */
        snprintf(k->why, sizeof k->why, "%s", n->text ? n->text : "lost");
        int r = n->a == 1 ? L1RS_HOST_ENDED : n->a == 3 ? L1RS_REMOVED : L1RS_HOST_LOST;
        set_state(k, L1NS_ERROR);
        set_reason(k, r, k->last_peer, "");
        if (k->started || r == L1RS_REMOVED) k->ended++, k->ended_reason = r;
        k->silent_s = 0, k->wait_s = 0;
        k->drop = 1;
        break;
    }
    case CS_N_PEER_JOIN:   /* host: a player is in */
        if (k->mode == L1N_HOST) set_state(k, L1NS_CONNECTED);
        if (k->mode == L1N_HOST && k->reason >= L1RS_PARTNER_LEFT) set_reason(k, L1RS_NONE, "", "");
        break;
    case CS_N_PEER_LEAVE:  /* host: the player is gone (the session has already dropped him) */
        if (k->mode == L1N_HOST && cs_remote_pid(k->s) < 0) set_state(k, L1NS_WAIT);
        if (k->mode == L1N_HOST && n->b != 2) {   /* b = 2: his old link was replaced by a new one (a quick rejoin): nobody left */
            int r = n->b == 3 ? L1RS_NONE : (n->a || n->b == 1) ? L1RS_PARTNER_LOST : L1RS_PARTNER_LEFT;   /* b = 3: this host removed him */
            if (n->text && n->text[0]) snprintf(k->last_peer, sizeof k->last_peer, "%s", n->text);
            set_reason(k, r, k->last_peer, "");
            if (k->started && r != L1RS_NONE) k->peer_gone++, k->peer_gone_kind = r;
            k->silent_s = 0, k->wait_s = 0;
        }
        break;
    case CS_N_START:       /* the host started the game: the roles and settings are final */
        k->started = 1;
        read_settings(k, &k->set);
        if (k->lobby) l1_net_set_started(k->lobby, 1);
        break;
    case CS_N_SILENT:      /* nothing from the partner for a = seconds (0 = heard again) */
        k->silent_s = n->a, k->silent_at = cs_now(k->s);
        if (n->text && n->text[0]) snprintf(k->last_peer, sizeof k->last_peer, "%s", n->text);
        break;
    case CS_N_WAIT:        /* this PC has loaded and waits for the partner at the barrier (a = seconds) */
        k->wait_s = n->a, k->wait_at = cs_now(k->s);
        snprintf(k->wait_name, sizeof k->wait_name, "%s", n->text ? n->text : "");
        break;
    case CS_N_LOAD_BEGIN:
        k->load_begins++;
        if (n->pid != cs_pid(k->s)) k->peer_load_area = (uint32_t)n->b, k->peer_load_epoch = n->a;   /* rev1 live 8: the partner's next level (the cross-level guard) */
        break;
    case CS_N_GO:
        k->wait_s = 0;
        k->gos++, k->last_go_epoch = n->a;
        break;
    case CS_N_EVENT:       /* an event of the other player (world sync, barrier, restart): handed on; the callbacks run with the session lock held and must only queue */
        for (int i = 0; n->ev && i < k->n_ev; i++) k->ev_cb[i](k->ev_user[i], n->ev);
        break;
    case CS_N_NOTICE:      /* a request of ours was not honoured (a pick, ...): the text is kept for the log */
        snprintf(k->why, sizeof k->why, "%s", n->text ? n->text : "");
        break;
    default:
        break;
    }
    if (n->what == CS_N_LOBBY || n->what == CS_N_PEER_JOIN || n->what == CS_N_PEER_LEAVE || n->what == CS_N_CONNECTED) sync_peer(k);
}

/* this PC's LAN address for the Host row: the address the OS uses for the default route (a UDP connect sends nothing) */
static void lan_address(char *out, size_t size, int port)
{
    out[0] = 0;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) return;
    SOCKET so = socket(AF_INET, SOCK_DGRAM, 0);
    if (so != INVALID_SOCKET) {
        struct sockaddr_in to, me;
        int n = sizeof me;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET, to.sin_port = htons(53), to.sin_addr.s_addr = inet_addr("8.8.8.8");
        if (connect(so, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(so, (struct sockaddr *)&me, &n) == 0) {
            const unsigned char *b = (const unsigned char *)&me.sin_addr;
            if (b[0] && me.sin_addr.s_addr != 0) snprintf(out, size, "%u.%u.%u.%u:%d", b[0], b[1], b[2], b[3], port);
        }
        closesocket(so);
    }
    WSACleanup();
}

lk_t *lk_new_cfg(l1_t *lobby, const char *transport, int threaded, const lk_config *cfg)
{
    lk_t *k = (lk_t *)calloc(1, sizeof *k);
    if (!k) return NULL;
    cs_config c;
    memset(&c, 0, sizeof c);
    c.name = cfg && cfg->name ? cfg->name : "Player";
    c.want_char = CP_CHAR_NONE;          /* the roles are assigned by the link (sync_model): the host's pick, the other slot for the joined player */
    c.mod_version = cfg && cfg->mod_version ? cfg->mod_version : LK_MOD_VERSION;
    c.is_bot = cfg ? cfg->is_bot != 0 : 0;
    c.no_rejoin = cfg ? cfg->no_rejoin != 0 : 0;
    if (cfg) memcpy(c.exe_tag, cfg->exe_tag, sizeof c.exe_tag), c.clock = cfg->clock, c.clock_user = cfg->clock_user, k->manual_start = cfg->manual_start != 0;
    c.on_note = on_note;
    c.user = k;
    InitializeCriticalSection(&k->lock);
    k->s = cs_new(&c);
    k->lobby = lobby;
    k->transport = transport;
    k->threaded = threaded;
    k->mode = L1N_OFF;
    k->state = L1NS_IDLE;
    k->role_wish = L1_LEAD;
    snprintf(k->mod, sizeof k->mod, "%s", c.mod_version);
    if (!k->s) { DeleteCriticalSection(&k->lock); free(k); return NULL; }
    return k;
}

lk_t *lk_new(l1_t *lobby, const char *transport, int threaded) { return lk_new_cfg(lobby, transport, threaded, NULL); }

/* round 2, the Steam way: the transport of the NEXT start (cs_host / cs_join take it per start; the link object stays, so every module that registered on it
 * keeps its registration) */
void lk_set_transport(lk_t *k, const char *transport)
{
    if (k && transport && transport[0]) k->transport = transport;
}
const char *lk_transport(const lk_t *k) { return k && k->transport ? k->transport : ""; }

void lk_free(lk_t *k)
{
    if (!k) return;
    EnterCriticalSection(&k->lock);
    cs_leave(k->s, 0);
    cs_free(k->s);
    LeaveCriticalSection(&k->lock);
    DeleteCriticalSection(&k->lock);
    free(k);
}

/* attach / detach the lobby; a newly attached model gets what the link is doing */
static void set_lobby_u(lk_t *k, l1_t *lobby)
{
    k->lobby = lobby;
    if (!lobby) return;
    if (k->mode != L1N_OFF) l1_net_adopt(lobby, k->mode, k->mode == L1N_JOIN && k->target[0] ? k->target : NULL);   /* lm1 live: a console join showed "type IP:port" */
    l1_net_set_state(lobby, k->state);
    if (k->reason) l1_net_set_reason(lobby, k->reason, k->ra1, k->ra2);
    sync_model(k);
    sync_peer(k);
    if (k->started) l1_net_set_started(lobby, 1);
}

static int start_u(lk_t *k, int mode, const char *target, int port, char *own_addr, size_t oa_size, char *err, size_t esz)
{
    if (own_addr && oa_size) own_addr[0] = 0;
    if (err && esz) err[0] = 0;
    cs_leave(k->s, 0);                   /* a previous attempt (or a dead one) is closed first */
    if (k->name_wish[0]) cs_set_name(k->s, k->name_wish);
    k->drop = 0, k->sent_ready = 0, k->cmd_ready = 0, k->own_table_ready = 0, k->why[0] = 0, k->started = 0;
    k->load_begins = k->gos = k->last_go_epoch = 0;
    k->peer_load_area = 0, k->peer_load_epoch = 0;
    memset(&k->set, 0, sizeof k->set);
    k->own_addr[0] = 0;
    k->reason = 0, k->ra1[0] = k->ra2[0] = 0, k->silent_s = 0, k->wait_s = 0, k->last_peer[0] = 0;
    snprintf(k->target, sizeof k->target, "%s", mode == L1N_JOIN && target ? target : "");
    k->mode = L1N_OFF;
    if (mode == L1N_HOST) {
        if (!cs_host(k->s, k->transport, port, k->threaded, err, esz)) {
            set_state(k, L1NS_ERROR), snprintf(k->why, sizeof k->why, "%s", err && err[0] ? err : "host failed");
            return 0;
        }
        k->mode = L1N_HOST;
        if (k->lobby) l1_net_adopt(k->lobby, L1N_HOST, NULL);
        lan_address(k->own_addr, sizeof k->own_addr, port);
        if (own_addr && oa_size) snprintf(own_addr, oa_size, "%s", k->own_addr);
        set_state(k, L1NS_WAIT);
        sync_model(k);                   /* the host's role from the model (or the wish) is the first pick */
        return 1;
    }
    if (mode == L1N_JOIN) {
        if (!target || !target[0]) {
            set_state(k, L1NS_ERROR), snprintf(k->why, sizeof k->why, "no address");
            if (err && esz) snprintf(err, esz, "no address");
            return 0;
        }
        if (!cs_join(k->s, k->transport, target, k->threaded, err, esz)) {
            set_state(k, L1NS_ERROR), snprintf(k->why, sizeof k->why, "%s", err && err[0] ? err : "join failed");
            return 0;
        }
        k->mode = L1N_JOIN;
        if (k->lobby) l1_net_adopt(k->lobby, L1N_JOIN, target);
        set_state(k, L1NS_CONNECTING);
        return 1;
    }
    return 0;
}

static void stop_u(lk_t *k)
{
    cs_leave(k->s, 0);
    k->own_addr[0] = 0;
    k->mode = L1N_OFF, k->drop = 0, k->sent_ready = 0, k->cmd_ready = 0, k->own_table_ready = 0, k->started = 0;
    set_state(k, L1NS_IDLE);
    if (k->lobby) l1_net_set_peer_ready(k->lobby, 0);
}

/* the roles: the host's wish (the model's role, else the console's) is applied to the lobby table; a joined player shows what the host gave him */
static void sync_model(lk_t *k)
{
    if (k->mode == L1N_OFF) return;
    int slot = local_slot(k);
    if (k->mode == L1N_HOST) {
        if (!k->started && cs_phase(k->s) == CP_PHASE_LOBBY) {
            int wish = k->role_wish;
            if (k->lobby) {
                l1_settings st;
                l1_settings_get(k->lobby, &st);
                wish = st.role == L1_PARTNER ? L1_PARTNER : L1_LEAD;
                k->role_wish = wish;
            }
            if (slot != wish) cs_assign_roles(k->s, wish);
        }
    } else if (k->lobby && k->state == L1NS_CONNECTED) {
        l1_net_set_assigned(k->lobby, slot >= 0 ? slot : -1);
    }
}

static void set_role_u(lk_t *k, int role)
{
    k->role_wish = role == L1_PARTNER ? L1_PARTNER : L1_LEAD;
    if (k->lobby) l1_net_set_role(k->lobby, k->role_wish);
    if (k->mode == L1N_HOST) sync_model(k);
}

static void set_ready_u(lk_t *k, int ready)
{
    k->cmd_ready = ready != 0;
    if (k->lobby) l1_net_set_ready(k->lobby, ready);
}

static int set_settings_u(lk_t *k, const lk_settings *s)
{
    if (k->mode != L1N_HOST || !s) return 0;
    cp_session ss;
    cs_session_get(k->s, &ss);
    if (s->episode > 0) ss.episode = (uint8_t)s->episode;
    if (s->difficulty >= 0) ss.difficulty = (uint8_t)s->difficulty;
    ss.flags = (uint8_t)s->flags;
    ss.area_id = s->area_id;
    if (s->seed) ss.seed = s->seed;
    return cs_set_session(k->s, &ss);
}

static int start_session_u(lk_t *k, int force, char *err, size_t esz)
{
    char local[120];
    if (!err || !esz) err = local, esz = sizeof local;
    err[0] = 0;
    if (k->mode != L1N_HOST) { snprintf(err, esz, "only the host can start"); return 0; }
    return cs_start(k->s, force, err, esz);
}

static void poll_u(lk_t *k)
{
    if (k->mode == L1N_OFF) return;
    cs_poll(k->s);
    if (k->drop) {
        k->drop = 0;
        cs_leave(k->s, 0);
    }
    int lobby_phase = !k->started && cs_phase(k->s) == CP_PHASE_LOBBY;
    sync_model(k);
    if (lobby_phase && k->state == L1NS_CONNECTED) {
        /* the host reset our ready flag (the roles changed): the player confirms again */
        cs_player_info me;
        int own = cs_player(k->s, cs_pid(k->s), &me) && me.ready;
        if (k->own_table_ready && !own && k->sent_ready) {
            k->sent_ready = 0, k->cmd_ready = 0;
            if (k->lobby) l1_net_set_ready(k->lobby, 0);
        }
        k->own_table_ready = own;
    } else
        k->own_table_ready = 0;
    /* the model drops its ready flag when the link leaves CONNECTED, so a 1 that was sent is taken back with a 0 here (the session ignores it when it is gone) */
    int want = k->state == L1NS_CONNECTED && ((k->lobby && l1_net_ready(k->lobby)) || k->cmd_ready);
    if (want != k->sent_ready) {
        cs_ready(k->s, want);
        k->sent_ready = want;
    }
    sync_peer(k);
    /* both ready: the host starts the session (the joined player cannot) */
    if (!k->manual_start && k->mode == L1N_HOST && lobby_phase && k->state == L1NS_CONNECTED && want && peer_ready(k)) {
        char err[120];
        cs_start(k->s, 0, err, sizeof err);
    }
}

int lk_active(const lk_t *k)
{
    return k->state == L1NS_WAIT || k->state == L1NS_CONNECTING || k->state == L1NS_CONNECTED;
}
int lk_state(const lk_t *k) { return k->state; }
int lk_mode(const lk_t *k) { return k->mode; }
const char *lk_last_reason(const lk_t *k) { return k->why; }
cs_t *lk_session(lk_t *k) { return k->s; }


static void get_info_u(lk_t *k, lk_info *o)
{
    memset(o, 0, sizeof *o);
    o->role = o->remote_role = -1;
    o->host = k->mode == L1N_HOST;
    if (k->mode == L1N_OFF) return;
    o->role = local_slot(k);
    int rp = cs_remote_pid(k->s);
    cs_player_info pi;
    if (rp >= 0 && cs_player(k->s, rp, &pi)) {
        o->remote_role = pi.character <= 3 ? CP_CHAR_SLOT(pi.character) : -1;
        snprintf(o->peer_name, sizeof o->peer_name, "%s", pi.name);
    }
    o->started = k->started;
    o->reason = k->reason;
    snprintf(o->ra1, sizeof o->ra1, "%s", k->ra1), snprintf(o->ra2, sizeof o->ra2, "%s", k->ra2);
    snprintf(o->last_peer, sizeof o->last_peer, "%s", k->last_peer);
    o->peer_gone = k->peer_gone, o->peer_gone_kind = k->peer_gone_kind, o->ended = k->ended, o->ended_reason = k->ended_reason;
    uint32_t now = cs_now(k->s);
    o->silent_s = k->silent_s, o->silent_age_ms = now - k->silent_at;
    o->wait_s = k->wait_s, o->wait_age_ms = now - k->wait_at;
    snprintf(o->wait_name, sizeof o->wait_name, "%s", k->wait_name);
    o->phase = cs_phase(k->s), o->epoch = (int)cs_epoch(k->s);
    o->load_begins = k->load_begins, o->gos = k->gos, o->last_go_epoch = k->last_go_epoch;
    o->peer_load_area = k->peer_load_area, o->peer_load_epoch = k->peer_load_epoch;
    if (k->state == L1NS_CONNECTED) o->ready = k->sent_ready, o->peer_ready = peer_ready(k);
    if (k->started) o->set = k->set;
    else read_settings(k, &o->set);
}

static void status_u(lk_t *k, char *out, size_t size)
{
    static const char *const st[] = {"idle", "waiting for a player", "connecting", "connected", "error"};
    lk_info in;
    get_info_u(k, &in);
    int o = snprintf(out, size, "link: %s, %s%s%s%s", k->mode == L1N_HOST ? "host" : k->mode == L1N_JOIN ? "join" : "off", st[k->state >= 0 && k->state <= 4 ? k->state : 0],
                     k->why[0] ? " (" : "", k->why, k->why[0] ? ")" : "");
    if (o < 0 || (size_t)o >= size || k->mode == L1N_OFF) return;
    o += snprintf(out + o, size - (size_t)o, "\nroles: you %s, other %s%s%s%s; %s; ready %d/%d", in.role == 0 ? "lead" : in.role == 1 ? "partner" : "?", in.remote_role == 0 ? "lead" : in.remote_role == 1 ? "partner" : "-",
                  in.peer_name[0] ? " (" : "", in.peer_name, in.peer_name[0] ? ")" : "", in.started ? "STARTED" : "not started", k->sent_ready, k->state == L1NS_CONNECTED ? peer_ready(k) : 0);
    if (o > 0 && (size_t)o < size)
        o += snprintf(out + o, size - (size_t)o, "\nsettings: episode %d difficulty %d flags %d area %u seed %08x; loads %d go %d (last epoch %d)", in.set.episode, in.set.difficulty, in.set.flags,
                      (unsigned)in.set.area_id, (unsigned)in.set.seed, in.load_begins, in.gos, in.last_go_epoch);
    if (o > 0 && (size_t)o < size) {
        char sb[1500];
        cs_status(k->s, sb, sizeof sb);
        snprintf(out + o, size - (size_t)o, "\n%s", sb);
    }
}

/* ---------------------------------------------------------------- the public calls (all under the link's lock) */
#define LOCKED(k, stmt) do { EnterCriticalSection(&(k)->lock); stmt; LeaveCriticalSection(&(k)->lock); } while (0)

void lk_set_lobby(lk_t *k, l1_t *lobby) { LOCKED(k, set_lobby_u(k, lobby)); }

int lk_start(lk_t *k, int mode, const char *target, int port, char *own_addr, size_t oa_size, char *err, size_t esz)
{
    int r;
    LOCKED(k, r = start_u(k, mode, target, port, own_addr, oa_size, err, esz));
    return r;
}

void lk_stop(lk_t *k) { LOCKED(k, stop_u(k)); }
void lk_poll(lk_t *k) { LOCKED(k, poll_u(k)); }
void lk_set_note_tap(lk_t *k, void (*fn)(void *user, const cs_note *n), void *user)
{
    if (!k) return;
    EnterCriticalSection(&k->lock);
    k->tap = fn, k->tap_user = user;
    LeaveCriticalSection(&k->lock);
}
void lk_set_role(lk_t *k, int role) { LOCKED(k, set_role_u(k, role)); }
int lk_force_roles(lk_t *k, int host_role)
{
    int r = -1;
    if (!k) return -1;
    EnterCriticalSection(&k->lock);
    if (k->mode == L1N_HOST && k->started && (host_role == L1_LEAD || host_role == L1_PARTNER)) {
        r = cs_force_roles(k->s, host_role);
        if (r >= 0) {
            k->role_wish = host_role;
            if (k->lobby) l1_net_set_role(k->lobby, host_role);
        }
    }
    LeaveCriticalSection(&k->lock);
    return r;
}
void lk_set_ready(lk_t *k, int ready) { LOCKED(k, set_ready_u(k, ready)); }

int lk_set_settings(lk_t *k, const lk_settings *s)
{
    int r;
    LOCKED(k, r = set_settings_u(k, s));
    return r;
}

int lk_start_session(lk_t *k, int force, char *err, size_t esz)
{
    int r;
    LOCKED(k, r = start_session_u(k, force, err, esz));
    return r;
}

int lk_add_event_cb(lk_t *k, lk_event_cb cb, void *user)
{
    int r = 0;
    EnterCriticalSection(&k->lock);
    for (int i = 0; i < k->n_ev; i++)
        if (k->ev_cb[i] == cb && k->ev_user[i] == user) r = 1;   /* already registered */
    if (!r && cb && k->n_ev < LK_EVENT_CBS) k->ev_cb[k->n_ev] = cb, k->ev_user[k->n_ev] = user, k->n_ev++, r = 1;
    LeaveCriticalSection(&k->lock);
    return r;
}

void lk_load_begin(lk_t *k, uint32_t area_id) { LOCKED(k, cs_load_begin(k->s, area_id)); }
void lk_load_done(lk_t *k) { LOCKED(k, cs_load_done(k->s)); }
void lk_get_info(lk_t *k, lk_info *o) { LOCKED(k, get_info_u(k, o)); }

static int kick_u(lk_t *k)
{
    if (k->mode != L1N_HOST || k->started || cs_phase(k->s) != CP_PHASE_LOBBY) return 0;
    int pid = cs_remote_pid(k->s);
    return pid > 0 && cs_kick(k->s, pid);
}
int lk_kick(lk_t *k)
{
    int r;
    LOCKED(k, r = kick_u(k));
    return r;
}
void lk_set_name(lk_t *k, const char *name) { LOCKED(k, snprintf(k->name_wish, sizeof k->name_wish, "%s", name ? name : "")); }
void lk_set_no_rejoin(lk_t *k, int on) { LOCKED(k, cs_set_no_rejoin(k->s, on)); }
void lk_status(lk_t *k, char *out, size_t size) { LOCKED(k, status_u(k, out, size)); }

void lk_set_manual_start(lk_t *k, int manual) { LOCKED(k, k->manual_start = manual != 0); }
int lk_manual_start(lk_t *k)
{
    int r;
    LOCKED(k, r = k->manual_start);
    return r;
}

void lk_own_addr(lk_t *k, char *out, size_t size)
{
    LOCKED(k, snprintf(out, size, "%s", k->own_addr));
}
