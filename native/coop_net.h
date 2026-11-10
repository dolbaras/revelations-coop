/*
 * Co-op transport for Revelations 2: a thin, thread-safe message pipe between machines behind a backend interface.
 *
 *   game thread  --cn_send-->  [outbox]  --net thread: cn_pump-->  backend (ENet UDP, sim, later Steam)
 *   game thread  <-cn_recv--   [inbox]   <--net thread: cn_pump--  backend
 *
 * Peers are small integers. The host sees its clients as 1, 2, 3 ...; a client sees the host as 0. The backends deliver
 * CONNECT / DISCONNECT / RECEIVE events, two channels (0 reliable ordered, 1 unreliable sequenced) and an RTT.
 * Everything above this file (protocol, lobby, state) is in coop_session.c and does not know which backend runs.
 * Backends: "enet" (direct IP, UDP), "sim" (in-process, deterministic loss/duplication/reordering for tests), "steam" (coop_steam.c:
 * a Steam lobby for rendezvous / invites / "Join game" and Steam's relay network for the messages, through the game's own steam_api;
 * see docs/steam-transport.md). More backends plug in through cn_register_backend.
 *
 * One cn_t is one endpoint; the proxy uses one, tests use two in one process.
 */
#ifndef COOP_NET_H
#define COOP_NET_H

#include <stddef.h>
#include <stdint.h>

#define CN_MAX_PEERS 8

typedef struct cn_t cn_t;

enum { CN_ROLE_NONE = 0, CN_ROLE_HOST = 1, CN_ROLE_CLIENT = 2 };
enum { CN_EV_NONE = 0, CN_EV_CONNECT = 1, CN_EV_DISCONNECT = 2, CN_EV_RECEIVE = 3 };

typedef struct {
    int type, peer, channel;
    uint8_t *data;   /* RECEIVE only: malloc'd, free with cn_event_free */
    size_t len;
    uint32_t t_ms;   /* when the network thread saw it (the endpoint's clock), set by cn_pump; backends leave it 0 */
} cn_event;

typedef struct cn_ops {
    const char *name;
    /* st = backend state (the backend allocates it); err gets a short reason on failure. host: bind port, max_peers = clients */
    int (*host)(void **st, int port, int max_peers, char *err, size_t esz);
    int (*join)(void **st, const char *target, char *err, size_t esz);
    void (*send)(void *st, int peer, int channel, int reliable, const void *data, size_t len);
    int (*service)(void *st, cn_event *out, int max);          /* non-blocking; returns the number of events written */
    void (*disconnect)(void *st, int peer, int graceful);      /* graceful: after the queued data */
    int (*rtt)(void *st, int peer);                            /* milliseconds, -1 unknown */
    void (*close)(void *st);
    int (*failed)(void *st, char *why, size_t size);           /* optional: 1 = a join that never connected has failed for good, `why` says why (the session reports it at once) */
} cn_ops;

typedef struct {
    uint32_t pk_in[2], pk_out[2], by_in[2], by_out[2];   /* per channel */
    uint32_t connects, disconnects;
} cn_stats;

cn_t *cn_new(void);
void cn_free(cn_t *c);
int cn_register_backend(const cn_ops *ops);                     /* adds a backend by name; the built-ins register themselves */

/* threaded = 1: a network thread calls cn_pump every millisecond or two (the game); 0: the caller pumps (tests) */
int cn_host(cn_t *c, const char *backend, int port, int max_clients, int threaded, char *err, size_t esz);
int cn_join(cn_t *c, const char *backend, const char *target, int threaded, char *err, size_t esz);
void cn_stop(cn_t *c);
void cn_pump(cn_t *c);

int cn_send(cn_t *c, int peer, int channel, int reliable, const void *data, size_t len);   /* peer -1 = all connected peers */
int cn_recv(cn_t *c, cn_event *ev);                              /* 1 = an event was returned (free data with cn_event_free) */
void cn_event_free(cn_event *ev);
void cn_disconnect_peer(cn_t *c, int peer, int graceful);

int cn_role(cn_t *c);
int cn_connected(cn_t *c, int peer);
int cn_failed(cn_t *c, char *why, size_t size);                  /* the backend gave up joining (optional feature of a backend); 0 = still trying / not supported */
int cn_rtt(cn_t *c, int peer);                                   /* ms or -1 */
uint32_t cn_peer_idle_ms(cn_t *c, int peer);                      /* since anything arrived from the peer (counted on the network thread) */
void cn_get_stats(cn_t *c, cn_stats *s);
const char *cn_backend_name(cn_t *c);
void cn_set_clock(cn_t *c, uint32_t (*fn)(void *), void *user);   /* tests: virtual time for idle counting */

/* logging sink for the transport and the session (the proxy sets r1_log, the bot stderr) */
void cn_set_log(void (*fn)(const char *line));
void cn_log(const char *fmt, ...);

/* real monotonic milliseconds */
uint32_t cn_now_ms(void);

/* ---- the "sim" backend: target/port is an integer id; faults apply to unreliable packets in both directions ---- */
typedef struct {
    int loss_pct, dup_pct, reorder_pct;   /* percent */
    int delay_ms, jitter_ms, reorder_extra_ms;
    uint32_t seed;
} cn_sim_params;
void cn_sim_set(const cn_sim_params *p);
void cn_sim_set_time(uint32_t ms);        /* virtual clock for the sim backend (until cn_sim_real_time) */
void cn_sim_real_time(void);

#endif
