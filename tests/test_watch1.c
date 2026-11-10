/*
 * test_watch1.c - rev1 live round 13, task 3: the watching PC shows the enemies around the human it watches (native/watch1.h), on a model of both PCs.
 *  A  R7 solo part: the host WATCHES, the client's Jill fights the 13_048_0 zombies on the client's AI (local1.h). Before the fix the host's units stay hidden
 *     where they were made (l12a / l12e: E1C 0x1000) and the watcher sees no enemy; with the fix they are replicas there, follow the client's records and stay
 *     visible after the zone pass, the host's batch keeps saying "no unit here" (the client keeps them), the client's death report kills them.
 *  B  the model "only un-hide on the watcher" without the reverse rule: the host's AI wakes next to the puppet and its records make the client hand them back
 *     = two owners in turn (the inconsistency that kept watchzone a LAB switch); the fix never lets that happen.
 *  C  lead order solo part: the guest WATCHES the host's Jill; the host simulates, the guest's replicas are hidden by its zone pass before the fix, visible after.
 *  D  the take-back: the host's own human comes back near after the solo part, or the client's records stop: the host runs the enemy again and the client
 *     hands it back; never both AIs for longer than LR_BACK_MS + a pass.
 *  E  the message: round trip, older sequences dropped across the 16-bit wrap, bad lengths; the minimap scope.
 *  F  the glue in the engine files (source checks).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/watch1.h"

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* ---- one enemy on both PCs */
typedef struct {
    uint16_t key;
    /* host */
    int h_unit, h_kind, h_hidden, h_spent, h_dead;
    float h_pos[3], h_hp;
    /* client */
    int c_kind, c_hidden, c_dead;
    float c_pos[3], c_hp;
} en_t;

#define W1_KEY_T(b, i) ((((unsigned)(b)) << 8) | ((unsigned)(i) & 0xFF))
static int fixed = 1;
static int unhide_only;          /* B: the watcher un-hides the host's units without the reverse rule */
static lr_t CL;                  /* the client's local1 table */
static wt_host HT;               /* the host's watch table */
static wt_tx TX;
static float g_view[3];          /* the host's view point (the watched human while it watches, else its own human) */

/* the host's batch record of the enemy (host_pass): ai, e1c */
static void host_record(const en_t *e, unsigned *ai, uint32_t *e1c)
{
    unsigned a = e->h_unit && !e->h_dead ? 7u : LR_AI_NOUNIT;
    *ai = fixed ? wt_batch_ai(&HT, e->key, a) : a;
    *e1c = 0x40000008u | (e->h_hidden ? LR_E1C_HIDDEN : 0);
}

/* the host's engine: its zone pass hides every unit away from its own (active) human; a host-owned kind 2 unit that is not hidden runs its AI (it walks
 * towards the human its AI sees: here the puppet of the client's human) */
static void host_engine(en_t *e, int host_watching, float host_own_dist, const float puppet[3], int *host_ai_frames)
{
    if (!e->h_unit || e->h_dead) return;
    int in_own_zone = !host_watching && host_own_dist < 1200.0f;
    e->h_hidden = !in_own_zone;
    if (unhide_only && host_watching) e->h_hidden = 0;   /* B: the watcher's un-hide (watchzone-like) */
    if (e->h_kind == 2 && !e->h_hidden) {               /* the host's AI runs it */
        (*host_ai_frames)++;
        for (int k = 0; k < 3; k += 2) e->h_pos[k] += (puppet[k] - e->h_pos[k]) * 0.05f;
    }
}

/* the client's per-pass rule (local1.h) + its sender (watch1.h rule 1) */
static void client_pass(en_t *e, float dist_own, uint32_t now, int *client_ai_frames)
{
    unsigned ai;
    uint32_t e1c;
    host_record(e, &ai, &e1c);
    if (!e->h_dead) lr_on_record(&CL, e->key, ai, e1c, now);
    int alive = !e->c_dead && e->c_hp > 0.0f;
    int a = lr_step(&CL, e->key, 0x24000000u + e->key, (unsigned)e->c_kind, alive, e->c_hp, dist_own, now);
    if (a == LR_MAKE_LOCAL) e->c_kind = 2;
    if (a == LR_MAKE_REPLICA || a == LR_HAND_BACK) {
        e->c_kind = 3;
        if (a == LR_HAND_BACK && e->h_unit && e->c_hp < e->h_hp) e->h_hp = e->c_hp;   /* LR_REC_HP: the host takes the lower hp */
    }
    if (a == LR_REPORT_DEATH) {   /* LR_REC_DEATH reaches the host */
        int hidden = fixed ? wt_report_hidden(&HT, e->key, e->h_hidden) : e->h_hidden;
        int d = lr_host_decide(LR_REC_DEATH, e->h_unit && !e->h_dead, hidden, e->h_spent, e->h_hp, 0.0f);
        if (d == LR_H_SPEND_KILL) e->h_dead = 1, e->h_spent = 1;
        if (d == LR_H_SPEND) e->h_spent = 1;
    }
    if (e->c_kind == 2 && alive) (*client_ai_frames)++;
}

/* rule 1 + 2: the client's records (every WT_TX_MS) reach the host, which applies them */
static void records(en_t *es, int n, uint32_t now, int host_watching, float host_own_dist)
{
    if (!fixed || unhide_only) return;
    if (wt_tx_due(&TX, now)) {
        wt_rec r[WT_PER_MSG];
        unsigned m = 0;
        for (int i = 0; i < n && m < WT_PER_MSG; i++) {
            lr_ent *le = lr_find(&CL, es[i].key, 0);
            int alive = !es[i].c_dead && es[i].c_hp > 0.0f && es[i].c_kind == 2;
            if (!le || !wt_tx_wants(le->local, le->reported, alive)) continue;
            r[m].key = es[i].key, r[m].hp = es[i].c_hp;
            memcpy(r[m].pos, es[i].c_pos, sizeof r[m].pos);
            m++;
        }
        if (m) {
            uint8_t b[WT_MSG_MAX];
            unsigned len = wt_enc(b, ++TX.seq, r, m);
            wt_rx(&HT, b, len, now);
        }
    }
    for (int i = 0; i < n; i++) {
        wt_ent *we = wt_find(&HT, es[i].key, 0);
        if (!we) continue;
        int alive = es[i].h_unit && !es[i].h_dead;
        float vx = we->pos[0] - g_view[0], vz = we->pos[2] - g_view[2];
        int a = wt_host_step(&HT, we, alive ? (unsigned)es[i].h_kind : 0, !host_watching, host_own_dist, sqrtf(vx * vx + vz * vz), now);
        if (a == WT_MAKE_REPLICA) es[i].h_kind = 3;
        if (a == WT_TAKE_BACK) es[i].h_kind = 2;
        if (we->active && alive && es[i].h_kind == 3 && wt_fresh(we, now)) {
            memcpy(es[i].h_pos, we->pos, sizeof es[i].h_pos);
            if (we->hp > 0.0f) es[i].h_hp = we->hp;
        }
    }
}

/* rule 4 on a PC: is the enemy drawn there (not hidden after the zone pass + the watcher's un-hide) */
static int drawn_host(const en_t *e, int watching, const float watched[3])
{
    if (!e->h_unit || e->h_dead) return 0;
    if (!e->h_hidden) return 1;
    float dx = e->h_pos[0] - watched[0], dz = e->h_pos[2] - watched[2];
    return fixed && !unhide_only && wt_keep_visible(1, watching, (unsigned)e->h_kind, 1, sqrtf(dx * dx + dz * dz));
}
static int drawn_client(const en_t *e, int watching, const float watched[3])
{
    if (e->c_dead) return 0;
    if (!e->c_hidden) return 1;
    float dx = e->c_pos[0] - watched[0], dz = e->c_pos[2] - watched[2];
    return fixed && wt_keep_visible(1, watching, (unsigned)e->c_kind, 1, sqrtf(dx * dx + dz * dz));
}

static float d2(const float a[3], const float b[3]) { float dx = a[0] - b[0], dz = a[2] - b[2]; return sqrtf(dx * dx + dz * dz); }

/* A / B: the R7 solo part (host Parker watches far away, the client's Jill at the key). Returns 1 when everything of the fixed rule holds. */
typedef struct { int min_drawn_host, host_ai_frames, both_ai_frames, max_err, killed_on_host, handed_back; } a_out;
static a_out run_r7(void)
{
    a_out o = {99, 0, 0, 0, 0, 0};
    en_t es[2];
    memset(es, 0, sizeof es);
    lr_reset(&CL), wt_host_reset(&HT), memset(&TX, 0, sizeof TX);
    float jill[3] = {25.0f, -600.0f, 8581.0f}, parker[3] = {-2000.0f, -800.0f, 2000.0f};
    memcpy(g_view, jill, sizeof g_view);   /* the host watches Jill */
    for (int i = 0; i < 2; i++) {
        es[i].key = (uint16_t)W1_KEY_T(0, i);
        es[i].h_unit = 1, es[i].h_kind = 2, es[i].h_hidden = 1, es[i].h_hp = 1100.0f + 100.0f * i;   /* the host's SpawnChara: hidden at once (Parker far) */
        es[i].h_pos[0] = -109.0f - 160.0f * i, es[i].h_pos[1] = -600.0f, es[i].h_pos[2] = 8515.0f;
        es[i].c_kind = 3, es[i].c_hidden = 0, es[i].c_hp = es[i].h_hp;
        memcpy(es[i].c_pos, es[i].h_pos, sizeof es[i].c_pos);
    }
    int host_ai = 0, client_ai = 0;
    uint32_t now = 1000;
    for (int f = 0; f < 1200; f++, now += 33) {   /* 40 s at 30 passes a second */
        float hd = d2(parker, es[0].h_pos);
        int host_ai_before = host_ai, client_ai_before = client_ai;
        for (int i = 0; i < 2; i++) host_engine(&es[i], 1, hd, jill, &host_ai);
        for (int i = 0; i < 2; i++) {
            if (es[i].c_kind == 2 && !es[i].c_dead) {   /* the client's AI walks to Jill and around */
                es[i].c_pos[0] += (jill[0] + 60.0f * (i ? 1 : -1) - es[i].c_pos[0]) * 0.03f + 3.0f * sinf(f * 0.05f);
                es[i].c_pos[2] += (jill[2] - es[i].c_pos[2]) * 0.03f;
            }
            client_pass(&es[i], d2(jill, es[i].c_pos), now, &client_ai);
        }
        records(es, 2, now, 1, hd);
        if (host_ai > host_ai_before && client_ai > client_ai_before) o.both_ai_frames++;
        if (f > 150 && f < 900) {   /* after the take (2.5 s) and the first records, until the kills */
            int dr = drawn_host(&es[0], 1, jill) + drawn_host(&es[1], 1, jill);
            if (dr < o.min_drawn_host) o.min_drawn_host = dr;
            for (int i = 0; i < 2; i++) {
                int err = (int)d2(es[i].h_pos, es[i].c_pos);
                if (err > o.max_err) o.max_err = err;
            }
        }
        if (f == 900) es[0].c_hp = 0.0f, es[0].c_dead = 1;   /* Jill kills 0/0 on the client */
        if (f == 960) es[1].c_hp = 0.0f, es[1].c_dead = 1;
    }
    o.host_ai_frames = host_ai;
    o.killed_on_host = es[0].h_dead + es[1].h_dead;
    o.handed_back = (int)CL.gave_back;
    return o;
}

/* C: the lead order solo part: the host's Jill plays the enemies (host AI), the guest's Parker watches far away: its replicas are hidden by its zone pass */
static int run_lead(void)
{
    en_t es[2];
    memset(es, 0, sizeof es);
    lr_reset(&CL), wt_host_reset(&HT), memset(&TX, 0, sizeof TX);
    float jill[3] = {25.0f, -600.0f, 8581.0f};
    int host_ai = 0, client_ai = 0, min_drawn = 99, took = 0;
    uint32_t now = 1000;
    for (int i = 0; i < 2; i++) {
        es[i].key = (uint16_t)W1_KEY_T(0, i);
        es[i].h_unit = 1, es[i].h_kind = 2, es[i].h_hidden = 0, es[i].h_hp = 1100.0f;
        es[i].h_pos[0] = 300.0f * i, es[i].h_pos[1] = -600.0f, es[i].h_pos[2] = 8500.0f;
        es[i].c_kind = 3, es[i].c_hidden = 1, es[i].c_hp = 1100.0f;   /* the guest's zone pass: its Parker is far */
        memcpy(es[i].c_pos, es[i].h_pos, sizeof es[i].c_pos);
    }
    for (int f = 0; f < 600; f++, now += 33) {
        for (int i = 0; i < 2; i++) {
            host_engine(&es[i], 0, d2(jill, es[i].h_pos), jill, &host_ai);
            memcpy(es[i].c_pos, es[i].h_pos, sizeof es[i].c_pos);   /* the host's batch drives the replica */
            client_pass(&es[i], 4000.0f /* the guest's Parker is far */, now, &client_ai);
            if (es[i].c_kind != 3) took++;
        }
        records(es, 2, now, 0, d2(jill, es[0].h_pos));
        if (f > 30) {
            int dr = drawn_client(&es[0], 1, jill) + drawn_client(&es[1], 1, jill);
            if (dr < min_drawn) min_drawn = dr;
        }
    }
    CHECK(took == 0, "C lead order: the guest never takes an enemy the host simulates");
    CHECK(HT.made == 0, "C lead order: no reverse replica on the host");
    return min_drawn;
}

/* D: after the solo part: the host's Parker comes back (not watching) next to an enemy the client still runs; then a second enemy whose records stop */
static void run_takeback(void)
{
    en_t e;
    memset(&e, 0, sizeof e);
    lr_reset(&CL), wt_host_reset(&HT), memset(&TX, 0, sizeof TX);
    float jill[3] = {0.0f, 0.0f, 0.0f}, parker[3] = {-5000.0f, 0.0f, 0.0f};
    e.key = 0x0001, e.h_unit = 1, e.h_kind = 2, e.h_hidden = 1, e.h_hp = 1000.0f, e.c_kind = 3, e.c_hp = 1000.0f;
    e.c_pos[0] = e.h_pos[0] = 300.0f;
    int host_ai = 0, client_ai = 0, max_both_run = 0, run = 0;
    uint32_t now = 1000;
    for (int f = 0; f < 900; f++, now += 33) {
        int watching = f < 300;   /* the solo part ends at f 300 */
        memcpy(g_view, watching ? jill : parker, sizeof g_view);
        if (f >= 300 && parker[0] < 200.0f) parker[0] += 40.0f;   /* Parker walks back to the enemy */
        int ha = host_ai, ca = client_ai;
        host_engine(&e, watching, d2(parker, e.h_pos), jill, &host_ai);
        if (e.c_kind == 2) e.c_pos[0] += 2.0f * sinf(f * 0.1f);
        client_pass(&e, d2(jill, e.c_pos), now, &client_ai);
        records(&e, 1, now, watching, d2(parker, e.h_pos));
        int b = host_ai > ha && client_ai > ca;
        run = b ? run + 1 : 0;
        if (run > max_both_run) max_both_run = run;
    }
    CHECK(HT.made >= 1, "D the host made a reverse replica during the solo part");
    CHECK(HT.taken_back >= 1, "D the host took the enemy back when its active human came near");
    CHECK(e.h_kind == 2 && e.c_kind == 3, "D after the take-back: the host runs it, the client shows a replica");
    CHECK(CL.gave_back >= 1, "D the client handed it back (local1.h rule 5)");
    CHECK(max_both_run * 33 <= (int)LR_BACK_MS + 300, "D both AIs at once only for the hand-back delay");
    /* the records stop (the link stalls) while the host watches and its unit is a reverse replica: taken back when they are stale */
    wt_host_reset(&HT);
    uint8_t b[WT_MSG_MAX];
    wt_rec r = {0x0002, {10.0f, 0.0f, 10.0f}, 500.0f};
    wt_rx(&HT, b, wt_enc(b, 7, &r, 1), 5000);
    wt_ent *we = wt_find(&HT, 0x0002, 0);
    CHECK(we && wt_host_step(&HT, we, 2, 0, 9999.0f, 100.0f, 5100) == WT_MAKE_REPLICA, "D2 a fresh record makes the host's own-AI unit a replica");
    CHECK(we && wt_host_step(&HT, we, 3, 0, 9999.0f, 100.0f, 5000 + WT_STALE_MS - 1) == WT_KEEP, "D2 still fresh: kept");
    CHECK(we && wt_host_step(&HT, we, 3, 0, 9999.0f, 100.0f, 5000 + WT_STALE_MS + 200) == WT_TAKE_BACK, "D2 stale records: the host takes it back");
    CHECK(!wt_reverse(&HT, 0x0002), "D2 no reverse replica after the take-back");
    /* D3 the m13c case: a record 3500 u from the watched human (no stage streamed there on the host) never becomes a replica */
    wt_host_reset(&HT);
    wt_rec r3 = {0x0003, {787.0f, -100.0f, 10950.0f}, 1600.0f};
    wt_rx(&HT, b, wt_enc(b, 9, &r3, 1), 7000);
    we = wt_find(&HT, 0x0003, 0);
    CHECK(we && wt_host_step(&HT, we, 2, 0, 9999.0f, 3500.0f, 7050) == WT_KEEP && !wt_reverse(&HT, 0x0003), "D3 far from the host's view point: left as it is (no replica, no position written)");
    CHECK(we && wt_host_step(&HT, we, 2, 0, 9999.0f, 2000.0f, 7100) == WT_MAKE_REPLICA, "D3 the same record near the view point: a replica");
}

static void run_msgs(void)
{
    uint8_t b[WT_MSG_MAX];
    wt_rec in[WT_PER_MSG], out[WT_PER_MSG];
    memset(in, 0, sizeof in), memset(out, 0, sizeof out);
    for (unsigned i = 0; i < WT_PER_MSG; i++) in[i].key = (uint16_t)(0x0100 + i), in[i].pos[0] = 1.5f * i, in[i].pos[1] = -600.0f, in[i].pos[2] = 8000.0f + i, in[i].hp = 900.0f - i;
    unsigned len = wt_enc(b, 0xFFFE, in, WT_PER_MSG), n = 0;
    uint16_t seq = 0;
    CHECK(len == WT_MSG_MAX && WT_MSG_MAX <= 92, "E the message fits the ext body (W1_EXT_MAX 92)");
    CHECK(wt_dec(b, len, &seq, out, &n) && n == WT_PER_MSG && seq == 0xFFFE && !memcmp(in, out, sizeof in), "E round trip");
    CHECK(!wt_dec(b, len - 1, &seq, out, &n), "E a short message is refused");
    b[0] = 0;
    CHECK(!wt_dec(b, len, &seq, out, &n), "E a count of 0 is refused");
    wt_host h;
    wt_host_reset(&h);
    CHECK(wt_rx(&h, b + 0, wt_enc(b, 0xFFFE, in, 1), 100) == 1, "E seq fffe taken");
    CHECK(wt_rx(&h, b, wt_enc(b, 0x0001, in, 1), 200) == 1, "E seq 0001 (after the wrap) taken");
    CHECK(wt_rx(&h, b, wt_enc(b, 0xFFFF, in, 1), 300) == 0 && h.rx_old == 1, "E seq ffff after 0001 is older: dropped");
    CHECK(wt_rx(&h, b, wt_enc(b, 0x0001, in, 1), 300) == 0, "E a duplicate is dropped");
    CHECK(wt_map_scope(1, 1, 1) == 0 && wt_map_scope(1, 1, 0) == 1, "E the minimap of a watching PC follows the other slot");
    CHECK(wt_map_scope(1, 0, 1) == -1 && wt_map_scope(0, 1, 1) == -1, "E no scope when not watching / switched off");
    CHECK(!wt_keep_visible(1, 1, 2, 1, 100.0f), "E an own-AI unit is never un-hidden by the watcher (its AI would wake)");
    CHECK(!wt_keep_visible(1, 0, 3, 1, 100.0f), "E a PC that does not watch changes nothing");
    CHECK(!wt_keep_visible(1, 1, 3, 1, WT_NEAR + 1.0f), "E far from the watched human: left to the zone pass");
    CHECK(!wt_keep_visible(1, 1, 3, 0, 100.0f), "E a dead replica is not kept");
    {   /* rule 6: the m13b case (0/2 at 2023 u from the watching guest's switched-off Parker, not simulated by the host) */
        lr_t L6;
        lr_reset(&L6);
        int took_fixed = 0, took_before = 0;
        for (uint32_t now = 1000; now < 9000; now += 250) {
            lr_on_record(&L6, 0x0002, LR_AI_NOUNIT, LR_E1C_HIDDEN, now);
            took_fixed |= lr_step(&L6, 0x0002, 0x24747a10u, 3, 1, 1600.0f, wt_local_dist(1, 2023.0f), now) == LR_MAKE_LOCAL;
        }
        lr_reset(&L6);
        for (uint32_t now = 1000; now < 9000; now += 250) {
            lr_on_record(&L6, 0x0002, LR_AI_NOUNIT, LR_E1C_HIDDEN, now);
            took_before |= lr_step(&L6, 0x0002, 0x24747a10u, 3, 1, 1600.0f, 2023.0f, now) == LR_MAKE_LOCAL;
        }
        CHECK(!took_fixed, "E rule 6: a watching PC takes no enemy near its switched-off human");
        CHECK(took_before, "E rule 6: the model before (m13b) takes it (the check above fails for it)");
        CHECK(wt_local_dist(0, 500.0f) == 500.0f, "E rule 6: an active PC keeps the distance");
    }
    wt_tx t;
    memset(&t, 0, sizeof t);
    CHECK(wt_tx_due(&t, 1000) && !wt_tx_due(&t, 1000 + WT_TX_MS - 1) && wt_tx_due(&t, 1000 + WT_TX_MS), "E the sender's rate");
    CHECK(!wt_tx_wants(0, 0, 1) && !wt_tx_wants(1, 1, 1) && !wt_tx_wants(1, 0, 0) && wt_tx_wants(1, 0, 1), "E only live local enemies are sent");
}

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static void run_sources(void)
{
    char *w = slurp("../native/world1_game.c"), *l = slurp("../native/local1_game.inc"), *s = slurp("../native/story1_game.inc"), *g = slurp("../native/watch1_game.inc"),
         *p = slurp("../native/coop_proto.h");
    CHECK(w && strstr(w, "wt_batch_ai(&WTH, key, lr_host_ai("), "F host_pass sends \"no unit here\" for a reverse replica");
    CHECK(w && strstr(w, "#include \"watch1_game.inc\"") && strstr(w, "watch_pass(now);") && strstr(w, "watch_map_revert();"), "F the glue is in the frame and the revert");
    CHECK(l && strstr(l, "wt_report_hidden(&WTH, key,"), "F local_rx decides a reverse replica's death report as a hidden unit's");
    CHECK(l && strstr(l, "watch_after_zpass();") && strstr(l, "wt_host_reset(&WTH);"), "F the zone-pass hook un-hides; the level reset clears the table");
    CHECK(s && strstr(s, "sub == CP_W1_RLOC") && strstr(s, "watch_rx(b, len)"), "F op_ext routes CP_W1_RLOC");
    CHECK(p && strstr(p, "#define CP_W1_RLOC 22"), "F CP_W1_RLOC = 22 (inside CP_W1_EXT_FIRST..LAST)");
    CHECK(g && strstr(g, "rd32(en + 4) != u") && strstr(g, "& ~LR_E1C_HIDDEN"), "F the hook checks that the entry still points at the unit before it un-hides");
    CHECK(g && strstr(g, "strcmp(cn, \"uMiniMapManager\")") && strstr(g, "capture_own_scope(s)"), "F the minimap copy only on a uMiniMapManager, with the own-slot scope");
    free(w), free(l), free(s), free(g), free(p);
}

int main(void)
{
    /* A: R7 solo part */
    fixed = 1, unhide_only = 0;
    a_out a = run_r7();
    CHECK(a.min_drawn_host == 2, "A fixed: the watching host draws both zombies all the time");
    CHECK(a.max_err <= 60, "A fixed: the host's replicas follow the client's zombies (position error <= 60 u)");
    CHECK(a.host_ai_frames == 0, "A fixed: the host's AI never runs them (no bite on the puppet)");
    CHECK(a.handed_back == 0, "A fixed: the client keeps them (no hand-back loop)");
    CHECK(a.killed_on_host == 2, "A fixed: the client's kills end both on the host too");
    printf("A fixed: min drawn %d, max error %d, host AI frames %d, hand-backs %d, killed on the host %d\n", a.min_drawn_host, a.max_err, a.host_ai_frames, a.handed_back, a.killed_on_host);
    fixed = 0;
    a_out a0 = run_r7();
    CHECK(a0.min_drawn_host == 0, "A before the fix (pre-fix model): the watching host draws no zombie (the l12a / l12e void of enemies)");
    printf("A before the fix: min drawn %d (the check above FAILS for this model as it should: drawn > 0 is the fix)\n", a0.min_drawn_host);
    CHECK(!(a0.min_drawn_host == 2), "A the fixed-rule check fails for the pre-fix model");
    /* B: un-hide only (watchzone-like) */
    fixed = 1, unhide_only = 1;
    a_out b = run_r7();
    CHECK(b.host_ai_frames > 0, "B un-hide only: the host's AI wakes next to the puppet (the client's real Jill then fights only replicas: the reason for the reverse rule)");
    printf("B un-hide only: host AI frames %d, hand-backs %d, both AIs frames %d\n", b.host_ai_frames, b.handed_back, b.both_ai_frames);
    unhide_only = 0;
    /* C: lead order */
    fixed = 1;
    int c = run_lead();
    CHECK(c == 2, "C fixed: the watching guest draws both of the host's enemies");
    fixed = 0;
    int c0 = run_lead();
    CHECK(c0 == 0, "C before the fix: the watching guest's replicas stay hidden");
    fixed = 1;
    /* D, E, F */
    run_takeback();
    run_msgs();
    run_sources();
    printf("test_watch1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
