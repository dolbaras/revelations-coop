/*
 * lobby_link.h - the network link of the Revelations 1 co-op lobby (stage 4). It owns one coop session (coop_session.h), turns the
 * session's notifications into the lobby model's state (l1_net_set_state / l1_net_set_peer_ready), sends this player's ready flag
 * (l1_net_ready) into the session, gives the two humans their roles (lead = unit slot 0, partner = unit slot 1), passes the host's session
 * settings to the joined player, starts the session when both are ready and keeps the level-load barrier counters for the game glue.
 * Every public call takes the link's lock, so the console thread and the game thread may both use one link. Game independent: menu.c / net1_glue.c drive it on the game thread (lk_poll once a frame); the offline tests (tests/test_lobbylink.c,
 * tests/test_netcore.c) and the bot drive it on the "sim" transport. The lobby model is optional (the console `net` command can run the
 * link without a lobby); closing the lobby never touches the link: only lk_stop (or a lost connection) ends it.
 *
 * States reported to the model:  Host: WAIT (listening) -> CONNECTED (a player joined) -> WAIT (he left)
 *                                Join: CONNECTING -> CONNECTED (the host accepted) | ERROR (refused, lost, no answer)
 *                                any: ERROR when the start fails; IDLE after lk_stop.
 * "Ready" is not a link state: it is the two ready flags (l1_net_ready for this player, l1_net_set_peer_ready for the other one),
 * which the model shows once the link is CONNECTED. When both are ready the HOST starts the session (cs_start) by itself (lk_config.manual_start = 0).
 *
 * Roles: the host plays settings.role of the lobby (or lk_set_role), the joined player gets the other one. A change of the host's role
 * swaps both in one lobby revision and withdraws both ready flags. After the start the roles are final (lk_get_info().role).
 * Settings: the host's lk_set_settings() (episode, difficulty, section id, seed, flags) travel in the session record; the joined player reads
 * them with lk_get_info() (they are final after the start).
 */
#ifndef LOBBY_LINK_H
#define LOBBY_LINK_H

#include <stddef.h>
#include <stdint.h>

#include "coop_session.h"
#include "lobby1.h"

#define LK_DEFAULT_PORT 28410   /* the Rev2 co-op port: a Host listens on it, a Join without ":port" connects to it (the transport's default) */

typedef struct lk_t lk_t;

typedef struct {
    const char *name;             /* the name the other player sees ("Player" when NULL) */
    uint8_t exe_tag[8];           /* first 8 bytes of rerev.exe's SHA-256: a host refuses a join from another build; zeros = not checked (bot, tests) */
    const char *mod_version;      /* NULL = the default */
    uint32_t (*clock)(void *);    /* NULL = the real clock (tests pass a virtual one) */
    void *clock_user;
    int manual_start;             /* 1: the host never starts the session by itself (lk_start_session / the console `net start`) */
    int is_bot;                   /* the lab's headless partner (coop_bot): exempt from the mod-version check */
    int no_rejoin;                /* host: nobody comes back after the START (Rev1 0.1.0 product rule; the lab keeps the rejoin) */
} lk_config;

/* what the host decides for the shared game; travels in the session record (cp_session) */
typedef struct {
    int episode;                  /* 1.. (0 = keep) */
    int difficulty;
    int flags;
    uint32_t area_id;             /* the section the host starts in (informational: the story decides) */
    uint32_t seed;                /* 0 = keep the host's random seed */
} lk_settings;

typedef struct {
    int started;                  /* the session START happened: roles and settings are final */
    int host;                     /* this PC is the host */
    int role;                     /* what this PC's human plays: L1_LEAD / L1_PARTNER; -1 = not known yet */
    int remote_role;              /* the other human's role; -1 = nobody */
    int phase, epoch;             /* enum cp_phase, load epoch */
    int load_begins, gos;         /* LOAD_BEGIN / GO notifications seen since the start (any player's LOAD_BEGIN, every GO) */
    int last_go_epoch;            /* epoch of the last GO (0 = none) */
    uint32_t peer_load_area;      /* rev1 live 8: the area id of the partner's last LOAD_BEGIN (the level it loads; 0 = not known, an older mod) ... */
    int peer_load_epoch;          /* ... and the epoch of that LOAD_BEGIN */
    lk_settings set;              /* the session settings as this PC knows them */
    char peer_name[CP_NAME_MAX + 1];
    /* lobby-must: what happened, for the lobby's reason line and the in-game strip (counters only grow, so the glue sees every event once) */
    int reason;                   /* L1RS_* of the last failure / partner event (L1RS_NONE after a fresh start) */
    char ra1[64], ra2[24];        /* its arguments (l1_net_set_reason) */
    char last_peer[CP_NAME_MAX + 1];   /* the partner's name, kept after he left */
    int peer_gone, peer_gone_kind;     /* host: the partner left / was lost after the START (count, L1RS_PARTNER_LEFT / LOST of the last one) */
    int ended, ended_reason;           /* guest: the session ended (count, L1RS_HOST_ENDED / HOST_LOST / REMOVED of the last one) */
    int silent_s;                 /* CS_N_SILENT: seconds the partner has sent nothing (0 = heard again) */
    uint32_t silent_age_ms;       /* time since that note (the strip goes 15 s after the last one) */
    int wait_s;                   /* CS_N_WAIT: seconds this PC waits for the partner's load at the barrier */
    uint32_t wait_age_ms;
    char wait_name[CP_NAME_MAX + 1];
    int ready, peer_ready;        /* ready1: this PC's ready flag as given to the session, the partner's from the session's table (both 0 unless CONNECTED) */
} lk_info;

/* transport: "enet" in the game, "sim" in tests. threaded: 1 = the transport's own thread pumps (the game), 0 = lk_poll pumps (tests). lobby may be NULL. */
lk_t *lk_new(l1_t *lobby, const char *transport, int threaded);
void lk_set_transport(lk_t *k, const char *transport);   /* for the next lk_start: "enet" | "steam" | "sim" (a string that lives on) */
const char *lk_transport(const lk_t *k);
lk_t *lk_new_cfg(l1_t *lobby, const char *transport, int threaded, const lk_config *cfg);
void lk_free(lk_t *k);
/* attach / detach the lobby model (NULL = detach; the link keeps running). Attaching pushes the link state, the mode and the roles into the model. */
void lk_set_lobby(lk_t *k, l1_t *lobby);

/* mode: L1N_HOST (listen on port) or L1N_JOIN (target = "ip[:port]", as typed). Returns 1 when the attempt has started; 0 = failed
 * (the lobby is in L1NS_ERROR, err says why). For a host, own_addr receives this PC's "ip:port" (LAN address, "" when unknown). */
int lk_start(lk_t *k, int mode, const char *target, int port, char *own_addr, size_t oa_size, char *err, size_t esz);
void lk_stop(lk_t *k);                                  /* closes the session; the lobby goes to L1NS_IDLE */
void lk_poll(lk_t *k);                                  /* once a frame: session notifications, ready flags, roles, the automatic start */
int lk_active(const lk_t *k);                           /* the link is running: WAIT, CONNECTING or CONNECTED (not IDLE, not ERROR) */
int lk_state(const lk_t *k);                            /* L1NS_* as last reported */
int lk_mode(const lk_t *k);                             /* L1N_OFF / L1N_HOST / L1N_JOIN */
const char *lk_last_reason(const lk_t *k);              /* text of the last failure from the session ("" = none); for the log */

/* roles and readiness without a lobby (the console); with a lobby attached the model is the source and these write into it */
void lk_set_role(lk_t *k, int role);                    /* host: what this PC's human plays (L1_LEAD / L1_PARTNER); a joined player cannot choose */
/* rev1 live 16: the host's title Continue of a co-op save puts the save's roles back (cs_force_roles: after the START, before the first level); the lobby's
 * role follows (the room shows the save's characters). 1 = changed, 0 = already so, -1 = refused / not the host */
int lk_force_roles(lk_t *k, int host_role);
void lk_set_ready(lk_t *k, int ready);                  /* this player's ready flag (only while CONNECTED) */
int lk_set_settings(lk_t *k, const lk_settings *s);     /* host, before the start: 1 = sent to the other player */
int lk_start_session(lk_t *k, int force, char *err, size_t esz);   /* host: cs_start now (force = without waiting for the ready flags) */
void lk_set_manual_start(lk_t *k, int manual);       /* 1: the host never starts the session by itself (console `net auto off`) */
int lk_manual_start(lk_t *k);

/* events of the other player (CP_EVENT, any kind): the callback gets every one, with the session lock held (it must only queue the event, not call back into the link). Up to LK_EVENT_CBS callbacks;
 * they stay registered across lk_stop. 1 = registered (also when it already was). */
#define LK_EVENT_CBS 4
typedef void (*lk_event_cb)(void *user, const cp_event *ev);
int lk_add_event_cb(lk_t *k, lk_event_cb cb, void *user);

/* the level-load barrier for the game glue (every load: section start, checkpoint, area change, retry) */
void lk_load_begin(lk_t *k, uint32_t area_id);
void lk_load_done(lk_t *k);

/* a tap on every session notification (called inside lk_poll / cs_poll on the polling thread, link lock held): the world model takes the CS_N_EVENT notes of kind CP_EV_W1 here. NULL = none. */
void lk_set_note_tap(lk_t *k, void (*fn)(void *user, const cs_note *n), void *user);

void lk_get_info(lk_t *k, lk_info *out);
int lk_kick(lk_t *k);                                   /* host, before the START: remove the joined player (REFUSE REMOVED); 1 = done */
void lk_set_name(lk_t *k, const char *name);           /* the name the other player sees (taken at the next start of the link) */
void lk_set_no_rejoin(lk_t *k, int on);
cs_t *lk_session(lk_t *k);                              /* the session itself, for the state layer */
void lk_status(lk_t *k, char *out, size_t size);        /* a few lines for the console */
void lk_own_addr(lk_t *k, char *out, size_t size);   /* host: this PC's "ip:port" found at the start ("" = unknown / not hosting) */

#endif
