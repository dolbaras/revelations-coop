/* Offline tests of the puppet logic (native/puppet1.c): sender dedup / redundancy, receiver order / loss / duplicates / reorder / jitter, dead-man, holds, epochs,
 * sequence wrap, position correction, STATE fill. No game, no session, no network: a simulated link with a virtual clock. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/puppet1.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static unsigned rng_s = 12345;
static unsigned rnd(void) { rng_s = rng_s * 1664525u + 1013904223u; return rng_s >> 8; }
static int chance(int percent) { return (int)(rnd() % 100) < percent; }

/* ---- sink: records every push ---- */
typedef struct {
    int n;
    cp_pad_rec rec[20000];
    uint32_t seq[20000];
    int bad_order;     /* output counter not strictly rising */
    uint32_t last_seq;
} Sink;
static int sink_push(void *u, const cp_pad_rec *r, uint32_t seq, uint32_t ts)
{
    Sink *s = u;
    (void)ts;
    if (s->n && (int32_t)(seq - s->last_seq) <= 0) s->bad_order++;
    s->last_seq = seq;
    if (s->n < 20000) s->rec[s->n] = *r, s->seq[s->n] = seq, s->n++;
    return 1;
}
static int is_neutral(const cp_pad_rec *r) { cp_pad_rec z; memset(&z, 0, sizeof z); return !memcmp(r, &z, sizeof z); }

/* ---- fake unit ---- */
typedef struct { float pos[3], yaw; int exists, tps; float last_tp[3]; } Unit;
static int u_get(void *u, float pos[3], float *yaw) { Unit *x = u; if (!x->exists) return 0; memcpy(pos, x->pos, 12); *yaw = x->yaw; return 1; }
static int u_tp(void *u, const float pos[3], float yaw) { Unit *x = u; memcpy(x->pos, pos, 12); memcpy(x->last_tp, pos, 12); x->yaw = yaw; x->tps++; return 1; }

/* ---- simulated link ---- */
typedef struct { uint32_t due; cp_pad pkt; } Flight;
typedef struct { Flight f[512]; int n; } Link;
static void link_send(Link *l, uint32_t now, const cp_pad *p, int loss, int dup, int reorder, int base, int jitter)
{
    if (chance(loss)) return;
    int copies = 1 + (chance(dup) ? 1 : 0);
    for (int c = 0; c < copies; c++) {
        if (l->n >= 512) return;
        int d = base + (jitter ? (int)(rnd() % (unsigned)(jitter + 1)) : 0);
        if (chance(reorder)) d += 40 + (int)(rnd() % 60);
        l->f[l->n].due = now + (uint32_t)d, l->f[l->n].pkt = *p, l->n++;
    }
}
static void link_deliver(Link *l, uint32_t now, p1_rx *rx, unsigned epoch)
{
    for (int i = 0; i < l->n;) {
        if ((int32_t)(now - l->f[i].due) >= 0) {
            p1_rx_packet(rx, now, &l->f[i].pkt, epoch);
            l->f[i] = l->f[--l->n];
        } else
            i++;
    }
}

/* a pad whose every numbered record is unique: lx carries a counter (above the dedup tolerance) */
static void make_pad(cp_pad_rec *r, int counter, uint32_t t)
{
    memset(r, 0, sizeof *r);
    r->lx = (int16_t)(1000 + counter * 2);
    r->ly = (int16_t)((t / 700) % 2 ? 20000 : -20000);
    r->buttons = (uint16_t)(((t / 300) % 2) ? 0x1000 : 0);
    r->rt = (uint8_t)(((t / 500) % 3) * 100);
}

static void test_sender(void)
{
    printf("-- sender: dedup, rate limit, redundancy, keepalive, flags, restart\n");
    p1_tx t;
    p1_tx_cfg c;
    p1_tx_defaults(&c);
    p1_tx_init(&t, &c);
    cp_pad out;
    cp_pad_rec idle, a;
    memset(&idle, 0, sizeof idle);
    a = idle;
    CHECK(c.redundancy == 3 && c.keepalive_ms == 50 && c.min_interval_ms == 15 && c.stick_eps == 192);
    CHECK(p1_tx_step(&t, 0, &idle, 0, &out) && out.count == 1 && out.seq == 1);
    CHECK(!p1_tx_step(&t, 16, &idle, 0, &out) && !p1_tx_step(&t, 33, &idle, 0, &out));   /* idle: nothing until the keepalive */
    CHECK(p1_tx_step(&t, 50, &idle, 0, &out) && out.seq == 1 && out.count == 1 && t.keepalives == 1);   /* keepalive repeats seq 1 */
    CHECK(!p1_tx_step(&t, 60, &idle, 0, &out));
    a.buttons = 0x1000;
    CHECK(p1_tx_step(&t, 66, &a, 0, &out) && out.seq == 2 && out.count == 2 && out.rec[0].buttons == 0x1000 && out.rec[1].buttons == 0 && out.dt[1] == 66);
    a.buttons = 0;
    CHECK(!p1_tx_step(&t, 70, &a, 0, &out) || out.seq == 2);   /* a change inside 15 ms of the last numbered record waits (or the keepalive) */
    CHECK(p1_tx_step(&t, 82, &a, 0, &out) && out.seq == 3 && out.count == 3 && out.rec[0].buttons == 0 && out.rec[1].buttons == 0x1000);
    /* sticks: a small wobble is the same record, a return to rest is not */
    cp_pad_rec s = a;
    s.lx = 20000;
    CHECK(p1_tx_step(&t, 100, &s, 0, &out) && out.seq == 4);
    s.lx = 20100;
    CHECK(!p1_tx_step(&t, 120, &s, 0, &out));
    s.lx = 20300;   /* 300 from the numbered 20000: over the tolerance */
    CHECK(p1_tx_step(&t, 140, &s, 0, &out) && out.seq == 5);
    s.lx = 0;
    CHECK(p1_tx_step(&t, 160, &s, 0, &out) && out.seq == 6 && out.rec[0].lx == 0);
    s.lx = 100;   /* from rest: leaving zero is always a change */
    CHECK(p1_tx_step(&t, 180, &s, 0, &out) && out.seq == 7);
    /* a hold flag change goes out at once and keeps the seq */
    CHECK(p1_tx_step(&t, 181, &s, CP_SF_PAUSED, &out) && out.seq == 7 && (out.flags & CP_SF_PAUSED));
    CHECK(!p1_tx_step(&t, 182, &s, CP_SF_PAUSED, &out));
    /* a button edge wins over the rate limit only after the interval */
    cp_pad_rec b = s;
    b.buttons = 4;
    CHECK(!p1_tx_step(&t, 190, &b, CP_SF_PAUSED, &out) || out.seq == 7);
    CHECK(p1_tx_step(&t, 200, &b, CP_SF_PAUSED, &out) && out.seq == 8);
    /* wrap of the 16-bit sequence */
    t.seq = 65535;
    b.buttons = 8;
    CHECK(p1_tx_step(&t, 300, &b, 0, &out) && out.seq == 0);
    /* redundancy 1 */
    p1_tx t2;
    c.redundancy = 1;
    p1_tx_init(&t2, &c);
    p1_tx_step(&t2, 0, &idle, 0, &out);
    CHECK(p1_tx_step(&t2, 20, &b, 0, &out) && out.count == 1);
    c.redundancy = 9;
    p1_tx_init(&t2, &c);
    CHECK(t2.cfg.redundancy == CP_PAD_MAX_REC);
    /* restart */
    p1_tx_restart(&t);
    CHECK(p1_tx_step(&t, 400, &b, 0, &out) && out.seq == 1 && out.count == 1);
    /* the packet fits the wire */
    cp_msg m;
    uint8_t buf[CP_MAX_PACKET];
    m.type = CP_PAD, m.u.pad = out;
    CHECK(cp_encode(&m, buf, sizeof buf) > 0);
}

/* The main simulation. */
typedef struct {
    int loss, dup, reorder, base, jitter, seconds;
    int sent_recs, got_recs, order_bad, lost_edges, edges;
    double min_deliver;
} Prof;

static void run_profile(const char *name, int loss, int dup, int reorder, int base, int jitter, double min_deliver)
{
    printf("-- link %s: loss %d%% dup %d%% reorder %d%% delay %d+%d ms\n", name, loss, dup, reorder, base, jitter);
    rng_s = 777 + (unsigned)loss;
    p1_tx tx;
    p1_tx_cfg tc;
    p1_tx_defaults(&tc);
    tc.stick_eps = 0;   /* every frame is a new record: the loss accounting below is then meaningful */
    p1_tx_init(&tx, &tc);
    Sink sink;
    memset(&sink, 0, sizeof sink);
    p1_sink sk = {sink_push, &sink};
    p1_rx rx;
    p1_rx_init(&rx, NULL, sk);
    Link link;
    memset(&link, 0, sizeof link);
    unsigned epoch = 1;
    int counter = 0;
    cp_pad_rec cur;
    uint32_t sent_marks = 0;
    uint32_t now = 1000;
    int unneutral_stall = 0;
    for (uint32_t t = 0; t < 40000; t++, now++) {
        if (t % 16 == 0) {
            counter++;
            make_pad(&cur, counter, t);
            cp_pad p;
            int before = (int)tx.records;
            if (p1_tx_step(&tx, now, &cur, 0, &p)) {
                p.epoch = (uint8_t)epoch;
                link_send(&link, now, &p, loss, dup, reorder, base, jitter);
                if ((int)tx.records > before) sent_marks++;
            }
        }
        link_deliver(&link, now, &rx, epoch);
        if (t % 16 == 8) p1_rx_tick(&rx, now, epoch, 0);
        if (rx.link == P1_LINK_STALLED || rx.mode == P1_MODE_NEUTRAL) unneutral_stall++;
    }
    /* every pushed record is non-decreasing in its counter (lx), the output counter strictly rises */
    int dec = 0, nonneutral = 0, last_lx = -1;
    for (int i = 0; i < sink.n; i++) {
        if (is_neutral(&sink.rec[i])) continue;
        nonneutral++;
        if (sink.rec[i].lx < last_lx) dec++;
        last_lx = sink.rec[i].lx;
    }
    double deliver = (double)rx.st.records / (double)tx.records;
    printf("   sent records %u, received %u (%.1f %%), pushed %d, repeats %u late %u lost %u, stalled frames %d, latency +%.0f max +%u\n", tx.records, rx.st.records, deliver * 100.0, sink.n, rx.st.keepalive_or_dup,
           rx.st.late, rx.st.lost, unneutral_stall, (double)rx.lat_ewma, rx.lat_max);
    CHECK(sink.bad_order == 0);
    CHECK(dec == 0);   /* in order: an old record never overwrites a newer one */
    CHECK(deliver >= min_deliver);
    CHECK(deliver <= 1.0001);   /* each record applied at most once: duplicates do not count twice */
    CHECK(rx.st.records + rx.st.lost >= tx.records - 12 && rx.st.records + rx.st.lost <= tx.records + 4);   /* lost + received accounts for the sent ones */
    CHECK(rx.st.pushed == (uint32_t)sink.n && rx.st.rejected == 0);
    CHECK(nonneutral > 0);
    if (loss == 0) CHECK(rx.st.stalls == 0 && rx.st.losts == 0);
    (void)sent_marks;
}

static void test_deadman_and_holds(void)
{
    printf("-- dead-man, stall, holds (pause, loading, dead, cutscene), epoch, wrap\n");
    p1_tx tx;
    p1_tx_init(&tx, NULL);
    Sink sink;
    memset(&sink, 0, sizeof sink);
    p1_sink sk = {sink_push, &sink};
    p1_rx rx;
    p1_rx_init(&rx, NULL, sk);
    uint32_t now = 5000;
    unsigned epoch = 3;
    cp_pad_rec cur;
    memset(&cur, 0, sizeof cur);
    cur.lx = 20000;
    cur.buttons = 0x2000;
    uint16_t flags = 0;
    cp_pad p;
    int n_before;
    /* idle link: nothing pushed, no neutral spam */
    for (int i = 0; i < 20; i++, now += 16) p1_rx_tick(&rx, now, epoch, 0);
    CHECK(sink.n == 0 && rx.link == P1_LINK_IDLE);
    /* live for 2 s */
    int c = 0;
    for (int i = 0; i < 125; i++, now += 16) {
        cur.rx = (int16_t)(1000 + 400 * (c++ % 40));
        if (p1_tx_step(&tx, now, &cur, flags, &p)) p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
        p1_rx_tick(&rx, now, epoch, 0);
    }
    CHECK(rx.link == P1_LINK_LIVE && rx.mode == P1_MODE_RUN && rx.st.stalls == 0);
    CHECK(sink.n > 20 && !is_neutral(&sink.rec[sink.n - 1]) && sink.rec[sink.n - 1].buttons == 0x2000);
    /* a held stick (an unchanged pad): the sender only repeats its newest record, the sink still gets it again every keepalive (the capture module's own timeout must not zero it) */
    n_before = sink.n;
    for (int i = 0; i < 60; i++, now += 16) {
        if (p1_tx_step(&tx, now, &cur, flags, &p)) p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
        p1_rx_tick(&rx, now, epoch, 0);
    }
    CHECK(sink.n - n_before >= 15 && sink.n - n_before <= 22);   /* ~ one per 50 ms over 960 ms */
    for (int i = n_before; i < sink.n; i++) CHECK(!memcmp(&sink.rec[i], &sink.rec[n_before], sizeof sink.rec[i]) && !is_neutral(&sink.rec[i]));
    /* the cable is pulled: after 400 ms a neutral record, then every 500 ms; after 10 s LOST */
    n_before = sink.n;
    uint32_t cut = now;
    int saw_neutral_at = -1;
    while (now - cut < 9500) {
        now += 16;
        p1_rx_tick(&rx, now, epoch, 0);
        if (saw_neutral_at < 0 && sink.n > n_before) saw_neutral_at = (int)(now - cut);
    }
    CHECK(saw_neutral_at >= 400 && saw_neutral_at <= 450);
    CHECK(rx.link == P1_LINK_STALLED && rx.mode == P1_MODE_NEUTRAL && is_neutral(&sink.rec[sink.n - 1]));
    CHECK(sink.n - n_before >= 17 && sink.n - n_before <= 21);   /* about one neutral per 500 ms over 9.5 s: repeated, not every frame */
    while (now - cut < 10600) now += 16, p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.link == P1_LINK_LOST && rx.st.losts == 1);
    /* return: the buttons come back and one teleport is due (resume) */
    rx.need_tp = 0;
    cur.buttons = 0x4000;
    CHECK(p1_tx_step(&tx, now, &cur, flags, &p));
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.link == P1_LINK_LIVE && rx.mode == P1_MODE_RUN && rx.need_tp && rx.st.resumes == 1);
    CHECK(sink.rec[sink.n - 1].buttons == 0x4000);
    /* pause of the sender: neutral at once, held records are dropped, no dead-man, return = the newest record + teleport */
    flags = CP_SF_PAUSED;
    now += 16;
    cur.buttons = 0x1000;
    p1_tx_step(&tx, now, &cur, flags, &p);
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.mode == P1_MODE_NEUTRAL && is_neutral(&sink.rec[sink.n - 1]));
    n_before = sink.n;
    uint32_t stalls0 = rx.st.stalls;
    for (int i = 0; i < 400; i++, now += 16) {   /* 6.4 s paused with keepalives */
        cur.lx = (int16_t)(2000 + 300 * (i % 30));   /* the sender's stick moves while it is paused (menu cursor): must not move the puppet */
        if (p1_tx_step(&tx, now, &cur, flags, &p)) p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
        p1_rx_tick(&rx, now, epoch, 0);
    }
    CHECK(rx.link == P1_LINK_LIVE && rx.mode == P1_MODE_NEUTRAL && rx.st.stalls == stalls0);
    for (int i = n_before; i < sink.n; i++) CHECK(is_neutral(&sink.rec[i]));
    CHECK(sink.n - n_before <= 14);
    CHECK(rx.st.held_dropped > 0);
    rx.need_tp = 0;
    flags = 0;
    cur.buttons = 0x0100;
    now += 16;
    p1_tx_step(&tx, now, &cur, flags, &p);
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.mode == P1_MODE_RUN && rx.need_tp && sink.rec[sink.n - 1].buttons == 0x0100);
    /* this PC loads (local flag): neutral, and back */
    p1_rx_tick(&rx, now + 16, epoch, CP_SF_LOADING);
    CHECK(rx.mode == P1_MODE_NEUTRAL && is_neutral(&sink.rec[sink.n - 1]));
    now += 32;
    p1_tx_step(&tx, now, &cur, flags, &p);
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.mode == P1_MODE_RUN);
    /* dead sender: neutral while the flag lasts */
    flags = CP_SF_DEAD;
    now += 16;
    p1_tx_step(&tx, now, &cur, flags, &p);
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.mode == P1_MODE_NEUTRAL && (rx.hold_flags & CP_SF_DEAD));
    /* cutscene: silent (one neutral on entry, nothing afterwards) */
    flags = CP_SF_CUTSCENE;
    now += 16;
    p1_tx_step(&tx, now, &cur, flags, &p);
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.mode == P1_MODE_SILENT && is_neutral(&sink.rec[sink.n - 1]));
    n_before = sink.n;
    for (int i = 0; i < 300; i++, now += 16) {
        if (p1_tx_step(&tx, now, &cur, flags, &p)) p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
        p1_rx_tick(&rx, now, epoch, 0);
    }
    CHECK(sink.n == n_before && rx.mode == P1_MODE_SILENT);
    flags = 0;
    rx.need_tp = 0;
    now += 16;
    p1_tx_step(&tx, now, &cur, flags, &p);
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(rx.mode == P1_MODE_RUN && rx.need_tp);

    /* epochs: a packet of another level instance is dropped, a new epoch restarts the sequence */
    uint32_t wrong = rx.st.wrong_epoch;
    cp_pad old = p;
    old.epoch = 2;
    p1_rx_packet(&rx, now, &old, epoch);
    CHECK(rx.st.wrong_epoch == wrong + 1);
    epoch = 4;
    p1_tx_restart(&tx);
    now += 16;
    cur.buttons = 0x0001;
    CHECK(p1_tx_step(&tx, now, &cur, 0, &p) && p.seq == 1);
    p.epoch = 4;
    n_before = sink.n;
    p.epoch = (uint8_t)epoch, p1_rx_packet(&rx, now, &p, epoch);
    p1_rx_tick(&rx, now, epoch, 0);
    CHECK(sink.n > n_before && sink.rec[sink.n - 1].buttons == 0x0001 && rx.need_tp);
    /* a packet from the new epoch arrives before our own epoch changed: dropped, not applied to the old one */
    CHECK(sink.bad_order == 0);
}

static void test_wrap(void)
{
    printf("-- sequence wrap over 65535 with losses\n");
    rng_s = 99;
    p1_tx tx;
    p1_tx_init(&tx, NULL);
    tx.seq = 65500;
    Sink sink;
    memset(&sink, 0, sizeof sink);
    p1_sink sk = {sink_push, &sink};
    p1_rx rx;
    p1_rx_init(&rx, NULL, sk);
    cp_pad_rec cur;
    cp_pad p;
    uint32_t now = 100;
    int order_bad = 0, last = -1;
    for (int i = 0; i < 400; i++, now += 16) {
        memset(&cur, 0, sizeof cur);
        cur.lx = (int16_t)(1000 + i * 3), cur.buttons = (uint16_t)(i & 1 ? 0x10 : 0);
        if (p1_tx_step(&tx, now, &cur, 0, &p) && !chance(30)) p1_rx_packet(&rx, now, &p, 0);
        p1_rx_tick(&rx, now, 0, 0);
    }
    for (int i = 0; i < sink.n; i++) {
        if (is_neutral(&sink.rec[i])) continue;
        if (sink.rec[i].lx < last) order_bad++;
        last = sink.rec[i].lx;
    }
    CHECK(tx.seq < 1000 && order_bad == 0 && rx.st.records > 250);   /* passed 65535 -> small numbers */
}

static void test_correction(void)
{
    printf("-- position correction\n");
    Sink sink;
    memset(&sink, 0, sizeof sink);
    p1_sink sk = {sink_push, &sink};
    p1_rx rx;
    p1_rx_init(&rx, NULL, sk);
    Unit u;
    memset(&u, 0, sizeof u);
    u.exists = 1, u.pos[0] = 100, u.pos[1] = 5, u.pos[2] = 100;
    p1_unit_ops ops = {&u, u_get, u_tp};
    p1_corr_cfg cc;
    p1_corr_defaults(&cc);
    p1_corr_result res;
    float tgt[3] = {100, 5, 100};
    cp_pad p;
    memset(&p, 0, sizeof p);
    p.count = 1, p.seq = 1;
    uint32_t now = 1000;
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    /* the level just started (need_tp): no sample yet -> wait */
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_WAIT);
    p1_rx_state(&rx, now, 1, tgt, 0.5f, 0, now, 1);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_NONE && !rx.need_tp && u.tps == 0);   /* already there: no jump */
    /* inside the band: nothing */
    tgt[0] = 120;
    now += 100;
    p1_rx_state(&rx, now, 1, tgt, 0, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_NONE && u.tps == 0);
    /* soft band: advice only, then a teleport after soft_ms */
    tgt[0] = 100 + 90;
    now += 100;
    p1_rx_state(&rx, now, 1, tgt, 0, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_SOFT && res.dir[0] > 0.99f && res.strength > 0.2f && res.strength < 0.5f && u.tps == 0);
    int soft = 0, tped = 0;
    for (int i = 0; i < 40; i++) {
        now += 50;
        p1_rx_state(&rx, now, 1, tgt, 0, 0, now, 1);
        p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
        p1_rx_tick(&rx, now, 1, 0);
        int a = p1_correct(&rx, now, &cc, &ops, &res);
        soft += a == P1_C_SOFT;
        if (a == P1_C_TP) { tped = 1; CHECK(res.reason == P1_R_SOFT_TIMEOUT); break; }
    }
    CHECK(tped && soft >= 20 && soft <= 35 && u.tps == 1 && u.pos[0] == 190);
    /* hard error: teleport; the rate limit holds a second one back; a jump overrides after 250 ms */
    tgt[0] = 190 + 250;
    now += 1000;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_TP && res.reason == P1_R_HARD && u.tps == 2 && u.yaw == 1.0f);
    u.pos[0] -= 300;
    now += 100;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_WAIT && u.tps == 2);   /* hard inside the 750 ms limit */
    u.pos[0] -= 400;                                                            /* now 700 away: a jump */
    now += 200;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_TP && res.reason == P1_R_JUMP && u.tps == 3);
    /* height error alone */
    u.pos[1] += 200;
    now += 900;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_TP && res.reason == P1_R_HARD && u.pos[1] == 5);
    /* the sender's TELEPORT flag: one teleport per distinct sample stamp, even for a small error */
    tgt[0] += 60;
    now += 300;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, CP_SF_TELEPORT, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_TP && res.reason == P1_R_FLAG);
    int tps = u.tps;
    now += 20;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, CP_SF_TELEPORT, now - 20, 1);   /* the same flagged sample again */
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_NONE && u.tps == tps);
    /* a hold: no correction, wait; the position is not touched while paused */
    p.flags = CP_SF_PAUSED;
    u.pos[0] -= 400;
    now += 1000;
    float before = u.pos[0];
    p1_rx_state(&rx, now, 1, tgt, 1.0f, CP_SF_PAUSED, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_NONE && u.pos[0] == before);
    /* the resume teleport after the hold: wanted but the first fresh sample must be unflagged; then it goes through */
    p.flags = 0;
    now += 20;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(rx.need_tp);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_TP && res.reason == P1_R_RESUME && u.pos[0] == tgt[0]);
    /* no unit (loading), a stale sample, a sample at (0,0,0): wait, never write */
    int t0 = u.tps;
    rx.need_tp = 1;
    u.exists = 0;
    now += 1000;
    p1_rx_state(&rx, now, 1, tgt, 1.0f, 0, now, 1);
    p.epoch = 1, p1_rx_packet(&rx, now, &p, 1);
    p1_rx_tick(&rx, now, 1, 0);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_WAIT);
    u.exists = 1;
    CHECK(p1_correct(&rx, now + 2000, &cc, &ops, &res) == P1_C_WAIT);   /* the sample is 2 s old */
    float zero[3] = {0, 0, 0};
    p1_rx_state(&rx, now, 1, zero, 0, 0, now, 1);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_WAIT);
    p1_rx_state(&rx, now, 1, tgt, 0, 0, now, 1);
    float sv = u.pos[0];
    u.pos[0] = u.pos[1] = u.pos[2] = 0;
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_WAIT);   /* our own unit not placed yet */
    u.pos[0] = sv;
    CHECK(u.tps == t0);
    /* the sample belongs to another epoch */
    p1_rx_state(&rx, now, 1, tgt, 0, 0, now, 7);
    CHECK(p1_correct(&rx, now, &cc, &ops, &res) == P1_C_WAIT);
    /* no ops at all */
    CHECK(p1_correct(&rx, now, &cc, NULL, &res) == P1_C_WAIT);

    /* solo without `net pad state`: no target at all -> never a teleport, whatever the unit does (live: Parker walks and is put back by the scene, tp counter 0) */
    {
        Sink s2;
        memset(&s2, 0, sizeof s2);
        p1_sink sk2 = {sink_push, &s2};
        p1_rx r2;
        p1_rx_init(&r2, NULL, sk2);
        Unit u2;
        memset(&u2, 0, sizeof u2);
        u2.exists = 1, u2.pos[0] = -247.7f, u2.pos[1] = -200, u2.pos[2] = 18165.9f;
        p1_unit_ops ops2 = {&u2, u_get, u_tp};
        uint32_t t2 = 5000;
        for (int i = 0; i < 600; i++, t2 += 16) {
            cp_pad pk2;
            memset(&pk2, 0, sizeof pk2);
            pk2.count = 1, pk2.seq = (uint16_t)(i + 1), pk2.epoch = 1, pk2.rec[0].ly = 20000;
            p1_rx_packet(&r2, t2, &pk2, 1);
            p1_rx_state(&r2, t2, 0, NULL, 0, 0, 0, 1);   /* what puppet_game does without a sample */
            p1_rx_tick(&r2, t2, 1, 0);
            u2.pos[0] += (float)(i % 7) * 3.0f - 9.0f;   /* the unit wanders, also by more than the thresholds */
            p1_correct(&r2, t2, &cc, &ops2, &res);
        }
        CHECK(u2.tps == 0 && r2.st.tp_total == 0 && r2.st.pushed > 0);
    }
}

static void test_state_fill(void)
{
    printf("-- STATE of this PC's character (Revelations 1 reading)\n");
    p1_snapshot s;
    memset(&s, 0, sizeof s);
    s.valid = 1, s.pos[0] = 10, s.pos[1] = 2, s.pos[2] = -5, s.yaw = 1.25f, s.hp = 350.4f, s.hpmax = 1000, s.upper_id = 5, s.mode = 3, s.pstate = 0x40000088u, s.ctrl = 0x1234, s.weapon = 2, s.aiming = 1;
    cp_state st;
    float prev[3] = {10, 2, -5};
    CHECK(p1_state_fill(&st, &s, CP_SF_PAUSED, 255, prev));
    CHECK(st.pos[0] == 10 && st.yaw == 1.25f && st.hp == 350 && st.hpmax == 1000 && st.action == 0x0305u && st.weapon == 2 && st.pstate == 0x40000088u && st.ctrl == 0x1234);
    CHECK((st.flags & CP_SF_AIMING) && (st.flags & CP_SF_FIRING) && (st.flags & CP_SF_PAUSED) && !(st.flags & CP_SF_DEAD) && !(st.flags & CP_SF_TELEPORT));
    CHECK(st.aim == 0 && st.aimv[0] == 0 && st.elev == 0);
    prev[0] = -2000;
    s.unit_flags = 1, s.weapon = -1, s.hp = 0;
    CHECK(p1_state_fill(&st, &s, 0, 0, prev) && (st.flags & CP_SF_DEAD) && (st.flags & CP_SF_TELEPORT) && st.weapon == 0xF);
    s.valid = 0;
    CHECK(!p1_state_fill(&st, &s, 0, 0, NULL));
    /* encodes to the 60 bytes of the protocol */
    s.valid = 1;
    cp_msg m;
    memset(&m, 0, sizeof m);
    m.type = CP_STATE;
    p1_state_fill(&m.u.state, &s, 0, 0, NULL);
    uint8_t b[CP_MAX_PACKET];
    CHECK(cp_encode(&m, b, sizeof b) == 60);
}

int main(void)
{
    test_sender();
    run_profile("clean", 0, 0, 0, 20, 0, 0.999);
    run_profile("lan", 0, 0, 0, 5, 8, 0.999);
    run_profile("loss5", 5, 5, 5, 40, 15, 0.995);
    run_profile("loss25", 25, 10, 10, 50, 30, 0.95);
    run_profile("harsh", 40, 20, 20, 80, 60, 0.80);
    test_deadman_and_holds();
    test_wrap();
    test_correction();
    test_state_fill();
    /* round 2: keep0 - automatic: on for solo or without a real pad on user 0, off as soon as a real pad is there (plugged in after the bind) */
    CHECK(p1_keep0_want(-1, 0, 0) == 1 && p1_keep0_want(-1, 0, 1) == 0);   /* network, keyboard player -> stand-in; a pad plugged in -> off */
    CHECK(p1_keep0_want(-1, 1, 1) == 1 && p1_keep0_want(-1, 1, 0) == 1);   /* solo: always */
    CHECK(p1_keep0_want(0, 0, 0) == 0 && p1_keep0_want(0, 1, 0) == 0 && p1_keep0_want(1, 0, 1) == 1);   /* the console's never / always */
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
