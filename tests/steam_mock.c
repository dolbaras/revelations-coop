/* Mock Steam universe for the tests of coop_steam.c, see steam_mock.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "steam_mock.h"

#define MU 8
#define ML 12
#define MD 24
#define APP MOCK_APP
#define ID_USER(i) (0x0110000100000000ull + 1000ull + (uint64_t)(i))
#define ID_LOBBY(i) (0x0186000000000000ull + 5000ull + (uint64_t)(i))

typedef struct pkt { struct pkt *next; uint32_t due; uint64_t from; int held; size_t len; uint8_t *data; } pkt;

struct mock_user {
    int used, idx;
    uint64_t id;
    char name[32];
    uint8_t friend_of[MU];
    int in_game;
    char pk[8][32], pv[8][96];
    int npres;
    cst_event ev[64];
    int eh, et;
    pkt *inbox;
    uint64_t search[24];
    int nsearch;
    int dead;
    char init_fail[80];
    uint64_t invites[8];
    int ninv;
    uint32_t last_rel_due[MU];
    uint32_t n_sent, n_recv;
    int n_sess_req;
    uint8_t broken[MU], hole[MU];
    char pbuf[96];
    int dialog;
};

typedef struct {
    int used;
    uint64_t id;
    int type, max;
    uint64_t owner;
    uint64_t members[8];
    int n;
    char k[MD][32], v[MD][96];
    int nd;
} mlobby;

static int pair_init[MU][MU];   /* pair_init[min][max] = index of the initiator */
static struct {
    mock_user users[MU];
    mlobby lobbies[ML];
    uint8_t pair[MU][MU];        /* [a][b] 1 = a initiated, b has to accept; 2 = established (stored at [min][max] and mirrored) */
    uint32_t now;
    int loss, dup, reorder, delay, jitter;
    uint32_t rng;
    int implicit;
    int n_lobbies;
} W;

void mock_reset(void)
{
    for (int i = 0; i < MU; i++) {
        pkt *p;
        while ((p = W.users[i].inbox) != NULL) { W.users[i].inbox = p->next; free(p->data); free(p); }
    }
    memset(&W, 0, sizeof W);
    memset(pair_init, 0, sizeof pair_init);
    W.rng = 12345;
    W.implicit = 1;
}

static uint32_t rnd(void)
{
    W.rng ^= W.rng << 13, W.rng ^= W.rng >> 17, W.rng ^= W.rng << 5;
    return W.rng;
}
static int pct(int p) { return p > 0 && (int)(rnd() % 100u) < p; }

mock_user *mock_add_user(const char *name)
{
    for (int i = 0; i < MU; i++)
        if (!W.users[i].used) {
            mock_user *u = &W.users[i];
            memset(u, 0, sizeof *u);
            u->used = 1, u->idx = i, u->id = ID_USER(i);
            snprintf(u->name, sizeof u->name, "%s", name);
            return u;
        }
    return NULL;
}
uint64_t mock_id(const mock_user *u) { return u->id; }
void mock_befriend(mock_user *a, mock_user *b) { a->friend_of[b->idx] = 1, b->friend_of[a->idx] = 1; }
void mock_set_time(uint32_t ms) { W.now = ms; }
uint32_t mock_time(void) { return W.now; }
void mock_set_faults(int loss, int dup, int reorder, int delay, int jitter, uint32_t seed)
{
    W.loss = loss, W.dup = dup, W.reorder = reorder, W.delay = delay, W.jitter = jitter;
    if (seed) W.rng = seed;
}
void mock_set_implicit_accept(int on) { W.implicit = on; }
void mock_set_init_fail(mock_user *u, const char *why) { snprintf(u->init_fail, sizeof u->init_fail, "%s", why ? why : ""); }

static mock_user *U(void *c) { return (mock_user *)c; }
static mock_user *user_of(uint64_t id)
{
    int i = (int)((id & 0xFFFFFFFFull) - 1000ull);
    return i >= 0 && i < MU && W.users[i].used && W.users[i].id == id ? &W.users[i] : NULL;
}
static mlobby *lobby_of(uint64_t id)
{
    for (int i = 0; i < ML; i++)
        if (W.lobbies[i].used && W.lobbies[i].id == id) return &W.lobbies[i];
    return NULL;
}
static void post(mock_user *u, const cst_event *e)
{
    int n = (u->et + 1) % 64;
    if (n != u->eh) u->ev[u->et] = *e, u->et = n;
}
static int is_member(mlobby *l, uint64_t id)
{
    for (int i = 0; i < l->n; i++)
        if (l->members[i] == id) return 1;
    return 0;
}
static int visible(mock_user *u, mlobby *l)   /* data / members of a lobby are readable for members and for what a search returned */
{
    if (is_member(l, u->id)) return 1;
    for (int i = 0; i < u->nsearch; i++)
        if (u->search[i] == l->id) return 1;
    return 0;
}
static void notify_members(mlobby *l, uint64_t except)
{
    for (int i = 0; i < l->n; i++) {
        mock_user *m = user_of(l->members[i]);
        if (!m || m->id == except) continue;
        cst_event e;
        memset(&e, 0, sizeof e);
        e.type = CST_EV_LOBBY_CHANGE, e.id = l->id;
        post(m, &e);
    }
}

/* ---------------------------------------------------------------------------------------- platform functions */
static int m_init(void *c, char *err, size_t esz)
{
    if (U(c)->init_fail[0]) { snprintf(err, esz, "%s", U(c)->init_fail); return 0; }
    return 1;
}
static uint32_t m_now(void *c) { (void)c; return W.now; }
static uint64_t m_my_id(void *c) { return U(c)->id; }
static const char *m_my_name(void *c) { return U(c)->name; }
static int m_poll(void *c, cst_event *ev)
{
    mock_user *u = U(c);
    if (u->eh == u->et) return 0;
    *ev = u->ev[u->eh];
    u->eh = (u->eh + 1) % 64;
    return 1;
}

static int m_lobby_create(void *c, int type, int max)
{
    mock_user *u = U(c);
    for (int i = 0; i < ML; i++)
        if (!W.lobbies[i].used) {
            mlobby *l = &W.lobbies[i];
            memset(l, 0, sizeof *l);
            l->used = 1, l->id = ID_LOBBY(W.n_lobbies++), l->type = type, l->max = max, l->owner = u->id;
            l->members[l->n++] = u->id;
            cst_event e;
            memset(&e, 0, sizeof e);
            e.type = CST_EV_LOBBY_CREATED, e.result = 1, e.id = l->id;
            post(u, &e);
            return 1;
        }
    return 0;
}

static int m_lobby_join(void *c, uint64_t id)
{
    mock_user *u = U(c);
    mlobby *l = lobby_of(id);
    cst_event e;
    memset(&e, 0, sizeof e);
    e.type = CST_EV_LOBBY_ENTER, e.id = id;
    int resp = 1;
    if (!l) resp = 2;
    else if (l->n >= l->max) resp = 4;
    else if (is_member(l, u->id)) resp = 1;
    else {
        mock_user *o = user_of(l->owner);
        int invited = 0;
        for (int i = 0; i < u->ninv; i++) invited |= u->invites[i] == id;
        if (l->type == 0 && !invited) resp = 3;                                        /* private: invitation only */
        else if (l->type == 1 && !invited && !(o && o->friend_of[u->idx])) resp = 3;   /* friends only */
    }
    e.result = resp;
    if (resp == 1 && l && !is_member(l, u->id)) {
        l->members[l->n++] = u->id;
        notify_members(l, u->id);
    }
    post(u, &e);
    return 1;
}

static void m_lobby_leave(void *c, uint64_t id)
{
    mock_user *u = U(c);
    mlobby *l = lobby_of(id);
    if (!l) return;
    for (int i = 0; i < l->n; i++)
        if (l->members[i] == u->id) {
            l->members[i] = l->members[--l->n];
            break;
        }
    if (l->n == 0) { l->used = 0; return; }
    if (l->owner == u->id) l->owner = l->members[0];
    notify_members(l, 0);
}

static int m_lobby_search(void *c, const char *k1, const char *v1, const char *k2, const char *v2, int max)
{
    mock_user *u = U(c);
    u->nsearch = 0;
    for (int i = 0; i < ML && u->nsearch < max && u->nsearch < 24; i++) {
        mlobby *l = &W.lobbies[i];
        if (!l->used || l->type != 2 || l->n >= l->max) continue;   /* only public lobbies with room are listed */
        int ok = 1;
        const char *ks[2] = {k1, k2}, *vs[2] = {v1, v2};
        for (int f = 0; f < 2; f++) {
            if (!ks[f]) continue;
            int found = 0;
            for (int d = 0; d < l->nd; d++)
                if (!strcmp(l->k[d], ks[f]) && !strcmp(l->v[d], vs[f])) found = 1;
            ok &= found;
        }
        if (ok) u->search[u->nsearch++] = l->id;
    }
    cst_event e;
    memset(&e, 0, sizeof e);
    e.type = CST_EV_LOBBY_LIST, e.n = u->nsearch;
    post(u, &e);
    return 1;
}
static uint64_t m_lobby_at(void *c, int i) { mock_user *u = U(c); return i >= 0 && i < u->nsearch ? u->search[i] : 0; }

static int m_lobby_set(void *c, uint64_t id, const char *k, const char *v)
{
    mlobby *l = lobby_of(id);
    if (!l || l->owner != U(c)->id) return 0;
    for (int d = 0; d < l->nd; d++)
        if (!strcmp(l->k[d], k)) { snprintf(l->v[d], sizeof l->v[d], "%s", v); return 1; }
    if (l->nd >= MD) return 0;
    snprintf(l->k[l->nd], sizeof l->k[0], "%s", k), snprintf(l->v[l->nd], sizeof l->v[0], "%s", v);
    l->nd++;
    return 1;
}
static int m_lobby_get(void *c, uint64_t id, const char *k, char *out, size_t size)
{
    mlobby *l = lobby_of(id);
    out[0] = 0;
    if (!l || !visible(U(c), l)) return 0;
    for (int d = 0; d < l->nd; d++)
        if (!strcmp(l->k[d], k)) { snprintf(out, size, "%s", l->v[d]); return (int)strlen(out); }
    return 0;
}
static uint64_t m_lobby_owner(void *c, uint64_t id) { mlobby *l = lobby_of(id); return l && visible(U(c), l) ? l->owner : 0; }
static int m_lobby_count(void *c, uint64_t id) { mlobby *l = lobby_of(id); return l && visible(U(c), l) ? l->n : 0; }
static uint64_t m_lobby_member(void *c, uint64_t id, int i) { mlobby *l = lobby_of(id); return l && is_member(l, U(c)->id) && i >= 0 && i < l->n ? l->members[i] : 0; }
static int m_lobby_limit(void *c, uint64_t id) { mlobby *l = lobby_of(id); return l && visible(U(c), l) ? l->max : 0; }
static int m_lobby_invite(void *c, uint64_t id, uint64_t user)
{
    mlobby *l = lobby_of(id);
    mock_user *t = user_of(user);
    if (!l || !t || !is_member(l, U(c)->id) || t->ninv >= 8) return 0;
    t->invites[t->ninv++] = id;
    return 1;
}

static int m_is_friend(void *c, uint64_t id) { mock_user *o = user_of(id); return o && U(c)->friend_of[o->idx]; }
static const char *m_name_of(void *c, uint64_t id) { (void)c; mock_user *o = user_of(id); return o ? o->name : ""; }
static int m_friend_count(void *c)
{
    int n = 0;
    for (int i = 0; i < MU; i++) n += U(c)->friend_of[i];
    return n;
}
static uint64_t m_friend_at(void *c, int k)
{
    for (int i = 0; i < MU; i++)
        if (U(c)->friend_of[i] && k-- == 0) return W.users[i].id;
    return 0;
}
static int m_friend_game(void *c, uint64_t id, uint32_t *app, uint64_t *lobby)
{
    mock_user *f = user_of(id);
    if (!f || !U(c)->friend_of[f->idx] || !f->in_game) return 0;
    *app = APP, *lobby = 0;
    for (int i = 0; i < f->npres; i++)
        if (!strcmp(f->pk[i], "connect")) {
            const char *k = strstr(f->pv[i], "+connect_lobby ");
            if (k) *lobby = strtoull(k + 15, NULL, 10);
        }
    return 1;
}
static int m_presence(void *c, const char *k, const char *v)
{
    mock_user *u = U(c);
    for (int i = 0; i < u->npres; i++)
        if (!strcmp(u->pk[i], k)) {
            if (!v || !v[0]) { u->pk[i][0] = 0, u->pv[i][0] = 0; if (i == u->npres - 1) u->npres--; }
            else snprintf(u->pv[i], sizeof u->pv[0], "%s", v);
            return 1;
        }
    if (!v || !v[0]) return 1;
    if (u->npres >= 8) return 0;
    snprintf(u->pk[u->npres], sizeof u->pk[0], "%s", k), snprintf(u->pv[u->npres], sizeof u->pv[0], "%s", v);
    u->npres++;
    return 1;
}
static void m_presence_clear(void *c) { U(c)->npres = 0; }
static int m_invite_dialog(void *c, uint64_t lobby) { (void)lobby; U(c)->dialog++; return 1; }

/* ---- pipe ---- */
static const char *m_pipe_name(void *c) { (void)c; return "mock"; }

static uint8_t *pair_ref(int a, int b) { return &W.pair[a < b ? a : b][a < b ? b : a]; }   /* 0 none, 2 established, 1 initiated (initiator stored in init[]) */

static void enqueue(mock_user *to, uint32_t due, uint64_t from, int held, const void *d, size_t n)
{
    pkt *k = (pkt *)calloc(1, sizeof *k);
    k->due = due, k->from = from, k->held = held, k->len = n;
    k->data = (uint8_t *)malloc(n);
    memcpy(k->data, d, n);
    pkt **pp = &to->inbox;
    while (*pp && (int32_t)((*pp)->due - due) <= 0) pp = &(*pp)->next;
    k->next = *pp;
    *pp = k;
}
static void release_held(mock_user *at, uint64_t from)
{
    for (pkt *k = at->inbox; k; k = k->next)
        if (k->from == from) k->held = 0;
}

static int m_pipe_send(void *c, uint64_t to, const void *data, size_t len, int reliable)
{
    mock_user *s = U(c), *d = user_of(to);
    if (!d || s->dead) return 0;
    s->n_sent++;
    if (d->dead || s->broken[d->idx] || s->hole[d->idx]) return 1;   /* vanishes */
    uint8_t *ps = pair_ref(s->idx, d->idx);
    int lo = s->idx < d->idx ? s->idx : d->idx, hi = s->idx < d->idx ? d->idx : s->idx;
    int held = 0;
    if (*ps == 0) {
        *ps = 1, pair_init[lo][hi] = s->idx;
        cst_event e;
        memset(&e, 0, sizeof e);
        e.type = CST_EV_SESSION_REQUEST, e.id2 = s->id;
        post(d, &e);
        d->n_sess_req++;
        held = 1;
    } else if (*ps == 1) {
        if (pair_init[lo][hi] == s->idx) held = 1;                 /* still waiting for the other side to accept */
        else if (W.implicit) { *ps = 2; release_held(s, d->id); }  /* a reply to a request is an acceptance */
        else held = 0;
    }
    uint32_t base = W.now + (uint32_t)W.delay + (W.jitter > 0 ? rnd() % (uint32_t)(W.jitter + 1) : 0);
    if (reliable) {
        if ((int32_t)(base - s->last_rel_due[d->idx]) < 0) base = s->last_rel_due[d->idx];
        s->last_rel_due[d->idx] = base;
        enqueue(d, base, s->id, held, data, len);
        return 1;
    }
    if (pct(W.loss)) return 1;
    if (pct(W.reorder)) base += 60;
    enqueue(d, base, s->id, held, data, len);
    if (pct(W.dup)) enqueue(d, base + 1 + rnd() % 20u, s->id, held, data, len);
    return 1;
}

static int m_pipe_recv(void *c, uint64_t *from, void *buf, size_t cap, size_t *len)
{
    mock_user *u = U(c);
    if (u->dead) return 0;
    pkt **pp = &u->inbox;
    while (*pp) {
        pkt *k = *pp;
        if ((int32_t)(k->due - W.now) > 0) break;   /* sorted by due */
        if (!k->held && k->len <= cap) {
            memcpy(buf, k->data, k->len);
            *from = k->from, *len = k->len;
            *pp = k->next;
            free(k->data), free(k);
            u->n_recv++;
            return 1;
        }
        pp = &k->next;
    }
    return 0;
}

static int m_pipe_accept(void *c, uint64_t who)
{
    mock_user *u = U(c), *o = user_of(who);
    if (!o) return 0;
    uint8_t *ps = pair_ref(u->idx, o->idx);
    int lo = u->idx < o->idx ? u->idx : o->idx, hi = u->idx < o->idx ? o->idx : u->idx;
    if (*ps == 1 && pair_init[lo][hi] == o->idx) {
        *ps = 2;
        release_held(u, who);
        return 1;
    }
    return 0;
}
static void m_pipe_close(void *c, uint64_t who)
{
    mock_user *u = U(c), *o = user_of(who);
    if (o) *pair_ref(u->idx, o->idx) = 0;
}
static int m_pipe_state(void *c, uint64_t who, int *ping)
{
    mock_user *u = U(c), *o = user_of(who);
    *ping = -1;
    if (!o) return CST_PS_NONE;
    if (u->broken[o->idx] || o->dead) return CST_PS_FAILED;
    uint8_t st = *pair_ref(u->idx, o->idx);
    if (st == 2) { *ping = 2 * W.delay; return CST_PS_UP; }
    return st == 1 ? CST_PS_CONNECTING : CST_PS_NONE;
}

static const cst_plat mock = {
    "mock", m_init, m_now, m_my_id, m_my_name, m_poll,
    m_lobby_create, m_lobby_join, m_lobby_leave, m_lobby_search, m_lobby_at, m_lobby_set, m_lobby_get, m_lobby_owner, m_lobby_count, m_lobby_member, m_lobby_limit,
    m_lobby_invite, m_is_friend, m_name_of, m_friend_count, m_friend_at, m_friend_game, m_presence, m_presence_clear, m_invite_dialog, NULL,
    m_pipe_name, m_pipe_send, m_pipe_recv, m_pipe_accept, m_pipe_close, m_pipe_state};
const cst_plat *mock_plat(void) { return &mock; }

/* ---------------------------------------------------------------------------------------- inspection and world actions */
const char *mock_lobby_get(uint64_t id, const char *key)
{
    static char b[8][100];
    static int k;
    char *o = b[k++ & 7];
    o[0] = 0;
    mlobby *l = lobby_of(id);
    for (int d = 0; l && d < l->nd; d++)
        if (!strcmp(l->k[d], key)) snprintf(o, 100, "%s", l->v[d]);
    return o;
}
int mock_lobby_count(uint64_t id) { mlobby *l = lobby_of(id); return l ? l->n : 0; }
int mock_lobby_limit(uint64_t id) { mlobby *l = lobby_of(id); return l ? l->max : 0; }
int mock_lobby_type(uint64_t id) { mlobby *l = lobby_of(id); return l ? l->type : -1; }
uint64_t mock_lobby_owner(uint64_t id) { mlobby *l = lobby_of(id); return l ? l->owner : 0; }
int mock_lobby_exists(uint64_t id) { return lobby_of(id) != NULL; }
uint64_t mock_owned_lobby(const mock_user *u)
{
    for (int i = 0; i < ML; i++)
        if (W.lobbies[i].used && W.lobbies[i].owner == u->id) return W.lobbies[i].id;
    return 0;
}
int mock_lobby_has_key_like(uint64_t id, const char *prefix)
{
    mlobby *l = lobby_of(id);
    for (int d = 0; l && d < l->nd; d++)
        if (!strncmp(l->k[d], prefix, strlen(prefix))) return 1;
    return 0;
}
const char *mock_presence(const mock_user *u, const char *key)
{
    for (int i = 0; i < u->npres; i++)
        if (!strcmp(u->pk[i], key)) return u->pv[i];
    return "";
}
int mock_sessions_requested(const mock_user *u) { return u->n_sess_req; }
uint32_t mock_sent(const mock_user *u) { return u->n_sent; }
uint32_t mock_received(const mock_user *u) { return u->n_recv; }

uint64_t mock_foreign_lobby(mock_user *owner, int type, int raid_like)
{
    cst_event e;
    m_lobby_create(owner, type, 2);
    m_poll(owner, &e);
    uint64_t id = e.id;
    if (raid_like) {
        char v[24];
        snprintf(v, sizeof v, "%016llX", (unsigned long long)owner->id);
        m_lobby_set(owner, id, "OwnerId", v);
        m_lobby_set(owner, id, "Name", owner->name);
        m_lobby_set(owner, id, "SlotPublicOpen", "1");
        m_lobby_set(owner, id, "SlotPublicMax", "2");
        m_lobby_set(owner, id, "SearchKeyNum", "7");
        m_lobby_set(owner, id, "SearchKey0", "1");
        m_lobby_set(owner, id, "SearchKey2", "31");
    } else
        m_lobby_set(owner, id, "somegame", "1");
    return id;
}
void mock_click_join_lobby(mock_user *u, uint64_t lobby, mock_user *from)
{
    cst_event e;
    memset(&e, 0, sizeof e);
    e.type = CST_EV_JOIN_LOBBY, e.id = lobby, e.id2 = from ? from->id : 0;
    post(u, &e);
}
void mock_click_join_rich(mock_user *u, mock_user *f, const char *connect)
{
    cst_event e;
    memset(&e, 0, sizeof e);
    e.type = CST_EV_JOIN_RICH, e.id2 = f->id;
    snprintf(e.text, sizeof e.text, "%s", connect);
    post(u, &e);
}
void mock_kill(mock_user *u) { u->dead = 1; }
void mock_leave_all(mock_user *u)
{
    for (int i = 0; i < ML; i++)
        if (W.lobbies[i].used && is_member(&W.lobbies[i], u->id)) m_lobby_leave(u, W.lobbies[i].id);
}
void mock_break_link(mock_user *a, mock_user *b) { a->broken[b->idx] = 1, b->broken[a->idx] = 1; }
void mock_blackhole(mock_user *a, mock_user *b) { a->hole[b->idx] = 1, b->hole[a->idx] = 1; }
void mock_set_in_game(mock_user *u, int in_game) { u->in_game = in_game; }
