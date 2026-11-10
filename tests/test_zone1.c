/*
 * test_zone1.c - round 9, PUPPET AUTHORITY (zone1.h, zone1_game.inc): the story's event zones count the partner's human only by HIS PC's report.
 *   A  the pure parts: key, wire, slots, the table (refresh, out, lease), the record filter, the class bit
 *   B  two PCs on a line with latency, the l8d situation (cc_must00[9] = the END of Episode 1, sphere (1251.1, -800, 12187) r 160, Jill only): the partner's
 *      puppet stands inside [9] on one PC while his real human stands outside on his own PC -> no END anywhere; then his real human walks in -> the END
 *      once on both PCs. Both role orders (R7 = l8d: the host plays Parker; the lead order). The MODEL BEFORE THE FIX (the rule off) fires the END by the
 *      puppet on the PC that holds it, as l8d did live.
 *   C  timing: the owner's FSM leaves the zone right after its hit (reports stop): the other PC still counts it within the lease; "out" ends a report at once
 *   D  the build facts in the executable image (the two FSM call sites of 0x6D5980 = the return addresses of the glue) and the glue wiring in the sources
 *      (CR-insensitive: the checkout may be CRLF)
 * Build: sh tests/build.sh (64-bit host target; the rule is pure).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/zone1.h"

static int fails, checks;
#define CHECK(c, msg)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        }                                               \
    } while (0)

/* ================================================================ A: pure parts */
static int counts_all(void *u, uint32_t pup) { (void)u; (void)pup; return 1; }
static int counts_none(void *u, uint32_t pup) { (void)u; (void)pup; return 0; }

static void t_pure(void)
{
    /* key */
    uint32_t k1 = z1_key("game\\scene\\13\\13_00\\13_00_cc_must00", 64), k2 = z1_key("GAME/Scene/13/13_00/13_00_CC_MUST00", 64), k3 = z1_key("game\\scene\\13\\13_00\\13_00_event", 64);
    CHECK(k1 == k2 && k1 != k3 && k1 != 0, "key: case and slash insensitive, different paths differ");
    CHECK(z1_key("", 64) == z1_key("", 64) && z1_key("", 64) != 0, "key: never 0");
    /* wire */
    uint8_t b[8];
    uint32_t key;
    unsigned idx, st;
    CHECK(z1_enc(b, sizeof b, k1, 9, Z1_IN) == 8 && z1_dec(b, 8, &key, &idx, &st) && key == k1 && idx == 9 && st == Z1_IN, "wire: round trip in");
    CHECK(z1_enc(b, sizeof b, k1, 300, Z1_OUT) == 8 && z1_dec(b, 8, &key, &idx, &st) && idx == 300 && st == Z1_OUT, "wire: round trip out, 16-bit record");
    b[7] = 2;
    CHECK(!z1_dec(b, 8, &key, &idx, &st), "wire: another version is refused");
    CHECK(!z1_dec(b, 7, &key, &idx, &st) && z1_enc(b, 7, k1, 1, 1) == 0, "wire: short buffers");
    /* record filter and class bit (0x6D34D0 / 0x74BBF0) */
    CHECK(z1_rec_ok(1, 0) && z1_rec_ok(3, 0) && z1_rec_ok(5, 0) && !z1_rec_ok(4, 0) && !z1_rec_ok(5, 1), "records: player-class bit 0, any-mode only (the 12_00_s200_obrevent[0] all-mode record stays stock)");
    CHECK(z1_class_bit(0x80011010u) == 2 && z1_class_bit(0x80011020u) == 4 && z1_class_bit(0x80011000u) == 1 && z1_class_bit(0x800110B0u) == 0x800 && z1_class_bit(0x80031300u) == 0,
          "class bits: Jill 2, Parker 4, enemies none");
    CHECK(z1_unit_counts(0x402, 0x80011010u, 2) && !z1_unit_counts(0x402, 0x80011010u, 4) && !z1_unit_counts(0x002, 0x80011010u, 2) && !z1_unit_counts(0x403, 0x80011010u, 2) &&
              z1_unit_counts(0x402, 0x80011020u, 0xFFFFF006u),
          "unit test: in play (state 2, +0xC 0x400) and the record's class mask (low 12 bits)");
    /* slots */
    uint32_t s[4] = {0x100, 0x200, 0, 0};
    int ch;
    CHECK(z1_slots(s, 0x200, 0, &ch) == 1 && s[1] == 0 && s[0] == 0x100 && ch, "slots: the puppet is removed, the own human stays");
    uint32_t s2[4] = {0x200, 0, 0, 0};
    CHECK(z1_slots(s2, 0x200, 0, &ch) == 0 && !s2[0] && ch, "slots: only the puppet -> no hit");
    uint32_t s3[4] = {0, 0, 0, 0};
    CHECK(z1_slots(s3, 0x200, 1, &ch) == 1 && s3[0] == 0x200 && ch, "slots: a counted report puts the puppet in the first free slot");
    uint32_t s4[4] = {0x200, 0x100, 0, 0};
    CHECK(z1_slots(s4, 0x200, 1, &ch) == 1 && s4[0] == 0x200 && s4[1] == 0x100 && !ch, "slots: counted and there: kept once, nothing written");
    uint32_t s5[4] = {0x100, 0x200, 0x200, 0x200};
    CHECK(z1_slots(s5, 0x200, 1, &ch) == 1 && s5[1] == 0x200 && !s5[2] && !s5[3], "slots: never twice");
    uint32_t s6[4] = {0x100, 0x300, 0x400, 0x500};
    CHECK(z1_slots(s6, 0x200, 1, &ch) == 1 && !ch, "slots: full: nothing to put (the stock had 4 hitters)");
    /* table: own hit -> in, refresh, out; report -> lease */
    static z1_t z;
    z1_init(&z);
    int d = z1_after_test(&z, k1, 9, 1000, 1, 0);
    CHECK((d & Z1_D_LOG_OWN) && !(d & Z1_D_REPORT), "table: first own hit is logged");
    CHECK(z1_next_send(&z, &key, &idx, &st) && key == k1 && idx == 9 && st == Z1_IN && !z1_next_send(&z, &key, &idx, &st), "table: one 'in' queued");
    z1_after_test(&z, k1, 9, 1000 + Z1_REFRESH_MS - 1, 1, 0);
    CHECK(!z1_next_send(&z, &key, &idx, &st), "table: no repeat before the refresh time");
    z1_after_test(&z, k1, 9, 1000 + Z1_REFRESH_MS, 1, 0);
    CHECK(z1_next_send(&z, &key, &idx, &st) && st == Z1_IN, "table: the refresh repeats 'in'");
    z1_after_test(&z, k1, 9, 1500 + Z1_REFRESH_MS, 0, 0);
    CHECK(z1_next_send(&z, &key, &idx, &st) && st == Z1_OUT, "table: a test without the own human sends 'out'");
    z1_after_test(&z, k1, 9, 1600 + Z1_REFRESH_MS, 0, 0);
    CHECK(!z1_next_send(&z, &key, &idx, &st), "table: 'out' once");
    z1_remote(&z, k3, 4, Z1_IN, 5000);
    d = z1_after_test(&z, k3, 4, 5000 + Z1_LEASE_MS - 1, 0, 0);
    CHECK((d & Z1_D_REPORT) && (d & Z1_D_LOG_REP), "lease: a report counts until the lease ends");
    d = z1_after_test(&z, k3, 4, 5000 + Z1_LEASE_MS, 0, 1);
    CHECK(!(d & Z1_D_REPORT) && (d & Z1_D_DROP), "lease: over -> the puppet in it is dropped again");
    z1_remote(&z, k3, 4, Z1_IN, 9000);
    z1_remote(&z, k3, 4, Z1_OUT, 9100);
    d = z1_after_test(&z, k3, 4, 9200, 0, 1);
    CHECK(!(d & Z1_D_REPORT) && (d & Z1_D_DROP), "lease: 'out' ends it at once");
    /* z1_rule */
    uint32_t r1[4] = {0x200, 0, 0, 0};
    int dd;
    CHECK(z1_rule(&z, k1, 2, 10000, r1, 0x100, 0x200, 1, counts_all, NULL, &dd, &ch) == 0 && (dd & Z1_D_DROP) && ch, "rule: a puppet-only stock hit becomes no hit");
    z1_remote(&z, k1, 2, Z1_IN, 10010);
    uint32_t r2[4] = {0, 0, 0, 0};
    CHECK(z1_rule(&z, k1, 2, 10020, r2, 0x100, 0x200, 0, counts_all, NULL, &dd, &ch) == 1 && r2[0] == 0x200 && (dd & Z1_D_REPORT), "rule: a report makes the stock miss a hit of the puppet");
    uint32_t r3[4] = {0, 0, 0, 0};
    CHECK(z1_rule(&z, k1, 2, 10030, r3, 0x100, 0x200, 0, counts_none, NULL, &dd, &ch) == 0 && !r3[0], "rule: a report does not count a puppet out of play / of another class");
    uint32_t r4[4] = {0x100, 0, 0, 0};
    CHECK(z1_rule(&z, k3, 7, 10040, r4, 0x100, 0x200, 1, counts_all, NULL, &dd, &ch) == 1 && !ch, "rule: the own human's hit is the stock result");
    /* reset keeps the counters, drops the records */
    unsigned tests = z.st.tests;
    z1_reset(&z);
    CHECK(z.n == 0 && z.st.tests == tests && !z1_next_send(&z, &key, &idx, &st), "reset: the level's records go, the counters stay");
    /* eviction never drops a pending send */
    z1_init(&z);
    for (unsigned i = 0; i < Z1_MAX; i++) z1_after_test(&z, 1000 + i, 0, 100 + i, 0, 0);
    z1_after_test(&z, 1000, 0, 5000, 1, 0);   /* pending 'in' on the oldest */
    z1_after_test(&z, 99999, 0, 5001, 0, 0);  /* full: evicts the oldest WITHOUT a pending send (1001) */
    int found = 0;
    while (z1_next_send(&z, &key, &idx, &st)) found |= key == 1000;
    CHECK(found && z.st.evicted == 1, "eviction: a pending report survives a full table");
}

/* ================================================================ B: two PCs, the l8d situation */
#define ENT_KEY_PATH "game\\scene\\13\\13_00\\13_00_cc_must00"
#define REC 9
static const float C9[3] = {1251.1f, -800.0f, 12187.0f};
#define R9 160.0f
#define CLS_JILL 0x80011010u
#define CLS_PARKER 0x80011020u

typedef struct {
    uint32_t own_u, pup_u, own_cls, pup_cls;
    float own_p[3], pup_p[3];
    int testing, ended, end_count;
    uint32_t end_ms, stop_testing_at;
    z1_t z;
} pc_t;

#define LINE_MAX 4096
typedef struct { uint32_t at; int to; uint8_t b[8]; } msg_t;
static msg_t line[LINE_MAX];
static int nline;

static int inside(const float p[3]) { float dx = p[0] - C9[0], dz = p[2] - C9[2]; return dx * dx + dz * dz <= R9 * R9 && fabsf(p[1] - C9[1]) < 200.0f; }
static int pc_counts_cls;
static int sim_counts(void *u, uint32_t pup) { pc_t *pc = (pc_t *)u; (void)pup; return z1_unit_counts(0x402, pc->pup_cls, 2); }

/* one frame of one PC: the stock test of record 9 (Jill only), then (rule on) z1_rule; the FSM node fires 13_060_0 on the first Jill bit */
static void pc_frame(pc_t *pc, int self, uint32_t now, int rule, uint32_t key)
{
    if (!pc->testing || pc->ended) return;
    uint32_t s[4] = {0, 0, 0, 0};
    int n = 0;
    if (inside(pc->own_p) && (z1_class_bit(pc->own_cls) & 2)) s[n++] = pc->own_u;
    if (inside(pc->pup_p) && (z1_class_bit(pc->pup_cls) & 2)) s[n++] = pc->pup_u;
    int r = n > 0;
    if (rule) {
        int d, ch;
        r = z1_rule(&pc->z, key, REC, now, s, pc->own_u, pc->pup_u, r, sim_counts, pc, &d, &ch);
        uint32_t k;
        unsigned idx, st;
        while (z1_next_send(&pc->z, &k, &idx, &st) && nline < LINE_MAX) {
            line[nline].at = now + 40, line[nline].to = 1 - self;   /* 40 ms one way */
            z1_enc(line[nline].b, 8, k, idx, st);
            nline++;
        }
    }
    int jill = 0;
    for (int j = 0; j < 4; j++) jill |= s[j] == (pc->own_cls == CLS_JILL ? pc->own_u : pc->pup_u) && s[j];
    if (r && jill) {
        pc->ended = 1, pc->end_count++, pc->end_ms = now;
        pc->testing = 0;   /* the node moves on: 13_060_0 runs */
    }
    if (pc->stop_testing_at && now >= pc->stop_testing_at) pc->testing = 0;
}

static void deliver(pc_t *pcs, uint32_t now)
{
    for (int i = 0; i < nline; i++) {
        if (line[i].at > now) continue;
        uint32_t k;
        unsigned idx, st;
        if (z1_dec(line[i].b, 8, &k, &idx, &st)) z1_remote(&pcs[line[i].to].z, k, idx, st, now);
        line[i] = line[--nline], i--;
    }
}

/* host = pcs[0], guest = pcs[1]. jill_pc = the PC whose human is Jill. Phase 1 (0..5 s): the Jill human stands outside [9] at the tied man's door
 * (1250, -800, 12410), her PUPPET on the other PC stands inside [9] (the l8d divergence, 63 u from the centre). Phase 2: she walks in (2 u per frame). */
typedef struct { int end_host, end_guest, cnt_host, cnt_guest; uint32_t t_host, t_guest, phase2_ms; } result_t;
static result_t run_l8d(int rule, int jill_pc, int walk_in)
{
    static pc_t pcs[2];
    memset(pcs, 0, sizeof pcs);
    nline = 0;
    uint32_t key = z1_key(ENT_KEY_PATH, 64);
    for (int i = 0; i < 2; i++) {
        z1_init(&pcs[i].z);
        pcs[i].own_u = 0x1000u + 0x100u * (unsigned)i, pcs[i].pup_u = 0x5000u + 0x100u * (unsigned)i;   /* units are per-PC pointers */
        pcs[i].own_cls = i == jill_pc ? CLS_JILL : CLS_PARKER, pcs[i].pup_cls = i == jill_pc ? CLS_PARKER : CLS_JILL;
        pcs[i].testing = 1;
    }
    const float door[3] = {1250.0f, -800.0f, 12410.0f}, diverged[3] = {1251.0f, -800.0f, 12250.0f}, parker[3] = {1560.0f, -800.0f, 12890.0f};
    pc_t *J = &pcs[jill_pc], *P = &pcs[1 - jill_pc];
    memcpy(J->own_p, door, sizeof door), memcpy(P->pup_p, diverged, sizeof diverged);   /* her real human outside, her puppet on the other PC inside */
    memcpy(P->own_p, parker, sizeof parker), memcpy(J->pup_p, parker, sizeof parker);
    result_t res;
    memset(&res, 0, sizeof res);
    uint32_t t = 0;
    for (; t < 5000; t += 16) {
        deliver(pcs, t);
        pc_frame(&pcs[0], 0, t, rule, key), pc_frame(&pcs[1], 1, t, rule, key);
    }
    res.phase2_ms = t;
    if (walk_in)
        for (; t < 12000; t += 16) {
            if (J->own_p[2] > C9[2]) J->own_p[2] -= 2.0f;   /* she walks in towards the centre */
            deliver(pcs, t);
            pc_frame(&pcs[0], 0, t, rule, key), pc_frame(&pcs[1], 1, t, rule, key);
        }
    res.end_host = pcs[0].ended, res.end_guest = pcs[1].ended, res.cnt_host = pcs[0].end_count, res.cnt_guest = pcs[1].end_count;
    res.t_host = pcs[0].end_ms, res.t_guest = pcs[1].end_ms;
    return res;
}

static void t_pair(void)
{
    /* R7 = l8d: the host plays Parker (pcs[0] own = Parker), the guest plays Jill (pcs[1]); the host holds Jill's puppet */
    result_t before = run_l8d(0, 1, 0);
    CHECK(before.end_host == 1 && before.end_guest == 0, "MODEL BEFORE THE FIX (R7, l8d): the host's END fires by the guest's PUPPET while her real Jill stands outside");
    printf("l8d model before the fix: host END %d at %u ms (the puppet), guest END %d\n", before.end_host, (unsigned)before.t_host, before.end_guest);
    result_t fix1 = run_l8d(1, 1, 0);
    CHECK(fix1.end_host == 0 && fix1.end_guest == 0, "fix (R7): no END anywhere while the guest's real Jill stands outside, her puppet inside [9] on the host");
    result_t fix2 = run_l8d(1, 1, 1);
    CHECK(fix2.end_guest == 1 && fix2.cnt_guest == 1, "fix (R7): the guest's real Jill walks in: her own PC's END once");
    CHECK(fix2.end_host == 1 && fix2.cnt_host == 1, "fix (R7): the host's END once, by her PC's report");
    CHECK(fix2.t_host >= fix2.t_guest && fix2.t_host - fix2.t_guest <= 40 + 2 * 16, "fix (R7): the host follows within the line latency plus a frame");
    printf("l8d with the fix: guest END at %u ms (her own Jill), host END at %u ms (+%u ms, the report)\n", (unsigned)fix2.t_guest, (unsigned)fix2.t_host, (unsigned)(fix2.t_host - fix2.t_guest));
    /* the lead order: the host plays Jill (pcs[0]); the guest holds Jill's puppet */
    result_t lb = run_l8d(0, 0, 0);
    CHECK(lb.end_guest == 1 && lb.end_host == 0, "MODEL BEFORE THE FIX (lead order): the guest's END fires by the host's puppet");
    result_t l1 = run_l8d(1, 0, 0);
    CHECK(!l1.end_guest && !l1.end_host, "fix (lead order): no END by the host's puppet on the guest");
    result_t l2 = run_l8d(1, 0, 1);
    CHECK(l2.cnt_host == 1 && l2.cnt_guest == 1 && l2.t_guest >= l2.t_host && l2.t_guest - l2.t_host <= 40 + 2 * 16, "fix (lead order): the host's Jill walks in -> the END once on both, the guest by the report");
}

/* ================================================================ C: timing (the lease) */
static void t_timing(void)
{
    static z1_t own, oth;
    uint32_t key = z1_key(ENT_KEY_PATH, 64), k;
    unsigned idx, st;
    int d, ch;
    /* the owner's FSM sees its human in the zone for ONE frame and moves on (its node fired); the other PC starts testing 1.2 s later (its cutscene
     * rendezvous): the report still counts (lease 1.5 s) */
    z1_init(&own), z1_init(&oth);
    uint32_t s[4] = {0x1000, 0, 0, 0};
    z1_rule(&own, key, REC, 1000, s, 0x1000, 0x5000, 1, counts_all, NULL, &d, &ch);
    CHECK(z1_next_send(&own, &k, &idx, &st) && st == Z1_IN, "lease: the single own hit is reported");
    z1_remote(&oth, k, idx, st, 1040);
    uint32_t e[4] = {0, 0, 0, 0};
    CHECK(z1_rule(&oth, key, REC, 2240, e, 0x1100, 0x5100, 0, counts_all, NULL, &d, &ch) == 1, "lease: the other PC counts it 1.2 s later");
    uint32_t e2[4] = {0, 0, 0, 0};
    CHECK(z1_rule(&oth, key, REC, 1040 + Z1_LEASE_MS + 16, e2, 0x1100, 0x5100, 0, counts_all, NULL, &d, &ch) == 0, "lease: not after the lease (the flag mirror carries a late story)");
    /* continuous presence: the owner keeps testing while its human stands inside for 4 s -> the lease never lapses on the other PC */
    z1_init(&own), z1_init(&oth);
    int lapsed = 0;
    for (uint32_t t = 0; t < 4000; t += 16) {
        uint32_t so[4] = {0x1000, 0, 0, 0};
        z1_rule(&own, key, REC, t, so, 0x1000, 0x5000, 1, counts_all, NULL, &d, &ch);
        while (z1_next_send(&own, &k, &idx, &st)) z1_remote(&oth, k, idx, st, t + 40);
        if (t > 200) {
            uint32_t sx[4] = {0, 0, 0, 0};
            lapsed |= !z1_rule(&oth, key, REC, t, sx, 0x1100, 0x5100, 0, counts_all, NULL, &d, &ch);
        }
    }
    CHECK(!lapsed && own.st.sent_in >= 4000 / Z1_REFRESH_MS - 1 && own.st.sent_in <= 4000 / Z1_REFRESH_MS + 2, "presence: refreshes every 400 ms keep the report alive (about 10 'in' in 4 s)");
    /* the owner's human leaves while its FSM still tests: 'out' -> the other PC drops the puppet's hit at once */
    uint32_t so2[4] = {0, 0, 0, 0};
    z1_rule(&own, key, REC, 4000, so2, 0x1000, 0x5000, 0, counts_all, NULL, &d, &ch);
    while (z1_next_send(&own, &k, &idx, &st)) z1_remote(&oth, k, idx, st, 4040);
    uint32_t sx2[4] = {0x5100, 0, 0, 0};
    CHECK(z1_rule(&oth, key, REC, 4056, sx2, 0x1100, 0x5100, 1, counts_all, NULL, &d, &ch) == 0, "out: the partner left (his PC says so): his puppet there no longer counts");
}

/* ================================================================ D: image facts and the glue wiring */
static char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long m = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)m + 1);
    size_t got = fread(b, 1, (size_t)m, f);
    fclose(f);
    b[got] = 0;
    size_t o = 0;   /* drop CRs (autocrlf checkouts) */
    for (size_t i = 0; i < got; i++)
        if (b[i] != '\r') b[o++] = b[i];
    b[o] = 0;
    if (n) *n = o;
    return b;
}

static void t_facts(void)
{
    const char *path = getenv("REV1_IMAGE");
    if (!path || !*path) path = "../../image/rerev-image.bin";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("(no executable image at %s: the call-site facts are not checked)\n", path);
    } else {
        static const uint32_t calls[2] = {0x74BCACu, 0x74C284u};
        uint32_t ra[2] = {0, 0};
        for (int i = 0; i < 2; i++) {
            uint8_t b[8];
            if (fseek(f, (long)(calls[i] - 0x400000u - 3u), SEEK_SET) == 0 && fread(b, 1, 8, f) == 8) {
                int32_t rel = (int32_t)((uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24);
                CHECK(b[0] == 0x8D && b[1] == 0x4B && b[2] == 0x48 && b[3] == 0xE8, "image: `lea ecx,[ebx+0x48]; call` at the FSM call site");
                CHECK(calls[i] + 5u + (uint32_t)rel == 0x6D5980u, "image: the call goes to cEventCollision::HitCheck 0x6D5980");
                ra[i] = calls[i] + 5u;
            } else CHECK(0, "image read");
        }
        uint8_t pro[15], want[15] = {0x57, 0x8b, 0xf9, 0x80, 0x7f, 0x0c, 0x00, 0x75, 0x06, 0x32, 0xc0, 0x5f, 0xc2, 0x04, 0x00};
        CHECK(fseek(f, 0x6D5980L - 0x400000L, SEEK_SET) == 0 && fread(pro, 1, 15, f) == 15 && !memcmp(pro, want, 15), "image: the HitCheck prologue (ret 4, al = hit)");
        fclose(f);
        size_t n;
        char *g = slurp("../native/zone1_game.inc", &n);
        char a[64], b2[64];
        snprintf(a, sizeof a, "#define Z_RA_HITCHECK 0x%XU", (unsigned)ra[0]);
        snprintf(b2, sizeof b2, "#define Z_RA_ARRIVAL 0x%XU", (unsigned)ra[1]);
        for (char *c = g; g && *c; c++)
            if (*c == 'u' && c > g && c[-1] >= '0' && c[-1] <= 'F') *c = 'U';   /* 0x74BCB1u -> 0x74BCB1U for the compare */
        CHECK(g && strstr(g, a) && strstr(g, b2), "glue: the two return addresses are the image's call sites + 5");
        free(g);
    }
    size_t n;
    char *g = slurp("../native/zone1_game.inc", &n), *w = slurp("../native/world1_game.c", &n), *s = slurp("../native/story1_game.inc", &n), *p = slurp("../native/coop_proto.h", &n),
         *h = slurp("../native/world1_hooks.h", &n), *hc = slurp("../native/world1_hooks.c", &n);
    CHECK(g && w && s && p && h && hc, "sources readable");
    if (!(g && w && s && p && h && hc)) return;
    CHECK(strstr(g, "(ra != Z_RA_HITCHECK && ra != Z_RA_ARRIVAL) || !g_active || !f_zone || idx < 0 || !z_pup) return r;"), "glue: only the two FSM callers, only while the model is bound");
    CHECK(strstr(g, "z1_rule(&ZT, key, (unsigned)idx, now, s, own, pup,") && strstr(g, "for (int j = 0; j < 4; j++) sp[j] = s[j];"), "glue: the rule edits the record's hitter slots");
    CHECK(strstr(g, "!z1_rec_ok(type, all)) return r;") && strstr(g, "rcp + 0xB8") && strstr(g, "rcp + 0xC8"), "glue: only any-mode player-class records");
    CHECK(strstr(g, "wh_player_by_slot(&MEM, (unsigned)(1 - g_own), &u) || u != pup) return 0;"), "glue: the puppet is looked up again before it is written into a slot");
    CHECK(strstr(w, "#include \"zone1_game.inc\"") && strstr(w, "{WH_ZONEHIT, (void *)hook_zhit, (void **)&orig_zhit},"), "world: the include and the hook table entry");
    CHECK(strstr(w, "w1n_frame(&N);\n        zone_pass(cs_phase(bound_cs) == CP_PHASE_PLAYING);"), "world: the logic pass every bound frame");
    CHECK(strstr(w, "!strcmp(sub, \"zone\")") && strstr(w, "zone_cmd(a2, arg, out, size);"), "world: the console `world zone`");
    const char *oe = strstr(s, "static void op_ext(void *u, unsigned sub, const uint8_t *b, unsigned len)");
    const char *zz = oe ? strstr(oe, "if (sub == CP_W1_ZONE) {") : NULL, *so = oe ? strstr(oe, "if (!story_on()) return;") : NULL;
    CHECK(zz && so && zz < so, "story: the zone report is taken before the story mirror's switch");
    CHECK(strstr(p, "#define CP_W1_ZONE 16"), "proto: CP_W1_ZONE = 16 (11..31 go to ops.ext; 14 piece, 15 prompt (ep1-probe))");
    const char *ha = strstr(h, "WH_DEATH87,"), *hz = strstr(h, "WH_ZONEHIT,"), *he = strstr(h, "WH_ENPART,");
    CHECK(ha && hz && he && ha < hz && hz < he, "hooks: WH_ZONEHIT among the detours, before the ep1-solo block");
    const char *ta = strstr(hc, "{\"DeathEnemy\""), *tz = strstr(hc, "{\"ZoneHitCheck\", 0x6D5980u"), *te = strstr(hc, "{\"FsmSetEnablePartner\"");
    CHECK(ta && tz && te && ta < tz && tz < te, "hooks: the site table entry is in the enum's place");
    free(g), free(w), free(s), free(p), free(h), free(hc);
}

int main(void)
{
    (void)pc_counts_cls;
    t_pure();
    t_pair();
    t_timing();
    t_facts();
    printf("test_zone1: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
