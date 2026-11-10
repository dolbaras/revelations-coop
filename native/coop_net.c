/*
 * Co-op transport: queues, network thread, and the "enet" and "sim" backends. See coop_net.h.
 * Pattern from the Jericho / TimeShift mods (ENet on a background thread so connections survive long level loads, two
 * locked queues between the thread and the game), made instantiable so tests can run two endpoints in one process.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <enet/enet.h>

#include "coop_net.h"
#include "coop_proto.h"

/* ---------------------------------------------------------------- log, time */
static void (*log_fn)(const char *);
void cn_set_log(void (*fn)(const char *line)) { log_fn = fn; }
void cn_log(const char *fmt, ...)
{
    if (!log_fn) return;
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_fn(line);
}

uint32_t cn_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint32_t)((unsigned long long)t.QuadPart * 1000ull / (unsigned long long)f.QuadPart);
}

/* ---------------------------------------------------------------- endpoint */
typedef struct qout { struct qout *next; int peer, channel, reliable; size_t len; uint8_t data[]; } qout;
typedef struct qin { struct qin *next; cn_event ev; } qin;

struct cn_t {
    CRITICAL_SECTION lock;
    const cn_ops *ops;
    void *st;
    int role, threaded;
    volatile int running;
    HANDLE thread;
    qout *out_head, *out_tail;
    qin *in_head, *in_tail;
    int in_unrel;
    int connected[CN_MAX_PEERS];
    uint32_t last_rx[CN_MAX_PEERS];
    cn_stats stats;
    uint32_t (*clock)(void *);
    void *clock_user;
};

#define MAX_BACKENDS 8
static const cn_ops *backends[MAX_BACKENDS];
static int n_backends;
extern const cn_ops cn_ops_enet, cn_ops_sim, cn_ops_steam;

int cn_register_backend(const cn_ops *ops)
{
    for (int i = 0; i < n_backends; i++)
        if (!strcmp(backends[i]->name, ops->name)) { backends[i] = ops; return 1; }
    if (n_backends >= MAX_BACKENDS) return 0;
    backends[n_backends++] = ops;
    return 1;
}
static const cn_ops *find_backend(const char *name)
{
    if (!n_backends) {
        backends[n_backends++] = &cn_ops_enet;
        backends[n_backends++] = &cn_ops_sim;
        backends[n_backends++] = &cn_ops_steam;   /* coop_steam.c: Steam lobby + Steam relay network */
    }
    if (!name || !*name) name = "enet";
    for (int i = 0; i < n_backends; i++)
        if (!strcmp(backends[i]->name, name)) return backends[i];
    return NULL;
}

cn_t *cn_new(void)
{
    cn_t *c = (cn_t *)calloc(1, sizeof *c);
    if (c) InitializeCriticalSection(&c->lock);
    return c;
}
void cn_free(cn_t *c)
{
    if (!c) return;
    cn_stop(c);
    DeleteCriticalSection(&c->lock);
    free(c);
}

void cn_set_clock(cn_t *c, uint32_t (*fn)(void *), void *user) { c->clock = fn, c->clock_user = user; }
static uint32_t now(cn_t *c) { return c->clock ? c->clock(c->clock_user) : cn_now_ms(); }

static void inbox_push(cn_t *c, int type, int peer, int channel, uint8_t *data, size_t len, uint32_t t_ms)
{
    qin *n = (qin *)calloc(1, sizeof *n);
    n->ev.type = type, n->ev.peer = peer, n->ev.channel = channel, n->ev.data = data, n->ev.len = len, n->ev.t_ms = t_ms;
    if (type == CN_EV_RECEIVE && channel == CP_CH_STATE) {
        if (c->in_unrel >= 256) {   /* the game thread is away (level load): drop the oldest unreliable message */
            qin **pp = &c->in_head;
            while (*pp && !((*pp)->ev.type == CN_EV_RECEIVE && (*pp)->ev.channel == CP_CH_STATE)) pp = &(*pp)->next;
            if (*pp) {
                qin *d = *pp;
                *pp = d->next;
                if (c->in_tail == d) {
                    c->in_tail = NULL;
                    for (qin *q = c->in_head; q; q = q->next) c->in_tail = q;
                }
                free(d->ev.data);
                free(d);
                c->in_unrel--;
            }
        }
        c->in_unrel++;
    }
    if (c->in_tail) c->in_tail->next = n;
    else c->in_head = n;
    c->in_tail = n;
}

static void do_send(cn_t *c, int peer, int channel, int reliable, const void *data, size_t len)
{
    if (channel < 0 || channel >= CP_CHANNELS) channel = 0;
    for (int i = 0; i < CN_MAX_PEERS; i++) {
        if (peer >= 0 && peer != i) continue;
        if (!c->connected[i]) continue;
        c->ops->send(c->st, i, channel, reliable, data, len);
        c->stats.pk_out[channel]++, c->stats.by_out[channel] += (uint32_t)len;
    }
}

/* hand everything queued by cn_send to the backend (lock held) */
static void flush_out(cn_t *c)
{
    qout *o;
    while ((o = c->out_head) != NULL) {
        c->out_head = o->next;
        if (!c->out_head) c->out_tail = NULL;
        do_send(c, o->peer, o->channel, o->reliable, o->data, o->len);
        free(o);
    }
}

void cn_pump(cn_t *c)
{
    EnterCriticalSection(&c->lock);
    if (c->ops && c->st) {
        flush_out(c);
        cn_event evs[32];
        int n;
        do {
            n = c->ops->service(c->st, evs, 32);
            for (int i = 0; i < n; i++) {
                cn_event *e = &evs[i];
                int p = e->peer;
                if (p < 0 || p >= CN_MAX_PEERS) { free(e->data); continue; }
                if (e->type == CN_EV_CONNECT) {
                    c->connected[p] = 1, c->last_rx[p] = now(c), c->stats.connects++;
                    inbox_push(c, CN_EV_CONNECT, p, 0, NULL, 0, c->last_rx[p]);
                } else if (e->type == CN_EV_DISCONNECT) {
                    if (c->connected[p]) {
                        c->connected[p] = 0, c->stats.disconnects++;
                        inbox_push(c, CN_EV_DISCONNECT, p, 0, NULL, 0, now(c));
                    }
                } else if (e->type == CN_EV_RECEIVE) {
                    if (!c->connected[p]) { free(e->data); continue; }
                    c->last_rx[p] = now(c);
                    int ch = e->channel >= 0 && e->channel < CP_CHANNELS ? e->channel : 0;
                    c->stats.pk_in[ch]++, c->stats.by_in[ch] += (uint32_t)e->len;
                    inbox_push(c, CN_EV_RECEIVE, p, ch, e->data, e->len, c->last_rx[p]);
                } else
                    free(e->data);
            }
        } while (n == 32);
    }
    LeaveCriticalSection(&c->lock);
}

static DWORD WINAPI net_thread(LPVOID arg)
{
    cn_t *c = (cn_t *)arg;
    while (c->running) {
        cn_pump(c);
        Sleep(1);
    }
    return 0;
}

static int start_common(cn_t *c, const cn_ops *ops, int role, int threaded)
{
    c->ops = ops, c->role = role, c->threaded = threaded;
    memset(c->connected, 0, sizeof c->connected);
    memset(&c->stats, 0, sizeof c->stats);
    if (threaded) {
        c->running = 1;
        c->thread = CreateThread(NULL, 0, net_thread, c, 0, NULL);
        if (!c->thread) { c->running = 0; return 0; }
    }
    return 1;
}

int cn_host(cn_t *c, const char *backend, int port, int max_clients, int threaded, char *err, size_t esz)
{
    cn_stop(c);
    const cn_ops *ops = find_backend(backend);
    if (!ops) { snprintf(err, esz, "unknown transport '%s'", backend ? backend : ""); return 0; }
    if (max_clients < 1 || max_clients >= CN_MAX_PEERS) max_clients = 1;
    EnterCriticalSection(&c->lock);
    void *st = NULL;
    err[0] = 0;
    int ok = ops->host(&st, port, max_clients, err, esz);
    if (ok) c->st = st;
    LeaveCriticalSection(&c->lock);
    if (!ok) { if (!err[0]) snprintf(err, esz, "host failed"); return 0; }
    cn_log("net: hosting on %s port %d (max %d client(s))", ops->name, port, max_clients);
    return start_common(c, ops, CN_ROLE_HOST, threaded);
}

int cn_join(cn_t *c, const char *backend, const char *target, int threaded, char *err, size_t esz)
{
    cn_stop(c);
    const cn_ops *ops = find_backend(backend);
    if (!ops) { snprintf(err, esz, "unknown transport '%s'", backend ? backend : ""); return 0; }
    EnterCriticalSection(&c->lock);
    void *st = NULL;
    err[0] = 0;
    int ok = ops->join(&st, target, err, esz);
    if (ok) c->st = st;
    LeaveCriticalSection(&c->lock);
    if (!ok) { if (!err[0]) snprintf(err, esz, "join failed"); return 0; }
    cn_log("net: joining %s via %s", target, ops->name);
    return start_common(c, ops, CN_ROLE_CLIENT, threaded);
}

void cn_stop(cn_t *c)
{
    if (!c->ops) return;
    /* flush what is queued (a LEAVE message) and give the peers a moment to hear the goodbye */
    cn_pump(c);
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (c->connected[i]) c->ops->disconnect(c->st, i, 1);
    LeaveCriticalSection(&c->lock);
    uint32_t t0 = cn_now_ms();
    for (;;) {
        int any = 0;
        if (!c->threaded) cn_pump(c);
        else Sleep(5);
        EnterCriticalSection(&c->lock);
        for (int i = 0; i < CN_MAX_PEERS; i++) any |= c->connected[i];
        LeaveCriticalSection(&c->lock);
        if (!any || cn_now_ms() - t0 > 300) break;
    }
    if (c->threaded && c->thread) {
        c->running = 0;
        WaitForSingleObject(c->thread, 3000);
        CloseHandle(c->thread);
        c->thread = NULL;
    }
    EnterCriticalSection(&c->lock);
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (c->connected[i]) c->ops->disconnect(c->st, i, 0);
    c->ops->close(c->st);
    c->st = NULL, c->ops = NULL, c->role = CN_ROLE_NONE;
    memset(c->connected, 0, sizeof c->connected);
    qout *o;
    while ((o = c->out_head) != NULL) { c->out_head = o->next; free(o); }
    c->out_tail = NULL;
    qin *q;
    while ((q = c->in_head) != NULL) { c->in_head = q->next; free(q->ev.data); free(q); }
    c->in_tail = NULL, c->in_unrel = 0;
    LeaveCriticalSection(&c->lock);
}

int cn_send(cn_t *c, int peer, int channel, int reliable, const void *data, size_t len)
{
    if (!c->ops || !data || !len || len > 60000) return 0;
    qout *o = (qout *)malloc(sizeof *o + len);
    if (!o) return 0;
    o->next = NULL, o->peer = peer, o->channel = channel, o->reliable = reliable, o->len = len;
    memcpy(o->data, data, len);
    EnterCriticalSection(&c->lock);
    if (!c->ops) { LeaveCriticalSection(&c->lock); free(o); return 0; }
    if (c->out_tail) c->out_tail->next = o;
    else c->out_head = o;
    c->out_tail = o;
    LeaveCriticalSection(&c->lock);
    return 1;
}

int cn_recv(cn_t *c, cn_event *ev)
{
    EnterCriticalSection(&c->lock);
    qin *n = c->in_head;
    if (n) {
        c->in_head = n->next;
        if (!c->in_head) c->in_tail = NULL;
        if (n->ev.type == CN_EV_RECEIVE && n->ev.channel == CP_CH_STATE) c->in_unrel--;
        *ev = n->ev;
        free(n);
    }
    LeaveCriticalSection(&c->lock);
    return n != NULL;
}
void cn_event_free(cn_event *ev) { free(ev->data); ev->data = NULL; }

void cn_disconnect_peer(cn_t *c, int peer, int graceful)
{
    if (peer < 0 || peer >= CN_MAX_PEERS) return;
    EnterCriticalSection(&c->lock);
    if (c->ops && c->connected[peer]) {
        flush_out(c);   /* what was queued for the peer (a REFUSE, a LEAVE) goes first */
        c->ops->disconnect(c->st, peer, graceful);
        if (!graceful) {
            c->connected[peer] = 0, c->stats.disconnects++;
            inbox_push(c, CN_EV_DISCONNECT, peer, 0, NULL, 0, now(c));
        }
    }
    LeaveCriticalSection(&c->lock);
}

int cn_role(cn_t *c) { return c->role; }
int cn_connected(cn_t *c, int peer) { return peer >= 0 && peer < CN_MAX_PEERS && c->connected[peer]; }
int cn_failed(cn_t *c, char *why, size_t size)
{
    int r = 0;
    EnterCriticalSection(&c->lock);
    if (c->ops && c->st && c->ops->failed) r = c->ops->failed(c->st, why, size);
    LeaveCriticalSection(&c->lock);
    return r;
}
int cn_rtt(cn_t *c, int peer)
{
    int r = -1;
    EnterCriticalSection(&c->lock);
    if (c->ops && peer >= 0 && peer < CN_MAX_PEERS && c->connected[peer]) r = c->ops->rtt(c->st, peer);
    LeaveCriticalSection(&c->lock);
    return r;
}
uint32_t cn_peer_idle_ms(cn_t *c, int peer)
{
    if (peer < 0 || peer >= CN_MAX_PEERS) return 0;
    EnterCriticalSection(&c->lock);
    uint32_t v = c->connected[peer] ? now(c) - c->last_rx[peer] : 0;
    LeaveCriticalSection(&c->lock);
    return v;
}
void cn_get_stats(cn_t *c, cn_stats *s)
{
    EnterCriticalSection(&c->lock);
    *s = c->stats;
    LeaveCriticalSection(&c->lock);
}
const char *cn_backend_name(cn_t *c) { return c->ops ? c->ops->name : "none"; }

/* ================================================================ ENet backend */
typedef struct {
    ENetHost *host;
    ENetPeer *peers[CN_MAX_PEERS];
    int is_host;
    int pending_connect;   /* client: connect() issued, no CONNECT event yet */
} enet_st;

static int enet_refs;
static int enet_ref(void)
{
    if (enet_refs == 0 && enet_initialize() != 0) return 0;
    enet_refs++;
    return 1;
}
static void enet_unref(void)
{
    if (enet_refs > 0 && --enet_refs == 0) enet_deinitialize();
}

static int enet_b_host(void **stp, int port, int max_peers, char *err, size_t esz)
{
    if (!enet_ref()) { snprintf(err, esz, "enet_initialize failed"); return 0; }
    enet_st *s = (enet_st *)calloc(1, sizeof *s);
    ENetAddress a = {ENET_HOST_ANY, (enet_uint16)port};
    s->host = enet_host_create(&a, (size_t)max_peers, CP_CHANNELS, 0, 0);
    s->is_host = 1;
    if (!s->host) {
        snprintf(err, esz, "cannot listen on UDP port %d (in use?)", port);
        free(s);
        enet_unref();
        return 0;
    }
    *stp = s;
    return 1;
}

static int enet_b_join(void **stp, const char *target, char *err, size_t esz)
{
    char host[200];
    int port = 28410;
    snprintf(host, sizeof host, "%s", target ? target : "");
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon) {   /* "ip:port" (an IPv6 literal has more colons and is not supported here) */
        *colon = 0;
        port = atoi(colon + 1);
    }
    if (!host[0] || port <= 0 || port > 65535) { snprintf(err, esz, "bad address '%s' (use ip:port)", target ? target : ""); return 0; }
    if (!enet_ref()) { snprintf(err, esz, "enet_initialize failed"); return 0; }
    enet_st *s = (enet_st *)calloc(1, sizeof *s);
    s->host = enet_host_create(NULL, 1, CP_CHANNELS, 0, 0);
    ENetAddress a;
    if (!s->host || enet_address_set_host(&a, host) != 0) {
        snprintf(err, esz, s->host ? "cannot resolve '%s'" : "cannot create the UDP socket", host);
        if (s->host) enet_host_destroy(s->host);
        free(s);
        enet_unref();
        return 0;
    }
    a.port = (enet_uint16)port;
    ENetPeer *p = enet_host_connect(s->host, &a, CP_CHANNELS, 0);
    if (!p) {
        snprintf(err, esz, "enet_host_connect failed");
        enet_host_destroy(s->host);
        free(s);
        enet_unref();
        return 0;
    }
    enet_peer_timeout(p, 0, 8000, 8000);   /* a connect that nobody answers fails after 8 s (the timeouts widen once connected) */
    s->peers[0] = p;
    s->pending_connect = 1;
    *stp = s;
    return 1;
}

static void enet_b_send(void *stp, int peer, int channel, int reliable, const void *data, size_t len)
{
    enet_st *s = (enet_st *)stp;
    if (!s->peers[peer]) return;
    ENetPacket *pk = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (pk && enet_peer_send(s->peers[peer], (enet_uint8)channel, pk) < 0) enet_packet_destroy(pk);
}

static int peer_index(enet_st *s, ENetPeer *p)
{
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (s->peers[i] == p) return i;
    return -1;
}

static int enet_b_service(void *stp, cn_event *out, int max)
{
    enet_st *s = (enet_st *)stp;
    int n = 0;
    ENetEvent ev;
    while (n < max && enet_host_service(s->host, &ev, 0) > 0) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            int id = -1;
            if (s->is_host) {
                for (int i = 1; i < CN_MAX_PEERS; i++)
                    if (!s->peers[i]) { id = i; break; }
            } else
                id = peer_index(s, ev.peer) == 0 ? 0 : -1;
            if (id < 0) { enet_peer_disconnect_now(ev.peer, 0); break; }
            s->peers[id] = ev.peer;
            s->pending_connect = 0;
            enet_peer_timeout(ev.peer, 64, 10000, 30000);   /* gone after 30 s without any acknowledgement (a long level load does not matter: this thread keeps running) */
            enet_peer_throttle_configure(ev.peer, 5000, 0, 0);   /* never thin out the (tiny) unreliable state stream because the link looks bad */
            out[n].type = CN_EV_CONNECT, out[n].peer = id, out[n].channel = 0, out[n].data = NULL, out[n].len = 0;
            n++;
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            int id = peer_index(s, ev.peer);
            if (id >= 0) {
                out[n].type = CN_EV_RECEIVE, out[n].peer = id, out[n].channel = ev.channelID;
                out[n].data = (uint8_t *)malloc(ev.packet->dataLength);
                memcpy(out[n].data, ev.packet->data, ev.packet->dataLength);
                out[n].len = ev.packet->dataLength;
                n++;
            }
            enet_packet_destroy(ev.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            int id = peer_index(s, ev.peer);
            if (id >= 0) {
                s->peers[id] = NULL;
                out[n].type = CN_EV_DISCONNECT, out[n].peer = id, out[n].channel = 0, out[n].data = NULL, out[n].len = 0;
                n++;
            }
            break;
        }
        default:
            break;
        }
    }
    /* a connect attempt that timed out is reset by ENet without an event: report it as a disconnect of the host (peer 0) */
    if (!s->is_host && s->pending_connect && s->peers[0] && s->peers[0]->state == ENET_PEER_STATE_DISCONNECTED && n < max) {
        s->peers[0] = NULL, s->pending_connect = 0;
        out[n].type = CN_EV_DISCONNECT, out[n].peer = 0, out[n].channel = 0, out[n].data = NULL, out[n].len = 0;
        n++;
    }
    enet_host_flush(s->host);
    return n;
}

static void enet_b_disconnect(void *stp, int peer, int graceful)
{
    enet_st *s = (enet_st *)stp;
    if (!s->peers[peer]) return;
    if (graceful) enet_peer_disconnect_later(s->peers[peer], 0);
    else {
        enet_peer_disconnect_now(s->peers[peer], 0);
        s->peers[peer] = NULL;
    }
    enet_host_flush(s->host);
}

static int enet_b_rtt(void *stp, int peer)
{
    enet_st *s = (enet_st *)stp;
    return s->peers[peer] ? (int)s->peers[peer]->roundTripTime : -1;
}

static void enet_b_close(void *stp)
{
    enet_st *s = (enet_st *)stp;
    if (!s) return;
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (s->peers[i]) enet_peer_disconnect_now(s->peers[i], 0);
    enet_host_flush(s->host);
    enet_host_destroy(s->host);
    free(s);
    enet_unref();
}

const cn_ops cn_ops_enet = {"enet", enet_b_host, enet_b_join, enet_b_send, enet_b_service, enet_b_disconnect, enet_b_rtt, enet_b_close};

/* ================================================================ sim backend (tests) */
typedef struct sim_pkt { struct sim_pkt *next; uint32_t due; int type, peer, channel; uint8_t *data; size_t len; } sim_pkt;
typedef struct sim_node {
    int is_host, port, alive;
    struct sim_node *remote[CN_MAX_PEERS];
    int idx_at_remote[CN_MAX_PEERS];
    uint32_t last_rel_due[CN_MAX_PEERS];
    sim_pkt *q;
} sim_node;

static sim_node *sim_hosts[32];
static cn_sim_params sim_p;
static uint32_t sim_rng = 12345;
static int sim_virtual;
static uint32_t sim_vtime;

void cn_sim_set(const cn_sim_params *p)
{
    sim_p = *p;
    sim_rng = p->seed ? p->seed : 12345;
}
void cn_sim_set_time(uint32_t ms) { sim_virtual = 1, sim_vtime = ms; }
void cn_sim_real_time(void) { sim_virtual = 0; }
static uint32_t sim_now(void) { return sim_virtual ? sim_vtime : cn_now_ms(); }
static uint32_t sim_rand(void)
{
    sim_rng ^= sim_rng << 13, sim_rng ^= sim_rng >> 17, sim_rng ^= sim_rng << 5;
    return sim_rng;
}
static int sim_pct(int pct) { return pct > 0 && (int)(sim_rand() % 100u) < pct; }

static void sim_enqueue(sim_node *to, uint32_t due, int type, int peer, int channel, const void *data, size_t len)
{
    sim_pkt *k = (sim_pkt *)calloc(1, sizeof *k);
    k->due = due, k->type = type, k->peer = peer, k->channel = channel, k->len = len;
    if (len) {
        k->data = (uint8_t *)malloc(len);
        memcpy(k->data, data, len);
    }
    sim_pkt **pp = &to->q;
    while (*pp && (int32_t)((*pp)->due - due) <= 0) pp = &(*pp)->next;   /* stable: after equal due times */
    k->next = *pp;
    *pp = k;
}

static int sim_port(const char *target)
{
    if (!target) return 0;
    if (!strncmp(target, "sim:", 4)) target += 4;
    return atoi(target);
}

static int sim_b_host(void **stp, int port, int max_peers, char *err, size_t esz)
{
    (void)max_peers;
    for (int i = 0; i < 32; i++)
        if (sim_hosts[i] && sim_hosts[i]->port == port) { snprintf(err, esz, "sim port %d in use", port); return 0; }
    for (int i = 0; i < 32; i++)
        if (!sim_hosts[i]) {
            sim_node *n = (sim_node *)calloc(1, sizeof *n);
            n->is_host = 1, n->port = port, n->alive = 1;
            sim_hosts[i] = n;
            *stp = n;
            return 1;
        }
    snprintf(err, esz, "sim: too many hosts");
    return 0;
}

static int sim_b_join(void **stp, const char *target, char *err, size_t esz)
{
    int port = sim_port(target);
    sim_node *h = NULL;
    for (int i = 0; i < 32; i++)
        if (sim_hosts[i] && sim_hosts[i]->port == port) h = sim_hosts[i];
    if (!h) { snprintf(err, esz, "sim: no host on port %d", port); return 0; }
    int idx = -1;
    for (int i = 1; i < CN_MAX_PEERS; i++)
        if (!h->remote[i]) { idx = i; break; }
    if (idx < 0) { snprintf(err, esz, "sim: host is full"); return 0; }
    sim_node *c = (sim_node *)calloc(1, sizeof *c);
    c->alive = 1;
    c->remote[0] = h, c->idx_at_remote[0] = idx;
    h->remote[idx] = c, h->idx_at_remote[idx] = 0;
    uint32_t t = sim_now();
    sim_enqueue(h, t, CN_EV_CONNECT, idx, 0, NULL, 0);
    sim_enqueue(c, t, CN_EV_CONNECT, 0, 0, NULL, 0);
    *stp = c;
    return 1;
}

static void sim_b_send(void *stp, int peer, int channel, int reliable, const void *data, size_t len)
{
    sim_node *n = (sim_node *)stp, *to = n->remote[peer];
    if (!to) return;
    int at = n->idx_at_remote[peer];
    uint32_t t = sim_now();
    uint32_t base = t + (uint32_t)sim_p.delay_ms + (sim_p.jitter_ms > 0 ? sim_rand() % (uint32_t)(sim_p.jitter_ms + 1) : 0);
    if (reliable) {   /* reliable ordered: never lost, never reordered, only late */
        if ((int32_t)(base - n->last_rel_due[peer]) < 0) base = n->last_rel_due[peer];
        n->last_rel_due[peer] = base;
        sim_enqueue(to, base, CN_EV_RECEIVE, at, channel, data, len);
        return;
    }
    if (sim_pct(sim_p.loss_pct)) return;
    if (sim_pct(sim_p.reorder_pct)) base += (uint32_t)(sim_p.reorder_extra_ms > 0 ? sim_p.reorder_extra_ms : 40);
    sim_enqueue(to, base, CN_EV_RECEIVE, at, channel, data, len);
    if (sim_pct(sim_p.dup_pct)) sim_enqueue(to, base + 1 + sim_rand() % 20u, CN_EV_RECEIVE, at, channel, data, len);
}

static int sim_b_service(void *stp, cn_event *out, int max)
{
    sim_node *n = (sim_node *)stp;
    int k = 0;
    uint32_t t = sim_now();
    while (n->q && k < max && (int32_t)(n->q->due - t) <= 0) {
        sim_pkt *p = n->q;
        n->q = p->next;
        out[k].type = p->type, out[k].peer = p->peer, out[k].channel = p->channel, out[k].data = p->data, out[k].len = p->len;
        k++;
        free(p);
    }
    return k;
}

static void sim_b_disconnect(void *stp, int peer, int graceful)
{
    (void)graceful;
    sim_node *n = (sim_node *)stp, *to = n->remote[peer];
    if (!to) return;
    int at = n->idx_at_remote[peer];
    sim_enqueue(to, sim_now() + (uint32_t)sim_p.delay_ms, CN_EV_DISCONNECT, at, 0, NULL, 0);
    sim_enqueue(n, sim_now(), CN_EV_DISCONNECT, peer, 0, NULL, 0);   /* the local side hears it too (ENet does after the goodbye is acked) */
    to->remote[at] = NULL;
    n->remote[peer] = NULL;
}

static int sim_b_rtt(void *stp, int peer)
{
    (void)stp, (void)peer;
    return 2 * sim_p.delay_ms;
}

static void sim_b_close(void *stp)
{
    sim_node *n = (sim_node *)stp;
    for (int i = 0; i < CN_MAX_PEERS; i++)
        if (n->remote[i]) sim_b_disconnect(n, i, 0);
    for (int i = 0; i < 32; i++)
        if (sim_hosts[i] == n) sim_hosts[i] = NULL;
    sim_pkt *p;
    while ((p = n->q) != NULL) { n->q = p->next; free(p->data); free(p); }
    free(n);
}

const cn_ops cn_ops_sim = {"sim", sim_b_host, sim_b_join, sim_b_send, sim_b_service, sim_b_disconnect, sim_b_rtt, sim_b_close};
