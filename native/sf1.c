/* sf1.c: the Episode 1 story mirror policy (see sf1.h). Pure C, no Windows and no game dependencies. */
#include "sf1.h"

#include <stdio.h>
#include <string.h>

/* the Episode 1 whitelist (notes\ep1-world.md 2.1; ids from work\d-story\out\evmap_11/12/13.txt): 1 + 84 + 129 + 161 + 5 = 380 */
static const struct { uint32_t lo, hi; } R[] = {
    {257, 257}, {4374, 4457}, {4608, 4736}, {4864, 5024}, {16725, 16729},
};
#define NR (sizeof R / sizeof R[0])

/* the set pieces whose break is mirrored (class hash): only classes whose break path was read (notes\ep1-world.md 1.3) */
static const uint32_t PIECES[] = {
    0x80061600u,   /* uOMChainLock: uBreakChain_vf43 0x626680 breaks it when hp <= 0 */
};

int sf1_index(uint32_t id)
{
    unsigned base = 0;
    for (unsigned i = 0; i < NR; i++) {
        if (id >= R[i].lo && id <= R[i].hi) return (int)(base + (id - R[i].lo));
        base += R[i].hi - R[i].lo + 1;
    }
    return -1;
}

uint32_t sf1_id_at(unsigned idx)
{
    for (unsigned i = 0; i < NR; i++) {
        unsigned n = R[i].hi - R[i].lo + 1;
        if (idx < n) return R[i].lo + idx;
        idx -= n;
    }
    return 0;
}

uint16_t sf1_layout(void)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < NR; i++) {
        uint32_t v[2] = {R[i].lo, R[i].hi};
        for (int j = 0; j < 2; j++)
            for (int b = 0; b < 4; b++) h = (h ^ ((v[j] >> (8 * b)) & 0xFF)) * 16777619u;
    }
    return (uint16_t)(h ^ (h >> 16));
}

int sf1_crosses(const sf1_t *s, uint32_t id) { return sf1_index(id) >= 0 || (s && s->wide && id >= 256 && id < 0x10000); }

static int bit(const uint8_t *b, int i) { return i >= 0 && ((b[i >> 3] >> (i & 7)) & 1); }
static void bset(uint8_t *b, int i) { if (i >= 0) b[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static const char *const WHY[SF1_W_N] = {"-", "not mirrored", "unchanged", "a clear (not sent by the guest)", "the host already decided it", "quiet window after the load", "no epoch", "wrong role", "hold table full"};
const char *sf1_why_name(int why) { return why >= 0 && why < SF1_W_N ? WHY[why] : "?"; }

void sf1_init(sf1_t *s, int role)
{
    memset(s, 0, sizeof *s);
    s->role = role;
    s->rate_last = -1;
}

void sf1_epoch(sf1_t *s, unsigned epoch, uint32_t now)
{
    if (s->epoch_valid && s->epoch == epoch) return;
    int role = s->role, wide = s->wide;
    sf1_stats st = s->st;
    sf1_init(s, role);
    s->wide = wide, s->st = st;
    s->epoch_valid = 1, s->epoch = epoch, s->epoch_ms = now, s->quiet_ms = now;
}

/* e1w2 (2026-11-08) live: after the host's restart call the session barrier opened within 3 s (the units exist, at 0,0,0) while the 1-1 start movie played for
 * ~110 s; the level start scripts (4449 ...) ran after it, outside a window that had started at the epoch: the digest went out before them (0 flags) and the
 * guest's own level start set crossed as a proposal before the host's own script had run. The window now starts when this PC's human is placed. */
static uint32_t qstart(const sf1_t *s) { return s->unplaced ? s->epoch_ms + SF1_PLACE_MAX_MS : s->quiet_ms; }
int sf1_in_quiet(const sf1_t *s, uint32_t now) { return s->epoch_valid && (int32_t)(now - qstart(s)) < (int32_t)SF1_QUIET_MS; }
void sf1_unplaced(sf1_t *s) { if (s->epoch_valid) s->unplaced = 1; }
int sf1_placed(sf1_t *s, uint32_t now)
{
    if (!s->epoch_valid || !s->unplaced) return 0;
    s->unplaced = 0;
    uint32_t cap = s->epoch_ms + SF1_PLACE_MAX_MS;
    s->quiet_ms = (int32_t)(now - cap) < 0 ? now : cap;   /* after the cap the window had started at the cap already: it stays there */
    return 1;
}
int sf1_may_propose(const sf1_t *s, uint32_t now) { return s->epoch_valid && !sf1_in_quiet(s, now) && s->digest_done; }

static int out(int *why, int w, int r)
{
    if (why) *why = w;
    return r;
}

int sf1_local(sf1_t *s, uint32_t id, int32_t old, int32_t value, uint32_t now, int *why)
{
    if (!s->epoch_valid) return out(why, SF1_W_NOEPOCH, SF1_SKIP);
    s->st.local_seen++;
    if (!sf1_crosses(s, id)) return s->st.skip_range++, out(why, SF1_W_RANGE, SF1_SKIP);
    if ((old != 0) == (value != 0) && old == value) return s->st.skip_same++, out(why, SF1_W_SAME, SF1_SKIP);
    int i = sf1_index(id);
    if (s->role == SF1_HOST) {
        bset(s->touched, i);
        s->st.local_sent++;
        return out(why, SF1_W_NONE, SF1_SEND);
    }
    if (s->role != SF1_GUEST) return out(why, SF1_W_ROLE, SF1_SKIP);
    if (value == 0) return s->st.skip_clear++, out(why, SF1_W_CLEAR, SF1_SKIP);
    bset(s->own, i);
    if (bit(s->touched, i)) return s->st.skip_touched++, out(why, SF1_W_TOUCHED, SF1_SKIP);
    if (!sf1_may_propose(s, now)) return s->st.prop_deferred++, out(why, SF1_W_QUIET, SF1_SKIP);   /* the digest (or its timeout) proposes it later if still set */
    if (bit(s->proposed, i)) return s->st.skip_same++, out(why, SF1_W_SAME, SF1_SKIP);
    bset(s->proposed, i);
    s->st.prop_sent++;
    return out(why, SF1_W_NONE, SF1_SEND);
}

static void hold_drop(sf1_t *s, uint32_t id)
{
    for (unsigned k = 0; k < s->nhold;)
        if (s->hold[k].id == id) s->hold[k] = s->hold[--s->nhold];
        else k++;
}

int sf1_remote_stream(sf1_t *s, uint32_t id, int32_t value, int32_t cur, uint32_t now, int *why)
{
    if (s->role != SF1_GUEST) return out(why, SF1_W_ROLE, SF1_SKIP);
    if (!s->epoch_valid) return out(why, SF1_W_NOEPOCH, SF1_SKIP);
    s->st.stream_rx++;
    int i = sf1_index(id);
    bset(s->touched, i);
    hold_drop(s, id);   /* a newer word of the host replaces a held one */
    if (value != 0) {
        if (i >= 0) s->rset_ms[i] = now | 1u;
        if (cur == value) return out(why, SF1_W_SAME, SF1_SKIP);
        s->st.stream_applied++;
        return out(why, SF1_W_NONE, SF1_APPLY);
    }
    if (cur == 0) return out(why, SF1_W_SAME, SF1_SKIP);
    if (i >= 0 && s->rset_ms[i] && (uint32_t)(now - s->rset_ms[i]) < SF1_HOLD_MS) {
        if (s->nhold >= SF1_MAXHOLD) {
            s->st.hold_full++, s->st.stream_applied++;
            return out(why, SF1_W_HOLDFULL, SF1_APPLY);
        }
        s->hold[s->nhold].id = id, s->hold[s->nhold].value = 0, s->hold[s->nhold].due = s->rset_ms[i] + SF1_HOLD_MS, s->nhold++;
        s->st.held++;
        return out(why, SF1_W_NONE, SF1_LATER);
    }
    s->st.stream_applied++;
    return out(why, SF1_W_NONE, SF1_APPLY);
}

int sf1_tick(sf1_t *s, uint32_t now, uint32_t *id, int32_t *value)
{
    for (unsigned k = 0; k < s->nhold; k++) {
        if ((int32_t)(now - s->hold[k].due) < 0) continue;
        *id = s->hold[k].id, *value = s->hold[k].value;
        s->hold[k] = s->hold[--s->nhold];
        s->st.held_applied++;
        return 1;
    }
    return 0;
}

int sf1_proposal(sf1_t *s, uint32_t id, int32_t value, int32_t cur, int *why)
{
    if (s->role != SF1_HOST) return out(why, SF1_W_ROLE, SF1_SKIP);
    if (!s->epoch_valid) return out(why, SF1_W_NOEPOCH, SF1_SKIP);
    s->st.prop_rx++;
    int i = sf1_index(id), w = SF1_W_NONE;
    if (!sf1_crosses(s, id)) w = SF1_W_RANGE;
    else if (value == 0) w = SF1_W_CLEAR;
    else if (bit(s->touched, i)) w = SF1_W_TOUCHED;
    else if (cur != 0) w = SF1_W_SAME, bset(s->touched, i);
    if (w != SF1_W_NONE) return s->st.prop_refused++, out(why, w, SF1_SKIP);
    bset(s->touched, i);
    s->st.prop_applied++;
    return out(why, SF1_W_NONE, SF1_APPLY);
}

int sf1_digest_due(sf1_t *s, uint32_t now)
{
    if (s->role != SF1_HOST || !s->epoch_valid || s->digest_done || sf1_in_quiet(s, now)) return 0;
    s->digest_done = 1;
    return 1;
}

unsigned sf1_digest_build(sf1_t *s, sf1_get_fn get, void *u, uint8_t *o, unsigned cap)
{
    if (cap < 4 + SF1_BYTES || !get) return 0;
    uint16_t lay = sf1_layout();
    o[0] = (uint8_t)lay, o[1] = (uint8_t)(lay >> 8), o[2] = (uint8_t)SF1_N, o[3] = (uint8_t)(SF1_N >> 8);
    memset(o + 4, 0, SF1_BYTES);
    for (unsigned i = 0; i < SF1_N; i++)
        if (get(u, sf1_id_at(i))) o[4 + (i >> 3)] |= (uint8_t)(1u << (i & 7));
    s->st.digest_tx++;
    return 4 + SF1_BYTES;
}

int sf1_digest_apply(sf1_t *s, const uint8_t *b, unsigned len, sf1_get_fn get, sf1_set_fn set, sf1_prop_fn prop, void *u, uint32_t now)
{
    if (s->role != SF1_GUEST || !s->epoch_valid || !get || len != 4 + SF1_BYTES || (unsigned)(b[0] | (b[1] << 8)) != sf1_layout() || (unsigned)(b[2] | (b[3] << 8)) != SF1_N) {
        s->st.digest_bad++;
        return 0;
    }
    s->st.digest_rx++;
    if (s->digest_done) return 1;   /* a second one of the same epoch (retransmitted after a lost ack): already applied */
    if (sf1_in_quiet(s, now)) {   /* this PC's own level start is still running its scripts: compare after them */
        memcpy(s->dig, b, len);
        s->dig_kept = 1;
        return 2;
    }
    s->dig_kept = 0;
    for (unsigned i = 0; i < SF1_N; i++) {
        uint32_t id = sf1_id_at(i);
        int h = (b[4 + (i >> 3)] >> (i & 7)) & 1, l = get(u, id) != 0;
        if (h) bset(s->touched, (int)i);
        if (h && !l) {
            if (set) set(u, id, 1);
            s->st.digest_set++;
        } else if (!h && l) {
            if (bit(s->own, (int)i) && !bit(s->touched, (int)i)) {   /* set by the guest's own engine in this epoch: the host decides on it as a proposal */
                if (!bit(s->proposed, (int)i)) {
                    bset(s->proposed, (int)i);
                    if (prop) prop(u, id);
                    s->st.digest_prop++, s->st.prop_sent++;
                }
            } else {   /* stale here (an older level instance, the guest's own save): the host's world wins */
                bset(s->touched, (int)i);
                if (set) set(u, id, 0);
                s->st.digest_clear++;
            }
        }
    }
    s->digest_done = 1;
    return 1;
}

int sf1_digest_kept(sf1_t *s, uint32_t now, sf1_get_fn get, sf1_set_fn set, sf1_prop_fn prop, void *u)
{
    if (!s->dig_kept || s->digest_done || sf1_in_quiet(s, now)) return 0;
    uint8_t b[4 + SF1_BYTES];
    memcpy(b, s->dig, sizeof b);
    s->st.digest_rx--;   /* counted once, on arrival */
    return sf1_digest_apply(s, b, sizeof b, get, set, prop, u, now) == 1;
}

int sf1_digest_timeout(sf1_t *s, uint32_t now, sf1_get_fn get, sf1_prop_fn prop, void *u)
{
    if (s->role != SF1_GUEST || !s->epoch_valid || s->digest_done || s->dig_kept || (int32_t)(now - qstart(s)) < (int32_t)(SF1_QUIET_MS + SF1_DIGEST_WAIT_MS)) return 0;
    s->digest_done = 1;
    s->st.digest_timeout++;
    for (unsigned i = 0; i < SF1_N; i++) {
        if (!bit(s->own, (int)i) || bit(s->touched, (int)i) || bit(s->proposed, (int)i)) continue;
        uint32_t id = sf1_id_at(i);
        if (get && !get(u, id)) continue;
        bset(s->proposed, (int)i);
        if (prop) prop(u, id);
        s->st.prop_sent++;
    }
    return 1;
}

/* ---------------------------------------------------------------- rate */
int sf1_rate_poll(sf1_t *s, int cur, int *kind, int *value)
{
    if (cur < 0 || cur > SF1_RATE_MAX) return 0;
    if (s->rate_last < 0) {
        s->rate_last = cur;   /* the baseline of this epoch: the host sends it with the digest */
        return 0;
    }
    if (cur == s->rate_last) return 0;
    int d = cur - s->rate_last;
    s->rate_last = cur;
    if (s->role == SF1_HOST) {
        *kind = SF1_RATE_ABS, *value = cur;
        s->st.rate_tx++;
        return 1;
    }
    if (s->role == SF1_GUEST && d > 0) {   /* a scan here: the host adds it */
        *kind = SF1_RATE_DELTA, *value = d;
        s->st.rate_tx++;
        return 1;
    }
    return 0;   /* the game lowered it itself (a new game): nothing to tell */
}

int sf1_rate_remote(sf1_t *s, int kind, int value, int cur, int *outv, int *send)
{
    *send = 0;
    s->st.rate_rx++;
    if (cur < 0 || cur > SF1_RATE_MAX) return 0;
    if (s->role == SF1_HOST && kind == SF1_RATE_DELTA && value > 0 && value <= SF1_RATE_MAX) {
        int n = cur + value;
        if (n > SF1_RATE_MAX) n = SF1_RATE_MAX;
        s->rate_last = n;
        if (n == cur) return 0;
        *outv = n, *send = 1;
        s->st.rate_applied++, s->st.rate_tx++;
        return 1;
    }
    if (s->role == SF1_GUEST && kind == SF1_RATE_ABS && value >= 0 && value <= SF1_RATE_MAX) {
        s->rate_last = value;
        if (value == cur) return 0;
        *outv = value;
        s->st.rate_applied++;
        return 1;
    }
    return 0;
}

int sf1_rate_value(const sf1_t *s) { return s->rate_last; }

/* ---------------------------------------------------------------- pieces */
int sf1_piece_class(uint32_t cls)
{
    for (unsigned i = 0; i < sizeof PIECES / sizeof PIECES[0]; i++)
        if (PIECES[i] == cls) return 1;
    return 0;
}

static sf1_piece *p_find(sf1_t *s, const uint32_t k[4])
{
    for (unsigned i = 0; i < s->npc; i++)
        if (!memcmp(s->pc[i].k, k, sizeof s->pc[i].k)) return &s->pc[i];
    return NULL;
}

static sf1_piece *p_add(sf1_t *s, const uint32_t k[4], uint32_t cls, uint8_t state)
{
    if (s->npc >= SF1_MAXPIECES) return NULL;
    sf1_piece *p = &s->pc[s->npc++];
    memcpy(p->k, k, sizeof p->k), p->cls = cls, p->state = state;
    return p;
}

int sf1_piece_local(sf1_t *s, const uint32_t k[4], uint32_t cls)
{
    if (!s->epoch_valid || !sf1_piece_class(cls)) return 0;
    sf1_piece *p = p_find(s, k);
    if (p) {
        int was = p->state;
        p->state |= 1;
        if (was) s->st.piece_dup++;
        return 0;   /* applied from the other PC (our own write broke it) or already announced */
    }
    p_add(s, k, cls, 1);
    s->st.piece_tx++;
    return 1;
}

int sf1_piece_remote(sf1_t *s, const uint32_t k[4], uint32_t cls)
{
    s->st.piece_rx++;
    if (!s->epoch_valid || !sf1_piece_class(cls)) return 0;
    sf1_piece *p = p_find(s, k);
    if (p) {
        p->state |= 2;
        s->st.piece_dup++;
        return 0;   /* broken here already (own blow) or applied before */
    }
    p_add(s, k, cls, 2);
    return 1;
}

int sf1_piece_known(const sf1_t *s, const uint32_t k[4])
{
    for (unsigned i = 0; i < s->npc; i++)
        if (!memcmp(s->pc[i].k, k, sizeof s->pc[i].k)) return s->pc[i].state;
    return 0;
}

/* ---------------------------------------------------------------- wire */
static void put32(uint8_t *o, uint32_t v) { o[0] = (uint8_t)v, o[1] = (uint8_t)(v >> 8), o[2] = (uint8_t)(v >> 16), o[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }

unsigned sf1_enc_prop(uint8_t *o, unsigned cap, uint32_t id, int32_t value)
{
    if (cap < 8) return 0;
    put32(o, id), put32(o + 4, (uint32_t)value);
    return 8;
}
int sf1_dec_prop(const uint8_t *b, unsigned n, uint32_t *id, int32_t *value)
{
    if (n != 8) return 0;
    *id = get32(b), *value = (int32_t)get32(b + 4);
    return 1;
}
unsigned sf1_enc_rate(uint8_t *o, unsigned cap, int kind, int value)
{
    if (cap < 3) return 0;
    o[0] = (uint8_t)kind, o[1] = (uint8_t)value, o[2] = (uint8_t)((unsigned)value >> 8);
    return 3;
}
int sf1_dec_rate(const uint8_t *b, unsigned n, int *kind, int *value)
{
    if (n != 3 || b[0] > SF1_RATE_DELTA) return 0;
    *kind = b[0], *value = (int16_t)(b[1] | (b[2] << 8));
    return 1;
}
unsigned sf1_enc_piece(uint8_t *o, unsigned cap, const uint32_t k[4], uint32_t cls)
{
    if (cap < 20) return 0;
    for (int i = 0; i < 4; i++) put32(o + 4 * i, k[i]);
    put32(o + 16, cls);
    return 20;
}
int sf1_dec_piece(const uint8_t *b, unsigned n, uint32_t k[4], uint32_t *cls)
{
    if (n != 20) return 0;
    for (int i = 0; i < 4; i++) k[i] = get32(b + 4 * i);
    *cls = get32(b + 16);
    return 1;
}

void sf1_summary(const sf1_t *s, char *o, size_t size)
{
    const sf1_stats *t = &s->st;
    snprintf(o, size,
             "story mirror %s epoch %d%s%s: local %u (sent %u, proposals %u, deferred %u; not mirrored %u, unchanged %u, clears kept %u, host decided %u); stream rx %u applied %u held %u (due %u, full %u); "
             "proposals rx %u applied %u refused %u; digest tx %u rx %u (set %u clear %u proposed %u bad %u timeout %u, done %d); rate tx %u rx %u applied %u (last %d); pieces tx %u rx %u dup %u",
             s->role == SF1_HOST ? "HOST" : s->role == SF1_GUEST ? "GUEST" : "-", s->epoch_valid ? (int)s->epoch : -1, s->wide ? " (wide)" : "", s->unplaced ? " (own human not placed yet)" : "", t->local_seen, t->local_sent, t->prop_sent, t->prop_deferred, t->skip_range,
             t->skip_same, t->skip_clear, t->skip_touched, t->stream_rx, t->stream_applied, t->held, t->held_applied, t->hold_full, t->prop_rx, t->prop_applied, t->prop_refused, t->digest_tx, t->digest_rx, t->digest_set,
             t->digest_clear, t->digest_prop, t->digest_bad, t->digest_timeout, s->digest_done, t->rate_tx, t->rate_rx, t->rate_applied, s->rate_last, t->piece_tx, t->piece_rx, t->piece_dup);
}

/* ep1-probe (the save-flag probe, notes\ep1-probe.md): the mirrored ids set on this PC as one line. FNV-1a 32 over the 48-byte bitmap (bit i = sf1_id_at(i)), so two
 * PCs' lines compare by the hash; the ids as many as fit ("..." when cut). Returns the count; -1 when get answered < 0 (the getter is not usable). */
int sf1_set_summary(sf1_get_fn get, void *u, uint32_t *hash, char *list, unsigned cap)
{
    uint8_t bm[SF1_BYTES];
    unsigned o = 0;
    int n = 0;
    memset(bm, 0, sizeof bm);
    if (list && cap) list[0] = 0;
    for (unsigned i = 0; i < SF1_N; i++) {
        int v = get(u, sf1_id_at(i));
        if (v < 0) {
            if (hash) *hash = 0;
            return -1;
        }
        if (!v) continue;
        bm[i >> 3] |= (uint8_t)(1u << (i & 7));
        n++;
        if (list && cap) {
            char w[12];
            int k = snprintf(w, sizeof w, "%s%u", o ? " " : "", (unsigned)sf1_id_at(i));
            if (k > 0 && o + (unsigned)k + 4 < cap) memcpy(list + o, w, (size_t)k + 1), o += (unsigned)k;
            else if (o + 4 <= cap && (o < 3 || strcmp(list + o - 3, "..."))) memcpy(list + o, "...", 4), o += 3;
        }
    }
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < SF1_BYTES; i++) h = (h ^ bm[i]) * 16777619u;
    if (hash) *hash = h;
    return n;
}

unsigned sf1_pre_build(sf1_get_fn get, void *u, uint32_t seq, uint8_t *o, unsigned cap)
{
    if (cap < SF1_PRE_LEN || !get) return 0;
    uint16_t lay = sf1_layout();
    o[0] = (uint8_t)lay, o[1] = (uint8_t)(lay >> 8);
    put32(o + 2, seq);
    memset(o + 6, 0, SF1_BYTES);
    for (unsigned i = 0; i < SF1_N; i++)
        if (get(u, sf1_id_at(i)) > 0) o[6 + (i >> 3)] |= (uint8_t)(1u << (i & 7));
    return SF1_PRE_LEN;
}

int sf1_pre_take(sf1_pre_t *p, const uint8_t *b, unsigned n, uint32_t now)
{
    if (n != SF1_PRE_LEN || (unsigned)(b[0] | (b[1] << 8)) != sf1_layout()) {
        p->bad++;
        return 0;
    }
    uint32_t seq = get32(b + 2);
    if (p->have && (int32_t)(seq - p->seq) <= 0) return 0;   /* an older (or the same) restart */
    p->have = 1, p->seq = seq, p->at = now;
    memcpy(p->bm, b + 6, SF1_BYTES);
    p->rx++;
    return 1;
}

int sf1_pre_apply(sf1_pre_t *p, uint32_t now, sf1_get_fn get, sf1_set_fn set, void *u, unsigned *set_n, unsigned *clr_n)
{
    unsigned s = 0, c = 0;
    if (set_n) *set_n = 0;
    if (clr_n) *clr_n = 0;
    if (!p->have || !get || !set) return 0;
    if ((uint32_t)(now - p->at) > SF1_PRE_MAX_MS) {
        p->have = 0, p->stale++;
        return 0;
    }
    for (unsigned i = 0; i < SF1_N; i++) {
        int want = (p->bm[i >> 3] >> (i & 7)) & 1, cur = get(u, sf1_id_at(i));
        if (cur < 0) continue;
        if ((cur > 0) != want) {
            set(u, sf1_id_at(i), want);
            if (want) s++;
            else c++;
        }
    }
    p->have = 0, p->applied++, p->set_n += s, p->clr_n += c;
    if (set_n) *set_n = s;
    if (clr_n) *clr_n = c;
    return 1;
}
