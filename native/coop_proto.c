/* Wire protocol encode/decode, see coop_proto.h and docs/protocol.md. Portable C, no I/O. */
#include <math.h>
#include <string.h>

#include "coop_proto.h"

/* ---- bounded writer / reader ---- */
typedef struct { uint8_t *p; size_t cap, len; int err; } W;
typedef struct { const uint8_t *p; size_t len, pos; int err; } R;

static void w8(W *w, unsigned v)
{
    if (w->len + 1 > w->cap) { w->err = 1; return; }
    w->p[w->len++] = (uint8_t)v;
}
static void w16(W *w, unsigned v) { w8(w, v & 0xFF); w8(w, (v >> 8) & 0xFF); }
static void w32(W *w, uint32_t v) { w16(w, v & 0xFFFF); w16(w, v >> 16); }
static void wf(W *w, float f)
{
    uint32_t u;
    if (!(f == f) || f > 3.0e38f || f < -3.0e38f) f = 0;
    memcpy(&u, &f, 4);
    w32(w, u);
}
static void wbytes(W *w, const void *b, size_t n)
{
    if (w->len + n > w->cap) { w->err = 1; return; }
    memcpy(w->p + w->len, b, n);
    w->len += n;
}
static void wstr(W *w, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 255) n = 255;
    w8(w, (unsigned)n);
    wbytes(w, s, n);
}
static int16_t quant(float v, float scale, int lim)
{
    if (!(v == v)) return 0;
    float x = v * scale;
    if (x > (float)lim) x = (float)lim;
    if (x < (float)-lim) x = (float)-lim;
    return (int16_t)lrintf(x);
}

static unsigned r8(R *r)
{
    if (r->pos + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->pos++];
}
static unsigned r16(R *r) { unsigned a = r8(r); return a | (r8(r) << 8); }
static uint32_t r32(R *r) { uint32_t a = r16(r); return a | ((uint32_t)r16(r) << 16); }
static float rf(R *r)
{
    uint32_t u = r32(r);
    float f;
    memcpy(&f, &u, 4);
    if (!(f == f)) f = 0;
    return f;
}
static void rbytes(R *r, void *b, size_t n)
{
    if (r->pos + n > r->len) { r->err = 1; memset(b, 0, n); return; }
    memcpy(b, r->p + r->pos, n);
    r->pos += n;
}
static void rstr(R *r, char *out, size_t cap)   /* cap includes the NUL; longer strings are cut, bytes < 32 become '?' */
{
    size_t n = r8(r), keep = n < cap - 1 ? n : cap - 1;
    if (r->pos + n > r->len) { r->err = 1; out[0] = 0; return; }
    for (size_t i = 0; i < keep; i++) {
        unsigned char c = r->p[r->pos + i];
        out[i] = c < 32 ? '?' : (char)c;
    }
    out[keep] = 0;
    r->pos += n;
}
/* an optional string at the end of a message (a field added later, see the forward rule in coop_proto.h): missing or cut = "", never an error, so an
 * older sender's message (without it) still decodes; whatever follows a complete one is ignored as usual */
static void ropt_str(R *r, char *out, size_t cap)
{
    out[0] = 0;
    if (r->err || r->pos >= r->len) return;
    size_t n = r->p[r->pos];
    if (r->pos + 1 + n > r->len) return;
    rstr(r, out, cap);
}

/* ---- blocks ---- */
static void w_session(W *w, const cp_session *s)
{
    w32(w, s->id);
    w8(w, s->game_type), w8(w, s->episode), w8(w, s->part), w8(w, s->difficulty), w8(w, s->max_players), w8(w, s->flags);
    w32(w, s->area_id);
    w32(w, s->seed);
}
static void r_session(R *r, cp_session *s)
{
    s->id = r32(r);
    s->game_type = (uint8_t)r8(r), s->episode = (uint8_t)r8(r), s->part = (uint8_t)r8(r), s->difficulty = (uint8_t)r8(r), s->max_players = (uint8_t)r8(r), s->flags = (uint8_t)r8(r);
    s->area_id = r32(r);
    s->seed = r32(r);
}
static void w_lobby(W *w, const cp_lobby *l)
{
    w32(w, l->rev);
    unsigned n = l->count > CP_MAX_PLAYERS ? CP_MAX_PLAYERS : l->count;
    w8(w, n);
    for (unsigned i = 0; i < n; i++) {
        w8(w, l->p[i].pid), w8(w, l->p[i].character), w8(w, l->p[i].ready), w8(w, l->p[i].flags);
        wstr(w, l->p[i].name);
    }
}
static void r_lobby(R *r, cp_lobby *l)
{
    memset(l, 0, sizeof *l);
    l->rev = r32(r);
    unsigned n = r8(r);
    if (n > CP_MAX_PLAYERS) { r->err = 1; return; }
    l->count = (uint8_t)n;
    for (unsigned i = 0; i < n; i++) {
        l->p[i].pid = (uint8_t)r8(r), l->p[i].character = (uint8_t)r8(r), l->p[i].ready = (uint8_t)r8(r), l->p[i].flags = (uint8_t)r8(r);
        rstr(r, l->p[i].name, sizeof l->p[i].name);
    }
}

static void w_state(W *w, const cp_state *s)
{
    float yaw = s->yaw;
    if (!(yaw == yaw)) yaw = 0;
    yaw = fmodf(yaw, 6.2831853f);
    if (yaw > 3.1415927f) yaw -= 6.2831853f;
    if (yaw < -3.1415927f) yaw += 6.2831853f;
    w8(w, CP_STATE), w8(w, s->pid), w16(w, s->seq), w32(w, s->t_ms);
    wf(w, s->pos[0]), wf(w, s->pos[1]), wf(w, s->pos[2]);
    w16(w, (uint16_t)quant(yaw, 10000.0f, 31416));
    {
        unsigned act = s->action == 0x80000000u ? 0xFFFFu : (s->action & 0xFFFFu);
        if (act == 0xFFFFu && s->action != 0x80000000u) act = 0xFFFEu;   /* 0xFFFF is reserved for "waiting" */
        w16(w, act);
    }
    w16(w, (uint16_t)s->hp), w16(w, (uint16_t)s->hpmax);
    w16(w, (uint16_t)quant(s->aim, 100.0f, 32767));
    for (int i = 0; i < 3; i++) w16(w, (uint16_t)quant(s->aimv[i], 16384.0f, 32767));
    w8(w, s->weapon), w8(w, s->epoch), w16(w, s->flags);
    w32(w, s->pstate), w32(w, s->ctrl);
    for (int i = 0; i < 3; i++) w16(w, s->item[i]);
    w8(w, s->move), w8(w, 0), w16(w, (uint16_t)s->elev), w16(w, 0);
}
static void r_state(R *r, cp_state *s)
{
    memset(s, 0, sizeof *s);
    r8(r);   /* type */
    s->pid = (uint8_t)r8(r), s->seq = (uint16_t)r16(r), s->t_ms = r32(r);
    s->pos[0] = rf(r), s->pos[1] = rf(r), s->pos[2] = rf(r);
    s->yaw = (float)(int16_t)r16(r) / 10000.0f;
    unsigned a = r16(r);
    s->action = a == 0xFFFFu ? 0x80000000u : a;
    s->hp = (int16_t)r16(r), s->hpmax = (int16_t)r16(r);
    s->aim = (float)(int16_t)r16(r) / 100.0f;
    for (int i = 0; i < 3; i++) s->aimv[i] = (float)(int16_t)r16(r) / 16384.0f;
    s->weapon = (uint8_t)r8(r), s->epoch = (uint8_t)r8(r), s->flags = (uint16_t)r16(r);
    s->pstate = r32(r), s->ctrl = r32(r);
    for (int i = 0; i < 3; i++) s->item[i] = (uint16_t)r16(r);
    s->move = (uint8_t)r8(r), r8(r);
    s->elev = (int16_t)r16(r);
    r16(r);
}

/* PAD: see coop_proto.h. The masks name the fields that differ from the previous record of the packet. */
enum { PM_BUTTONS = 1, PM_LT = 2, PM_RT = 4, PM_LX = 8, PM_LY = 16, PM_RX = 32, PM_RY = 64, PM_RESERVED = 128 };

static unsigned pad_mask(const cp_pad_rec *a, const cp_pad_rec *p)
{
    return (a->buttons != p->buttons ? PM_BUTTONS : 0u) | (a->lt != p->lt ? PM_LT : 0u) | (a->rt != p->rt ? PM_RT : 0u) | (a->lx != p->lx ? PM_LX : 0u) | (a->ly != p->ly ? PM_LY : 0u) |
           (a->rx != p->rx ? PM_RX : 0u) | (a->ry != p->ry ? PM_RY : 0u);
}
static int w_pad(W *w, const cp_pad *d)
{
    if (d->count < 1 || d->count > CP_PAD_MAX_REC) return 0;
    w8(w, CP_PAD), w8(w, d->pid), w8(w, d->epoch), w16(w, d->flags), w16(w, d->seq), w32(w, d->t_ms), w8(w, d->count);
    cp_pad_rec prev;
    memset(&prev, 0, sizeof prev);
    for (unsigned i = 0; i < d->count; i++) {
        const cp_pad_rec *r = &d->rec[i];
        unsigned m = pad_mask(r, &prev);
        if (i) w8(w, d->dt[i]);
        w8(w, m);
        if (m & PM_BUTTONS) w16(w, r->buttons);
        if (m & PM_LT) w8(w, r->lt);
        if (m & PM_RT) w8(w, r->rt);
        if (m & PM_LX) w16(w, (uint16_t)r->lx);
        if (m & PM_LY) w16(w, (uint16_t)r->ly);
        if (m & PM_RX) w16(w, (uint16_t)r->rx);
        if (m & PM_RY) w16(w, (uint16_t)r->ry);
        prev = *r;
    }
    return 1;
}
static void r_pad(R *r, cp_pad *d)
{
    memset(d, 0, sizeof *d);
    r8(r);   /* type */
    d->pid = (uint8_t)r8(r), d->epoch = (uint8_t)r8(r), d->flags = (uint16_t)r16(r), d->seq = (uint16_t)r16(r), d->t_ms = r32(r), d->count = (uint8_t)r8(r);
    if (d->count < 1 || d->count > CP_PAD_MAX_REC) { r->err = 1; return; }
    cp_pad_rec prev;
    memset(&prev, 0, sizeof prev);
    for (unsigned i = 0; i < d->count; i++) {
        if (i) d->dt[i] = (uint8_t)r8(r);
        unsigned m = r8(r);
        if (m & PM_RESERVED) { r->err = 1; return; }
        cp_pad_rec c = prev;
        if (m & PM_BUTTONS) c.buttons = (uint16_t)r16(r);
        if (m & PM_LT) c.lt = (uint8_t)r8(r);
        if (m & PM_RT) c.rt = (uint8_t)r8(r);
        if (m & PM_LX) c.lx = (int16_t)r16(r);
        if (m & PM_LY) c.ly = (int16_t)r16(r);
        if (m & PM_RX) c.rx = (int16_t)r16(r);
        if (m & PM_RY) c.ry = (int16_t)r16(r);
        d->rec[i] = c;
        prev = c;
    }
}

/* ---- messages ---- */
size_t cp_encode(const cp_msg *m, uint8_t *buf, size_t cap)
{
    W w = {buf, cap, 0, 0};
    switch (m->type) {
    case CP_HELLO:
        w8(&w, CP_HELLO), w32(&w, CP_MAGIC), w16(&w, m->u.hello.version), w16(&w, m->u.hello.min_version);
        wbytes(&w, m->u.hello.exe_tag, 8);
        w8(&w, m->u.hello.want_char), w8(&w, m->u.hello.flags);
        wstr(&w, m->u.hello.name), wstr(&w, m->u.hello.mod);
        break;
    case CP_WELCOME:
        w8(&w, CP_WELCOME), w16(&w, m->u.welcome.version), w8(&w, m->u.welcome.pid), w8(&w, m->u.welcome.flags), w32(&w, m->u.welcome.host_ms);
        w8(&w, m->u.welcome.phase), w8(&w, m->u.welcome.epoch);
        w_session(&w, &m->u.welcome.session);
        w_lobby(&w, &m->u.welcome.lobby);
        wstr(&w, m->u.welcome.mod);   /* tail (0.2.0) */
        break;
    case CP_REFUSE:
    case CP_NOTICE:
        w8(&w, m->type), w8(&w, m->u.refuse.code), wstr(&w, m->u.refuse.text);
        if (m->type == CP_REFUSE) wstr(&w, m->u.refuse.mod);   /* tail (0.2.0) */
        break;
    case CP_SESSION:
        w8(&w, CP_SESSION), w_session(&w, &m->u.session);
        break;
    case CP_LOBBY_PICK:
        w8(&w, CP_LOBBY_PICK), w8(&w, m->u.pick.character);
        break;
    case CP_LOBBY_READY:
        w8(&w, CP_LOBBY_READY), w8(&w, m->u.ready.ready);
        break;
    case CP_LOBBY_STATE:
        w8(&w, CP_LOBBY_STATE), w_lobby(&w, &m->u.lobby);
        break;
    case CP_START:
        w8(&w, CP_START), w_session(&w, &m->u.start.session), w_lobby(&w, &m->u.start.lobby);
        break;
    case CP_LOAD_BEGIN:
        w8(&w, CP_LOAD_BEGIN), w8(&w, m->u.load.pid), w8(&w, m->u.load.epoch), w32(&w, m->u.load.area_id);
        break;
    case CP_LOAD_DONE:
        w8(&w, CP_LOAD_DONE), w8(&w, m->u.load.pid), w8(&w, m->u.load.epoch);
        break;
    case CP_GO:
        w8(&w, CP_GO), w8(&w, m->u.load.epoch);
        break;
    case CP_EVENT: {
        const cp_event *e = &m->u.event;
        if (e->len > CP_EVENT_MAX) return 0;
        w8(&w, CP_EVENT), w8(&w, e->pid), w16(&w, e->kind), w16(&w, e->eseq), w32(&w, e->t_ms), w8(&w, e->epoch), w8(&w, e->len);
        wbytes(&w, e->body, e->len);
        break;
    }
    case CP_WORLD: {
        const cp_world *wd = &m->u.world;
        if (wd->len > CP_WORLD_MAX) return 0;
        w8(&w, CP_WORLD), w8(&w, wd->pid), w8(&w, wd->kind), w16(&w, wd->seq), w32(&w, wd->t_ms), w8(&w, wd->epoch), w16(&w, wd->len);
        wbytes(&w, wd->body, wd->len);
        break;
    }
    case CP_PAD:
        if (!w_pad(&w, &m->u.pad)) return 0;
        break;
    case CP_HEARTBEAT: {
        const cp_heartbeat *h = &m->u.hb;
        w8(&w, CP_HEARTBEAT), w8(&w, h->pid), w32(&w, h->t_ms), w32(&w, h->echo_t_ms), w32(&w, h->echo_dwell_ms), w8(&w, h->phase), w8(&w, h->epoch), w16(&w, h->state_seq);
        break;
    }
    case CP_LEAVE:
        w8(&w, CP_LEAVE), w8(&w, m->u.leave.reason);
        break;
    case CP_STATE:
        w_state(&w, &m->u.state);
        break;
    default:
        return 0;
    }
    return w.err ? 0 : w.len;
}

int cp_decode(const uint8_t *buf, size_t len, cp_msg *m)
{
    memset(m, 0, sizeof *m);
    if (!buf || len < 1) return 0;
    R r = {buf, len, 0, 0};
    unsigned type = r8(&r);
    m->type = (uint8_t)type;
    switch (type) {
    case CP_HELLO: {
        cp_hello *h = &m->u.hello;
        if (r32(&r) != CP_MAGIC) return 0;
        h->version = (uint16_t)r16(&r), h->min_version = (uint16_t)r16(&r);
        rbytes(&r, h->exe_tag, 8);
        h->want_char = (uint8_t)r8(&r), h->flags = (uint8_t)r8(&r);
        rstr(&r, h->name, sizeof h->name), rstr(&r, h->mod, sizeof h->mod);
        break;
    }
    case CP_WELCOME:
        m->u.welcome.version = (uint16_t)r16(&r), m->u.welcome.pid = (uint8_t)r8(&r), m->u.welcome.flags = (uint8_t)r8(&r), m->u.welcome.host_ms = r32(&r);
        m->u.welcome.phase = (uint8_t)r8(&r), m->u.welcome.epoch = (uint8_t)r8(&r);
        r_session(&r, &m->u.welcome.session);
        r_lobby(&r, &m->u.welcome.lobby);
        ropt_str(&r, m->u.welcome.mod, sizeof m->u.welcome.mod);
        break;
    case CP_REFUSE:
    case CP_NOTICE:
        m->u.refuse.code = (uint8_t)r8(&r);
        rstr(&r, m->u.refuse.text, sizeof m->u.refuse.text);
        if (type == CP_REFUSE) ropt_str(&r, m->u.refuse.mod, sizeof m->u.refuse.mod);
        break;
    case CP_SESSION:
        r_session(&r, &m->u.session);
        break;
    case CP_LOBBY_PICK:
        m->u.pick.character = (uint8_t)r8(&r);
        break;
    case CP_LOBBY_READY:
        m->u.ready.ready = (uint8_t)r8(&r);
        break;
    case CP_LOBBY_STATE:
        r_lobby(&r, &m->u.lobby);
        break;
    case CP_START:
        r_session(&r, &m->u.start.session);
        r_lobby(&r, &m->u.start.lobby);
        break;
    case CP_LOAD_BEGIN:
        m->u.load.pid = (uint8_t)r8(&r), m->u.load.epoch = (uint8_t)r8(&r), m->u.load.area_id = r32(&r);
        break;
    case CP_LOAD_DONE:
        m->u.load.pid = (uint8_t)r8(&r), m->u.load.epoch = (uint8_t)r8(&r);
        break;
    case CP_GO:
        m->u.load.epoch = (uint8_t)r8(&r);
        break;
    case CP_EVENT: {
        cp_event *e = &m->u.event;
        e->pid = (uint8_t)r8(&r), e->kind = (uint16_t)r16(&r), e->eseq = (uint16_t)r16(&r), e->t_ms = r32(&r), e->epoch = (uint8_t)r8(&r), e->len = (uint8_t)r8(&r);
        if (e->len > CP_EVENT_MAX) return 0;
        rbytes(&r, e->body, e->len);
        break;
    }
    case CP_WORLD: {
        cp_world *wd = &m->u.world;
        wd->pid = (uint8_t)r8(&r), wd->kind = (uint8_t)r8(&r), wd->seq = (uint16_t)r16(&r), wd->t_ms = r32(&r), wd->epoch = (uint8_t)r8(&r), wd->len = (uint16_t)r16(&r);
        if (wd->len > CP_WORLD_MAX) return 0;
        rbytes(&r, wd->body, wd->len);
        break;
    }
    case CP_PAD:
        r.pos = 0;
        r_pad(&r, &m->u.pad);
        break;
    case CP_HEARTBEAT: {
        cp_heartbeat *h = &m->u.hb;
        h->pid = (uint8_t)r8(&r), h->t_ms = r32(&r), h->echo_t_ms = r32(&r), h->echo_dwell_ms = r32(&r), h->phase = (uint8_t)r8(&r), h->epoch = (uint8_t)r8(&r), h->state_seq = (uint16_t)r16(&r);
        break;
    }
    case CP_LEAVE:
        m->u.leave.reason = (uint8_t)r8(&r);
        break;
    case CP_STATE:
        r.pos = 0;
        r_state(&r, &m->u.state);
        break;
    default:
        return 0;
    }
    if (r.err) {
        memset(m, 0, sizeof *m);
        return 0;
    }
    return 1;
}

const char *cp_type_name(unsigned t)
{
    switch (t) {
    case CP_HELLO: return "HELLO";
    case CP_WELCOME: return "WELCOME";
    case CP_REFUSE: return "REFUSE";
    case CP_SESSION: return "SESSION";
    case CP_LOBBY_PICK: return "LOBBY_PICK";
    case CP_LOBBY_READY: return "LOBBY_READY";
    case CP_LOBBY_STATE: return "LOBBY_STATE";
    case CP_START: return "START";
    case CP_NOTICE: return "NOTICE";
    case CP_LOAD_BEGIN: return "LOAD_BEGIN";
    case CP_LOAD_DONE: return "LOAD_DONE";
    case CP_GO: return "GO";
    case CP_EVENT: return "EVENT";
    case CP_HEARTBEAT: return "HEARTBEAT";
    case CP_LEAVE: return "LEAVE";
    case CP_STATE: return "STATE";
    case CP_WORLD: return "WORLD";
    case CP_PAD: return "PAD";
    default: return "?";
    }
}

const char *cp_char_name(unsigned c)
{
    switch (c) {
    case CP_CHAR_LEAD: return "lead";
    case CP_CHAR_PARTNER: return "partner";
    case CP_CHAR_LEAD_B: return "lead-b";
    case CP_CHAR_PARTNER_B: return "partner-b";
    default: return "none";
    }
}

int cp_char_parse(const char *s)
{
    /* the Revelations 1 role names, then the Revelations 2 ones (kept so that old scripts and the shared tests still parse) */
    static const char *names[] = {"lead", "partner", "lead-b", "partner-b", "claire", "moira", "barry", "natalia"};
    if (!s) return CP_CHAR_NONE;
    for (int i = 0; i < 8; i++) {
        const char *a = s, *b = names[i];
        while (*a && *b && ((*a | 0x20) == *b)) a++, b++;
        if (!*a && !*b) return i & 3;
    }
    if (s[0] >= '0' && s[0] <= '3' && !s[1]) return s[0] - '0';
    return CP_CHAR_NONE;
}

const char *cp_refuse_text(unsigned code)
{
    switch (code) {
    case CP_REF_VERSION: return "different co-op protocol version";
    case CP_REF_BUILD: return "different game build";
    case CP_REF_FULL: return "session is full";
    case CP_REF_STARTED: return "the game already started";
    case CP_REF_BAD: return "bad request";
    case CP_REF_MODVER: return "different co-op mod version";
    case CP_REF_REMOVED: return "removed by the host";
    default: return "refused";
    }
}
