/*
 * test_give1.c - offline test of the RE5-style "give to partner" (native/give1.h, give1_text.h, the glue give1.c; notes/give1.md).
 *   rule: which cells give, the distance (3 m, the same floor), a downed / dead partner, the giver down, no partner (solo part), a give still open;
 *   split: one pass moves min(offer, room); a full bag refuses; nothing is created or lost;
 *   network: two PCs (each writes only its own human's bag) on a reliable in-order line with delays, a lost answer window (timeout -> CANCEL), repeated
 *            deliveries, a full / downed / loading receiver, level instances (reload restores both bags; a section change carries them), the session's end;
 *            the sum of the two humans' counts never grows (no duplicate) and only shrinks in the documented level-change race;
 *   texts: 13 languages, the stock memo pattern, lengths, UTF-8, the game's glyphs (when the game folder is found);
 *   build facts: the stock bytes of every call of give1.c and the item tables in the executable image (when it is at hand);
 *   glue: wiring in probe.c / build.sh, the event kind is free, counts only (no bag slot word written).
 *   sh tests/build.sh   ->  tests/build/test_give1.exe
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/give1.h"
#include "../native/give1_text.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static gv_bag full_caps(void)
{
    gv_bag b;
    memset(&b, 0, sizeof b);
    b.herb_max = 5;
    int am[5] = {60, 20, 100, 15, 12};
    for (int i = 0; i < 5; i++) b.ammo_max[i] = am[i];
    for (int i = 0; i < 4; i++) b.gren_max[i] = 5;
    return b;
}
static int total(const gv_bag *a, const gv_bag *b, const gv_item *it) { return gv_have(a, it) + gv_have(b, it); }
static void bag_addn(gv_bag *b, const gv_item *it, int n)   /* the stock adds: capped at the max */
{
    int *c = it->kind == GV_K_HERB ? &b->herb : it->kind == GV_K_AMMO ? &b->ammo[it->index] : &b->gren[it->index];
    int mx = gv_max(b, it);
    *c += n;
    if (*c > mx) *c = mx;
}
static void bag_subn(gv_bag *b, const gv_item *it, int n)
{
    int *c = it->kind == GV_K_HERB ? &b->herb : it->kind == GV_K_AMMO ? &b->ammo[it->index] : &b->gren[it->index];
    *c -= n;
}

static void t_items(void)
{
    /* every cell id once per tab, the classes of table 0x107E580, the indices of mBullet / mSubWeapon */
    for (int i = 0; i < GV_NITEMS; i++)
        for (int j = i + 1; j < GV_NITEMS; j++) CHECK(!(GV_ITEMS[i].id == GV_ITEMS[j].id && GV_ITEMS[i].tab == GV_ITEMS[j].tab) && GV_ITEMS[i].cls != GV_ITEMS[j].cls);
    CHECK(gv_item_by_id(0, 0x47)->kind == GV_K_HERB && gv_item_by_id(0, 0x47)->cls == 0x80040101u);
    CHECK(gv_item_by_id(1, 0x50)->cls == 0x80040200u && gv_item_by_id(1, 0x50)->index == 0);
    CHECK(gv_item_by_id(1, 0x53)->cls == 0x80040201u && gv_item_by_id(1, 0x53)->index == 1);   /* shotgun: mBullet[1] (FUN_0079AD40: 0x53 -> 1) */
    CHECK(gv_item_by_id(1, 0x52)->index == 2 && gv_item_by_id(1, 0x54)->index == 3 && gv_item_by_id(1, 0x51)->index == 4);
    for (int i = 0; i < GV_NITEMS; i++)
        if (GV_ITEMS[i].kind == GV_K_AMMO) CHECK(GV_ITEMS[i].cls == 0x80040200u + (uint32_t)GV_ITEMS[i].index);   /* cItemPack_vf13: class - 0x80040200 = index */
    CHECK(gv_item_by_id(0, 0x32)->cls == 0x80051900u && gv_item_by_id(0, 0x35)->cls == 0x80051960u && gv_item_by_id(0, 0x34)->index == 2);
    CHECK(gv_item_by_id(0, 0x50) == NULL && gv_item_by_id(1, 0x47) == NULL);   /* the id must match its tab */
    CHECK(gv_item_by_id(0, 0x5F) == NULL && gv_item_by_id(0, 0x63) == NULL && gv_item_by_id(2, 0x8B) == NULL);   /* custom parts, weapon case, key items: never */
    /* the offer */
    CHECK(gv_offer(GV_K_HERB, 3) == 1 && gv_offer(GV_K_GRENADE, 1) == 1 && gv_offer(GV_K_HERB, 0) == 0);
    CHECK(gv_offer(GV_K_AMMO, 1) == 1 && gv_offer(GV_K_AMMO, 9) == 5 && gv_offer(GV_K_AMMO, 30) == 15 && gv_offer(GV_K_AMMO, 0) == 0 && gv_offer(GV_K_NONE, 5) == 0);
}

static gv_ctx base_ctx(void)
{
    gv_ctx c;
    memset(&c, 0, sizeof c);
    c.mode = GV_MODE_SPLIT, c.tab = 0, c.item_id = 0x47, c.have = 2, c.giver_up = 1, c.partner_present = 1, c.partner_up = 1, c.dx = 100, c.dy = 0, c.dz = 100;
    return c;
}
static void t_check(void)
{
    const gv_item *it = NULL;
    gv_ctx c = base_ctx();
    CHECK(gv_check(&c, &it) == GV_WHY_OK && it && it->kind == GV_K_HERB);
    c.mode = GV_MODE_NONE;
    CHECK(gv_check(&c, &it) == GV_WHY_NOCOOP);   /* single player: never */
    c = base_ctx(), c.mode = GV_MODE_NET;
    CHECK(gv_check(&c, &it) == GV_WHY_OK);
    c = base_ctx(), c.tab = 2, c.item_id = 0x8B;
    CHECK(gv_check(&c, &it) == GV_WHY_TAB);
    c = base_ctx(), c.tab = 3;
    CHECK(gv_check(&c, &it) == GV_WHY_TAB);   /* the map */
    c = base_ctx(), c.item_id = 0x5F;
    CHECK(gv_check(&c, &it) == GV_WHY_ITEM && !it);
    c = base_ctx(), c.item_id = -1;
    CHECK(gv_check(&c, &it) == GV_WHY_ITEM);   /* an empty cell */
    c = base_ctx(), c.have = 0;
    CHECK(gv_check(&c, &it) == GV_WHY_EMPTY);
    c = base_ctx(), c.giver_up = 0;
    CHECK(gv_check(&c, &it) == GV_WHY_GIVER);
    c = base_ctx(), c.partner_present = 0;
    CHECK(gv_check(&c, &it) == GV_WHY_NOPARTNER);   /* a solo part */
    c = base_ctx(), c.partner_up = 0;
    CHECK(gv_check(&c, &it) == GV_WHY_DOWN);   /* downed (hp 1) or dead: no give (notes 2) */
    /* the reach: 300 units horizontally, |dy| <= 200 */
    c = base_ctx(), c.dx = 300, c.dz = 0;
    CHECK(gv_check(&c, &it) == GV_WHY_OK);
    c.dx = 300.5f;
    CHECK(gv_check(&c, &it) == GV_WHY_FAR);
    c.dx = 212, c.dz = 212;   /* 299.8 */
    CHECK(gv_check(&c, &it) == GV_WHY_OK);
    c.dx = 213, c.dz = 213;   /* 301.2 */
    CHECK(gv_check(&c, &it) == GV_WHY_FAR);
    c = base_ctx(), c.dy = 200;
    CHECK(gv_check(&c, &it) == GV_WHY_OK);
    c.dy = -201;
    CHECK(gv_check(&c, &it) == GV_WHY_FAR);   /* another floor */
    c = base_ctx(), c.dx = NAN;
    CHECK(gv_check(&c, &it) == GV_WHY_FAR);   /* no position: never in reach */
    c = base_ctx(), c.mode = GV_MODE_NET, c.busy = 1;
    CHECK(gv_check(&c, &it) == GV_WHY_BUSY);
    c = base_ctx(), c.tab = 1, c.item_id = 0x51, c.have = 3;
    CHECK(gv_check(&c, &it) == GV_WHY_OK && it->cls == 0x80040204u);
    for (int w = 0; w < GV_WHY_COUNT; w++) CHECK(strcmp(gv_why_name(w), "?") != 0);
}

static void t_split(void)
{
    const gv_item *herb = gv_item_by_id(0, 0x47), *hg = gv_item_by_id(1, 0x50), *gren = gv_item_by_id(0, 0x33);
    gv_bag a = full_caps(), b = full_caps();
    a.herb = 3, b.herb = 0;
    gv_plan p = gv_split(herb, &a, &b);
    CHECK(p.why == GV_WHY_OK && p.n == 1);
    b.herb = 5;
    p = gv_split(herb, &a, &b);
    CHECK(p.why == GV_WHY_FULL && p.n == 0);   /* "Your partner cannot hold any more.": nothing moves */
    a.ammo[0] = 30, b.ammo[0] = 50;   /* room 10 < offer 15 */
    p = gv_split(hg, &a, &b);
    CHECK(p.why == GV_WHY_OK && p.n == 10);
    a.ammo[0] = 0;
    p = gv_split(hg, &a, &b);
    CHECK(p.why == GV_WHY_EMPTY);
    a.gren[1] = 2, b.gren[1] = 0;   /* a type the receiver has none of: the stock add makes his own unit (give1.c bag_add) */
    p = gv_split(gren, &a, &b);
    CHECK(p.why == GV_WHY_OK && p.n == 1);
    CHECK(gv_split(NULL, &a, &b).why == GV_WHY_ITEM);
    /* 1000 random presses: the sum per item never changes, no count leaves [0, max] */
    srand(7);
    gv_bag x = full_caps(), y = full_caps();
    x.herb = 4, x.ammo[2] = 77, x.gren[3] = 3, y.ammo[2] = 90;
    for (int k = 0; k < 1000; k++) {
        const gv_item *it = &GV_ITEMS[rand() % GV_NITEMS];
        gv_bag *g = (rand() & 1) ? &x : &y, *r = g == &x ? &y : &x;
        int before = total(&x, &y, it);
        gv_plan q = gv_split(it, g, r);
        if (q.n > 0) bag_subn(g, it, q.n), bag_addn(r, it, q.n);
        CHECK(total(&x, &y, it) == before);
        CHECK(gv_have(g, it) >= 0 && gv_have(r, it) <= gv_max(r, it));
        if (fails > 20) break;
    }
}

static void t_wire(void)
{
    gv_msg m, d;
    memset(&m, 0, sizeof m);
    m.type = GV_M_GIVE, m.id = 0x01000007u, m.epoch = 3, m.sect = 0x91090200u, m.from = 1, m.to = 0, m.kind = GV_K_AMMO, m.cls = 0x80040202u, m.n = 300, m.taken = 0, m.why = 0;
    uint8_t b[GV_MSG_LEN];
    CHECK(gv_enc(b, &m) == GV_MSG_LEN && GV_MSG_LEN <= 96);   /* CP_EVENT_MAX */
    CHECK(gv_dec(b, GV_MSG_LEN, &d) && d.type == m.type && d.id == m.id && d.epoch == 3 && d.sect == 0x91090200u && d.from == 1 && d.to == 0 && d.kind == GV_K_AMMO &&
          d.cls == m.cls && d.n == 300);
    CHECK(!gv_dec(b, GV_MSG_LEN - 1, &d));
    uint8_t c[GV_MSG_LEN];
    memcpy(c, b, sizeof c), c[0] = 1;
    CHECK(!gv_dec(c, GV_MSG_LEN, &d));   /* version 1 (no section) is refused: both PCs run the same build (the session checks the exe tag, the DLLs come together) */
    memcpy(c, b, sizeof c), c[1] = 9;
    CHECK(!gv_dec(c, GV_MSG_LEN, &d));   /* type */
    memcpy(c, b, sizeof c), c[8] = 1;
    CHECK(!gv_dec(c, GV_MSG_LEN, &d));   /* from == to */
    memcpy(c, b, sizeof c), c[10] = 0x05;
    CHECK(!gv_dec(c, GV_MSG_LEN, &d));   /* an unknown class (a weapon, a key item) is never accepted */
    m.type = GV_M_ACK, m.taken = 301;
    gv_enc(b, &m);
    CHECK(!gv_dec(b, GV_MSG_LEN, &d));   /* taken > n */
    CHECK(!gv_dec(NULL, GV_MSG_LEN, &d));
    /* the level change rule */
    CHECK(gv_level_change(4, 0x91090200u, 4, 0x91090200u) == GV_LC_NONE);
    CHECK(gv_level_change(4, 0x91090200u, 5, 0x91090200u) == GV_LC_RELOAD);    /* the same section again: Continue / retry / checkpoint */
    CHECK(gv_level_change(4, 0x91090200u, 5, 0x91090300u) == GV_LC_SECTION);   /* the next section: the bags were carried */
    CHECK(gv_level_change(255, 0x91090200u, 0, 0x91090300u) == GV_LC_SECTION);  /* the instance byte wraps */
}

/* ---------------------------------------------------------------- two PCs on a reliable line (in order unless a test delays one message) */
#define LQ 512
typedef struct { uint8_t b[GV_MSG_LEN]; uint32_t at; int to_pc; } lmsg;
typedef struct {
    lmsg q[LQ];
    int n;
    uint32_t delay, extra;   /* every message waits delay (+ extra while set: the "slow answer" window) */
    int dup_every;           /* every k-th message is delivered twice (defensive: the lane is reliable, never doubles) */
    unsigned sent;
    int reorder;             /* 1 = a later message may overtake an earlier one (the real lane keeps the order: the adversarial case) */
    uint32_t last_at[2];     /* in order: never before the previous message to that PC */
} line_t;
typedef struct {
    gv_bag bag;              /* this PC's OWN human's bag (the only one it writes) */
    gv_bag ckpt;             /* its checkpoint: what a reload restores */
    gv_giver g;
    gv_recv r;
    uint8_t epoch;
    uint32_t sect;
    int playing, up;         /* the level is in play (else messages that need it wait) / the own human is up */
    int refused_lines;
} pc_t;
static line_t L;
static pc_t PC[2];
static void lsend(int to_pc, const gv_msg *m, uint32_t now)
{
    if (L.n >= LQ) return;
    gv_enc(L.q[L.n].b, m);
    uint32_t at = now + L.delay + L.extra;
    if (!L.reorder && (int32_t)(L.last_at[to_pc] - at) >= 0) at = L.last_at[to_pc] + 1;
    L.last_at[to_pc] = at;
    L.q[L.n].at = at, L.q[L.n].to_pc = to_pc, L.n++, L.sent++;
    if (L.dup_every && L.sent % (unsigned)L.dup_every == 0 && L.n < LQ) L.q[L.n] = L.q[L.n - 1], L.q[L.n].at += 5, L.n++;
}
static void lpush_back(int to_pc, const uint8_t *b, uint32_t at)   /* the glue's q_push_back: the message waits for the next pass */
{
    if (L.n >= LQ) return;
    memcpy(L.q[L.n].b, b, GV_MSG_LEN), L.q[L.n].at = at, L.q[L.n].to_pc = to_pc, L.n++;
}
/* one message on PC k, as give1.c net_pass handles it; 0 = handled, 1 = waits */
static int deliver(int k, const gv_msg *m, uint32_t now)
{
    pc_t *p = &PC[k];
    const gv_item *it = gv_item_by_cls(m->kind, m->cls);
    gv_msg a;
    if (m->type == GV_M_GIVE) {
        if (!p->playing) return 1;   /* this PC loads: the GIVE waits (the glue refuses it after 30 s) */
        int room = p->up ? gv_room(&p->bag, it) : -1;
        int take = gv_recv_give(&p->r, m, p->epoch, p->sect, room, &a);
        if (take == GV_DEFER) return 1;
        if (take > 0) bag_addn(&p->bag, it, take);
        if (a.type) lsend(1 - k, &a, now);
    } else if (m->type == GV_M_CANCEL) {
        gv_recv_cancel(&p->r, m, &a);
        if (a.type) lsend(1 - k, &a, now);
    } else {
        if (!p->playing) return 1;   /* an answer waits for the own level */
        int kind, closed, why;
        uint32_t cls;
        int back = gv_giver_answer(&p->g, m, p->epoch, p->sect, &kind, &cls, &closed, &why);
        if (back > 0) bag_addn(&p->bag, gv_item_by_cls(kind, cls), back);
        if (closed && m->taken == 0) p->refused_lines++;
    }
    return 0;
}
static void lrun(uint32_t now)
{
    int guard = 0, blocked[2] = {0, 0};
    unsigned seq = 0;
    for (;;) {   /* the earliest message whose time came (ties keep the send order) */
        int best = -1;
        for (int i = 0; i < L.n; i++)
            if (L.q[i].at <= now && (best < 0 || L.q[i].at < L.q[best].at)) best = i;
        if (best < 0 || ++guard > 4 * LQ) return;
        lmsg m = L.q[best];
        memmove(&L.q[best], &L.q[best + 1], (size_t)(L.n - best - 1) * sizeof(lmsg)), L.n--;
        gv_msg d;
        if (blocked[m.to_pc]) {   /* the glue keeps the order: behind a waiting message everything waits (later in the same order) */
            lpush_back(m.to_pc, m.b, now + 20 + (uint32_t)(++seq));
            continue;
        }
        if (gv_dec(m.b, GV_MSG_LEN, &d) && deliver(m.to_pc, &d, now)) lpush_back(m.to_pc, m.b, now + 20 + (uint32_t)(++seq)), blocked[m.to_pc] = 1;
    }
}
static void ticks(uint32_t now)
{
    for (int k = 0; k < 2; k++) {
        gv_msg c[GV_PEND_MAX];
        int n = gv_giver_tick(&PC[k].g, now, PC[k].playing, PC[k].epoch, PC[k].sect, c, GV_PEND_MAX);
        for (int i = 0; i < n; i++) lsend(1 - k, &c[i], now);
    }
    lrun(now);
}
static int net_give(int k, const gv_item *it, uint32_t now)   /* the giver's pass (give1.c do_request, network branch): remove first, then GIVE */
{
    pc_t *p = &PC[k];
    int n = gv_offer(it->kind, gv_have(&p->bag, it));
    if (n <= 0 || gv_giver_open(&p->g) || !p->playing) return 0;
    gv_msg m;
    bag_subn(&p->bag, it, n);
    if (!gv_giver_begin(&p->g, p->epoch, p->sect, it->kind, it->cls, n, now, &m)) {
        bag_addn(&p->bag, it, n);
        return 0;
    }
    lsend(1 - k, &m, now);
    return n;
}
static void net_reset(void)
{
    memset(&L, 0, sizeof L);
    L.delay = 40;
    for (int k = 0; k < 2; k++) {
        memset(&PC[k], 0, sizeof PC[k]);
        PC[k].bag = full_caps(), PC[k].epoch = 1, PC[k].sect = 0x91090200u, PC[k].playing = 1, PC[k].up = 1;
        gv_giver_init(&PC[k].g, k), gv_recv_init(&PC[k].r, k);
    }
}
static void checkpoint(void) { PC[0].ckpt = PC[0].bag, PC[1].ckpt = PC[1].bag; }
/* a level change on PC k: SECTION (the bags carried; the section start is a checkpoint) or RELOAD (the bags from the checkpoint); the PC loads (not playing) */
static void level_change(int k, int reload)
{
    pc_t *p = &PC[k];
    p->epoch++;
    if (reload) p->bag = p->ckpt;
    else p->sect += 0x100u, p->ckpt = p->bag;
    p->playing = 0;
}
static void both_change(int reload, uint32_t *t, uint32_t load_ms)
{
    level_change(0, reload), level_change(1, reload);
    for (uint32_t w = 0; w < load_ms; w += 50) ticks(*t += 50);
    PC[0].playing = PC[1].playing = 1;
}
static int sum(const gv_item *it) { return gv_have(&PC[0].bag, it) + gv_have(&PC[1].bag, it); }
static void settle(uint32_t *t)
{
    L.extra = 0;
    for (int i = 0; i < 400; i++) ticks(*t += 100);
}

static void t_net(void)
{
    const gv_item *herb = gv_item_by_id(0, 0x47), *mg = gv_item_by_id(1, 0x52), *pulse = gv_item_by_id(0, 0x35), *hg = gv_item_by_id(1, 0x50);
    uint32_t t = 1000;
    /* 1. the plain give: the herb leaves PC 0 at once, arrives on PC 1 one delay later, nothing comes back */
    net_reset();
    PC[0].bag.herb = 2;
    CHECK(net_give(0, herb, t) == 1 && PC[0].bag.herb == 1 && PC[1].bag.herb == 0);
    CHECK(gv_giver_open(&PC[0].g) == 1);
    ticks(t += 50);
    CHECK(PC[1].bag.herb == 1);
    ticks(t += 50);
    CHECK(gv_giver_open(&PC[0].g) == 0 && PC[0].bag.herb == 1 && sum(herb) == 2 && PC[0].g.answered == 1 && PC[0].g.refunded_units == 0);
    /* 2. a full receiver: refused, all back, the refusal line */
    net_reset();
    PC[0].bag.herb = 2, PC[1].bag.herb = 5;
    net_give(0, herb, t);
    CHECK(PC[0].bag.herb == 1);
    ticks(t += 100), ticks(t += 100);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 5 && PC[0].refused_lines == 1 && PC[1].r.refused == 1);
    /* 3. partial: ammo offer 40 of 80, room 7 -> 7 there, 33 back */
    net_reset();
    PC[0].bag.ammo[2] = 80, PC[1].bag.ammo[2] = 93;
    CHECK(net_give(0, mg, t) == 40 && PC[0].bag.ammo[2] == 40);
    ticks(t += 100), ticks(t += 100);
    CHECK(PC[1].bag.ammo[2] == 100 && PC[0].bag.ammo[2] == 73 && sum(mg) == 173 && PC[0].refused_lines == 0);
    /* 4. a downed receiver: "cannot take it now", all back */
    net_reset();
    PC[0].bag.gren[3] = 1, PC[1].up = 0;
    net_give(0, pulse, t);
    CHECK(PC[0].bag.gren[3] == 0);
    ticks(t += 100), ticks(t += 100);
    CHECK(PC[0].bag.gren[3] == 1 && PC[1].bag.gren[3] == 0 && PC[0].refused_lines == 1);
    /* 5. one give at a time (the entry hides meanwhile) */
    net_reset();
    PC[0].bag.herb = 3;
    CHECK(net_give(0, herb, t) == 1 && net_give(0, herb, t) == 0 && PC[0].bag.herb == 2);
    ticks(t += 100), ticks(t += 100);
    CHECK(net_give(0, herb, t) == 1);
    ticks(t += 100), ticks(t += 100);
    CHECK(PC[0].bag.herb == 1 && PC[1].bag.herb == 2);
    /* 6. a slow answer (applied on PC 1, the ACK takes 9 s): CANCEL -> CANCELACK(taken) -> no refund, no double */
    net_reset();
    PC[0].bag.herb = 2;
    L.extra = 9000;
    net_give(0, herb, t);
    ticks(t += 100);
    CHECK(PC[1].bag.herb == 0);
    for (int i = 0; i < 400; i++) ticks(t += 100);
    CHECK(PC[1].bag.herb == 1 && PC[0].bag.herb == 1 && sum(herb) == 2 && PC[0].g.cancels >= 1 && PC[0].g.stray >= 1 && gv_giver_open(&PC[0].g) == 0);
    /* 7. the GIVE itself is late (reordered after its CANCEL): PC 1 marks the id cancelled and refuses the GIVE later: one refund, the receiver gets nothing */
    net_reset();
    PC[0].bag.herb = 2;
    gv_msg m;
    bag_subn(&PC[0].bag, herb, 1);
    gv_giver_begin(&PC[0].g, PC[0].epoch, PC[0].sect, herb->kind, herb->cls, 1, t, &m);   /* the GIVE is held back by hand */
    L.reorder = 1;
    ticks(t += GV_TIMEOUT_MS + 10);
    ticks(t += 100), ticks(t += 100);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 0 && gv_giver_open(&PC[0].g) == 0);
    lsend(1, &m, t), ticks(t += 100), ticks(t += 100);
    CHECK(PC[1].bag.herb == 0 && PC[0].bag.herb == 2 && PC[1].r.late_after_cancel == 1 && sum(herb) == 2);
    /* 8. repeated deliveries (defensive): every message twice -> never applied twice, never refunded twice */
    net_reset();
    L.dup_every = 1;
    PC[0].bag.ammo[0] = 40;
    for (int i = 0; i < 6; i++) {
        net_give(0, hg, t);
        for (int j = 0; j < 4; j++) ticks(t += 50);
    }
    CHECK(sum(hg) == 40 && PC[1].r.dups > 0 && PC[0].g.stray > 0);

    /* ---- SECTION changes (bags carried over) in every window of the protocol: nothing may be lost or doubled (lead 2026-11-11: the old rule lost the item) */
    /* S1 the GIVE in flight across the change: PC 1 gets it after its load and applies it in the new section */
    net_reset();
    PC[0].bag.herb = 2, L.extra = 300;
    net_give(0, herb, t);
    both_change(0, &t, 1000);
    settle(&t);
    CHECK(PC[0].bag.herb == 1 && PC[1].bag.herb == 1 && sum(herb) == 2 && gv_giver_open(&PC[0].g) == 0);
    /* S2 PC 1 applied before the change, the ACK in flight across it: no refund (taken all) */
    net_reset();
    PC[0].bag.herb = 2;
    net_give(0, herb, t);
    ticks(t += 50);   /* applied on PC 1, ACK on the way */
    CHECK(PC[1].bag.herb == 1);
    both_change(0, &t, 600);
    settle(&t);
    CHECK(PC[0].bag.herb == 1 && PC[1].bag.herb == 1 && sum(herb) == 2 && gv_giver_open(&PC[0].g) == 0);
    /* S3 a REFUSAL (full) in flight across the change: the refund comes after the change (the old rule dropped it: the item was lost) */
    net_reset();
    PC[0].bag.herb = 2, PC[1].bag.herb = 5;
    net_give(0, herb, t);
    ticks(t += 50);   /* refused on PC 1, the ACK(0) on the way */
    both_change(0, &t, 600);
    settle(&t);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 5 && sum(herb) == 7 && gv_giver_open(&PC[0].g) == 0);
    /* S4 a partial take in flight across the change: the remainder comes back */
    net_reset();
    PC[0].bag.ammo[2] = 80, PC[1].bag.ammo[2] = 93;
    net_give(0, mg, t);
    ticks(t += 50);
    both_change(0, &t, 600);
    settle(&t);
    CHECK(PC[0].bag.ammo[2] == 73 && PC[1].bag.ammo[2] == 100 && sum(mg) == 173);
    /* S5 the GIVE is slow (9 s): the giver asks at once after the change (CANCEL behind the GIVE): PC 1 applies, then answers "taken" */
    net_reset();
    PC[0].bag.herb = 2, L.extra = 9000;
    net_give(0, herb, t);
    both_change(0, &t, 600);
    for (int i = 0; i < 5; i++) ticks(t += 50);   /* the question goes out on the same slow line: behind the GIVE (the lane keeps the order) */
    CHECK(PC[0].g.asked_after_section == 1);
    settle(&t);
    CHECK(PC[0].bag.herb == 1 && PC[1].bag.herb == 1 && sum(herb) == 2 && gv_giver_open(&PC[0].g) == 0 && PC[1].r.gives == 1);
    /* S5b the same, but the question overtakes the GIVE (a reorder the real lane never does): cancelled first, the herb back, the late GIVE refused: still one herb */
    net_reset();
    PC[0].bag.herb = 2, L.extra = 9000, L.reorder = 1;
    net_give(0, herb, t);
    both_change(0, &t, 600);
    settle(&t);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 0 && sum(herb) == 2 && gv_giver_open(&PC[0].g) == 0 && PC[1].r.late_after_cancel == 1);
    /* S6 the CANCEL (no answer for 6 s) in flight across the change, the GIVE lost on the way (only the CANCEL arrives): "not applied" -> refund */
    net_reset();
    PC[0].bag.herb = 2;
    bag_subn(&PC[0].bag, herb, 1);
    gv_giver_begin(&PC[0].g, PC[0].epoch, PC[0].sect, herb->kind, herb->cls, 1, t, &m);   /* never sent */
    ticks(t += GV_TIMEOUT_MS + 10);   /* the CANCEL goes out */
    both_change(0, &t, 30);
    settle(&t);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 0 && sum(herb) == 2 && gv_giver_open(&PC[0].g) == 0);
    /* S7 only the giver changed so far (the receiver still loads the old instance): the GIVE waits on PC 1 (DEFER) until it is in the new instance too */
    net_reset();
    PC[0].bag.herb = 2;
    level_change(0, 0), PC[0].playing = 1;   /* PC 0 is in the new section, PC 1 still in the old one */
    net_give(0, herb, t);
    for (int i = 0; i < 10; i++) ticks(t += 50);
    CHECK(PC[1].bag.herb == 0 && PC[1].r.deferred > 0);
    level_change(1, 0), PC[1].playing = 1;
    settle(&t);
    CHECK(PC[0].bag.herb == 1 && PC[1].bag.herb == 1 && sum(herb) == 2);
    /* S8 the receiver loads for a long time with the GIVE waiting; repeated deliveries across the change */
    net_reset();
    PC[0].bag.ammo[0] = 30, L.dup_every = 1;
    net_give(0, hg, t);
    level_change(0, 0), level_change(1, 0);
    for (int i = 0; i < 60; i++) ticks(t += 100);   /* both load 6 s: the giver's timeout CANCELs wait in the line too */
    PC[0].playing = PC[1].playing = 1;
    settle(&t);
    CHECK(sum(hg) == 30 && PC[0].bag.ammo[0] + PC[1].bag.ammo[0] == 30 && gv_giver_open(&PC[0].g) == 0);

    /* S9 a long wait (the receiver holds / loads 2 minutes): the questions back off (6, 12, 24, then every 48 s), the give still completes once */
    net_reset();
    PC[0].bag.herb = 2;
    net_give(0, herb, t);
    PC[1].playing = 0;
    for (int i = 0; i < 1200; i++) ticks(t += 100);
    CHECK(PC[0].g.cancels >= 3 && PC[0].g.cancels <= 6);
    PC[1].playing = 1;
    settle(&t);
    CHECK(PC[0].bag.herb == 1 && PC[1].bag.herb == 1 && sum(herb) == 2 && gv_giver_open(&PC[0].g) == 0);

    /* ---- RELOADS (the same section again, bags from the checkpoint): the give is undone on both sides */
    /* R1 the GIVE in flight across a reload: refused on PC 1 (a reload since), no refund on PC 0: both bags = the checkpoint */
    net_reset();
    PC[0].bag.herb = 2;
    checkpoint();
    L.extra = 300;
    net_give(0, herb, t);
    both_change(1, &t, 1000);
    settle(&t);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 0 && sum(herb) == 2 && PC[1].r.refused_reload + PC[0].g.dropped_reload >= 1);
    /* R2 applied on PC 1, the ACK in flight across the reload: the reload took it away from PC 1 and gave it back to PC 0; the late ACK refunds nothing */
    net_reset();
    PC[0].bag.herb = 2;
    checkpoint();
    net_give(0, herb, t);
    ticks(t += 50);
    CHECK(PC[1].bag.herb == 1);
    both_change(1, &t, 600);
    settle(&t);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 0 && sum(herb) == 2);
    /* R3 a refusal in flight across the reload: no refund (the checkpoint already holds the herb) */
    net_reset();
    PC[0].bag.herb = 2, PC[1].bag.herb = 5;
    checkpoint();
    net_give(0, herb, t);
    ticks(t += 50);
    both_change(1, &t, 600);
    settle(&t);
    CHECK(PC[0].bag.herb == 2 && PC[1].bag.herb == 5);
    /* R4 a section change, then a reload of the new section with the give still open: the new section's start checkpoint holds the carried bags: the give completes */
    net_reset();
    PC[0].bag.herb = 2, L.extra = 2000;
    net_give(0, herb, t);
    both_change(0, &t, 300);   /* section: the bags carried, the new checkpoint = them (giver 1, receiver 0) */
    both_change(1, &t, 300);   /* reload of that section */
    settle(&t);
    CHECK(sum(herb) == 2 && PC[0].bag.herb + PC[1].bag.herb == 2);
    /* 9. both PCs give at the same time, every kind, 3000 rounds with slow answers, duplicates, reorders, SECTION changes (and reloads with checkpoints): never more
     *    than there was, never negative; after quiescence without reloads: exactly what there was */
    for (int pass = 0; pass < 4; pass++) {
        net_reset();
        srand(100 + pass);
        L.dup_every = pass >= 2 ? 3 : 0, L.reorder = pass >= 2;
        for (int k = 0; k < 2; k++) PC[k].bag.herb = 2, PC[k].bag.ammo[0] = 25, PC[k].bag.ammo[4] = 6, PC[k].bag.gren[0] = 2, PC[k].bag.gren[2] = 1;   /* sums <= max */
        checkpoint();
        int s0[GV_NITEMS];
        for (int i = 0; i < GV_NITEMS; i++) s0[i] = sum(&GV_ITEMS[i]);
        unsigned sections = 0, reloads = 0;
        for (int r = 0; r < 3000; r++) {
            int k = rand() & 1;
            if (rand() % 3 == 0) net_give(k, &GV_ITEMS[rand() % GV_NITEMS], t);
            if (pass >= 1) L.extra = rand() % 10 == 0 ? (uint32_t)(GV_TIMEOUT_MS + rand() % 4000) : (uint32_t)(rand() % 300);
            PC[1 - k].up = rand() % 10 != 0;
            int ev = rand() % 200;
            if (ev == 0) {
                both_change(0, &t, 200 + (uint32_t)(rand() % 3000)), sections++;
            } else if (ev == 1 && pass == 3) {   /* a reload: back to the checkpoint (the sums at the checkpoint are the sums: gives only move units) */
                both_change(1, &t, 200 + (uint32_t)(rand() % 2000)), reloads++;
            } else if (ev == 2) {   /* the two PCs load at different times */
                level_change(0, 0), level_change(1, 0);
                PC[0].playing = 1;
                for (int i = 0; i < 20; i++) ticks(t += 50);
                PC[1].playing = 1, sections++;
            }
            ticks(t += 20 + (uint32_t)(rand() % 200));
            for (int i = 0; i < GV_NITEMS; i++) {
                const gv_item *it = &GV_ITEMS[i];
                CHECK(gv_have(&PC[0].bag, it) >= 0 && gv_have(&PC[1].bag, it) >= 0);
                CHECK(sum(it) <= s0[i]);
            }
            if (fails > 20) return;
        }
        L.dup_every = 0;
        PC[0].up = PC[1].up = 1;
        settle(&t);
        CHECK(gv_giver_open(&PC[0].g) == 0 && gv_giver_open(&PC[1].g) == 0 && L.n == 0);
        for (int i = 0; i < GV_NITEMS; i++) {
            /* sections only: exactly what there was. With reloads (pass 3) never more: a give in flight at a section start is in neither bag of that section's start
             * checkpoint, so a later reload of it rolls the unit back with every other change since (the documented rest, notes 2) */
            int ok = pass < 3 ? sum(&GV_ITEMS[i]) == s0[i] : sum(&GV_ITEMS[i]) <= s0[i];
            if (!ok) printf("FAIL: pass %d item %02x sum %d vs %d\n", pass, (unsigned)GV_ITEMS[i].id, sum(&GV_ITEMS[i]), s0[i]);
            CHECK(ok);
        }
        printf("net chaos pass %d: gives %u + %u, section changes %u, reloads %u, asked after a section %u + %u, dropped by a reload %u + %u, refunds %u + %u units, "
               "waited %u + %u, dups %u + %u, late %u + %u\n", pass, PC[0].g.sent, PC[1].g.sent, sections, reloads, PC[0].g.asked_after_section, PC[1].g.asked_after_section,
               PC[0].g.dropped_reload, PC[1].g.dropped_reload, PC[0].g.refunded_units, PC[1].g.refunded_units, PC[0].r.deferred, PC[1].r.deferred, PC[0].r.dups, PC[1].r.dups,
               PC[0].r.late_after_cancel, PC[1].r.late_after_cancel);
    }
    /* 10. the session ends with a give open: dropped (no refund) */
    net_reset();
    PC[0].bag.herb = 2;
    L.extra = 500;
    net_give(0, herb, t);
    CHECK(gv_giver_session_end(&PC[0].g) == 1 && gv_giver_open(&PC[0].g) == 0 && PC[0].g.lost_session == 1);
    /* 11. foreign / malformed answers never refund */
    net_reset();
    gv_msg f;
    memset(&f, 0, sizeof f);
    f.type = GV_M_ACK, f.id = 0x00000063u, f.from = 1, f.to = 0, f.kind = GV_K_HERB, f.cls = 0x80040101u, f.n = 5, f.taken = 0, f.sect = 0x91090200u;
    int kind, closed, why;
    uint32_t cls;
    CHECK(gv_giver_answer(&PC[0].g, &f, 1, 0x91090200u, &kind, &cls, &closed, &why) == 0 && !closed && PC[0].g.stray == 1);
    f.to = 1;
    CHECK(gv_giver_answer(&PC[0].g, &f, 1, 0x91090200u, &kind, &cls, &closed, &why) == 0);
    gv_msg a;
    f.type = GV_M_GIVE, f.from = 0, f.to = 0;
    CHECK(gv_recv_give(&PC[1].r, &f, 1, 0x91090200u, 5, &a) == 0 && a.type == 0);   /* not for this PC's human: no answer, nothing added */
}

/* ---------------------------------------------------------------- texts */
static int utf8_ok(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return 0;
        p++;
        for (int i = 0; i < n; i++, p++)
            if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}
static unsigned next_cp(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    unsigned c = *p++;
    if (c >= 0xF0) c = (c & 7) << 18 | (unsigned)(p[0] & 0x3F) << 12 | (unsigned)(p[1] & 0x3F) << 6 | (p[2] & 0x3F), p += 3;
    else if (c >= 0xE0) c = (c & 15) << 12 | (unsigned)(p[0] & 0x3F) << 6 | (p[1] & 0x3F), p += 2;
    else if (c >= 0xC0) c = (c & 31) << 6 | (p[0] & 0x3F), p += 1;
    *pp = p;
    return c;
}
static int is_cjk(int lang) { return lang == 5 || lang == 11 || lang == 12; }
/* the game's glyph set of a font file (GFD: count at +0x1C, 16-byte records at the end, first word = code point) */
static unsigned *gfd_cps(const char *path, unsigned *n)
{
    *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *d = (unsigned char *)malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz || sz < 0x40 || memcmp(d, "GFD", 4)) {
        fclose(f), free(d);
        return NULL;
    }
    fclose(f);
    unsigned cnt = (unsigned)d[0x1C] | (unsigned)d[0x1D] << 8 | (unsigned)d[0x1E] << 16 | (unsigned)d[0x1F] << 24;
    if (!cnt || (long)cnt * 16 > sz) {
        free(d);
        return NULL;
    }
    unsigned *cp = (unsigned *)malloc(cnt * sizeof(unsigned));
    const unsigned char *rec = d + sz - (long)cnt * 16;
    for (unsigned i = 0; i < cnt; i++) cp[i] = (unsigned)rec[i * 16] | (unsigned)rec[i * 16 + 1] << 8 | (unsigned)rec[i * 16 + 2] << 16 | (unsigned)rec[i * 16 + 3] << 24;
    free(d);
    *n = cnt;
    return cp;
}
static int has_cp(const unsigned *set, unsigned n, unsigned c)
{
    for (unsigned i = 0; i < n; i++)
        if (set[i] == c) return 1;
    return 0;
}
static void t_texts(void)
{
    for (int id = 0; id < GV_T_COUNT; id++)
        for (int l = 0; l < GV_LANGS; l++) {
            const char *s = GV_TEXT[id][l];
            CHECK(s && *s && utf8_ok(s));
            if (!s) continue;
            CHECK(!strstr(s, "\xE2\x80\x98") && !strstr(s, "\xE2\x80\x99") && !strstr(s, "\xE2\x80\x9A") && !strstr(s, "\xE2\x80\x9B"));   /* the PowerShell quote trap */
            CHECK(!strstr(s, " - ") && !strchr(s, '\n') && !strchr(s, '<'));   /* no dash, one line, no tag of our own */
            int v = gv_visible_cps(s), cjk = is_cjk(l);
            CHECK(v >= 2 && v <= (id == GV_T_GIVE ? (cjk ? 10 : 22) : (cjk ? 20 : 44)));   /* the memo bar: "<B_DE> action  <B_CA> Close" / a refusal line + close */
            if (l > 0 && id != GV_T_GIVE) CHECK(strcmp(s, GV_TEXT[id][0]) != 0);   /* translated (Italian / Polish "partner" are real words of those games) */
            if (l == 6) CHECK(strstr(s, "Партнёр") || strstr(s, "партнёру"));   /* the mod writes ё (i18n-style rule 11) */
            if (l == 11 || l == 12) CHECK(strstr(s, "同伴") != NULL);   /* the game's partner word */
            if (l == 5) CHECK(strstr(s, "パートナー") != NULL);
        }
    char b[160];
    gv_memo_action(b, sizeof b, 0);
    CHECK(!strcmp(b, "<B_DE> Give to partner"));
    gv_memo_action(b, sizeof b, 3);
    CHECK(!strcmp(b, "<B_DE>Dem Partner geben"));   /* the stock German memo: "<B_DE>Lesen" */
    gv_memo_action(b, sizeof b, 6);
    CHECK(!strcmp(b, "<B_DE>Отдать партнёру"));
    gv_memo_action(b, sizeof b, 11);
    CHECK(!strcmp(b, "<B_DE>交给同伴"));
    gv_memo_action(b, sizeof b, 5);
    CHECK(!strcmp(b, "<B_DE> パートナーに渡す"));   /* the stock Japanese memo: "<B_DE> 読む" */
    CHECK(!strcmp(gv_text(GV_T_FULL, 99), GV_TEXT[GV_T_FULL][0]) && !strcmp(gv_text(-1, 0), ""));
    CHECK(gv_visible_cps("<B_DE> Ab") == 3 && gv_visible_cps("<B_CA>Закрыть") == 7);
    /* the glyphs of the game's fonts (Font_eng: Latin, Cyrillic, kana / kanji; Font_chS / Font_chT) */
    const char *dirs[] = {getenv("GAME_DIR"), "../../game", "../../work/game", NULL};
    char path[600];
    int found = 0;
    for (int d = 0; dirs[d] || d == 0; d++) {
        if (!dirs[d]) continue;
        unsigned ne = 0, ns = 0, nt = 0;
        snprintf(path, sizeof path, "%s/nativePC/id_bhr/00_font/Font_eng.gfd", dirs[d]);
        unsigned *fe = gfd_cps(path, &ne);
        snprintf(path, sizeof path, "%s/nativePC/id_bhr/00_font/Font_chS.gfd", dirs[d]);
        unsigned *fs = gfd_cps(path, &ns);
        snprintf(path, sizeof path, "%s/nativePC/id_bhr/00_font/Font_chT.gfd", dirs[d]);
        unsigned *ft = gfd_cps(path, &nt);
        if (fe && fs && ft) {
            found = 1;
            int miss = 0;
            for (int id = 0; id < GV_T_COUNT; id++)
                for (int l = 0; l < GV_LANGS; l++) {
                    const unsigned *set = l == 11 ? fs : l == 12 ? ft : fe;
                    unsigned n = l == 11 ? ns : l == 12 ? nt : ne;
                    for (const unsigned char *p = (const unsigned char *)GV_TEXT[id][l]; *p;) {
                        unsigned c = next_cp(&p);
                        if (c != ' ' && !has_cp(set, n, c)) {
                            miss++;
                            printf("FAIL: text %d lang %d: U+%04X is not in the game's font\n", id, l, c);
                        }
                    }
                }
            CHECK(miss == 0);
            printf("glyphs: every character of the give texts is in the game's fonts (%u / %u / %u glyphs)\n", ne, ns, nt);
        }
        free(fe), free(fs), free(ft);
        if (found) break;
    }
    if (!found) printf("(no game folder: the glyph check of the give texts did not run; set GAME_DIR=<game folder>)\n");
}

/* ---------------------------------------------------------------- the executable image and the glue's sources */
static unsigned char *img;
static long img_n;
static uint32_t i32(uint32_t va)
{
    long o = (long)va - 0x400000L;
    if (!img || o < 0 || o + 4 > img_n) return 0xFFFFFFFFu;
    return (uint32_t)img[o] | (uint32_t)img[o + 1] << 8 | (uint32_t)img[o + 2] << 16 | (uint32_t)img[o + 3] << 24;
}
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (!s || fread(s, 1, (size_t)n, f) != (size_t)n) {
        fclose(f), free(s);
        return NULL;
    }
    fclose(f);
    s[n] = 0;
    return s;
}
static void t_image(const char *glue)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    char *d = slurp(path);
    if (!d) {
        printf("(no executable image at %s: the build facts of give1 are not compared)\n", path);
        return;
    }
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END), img_n = ftell(f), fclose(f);
    img = (unsigned char *)d;
    CHECK(i32(0x1082698u + 0x20u) == 0x7BCD30u);    /* uGUI_SubMenu slot 8 = its update */
    CHECK(i32(0x10186B0u + 0x28u) == 0x5C5B90u && i32(0x10186B0u + 0x2Cu) == 0x5C2CE0u && i32(0x10186B0u + 0x34u) == 0x5C2D20u && i32(0x10186B0u + 0x48u) == 0x5C2E70u);
    for (int i = 0; i < GV_NITEMS; i++) CHECK(i32(0x107E580u + (uint32_t)GV_ITEMS[i].id * 12u + 8u) == GV_ITEMS[i].cls);   /* id -> class (FUN_0079AD20) */
    int ammo_ids[5] = {0x50, 0x53, 0x54, 0x52, 0x51};   /* table 0x1082830 (the Ammunition tab) */
    for (int i = 0; i < 5; i++) CHECK(i32(0x1082830u + 4u * (uint32_t)i) == (uint32_t)ammo_ids[i] && gv_item_by_id(1, ammo_ids[i]) != NULL);
    for (int i = 0; i < 4; i++) {   /* table 0x1082864: class, id pairs of the grenades (the Items tab) */
        uint32_t cls = i32(0x1082864u + 8u * (uint32_t)i), id = i32(0x1082868u + 8u * (uint32_t)i);
        const gv_item *it = gv_item_by_id(0, (int)id);
        CHECK(it && it->cls == cls && it->kind == GV_K_GRENADE && it->index == i);
    }
    CHECK(i32(0x5C2D20u + 0x18u) != 0xFFFFFFFFu);
    /* every SIG entry of give1.c: its 12 bytes at its address */
    int nsig = 0;
    const char *p = glue ? strstr(glue, "} SIG[] = {") : NULL;
    const char *end = p ? strstr(p, "};") : NULL;
    while (p && end && (p = strstr(p, "\n    {GVA_")) && p < end) {
        char name[40];
        unsigned b[12];
        if (sscanf(p, "\n    {%39[A-Z0-9_], {0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x}", name, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &b[8],
                   &b[9], &b[10], &b[11]) == 13) {
            char key[64];
            snprintf(key, sizeof key, "#define %s ", name);
            const char *def = strstr(glue, key);
            unsigned va = 0;
            CHECK(def && sscanf(def + strlen(key), " 0x%x", &va) == 1);
            int same = 1;
            for (int k = 0; k < 12; k++) same &= va >= 0x400000u && va - 0x400000u + 12u <= (unsigned)img_n && img[va - 0x400000u + (unsigned)k] == b[k];
            if (!same) printf("FAIL: SIG %s at %08x differs from the image\n", name, va);
            CHECK(same);
            nsig++;
        }
        p++;
    }
    CHECK(nsig == 19);
    /* the stack words every called stock function pops (its ret): the glue's thiscall typedefs push exactly that many (g1n1 / g1n2: FUN_0040EEF0 pops one word
     * the first glue did not push: esp off by 4, the host's game died at BACK) */
    static const struct { uint32_t fn, ret_at; unsigned bytes; const char *typedef_text; } RETS[] = {
        {0x40EEF0u, 0x40EF53u, 4, "typedef uint32_t (TC *local_fn)(void *splayer, void *unused);"},
        {0x7BC120u, 0x7BCA9Cu, 0, "typedef void (TC *gui_fn)(void *gui);"},
        {0x7BB710u, 0x7BBA20u, 0, "typedef void (TC *gui_fn)(void *gui);"},
        {0x848E50u, 0x848EB0u, 12, "typedef void (TC *memo2_fn)(void *gui, void *text, const char *s1, const char *s2);"},
        {0xA10F10u, 0xA10F21u, 4, "typedef const char *(TC *bankstr_fn)(void *bank, unsigned id);"},
        {0x8C0100u, 0x8C014Eu, 8, "typedef void (TC *sound_fn)(void *snd, unsigned index, void *gui);"},
        {0x848AD0u, 0x848BA3u, 8, "typedef unsigned (TC *hover_fn)(void *gui, void *cell, int k);"},
        {0x5C4300u, 0x5C4358u, 4, "typedef void (TC *bag1_fn)(void *bag, unsigned a);"},
        {0x5C8230u, 0x5C8455u, 4, "typedef void (TC *bag1_fn)(void *bag, unsigned a);"},
        {0x5C7B10u, 0x5C7C63u, 8, "typedef void (TC *bag2_fn)(void *bag, unsigned a, unsigned b);"},
        {0x5C2CE0u, 0x5C2CF1u, 8, "typedef void (TC *bag2_fn)(void *bag, unsigned a, unsigned b);"},
        {0x5C2D20u, 0x5C2D76u, 8, "typedef void (TC *bag2_fn)(void *bag, unsigned a, unsigned b);"},
        {0x5C2E50u, 0x5C2E64u, 8, "typedef void (TC *bag2_fn)(void *bag, unsigned a, unsigned b);"},   /* vf18 0x5C2E70 tail-jumps into vf19 */
        {0x5C5B90u, 0x5C5BFAu, 4, "typedef unsigned (TC *bagmax_fn)(void *bag, unsigned cls);"},
        {0x84D9C0u, 0x84DA0Du, 4, "typedef unsigned (TC *log1_fn)(void *log, unsigned cls);"},
        {0x84D890u, 0x84D9B2u, 8, "typedef void (TC *log2_fn)(void *log, unsigned cls, unsigned v);"},
        {0x9B12C0u, 0x9B12FDu, 8, "typedef unsigned (TC *exists_fn)(void *sres, void *dti, const char *path);"},
        {0x720320u, 0x720350u, 0, "typedef uint32_t (TC *sect_fn)(void *episode);"},
    };
    for (unsigned i = 0; i < sizeof RETS / sizeof RETS[0]; i++) {
        long o = (long)RETS[i].ret_at - 0x400000L;
        int ok = o >= 0 && o + 3 <= img_n && (RETS[i].bytes ? img[o] == 0xC2 && img[o + 1] == RETS[i].bytes && img[o + 2] == 0 : img[o] == 0xC3);
        if (!ok) printf("FAIL: the ret of %08x at %08x is not %u bytes\n", (unsigned)RETS[i].fn, (unsigned)RETS[i].ret_at, RETS[i].bytes);
        CHECK(ok);
        CHECK(glue && strstr(glue, RETS[i].typedef_text) != NULL);
    }
    printf("image: %d stock signatures of give1.c match, item tables and vtables checked\n", nsig);
    free(d), img = NULL;
}
static void t_glue(const char *glue, const char *probe, const char *build, const char *tbuild)
{
    CHECK(glue && probe && build && tbuild);
    if (!glue || !probe || !build || !tbuild) return;
    CHECK(strstr(probe, "give1_init();") && strstr(probe, "\"give\", NULL") && strstr(probe, "give_cmd(arg, out, size);"));
    CHECK(strstr(build, " give1.c ") != NULL && strstr(tbuild, "test_give1") != NULL);
    const char *hk = strstr(glue, "static void TC __attribute__((force_align_arg_pointer)) update_hook(void *gui)");   /* g1n1 live: the game's 4-byte stack + SSE = #GP */
    CHECK(hk && strstr(hk, "orig_update(gui);") && strstr(hk, "ui_pass(") > strstr(hk, "orig_update(gui);"));   /* the stock update first */
    /* counts only: the giver's removal writes herbs / ammo / grenades, never a slot word (+4..+0x3C) of a bag */
    const char *rm = strstr(glue, "static int bag_remove(");
    const char *rm_end = rm ? strstr(rm, "\n}\n") : NULL;
    if (rm && !rm_end) rm_end = strstr(rm, "\n}\r\n");   /* a checkout with core.autocrlf (the allep1 merge) has CRLF lines */
    CHECK(rm && rm_end);
    if (rm && rm_end) {
        int bad = 0;
        for (const char *q = rm; q < rm_end; q++)
            if (!strncmp(q, "wr32(b + 0x", 11)) bad |= strncmp(q, "wr32(b + 0xC8", 13) != 0;
        CHECK(!bad);
        CHECK(strstr(rm, "wr32(s4 + 0xF7C") != NULL && strstr(rm, "GVA_GREN_REFRESH") != NULL);
    }
    CHECK(strstr(glue, "wr32(b + 0x14") == NULL && strstr(glue, "+ 0x30, ") == NULL);   /* no slot write, no weapon owner write */
    CHECK(strstr(glue, "cs_send_event(cs, GV_EV_KIND") != NULL && strstr(glue, "lk_add_event_cb(k, on_event, NULL)") != NULL);
    /* the giver removes before it sends; the receiver adds only what gv_recv_give returned */
    const char *dr = strstr(glue, "static void do_request(");
    CHECK(dr && strstr(dr, "int out = bag_remove(gb, it, n);") && strstr(dr, "gv_giver_begin(&G") > strstr(dr, "int out = bag_remove(gb, it, n);"));
    const char *np = strstr(glue, "int take = gv_recv_give(&R, &m, ep, sect, room, &a);");
    const char *nd = np ? strstr(np, "if (take == GV_DEFER) {") : NULL;
    const char *ng = np ? strstr(np, "int got = take > 0 ? bag_add(ob, it, take) : 0;") : NULL;
    CHECK(np && nd && ng && nd < ng && ng - np < 400);
    /* the level-aware rule in the glue: a GIVE / an answer waits while this PC's level is not in play; the answer and the tick get the section */
    CHECK(strstr(glue, "int back = gv_giver_answer(&G, &m, ep, sect, &kind, &cls, &closed, &why);") && strstr(glue, "gv_giver_tick(&G, now, level_ok, ep, sect, c, GV_PEND_MAX)"));
    CHECK(strstr(glue, "gv_giver_begin(&G, ep, sect, it->kind, it->cls, out, now, &m)") && strstr(glue, "q_push_back(b, t0 ? t0 : now), blocked = 1;") && strstr(glue, "if (blocked) {"));
}
static void t_kind_free(void)
{
    static const char *files[] = {"../native/rolebarrier1.h", "../native/downed1_net.h", "../native/coop_proto.h", "../native/world1.h", "../native/coopsave1.h", NULL};
    for (int i = 0; files[i]; i++) {
        char *s = slurp(files[i]);
        if (!s) continue;
        CHECK(!strstr(s, "KIND 0x144") && !strstr(s, "USER + 0x44"));
        free(s);
    }
    CHECK(GV_EV_KIND == 0x100u + 0x44u);
}

int main(void)
{
    t_items();
    t_check();
    t_split();
    t_wire();
    t_net();
    t_texts();
    char *glue = slurp("../native/give1.c"), *probe = slurp("../native/probe.c"), *build = slurp("../native/build.sh"), *tbuild = slurp("build.sh");
    t_image(glue);
    t_glue(glue, probe, build, tbuild);
    t_kind_free();
    free(glue), free(probe), free(build), free(tbuild);
    printf("test_give1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
