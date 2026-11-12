/*
 * give1.h - RE5-style "give to partner" from the game's own item screen (Revelations 1, 0.2.0). Pure: no Windows, no engine, no session; give1.c is the glue,
 * tests/test_give1.c drives it. Design and the static RE: notes/give1.md.
 *
 * What moves: COUNTS only - a herb (bag +0xC8 mHerbNum), a grenade (bag +0xA0 mSubWeapon[type]) or ammo (bag +0x40 mBullet[type]). The rule never writes a bag slot
 * word (+4..+0x3C) and never moves a weapon object between bags (packcopy / lobby-must 11: one object in two bags crashed the reload). A grenade type the receiver
 * holds no unit of gets a NEW unit of his own through the stock add (FUN_005C7B10), on the receiver's PC.
 *
 * Who writes what: split screen (both humans on one PC): one pass moves n from the giver to the receiver (gv_split). Network: every PC writes ONLY its own human's bag:
 * the giver removes n and sends GIVE; the receiver's PC adds what fits and answers ACK(taken); the giver gives back n - taken. A GIVE without an answer for GV_TIMEOUT_MS
 * is cancelled (CANCEL -> CANCELACK(taken)): the refund follows the receiver's word only, never a guess.
 * Level changes (every message carries the level instance AND the section id 0x91EESS00 of FUN_00720320): a RELOAD (Continue / retry / checkpoint: a new instance of
 * the SAME section) restores both bags from the checkpoint, so an open give is dropped without a refund and a GIVE of the old instance is never applied; a SECTION
 * change (a new instance of ANOTHER section) carries both bags over, so the give is completed: the receiver applies a late GIVE, the giver asks again at once
 * (CANCEL = "did you apply #id?") and refunds what the receiver says it did not take. The receiver keeps every answered id for the whole session.
 */
#ifndef GIVE1_H
#define GIVE1_H

#include <stdint.h>
#include <string.h>

#define GV_EV_KIND 0x144u          /* CP_EV_USER + 0x44: a reliable session event (0x140 world, 0x141 role barrier, 0x142 downed, 0x143 pre-load digest are taken) */
#define GV_REACH 300.0f            /* 3 m horizontally (units are cm: downed1's revive reach is 170) */
#define GV_REACH_DY 200.0f         /* the same floor */
#define GV_TIMEOUT_MS 6000u        /* no answer -> CANCEL (and again every GV_TIMEOUT_MS) */
#define GV_PEND_MAX 8
#define GV_RING 256                /* answered ids of the session (gives are rare: a few per section) */
#define GV_MSG_LEN 24
#define GV_VERSION 2

/* the item screen's ids (uGUI_SubMenu +0x2B4 lists; table 0x107E580: name msg, desc msg, class) */
enum { GV_K_NONE = 0, GV_K_HERB = 1, GV_K_AMMO = 2, GV_K_GRENADE = 3 };
enum { GV_TAB_ITEMS = 0, GV_TAB_AMMO = 1 };
typedef struct { int id, kind, tab; uint32_t cls; int index; } gv_item;
static const gv_item GV_ITEMS[] = {
    {0x47, GV_K_HERB, GV_TAB_ITEMS, 0x80040101u, 0},      /* Green Herb (mHerbNum; max = max(0x80040101)) */
    {0x32, GV_K_GRENADE, GV_TAB_ITEMS, 0x80051900u, 0},   /* Hand Grenade */
    {0x33, GV_K_GRENADE, GV_TAB_ITEMS, 0x80051940u, 1},   /* Shock Grenade */
    {0x34, GV_K_GRENADE, GV_TAB_ITEMS, 0x80051950u, 2},   /* B.O.W. Decoy */
    {0x35, GV_K_GRENADE, GV_TAB_ITEMS, 0x80051960u, 3},   /* Pulse Grenade */
    {0x50, GV_K_AMMO, GV_TAB_AMMO, 0x80040200u, 0},       /* Handgun Ammo   mBullet[0] */
    {0x53, GV_K_AMMO, GV_TAB_AMMO, 0x80040201u, 1},       /* Shotgun Shells mBullet[1] */
    {0x52, GV_K_AMMO, GV_TAB_AMMO, 0x80040202u, 2},       /* Machine Gun Ammo mBullet[2] */
    {0x54, GV_K_AMMO, GV_TAB_AMMO, 0x80040203u, 3},       /* Rifle Ammo     mBullet[3] */
    {0x51, GV_K_AMMO, GV_TAB_AMMO, 0x80040204u, 4},       /* Magnum Ammo    mBullet[4] */
};
#define GV_NITEMS ((int)(sizeof GV_ITEMS / sizeof GV_ITEMS[0]))

static inline const gv_item *gv_item_by_id(int tab, int id)
{
    for (int i = 0; i < GV_NITEMS; i++)
        if (GV_ITEMS[i].id == id && GV_ITEMS[i].tab == tab) return &GV_ITEMS[i];
    return NULL;
}
static inline const gv_item *gv_item_by_cls(int kind, uint32_t cls)
{
    for (int i = 0; i < GV_NITEMS; i++)
        if (GV_ITEMS[i].kind == kind && GV_ITEMS[i].cls == cls) return &GV_ITEMS[i];
    return NULL;
}

/* the counts of one bag that the rule needs (the glue reads them with the stock getters) */
typedef struct {
    int herb, herb_max;
    int ammo[5], ammo_max[5];
    int gren[4], gren_max[4];
} gv_bag;
static inline int gv_have(const gv_bag *b, const gv_item *it)
{
    if (!b || !it) return 0;
    return it->kind == GV_K_HERB ? b->herb : it->kind == GV_K_AMMO ? b->ammo[it->index] : it->kind == GV_K_GRENADE ? b->gren[it->index] : 0;
}
static inline int gv_max(const gv_bag *b, const gv_item *it)
{
    if (!b || !it) return 0;
    return it->kind == GV_K_HERB ? b->herb_max : it->kind == GV_K_AMMO ? b->ammo_max[it->index] : it->kind == GV_K_GRENADE ? b->gren_max[it->index] : 0;
}
static inline int gv_room(const gv_bag *b, const gv_item *it)
{
    int r = gv_max(b, it) - gv_have(b, it);
    return r > 0 ? r : 0;
}

/* what one press offers: a herb 1, a grenade 1, ammo half of what the giver carries (rounded up) */
static inline int gv_offer(int kind, int have)
{
    if (have <= 0) return 0;
    if (kind == GV_K_AMMO) return (have + 1) / 2;
    return kind == GV_K_HERB || kind == GV_K_GRENADE ? 1 : 0;
}

/* ---- when the entry shows ---- */
enum { GV_MODE_NONE = 0, GV_MODE_SPLIT = 1, GV_MODE_NET = 2 };
enum {
    GV_WHY_OK = 0, GV_WHY_NOCOOP, GV_WHY_TAB, GV_WHY_ITEM, GV_WHY_EMPTY, GV_WHY_NOPARTNER, GV_WHY_DOWN, GV_WHY_FAR, GV_WHY_GIVER, GV_WHY_BUSY,
    GV_WHY_FULL, GV_WHY_NOW, GV_WHY_EPOCH, GV_WHY_CANCELLED, GV_WHY_BAD, GV_WHY_TRANSIT, GV_WHY_COUNT
};
static inline const char *gv_why_name(int w)
{
    static const char *const n[GV_WHY_COUNT] = {"ok", "no co-op session", "not the Items / Ammunition tab", "not a herb, grenade or ammo", "none left",
                                                "no partner (solo part / not placed)", "the partner is down or dead", "the partner is too far", "the giver is down",
                                                "a give is still open", "the partner's bag is full", "the partner cannot take it now", "a reload since the give",
                                                "cancelled", "bad message", "a level change is under way"};
    return w >= 0 && w < GV_WHY_COUNT ? n[w] : "?";
}
typedef struct {
    int mode;                 /* GV_MODE_* */
    int tab, item_id;         /* the screen's tab and the cell's id */
    int have;                 /* the giver's count of that item */
    int giver_up;             /* the giver is not downed / dead */
    int partner_present;      /* the partner's unit exists (not a solo part) */
    int partner_up;           /* not downed, not dead (split: hp > 1; network: no DOWN / DEAD flag) */
    float dx, dy, dz;         /* partner - giver */
    int busy;                 /* network: a give of this PC is still open */
    int transit;              /* a level change is under way (the barrier's shield after a restart, this PC or the partner loading) */
} gv_ctx;
static inline int gv_check(const gv_ctx *c, const gv_item **out)
{
    if (out) *out = NULL;
    if (!c || (c->mode != GV_MODE_SPLIT && c->mode != GV_MODE_NET)) return GV_WHY_NOCOOP;
    if (c->tab != GV_TAB_ITEMS && c->tab != GV_TAB_AMMO) return GV_WHY_TAB;
    const gv_item *it = gv_item_by_id(c->tab, c->item_id);
    if (!it) return GV_WHY_ITEM;
    if (out) *out = it;
    if (c->have <= 0) return GV_WHY_EMPTY;
    if (c->transit) return GV_WHY_TRANSIT;
    if (!c->giver_up) return GV_WHY_GIVER;
    if (!c->partner_present) return GV_WHY_NOPARTNER;
    if (!c->partner_up) return GV_WHY_DOWN;
    if (!(c->dx * c->dx + c->dz * c->dz <= GV_REACH * GV_REACH) || !(c->dy <= GV_REACH_DY && c->dy >= -GV_REACH_DY)) return GV_WHY_FAR;   /* NaN -> far */
    if (c->busy) return GV_WHY_BUSY;
    return GV_WHY_OK;
}

/* ---- split screen: one pass, both bags known ---- */
typedef struct { int n, why; } gv_plan;
static inline gv_plan gv_split(const gv_item *it, const gv_bag *giver, const gv_bag *recv)
{
    gv_plan p = {0, GV_WHY_OK};
    int offer = gv_offer(it ? it->kind : 0, gv_have(giver, it)), room = gv_room(recv, it);
    if (!it) p.why = GV_WHY_ITEM;
    else if (offer <= 0) p.why = GV_WHY_EMPTY;
    else if (room <= 0) p.why = GV_WHY_FULL;
    else p.n = offer < room ? offer : room;
    return p;
}

/* ---- the wire (session event body, little endian) ---- */
enum { GV_M_GIVE = 1, GV_M_ACK = 2, GV_M_CANCEL = 3, GV_M_CANCELACK = 4 };
typedef struct {
    uint8_t type;
    uint32_t id;              /* (giver slot << 24) | counter: unique in the session */
    uint8_t epoch;            /* the giver's level instance at the give (low byte) */
    uint32_t sect;            /* the giver's section at the give (FUN_00720320: 0x91EESS00) */
    uint8_t from, to;         /* human slots */
    uint8_t kind;
    uint32_t cls;
    uint16_t n, taken;
    uint8_t why;
} gv_msg;
static inline void gv_w32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v, b[1] = (uint8_t)(v >> 8), b[2] = (uint8_t)(v >> 16), b[3] = (uint8_t)(v >> 24); }
static inline uint32_t gv_r32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static inline unsigned gv_enc(uint8_t *b, const gv_msg *m)
{
    memset(b, 0, GV_MSG_LEN);
    b[0] = GV_VERSION, b[1] = m->type;
    gv_w32(b + 2, m->id);
    b[6] = m->epoch, b[7] = m->from, b[8] = m->to, b[9] = m->kind;
    gv_w32(b + 10, m->cls);
    b[14] = (uint8_t)m->n, b[15] = (uint8_t)(m->n >> 8), b[16] = (uint8_t)m->taken, b[17] = (uint8_t)(m->taken >> 8), b[18] = m->why;
    gv_w32(b + 19, m->sect);
    return GV_MSG_LEN;
}
static inline int gv_dec(const uint8_t *b, unsigned len, gv_msg *m)
{
    if (!b || !m || len != GV_MSG_LEN || b[0] != GV_VERSION || b[1] < GV_M_GIVE || b[1] > GV_M_CANCELACK) return 0;
    memset(m, 0, sizeof *m);
    m->type = b[1], m->id = gv_r32(b + 2), m->epoch = b[6], m->from = b[7], m->to = b[8], m->kind = b[9], m->cls = gv_r32(b + 10);
    m->n = (uint16_t)(b[14] | b[15] << 8), m->taken = (uint16_t)(b[16] | b[17] << 8), m->why = b[18], m->sect = gv_r32(b + 19);
    if (m->from > 1 || m->to > 1 || m->from == m->to || !gv_item_by_cls(m->kind, m->cls) || m->taken > m->n) return 0;
    return 1;
}
/* a level change between two moments, from the level instance and the section of each: 0 none, 1 a SECTION change (bags carried over), 2 a RELOAD (the same
 * section again: bags from the checkpoint) */
enum { GV_LC_NONE = 0, GV_LC_SECTION = 1, GV_LC_RELOAD = 2 };
static inline int gv_level_change(uint8_t ep_then, uint32_t sect_then, uint8_t ep_now, uint32_t sect_now)
{
    if (ep_then == ep_now) return GV_LC_NONE;
    return sect_then == sect_now ? GV_LC_RELOAD : GV_LC_SECTION;
}

/* ---- the giver's side (network) ---- */
typedef struct { int used, cancelling, asked, tries; uint32_t id, t0, tsent; uint8_t epoch; uint32_t sect; int kind; uint32_t cls; int n; } gv_pend;
typedef struct {
    gv_pend p[GV_PEND_MAX];
    uint32_t counter;
    int me;
    unsigned sent, answered, refunded_units, refusals, cancels, dropped_reload, asked_after_section, lost_session, stray;
} gv_giver;
static inline void gv_giver_init(gv_giver *g, int me) { memset(g, 0, sizeof *g), g->me = me; }
static inline int gv_giver_open(const gv_giver *g)
{
    int n = 0;
    for (int i = 0; i < GV_PEND_MAX; i++) n += g->p[i].used != 0;
    return n;
}
/* the giver has ALREADY removed n from its own bag: record it and build the GIVE. 0 = table full (the caller must put the units back) */
static inline int gv_giver_begin(gv_giver *g, uint8_t epoch, uint32_t sect, int kind, uint32_t cls, int n, uint32_t now, gv_msg *out)
{
    int k = -1;
    for (int i = 0; i < GV_PEND_MAX; i++)
        if (!g->p[i].used) { k = i; break; }
    if (k < 0 || n <= 0 || n > 0xFFFF || !gv_item_by_cls(kind, cls) || (g->me != 0 && g->me != 1)) return 0;
    gv_pend *p = &g->p[k];
    memset(p, 0, sizeof *p);
    p->used = 1, p->id = ((uint32_t)g->me << 24) | (++g->counter & 0xFFFFFFu), p->t0 = p->tsent = now, p->epoch = epoch, p->sect = sect, p->kind = kind, p->cls = cls, p->n = n;
    memset(out, 0, sizeof *out);
    out->type = GV_M_GIVE, out->id = p->id, out->epoch = epoch, out->sect = sect, out->from = (uint8_t)g->me, out->to = (uint8_t)(1 - g->me), out->kind = (uint8_t)kind,
    out->cls = cls, out->n = (uint16_t)n;
    g->sent++;
    return 1;
}
/* an ACK / CANCELACK for this PC, read where this PC's level is known (cur_epoch / cur_sect = now). Returns the units to give back to the own human, fills
 * *kind / *cls; *closed = the give is finished; *why = the receiver's reason. A give from before a RELOAD refunds nothing (the reload gave the units back);
 * across a SECTION change the refund is due (the bag was carried). Answers to unknown ids (a second answer, another session) refund nothing. */
static inline int gv_giver_answer(gv_giver *g, const gv_msg *m, uint8_t cur_epoch, uint32_t cur_sect, int *kind, uint32_t *cls, int *closed, int *why)
{
    *kind = 0, *cls = 0, *closed = 0, *why = GV_WHY_OK;
    if (!m || (m->type != GV_M_ACK && m->type != GV_M_CANCELACK) || m->to != (uint8_t)g->me) return 0;
    for (int i = 0; i < GV_PEND_MAX; i++) {
        gv_pend *p = &g->p[i];
        if (!p->used || p->id != m->id) continue;
        int taken = m->taken > p->n ? p->n : m->taken, back = p->n - taken;
        *kind = p->kind, *cls = p->cls, *closed = 1, *why = m->why;
        p->used = 0, g->answered++;
        if (gv_level_change(p->epoch, p->sect, cur_epoch, cur_sect) == GV_LC_RELOAD) {
            g->dropped_reload++;
            return 0;
        }
        if (back > 0) g->refunded_units += (unsigned)back;
        if (taken == 0) g->refusals++;
        return back;
    }
    g->stray++;
    return 0;
}
/* once per pass. level_ok = this PC's level is in play (cur_epoch / cur_sect are valid). A give from before a RELOAD is dropped (no refund, no question); after a
 * SECTION change the receiver is asked at once (CANCEL); a give without an answer for GV_TIMEOUT_MS is asked again (CANCEL). Returns the messages written to out. */
static inline int gv_giver_tick(gv_giver *g, uint32_t now, int level_ok, uint8_t cur_epoch, uint32_t cur_sect, gv_msg *out, int max)
{
    int n = 0;
    for (int i = 0; i < GV_PEND_MAX && n < max; i++) {
        gv_pend *p = &g->p[i];
        if (!p->used) continue;
        int lc = level_ok ? gv_level_change(p->epoch, p->sect, cur_epoch, cur_sect) : GV_LC_NONE;
        if (lc == GV_LC_RELOAD) {
            p->used = 0, g->dropped_reload++;
            continue;
        }
        int ask_now = lc == GV_LC_SECTION && !p->asked;
        uint32_t wait = GV_TIMEOUT_MS << (p->tries < 3 ? p->tries : 3);   /* 6, 12, 24, then every 48 s: a long hold / load is not flooded (g1n4: 80 CANCELs in 8 min) */
        if (!ask_now && (uint32_t)(now - p->tsent) < wait) continue;
        if (ask_now) p->asked = 1, g->asked_after_section++;
        p->tsent = now, p->cancelling = 1, p->tries++, g->cancels++;
        memset(&out[n], 0, sizeof out[n]);
        out[n].type = GV_M_CANCEL, out[n].id = p->id, out[n].epoch = p->epoch, out[n].sect = p->sect, out[n].from = (uint8_t)g->me, out[n].to = (uint8_t)(1 - g->me),
        out[n].kind = (uint8_t)p->kind, out[n].cls = p->cls, out[n].n = (uint16_t)p->n;
        n++;
    }
    return n;
}
/* the session ended / was replaced: the open gives are dropped without refund (the receiver's state is unknown). Returns the count dropped. */
static inline int gv_giver_session_end(gv_giver *g)
{
    int n = 0;
    for (int i = 0; i < GV_PEND_MAX; i++)
        if (g->p[i].used) g->p[i].used = 0, n++;
    g->lost_session += (unsigned)n;
    return n;
}

/* ---- the receiver's side (network) ---- */
enum { GV_SEEN_APPLIED = 1, GV_SEEN_CANCELLED = 2 };
#define GV_DEFER (-1)
typedef struct { uint32_t id; uint16_t taken; uint8_t state, why; } gv_seen;
typedef struct {
    gv_seen r[GV_RING];       /* every answered id of the session (survives level changes) */
    int head, me;
    unsigned gives, applied_units, refused, refused_reload, dups, cancels, late_after_cancel, foreign, deferred;
} gv_recv;
static inline void gv_recv_init(gv_recv *r, int me) { memset(r, 0, sizeof *r), r->me = me; }
static inline gv_seen *gv_recv_find(gv_recv *r, uint32_t id)
{
    for (int i = 0; i < GV_RING; i++)
        if (r->r[i].state && r->r[i].id == id) return &r->r[i];
    return NULL;
}
static inline void gv_recv_note(gv_recv *r, uint32_t id, uint16_t taken, uint8_t state, uint8_t why)
{
    gv_seen *s = &r->r[r->head];
    r->head = (r->head + 1) % GV_RING;
    s->id = id, s->taken = taken, s->state = state, s->why = why;
}
/* a GIVE for this PC's human, read where this PC's level is in play (cur_epoch / cur_sect). room = what the own bag can take of that class now (-1 = nothing now:
 * downed, no unit). Returns the units the glue must ADD (0 = nothing) or GV_DEFER (the giver is in a newer level instance than this PC: keep the message until this
 * PC is there too) and fills the answer (ans->type 0 = no answer). A GIVE from before a RELOAD of this section is refused (the reload restored the giver too); one
 * from another section is applied (a section change carried both bags). */
static inline int gv_recv_give(gv_recv *r, const gv_msg *m, uint8_t cur_epoch, uint32_t cur_sect, int room, gv_msg *ans)
{
    memset(ans, 0, sizeof *ans);
    if (!m || m->type != GV_M_GIVE) return 0;
    if (m->to != (uint8_t)r->me) {
        r->foreign++;
        return 0;
    }
    gv_seen *s = gv_recv_find(r, m->id);
    if (!s && (int8_t)(uint8_t)(m->epoch - cur_epoch) > 0) {   /* this PC still in an older instance (its load not done): later */
        r->deferred++;
        return GV_DEFER;
    }
    ans->type = GV_M_ACK, ans->id = m->id, ans->epoch = m->epoch, ans->sect = m->sect, ans->from = m->to, ans->to = m->from, ans->kind = m->kind, ans->cls = m->cls,
    ans->n = m->n;
    if (s) {   /* a repeated GIVE (or one that arrives after its CANCEL): the recorded answer, never applied twice */
        if (s->state == GV_SEEN_CANCELLED) r->late_after_cancel++;
        else r->dups++;
        ans->taken = s->state == GV_SEEN_APPLIED ? s->taken : 0, ans->why = s->state == GV_SEEN_CANCELLED ? GV_WHY_CANCELLED : s->why;
        return 0;
    }
    r->gives++;
    int why = GV_WHY_OK, take = 0;
    if (gv_level_change(m->epoch, m->sect, cur_epoch, cur_sect) == GV_LC_RELOAD) why = GV_WHY_EPOCH, r->refused_reload++;
    else if (room < 0) why = GV_WHY_NOW;
    else if (room == 0) why = GV_WHY_FULL;
    else take = m->n < room ? m->n : room;
    if (!take) r->refused++;
    r->applied_units += (unsigned)take;
    ans->taken = (uint16_t)take, ans->why = (uint8_t)why;
    gv_recv_note(r, m->id, (uint16_t)take, GV_SEEN_APPLIED, (uint8_t)why);
    return take;
}
/* a CANCEL ("did you apply #id?"): what was applied (CANCELACK with that), or nothing yet -> the id is marked cancelled (a later GIVE of it takes nothing) */
static inline void gv_recv_cancel(gv_recv *r, const gv_msg *m, gv_msg *ans)
{
    memset(ans, 0, sizeof *ans);
    if (!m || m->type != GV_M_CANCEL || m->to != (uint8_t)r->me) return;
    r->cancels++;
    ans->type = GV_M_CANCELACK, ans->id = m->id, ans->epoch = m->epoch, ans->sect = m->sect, ans->from = m->to, ans->to = m->from, ans->kind = m->kind, ans->cls = m->cls,
    ans->n = m->n;
    gv_seen *s = gv_recv_find(r, m->id);
    if (s) {
        ans->taken = s->state == GV_SEEN_APPLIED ? s->taken : 0, ans->why = s->state == GV_SEEN_APPLIED ? s->why : GV_WHY_CANCELLED;
        return;
    }
    ans->taken = 0, ans->why = GV_WHY_CANCELLED;
    gv_recv_note(r, m->id, 0, GV_SEEN_CANCELLED, GV_WHY_CANCELLED);
}

#endif
