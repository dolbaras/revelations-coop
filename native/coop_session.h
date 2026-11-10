/*
 * Co-op session layer: handshake with a version gate, lobby (character picks, ready), session settings, the level-change
 * barrier, 20 Hz player state with interpolation, events, heartbeat/RTT. Game independent: the proxy's glue (coop_game.inc)
 * and the headless bot / unit tests use the same code. Wire format: coop_proto.h, docs/protocol.md.
 *
 * Threading: every function takes the session lock (recursive), so the console thread may call host/join/pick/... while the
 * game thread calls cs_poll / cs_submit_state / cs_remote_sample. Notifications are delivered from cs_poll (and from the
 * calls above when they change local state) with the lock held; they must not block.
 */
#ifndef COOP_SESSION_H
#define COOP_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "coop_net.h"
#include "coop_proto.h"

typedef struct cs_t cs_t;

enum cs_notify {
    CS_N_CONNECTED = 1,    /* client: the host accepted us (pid = our pid) */
    CS_N_REFUSED,          /* client: refused or no answer (text, a = cp_refuse_code or 0, peer_mod = the host's mod version when it sent one);
                              a = CP_REF_MODVER also when this client left a host whose WELCOME names another mod version ("" = a host older than that field) */
    CS_N_LOST,             /* the connection to the host is gone / session ended (text; a = 1 the host left on purpose, 2 the host was silent for the limit, 3 the host removed us, 0 transport) */
    CS_N_PEER_JOIN,        /* pid joined the lobby / reconnected */
    CS_N_PEER_LEAVE,       /* pid left or was lost (a = 1 when lost without goodbye; b = 1 given up after the silence limit, 2 its old link was replaced by a new
                              connection of the same player, 3 removed by this host: cs_kick) */
    CS_N_LOBBY,            /* lobby table changed */
    CS_N_SESSION,          /* session settings changed */
    CS_N_START,            /* the host started the game: characters are final */
    CS_N_LOAD_BEGIN,       /* a level load started somewhere (pid = who, a = epoch, b = area id): flip puppets */
    CS_N_GO,               /* barrier open: every player finished loading (a = epoch) */
    CS_N_EVENT,            /* ev = a game event from another player */
    CS_N_NOTICE,           /* host notice for us (text) */
    CS_N_WAIT,             /* stage 6i: this machine has finished loading and still waits for the barrier (a = seconds waited, b = the barrier limit in seconds (host) or 0, pid = who is awaited, text = his name) */
    CS_N_SILENT            /* nothing at all has come from pid for a while (its game paused / in the background, a long load or a stall; the link itself is up):
                              a = seconds of silence (first after 8 s, then every 10 s), b = the limit in seconds after which the session gives up on it, text = name;
                              a = 0: pid is heard again (b = how long it was silent) */
};

typedef struct {
    int what, pid, a, b;
    const char *text;
    const cp_event *ev;
    const char *peer_mod;  /* CS_N_REFUSED: the host's mod version ("" unknown); NULL otherwise */
} cs_note;

typedef struct {
    const char *name;                 /* player name shown in the lobby */
    int want_char;                    /* CP_CHAR_* or CP_CHAR_NONE */
    uint8_t exe_tag[8];               /* first 8 bytes of the exe SHA-256; zeros = skip the build check (bot) */
    const char *mod_version;
    int is_bot;
    uint32_t (*clock)(void *);        /* NULL = real monotonic clock (tests pass a virtual one) */
    void *clock_user;
    void (*on_note)(void *user, const cs_note *n);
    void *user;
    int interp_delay_ms;              /* 0 = default 100 */
    int no_rejoin;                    /* host: after the START nobody may come back, not even a known name (REFUSE STARTED; Rev1 0.1.0 product rule, the lab keeps the rejoin) */
} cs_config;

typedef struct {
    uint32_t tx_state, rx_state, rx_dup, rx_old, rx_stale_epoch, rx_gap;   /* summed over remote players */
    uint32_t tx_event, rx_event, rx_bad, hb_rx;
} cs_stats;

typedef struct {
    int pid, used, connected, peer;
    char name[CP_NAME_MAX + 1];
    uint8_t character, ready, flags, loaded;
    int rtt_ms;                       /* -1 unknown */
    uint32_t rx, dup, old, stale, gap;
} cs_player_info;

/* result of cs_remote_sample */
enum { CS_SAMPLE_NONE = 0, CS_SAMPLE_INTERP = 1, CS_SAMPLE_EXTRAP = 2, CS_SAMPLE_STALE = 3 };

cs_t *cs_new(const cs_config *cfg);
void cs_free(cs_t *s);

int cs_host(cs_t *s, const char *transport, int port, int threaded, char *err, size_t esz);
int cs_join(cs_t *s, const char *transport, const char *target, int threaded, char *err, size_t esz);
void cs_leave(cs_t *s, int reason);                    /* LEAVE + close the transport; the session object stays usable */

void cs_poll(cs_t *s);                                 /* drain the transport, run timers (heartbeat, deadlines, barrier timeout) */

/* lobby (the host applies at once, clients ask the host and see the answer in the next LOBBY_STATE) */
int cs_pick(cs_t *s, int character);
int cs_ready(cs_t *s, int ready);
int cs_assign_roles(cs_t *s, int host_slot);          /* host only: the host plays slot 0 (lead) or 1 (partner), the other player the other one (Revelations 1) */
/* rev1 live 16 (Continue keeps the roles of the co-op save): the same after the START, only before the first level of the session (phase LOADING of epoch 1,
 * nobody loaded): the new table goes to the other player in order on the control channel, before anything the host sends after it. 1 = changed, 0 = already
 * so, -1 = refused (not the host, a level was loaded, a bad slot) */
int cs_force_roles(cs_t *s, int host_slot);
int cs_set_session(cs_t *s, const cp_session *ss);     /* host only */
int cs_start(cs_t *s, int force, char *err, size_t esz);   /* host only: needs everybody ready unless force */
int cs_kick(cs_t *s, int pid);                         /* host only: REFUSE(REMOVED) to that player and drop him (PEER_LEAVE b=3); 1 = done */
int cs_set_name(cs_t *s, const char *name);            /* the name the other player sees, only while no session runs; 1 = taken */
void cs_set_no_rejoin(cs_t *s, int on);                /* cs_config.no_rejoin at run time (the lab console) */

/* load barrier: call load_begin when this machine starts a level load (or at once after START), load_done when it is playable again */
void cs_load_begin(cs_t *s, uint32_t area_id);
void cs_load_done(cs_t *s);

/* player state: the caller submits its character's state as often as it likes; at most one packet per 45 ms goes out (a TELEPORT flag
 * sends at once). Returns 1 when a packet was sent. Nothing is sent outside PLAYING. */
int cs_submit_state(cs_t *s, const cp_state *st);
/* interpolated state of another player, shown ~100 ms in the past; now_ms on the session clock (cs_now) */
int cs_remote_sample(cs_t *s, int pid, uint32_t now_ms, cp_state *out);

int cs_send_event(cs_t *s, unsigned kind, const void *body, unsigned len);   /* reliable, to everybody */

/* stage 6 world records: unreliable + sequenced (newest wins), only while PLAYING, body <= CP_WORLD_MAX. The receiver polls its inbox from the game thread (a ring of the newest 64). */
int cs_send_world(cs_t *s, unsigned kind, const void *body, unsigned len);
int cs_recv_world(cs_t *s, cp_world *out);                  /* 1 = one message taken */
void cs_world_stats(cs_t *s, uint32_t *tx, uint32_t *rx, uint32_t *drop);

/* Revelations 1 puppet (version 3): the sender's pad records (CP_PAD), unreliable + sequenced on the state channel, only while PLAYING. cs_send_pad sets pid, epoch and masks the flags;
 * the caller (puppet1.c) decides the rate and fills seq, t_ms, count, dt, rec. The receiver polls its inbox from the game thread (a ring of the newest 64, old epochs dropped). */
int cs_send_pad(cs_t *s, const cp_pad *p);
int cs_recv_pad(cs_t *s, cp_pad *out);                      /* 1 = one message taken */
void cs_pad_stats(cs_t *s, uint32_t *tx, uint32_t *rx, uint32_t *drop);

/* information */
int cs_active(cs_t *s);                                /* a session exists (host or joined) */
int cs_is_host(cs_t *s);
int cs_pid(cs_t *s);
int cs_phase(cs_t *s);                                 /* enum cp_phase */
unsigned cs_epoch(cs_t *s);
uint32_t cs_now(cs_t *s);                              /* the session clock (milliseconds) */
int cs_local_character(cs_t *s);
int cs_player_count(cs_t *s);
int cs_player(cs_t *s, int pid, cs_player_info *out);  /* 1 = pid used */
int cs_remote_pid(cs_t *s);                            /* the first other connected player or -1 (two-player convenience) */
void cs_session_get(cs_t *s, cp_session *out);
void cs_get_stats(cs_t *s, cs_stats *out);
cn_t *cs_transport(cs_t *s);
void cs_status(cs_t *s, char *out, size_t size);

/* generic console commands (host, join, status, char, ready, start, stop, session, event, say); returns 0 when the command is not ours */
int cs_command(cs_t *s, const char *line, char *out, size_t size);

#endif
